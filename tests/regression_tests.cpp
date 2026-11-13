#include "common.h"
#include "commands/live_control.h"
#include "cpu_report.h"
#include "disassembler.h"
#include "etw_decode.h"
#include "live_data.h"
#include "live_top.h"
#include "off_cpu_report.h"
#include "top_renderer.h"
#include "output.h"
#include "symbolizer.h"
#include "archive.h"
#include "analysis.h"
#include "etl_reader.h"


#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <psapi.h>
#include <fstream>

namespace wperf {
struct LiveCaptureTestAccess {
    static void Feed(LiveTopCapture& capture, EVENT_RECORD& event) { capture.OnEvent(&event); }
};
}

namespace {
int failures = 0;
void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
const GUID perf = {0xce1dbfb4,0x137e,0x4da6,{0x87,0xb0,0x3f,0x59,0xaa,0x10,0x2c,0xbc}};
const GUID thread = {0x3d6fa8d1,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}};
void Feed(wperf::LiveTopCapture& capture, GUID provider, BYTE opcode, uint64_t qpc,
          uint32_t pid, uint32_t tid, void* payload, USHORT size) {
    EVENT_RECORD event{};
    event.EventHeader.ProviderId = provider;
    event.EventHeader.EventDescriptor.Opcode = opcode;
    event.EventHeader.Flags = EVENT_HEADER_FLAG_64_BIT_HEADER;
    event.EventHeader.TimeStamp.QuadPart = qpc;
    event.EventHeader.ProcessId = pid;
    event.EventHeader.ThreadId = tid;
    event.UserData = payload;
    event.UserDataLength = size;
    wperf::LiveCaptureTestAccess::Feed(capture, event);
}

void TestProfileDecoder() {
    wperf::TraceData trace;
    uint32_t interval[] = {0, 20000, 10000};
    EVENT_RECORD event{};
    event.EventHeader.EventDescriptor.Opcode = 73;
    event.UserData = interval;
    event.UserDataLength = sizeof(interval);
    Check(wperf::DecodeProfileInterval(event, trace), "decode CPU interval metadata");
    Check(trace.profile_interval_ns == 2000000, "CPU intervals are converted from 100ns");
    interval[0] = 19; interval[1] = 65536;
    Check(wperf::DecodeProfileInterval(event, trace), "decode PMC interval metadata");
    std::array<BYTE,16> payload{};
    const uint64_t ip = 0x12345678;
    const uint32_t tid = 42;
    const uint16_t source = 19;
    std::memcpy(payload.data(), &ip, 8);
    std::memcpy(payload.data()+8, &tid, 4);
    std::memcpy(payload.data()+12, &source, 2);
    event.EventHeader.EventDescriptor.Opcode = 47;
    event.EventHeader.Flags = EVENT_HEADER_FLAG_64_BIT_HEADER | EVENT_HEADER_FLAG_PROCESSOR_INDEX;
    event.BufferContext.ProcessorIndex = 300;
    event.UserData = payload.data(); event.UserDataLength = static_cast<USHORT>(payload.size());
    wperf::SampleRecord sample;
    Check(wperf::DecodeProfileSample(event, trace, sample), "decode PMC interrupt");
    Check(sample.kind == wperf::SampleKind::Pmc && sample.profile_source == 19 &&
          sample.period == 65536 && sample.cpu == 300 && sample.tid == 42 &&
          sample.instruction_pointer == ip, "PMC identity and native period preserved");
    Check(wperf::SampleWeightNs(trace, sample) == 0, "PMC count is not CPU time");
    event.UserDataLength = 15;
    Check(!wperf::DecodeProfileSample(event, trace, sample), "reject truncated PMC interrupt");
    event.UserDataLength = 16; event.EventHeader.EventDescriptor.Opcode = 46;
    Check(wperf::DecodeProfileSample(event, trace, sample) && sample.period == 2000000,
          "CPU sample keeps its sampling period");
    sample.timestamp_ns = 1;
    Check(wperf::SampleWeightNs(trace, sample) == 2000000, "weight uses period, not distance to next sample");
}

void TestLiveWindowCodec() {
    wperf::TraceData original;
    original.profile_interval_ns = 1000000;
    original.quality.events_total = 500;
    original.quality.unknown_events = 2;
    original.quality.samples_total = 1;
    original.quality.stack_exact_total = 1;
    wperf::ImageRecord image;
    image.pid = 42; image.base = 0x1000; image.size = 0x1000; image.load_ns = 10;
    image.path = L"C:\\path with spaces\\sample.dll";
    original.images.push_back(image);
    original.processes.push_back({42, 0, L"sample.exe", L"", 1, 0});
    original.threads.push_back({42, 7, 1, 0});
    wperf::SampleRecord sample;
    sample.timestamp_ns = 123456; sample.cpu = 3; sample.pid = 42; sample.tid = 7;
    sample.instruction_pointer = 0x1010; sample.period = 1000000;
    sample.stack = [] {
        std::vector<uint64_t> frames;
        for (uint64_t index = 0; index < 300; ++index) frames.push_back(0x1000 + index * 8);
        return frames;
    }();
    sample.pmc_counters = {11, 22};
    sample.stack_quality = wperf::StackQuality::Exact;
    original.samples.push_back(sample);

    const auto lines = wperf::EncodeLiveWindow(original, 250);
    wperf::LiveWindowDecoder decoder;
    std::wstring error;
    for (const std::wstring& line : lines) {
        bool ok = false;
        if (line.rfind(L"TOP2_BEGIN ", 0) == 0) ok = decoder.Begin(line, &error);
        else if (line.rfind(L"TOP2_", 0) == 0) ok = decoder.Consume(line, &error);
        Check(ok, "structured live IPC message decodes");
        if (!ok) break;
    }
    wperf::TraceData decoded;
    uint64_t interval = 0;
    Check(decoder.Finish(decoded, interval, &error), "structured live IPC window finishes");
    Check(interval == 250, "structured live IPC preserves interval");
    Check(decoded.images.size() == 1 && decoded.images[0].path == image.path,
          "structured live IPC preserves image identity");
    Check(decoded.processes.size() == 1 && decoded.processes[0].image_name == L"sample.exe",
          "structured live IPC preserves process metadata");
    Check(decoded.samples.size() == 1 && decoded.samples[0].stack.size() == 300,
          "structured live IPC preserves complete stacks");
    Check(decoded.samples[0].pmc_counters == std::vector<uint64_t>({11, 22}),
          "structured live IPC preserves PMC counters");
}

