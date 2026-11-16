#include "etw.h"
#include "requests.h"
#include "diagnostic_decode.h"
#include "etw_decode.h"
#include "diagnostic_report.h"
#include "etl_reader.h"
#include "output.h"
#include "json_writer.h"
#include "pool_tags.h"
#include "heap_metadata.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

namespace {
int failures = 0;
void Check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
}
void TestDecoders();
void TestAnalysis();
void Contracts(const std::filesystem::path& path);
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--compare-driver") {
        wperf::TraceData trace; std::wstring error;
        if (!wperf::ReadEtwFile(argv[2], trace, &error, false, L"driver")) { std::wcerr << error; return 1; }
        const auto analysis = wperf::AnalyzeDiagnostics(trace, L"driver", {});
        uint64_t count = 0, zero = 0;
        for (const auto& row : analysis.execution) if (row.kind == L"driver-dispatch") {
            count += row.durations.size();
            zero += static_cast<uint64_t>(std::count(row.durations.begin(), row.durations.end(), uint64_t{0}));
        }
        std::cout << "dispatch=" << count << " zero_ns=" << zero << '\n';
        return 0;
    }
    TestDecoders();
    TestAnalysis();
    if (argc == 3 && std::string(argv[1]) == "--contracts") Contracts(argv[2]);
    for (const char* profile : {"dpc", "isr", "heap", "pool", "driver"}) {
        wperf::CommandRequest request;
        std::wstring error;
        Check(wperf::BuildCommandRequest("record", {{"--profile", profile}, {"--pid", "42"}}, {}, {}, request, &error),
              "diagnostic capture profile is accepted");
    }
    Check((wperf::TraceFlagsForProfile(L"dpc") & EVENT_TRACE_FLAG_DPC) != 0, "DPC capture enabled");
    Check((wperf::TraceFlagsForProfile(L"isr") & EVENT_TRACE_FLAG_INTERRUPT) != 0, "ISR capture enabled");
    Check((wperf::TraceFlagsForProfile(L"driver") & EVENT_TRACE_FLAG_DRIVER) != 0, "driver capture enabled");
    Check((wperf::TraceFlagsForProfile(L"pool") & EVENT_TRACE_FLAG_PROFILE) == 0, "pool capture avoids CPU sampling");
    Check((wperf::TraceFlagsForProfile(L"driver") & EVENT_TRACE_FLAG_CSWITCH) != 0, "driver capture includes scheduler context");
    wperf::CommandRequest pool_request;
    Check(wperf::BuildCommandRequest("record", {{"--profile", "pool"}, {"--pool-tag", "Irp "}}, {}, {}, pool_request), "pool tag preserves trailing spaces");
    Check(std::get<wperf::RecordRequest>(pool_request).pool_tag == 0x20707249u, "pool tag packs native little-endian bytes");
    Check(!wperf::BuildCommandRequest("record", {{"--pool-tag", "short"}}, {}, {}, pool_request), "malformed pool tag is rejected");
    Check(wperf::BuildCommandRequest("list", {}, {{"--pool-tags", true}}, {}, pool_request) && std::get<wperf::ListRequest>(pool_request).pool_tags,
          "pool tag listing reaches the typed request");
    Check(wperf::PoolTagText(0x20707249u) == L"Irp ", "pool tags retain four character display");
    uint32_t timestamp = 0, checksum = 0;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    Check(wperf::ReadLoadedPeIdentity(GetCurrentProcess(), base, nt->OptionalHeader.SizeOfImage, timestamp, checksum) &&
          timestamp == nt->FileHeader.TimeDateStamp && checksum == nt->OptionalHeader.CheckSum,
          "heap snapshot identity comes from loaded headers, not a possibly replaced local file");
    Check(!wperf::ReadLoadedPeIdentity(GetCurrentProcess(), base, 16, timestamp, checksum), "truncated loaded PE mapping rejected");
    Check(!wperf::ReadLoadedPeIdentity(GetCurrentProcess(), UINT64_MAX - 10, 100, timestamp, checksum), "loaded PE address overflow rejected");
    return failures == 0 ? 0 : 1;
}

