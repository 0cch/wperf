#include "common.h"
#include "commands/live_control.h"
#include "cpu_report.h"
#include "dbghelp_loader.h"
#include "disassembler.h"
#include "elevation.h"
#include "etl_reader.h"
#include "etw.h"
#include "flamegraph.h"
#include "ipc.h"
#include "lbr.h"
#include "state.h"
#include "symbolizer.h"
#include "trace_events.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>

#include <psapi.h>
#include <sstream>
#include <string>
#include <vector>

namespace {
int g_failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L"\n";
        ++g_failures;
    }
}

void TestQuoteArgument() {
    Check(wperf::QuoteCommandLineArgument(L"plain") == L"plain", L"plain quoting");
    Check(wperf::QuoteCommandLineArgument(L"hello world") == L"\"hello world\"",
          L"space quoting");
    Check(wperf::QuoteCommandLineArgument(L"a b\\") == L"\"a b\\\\\"",
          L"trailing backslash quoting");
}

void TestStateRoundTrip() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (L"wperf-tests-" + wperf::NewId());
    std::wstring error;
    Check(wperf::EnsureDirectory(directory, &error), L"create state directory");

    wperf::RunState state;
    state.correlation_id = wperf::NewId();
    state.phase = L"CAPTURING";
    state.session_name = L"wperf-test-session";
    state.trace_path = L"C:\\path with space\\trace.etl";
    state.worker_pid = 20;
    state.restore_source=19; state.restore_interval=1000000; state.configured_interval=2000000; state.restore_clear_pmc=1;

    wperf::StateStore store(directory / (state.correlation_id + L".json"));
    Check(store.Write(state, &error), L"write state file");
    const auto states = wperf::StateStore::ReadAll(directory);
    Check(states.size() == 1, L"read one state file");
    if (!states.empty()) {
        Check(states[0].phase == L"CAPTURING", L"state phase round trip");
        Check(states[0].worker_pid == 20, L"state pid round trip");
        Check(states[0].restore_source==19&&states[0].restore_interval==1000000&&states[0].configured_interval==2000000&&states[0].restore_clear_pmc==1,
              L"recovery checkpoint round trip");
        Check(states[0].trace_path == state.trace_path, L"state path round trip");
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void TestEtwQuery() {
    std::wstring error;
    const auto sources = wperf::QueryProfileSources(&error);
    Check(error.empty(), L"profile source query");
    Check(!sources.empty(), L"profile source list");
}

void TestCpuReportSynthetic() {
    wperf::TraceData trace;
    trace.quality.samples_total = 1;
    trace.pointer_size = 8;
    wperf::ProcessRecord process;
    process.pid = 42;
    process.image_name = L"app.exe";
    trace.processes.push_back(process);
    wperf::ImageRecord image;
    image.pid = 42;
    image.base = 0x1000;
    image.size = 0x1000;
    image.path = L"app.exe";
    trace.images.push_back(image);
    wperf::SampleRecord sample;
    sample.pid = 42;
    sample.tid = 7;
    sample.cpu = 0;
    sample.instruction_pointer = 0x1010;
    sample.stack = {0x1010, 0x1000};
    trace.samples.push_back(sample);

    wperf::Symbolizer symbolizer;
    wperf::CpuReportOptions options;
    std::wostringstream output;
    wperf::WriteCpuReport(trace, symbolizer, options, output);
    Check(output.str().find(L"app.exe") != std::wstring::npos,
          L"CPU report resolves synthetic image");

    wperf::CpuReportOptions folded_options;
    folded_options.folded = true;
    std::wostringstream folded_output;
    wperf::CpuReportWeightInfo weight_info;
    wperf::WriteCpuReport(trace, symbolizer, folded_options, folded_output,
                          nullptr, &weight_info);
    Check(weight_info.fallback && weight_info.unit == L"samples",
          L"unknown-period folded weight reports sample fallback");
    Check(folded_output.str().rfind(L"app.exe[42@0];", 0) == 0,
          L"folded report starts with process name");
    Check(!folded_output.str().empty() &&
              folded_output.str().back() == L'\n',
          L"folded report uses one line per stack");

    trace.samples.front().period = 1000000;
    std::wostringstream period_output;
    wperf::CpuReportWeightInfo period_info;
    wperf::WriteCpuReport(trace, symbolizer, folded_options, period_output,
                          nullptr, &period_info);
    Check(!period_info.fallback && period_info.use_period &&
              period_info.unit == L"nanoseconds",
          L"CPU period weight reports nanosecond units");
    trace.samples.front().kind = wperf::SampleKind::Pmc;
    trace.samples.front().profile_source = 19;
    std::wostringstream pmc_output;
    wperf::CpuReportWeightInfo pmc_info;
    wperf::WriteCpuReport(trace, symbolizer, folded_options, pmc_output,
                          nullptr, &pmc_info);
    Check(!pmc_info.fallback && pmc_info.use_period && pmc_info.unit == L"events",
          L"PMC period weight reports event units");
}
void TestInvalidEtwFile() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / (L"wperf-bad-" + wperf::NewId() + L".etl");
    {
        std::ofstream stream(path, std::ios::binary);
        stream << "not an etl trace";
    }
    wperf::TraceData trace;
    std::wstring error;
    const bool result = wperf::ReadEtwFile(path, trace, &error);
    Check(!result, L"invalid ETL is rejected");
    Check(!error.empty(), L"invalid ETL returns an error");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
bool HasFixtureProcess(const wperf::TraceData& trace) {
    return std::any_of(trace.processes.begin(), trace.processes.end(), [](const wperf::ProcessRecord& process) {
        return std::filesystem::path(process.image_name).filename().wstring() ==
               L"wperf_etw_fixture_workload.exe";
    });
}
void TestTruncatedEtwFiles() {
    const std::filesystem::path fixture =
        std::filesystem::path(WPERF_TEST_DATA_DIR) / L"cpu-stack.etl";
    if (!std::filesystem::exists(fixture)) {
        return;
    }
    std::ifstream source(fixture, std::ios::binary);
    Check(source.good(), L"open fixture for truncation tests");
    if (!source.good()) {
        return;
    }
    const std::vector<size_t> sizes{0, 64};
    for (size_t size : sizes) {
        source.clear();
        source.seekg(0, std::ios::beg);
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            (L"wperf-truncated-" + wperf::NewId() + L".etl");
        {
            std::ofstream target(path, std::ios::binary);
            std::vector<char> buffer(size);
            if (size != 0) {
                source.read(buffer.data(), static_cast<std::streamsize>(size));
            }
            if (!buffer.empty()) {
                target.write(buffer.data(),
                             static_cast<std::streamsize>(buffer.size()));
            }
        }
        wperf::TraceData trace;
        std::wstring error;
        const bool result = wperf::ReadEtwFile(path, trace, &error);
        Check(!result, L"truncated ETL is rejected");
        Check(!error.empty(), L"truncated ETL returns an error");
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
}

void TestLbrDecode() {
    std::vector<BYTE> payload(24 + 2 * 24, 0);
    const uint64_t qpc = 0x12345678;
    const uint32_t pid = 42;
    const uint32_t tid = 77;
    const uint32_t filters = 0x20;
    std::memcpy(payload.data(), &qpc, sizeof(qpc));
    std::memcpy(payload.data() + 8, &pid, sizeof(pid));
    std::memcpy(payload.data() + 12, &tid, sizeof(tid));
    std::memcpy(payload.data() + 16, &filters, sizeof(filters));
    const uint64_t branches[2][3] = {
        {0x1000, 0x2000, 0},
        {0x2000, 0x3000, 0},
    };
    std::memcpy(payload.data() + 24, branches, sizeof(branches));

    wperf::LbrSample sample;
    Check(wperf::DecodeLbrPayload(payload.data(), payload.size(), 8, sample),
          L"decode LBR payload");
    Check(sample.pid == pid && sample.tid == tid, L"decode LBR identity");
    Check(sample.filters == filters, L"decode LBR filters");
    Check(sample.branches.size() == 2, L"decode LBR branch count");
    Check(sample.branches[0].from_address == 0x1000 &&
              sample.branches[0].to_address == 0x2000,
          L"decode first LBR branch");
    Check(!wperf::DecodeLbrPayload(payload.data(), 16, 8, sample),
          L"reject truncated LBR payload");
}

void TestPmcQueries() {
    std::wstring error;
    const uint32_t maximum = wperf::QueryMaxPmcCounters(&error);
    Check(maximum > 0, L"query maximum PMC counters");
    error.clear();
    const auto owners = wperf::QueryPmcCounterOwners(&error);
    (void)owners;
    for (const auto& owner : owners) {
        Check(owner.processor < GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
              L"PMC owner identifies an existing processor");
        Check(owner.owner_type != EtwPmcOwnerFree, L"PMC owner list excludes free counters");
    }
    error.clear();
    const auto sessions = wperf::QueryPmcSessions(&error);
    for (const auto& session : sessions) {
        Check(session.sources.size() <= maximum, L"PMC session sources fit the counter capacity");
    }
}

void TestTraceEventCorrelation() {

    wperf::TraceData trace;
    trace.threads.push_back(wperf::ThreadRecord{100, 10, 0, 0});
    trace.threads.push_back(wperf::ThreadRecord{200, 20, 0, 0});

    wperf::TraceEvent ready;
    ready.timestamp_ns = 1000;
    ready.kind = wperf::TraceEventKind::ReadyThread;
    ready.related_tid = 10;
    trace.events.push_back(ready);

    wperf::TraceEvent context_switch;
    context_switch.timestamp_ns = 1500;
    context_switch.kind = wperf::TraceEventKind::ContextSwitch;
    context_switch.tid = 10;
    context_switch.related_tid = 20;
    context_switch.value2 = 10;
    context_switch.wait_reason = 6;
    context_switch.previous_state = 5;
    trace.events.push_back(context_switch);

    wperf::TraceEvent syscall_enter;
    syscall_enter.timestamp_ns = 2000;
    syscall_enter.kind = wperf::TraceEventKind::SyscallEnter;
    syscall_enter.tid = 10;
    trace.events.push_back(syscall_enter);

    wperf::TraceEvent syscall_exit;
    syscall_exit.timestamp_ns = 2500;
    syscall_exit.kind = wperf::TraceEventKind::SyscallExit;
    syscall_exit.tid = 10;
    syscall_exit.status = -1073741823;
    syscall_exit.has_status = true;
    trace.events.push_back(syscall_exit);

    wperf::TraceEvent io_start;
    io_start.timestamp_ns = 3000;
    io_start.kind = wperf::TraceEventKind::FileIo;
    io_start.event_id = 67;
    io_start.correlation_id = 0x1234;
    io_start.size = 4096;
    trace.events.push_back(io_start);

    wperf::TraceEvent io_end;
    io_end.timestamp_ns = 3500;
    io_end.kind = wperf::TraceEventKind::FileIo;
    io_end.event_id = 76;
    io_end.correlation_id = 0x1234;
    io_end.is_completion = true;
    trace.events.push_back(io_end);

    wperf::EnrichTraceEvents(trace);
    Check(trace.events[0].related_pid == 100,
          L"ready thread resolves awakened process");
    Check(trace.events[1].pid == 100 && trace.events[1].related_pid == 200,
          L"context switch resolves both processes");
    Check(trace.events[1].has_duration && trace.events[1].duration_ns == 500,
          L"context switch records wakeup latency");
    Check(trace.events[3].has_duration && trace.events[3].duration_ns == 500,
          L"syscall exit records duration");
    Check(trace.events[5].has_duration && trace.events[5].duration_ns == 500,
          L"file IO completion records duration");
    Check(trace.events[5].size == 4096,
          L"file IO completion inherits request size");
    Check(std::wstring(wperf::TraceWaitReasonName(6)) == L"UserRequest",
          L"wait reason name mapping");
    Check(std::wstring(wperf::TraceThreadStateName(5)) == L"Wait",
          L"thread state name mapping");
}

void TestDisassembler() {
    if (!wperf::DisassemblyAvailable()) {
        return;
    }
    const uint8_t code[] = {0x48, 0x8B, 0x05, 0x78, 0x56, 0x34, 0x12, 0xC3};
    std::vector<wperf::DisassembledInstruction> instructions;
    std::wstring error;
    Check(wperf::DisassembleBuffer(code, sizeof(code), 0x1000, 8, 4,
                                   instructions, &error),
          L"disassemble fixed x64 bytes");
    if (instructions.size() < 2) {
        Check(false, L"disassemble expected instruction count");
        return;
    }
    Check(instructions[0].length == 7 && instructions[0].text.find("mov") != std::string::npos,
          L"disassemble first instruction");
    Check(instructions[1].text.find("ret") != std::string::npos,
          L"disassemble ret instruction");
}

void TestThreadAndProcessIntervals() {
    wperf::TraceData trace;
    trace.threads.push_back(wperf::ThreadRecord{100, 7, 100, 200});
    trace.threads.push_back(wperf::ThreadRecord{200, 7, 300, 0});
    const auto* first_thread = wperf::FindThread(trace, 7, 150);
    Check(first_thread != nullptr && first_thread->pid == 100,
          L"find first thread lifetime");
    Check(wperf::FindThread(trace, 7, 250) == nullptr,
          L"thread id is not attributed between lifetimes");
    const auto* second_thread = wperf::FindThread(trace, 7, 350);
    Check(second_thread != nullptr && second_thread->pid == 200,
          L"find reused thread id lifetime");

    trace.processes.push_back(wperf::ProcessRecord{
        42, 0, L"first.exe", L"", 100, 200});
    trace.processes.push_back(wperf::ProcessRecord{
        42, 0, L"second.exe", L"", 300, 0});
    const auto* first_process = wperf::FindProcess(trace, 42, 150);
    Check(first_process != nullptr && first_process->image_name == L"first.exe",
          L"find first process lifetime");
    const auto* second_process = wperf::FindProcess(trace, 42, 350);
    Check(second_process != nullptr && second_process->image_name == L"second.exe",
          L"find reused process id lifetime");
    Check(wperf::ProcessDisplayName(trace, UINT32_MAX) == L"[unattributed]",
          L"unknown process id has a stable display name");
    Check(wperf::ProcessDisplayName(trace, 42) == L"second.exe",
          L"process display name resolves the current lifetime");
}

void TestDbgHelpDiscovery() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (L"wperf-dbghelp-" + wperf::NewId());
    std::wstring error;
    Check(wperf::EnsureDirectory(directory, &error), L"create DbgHelp test directory");
    const auto dll = directory / L"dbghelp.dll";
    {
        std::ofstream stream(dll, std::ios::binary);
        stream << "test";
    }
    const DWORD required = GetEnvironmentVariableW(L"WPERF_DBGHELP_PATH", nullptr, 0);
    std::wstring original(required, L'\0');
    if (required > 0) {
        const DWORD written = GetEnvironmentVariableW(
            L"WPERF_DBGHELP_PATH", original.data(), required);
        original.resize(written);
    }
    _wputenv_s(L"WPERF_DBGHELP_PATH", directory.c_str());
    Check(wperf::FindPreferredDbgHelpPath() == dll,
          L"WPERF_DBGHELP_PATH directory selects dbghelp.dll");
    _wputenv_s(L"WPERF_DBGHELP_PATH", dll.c_str());
    Check(wperf::FindPreferredDbgHelpPath() == dll,
          L"WPERF_DBGHELP_PATH file selects dbghelp.dll");
    _wputenv_s(L"WPERF_DBGHELP_PATH", original.c_str());
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void TestModulePathNormalization() {
    const std::wstring executable = wperf::GetExecutablePath();
    const std::wstring drive = std::filesystem::path(executable).root_name().wstring();
    wchar_t device[1024]{};
    Check(!drive.empty() &&
              QueryDosDeviceW(drive.c_str(), device, static_cast<DWORD>(std::size(device))) != 0,
          L"query DOS device for module path normalization");
    if (!drive.empty() && device[0] != L'\0') {
        std::wstring device_path = std::wstring(device) + LR"(\windows\system32\ntdll.dll)";
        std::transform(device_path.begin(), device_path.end(), device_path.begin(),
                       [](wchar_t character) { return std::towlower(character); });
        const std::wstring normalized = wperf::NormalizeModulePath(device_path);
        const std::wstring expected = drive + LR"(\windows\system32\ntdll.dll)";
        Check(_wcsicmp(normalized.c_str(), expected.c_str()) == 0,
              L"lowercase device path is normalized to a drive path");
    }

    const DWORD required = GetEnvironmentVariableW(L"SystemRoot", nullptr, 0);
    Check(required > 0, L"read SystemRoot for module path normalization");
    if (required > 0) {
        std::wstring system_root(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(
            L"SystemRoot", system_root.data(), required);
        system_root.resize(written);
        const std::wstring normalized =
            wperf::NormalizeModulePath(LR"(\systemroot\System32\ntdll.dll)");
        const std::wstring expected = system_root + LR"(\System32\ntdll.dll)";
        Check(_wcsicmp(normalized.c_str(), expected.c_str()) == 0,
              L"case-insensitive SystemRoot path is normalized");
    }
}

void TestSymbolizerPdbIdentity() {
    HMODULE module = GetModuleHandleW(nullptr);
    MODULEINFO module_info{};
    Check(module != nullptr && GetModuleInformation(GetCurrentProcess(), module,
                                                     &module_info,
                                                     sizeof(module_info)),
          L"query current module identity");
    if (module == nullptr || module_info.SizeOfImage == 0) {
        return;
    }

    wperf::ImageRecord image;
    image.pid = GetCurrentProcessId();
    image.base = reinterpret_cast<uint64_t>(module);
    image.size = module_info.SizeOfImage;
    image.path = wperf::GetExecutablePath();

    wperf::TraceData trace;
    trace.images.push_back(image);
    const std::filesystem::path cache =
        std::filesystem::temp_directory_path() / (L"wperf-symbolizer-test-" + wperf::NewId());
    wperf::Symbolizer symbolizer;
    std::wstring error;
    Check(symbolizer.Initialize(L"", &error, cache), L"initialize symbolizer");
    symbolizer.AddModule(trace.images.front());
    const uint64_t address =
        reinterpret_cast<uint64_t>(&TestSymbolizerPdbIdentity);
    const auto resolved = symbolizer.Resolve(trace, image.pid, address, 1);
    Check(resolved.module.find(L"wperf_unit_tests.exe") != std::wstring::npos,
          L"symbolizer resolves current module");
    Check(resolved.resolved, L"symbolizer loads the matching PDB");
    const auto hits_before = symbolizer.Stats().cache_hits;
    const auto cached = symbolizer.Resolve(trace, image.pid, address, 1);
    Check(cached.symbol == resolved.symbol, L"symbol cache returns stable result");
    const auto stats = symbolizer.Stats();
    Check(stats.cache_hits == hits_before+1, L"symbol cache records a hit");
    Check(stats.memory_hits >= 1 && stats.dbghelp_resolves >= 1,
          L"symbol cache separates memory and DbgHelp work");
    const auto diagnostics = symbolizer.Diagnostics();
    Check(diagnostics.stats.modules_total >= 1 && diagnostics.stats.modules_loaded >= 1,
          L"symbol diagnostics counts loaded modules");
    Check(!diagnostics.dbghelp_path.empty() &&
              !diagnostics.symcache_path.empty(),
          L"symbol diagnostics exposes DbgHelp and symcache paths");
    Check(!diagnostics.modules.empty() && diagnostics.modules.front().status == L"loaded",
          L"symbol diagnostics exposes per-module status");

    wperf::TraceData stale_trace;
    wperf::ImageRecord stale = image;
    stale.timestamp = 1;
    stale_trace.images.push_back(stale);
    wperf::Symbolizer stale_symbolizer;
    Check(stale_symbolizer.Initialize(L"", &error, cache),
          L"initialize stale symbolizer");
    stale_symbolizer.AddModule(stale_trace.images.front());
    const auto stale_resolved =
        stale_symbolizer.Resolve(stale_trace, stale.pid, address, 1);
    Check(!stale_resolved.resolved, L"stale PE identity is rejected");
    Check(stale_resolved.symbol.find(L"+0x") != std::wstring::npos,
          L"stale PE identity returns module offset");
    const auto stale_diagnostics = stale_symbolizer.Diagnostics();
    Check(stale_diagnostics.stats.modules_rejected >= 1 &&
              !stale_diagnostics.modules.empty() &&
              stale_diagnostics.modules.front().reason == L"pe-identity-mismatch",
          L"symbol diagnostics records module rejection reason");
    const auto unresolved_before = stale_diagnostics.stats.unresolved;
    wperf::TraceData unmapped_trace;
    const auto unmapped =
        stale_symbolizer.Resolve(unmapped_trace, 1, 0x1234, 1);
    Check(!unmapped.resolved && unmapped.symbol == L"[unknown]",
          L"unmapped address remains unresolved");
    const auto unresolved_after = stale_symbolizer.Diagnostics().stats.unresolved;
    Check(unresolved_after == unresolved_before + 1,
          L"unmapped addresses contribute to the global unresolved count");
    std::error_code ignored;
    std::filesystem::remove_all(cache, ignored);
}

void TestSymbolPathComposition() {
    const wchar_t* name = L"_NT_SYMBOL_PATH";
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    std::wstring previous;
    if (required != 0) {
        previous.resize(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(name, previous.data(), required);
        if (written == 0 || written >= required) previous.clear();
        else previous.resize(written);
    }
    const std::wstring environment =
        L"srv*C:\\test-symbol-cache*https://example.invalid/symbols";
    Check(SetEnvironmentVariableW(name, environment.c_str()) != FALSE,
          L"set test symbol environment");
    wperf::Symbolizer symbolizer;
    std::wstring error;
    const std::filesystem::path cache =
        std::filesystem::temp_directory_path() / (L"wperf-symbol-path-test-" + wperf::NewId());
    Check(symbolizer.Initialize(L"local-symbols", &error, cache),
          L"initialize symbolizer with combined symbol path");
    const auto diagnostics = symbolizer.Diagnostics();
    Check(diagnostics.explicit_symbol_path == L"local-symbols" &&
              diagnostics.environment_symbol_path == environment,
          L"symbol diagnostics preserve explicit and environment paths");
    Check(diagnostics.symbol_path == L"local-symbols;" + environment &&
              diagnostics.symbol_path_source == L"option+environment",
          L"explicit symbol path augments rather than replaces _NT_SYMBOL_PATH");
    symbolizer.Close();
    if (previous.empty()) {
        SetEnvironmentVariableW(name, nullptr);
    } else {
        SetEnvironmentVariableW(name, previous.c_str());
    }
    std::error_code ignored;
    std::filesystem::remove_all(cache, ignored);
}

void TestSymbolPersistentCacheDiagnostics() {
    HMODULE module = GetModuleHandleW(nullptr);
    MODULEINFO module_info{};
    if (module == nullptr || !GetModuleInformation(GetCurrentProcess(), module,
                                                   &module_info,
                                                   sizeof(module_info))) {
        Check(false, L"query module for persistent cache diagnostics");
        return;
    }
    wperf::ImageRecord image;
    image.pid = GetCurrentProcessId();
    image.base = reinterpret_cast<uint64_t>(module);
    image.size = module_info.SizeOfImage;
    image.path = wperf::GetExecutablePath();
    wperf::TraceData trace;
    trace.images.push_back(image);
    const uint64_t address =
        reinterpret_cast<uint64_t>(&TestSymbolPersistentCacheDiagnostics);
    const std::filesystem::path cache =
        std::filesystem::temp_directory_path() / (L"wperf-symcache-test-" + wperf::NewId());

    {
        wperf::Symbolizer first;
        std::wstring error;
        Check(first.Initialize(L"", &error, cache), L"initialize first cache symbolizer");
        first.AddModule(image);
        const auto resolved = first.Resolve(trace, image.pid, address, 1);
        Check(resolved.resolved, L"resolve symbol before writing persistent cache");
        first.Close();
    }
    {
        wperf::Symbolizer second;
        std::wstring error;
        Check(second.Initialize(L"", &error, cache), L"initialize warm cache symbolizer");
        second.AddModule(image);
        const auto resolved = second.Resolve(trace, image.pid, address, 1);
        Check(resolved.resolved, L"resolve symbol from persistent cache");
        const auto diagnostics = second.Diagnostics();
        Check(diagnostics.stats.persistent_hits >= 1,
              L"persistent cache hit is exposed in diagnostics");
        Check(diagnostics.stats.cache_hits >= diagnostics.stats.persistent_hits,
              L"aggregate cache hits include persistent hits");
    }
    std::error_code ignored;
    std::filesystem::remove_all(cache, ignored);
}

void TestGoldenCpuStackReplay() {
    const std::filesystem::path data_dir(WPERF_TEST_DATA_DIR);
    const std::filesystem::path fixture = data_dir / L"cpu-stack.etl";
    if (!std::filesystem::exists(fixture)) {
        return;
    }

    wperf::TraceData trace;
    std::wstring error;
    Check(wperf::ReadEtwFile(fixture, trace, &error), L"replay runtime CPU ETL");
    Check(error.empty(), L"runtime CPU ETL has no replay error");
    Check(trace.quality.events_total > 0, L"runtime CPU ETL contains events");
    Check(trace.quality.samples_total > 0, L"runtime CPU ETL contains samples");
    Check(!trace.samples.empty(), L"runtime CPU ETL materializes samples");
    Check(trace.quality.stack_events_total > 0, L"runtime CPU ETL contains stack events");
    Check(trace.quality.stack_exact_total > 0, L"runtime CPU ETL contains exact stacks");
    Check(trace.quality.stack_exact_total >= trace.quality.stack_approximate_total,
          L"runtime stacks prefer exact matches");
    Check(trace.quality.stack_key_mismatch == 0, L"runtime stack keys are consistent");
    Check(trace.quality.samples_without_stack <= trace.quality.samples_total,
          L"runtime missing-stack count is bounded");
    Check(trace.quality.pmc_samples == 0 && !trace.quality.incomplete,
          L"runtime CPU capture is classified correctly");
    Check(HasFixtureProcess(trace), L"runtime CPU ETL contains fixture workload");
}
void TestLiveTraceFlags() {
    const ULONG base = EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD |
                       EVENT_TRACE_FLAG_IMAGE_LOAD;
    Check((wperf::TraceFlagsForLiveFilter(L"sched") &
           (EVENT_TRACE_FLAG_CSWITCH | EVENT_TRACE_FLAG_DISPATCHER)) ==
              (EVENT_TRACE_FLAG_CSWITCH | EVENT_TRACE_FLAG_DISPATCHER),
          L"live sched enables context switches");
    Check((wperf::TraceFlagsForLiveFilter(L"syscall") &
           EVENT_TRACE_FLAG_SYSTEMCALL) != 0,
          L"live syscall enables system calls");
    Check((wperf::TraceFlagsForLiveFilter(L"pagefault") &
           EVENT_TRACE_FLAG_MEMORY_PAGE_FAULTS) != 0,
          L"live pagefault enables page faults");
    Check((wperf::TraceFlagsForLiveFilter(L"io") &
           (EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_FILE_IO)) ==
              (EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_FILE_IO),
          L"live io enables disk and file IO");
    Check((wperf::TraceFlagsForLiveFilter(L"sched") & base) == base,
          L"live filters retain process/thread/image flags");
}

void TestLiveWorkerControlPlane() {

    const std::filesystem::path worker_executable =
        std::filesystem::path(wperf::GetExecutablePath()).parent_path() /
        L"wperf.exe";
    Check(std::filesystem::exists(worker_executable), L"live worker executable exists");

    wperf::LiveWorkerOptions options;
    options.mode = L"top";
    options.session_name = L"wperf-test-live-" + wperf::NewId();
    options.interval_ms = 100;
    options.duration_text = L"250";
    wperf::LiveWorkerClient worker;
    std::wstring error;
    Check(worker.StartWithExecutable(options, worker_executable.wstring(), false,
                                     &error),
          L"start live worker control plane");

    bool finished = false;
    int result = -1;
    const ULONGLONG deadline = GetTickCount64() + 20000;
    while (GetTickCount64() < deadline) {
        if (!worker.HasData()) {
            Sleep(20);
            continue;
        }
        std::wstring line;
        if (!worker.ReceiveLine(line, 2000, &error)) {
            break;
        }
        if (line.rfind(L"FINISHED ", 0) == 0) {
            result = _wtoi(line.c_str() + 9);
            finished = true;
            break;
        }
    }
    Check(finished, L"live worker sends FINISHED");
    Check(result == 0 || result == 4, L"live worker result is success or access denied");
    DWORD exit_code = 0;
    Check(worker.WaitForExit(10000, exit_code, &error), L"live worker exits");
    Check(exit_code == static_cast<DWORD>(result), L"live worker exit code matches");
    worker.Close();
}

void TestGoldenR3Replay() {
    const std::filesystem::path data_dir(WPERF_TEST_DATA_DIR);
    if (!std::filesystem::exists(data_dir / L"sched.etl") ||
        !std::filesystem::exists(data_dir / L"syscall.etl") ||
        !std::filesystem::exists(data_dir / L"io.etl")) {
        return;
    }

    wperf::TraceData sched;
    std::wstring error;
    const auto sched_path = data_dir / L"sched.etl";
    Check(wperf::ReadEtwFile(sched_path, sched, &error),
          L"replay runtime scheduler ETL");
    const uint64_t context_switches = static_cast<uint64_t>(std::count_if(
        sched.events.begin(), sched.events.end(), [](const wperf::TraceEvent& event) {
            return event.kind == wperf::TraceEventKind::ContextSwitch;
        }));
    const uint64_t ready_threads = static_cast<uint64_t>(std::count_if(
        sched.events.begin(), sched.events.end(), [](const wperf::TraceEvent& event) {
            return event.kind == wperf::TraceEventKind::ReadyThread;
        }));
    const uint64_t wakeup_latencies = static_cast<uint64_t>(std::count_if(
        sched.events.begin(), sched.events.end(), [](const wperf::TraceEvent& event) {
            return event.kind == wperf::TraceEventKind::ContextSwitch && event.has_duration;
        }));
    Check(sched.quality.events_total > 0, L"runtime scheduler ETL contains events");
    Check(context_switches > 0, L"runtime scheduler ETL contains context switches");
    Check(ready_threads > 0, L"runtime scheduler ETL contains ready-thread events");
    Check(wakeup_latencies > 0, L"runtime scheduler ETL contains wakeup latencies");
    Check(HasFixtureProcess(sched), L"runtime scheduler ETL contains fixture workload");

    wperf::TraceData syscall;
    const auto syscall_path = data_dir / L"syscall.etl";
    Check(wperf::ReadEtwFile(syscall_path, syscall, &error),
          L"replay runtime syscall ETL");
    const uint64_t enters = static_cast<uint64_t>(std::count_if(
        syscall.events.begin(), syscall.events.end(), [](const wperf::TraceEvent& event) {
            return event.kind == wperf::TraceEventKind::SyscallEnter;
        }));
    const uint64_t exits = static_cast<uint64_t>(std::count_if(
        syscall.events.begin(), syscall.events.end(), [](const wperf::TraceEvent& event) {
            return event.kind == wperf::TraceEventKind::SyscallExit;
        }));
    Check(syscall.quality.events_total > 0, L"runtime syscall ETL contains events");
    Check(enters > 0, L"runtime syscall ETL contains enter events");
    Check(exits > 0, L"runtime syscall ETL contains exit events");
    Check(HasFixtureProcess(syscall), L"runtime syscall ETL contains fixture workload");

    wperf::TraceData io;
    const auto io_path = data_dir / L"io.etl";
    Check(wperf::ReadEtwFile(io_path, io, &error), L"replay runtime IO ETL");
    uint64_t file_io = 0;
    uint64_t disk_io = 0;
    uint64_t file_completions = 0;
    uint64_t disk_completions = 0;
    uint64_t file_durations = 0;
    uint64_t disk_durations = 0;
    for (const wperf::TraceEvent& event : io.events) {
        if (event.kind == wperf::TraceEventKind::FileIo) {
            ++file_io;
            file_completions += event.is_completion ? 1 : 0;
            file_durations += event.has_duration ? 1 : 0;
        } else if (event.kind == wperf::TraceEventKind::DiskIo) {
            ++disk_io;
            disk_completions += event.is_completion ? 1 : 0;
            disk_durations += event.has_duration ? 1 : 0;
        }
    }
    Check(io.quality.events_total > 0, L"runtime IO ETL contains events");
    Check(file_io > 0, L"runtime IO ETL contains file IO");
    Check(disk_io > 0, L"runtime IO ETL contains disk IO");
    Check(file_completions > 0, L"runtime IO ETL contains file completions");
    Check(disk_completions > 0, L"runtime IO ETL contains disk completions");
    Check(file_durations > 0, L"runtime IO ETL contains file durations");
    Check(disk_durations > 0, L"runtime IO ETL contains disk durations");
    Check(io.quality.io_init_events > 0, L"runtime IO ETL contains init events");
    Check(io.quality.io_complete_events > 0, L"runtime IO ETL contains completion events");
    Check(HasFixtureProcess(io), L"runtime IO ETL contains fixture workload");
}
void TestWorkerIpcFailurePath() {

    const std::wstring correlation_id = wperf::NewId();
    const std::wstring nonce = wperf::NewId();
    const std::wstring session_name = L"wperf-test-" + correlation_id;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (L"wperf-ipc-" + correlation_id);
    std::wstring error;
    Check(wperf::EnsureDirectory(directory, &error), L"create IPC directory");

    wperf::PipeServer pipe;
    Check(pipe.Create(correlation_id, &error), L"create pipe");

    const std::filesystem::path executable =
        std::filesystem::path(wperf::GetExecutablePath()).parent_path() / L"wperf.exe";
    std::vector<std::wstring> arguments{
        L"--internal-elevated-worker",
        L"--pipe", wperf::MakePipePath(correlation_id),
        L"--nonce", nonce,
        L"--controller-pid", std::to_wstring(GetCurrentProcessId()),
        L"--session", session_name,
        L"--output", (directory / L"trace.etl").wstring(),
        L"--log", (directory / L"worker.jsonl").wstring(),
        L"--guard-log", (directory / L"guard.jsonl").wstring(),
        L"--state", (directory / L"state.json").wstring(),
        L"--correlation", correlation_id,
        L"--duration-ms", L"1000",
    };

    wperf::ProcessLaunchResult launch;
    Check(wperf::LaunchWorkerProcess(executable.wstring(), arguments, false, launch, &error),
          L"launch worker");
    if (launch.process == nullptr) {
        return;
    }
    Check(pipe.WaitForClient(10000, &error), L"worker connects");
    std::wstring line;
    Check(pipe.ReceiveLine(line, 10000, &error), L"receive hello");
    Check(line == L"HELLO " + nonce + L" " + std::to_wstring(launch.process_id),
          L"validate hello");
    Check(pipe.SendLine(L"START", &error), L"send start");

    bool finished = false;
    int result = -1;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
        if (!pipe.HasData()) {
            Sleep(20);
            continue;
        }
        if (!pipe.ReceiveLine(line, 2000, &error)) {
            break;
        }
        if (line.rfind(L"FINISHED ", 0) == 0) {
            result = _wtoi(line.c_str() + 9);
            finished = true;
            break;
        }
    }
    Check(finished, L"worker finished");
    Check(result == 0 || result == 4, L"expected worker result");
    DWORD exit_code = 0;
    Check(wperf::WaitForProcessExit(launch.process, 10000, exit_code), L"worker process exit");
    CloseHandle(launch.process);
    if (result == 0) {
        wperf::StopSessionByName(session_name, nullptr);
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}
}  // namespace

void TestFlameGraph() {
    const std::wstring folded =
        L"app.exe[1@1];main;alpha 60\n"
        L"app.exe[1@1];main;beta 40\n"
        L"app.exe[1@1];main;alpha;<script> 5\n";
    wperf::FlameGraphOptions options;
    options.title = L"Smoke <graph>";
    options.weight_name = L"nanoseconds";
    options.source = L"C:\\traces\\cpu.folded";
    options.details.push_back({L"mode", L"cpu"});
    wperf::FlameGraphResult result;
    std::wstring error;
    Check(wperf::RenderFoldedFlameGraph(folded, options, result, &error),
          L"render folded flame graph");
    Check(result.total_weight == 105 && result.stacks == 3 && result.nodes == 5,
          L"flame graph totals are wrong");
    Check(result.html.starts_with(L"<!doctype html>") &&
              result.html.find(L"<title>Smoke &lt;graph&gt;</title>") != std::wstring::npos,
          L"flame graph HTML escaping is wrong");
    Check(result.html.find(L"\\u003cscript\\u003e") != std::wstring::npos &&
              result.html.find(L"\"weight_name\":\"nanoseconds\"") != std::wstring::npos,
          L"flame graph embedded data is wrong");
    Check(result.html.find(L"https://") == std::wstring::npos,
          L"flame graph should not reference external resources");

    error.clear();
    Check(!wperf::RenderFoldedFlameGraph(L"broken line\n", options, result, &error) &&
              !error.empty(),
          L"malformed folded input must be rejected");
    error.clear();
    Check(!wperf::RenderFoldedFlameGraph(L"app;main -1\n", options, result, &error),
          L"negative folded weight must be rejected");
    error.clear();
    Check(wperf::RenderFoldedFlameGraph(L"\ufeffapp;main 3\n", options, result, &error) &&
              result.total_weight == 3,
          L"UTF-8 BOM before folded data must be accepted");
    error.clear();
    const std::wstring max_weight = L"18446744073709551615";
    Check(wperf::RenderFoldedFlameGraph(L"app;main " + max_weight + L"\n", options,
                                       result, &error) &&
              result.total_weight == UINT64_MAX,
          L"uint64 maximum folded weight must be accepted");
    Check(result.html.find(L"\"value\":\"18446744073709551615\"") !=
              std::wstring::npos,
          L"flame graph weights must remain exact decimal strings in embedded JSON");
    Check(result.html.find(L"\"value\":18446744073709551615") ==
              std::wstring::npos,
          L"flame graph weights must not be emitted as imprecise JSON numbers");
}

void RunTest(const wchar_t* name, void (*test)()) {
    std::wcout << L"[ RUN      ] " << name << std::endl;
    const int failures_before = g_failures;
    test();
    if (g_failures == failures_before) {
        std::wcout << L"[       OK ] " << name << std::endl;
    } else {
        std::wcerr << L"[   FAILED ] " << name << std::endl;
    }
}

int wmain() {
    RunTest(L"QuoteArgument", TestQuoteArgument);
    RunTest(L"StateRoundTrip", TestStateRoundTrip);
    RunTest(L"EtwQuery", TestEtwQuery);
    RunTest(L"CpuReportSynthetic", TestCpuReportSynthetic);
    RunTest(L"InvalidEtwFile", TestInvalidEtwFile);
    RunTest(L"TruncatedEtwFiles", TestTruncatedEtwFiles);
    RunTest(L"ThreadAndProcessIntervals", TestThreadAndProcessIntervals);
    RunTest(L"Disassembler", TestDisassembler);
    RunTest(L"LbrDecode", TestLbrDecode);
    RunTest(L"PmcQueries", TestPmcQueries);
    RunTest(L"TraceEventCorrelation", TestTraceEventCorrelation);
    RunTest(L"DbgHelpDiscovery", TestDbgHelpDiscovery);
    RunTest(L"ModulePathNormalization", TestModulePathNormalization);
    RunTest(L"SymbolizerPdbIdentity", TestSymbolizerPdbIdentity);
    RunTest(L"SymbolPathComposition", TestSymbolPathComposition);
    RunTest(L"SymbolPersistentCacheDiagnostics", TestSymbolPersistentCacheDiagnostics);
    RunTest(L"LiveTraceFlags", TestLiveTraceFlags);
    RunTest(L"LiveWorkerControlPlane", TestLiveWorkerControlPlane);
    RunTest(L"GoldenR3Replay", TestGoldenR3Replay);
    RunTest(L"GoldenCpuStackReplay", TestGoldenCpuStackReplay);
    RunTest(L"FlameGraph", TestFlameGraph);
    RunTest(L"WorkerIpcFailurePath", TestWorkerIpcFailurePath);
    if (g_failures == 0) {
        std::wcout << L"wperf unit tests: PASS\n";
        return 0;
    }
    std::wcerr << L"wperf unit tests: " << g_failures << L" failure(s)\n";
    return 1;
}