void TestLiveWindowLongPayloadBound() {
    wperf::TraceData original;
    original.quality.samples_total = 1;
    wperf::ProcessRecord process;
    process.pid = 42;
    process.image_name = L"sample.exe";
    process.command_line.assign(30000, static_cast<wchar_t>(0x4e2d));
    original.processes.push_back(process);
    wperf::SampleRecord sample;
    sample.timestamp_ns = 100;
    sample.pid = 42;
    sample.tid = 7;
    sample.instruction_pointer = 0x1000;
    std::vector<uint64_t> frames;
    frames.reserve(10000);
    for (uint64_t index = 0; index < 10000; ++index) {
        frames.push_back(0x1000 + index * 8);
    }
    sample.stack = frames;
    sample.stack_quality = wperf::StackQuality::Exact;
    original.samples.push_back(sample);

    const auto lines = wperf::EncodeLiveWindow(original, 1000);
    wperf::LiveWindowDecoder decoder;
    std::wstring error;
    size_t max_line = 0;
    for (const std::wstring& line : lines) {
        max_line = std::max(max_line, line.size());
        bool ok = false;
        if (line.rfind(L"TOP2_BEGIN ", 0) == 0) ok = decoder.Begin(line, &error);
        else if (line.rfind(L"TOP2_", 0) == 0) ok = decoder.Consume(line, &error);
        Check(ok, "bounded live IPC message decodes");
        if (!ok) break;
    }
    wperf::TraceData decoded;
    uint64_t interval = 0;
    Check(decoder.Finish(decoded, interval, &error), "bounded live IPC window finishes");
    Check(max_line < 65536, "every live IPC message stays below the pipe receive limit");
    Check(decoded.processes.size() == 1 &&
              decoded.processes[0].command_line.size() <
                  original.processes[0].command_line.size(),
          "long command lines are truncated instead of failing the window");
    Check(decoded.samples.size() == 1 && !decoded.samples[0].stack.empty() &&
              decoded.samples[0].stack.size() < original.samples[0].stack.size(),
          "oversized stacks are truncated at item boundaries");
    Check(!decoded.samples.empty() &&
              decoded.samples[0].stack_quality == wperf::StackQuality::Approximate,
          "stack truncation degrades exact stack quality to approximate");
    Check(decoded.quality.metadata_dropped >= 2,
          "live IPC truncation is reported in quality metadata");
    Check(decoded.quality.incomplete,
          "field truncation marks the live window incomplete even without window overflow");

    wperf::TraceData budgeted = original;
    budgeted.quality.samples_total = 700;
    budgeted.samples.clear();
    for (size_t index = 0; index < 700; ++index) {
        wperf::SampleRecord repeated = sample;
        repeated.timestamp_ns += index;
        budgeted.samples.push_back(repeated);
    }
    const auto budgeted_lines = wperf::EncodeLiveWindow(budgeted, 1000);
    size_t budgeted_chars = 0;
    for (const std::wstring& line : budgeted_lines) budgeted_chars += line.size() + 1;
    Check(budgeted_chars < 8 * 1024 * 1024 + 4096,
          "the complete live window respects its total encoded budget");
    wperf::LiveWindowDecoder budgeted_decoder;
    for (const std::wstring& line : budgeted_lines) {
        bool ok = false;
        if (line.rfind(L"TOP2_BEGIN ", 0) == 0) ok = budgeted_decoder.Begin(line, &error);
        else if (line.rfind(L"TOP2_", 0) == 0) ok = budgeted_decoder.Consume(line, &error);
        Check(ok, "budgeted live window messages decode");
        if (!ok) break;
    }
    wperf::TraceData budgeted_decoded;
    Check(budgeted_decoder.Finish(budgeted_decoded, interval, &error),
          "budgeted live window finishes");
    Check(budgeted_decoded.quality.samples_total == 700 &&
              budgeted_decoded.quality.samples_dropped > 0 &&
              budgeted_decoded.samples.size() < 700 &&
              budgeted_decoded.quality.incomplete,
          "whole-window overflow is explicitly counted and marks the window incomplete");

    const auto process_line = std::find_if(
        lines.begin(), lines.end(), [](const std::wstring& line) {
            return line.rfind(L"TOP2_PROCESS ", 0) == 0;
        });
    Check(process_line != lines.end() && process_line->size() < 65536,
          "long metadata is encoded as one bounded pipe message");
    if (process_line != lines.end()) {
        const std::wstring correlation_id = wperf::NewId();
        wperf::PipeServer server;
        Check(server.Create(correlation_id, &error), "create bounded IPC test pipe");
        bool client_ok = false;
        std::wstring client_error;
        std::thread client_thread([&] {
            wperf::PipeClient client;
            if (!client.Connect(wperf::MakePipePath(correlation_id), 3000, &client_error)) {
                return;
            }
            client_ok = client.SendLine(*process_line, &client_error);
        });
        Check(server.WaitForClient(3000, &error), "bounded IPC test client connects");
        std::wstring received;
        Check(server.ReceiveLine(received, 5000, &error), "bounded long message crosses the pipe");
        client_thread.join();
        Check(client_ok && received == *process_line,
              "bounded long Unicode metadata round-trips through the named pipe");
    }
}

void TestPipeSendDeadline() {
    const std::wstring correlation_id = wperf::NewId();
    wperf::PipeServer server;
    std::wstring error;
    Check(server.Create(correlation_id, &error), "create send-deadline test pipe");
    bool final_result = true;
    std::wstring send_error;
    ULONGLONG elapsed_ms = 0;
    std::thread client_thread([&] {
        wperf::PipeClient client;
        if (!client.Connect(wperf::MakePipePath(correlation_id), 3000, &send_error)) {
            final_result = false;
            return;
        }
        const ULONGLONG started = GetTickCount64();
        const std::wstring payload(16000, L'x');
        for (size_t index = 0; index < 64 && final_result; ++index) {
            final_result = client.SendLine(payload, &send_error, 500);
        }
        elapsed_ms = GetTickCount64() - started;
    });
    Check(server.WaitForClient(3000, &error), "send-deadline client connects");
    client_thread.join();
    Check(!final_result && !send_error.empty() && elapsed_ms < 3000,
          "a stalled pipe reader cannot block the worker send indefinitely");
}

void TestLiveIdentityAndCorrelation() {
    wperf::LiveTopCapture capture;
    uint64_t syscall = 0x1234;
    Feed(capture, perf, 51, 10, UINT32_MAX, UINT32_MAX, &syscall, 8);
    Feed(capture, perf, 52, 20, UINT32_MAX, UINT32_MAX, &syscall, 8);
    auto anonymous = capture.Snapshot();
    Check(!anonymous.events.back().has_duration, "anonymous syscalls never invent duration");

    uint32_t identity[] = {100,10};
    std::array<BYTE,16> sample{};
    std::memcpy(sample.data(), &syscall, 8);
    std::memcpy(sample.data()+8, identity+1, 4);
    Feed(capture, thread, 1, 100, 100, 10, identity, 8);
    Feed(capture, perf, 46, 150, UINT32_MAX, 10, sample.data(), 16);
    Feed(capture, thread, 2, 200, 100, 10, identity, 8);
    identity[0] = 200;
    Feed(capture, thread, 1, 300, 200, 10, identity, 8);
    Feed(capture, perf, 46, 350, UINT32_MAX, 10, sample.data(), 16);
    auto reused = capture.Snapshot();
    Check(reused.samples.size() == 2 && reused.samples[0].pid == 100 && reused.samples[1].pid == 200,
          "pending samples retain exited and reused thread identity");
    uint32_t ready[] = {10,0};
    Feed(capture, thread, 50, 400, 200, 10, ready, 8);
    capture.Snapshot();
    uint32_t sw[] = {10,20,0,0};
    Feed(capture, thread, 36, 500, UINT32_MAX, UINT32_MAX, sw, 16);
    auto cross_window = capture.Snapshot();
    Check(cross_window.events[0].has_duration && cross_window.events[0].duration_ns == 10000,
          "ready correlation survives snapshot boundary");
    Feed(capture, perf, 51, 600, 200, 10, &syscall, 8);
    capture.Snapshot();
    Feed(capture, thread, 2, 700, 200, 10, identity, 8);
    identity[0] = 300;
    Feed(capture, thread, 1, 800, 300, 10, identity, 8);
    Feed(capture, perf, 52, 900, 300, 10, &syscall, 8);
    auto wrong_lifetime = capture.Snapshot();
    Check(!wrong_lifetime.events.back().has_duration, "syscall never crosses thread lifetimes");
}