namespace {
struct Raw {
    EVENT_RECORD event{};
    std::vector<BYTE> bytes;
    void Bind() { event.UserData = bytes.data(); event.UserDataLength = static_cast<USHORT>(bytes.size()); }
};
template<typename T> void Put(Raw& raw, size_t offset, T value) { std::memcpy(raw.bytes.data() + offset, &value, sizeof(value)); }
void Pointer(Raw& raw, size_t offset, uint32_t width, uint64_t value) {
    if (width == 8) Put(raw, offset, value); else Put(raw, offset, static_cast<uint32_t>(value));
}
Raw Event(const GUID& guid, UCHAR opcode, UCHAR version, uint32_t width, size_t size, uint64_t time = 200) {
    Raw raw; raw.bytes.resize(size);
    raw.event.EventHeader.ProviderId = guid;
    raw.event.EventHeader.EventDescriptor.Opcode = opcode;
    raw.event.EventHeader.EventDescriptor.Version = version;
    raw.event.EventHeader.TimeStamp.QuadPart = time;
    raw.event.EventHeader.ProcessId = 42; raw.event.EventHeader.ThreadId = 43;
    raw.event.EventHeader.Flags = width == 8 ? EVENT_HEADER_FLAG_64_BIT_HEADER : EVENT_HEADER_FLAG_32_BIT_HEADER;
    raw.Bind(); return raw;
}
void RejectTruncations(Raw& raw) {
    for (size_t size = 0; size < raw.bytes.size(); ++size) {
        raw.Bind(); raw.event.UserDataLength = static_cast<USHORT>(size);
        wperf::TraceData trace;
        Check(wperf::DecodeDiagnosticRecord(raw.event, raw.event.EventHeader.ProviderId, trace), "truncated diagnostic is recognized");
        Check(trace.quality.decode_errors == 1 && trace.allocations.empty() && trace.interrupts.empty() && trace.driver_events.empty(), "truncated diagnostic cannot create a partial record");
    }
    raw.Bind();
}
wperf::AllocationRecord Allocation(uint64_t time, uint64_t address, uint64_t size,
                                    wperf::MemoryOperation operation = wperf::MemoryOperation::Allocate) {
    wperf::AllocationRecord event; event.timestamp_ns = time; event.address = address; event.size = size;
    event.pid = 42; event.tid = 43; event.heap = 100; event.operation = operation;
    event.stack = {0x10010}; event.stack_quality = wperf::StackQuality::Exact;
    return event;
}
wperf::TraceData Fixture() {
    wperf::TraceData trace; trace.start_ns = 0; trace.end_ns = 10000000;
    trace.processes.push_back({42, 0, L"sample.exe", L"", 1, 0});
    wperf::ImageRecord image; image.pid = 0; image.base = 0x10000; image.size = 0x1000; image.path = L"sample.sys";
    trace.images.push_back(image);
    trace.interrupts.push_back({10, 100, 0x10010, 0, wperf::InterruptKind::Dpc});
    trace.interrupts.push_back({20, 30, 0x10010, 0, wperf::InterruptKind::Isr});
    trace.allocations = {Allocation(10, 1000, 100), Allocation(20, 1001, 50), Allocation(30, 1000, 0, wperf::MemoryOperation::Free)};
    auto pool = Allocation(15, 2000, 200); pool.pid = UINT32_MAX; pool.domain = wperf::MemoryDomain::Pool;
    pool.heap = 0; pool.tag = 0x74736554; pool.pool_type = 1; trace.allocations.push_back(pool);
    wperf::DriverRecord enter; enter.timestamp_ns = 40; enter.pid = 42; enter.tid = 43;
    enter.irp = 3000; enter.routine = 0x10010; enter.match_id = 9; enter.major = 3;
    auto exit = enter; exit.timestamp_ns = 70; exit.operation = wperf::DriverOperation::DispatchReturn; exit.pid = 4; exit.tid = 5;
    trace.driver_events = {enter, exit};
    return trace;
}
}

