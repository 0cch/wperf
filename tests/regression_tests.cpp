#include "common.h"
#include "commands/live_control.h"
#include "cpu_report.h"
#include "disassembler.h"
#include "etw_decode.h"
#include "live_top.h"
#include "output.h"
#include "symbolizer.h"

#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>
#include <psapi.h>

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
}

int main() {
    TestProfileDecoder();
    TestLiveIdentityAndCorrelation();
    TestBoundedCorrelation();
    TestAddressSpacesAndNames();
    TestSymbolSessionIsolation();
    TestLeafAndMachineOutput();
    TestReadyHandshakeAndDisconnect();
    if (failures == 0) std::cout << "regression tests: PASS\n";
    return failures == 0 ? 0 : 1;
}