void TestBoundedCorrelation() {
    wperf::TraceCorrelationState state(2);
    wperf::TraceData trace;
    for (uint32_t tid = 1; tid <= 3; ++tid) {
        wperf::TraceEvent event;
        event.kind = wperf::TraceEventKind::SyscallEnter;
        event.tid = tid; event.timestamp_ns = tid;
        trace.events.push_back(event);
    }
    state.Enrich(trace);
    Check(trace.quality.correlation_evicted == 2 && trace.quality.incomplete,
          "pending-state overflow is bounded and visible");
    wperf::TraceData exit;
    wperf::TraceEvent event;
    event.kind = wperf::TraceEventKind::SyscallExit; event.tid = 1; event.timestamp_ns = 4;
    exit.events.push_back(event); state.Enrich(exit);
    Check(!exit.events[0].has_duration && exit.quality.syscall_orphaned == 1,
          "evicted state cannot fabricate a duration");
}

void TestAddressSpacesAndNames() {
    wperf::TraceData trace;
    trace.images.push_back({4,0xfffff80000000000ull,0x100000,0,0,0,0,L"ntoskrnl.exe"});
    trace.images.push_back({200,0x1000,0x1000,0,0,0,0,L"other.exe"});
    trace.processes.push_back({100,0,L"before.exe",L"",100,200});
    trace.processes.push_back({100,0,L"after.exe",L"",300,0});
    wperf::ImageIndex images; images.Build(trace);
    Check(images.Find(100,0xfffff80000001000ull,150) != nullptr, "kernel addresses use shared module map");
    Check(images.Find(100,0x1100,150) == nullptr, "user addresses never borrow another process module");
    wperf::ProcessNameIndex names(trace);
    Check(names.Name(100,150) == L"before.exe" && names.Name(100,350) == L"after.exe",
          "PID reuse keeps time-specific command name");
    Check(wperf::ProcessDisplayName(trace,100,150) == L"before.exe", "public name lookup respects time");
}

void TestSymbolSessionIsolation() {
    MODULEINFO info{};
    const HMODULE module = GetModuleHandleW(nullptr);
    Check(GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) != FALSE,
          "read test module information");
    wperf::ImageRecord image;
    image.pid = GetCurrentProcessId(); image.base = reinterpret_cast<uint64_t>(module);
    image.size = info.SizeOfImage; image.path = wperf::GetExecutablePath();
    wperf::TraceData trace; trace.images.push_back(image);
    wperf::Symbolizer first;
    std::wstring error;
    Check(first.Initialize(L"",&error), "initialize first symbol session"); first.AddModule(image);
    const auto address = reinterpret_cast<uint64_t>(&TestSymbolSessionIsolation);
    const auto initial = first.Resolve(trace,image.pid,address,1);
    Check(initial.resolved, "first session resolves own test function");
    {
        wperf::Symbolizer second;
        Check(second.Initialize(L"",&error), "initialize simultaneous symbol session");
        second.AddModule(image);
        Check(second.Resolve(trace,image.pid,address,1).resolved, "second session resolves same synthetic range");
    }
    // Resolve a new address to avoid only exercising the first session's cache.
    Check(first.Resolve(trace,image.pid,reinterpret_cast<uint64_t>(&TestProfileDecoder),1).resolved,
          "closing second session preserves first session's DbgHelp state");
    const auto pdb = first.PdbPath(image);
    const auto identity = wperf::ReadModuleIdentity(image.path);
    Check(!pdb.empty() && wperf::PdbIdentityMatches(pdb, identity),
          "find and validate identity-matching PDB through the symbol path");
    auto mismatch = identity;
    ++mismatch.pdb_age;
    Check(!wperf::PdbIdentityMatches(pdb, mismatch), "archive cannot trust mismatched PDB age");
    auto stale = image;
    stale.timestamp = identity.timestamp ^ 1;
    std::vector<wperf::DisassembledInstruction> instructions(1);
    Check(!wperf::DisassembleImageAddress(stale, address, 8, 1, instructions, &error) &&
          instructions.empty() && error.find(L"identity") != std::wstring::npos,
          "wrong-version disassembly is rejected and clears stale instructions");
}

void TestLeafAndMachineOutput() {
    wperf::TraceData trace;
    trace.images.push_back({1,0x1000,0x1000,0,0,0,0,L"a\"b.exe"});
    trace.images.push_back({1,0x3000,0x1000,0,0,0,0,L"caller.dll"});
    wperf::SampleRecord sample;
    sample.pid=1; sample.instruction_pointer=0x1100; sample.stack={0x3100};
    trace.samples.push_back(sample);
    wperf::Symbolizer symbols;
    wperf::CpuReportOptions options; options.csv=true;
    std::wostringstream table; wperf::WriteCpuReport(trace,symbols,options,table);
    Check(table.str().find(L"caller.dll") == std::wstring::npos, "table hotspot comes from sampled IP");
    Check(table.str().find(L"a\"\"b.exe") != std::wstring::npos, "CSV quotes are doubled");
    Check(wperf::EscapeJson(std::wstring{L'"',L'\\',L'\n',wchar_t{1}}) == L"\\\"\\\\\\n\\u0001",
          "JSON escapes quotes, backslashes, newlines and controls");
    options.csv=false; options.folded=true; options.exact_stacks_only=true;
    std::wostringstream excluded; wperf::WriteCpuReport(trace,symbols,options,excluded);
    Check(excluded.str().empty(), "exact-only output excludes uncertain stacks");
}

void TestReadyHandshakeAndDisconnect() {
    wperf::PipeServer server;
    const std::wstring id = wperf::NewId();
    std::wstring error;
    Check(server.Create(id,&error), "create READY test pipe");
    HANDLE worker = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic_bool ready_sent{false};
    std::thread sender([&] {
        wperf::PipeClient client;
        if (!client.Connect(wperf::MakePipePath(id),2000,nullptr)) return;
        client.SendLine(L"PHASE CAPTURING",nullptr);
        Sleep(30);
        ready_sent.store(true);
        client.SendLine(L"READY test-session",nullptr);
        Sleep(30);
    });
    Check(server.WaitForClient(2000,&error), "connect READY worker");
    Check(wperf::WaitForCaptureReady(server,worker,L"test-session",2000,&error) && ready_sent.load(),
          "target startup waits for matching READY, not a phase message");
    sender.join(); CloseHandle(worker); server.Close();

    const std::wstring disconnect_id = wperf::NewId();
    Check(server.Create(disconnect_id,&error), "create disconnect test pipe");
    wperf::PipeClient client;
    Check(client.Connect(wperf::MakePipePath(disconnect_id),2000,&error), "connect disconnect client");
    Check(server.WaitForClient(2000,&error), "accept disconnect client");
    server.Close();
    Check(!client.IsConnected(), "broken pipe is detected even without pending messages");
}

#if defined(NDEBUG)
__forceinline int InlineInner(volatile int* value,int index) { return *value+index*3; }
__forceinline int InlineOuter(volatile int* value,int index) { return InlineInner(value,index)+*value*index; }
__declspec(noinline) int InlineAnchor(int input) {
    volatile int value=input; int result=0;
    for(int i=0;i<40;++i)result+=InlineOuter(&value,i);
    return result;
}
#endif