void TestDecoders() {
    for (const uint32_t width : {4u, 8u}) {
        for (const UCHAR opcode : {UCHAR{50}, UCHAR{66}, UCHAR{67}, UCHAR{68}, UCHAR{69}}) {
            const bool isr = opcode == 50 || opcode == 67;
            auto raw = Event(wperf::kDiagnosticPerfGuid, opcode, 2, width, 8 + width + (isr ? 4 : 0));
            Put(raw, 0, uint64_t{100}); Pointer(raw, 8, width, 0x10010);
            raw.Bind(); wperf::TraceData trace;
            Check(wperf::DecodeDiagnosticRecord(raw.event, raw.event.EventHeader.ProviderId, trace), "interrupt decoded");
            Check(trace.interrupts.size() == 1 && trace.interrupts[0].start_ns == 10000 && trace.interrupts[0].end_ns == 20000, "interrupt QPC boundaries preserved");
            Check(trace.interrupts[0].kind == (isr ? wperf::InterruptKind::Isr : opcode == 66 ? wperf::InterruptKind::ThreadedDpc : opcode == 69 ? wperf::InterruptKind::TimerDpc : wperf::InterruptKind::Dpc), "interrupt subtype preserved");
            RejectTruncations(raw);
            Put(raw, 0, uint64_t{201}); raw.Bind(); wperf::TraceData bad;
            wperf::DecodeDiagnosticRecord(raw.event, raw.event.EventHeader.ProviderId, bad);
            Check(bad.quality.decode_errors == 1, "negative interrupt duration is rejected");
        }
        for (const UCHAR opcode : {UCHAR{33}, UCHAR{34}, UCHAR{35}, UCHAR{36}}) {
            const size_t size = opcode == 33 ? 3 * width + 4 : opcode == 34 ? 5 * width + 4 : opcode == 36 ? 2 * width + 4 : width;
            auto raw = Event(wperf::kHeapGuid, opcode, 0, width, size);
            Pointer(raw, 0, width, 77);
            if (opcode == 33) { Pointer(raw, width, width, 99); Pointer(raw, 2 * width, width, 1000); }
            else if (opcode == 34) { Pointer(raw, width, width, 1001); Pointer(raw, 2 * width, width, 1000); Pointer(raw, 3 * width, width, 199); Pointer(raw, 4 * width, width, 99); }
            else if (opcode == 36) Pointer(raw, width, width, 1000);
            raw.Bind(); wperf::TraceData trace;
            wperf::DecodeDiagnosticRecord(raw.event, raw.event.EventHeader.ProviderId, trace);
            Check(trace.allocations.size() == 1 && trace.allocations[0].heap == 77 && trace.allocations[0].pid == 42, "heap pointer width and ownership decoded");
            if (opcode == 34) Check(trace.allocations[0].old_address == 1000 && trace.allocations[0].size == 199, "realloc identities/sizes decoded");
            RejectTruncations(raw);
        }
        for (UCHAR opcode = 32; opcode <= 35; ++opcode) {
            const bool session = opcode == 33 || opcode == 35;
            auto raw = Event(wperf::kPoolGuid, opcode, 2, width, 8 + 2 * width + (session ? 4 : 0));
            Put(raw, 0, uint32_t{1}); Put(raw, 4, uint32_t{0x74736554});
            Pointer(raw, 8, width, 321); Pointer(raw, 8 + width, width, 2000);
            if (session) Put(raw, 8 + 2 * width, uint32_t{7});
            raw.Bind(); wperf::TraceData trace;
            wperf::DecodeDiagnosticRecord(raw.event, raw.event.EventHeader.ProviderId, trace);
            Check(trace.allocations.size() == 1 && trace.allocations[0].pid == UINT32_MAX && trace.allocations[0].size == 321 && trace.allocations[0].tag == 0x74736554, "pool fields decoded without inventing process ownership");
            Check(trace.allocations[0].session_id == (session ? 7u : UINT32_MAX), "pool session domain preserved");
            RejectTruncations(raw);
        }
        for (const UCHAR opcode : {UCHAR{34}, UCHAR{35}, UCHAR{37}, UCHAR{52}, UCHAR{53}}) {
            const size_t size = opcode == 34 ? 12 + 3 * width : opcode == 37 || opcode == 52 ? 4 + 2 * width : 4 + width;
            auto raw = Event(wperf::kDiagnosticDiskGuid, opcode, 0, width, size);
            if (opcode == 34) { Put(raw, 0, uint32_t{3}); Pointer(raw, 8, width, 0x10010); Pointer(raw, 8 + 2 * width, width, 3000); Put(raw, 8 + 3 * width, uint32_t{9}); }
            else if (opcode == 37 || opcode == 52) { Pointer(raw, 0, width, 0x10010); Pointer(raw, width, width, 3000); Put(raw, 2 * width, uint32_t{9}); }
            else { Pointer(raw, 0, width, 3000); Put(raw, width, uint32_t{9}); }
            raw.Bind(); wperf::TraceData trace;
            wperf::DecodeDiagnosticRecord(raw.event, raw.event.EventHeader.ProviderId, trace);
            Check(trace.driver_events.size() == 1 && trace.driver_events[0].irp == 3000 && trace.driver_events[0].match_id == 9, "driver IRP and unique match ID decoded");
            RejectTruncations(raw);
        }
    }
    auto raw = Event(wperf::kDiagnosticPerfGuid, 68, 2, 8, 16);
    Put(raw, 0, uint64_t{100}); Pointer(raw, 8, 8, 0x10010); raw.Bind();
    wperf::TraceData replay;
    Check(wperf::ReadEtwRecords(std::span<const EVENT_RECORD>(&raw.event, 1), replay), "diagnostic replay accepts a raw record");
    Check(replay.interrupts.size() == 1 && replay.quality.unknown_events == 0, "diagnostics reach the shared replay model");
    raw.event.EventHeader.EventDescriptor.Version = 255;
    wperf::TraceData unknown;
    wperf::ReadEtwRecords(std::span<const EVENT_RECORD>(&raw.event, 1), unknown);
    Check(unknown.quality.diagnostic_unsupported == 1 && unknown.quality.incomplete && unknown.interrupts.empty(), "unknown versions affect quality and never guess a layout");
    wperf::TraceData omitted;
    wperf::ReadEtwRecords(std::span<const EVENT_RECORD>(&raw.event, 1), omitted, 10000000, L"heap");
    Check(omitted.interrupts.empty() && omitted.quality.diagnostic_unsupported == 0 && omitted.quality.budget_estimated_bytes == 0,
          "unrelated diagnostic events do not consume the selected domain's replay budget or decode quality");
    std::string metadata = "wperf-meta-v1\nheap-target 42\nsnapshot-process 42 " + wperf::HexText(L"sample.exe") +
        "\nsnapshot-image 42 65536 4096 123 456 " + wperf::HexText(L"sample.exe") + "\n";
    EVENT_RECORD snapshot{}; snapshot.UserData = metadata.data(); snapshot.UserDataLength = static_cast<USHORT>(metadata.size());
    snapshot.EventHeader.TimeStamp.QuadPart = 100;
    wperf::TraceData attached;
    Check(wperf::DecodeWperfMetadata(snapshot, attached) && attached.heap_target_pid == 42 && attached.heap_snapshot_metadata &&
          attached.images.size() == 1 && attached.images[0].snapshot_only && attached.images[0].load_ns == 10000,
          "heap attach metadata preserves its observed timestamp and snapshot provenance");
    Check(attached.processes.size() == 1 && attached.processes[0].image_name == L"sample.exe", "attach metadata supplies process name without guessing creation time");
    metadata = "wperf-meta-v1\nheap-target 42\nsnapshot-image 43 65536 4096 123 456 " + wperf::HexText(L"other.exe") + "\n";
    snapshot.UserData = metadata.data(); snapshot.UserDataLength = static_cast<USHORT>(metadata.size());
    wperf::TraceData foreign;
    Check(!wperf::DecodeWperfMetadata(snapshot, foreign) && foreign.images.empty(), "heap snapshot cannot relabel another process's module");
    const GUID image_guid = {0x2cb15d1d,0x5fc1,0x11d2,{0xab,0xe1,0x00,0xa0,0xc9,0x11,0xf5,0x18}};
    const GUID thread_guid = {0x3d6fa8d1,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}};
    auto interrupt = Event(wperf::kDiagnosticPerfGuid, 68, 2, 8, 16, 200);
    Put(interrupt, 0, uint64_t{100}); Pointer(interrupt, 8, 8, 0x10010); interrupt.Bind();
    const std::wstring path = L"sample.sys";
    auto image = Event(image_guid, 3, 3, 8, 56 + (path.size() + 1) * 2, 300);
    Pointer(image, 0, 8, 0x10000); Pointer(image, 8, 8, 4096); Put(image, 16, uint32_t{0});
    std::memcpy(image.bytes.data() + 56, path.c_str(), (path.size() + 1) * 2); image.Bind();
    auto thread = Event(thread_guid, 3, 3, 8, 8, 300); Put(thread, 0, uint32_t{42}); Put(thread, 4, uint32_t{43}); thread.Bind();
    const EVENT_RECORD records[] = {interrupt.event, image.event, thread.event};
    wperf::TraceData rundown;
    wperf::ReadEtwRecords(records, rundown);
    Check(rundown.images[0].load_ns == 0 && wperf::FindImage(rundown, 0, 0x10010, 10000) != nullptr,
          "start rundown describes an image already present before its delayed metadata event");
    Check(wperf::FindThread(rundown, 43, 10000) != nullptr,
          "start rundown supplies the initial thread baseline rather than a fictitious late creation");
    image.event.EventHeader.EventDescriptor.Opcode = 4;
    image.event.EventHeader.TimeStamp.QuadPart = 400;
    thread.event.EventHeader.EventDescriptor.Opcode = 4;
    thread.event.EventHeader.TimeStamp.QuadPart = 400;
    const EVENT_RECORD complete[] = {records[0], records[1], records[2], image.event, thread.event};
    wperf::ReadEtwRecords(complete, rundown);
    Check(wperf::FindImage(rundown, 0, 0x10010, 50000) != nullptr && wperf::FindThread(rundown, 43, 50000) != nullptr,
          "end rundown cannot masquerade as image unload or thread exit");
}

