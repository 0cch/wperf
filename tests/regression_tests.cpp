#include "common.h"
#include "commands/live_control.h"
#include "cpu_report.h"
#include "disassembler.h"
#include "etw_decode.h"
#include "live_top.h"
#include "output.h"
#include "symbolizer.h"
#include "archive.h"
#include "analysis.h"
#include "etl_reader.h"

#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <sstream>
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

__forceinline int InlineInner(volatile int* value,int index) { return *value+index*3; }
__forceinline int InlineOuter(volatile int* value,int index) { return InlineInner(value,index)+*value*index; }
__declspec(noinline) int InlineAnchor(int input) {
    volatile int value=input; int result=0;
    for(int i=0;i<40;++i)result+=InlineOuter(&value,i);
    return result;
}
void TestPeriodsIdentityArchiveAndInline() {
    wperf::TraceData trace;
    trace.profile_interval_ns=2000000;
    wperf::SampleRecord unknown;
    Check(wperf::SampleWeightNs(trace,unknown)==0,"late metadata does not backfill unknown period");
    trace.processes.push_back({100,0,L"应用.exe",L"",10,200});
    trace.processes.push_back({100,0,L"应用.exe",L"",300,0});
    trace.images.push_back({100,0x1000,0x1000,1,0,0,0,L"C:/v1/plugin.dll"});
    trace.images.push_back({100,0x3000,0x1000,2,0,0,0,L"C:/v2/plugin.dll"});
    wperf::SampleRecord first; first.pid=100; first.timestamp_ns=100; first.instruction_pointer=0x1100;
    trace.samples.push_back(first); first.instruction_pointer=0x3100; trace.samples.push_back(first);
    first.timestamp_ns=350; first.instruction_pointer=0x1100; trace.samples.push_back(first);
    wperf::Symbolizer empty; wperf::CpuReportOptions options; options.json=true;
    std::wostringstream output; wperf::WriteCpuReport(trace,empty,options,output);
    size_t rows=0,position=0; while((position=output.str().find(L"\"samples\":1",position))!=std::wstring::npos){++rows;position+=10;}
    Check(rows==3,"DLL versions and PID lifetimes stay separate");
    Check(output.str().find(L"应用.exe")!=std::wstring::npos,"Unicode survives JSON report");

    MODULEINFO information{}; auto module=GetModuleHandleW(nullptr);
    GetModuleInformation(GetCurrentProcess(),module,&information,sizeof(information));
    wperf::ImageRecord image; image.pid=100; image.base=reinterpret_cast<uint64_t>(module);
    image.size=information.SizeOfImage; image.path=wperf::GetExecutablePath();
    const auto identity=wperf::ReadModuleIdentity(image.path);
    image.timestamp=identity.timestamp; image.checksum=identity.checksum;
    wperf::TraceData symbols_trace; symbols_trace.images.push_back(image);
    const auto directory=std::filesystem::temp_directory_path()/(L"wperf-symbols-"+wperf::NewId());
    const auto cache=directory/L"cache";
    std::wstring error; uint64_t inline_address=0;
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
    Check(wperf::LoadArchiveModules(directory,symbols_trace,&error)&&
          symbols_trace.images[0].resolved_path==archived.wstring(),"archive maps missing original paths after hash/PE validation");
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
    TestSelectorsAndObservedIntervals();
    if (failures == 0) std::cout << "regression tests: PASS\n";
    return failures == 0 ? 0 : 1;
}