void TestPeriodsIdentityArchiveAndInline() {
    wperf::TraceData trace;
    trace.profile_interval_ns=2000000;
    wperf::SampleRecord unknown;
    Check(wperf::SampleWeightNs(trace,unknown)==0,"late metadata does not backfill unknown period");
    trace.processes.push_back({100,0,L"sample.exe",L"",10,200});
    trace.processes.push_back({100,0,L"sample.exe",L"",300,0});
    trace.images.push_back({100,0x1000,0x1000,1,0,0,0,L"C:/v1/plugin.dll"});
    trace.images.push_back({100,0x3000,0x1000,2,0,0,0,L"C:/v2/plugin.dll"});
    wperf::SampleRecord first; first.pid=100; first.timestamp_ns=100; first.instruction_pointer=0x1100;
    trace.samples.push_back(first); first.instruction_pointer=0x3100; trace.samples.push_back(first);
    first.timestamp_ns=350; first.instruction_pointer=0x1100; trace.samples.push_back(first);
    wperf::Symbolizer empty; wperf::CpuReportOptions options; options.json=true;
    std::wostringstream output; wperf::WriteCpuReport(trace,empty,options,output);
    size_t rows=0,position=0; while((position=output.str().find(L"\"samples\":1",position))!=std::wstring::npos){++rows;position+=10;}
    Check(rows==3,"DLL versions and PID lifetimes stay separate");
    Check(output.str().find(L"sample.exe")!=std::wstring::npos,"Unicode survives JSON report");

    MODULEINFO information{}; auto module=GetModuleHandleW(nullptr);
    GetModuleInformation(GetCurrentProcess(),module,&information,sizeof(information));
    wperf::ImageRecord image; image.pid=100; image.base=reinterpret_cast<uint64_t>(module);
    image.size=information.SizeOfImage; image.path=wperf::GetExecutablePath();
    const auto identity=wperf::ReadModuleIdentity(image.path);
    image.timestamp=identity.timestamp; image.checksum=identity.checksum;
    wperf::TraceData symbols_trace; symbols_trace.images.push_back(image);
    const auto directory=std::filesystem::temp_directory_path()/(L"wperf-symbols-"+wperf::NewId());
    std::wstring error;
#if defined(NDEBUG)
    const auto cache=directory/L"cache";
    uint64_t inline_address=0;
    Check(InlineAnchor(17)!=0,"optimized inline fixture executes");
    {
        wperf::Symbolizer symbols; Check(symbols.Initialize(L"",&error,cache),"initialize isolated symcache"); symbols.AddModule(image);
        const uint64_t address=reinterpret_cast<uint64_t>(&InlineAnchor);
        auto anchor=symbols.Resolve(symbols_trace,100,address,1);
        const uint64_t count=anchor.function_size?std::min<uint64_t>(anchor.function_size,2048):512;
        for(uint64_t offset=0;offset<count;++offset) {
            const auto symbol=symbols.Resolve(symbols_trace,100,address+offset,1);
            for(const auto& frame:symbol.inline_frames)
                if(frame.symbol.find(L"InlineInner")!=std::wstring::npos||frame.symbol.find(L"InlineOuter")!=std::wstring::npos)
                    inline_address=address+offset;
            if(inline_address)break;
        }
        Check(inline_address!=0,"optimized PDB expands inline frames");
    }
    {
        wperf::Symbolizer symbols; symbols.Initialize(L"",&error,cache); symbols.AddModule(image);
        if(inline_address) {
            const auto restored=symbols.Resolve(symbols_trace,100,inline_address,1);
            Check(!restored.inline_frames.empty()&&symbols.Stats().cache_hits>0,"dedicated symcache restores inline context");
        }
    }
#else
    std::cout << "SKIP: inline PDB checks require an optimized build\n";
#endif
    // Build a verified archive map whose recorded path no longer exists.
    const auto archived=directory/L"modules"/L"fixture.exe";
    wperf::EnsureDirectory(archived.parent_path(),nullptr);
    std::filesystem::copy_file(image.path,archived);
    image.path=L"Z:/unavailable/fixture.exe"; symbols_trace.images={image};
    {
        std::ofstream map(directory/L"modules.tsv",std::ios::binary);
        map<<"wperf-archive-map-v1\n"<<wperf::HexText(image.path)<<' '<<image.timestamp<<' '<<image.checksum<<' '
           <<image.size<<' '<<wperf::HexText(L"modules/fixture.exe")<<' '<<wperf::ToUtf8(wperf::FileSha256(archived))<<'\n';
    }
    const bool mapped=wperf::LoadArchiveModules(directory,symbols_trace,&error);
    std::error_code equivalent_error;
    Check(mapped&&std::filesystem::equivalent(symbols_trace.images[0].resolved_path,archived,equivalent_error),
          "archive maps missing original paths after hash/PE validation");
    if(!mapped)std::wcerr<<L"archive map error: "<<error<<L"\n";
    {
        std::ofstream file(archived,std::ios::binary|std::ios::app); file<<'X';
    }
    Check(!wperf::LoadArchiveModules(directory,symbols_trace,&error),"archive detects tampered binary");
    std::error_code ignored; std::filesystem::remove_all(directory,ignored);
}

void TestModernStackKeyEpochs() {
    const GUID stack_guid={0xdef2fe46,0x7bd6,0x4b80,{0xbd,0x94,0xf5,0x7f,0xe2,0x0d,0x0c,0xe3}};
    std::vector<std::vector<BYTE>> storage;
    std::vector<EVENT_RECORD> records;
    const auto record=[&](GUID provider,BYTE opcode,uint64_t time,std::vector<BYTE> payload) {
        storage.push_back(std::move(payload)); EVENT_RECORD event{};
        event.EventHeader.ProviderId=provider; event.EventHeader.Flags=EVENT_HEADER_FLAG_64_BIT_HEADER;
        event.EventHeader.ProcessId=100; event.EventHeader.EventDescriptor.Opcode=opcode;
        event.EventHeader.TimeStamp.QuadPart=time; event.UserData=storage.back().data();
        event.UserDataLength=static_cast<USHORT>(storage.back().size()); records.push_back(event);
    };
    const auto sample=[&](uint64_t time,uint64_t address) {
        std::vector<BYTE> bytes(16); uint32_t tid=10; memcpy(bytes.data(),&address,8); memcpy(bytes.data()+8,&tid,4);
        record(perf,46,time,std::move(bytes));
        bytes.assign(24,0); uint32_t pid=100; uint64_t key=0xaa;
        memcpy(bytes.data(),&time,8); memcpy(bytes.data()+8,&pid,4); memcpy(bytes.data()+12,&tid,4); memcpy(bytes.data()+16,&key,8);
        record(stack_guid,38,time,std::move(bytes));
    };
    const auto definition=[&](uint64_t time,uint64_t address) {
        std::vector<BYTE> bytes(24); const uint64_t values[]={0xaa,address,0x5000}; memcpy(bytes.data(),values,24);
        record(stack_guid,35,time,std::move(bytes));
    };
    sample(100,0x1010); definition(110,0x1010);
    sample(200,0x2020); definition(210,0x2020);
    sample(300,0x1010); definition(310,0x1010);
    wperf::TraceData trace; wperf::ReadEtwRecords(records,trace);
    Check(trace.quality.stack_exact_total==3&&trace.samples[0].stack.front()==0x1010&&trace.samples[1].stack.front()==0x2020,
          "reused StackWalk key resolves against the correct delete/rundown epoch");
    Check(&trace.samples[0].stack.View()==&trace.samples[2].stack.View(),"repeated stacks share interned frame storage");
}