void TestAnalysis() {
    auto trace = Fixture(); wperf::CpuReportOptions options;
    auto analysis = wperf::AnalyzeDiagnostics(trace, L"interrupt", options);
    Check(analysis.execution.size() == 2 && analysis.execution[0].pid == UINT32_MAX, "interrupt rows are system-owned");
    trace.interrupts.push_back({50, 150, 0x10010, 0, wperf::InterruptKind::Dpc});
    analysis = wperf::AnalyzeDiagnostics(trace, L"dpc", options);
    Check(analysis.execution.size() == 1 && analysis.execution[0].inclusive_ns == 190 && analysis.execution[0].busy_ns == 140, "overlap union differs from inclusive sum");
    Check(analysis.execution[0].max_ns == 100 && analysis.execution[0].p95_ns == 100, "interrupt quantiles computed");
    options.cpu = 1; Check(wperf::AnalyzeDiagnostics(trace, L"dpc", options).execution.empty(), "interrupt CPU selector"); options.cpu.reset();
    options.start_offset_ns = 60; options.end_offset_ns = 80;
    analysis = wperf::AnalyzeDiagnostics(trace, L"dpc", options);
    Check(analysis.execution[0].inclusive_ns == 40 && analysis.execution[0].busy_ns == 20, "interrupt intervals clip at time boundaries");
    options = {};
    analysis = wperf::AnalyzeDiagnostics(trace, L"heap", options);
    Check(analysis.memory.size() == 1 && analysis.memory[0].allocated_bytes == 150 && analysis.memory[0].freed_bytes == 100 && analysis.memory[0].live_bytes == 50 && analysis.memory[0].peak_live_bytes == 150, "heap lifecycle totals and peak");
    trace.allocations[2].tid = 99; options.tid = 43;
    Check(wperf::AnalyzeDiagnostics(trace, L"heap", options).memory[0].live_bytes == 50, "free on another thread survives allocation-side selection");
    options = {}; options.end_offset_ns = 25;
    Check(wperf::AnalyzeDiagnostics(trace, L"heap", options).memory[0].live_bytes == 150, "heap snapshot ends before later free");
    options.start_offset_ns = 15;
    Check(wperf::AnalyzeDiagnostics(trace, L"heap", options).memory[0].allocated_bytes == 50, "time range selects allocation cohort");
    options = {};
    auto reallocation = Allocation(40, 1002, 80, wperf::MemoryOperation::Reallocate); reallocation.old_address = 1001;
    trace.allocations.push_back(reallocation);
    analysis = wperf::AnalyzeDiagnostics(trace, L"heap", options);
    Check(analysis.memory[0].live_bytes == 80 && analysis.memory[0].freed_bytes == 150, "reallocation frees old identity before adding new identity");
    auto native = Fixture();
    native.allocations.push_back(Allocation(35, 1002, 80));
    native.allocations.push_back(Allocation(36, 1001, 0, wperf::MemoryOperation::Free));
    native.allocations.push_back(reallocation);
    auto native_result = wperf::AnalyzeDiagnostics(native, L"heap", options);
    Check(native_result.address_reuse == 0 && native_result.unmatched_frees == 0 && native_result.realloc_notifications == 1 &&
          native_result.memory[0].allocated_bytes == 230 && native_result.memory[0].live_bytes == 80,
          "native alloc/free plus realloc notification is counted once");
    auto noop = Allocation(45, 1002, 80, wperf::MemoryOperation::Reallocate); noop.old_address = 1002; noop.old_size = 80;
    native.allocations.push_back(noop);
    Check(wperf::AnalyzeDiagnostics(native, L"heap", options).memory[0].allocated_bytes == 230, "no-op realloc does not invent allocation churn");
    auto destroy = Allocation(50, 0, 0, wperf::MemoryOperation::DestroyHeap); trace.allocations.push_back(destroy);
    analysis = wperf::AnalyzeDiagnostics(trace, L"heap", options);
    Check(analysis.memory[0].live_bytes == 0 && analysis.memory[0].lifecycle_released_bytes == 80, "heap destroy closes live allocations");
    trace = Fixture(); trace.processes[0].end_ns = 40;
    Check(wperf::AnalyzeDiagnostics(trace, L"heap", options).memory[0].live_bytes == 0, "process exit closes heap lifetime");
    trace = Fixture(); trace.allocations.push_back(Allocation(40, 9999, 0, wperf::MemoryOperation::Free));
    analysis = wperf::AnalyzeDiagnostics(trace, L"heap", options);
    Check(analysis.unmatched_frees == 1 && !analysis.incomplete, "unobserved baseline free is counted without inventing allocations");
    trace.allocations.push_back(Allocation(50, 1001, 75));
    analysis = wperf::AnalyzeDiagnostics(trace, L"heap", options);
    Check(analysis.address_reuse == 1 && analysis.incomplete && analysis.memory[0].live_bytes == 75, "lost free/address reuse never doubles live bytes");
    trace = Fixture(); analysis = wperf::AnalyzeDiagnostics(trace, L"driver", options);
    Check(analysis.execution.size() == 1 && analysis.execution[0].inclusive_ns == 30 && analysis.execution[0].pid == 42, "driver return on another context pairs by IRP and unique ID");
    options.pid = 42; Check(wperf::AnalyzeDiagnostics(trace, L"driver", options).execution.size() == 1, "driver filtering uses entry context"); options.pid = 4;
    Check(wperf::AnalyzeDiagnostics(trace, L"driver", options).execution.empty(), "driver filtering never uses completion header PID"); options = {};
    trace.driver_events[1].match_id = 10; analysis = wperf::AnalyzeDiagnostics(trace, L"driver", options);
    Check(analysis.execution.empty() && analysis.unmatched_entries == 1 && analysis.unmatched_returns == 1 && analysis.incomplete, "driver mismatched unique ID cannot synthesize latency");
    trace = Fixture(); trace.driver_events.insert(trace.driver_events.begin() + 1, trace.driver_events[0]);
    analysis = wperf::AnalyzeDiagnostics(trace, L"driver", options);
    Check(analysis.execution.empty() && analysis.duplicate_entries == 1 && analysis.incomplete, "ambiguous duplicate driver entries rejected");
    trace = Fixture(); trace.processes[0].end_ns = 50;
    Check(wperf::AnalyzeDiagnostics(trace, L"driver", options).execution.empty(), "driver pairing cannot cross the entry process lifetime");
    trace = Fixture(); trace.driver_events[0].pid = UINT32_MAX; trace.driver_events[0].tid = UINT32_MAX;
    trace.threads.push_back({42, 43, 1, 0});
    wperf::TraceEvent switched; switched.timestamp_ns = 35; switched.kind = wperf::TraceEventKind::ContextSwitch; switched.value1 = 43;
    trace.events.push_back(switched);
    analysis = wperf::AnalyzeDiagnostics(trace, L"driver", options);
    Check(analysis.execution.size() == 1 && analysis.execution[0].pid == 42 && analysis.execution[0].tid == 43 &&
          analysis.execution[0].context_basis == L"scheduler", "driver context is explicitly sourced from same-CPU scheduling");
    trace = Fixture(); analysis = wperf::AnalyzeDiagnostics(trace, L"pool", options);
    Check(analysis.memory.size() == 1 && analysis.memory[0].pid == UINT32_MAX && analysis.memory[0].tag == 0x74736554, "pool tag and global owner retained");
    auto free = trace.allocations.back(); free.operation = wperf::MemoryOperation::Free; free.timestamp_ns = 40; free.tid = 88; trace.allocations.push_back(free);
    Check(wperf::AnalyzeDiagnostics(trace, L"pool", options).memory[0].live_bytes == 0, "pool free matches across execution contexts");
}