void TestRawPayloadsAndCounterDomains() {
    EVENT_RECORD event{}; std::array<BYTE,24> payload{};
    event.UserData=payload.data(); event.UserDataLength=24; event.EventHeader.Flags=EVENT_HEADER_FLAG_64_BIT_HEADER;
    event.EventHeader.EventDescriptor.Opcode=14; uint64_t ticks=100,irp=0x123;
    memcpy(payload.data()+8,&ticks,8); memcpy(payload.data()+16,&irp,8);
    wperf::TraceEvent decoded;
    Check(wperf::DecodeKernelEvent(event,8,10000000,wperf::TraceEventKind::DiskIo,decoded)&&
          decoded.correlation_id==irp&&decoded.duration_ns==10000&&decoded.is_completion,"disk flush uses its own payload layout");
    for(USHORT length=0;length<24;++length) { event.UserDataLength=length;
        Check(!wperf::DecodeKernelEvent(event,8,10000000,wperf::TraceEventKind::DiskIo,decoded),"truncated flush rejected"); }
    wperf::TraceData trace; trace.pmc_counter_sources={19,26};
    for(uint32_t cpu=0;cpu<2;++cpu)for(uint32_t step=0;step<2;++step) {
        wperf::TraceEvent point; point.kind=wperf::TraceEventKind::ContextSwitch;
        point.cpu=cpu; point.timestamp_ns=10+step; point.related_tid=20; point.related_pid=200;
        point.pmc_counters={1000*cpu+100+step*(20+cpu*10),5000*cpu+200+step*40}; trace.events.push_back(point);
    }
    const auto rows=wperf::AggregatePmcCounters(trace);
    uint64_t cycles=0,instructions=0;
    for(const auto& row:rows) { if(row.source==19)cycles+=row.delta;else instructions+=row.delta; }
    Check(rows.size()==4&&cycles==50&&instructions==80,"PMC differences stay within CPU/source and old-thread domains");
    uint32_t seed=1;
    for(size_t i=0;i<2000;++i) {
        seed=seed*1664525+1013904223;
        event.UserDataLength=static_cast<USHORT>(seed%24); event.EventHeader.EventDescriptor.Opcode=BYTE(seed%80);
        wperf::DecodeKernelEvent(event,8,10000000,wperf::TraceEventKind::FileIo,decoded);
    }
}

void TestStreamingAndStopDeadline() {
    const auto fixture=std::filesystem::path(WPERF_TEST_DATA_DIR)/L"cpu-stack.etl";
    wperf::TraceData trace; std::wstring error;
    Check(wperf::ReadEtwMetadata(fixture,trace,&error)&&trace.samples.empty()&&!trace.stacks_loaded,
          "metadata pass does not materialize sample or event windows");
    uint64_t samples=0;
    Check(wperf::StreamEtwSamples(fixture,trace,[&](const auto&){++samples;},&error)&&samples==17323&&trace.samples.empty(),
          "streaming consumes all real ETL samples without retaining them");
    const auto executable=std::filesystem::path(wperf::GetExecutablePath()).parent_path()/L"wperf.exe";
    SetEnvironmentVariableW(L"WPERF_ENABLE_TEST_FAULTS",L"1");
    wperf::ProcessLaunchResult child;
    Check(wperf::LaunchWorkerProcess(executable.wstring(),{L"--internal-stop-timeout-test"},false,child,&error),
          "launch owned teardown-timeout fixture");
    if(child.process) {
        DWORD code=0;
        Check(wperf::WaitForProcessExit(child.process,3000,code)&&code==ERROR_TIMEOUT,
              "worker teardown watchdog exits within its deadline");
        CloseHandle(child.process);
    }
    SetEnvironmentVariableW(L"WPERF_ENABLE_TEST_FAULTS",nullptr);
}

void TestRawSchedulerStackAssociation() {
    const GUID thread_guid = {0x3d6fa8d1,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}};
    const GUID stack_guid = {0xdef2fe46,0x7bd6,0x4b80,{0xbd,0x94,0xf5,0x7f,0xe2,0x0d,0x0c,0xe3}};
    std::vector<std::vector<BYTE>> storage;
    storage.reserve(2);
    std::vector<EVENT_RECORD> records;
    records.reserve(2);
    const auto add = [&](GUID provider, BYTE opcode, uint64_t qpc,
                         std::vector<BYTE> payload) {
        storage.push_back(std::move(payload));
        EVENT_RECORD record{};
        record.EventHeader.ProviderId = provider;
        record.EventHeader.EventDescriptor.Opcode = opcode;
        record.EventHeader.Flags = EVENT_HEADER_FLAG_64_BIT_HEADER;
        record.EventHeader.TimeStamp.QuadPart = qpc;
        record.UserData = storage.back().data();
        record.UserDataLength = static_cast<USHORT>(storage.back().size());
        records.push_back(record);
    };
    std::vector<BYTE> switch_payload(16);
    const uint32_t incoming = 20, outgoing = 10;
    std::memcpy(switch_payload.data(), &incoming, 4);
    std::memcpy(switch_payload.data() + 4, &outgoing, 4);
    switch_payload[12] = 6;
    switch_payload[14] = 5;
    add(thread_guid, 36, 100, std::move(switch_payload));
    std::vector<BYTE> stack_payload(16 + 2 * sizeof(uint64_t));
    const uint64_t qpc = 100, frame1 = 0x1000, frame2 = 0x2000;
    const uint32_t pid = 100, tid = 10;
    std::memcpy(stack_payload.data(), &qpc, 8);
    std::memcpy(stack_payload.data() + 8, &pid, 4);
    std::memcpy(stack_payload.data() + 12, &tid, 4);
    std::memcpy(stack_payload.data() + 16, &frame1, 8);
    std::memcpy(stack_payload.data() + 24, &frame2, 8);
    add(stack_guid, 32, 100, std::move(stack_payload));
    wperf::TraceData trace;
    Check(wperf::ReadEtwRecords(records, trace), "raw scheduler stack replay succeeds");
    Check(trace.events.size() == 1 && trace.events[0].related_tid == outgoing &&
              trace.events[0].tid == incoming,
          "CSwitch decoder keeps incoming and outgoing thread contract");
    Check(trace.events[0].stack.size() == 2 &&
              trace.events[0].stack.front() == frame1 &&
              trace.events[0].stack_quality == wperf::StackQuality::Exact,
          "Raw CSwitch stack attaches to the outgoing thread");
}

void TestOffCpuIntervals() {
    wperf::TraceData trace;
    trace.threads.push_back({100, 10, 1, 0});
    trace.threads.push_back({200, 20, 1, 0});
    trace.quality.events_total = 3;
    trace.start_ns = 100;
    trace.end_ns = 800;
    wperf::TraceEvent leave;
    leave.kind = wperf::TraceEventKind::ContextSwitch;
    leave.timestamp_ns = 100; leave.cpu = 0; leave.pid = UINT32_MAX;
    leave.tid = 20; leave.related_tid = 10; leave.related_pid = UINT32_MAX;
    leave.previous_state = 5; leave.wait_reason = 6;
    leave.stack = {0x1000, 0x2000};
    leave.stack_quality = wperf::StackQuality::Exact;
    leave.stack_missing = false;
    trace.events.push_back(leave);
    wperf::TraceEvent ready;
    ready.kind = wperf::TraceEventKind::ReadyThread;
    ready.timestamp_ns = 300; ready.cpu = 0; ready.pid = 200;
    ready.tid = 20; ready.related_tid = 10; ready.related_pid = 100;
    ready.stack = {0x3000};
    ready.stack_quality = wperf::StackQuality::Exact;
    ready.stack_missing = false;
    trace.events.push_back(ready);
    wperf::TraceEvent resume;
    resume.kind = wperf::TraceEventKind::ContextSwitch;
    resume.timestamp_ns = 800; resume.cpu = 0; resume.pid = UINT32_MAX;
    resume.tid = 10; resume.related_tid = 20; resume.related_pid = 200;
    resume.previous_state = 1;
    resume.stack = {0x4000};
    resume.stack_quality = wperf::StackQuality::Exact;
    resume.stack_missing = false;
    trace.events.push_back(resume);
    wperf::EnrichTraceEvents(trace);
    wperf::BuildOffCpuIntervals(trace);
    Check(trace.off_cpu_intervals.size() == 2, "off-CPU blocked and ready intervals are separated");
    if (trace.off_cpu_intervals.size() == 2) {
        Check(trace.off_cpu_intervals[0].kind == wperf::OffCpuKind::Blocked &&
                  trace.off_cpu_intervals[0].duration_ns == 200,
              "blocked interval ends when the thread becomes ready");
        Check(trace.off_cpu_intervals[1].kind == wperf::OffCpuKind::Ready &&
                  trace.off_cpu_intervals[1].duration_ns == 500,
              "ready interval ends when the thread resumes");
        Check(trace.off_cpu_intervals[0].pid == 100,
              "outgoing thread PID falls back to lifecycle metadata");
        Check(trace.off_cpu_intervals[0].wakeup_tid == 20,
              "wakeup thread identity is preserved");
        Check(trace.off_cpu_intervals[0].stack.size() == 2 &&
                  trace.off_cpu_intervals[0].stack.front() == 0x1000,
              "off-CPU interval retains the switching-out stack");
    }
}

void TestOffCpuTruncatedInterval() {
    wperf::TraceData trace;
    trace.threads.push_back({100, 10, 1, 0});
    trace.start_ns = 100;
    trace.end_ns = 800;
    wperf::TraceEvent leave;
    leave.kind = wperf::TraceEventKind::ContextSwitch;
    leave.timestamp_ns = 100; leave.cpu = 1; leave.pid = UINT32_MAX;
    leave.tid = 20; leave.related_tid = 10; leave.related_pid = 100;
    leave.previous_state = 5; leave.wait_reason = 6;
    trace.events.push_back(leave);
    wperf::BuildOffCpuIntervals(trace);
    Check(trace.off_cpu_intervals.size() == 1 &&
              trace.off_cpu_intervals[0].kind == wperf::OffCpuKind::Blocked &&
              trace.off_cpu_intervals[0].duration_ns == 700 &&
              trace.off_cpu_intervals[0].boundary_inferred,
          "end-of-trace off-CPU closure is retained as a truncated interval");
    Check(trace.quality.off_cpu_inferred_intervals == 1 &&
              trace.quality.off_cpu_unclosed == 1,
          "truncated off-CPU intervals are counted in quality metadata");
}

wperf::TraceEvent OffCpuSwitch(uint64_t time, uint32_t incoming,
                               uint32_t outgoing, uint8_t state) {
    std::array<BYTE, 16> payload{};
    std::memcpy(payload.data(), &incoming, 4);
    std::memcpy(payload.data() + 4, &outgoing, 4);
    payload[12] = 6;
    payload[14] = state;
    EVENT_RECORD record{};
    record.EventHeader.TimeStamp.QuadPart = time;
    record.EventHeader.EventDescriptor.Opcode = 36;
    record.UserData = payload.data();
    record.UserDataLength = static_cast<USHORT>(payload.size());
    wperf::TraceEvent event;
    Check(wperf::DecodeKernelEvent(record, 8, 1000000000,
                                   wperf::TraceEventKind::ContextSwitch, event),
          "off-CPU transition starts with the actual CSwitch decoder");
    event.stack = {0x1100};
    event.stack_quality = wperf::StackQuality::Exact;
    event.stack_missing = false;
    return event;
}

wperf::TraceEvent OffCpuReady(uint64_t time, uint32_t target,
                              uint32_t waker, uint32_t waker_pid, uint64_t frame) {
    std::array<BYTE, 8> payload{};
    std::memcpy(payload.data(), &target, 4);
    EVENT_RECORD record{};
    record.EventHeader.TimeStamp.QuadPart = time;
    record.EventHeader.ThreadId = waker;
    record.EventHeader.ProcessId = waker_pid;
    record.EventHeader.EventDescriptor.Opcode = 50;
    record.UserData = payload.data();
    record.UserDataLength = static_cast<USHORT>(payload.size());
    wperf::TraceEvent event;
    Check(wperf::DecodeKernelEvent(record, 8, 1000000000,
                                   wperf::TraceEventKind::ReadyThread, event),
          "off-CPU wakeup starts with the actual ReadyThread decoder");
    event.stack = {frame};
    event.stack_quality = wperf::StackQuality::Exact;
    event.stack_missing = false;
    return event;
}