void Contracts(const std::filesystem::path& path) {
    std::filesystem::create_directories(path);
    auto trace = Fixture(); wperf::CpuReportOptions options; options.json = true;
    wperf::Symbolizer symbols;
    for (const wchar_t* kind : {L"dpc", L"isr", L"interrupt", L"heap", L"pool", L"driver"}) {
        const auto analysis = wperf::AnalyzeDiagnostics(trace, kind, options);
        std::wostringstream output;
        wperf::JsonStreamWriter envelope(output); envelope.BeginObject(); envelope.Key(L"rows");
        Check(wperf::WriteDiagnosticRows(trace, analysis, kind, symbols, options, output, &envelope) > 0, "contract emits diagnostic rows");
        envelope.Key(L"quality"); envelope.Item(wperf::QualityJson(trace)); envelope.EndObject();
        std::ofstream file(path / (std::wstring(kind) + L".json"), std::ios::binary); file << wperf::ToUtf8(output.str());
        const auto json = nlohmann::json::parse(wperf::ToUtf8(output.str()));
        Check(json["rows"].size() > 0 && json["quality"]["incomplete"] == false, "diagnostic JSON envelope parses with integer metrics");
        if (std::wstring(kind) == L"pool") Check(json["rows"][0]["tag"] == "Test" && json["rows"][0]["pid"].is_null(), "pool JSON does not invent PID");
    }
}