size_t CountRows(const std::wstring& json, const std::wstring& needle = L"\"kind\":") {
    size_t count = 0, offset = 0;
    while ((offset = json.find(needle, offset)) != std::wstring::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

void TestOffCpuConfirmedBlockAndReusedReady() {
    wperf::TraceData trace;
    trace.start_ns = 100; trace.end_ns = 800;
    trace.threads = {{100, 10, 1, 0}, {200, 20, 1, 0}};
    trace.events = {OffCpuSwitch(100, 20, 10, 5), OffCpuReady(300, 10, 20, 200, 0x2100)};
    wperf::EnrichTraceEvents(trace);
    wperf::BuildOffCpuIntervals(trace);
    Check(trace.off_cpu_intervals.size() == 2, "capture end retains blocked and ready intervals");
    if (trace.off_cpu_intervals.size() == 2) {
        Check(trace.off_cpu_intervals[0].duration_ns == 200 &&
                  !trace.off_cpu_intervals[0].boundary_inferred &&
                  trace.off_cpu_intervals[1].duration_ns == 500 &&
                  trace.off_cpu_intervals[1].boundary_inferred,
              "confirmed blocked endpoint survives an unclosed ready tail");
    }
    wperf::Symbolizer symbols;
    wperf::CpuReportOptions json; json.json = true;
    std::wostringstream report;
    wperf::WriteOffCpuReport(trace, symbols, json, report);
    Check(CountRows(report.str()) == 1 &&
              report.str().find(L"\"confirmed_duration_ns\":200") != std::wstring::npos,
          "default off-CPU report preserves confirmed blocked time at capture end");

    wperf::TraceData reused;
    reused.start_ns = 1; reused.end_ns = 400;
    reused.processes = {{100, 0, L"old.exe", L"", 1, 200},
                        {200, 0, L"new.exe", L"", 300, 0}};
    reused.threads = {{100, 10, 1, 200}, {200, 10, 300, 0}, {300, 20, 1, 0}};
    reused.events = {OffCpuSwitch(100, 20, 10, 5), OffCpuReady(300, 10, 20, 300, 0x2100),
                     OffCpuSwitch(400, 10, 20, 0)};
    wperf::EnrichTraceEvents(reused);
    wperf::BuildOffCpuIntervals(reused);
    Check(reused.off_cpu_intervals.size() == 1 && reused.quality.off_cpu_evicted == 1,
          "ReadyThread evicts only the stale TID lifetime and starts the new ready interval");
    if (!reused.off_cpu_intervals.empty()) {
        const auto& interval = reused.off_cpu_intervals.front();
        Check(interval.pid == 200 && interval.thread_start_ns == 300 &&
                  interval.process_start_ns == 300 && interval.duration_ns == 100 &&
                  !interval.boundary_inferred && interval.kind == wperf::OffCpuKind::Ready,
              "reused TID reports the new process's confirmed ready duration");
    }
}

void TestOffCpuWakeupSourcesAndStackTimes() {
    wperf::TraceData trace;
    trace.start_ns = 1; trace.end_ns = 120;
    trace.threads = {{100, 10, 1, 0}, {200, 20, 1, 0}, {300, 30, 1, 0}};
    trace.processes = {{100, 0, L"waiter.exe", L"", 1, 0},
                       {200, 0, L"first.exe", L"", 1, 0},
                       {300, 0, L"second.exe", L"", 1, 0}};
    trace.images = {{100, 0x1000, 0x1000, 1, 0, 1, 15, L"C:/waiting-old.dll"},
                    {100, 0x1000, 0x1000, 2, 0, 15, 0, L"C:/waiting-new.dll"},
                    {200, 0x2000, 0x1000, 3, 0, 1, 0, L"C:/first-waker.dll"},
                    {300, 0x2000, 0x1000, 4, 0, 1, 0, L"C:/second-waker.dll"}};
    trace.events = {OffCpuSwitch(10, 20, 10, 5), OffCpuReady(20, 10, 20, 200, 0x2100),
                    OffCpuSwitch(30, 10, 20, 0), OffCpuSwitch(40, 30, 10, 5),
                    OffCpuReady(50, 10, 30, 300, 0x2100), OffCpuSwitch(60, 10, 30, 0),
                    OffCpuSwitch(70, 20, 10, 5), OffCpuReady(80, 10, 20, 200, 0x2200),
                    OffCpuSwitch(90, 10, 20, 0), OffCpuSwitch(100, 20, 10, 5),
                    OffCpuReady(110, 10, 20, 200, 0x2100), OffCpuSwitch(120, 10, 20, 0)};
    wperf::EnrichTraceEvents(trace);
    wperf::BuildOffCpuIntervals(trace);
    Check(trace.off_cpu_intervals.size() == 8, "four waits retain their blocked and ready intervals");
    if (trace.off_cpu_intervals.size() == 8) {
        Check(trace.off_cpu_intervals[0].wakeup_pid == 200 &&
                  trace.off_cpu_intervals[0].wakeup_tid == 20 &&
                  trace.off_cpu_intervals[0].wakeup_process_start_ns == 1 &&
                  trace.off_cpu_intervals[0].wakeup_thread_start_ns == 1,
              "wakeup process and thread lifetimes are retained independently of the waiter");
        Check(trace.off_cpu_intervals[1].stack_timestamp_ns == 10 &&
                  trace.off_cpu_intervals[1].start_ns == 20,
              "ready interval retains the original switch-out stack capture time");
    }
    wperf::Symbolizer symbols;
    wperf::CpuReportOptions options; options.json = true;
    std::wostringstream json;
    wperf::WriteOffCpuReport(trace, symbols, options, json);
    Check(CountRows(json.str()) == 8 && CountRows(json.str(), L"\"intervals\":1") == 8,
          "different wakeup processes or stacks are not overwritten in a merged row");
    Check(json.str().find(L"\"wakeup_pid\":200") != std::wstring::npos &&
              json.str().find(L"\"wakeup_pid\":300") != std::wstring::npos &&
              json.str().find(L"\"module\":\"first-waker.dll\"") != std::wstring::npos &&
              json.str().find(L"\"module\":\"second-waker.dll\"") != std::wstring::npos,
          "wakeup frames resolve in the waking process even at the same virtual address");
    Check(CountRows(json.str(), L"\"module\":\"waiting-old.dll\"") == 2,
          "both blocked and ready stacks resolve at capture time rather than ready time");
    options.json = false; options.csv = true;
    std::wostringstream csv;
    wperf::WriteOffCpuReport(trace, symbols, options, csv);
    Check(csv.str().find(L"wakeup_pid,wakeup_tid") != std::wstring::npos &&
              csv.str().find(L"first-waker.dll") != std::wstring::npos &&
              csv.str().find(L"second-waker.dll") != std::wstring::npos,
          "CSV exposes the separate wakeup identities and symbolized stacks");
}

void TestRendererDrainDeadlineAndErrors() {
    struct Latch {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false, release = false, returned = false;
    };
    auto latch = std::make_shared<Latch>();
    wperf::AsyncTopRenderer blocked([latch](wperf::TraceData&, uint64_t, std::wstring*) {
        std::unique_lock lock(latch->mutex);
        latch->entered = true;
        latch->changed.notify_all();
        latch->changed.wait(lock, [&] { return latch->release; });
        latch->returned = true;
        latch->changed.notify_all();
        return true;
    });
    blocked.Push({}, 100);
    {
        std::unique_lock lock(latch->mutex);
        Check(latch->changed.wait_for(lock, std::chrono::seconds(2), [&] { return latch->entered; }),
              "the production render queue starts the blocked sink");
    }
    blocked.Push({}, 100);
    blocked.Push({}, 100);
    const auto started = GetTickCount64();
    Check(!blocked.Close(true, 20) && GetTickCount64() - started < 1000,
          "renderer drain timeout returns failure without blocking capture cleanup");
    std::wstring error;
    Check(blocked.Failed(&error) && error.find(L"incomplete") != std::wstring::npos &&
              blocked.DroppedWindows() == 3,
          "drain timeout reports the active and queued unfinished windows");
    {
        std::unique_lock lock(latch->mutex);
        latch->release = true;
        latch->changed.notify_all();
        Check(latch->changed.wait_for(lock, std::chrono::seconds(2), [&] { return latch->returned; }),
              "timed-out renderer retains safe ownership until the sink returns");
    }
    Check(blocked.Failed(nullptr), "late render completion cannot turn a drain timeout into success");

    std::atomic_uint rendered{0};
    wperf::AsyncTopRenderer normal([&rendered](wperf::TraceData&, uint64_t, std::wstring*) {
        ++rendered;
        return true;
    });
    normal.Push({}, 100);
    Check(normal.Close(true, 2000) && rendered == 1 && !normal.Failed(nullptr),
          "normal renderer drain waits for output completion and returns success");

    wperf::AsyncTopRenderer failing([](wperf::TraceData&, uint64_t, std::wstring*) -> bool {
        throw std::runtime_error("render failure");
    });
    failing.Push({}, 100);
    Check(!failing.Close(true, 2000) && failing.Failed(&error) &&
              error.find(L"render failure") != std::wstring::npos,
          "render exceptions propagate as errors instead of terminating the controller");
}

void TestOffCpuReportIdentityBoundariesAndFilters() {
    const auto count_text = [](const std::wstring& text, const std::wstring& needle) {
        size_t count = 0;
        size_t offset = 0;
        while ((offset = text.find(needle, offset)) != std::wstring::npos) {
            ++count;
            offset += needle.size();
        }
        return count;
    };

    wperf::TraceData trace;
    trace.start_ns = 1000;
    trace.end_ns = 8000;
    trace.processes.push_back({42, 1, L"first.exe", L"", 1000, 4000});
    trace.processes.push_back({42, 1, L"second.exe", L"", 5000, 9000});
    trace.processes.push_back({43, 42, L"child.exe", L"", 5500, 9000});
    trace.threads.push_back({42, 7, 1000, 4000});
    trace.threads.push_back({42, 7, 5000, 9000});
    trace.threads.push_back({43, 8, 5500, 9000});

    wperf::OffCpuInterval first;
    first.start_ns = 1000; first.end_ns = 3000; first.duration_ns = 2000;
    first.pid = 42; first.tid = 7; first.process_start_ns = 1000;
    first.thread_start_ns = 1000; first.kind = wperf::OffCpuKind::Blocked;
    first.stack = {0x1000}; first.stack_quality = wperf::StackQuality::Exact;
    first.stack_missing = false;
    trace.off_cpu_intervals.push_back(first);

    wperf::OffCpuInterval second = first;
    second.start_ns = 5000; second.end_ns = 6000; second.duration_ns = 1000;
    second.process_start_ns = 5000; second.thread_start_ns = 5000;
    trace.off_cpu_intervals.push_back(second);

    wperf::OffCpuInterval inferred = first;
    inferred.start_ns = 6000; inferred.end_ns = 7000; inferred.duration_ns = 1000;
    inferred.process_start_ns = 5000; inferred.thread_start_ns = 5000;
    inferred.boundary_inferred = true;
    trace.off_cpu_intervals.push_back(inferred);

    wperf::TraceData module_trace;
    module_trace.start_ns = 1;
    module_trace.processes.push_back({42, 1, L"module.exe", L"", 1, 0});
    module_trace.threads.push_back({42, 7, 1, 0});
    wperf::ImageRecord first_image;
    first_image.pid = 42; first_image.base = 0x1000; first_image.size = 0x1000;
    first_image.timestamp = 1; first_image.checksum = 1;
    first_image.load_ns = 1; first_image.path = L"C:\\modules\\first.dll";
    wperf::ImageRecord second_image = first_image;
    second_image.timestamp = 2; second_image.checksum = 2;
    second_image.load_ns = 30; second_image.path = L"C:\\modules\\second.dll";
    module_trace.images = {first_image, second_image};
    wperf::OffCpuInterval module_first;
    module_first.start_ns = 10; module_first.end_ns = 20; module_first.duration_ns = 10;
    module_first.pid = 42; module_first.tid = 7; module_first.process_start_ns = 1;
    module_first.thread_start_ns = 1; module_first.stack = {0x1010};
    module_first.stack_quality = wperf::StackQuality::Exact; module_first.stack_missing = false;
    wperf::OffCpuInterval module_second = module_first;
    module_second.start_ns = 30; module_second.end_ns = 40; module_second.duration_ns = 10;
    module_trace.off_cpu_intervals = {module_first, module_second};

    wperf::Symbolizer symbolizer;
    wperf::CpuReportOptions options;
    options.json = true;
    std::wostringstream complete;
    wperf::WriteOffCpuReport(trace, symbolizer, options, complete);
    Check(count_text(complete.str(), L"\"kind\":") == 2,
          "off-CPU default excludes inferred-boundary intervals from exact totals");
    Check(complete.str().find(L"\"process_start_ns\":1000") != std::wstring::npos &&
              complete.str().find(L"\"process_start_ns\":5000") != std::wstring::npos,
          "off-CPU aggregation keeps reused PID process instances separate");
    std::wostringstream module_rows;
    wperf::WriteOffCpuReport(module_trace, symbolizer, options, module_rows);
    Check(count_text(module_rows.str(), L"\"kind\":") == 2,
          "off-CPU aggregation keys frames by module identity and RVA");


    options.off_cpu_include_inferred = true;
    std::wostringstream with_inferred;
    wperf::WriteOffCpuReport(trace, symbolizer, options, with_inferred);
    Check(with_inferred.str().find(L"\"boundary\":\"truncated\"") != std::wstring::npos &&
              with_inferred.str().find(L"\"inferred_duration_ns\":1000") != std::wstring::npos,
          "off-CPU JSON preserves inferred boundary status and duration");

    options.csv = true;
    options.json = false;
    std::wostringstream csv_inferred;
    wperf::WriteOffCpuReport(trace, symbolizer, options, csv_inferred);
    Check(csv_inferred.str().find(L"inferred_duration_ns") != std::wstring::npos &&
              csv_inferred.str().find(L"truncated") != std::wstring::npos,
          "off-CPU CSV preserves inferred boundary status and duration");

    options.csv = false;
    options.folded = true;
    std::wostringstream folded_inferred;
    wperf::WriteOffCpuReport(trace, symbolizer, options, folded_inferred);
    Check(folded_inferred.str().find(L"[truncated-boundary]") != std::wstring::npos,
          "off-CPU folded output marks truncated boundaries");

    options = {};
    options.json = true;
    options.start_offset_ns = 500;
    options.end_offset_ns = 1000;
    trace.off_cpu_intervals = {first};
    std::wostringstream cropped;
    wperf::WriteOffCpuReport(trace, symbolizer, options, cropped);
    Check(cropped.str().find(L"\"duration_ns\":500") != std::wstring::npos,
          "off-CPU time selectors use capture-relative time and crop crossing intervals");

    wperf::OffCpuInterval child = first;
    child.start_ns = 6000; child.end_ns = 7000; child.duration_ns = 1000;
    child.pid = 43; child.tid = 8; child.process_start_ns = 5500;
    child.thread_start_ns = 5500; child.stack = {0x1000};
    trace.off_cpu_intervals = {child};
    options = {};
    options.json = true;
    options.pid = 1;
    options.include_children = true;
    std::wostringstream descendants;
    wperf::WriteOffCpuReport(trace, symbolizer, options, descendants);
    Check(descendants.str().find(L"\"pid\":43") != std::wstring::npos,
          "off-CPU --include-children selects descendant process intervals");

    child.stack = {0xfffff80000001000ull};
    trace.off_cpu_intervals = {child};
    options.include_children = false;
    options.pid.reset();
    options.scope = wperf::CpuReportOptions::Scope::User;
    std::wostringstream user_only;
    wperf::WriteOffCpuReport(trace, symbolizer, options, user_only);
    Check(count_text(user_only.str(), L"\"kind\":") == 0,
          "off-CPU --scope user rejects kernel leaf frames");
    options.scope = wperf::CpuReportOptions::Scope::Kernel;
    std::wostringstream kernel_only;
    wperf::WriteOffCpuReport(trace, symbolizer, options, kernel_only);
    Check(count_text(kernel_only.str(), L"\"kind\":") == 1,
          "off-CPU --scope kernel retains kernel leaf frames");
}

void TestSelectorsAndObservedIntervals() {
    wperf::TraceData trace; trace.start_ns=1000;
    trace.processes.push_back({100,0,L"parent.exe",L"",0,0});
    trace.processes.push_back({200,100,L"child.exe",L"",10,0});
    wperf::SampleRecord sample; sample.pid=200; sample.cpu=3; sample.timestamp_ns=2000; sample.instruction_pointer=0x1000;
    wperf::CpuReportOptions options; options.pid=100;
    Check(!wperf::MatchesSampleSelection(trace,sample,options),"PID selector does not silently include children");
    options.include_children=true; options.cpu=3; options.start_offset_ns=500; options.end_offset_ns=1500;
    Check(wperf::MatchesSampleSelection(trace,sample,options),"descendant CPU and relative-time selection");
    options.scope=wperf::CpuReportOptions::Scope::Kernel;
    Check(!wperf::MatchesSampleSelection(trace,sample,options),"kernel selector excludes user IP");
    sample.instruction_pointer=0xfffff80000001000ull;
    Check(wperf::MatchesSampleSelection(trace,sample,options),"kernel selector retains kernel IP");
    wperf::TraceEvent enter; enter.kind=wperf::TraceEventKind::ContextSwitch; enter.timestamp_ns=100;
    enter.tid=10; enter.pid=100; trace.events.push_back(enter);
    auto leave=enter; leave.timestamp_ns=300; leave.tid=20; leave.pid=200;
    leave.related_tid=10; leave.related_pid=100; leave.previous_state=5; trace.events.push_back(leave);
    wperf::TraceEvent ready; ready.kind=wperf::TraceEventKind::ReadyThread; ready.related_tid=10; ready.related_pid=100;
    ready.timestamp_ns=800; trace.events.push_back(ready);
    const auto rows=wperf::AnalyzeScheduler(trace);
    Check(rows.at(100).running_ns==200&&rows.at(100).blocked_ns==500,"scheduler times use matched observed endpoints");
}
}

int main() {
    TestProfileDecoder();
    TestLiveWindowCodec();
    TestLiveWindowLongPayloadBound();
    TestPipeSendDeadline();
    TestLiveIdentityAndCorrelation();
    TestBoundedCorrelation();
    TestAddressSpacesAndNames();
    TestSymbolSessionIsolation();
    TestLeafAndMachineOutput();
    TestReadyHandshakeAndDisconnect();
    TestPeriodsIdentityArchiveAndInline();
    TestModernStackKeyEpochs();
    TestRawPayloadsAndCounterDomains();
    TestStreamingAndStopDeadline();
    TestRawSchedulerStackAssociation();
    TestOffCpuIntervals();
    TestOffCpuTruncatedInterval();
    TestOffCpuConfirmedBlockAndReusedReady();
    TestOffCpuWakeupSourcesAndStackTimes();
    TestRendererDrainDeadlineAndErrors();
    TestOffCpuReportIdentityBoundariesAndFilters();
    TestSelectorsAndObservedIntervals();
    if (failures == 0) std::cout << "regression tests: PASS\n";
    return failures == 0 ? 0 : 1;
}
