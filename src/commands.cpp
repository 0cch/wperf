#include "commands.h"
#include "commands/live_control.h"

#include "commands/advanced.h"
#include "commands/help.h"
#include "commands/offline.h"
#include "commands/live_worker.h"
#include "commands/top.h"
#include "commands/util.h"

#include <combaseapi.h>

#include "common.h"
#include <psapi.h>
#include <realtimeapiset.h>
#include <tlhelp32.h>
#include "cpu_report.h"
#include "crash.h"
#include "elevation.h"
#include "etl_reader.h"
#include "etw.h"
#include "ipc.h"
#include "logger.h"
#include "state.h"
#include "symbolizer.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace wperf {
using namespace commands;

namespace {

std::atomic_bool g_interrupt_requested{false};
std::atomic_bool g_top_mode{false};
std::atomic_bool g_top_interrupted{false};

BOOL WINAPI ConsoleControlHandler(DWORD control_type) {
    if (control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT ||
        control_type == CTRL_CLOSE_EVENT || control_type == CTRL_LOGOFF_EVENT ||
        control_type == CTRL_SHUTDOWN_EVENT) {
        g_interrupt_requested.store(true);
        if (g_top_mode.load()) {
            g_top_interrupted.store(true);
        }
        return TRUE;
    }
    return FALSE;
}

int RunList(const std::vector<std::wstring>& arguments) {
    const bool providers_mode = !arguments.empty() && arguments[0] == L"providers";
    uint64_t limit = 500;
    for (size_t index = providers_mode ? 1 : 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            PrintCommandHelp(L"list");
            return 0;
        }
        if (providers_mode && arguments[index] == L"--max") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], limit)) {
                PrintError(L"--max requires a non-negative provider count");
                return 2;
            }
        } else {
            PrintError(L"unknown list option: " + arguments[index]);
            return 2;
        }
    }

    if (providers_mode) {
        std::wstring error;
        const auto providers = QueryProviders(&error);
        if (!error.empty()) {
            PrintError(error);
            return 5;
        }
        std::wcout << L"ETW providers: " << providers.size() << L"\n";
        std::wcout << L"GUID                                  SchemaSource  Name\n";
        for (size_t index = 0; index < providers.size() && index < limit; ++index) {
            std::wcout << FormatGuid(providers[index].id) << L"  " << std::setw(12)
                       << providers[index].schema_source << L"  "
                       << providers[index].name << L"\n";
        }
        return 0;
    }

    std::wstring error;
    const auto sources = QueryProfileSources(&error);
    if (!error.empty()) {
        PrintError(error);
        return 5;
    }

    std::wcout << L"Profile sources:\n";
    std::wcout << L"  Source  MinInterval  MaxInterval  Description\n";
    for (const auto& source : sources) {
        std::wcout << L"  " << std::setw(6) << source.source << L"  "
                   << std::setw(11) << source.min_interval << L"  "
                   << std::setw(11) << source.max_interval << L"  "
                   << source.description << L"\n";
    }

    error.clear();
    const auto sessions = QueryActiveSessions(&error);
    std::wcout << L"\nActive ETW sessions: " << sessions.size();
    if (!error.empty()) {
        std::wcout << L" (" << error << L")";
    }
    std::wcout << L"\n";
    for (const auto& session : sessions) {
        std::wcout << L"  " << session << L"\n";
    }
    return 0;
}
int RunDoctor(const std::vector<std::wstring>& arguments) {
    bool include_dumps = false;
    std::wstring bundle;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            PrintCommandHelp(L"doctor");
            return 0;
        }
        if (arguments[index] == L"--include-dumps") {
            include_dumps = true;
        } else if (arguments[index] == L"--bundle") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--bundle requires a directory");
                return 2;
            }
            bundle = arguments[++index];
        } else {
            PrintError(L"unknown doctor option: " + arguments[index]);
            return 2;
        }
    }
    bool elevated = false;
    std::wstring error;
    const bool elevation_ok = IsProcessElevated(elevated, &error);
    const auto sources = QueryProfileSources(&error);
    const uint32_t max_pmc = QueryMaxPmcCounters(&error);
    const auto sessions = QueryActiveSessions(&error);
    const auto states = StateStore::ReadAll(GetSessionsDirectory());

    std::wostringstream report;
    report << L"wperf " << kVersion << L"\n";
    report << L"Executable: " << GetExecutablePath() << L"\n";
    report << L"Elevated: " << (elevation_ok ? (elevated ? L"yes" : L"no")
                                              : L"unknown") << L"\n";
    report << L"State root: " << GetWperfRootDirectory().wstring() << L"\n";
    report << L"Profile sources: " << sources.size() << L"\n";
    report << L"Max PMC counters: " << max_pmc << L"\n";
    std::wstring pmc_owner_error;
    const auto pmc_owners = QueryPmcCounterOwners(&pmc_owner_error);
    std::wstring pmc_session_error;
    const auto pmc_sessions = QueryPmcSessions(&pmc_session_error);
    report << L"PMC counter owners: " << pmc_owners.size();
    if (!pmc_owner_error.empty()) {
        report << L" (" << pmc_owner_error << L")";
    }
    report << L"\n";
    report << L"PMC-enabled sessions: " << pmc_sessions.size();
    if (!pmc_session_error.empty()) {
        report << L" (" << pmc_session_error << L")";
    }
    report << L"\n";
    report << L"Active ETW sessions: " << sessions.size() << L"\n";
    for (const auto& session : sessions) {
        report << L"  " << session << L"\n";
    }
    report << L"Recorded runs: " << states.size() << L"\n";
    for (const auto& state : states) {
        report << L"  " << state.correlation_id << L" phase=" << state.phase
               << L" session=" << state.session_name
               << L" worker=" << state.worker_pid << L"\n";
    }
    const std::wstring text = report.str();
    std::wcout << text;

    if (!bundle.empty()) {
        const std::filesystem::path destination(bundle);
        if (!EnsureDirectory(destination, &error)) {
            PrintError(error);
            return 9;
        }
        std::ofstream doctor_file(destination / L"doctor.txt", std::ios::binary);
        doctor_file << ToUtf8(text);
        doctor_file.close();
        std::ofstream redaction(destination / L"REDACTION.txt", std::ios::binary);
        redaction << "Diagnostic bundles may contain paths, PIDs, user names and command lines.\n"
                  << "Crash dumps are excluded unless --include-dumps is specified.\n"
                  << "Review bundle contents before sharing.\n";
        redaction.close();

        std::error_code ec;
        const std::filesystem::path directories[] = {
            GetSessionsDirectory(), GetLogsDirectory(), GetCrashesDirectory()};
        for (const auto& directory : directories) {
            if (!std::filesystem::exists(directory, ec)) {
                continue;
            }
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
                if (ec || !entry.is_regular_file()) {
                    continue;
                }
                if (!include_dumps && entry.path().extension() == L".dmp") {
                    continue;
                }
                std::filesystem::copy_file(
                    entry.path(), destination / entry.path().filename(),
                    std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
        std::wcout << L"Diagnostic bundle: " << destination.wstring() << L"\n";
    }
    return 0;
}

int RunTest() {
    std::wstring error;
    if (!EnsureDirectory(GetSessionsDirectory(), &error) ||
        !EnsureDirectory(GetLogsDirectory(), &error) ||
        !EnsureDirectory(GetCrashesDirectory(), &error)) {
        PrintError(error);
        return 9;
    }

    const std::wstring correlation_id = NewId();
    Logger logger(GetLogsDirectory() / (correlation_id + L"-test.jsonl"),
                  correlation_id, L"test");
    logger.Log(LogLevel::Info, L"TEST", L"wperf self-test started");
    StateStore state_store(StateStore::PathFor(correlation_id));
    RunState state;
    state.correlation_id = correlation_id;
    state.phase = L"TEST";
    state.controller_pid = GetCurrentProcessId();
    state.controller_role = L"test";
    if (!state_store.Write(state, &error)) {
        PrintError(error);
        return 9;
    }

    bool elevated = false;
    IsProcessElevated(elevated, &error);
    const auto sources = QueryProfileSources(&error);
    const auto sessions = QueryActiveSessions(&error);
    std::wcout << L"Self-test\n";
    std::wcout << L"  elevated: " << (elevated ? L"yes" : L"no") << L"\n";
    std::wcout << L"  profile sources: " << sources.size() << L"\n";
    std::wcout << L"  active ETW sessions: " << sessions.size() << L"\n";
    std::wcout << L"  correlation: " << correlation_id << L"\n";
    std::wcout << L"  log: " << (logger.IsOpen() ? L"yes" : L"no") << L"\n";
    std::wcout << L"  state: " << state_store.Path().wstring() << L"\n";
    return error.empty() ? 0 : 5;
}

int RunRecover(const std::vector<std::wstring>& arguments) {
    std::wstring explicit_session;
    bool all_stale = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            PrintCommandHelp(L"recover");
            return 0;
        }
        if (arguments[index] == L"--session") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--session requires an ETW session name");
                return 2;
            }
            explicit_session = arguments[++index];
        } else if (arguments[index] == L"--all-stale") {
            all_stale = true;
        } else {
            PrintError(L"unknown recover option: " + arguments[index]);
            return 2;
        }
    }
    if (!explicit_session.empty() && all_stale) {
        PrintError(L"--session and --all-stale are mutually exclusive");
        return 2;
    }

    bool elevated = false;
    std::wstring error;
    if (!IsProcessElevated(elevated, &error) || !elevated) {
        PrintError(L"recover requires an elevated process");
        return 3;
    }

    std::vector<std::wstring> sessions;
    if (!explicit_session.empty()) {
        sessions.push_back(explicit_session);
    } else if (all_stale) {
        for (const auto& state : StateStore::ReadAll(GetSessionsDirectory())) {
            if (IsStale(state) && !state.session_name.empty()) {
                sessions.push_back(state.session_name);
            }
        }
    } else {
        PrintError(L"recover requires --session NAME or --all-stale");
        return 2;
    }

    int result = 0;
    for (const auto& session : sessions) {
        if (StopSessionByName(session, &error)) {
            std::wcout << L"recovered: " << session << L"\n";
        } else {
            PrintError(error);
            result = 4;
        }
    }
    return result;
}

}  // namespace
}  // namespace wperf

namespace wperf {
namespace {

int RunWorker(const std::vector<std::wstring>& arguments) {
    const std::wstring pipe_path = OptionValue(arguments, L"--pipe");
    const std::wstring nonce = OptionValue(arguments, L"--nonce");
    const std::wstring session_name = OptionValue(arguments, L"--session");
    const std::wstring output = OptionValue(arguments, L"--output");
    const std::wstring log_path = OptionValue(arguments, L"--log");
    const std::wstring guard_log = OptionValue(arguments, L"--guard-log");
    const std::wstring state_path = OptionValue(arguments, L"--state");
    const std::wstring correlation_id = OptionValue(arguments, L"--correlation");
    const std::wstring duration_text = OptionValue(arguments, L"--duration-ms", L"5000");
    const std::wstring interval_text =
        OptionValue(arguments, L"--profile-interval", L"10000");
    const std::wstring profile_text = OptionValue(arguments, L"--profile", L"cpu");
    const std::wstring pmc_source_text = OptionValue(arguments, L"--pmc-source", L"0");
    const std::wstring pmc_source_set_text =
        OptionValue(arguments, L"--pmc-source-set", L"0");
    const std::wstring lbr_text = OptionValue(arguments, L"--lbr", L"0");
    const std::wstring context_registers_text =
        OptionValue(arguments, L"--context-registers", L"0");
    const std::wstring call_graph_text =
        OptionValue(arguments, L"--call-graph", L"1");
    const std::wstring provider_text = OptionValue(arguments, L"--provider");
    const std::wstring controller_pid_text =
        OptionValue(arguments, L"--controller-pid");
    uint64_t duration_ms = 5000;
    uint32_t profile_interval = 10000;
    uint32_t pmc_source = 0;
    if (pipe_path.empty() || nonce.empty() || session_name.empty() ||
        output.empty() || log_path.empty() || guard_log.empty() ||
        state_path.empty() || correlation_id.empty() ||
        !ParseUnsigned(duration_text, duration_ms) ||
        !ParseUint32(interval_text, profile_interval)) {
        PrintError(L"invalid internal worker arguments");
        return 2;
    }

    InstallCrashHandler(GetCrashesDirectory(), correlation_id);
    Logger logger(log_path, correlation_id, L"worker");
    logger.Log(LogLevel::Info, L"ELEVATING", L"worker started");

    PipeClient pipe;
    std::wstring error;
    if (!pipe.Connect(pipe_path, 15000, &error)) {
        logger.Log(LogLevel::Error, L"ELEVATING", error);
        return 3;
    }
    uint32_t expected_controller_pid = 0;
    uint32_t actual_controller_pid = 0;
    if (!ParseUint32(controller_pid_text, expected_controller_pid) ||
        !pipe.ServerProcessId(actual_controller_pid) ||
        actual_controller_pid != expected_controller_pid) {
        pipe.SendLine(L"FINISHED 4", nullptr);
        return 4;
    }
    if (!pipe.SendLine(L"HELLO " + nonce + L" " +
                           std::to_wstring(GetCurrentProcessId()), &error)) {
        logger.Log(LogLevel::Error, L"ELEVATING", error);
        return 9;
    }

    ScopedHandle controller(OpenProcess(SYNCHRONIZE,FALSE,expected_controller_pid));
    if(!controller.get())return 4;
    std::wstring command;
    if (!pipe.ReceiveLine(command, 10000, &error) || command != L"START") {
        logger.Log(LogLevel::Error, L"ELEVATING", L"START handshake failed");
        return 9;
    }

    StateStore state_store(state_path);
    RunState state;
    state.correlation_id = correlation_id;
    state.phase = L"WORKER_READY";
    state.session_name = session_name;
    state.trace_path = output;
    state.controller_role = L"worker";
    state.worker_pid = GetCurrentProcessId();
    state_store.Write(state, nullptr);

    std::wstring privilege_error;
    if (!EnablePrivilege(L"SeSystemProfilePrivilege", &privilege_error)) {
        logger.Log(LogLevel::Warning, L"WORKER_READY", privilege_error);
    }

    EtwSession session;
    TraceOptions trace_options;
    trace_options.session_name = session_name;
    trace_options.output_path = output;
    trace_options.profile_interval_100ns = profile_interval;
    trace_options.enable_flags = TraceFlagsForProfile(profile_text);
    trace_options.pmc_profile = profile_text == L"pmu" || pmc_source_set_text == L"1";
    trace_options.pmc_source = pmc_source;
    if (!ParseUint32(OptionValue(arguments, L"--period", L"0"), trace_options.pmc_interval)) return 2;
    trace_options.enable_lbr = lbr_text == L"1";
    trace_options.enable_context_registers = context_registers_text == L"1";
    trace_options.stack_cache=HasFlag(arguments,L"--stack-cache");
    const auto counter_text=OptionValue(arguments,L"--pmc-counters");
    if(!counter_text.empty()) {
        std::wistringstream values(counter_text); std::wstring token;
        while(std::getline(values,token,L',')) { uint32_t source=0; if(!ParseUint32(token,source))return 2; trace_options.pmc_counter_sources.push_back(source); }
    }
    trace_options.enable_stack_tracing = call_graph_text == L"1";
    trace_options.enable_scheduler_stack =
        call_graph_text == L"1" &&
        (profile_text == L"sched" || profile_text == L"cpu-sched" ||
         profile_text == L"all");
    if (!provider_text.empty()) {
        if (FAILED(IIDFromString(provider_text.c_str(), &trace_options.provider_id))) {
            logger.Log(LogLevel::Error, L"WORKER_READY", L"invalid provider GUID");
            pipe.SendLine(L"ERROR 4 invalid provider GUID", nullptr);
            pipe.SendLine(L"FINISHED 4", nullptr);
            return 4;
        }
        trace_options.system_logger = false;
        trace_options.enable_provider = true;
        trace_options.enable_stack_tracing = false;
    }
    const std::wstring clean_event_name = L"Local\\wperf-clean-" + correlation_id;
    HANDLE clean_event = CreateEventW(nullptr, TRUE, FALSE, clean_event_name.c_str());
    if (clean_event == nullptr) {
        error = L"CreateEventW(clean) failed: " + FormatWindowsError(GetLastError());
        session.Stop(nullptr);
        state.phase = L"INTERNAL_ERROR";
        state.error_code = 9;
        state_store.Write(state, nullptr);
        pipe.SendLine(L"FINISHED 9", nullptr);
        return 9;
    }

    const ProfileRestoreInfo restore{};
    std::vector<std::wstring> guard_arguments{
        L"--internal-session-guard",
        L"--session", session_name,
        L"--event", clean_event_name,
        L"--worker-pid", std::to_wstring(GetCurrentProcessId()),
        L"--restore-source", std::to_wstring(restore.source),
        L"--restore-interval", std::to_wstring(restore.original_interval),
        L"--configured-interval", std::to_wstring(restore.configured_interval),
        L"--clear-pmc", restore.clear_pmc ? L"1" : L"0",
        L"--log", guard_log,
        L"--state", state_path,
        L"--correlation", correlation_id,
    };
    ProcessLaunchResult guard;
    if (!LaunchWorkerProcess(GetExecutablePath(), guard_arguments, false, guard, &error)) {
        logger.Log(LogLevel::Error, L"CAPTURING", error);
        session.Stop(nullptr);
        SetEvent(clean_event);
        CloseHandle(clean_event);
        state.phase = L"INTERNAL_ERROR";
        state.error_code = 9;
        state_store.Write(state, nullptr);
        pipe.SendLine(L"FINISHED 9", nullptr);
        return 9;
    }
    CloseHandle(guard.process);
    state.guard_pid = guard.process_id;
    state_store.Write(state, nullptr);
    pipe.SendLine(L"PHASE GUARD_STARTED " + std::to_wstring(guard.process_id), nullptr);
    session.SetRecoveryWriter([&](const ProfileRestoreInfo& restore) {
        state.restore_source=restore.source; state.restore_interval=restore.original_interval;
        state.configured_interval=restore.configured_interval; state.restore_clear_pmc=restore.clear_pmc;
        state.phase=L"CONFIGURING";
        return state_store.Write(state,&error);
    });
    if (!session.Start(trace_options, &error)) {
        logger.Log(LogLevel::Error, L"WORKER_READY", error);
        state.phase = L"INTERNAL_ERROR";
        state.error_code = 4;
        state_store.Write(state, nullptr);
        pipe.SendLine(L"ERROR 4 " + error, nullptr);
        pipe.SendLine(L"FINISHED 4", nullptr);
        if(!session.IsActive()&&!session.RestoreInfo().clear_pmc)SetEvent(clean_event);
        CloseHandle(clean_event);
        return 4;
    }

    state.phase = L"CAPTURING";
    state_store.Write(state, nullptr);
    pipe.SendLine(L"PHASE CAPTURING", nullptr);
    if (trace_options.enable_lbr) {
        pipe.SendLine(L"STATUS lbr=" + session.LbrStatus(), nullptr);
    }
    if (trace_options.enable_context_registers) {
        pipe.SendLine(L"STATUS context-registers=" +
                          session.ContextRegisterStatus(),
                      nullptr);
    }
    if (trace_options.enable_scheduler_stack) {
        pipe.SendLine(L"STATUS scheduler-stack=" +
                          session.SchedulerStackStatus(),
                      nullptr);
    }
    logger.Log(LogLevel::Info, L"CAPTURING", L"ETW session started");

    pipe.SendLine(L"READY " + session_name, nullptr);

    const ULONGLONG deadline = duration_ms == 0 ? 0 : GetTickCount64() + duration_ms;
    while (deadline == 0 || GetTickCount64() < deadline) {
        if (!pipe.IsConnected() || WaitForSingleObject(controller.get(),0)!=WAIT_TIMEOUT) break;
        if (pipe.HasData()) {
            std::wstring line;
            if (!pipe.ReceiveLine(line, 1000, &error)) {
                logger.Log(LogLevel::Warning, L"CAPTURING", error);
                break;
            }
            if (line == L"STOP") {
                break;
            }
            if (line == L"INJECT_CRASH" &&
                GetEnvironmentVariableW(L"WPERF_ENABLE_TEST_FAULTS", nullptr, 0) > 0) {
                RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
            }
        }
        Sleep(25);
    }

    state.phase = L"STOPPING";
    state_store.Write(state, nullptr);
    int exit_code=4;
    { WorkerStopWatchdog timeout; exit_code=session.Stop(&error)?0:4; }
    TraceQuality quality;
    session.QueryQuality(quality, nullptr);
    if (quality.lost_events || quality.lost_buffers) {
        pipe.SendLine(L"STATUS incomplete lost=" + std::to_wstring(quality.lost_events) +
                      L" lost_buffers=" + std::to_wstring(quality.lost_buffers), nullptr);
    }
    if (exit_code == 0) SetEvent(clean_event);
    CloseHandle(clean_event);
    state.phase = L"FINISHED";
    state.error_code = static_cast<uint32_t>(exit_code);
    state_store.Write(state, nullptr);
    pipe.SendLine(L"FINISHED " + std::to_wstring(exit_code), nullptr);
    logger.Log(LogLevel::Info, L"FINISHED", L"worker finished");
    return exit_code;
}

int RunGuard(const std::vector<std::wstring>& arguments) {
    const std::wstring session_name = OptionValue(arguments, L"--session");
    const std::wstring event_name = OptionValue(arguments, L"--event");
    const std::wstring worker_text = OptionValue(arguments, L"--worker-pid");
    const std::wstring log_path = OptionValue(arguments, L"--log");
    const std::wstring state_path = OptionValue(arguments, L"--state");
    const std::wstring correlation_id = OptionValue(arguments, L"--correlation");
    uint32_t worker_pid = 0;
    if (session_name.empty() || event_name.empty() || log_path.empty() ||
        state_path.empty() || correlation_id.empty() ||
        !ParseUint32(worker_text, worker_pid)) {
        return 2;
    }

    InstallCrashHandler(GetCrashesDirectory(), correlation_id);
    Logger logger(log_path, correlation_id, L"guard");
    logger.Log(LogLevel::Info, L"GUARD_STARTED", L"session guard started");

    HANDLE clean_event = OpenEventW(SYNCHRONIZE, FALSE, event_name.c_str());
    HANDLE worker = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, worker_pid);
    if (clean_event == nullptr || worker == nullptr) {
        if (clean_event != nullptr) {
            CloseHandle(clean_event);
        }
        if (worker != nullptr) {
            CloseHandle(worker);
        }
        return 0;
    }

    HANDLE handles[] = {clean_event, worker};
    const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    int result = 0;
    if (wait == WAIT_OBJECT_0 + 1) {
        DWORD worker_exit = 0;
        GetExitCodeProcess(worker, &worker_exit);
        std::wstring error;
        if (!StopSessionByName(session_name, &error)) {
            logger.Log(LogLevel::Error, L"WORKER_LOST", error);
            result = 4;
        } else {
            logger.Log(LogLevel::Warning, L"WORKER_LOST",
                       L"stopped orphan session after worker exit");
        }

        StateStore state_store(state_path);
        RunState state;
        for (const auto& candidate : StateStore::ReadAll(std::filesystem::path(state_path).parent_path())) {
            if (candidate.correlation_id == correlation_id) {
                state = candidate;
                break;
            }
        }
        state.correlation_id = correlation_id;
        state.session_name = session_name;
        state.phase = L"TRACE_INCOMPLETE";
        state.guard_pid = GetCurrentProcessId();
        state.worker_pid = worker_pid;
        state.error_code = worker_exit;
        ProfileRestoreInfo restore;
        ParseUint32(OptionValue(arguments, L"--restore-source", L"0"), restore.source);
        ParseUint32(OptionValue(arguments, L"--restore-interval", L"0"), restore.original_interval);
        ParseUint32(OptionValue(arguments, L"--configured-interval", L"0"), restore.configured_interval);
        restore.clear_pmc = OptionValue(arguments, L"--clear-pmc", L"0") == L"1";
        if(state.restore_interval||state.restore_clear_pmc) {
            restore={state.restore_source,state.restore_interval,state.configured_interval,state.restore_clear_pmc!=0};
        }
        EnablePrivilege(L"SeSystemProfilePrivilege", nullptr);
        if (!RestoreProfileConfiguration(restore, &error)) {
            logger.Log(LogLevel::Error, L"WORKER_LOST", error);
            result = 4;
        } else { state.restore_interval=state.configured_interval=state.restore_clear_pmc=0; }
        state_store.Write(state, nullptr);
    } else if (wait != WAIT_OBJECT_0) {
        result = 9;
    }

    CloseHandle(worker);
    CloseHandle(clean_event);
    return result;
}

struct RecordOptions {
    uint64_t duration_ms = 0;
    bool duration_explicit = false;
    uint32_t profile_interval_100ns = 10000;
    std::filesystem::path output;
    std::wstring requested_session;
    std::optional<uint32_t> pid;
    std::vector<std::wstring> target;
    std::wstring profile = L"cpu";
    uint32_t pmc_source = 0;
    bool pmc_source_set = false;
    uint32_t pmc_interval = 0;
    std::vector<uint32_t> pmc_counter_sources;
    bool stack_cache = false;
    bool frequency_explicit = false;
    bool lbr = false;
    bool context_registers = false;
    bool call_graph = true;
    std::optional<GUID> provider;
    bool elevate = false;
};

bool ParseRecordOptions(const std::vector<std::wstring>& arguments,
                        RecordOptions& options, std::wstring& error) {
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"--") {
            options.target.assign(arguments.begin() + static_cast<ptrdiff_t>(index + 1),
                                  arguments.end());
            break;
        }
        if (argument == L"--elevate") {
            options.elevate = true;
        } else if (argument == L"--duration" || argument == L"-d") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], options.duration_ms) ||
                options.duration_ms == 0) {
                error = L"--duration requires milliseconds";
                return false;
            }
            options.duration_explicit = true;
        } else if (argument == L"--output" || argument == L"-o") {
            if (index + 1 >= arguments.size()) {
                error = L"--output requires a path";
                return false;
            }
            options.output = arguments[++index];
        } else if (argument == L"--session") {
            if (index + 1 >= arguments.size()) {
                error = L"--session requires a name";
                return false;
            }
            options.requested_session = arguments[++index];
        } else if (argument == L"--pmc-source" || argument == L"--pmu-source") {
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], options.pmc_source)) {
                error = L"--pmc-source requires a numeric source id";
                return false;
            }
            options.pmc_source_set = true;
        } else if(argument==L"--stack-cache") {
            options.stack_cache=true;
        } else if(argument==L"--pmc-counters") {
            if(index+1>=arguments.size())return false;
            std::wistringstream values(arguments[++index]); std::wstring token;
            while(std::getline(values,token,L',')) {
                uint32_t source=0;
                if(!ParseUint32(token,source)||source==0) { error=L"--pmc-counters requires hardware source IDs"; return false; }
                if(std::find(options.pmc_counter_sources.begin(),options.pmc_counter_sources.end(),source)!=options.pmc_counter_sources.end()) {
                    error=L"duplicate PMC snapshot source"; return false;
                }
                options.pmc_counter_sources.push_back(source);
            }
            if(options.pmc_counter_sources.empty()) { error=L"empty PMC snapshot source list"; return false; }
        } else if (argument == L"--period") {
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], options.pmc_interval) || options.pmc_interval == 0) {
                error = L"--period requires a positive PMC event count"; return false;
            }
        } else if (argument == L"--lbr" || argument == L"-b") {
            options.lbr = true;
        } else if (argument == L"--context-registers") {
            options.context_registers = true;
        } else if (argument == L"-g" || argument == L"--call-graph") {
            options.call_graph = true;
        } else if (argument == L"--no-call-graph") {
            options.call_graph = false;
        } else if (argument == L"--provider") {
            if (index + 1 >= arguments.size()) {
                error = L"--provider requires a GUID";
                return false;
            }
            GUID provider_id{};
            if (FAILED(IIDFromString(arguments[++index].c_str(), &provider_id))) {
                error = L"invalid provider GUID";
                return false;
            }
            options.provider = provider_id;
        } else if (argument == L"--profile") {
            if (index + 1 >= arguments.size()) {
                error = L"--profile requires a profile name";
                return false;
            }
            options.profile = arguments[++index];
            if (options.profile != L"cpu" && options.profile != L"cpu-sched" &&
                options.profile != L"sched" && options.profile != L"syscall" &&
                options.profile != L"vm" && options.profile != L"io" &&
                options.profile != L"kernel" && options.profile != L"all" &&
                options.profile != L"pmu") {
                error = L"unsupported profile: " + options.profile;
                return false;
            }
        } else if (argument == L"-e" || argument == L"--event") {
            if (index + 1 >= arguments.size()) {
                error = L"--event requires a profile name or PMC source";
                return false;
            }
            const std::wstring event = arguments[++index];
            uint32_t source = 0;
            if (ParseUint32(event, source)) {
                options.pmc_source = source;
                options.pmc_source_set = true;
            } else if (event == L"cpu" || event == L"cpu-sched" ||
                       event == L"sched" || event == L"syscall" ||
                       event == L"vm" || event == L"io" ||
                       event == L"kernel" || event == L"all" ||
                       event == L"pmu") {
                options.profile = event;
            } else {
                std::wstring source_error;
                const auto sources = QueryProfileSources(&source_error);
                bool found = false;
                for (const auto& candidate : sources) {
                    if (_wcsicmp(candidate.description.c_str(), event.c_str()) == 0) {
                        options.pmc_source = candidate.source;
                        options.pmc_source_set = true;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    error = L"unsupported record event: " + event;
                    return false;
                }
            }
        } else if (argument == L"--freq" || argument == L"-F") {
            uint64_t frequency = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], frequency) || frequency == 0 ||
                frequency > 100000) {
                error = L"--freq requires HZ between 1 and 100000";
                return false;
            }
            options.frequency_explicit = true;
            options.profile_interval_100ns =
                static_cast<uint32_t>(std::max<uint64_t>(1, 10000000 / frequency));
        } else if (argument == L"--pid" || argument == L"-p") {
            uint32_t pid = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], pid)) {
                error = L"--pid requires a process id";
                return false;
            }
            options.pid = pid;
        } else if (argument == L"--help" || argument == L"-h") {
            PrintCommandHelp(L"record");
            return false;
        } else {
            error = L"unknown record option: " + argument;
            return false;
        }
    }
    if (options.pid.has_value() && !options.target.empty()) {
        error = L"--pid and a target command are mutually exclusive";
        return false;
    }
    if(options.stack_cache&&!options.call_graph) { error=L"--stack-cache requires call graphs"; return false; }
    const bool pmc = options.profile == L"pmu" || options.pmc_source_set;
    if ((pmc && options.frequency_explicit) || (!pmc && options.pmc_interval != 0)) {
        error = L"use --period for PMC event counts and -F for CPU sampling frequency";
        return false;
    }
    if (pmc && !options.pmc_source_set) {
        error = L"PMC sampling requires -e SOURCE or --pmc-source ID"; return false;
    }
    return true;
}
int RunRecord(const std::vector<std::wstring>& arguments, uint32_t* captured_pid = nullptr) {
    RecordOptions options;
    std::wstring error;
    if (!ParseRecordOptions(arguments, options, error)) {
        if (!error.empty()) {
            PrintError(error);
            return 2;
        }
        return 0;
    }
    if (options.duration_ms == 0 && !options.duration_explicit &&
        !options.pid.has_value() && options.target.empty()) {
        options.duration_ms = 5000;
    }

    bool elevated = false;
    if (!IsProcessElevated(elevated, &error)) {
        PrintError(error);
        return 9;
    }
    if (!elevated && !options.elevate && !options.provider.has_value()) {
        PrintError(L"record requires administrator rights; rerun with --elevate");
        return 3;
    }

    const std::wstring correlation_id = NewId();
    const std::wstring nonce = NewId();
    const std::wstring session_name =
        options.requested_session.empty() ? (L"wperf-" + correlation_id)
                                          : options.requested_session;
    if (SessionExists(session_name)) {
        PrintError(L"ETW session already exists: " + session_name);
        return 4;
    }

    std::filesystem::path output = options.output;
    if (output.empty()) {
        output = std::filesystem::current_path() /
                 (L"wperf-" + correlation_id + L".etl");
    } else if (output.is_relative()) {
        output = std::filesystem::absolute(output);
    }
    if (std::filesystem::exists(output)) {
        PrintError(L"output already exists: " + output.wstring());
        return 2;
    }
    EnsureDirectory(output.parent_path(), &error);

    const std::wstring controller_log =
        (GetLogsDirectory() / (correlation_id + L"-controller.jsonl")).wstring();
    const std::wstring worker_log =
        (GetLogsDirectory() / (correlation_id + L"-worker.jsonl")).wstring();
    const std::wstring guard_log =
        (GetLogsDirectory() / (correlation_id + L"-guard.jsonl")).wstring();
    const std::wstring state_path = StateStore::PathFor(correlation_id).wstring();

    Logger logger(controller_log, correlation_id, L"controller");
    StateStore state_store(state_path);
    RunState state;
    state.correlation_id = correlation_id;
    state.phase = L"CREATED";
    state.session_name = session_name;
    state.trace_path = output.wstring();
    state.controller_role = L"controller";
    state.controller_pid = GetCurrentProcessId();
    state_store.Write(state, nullptr);

    PipeServer pipe;
    if (!pipe.Create(correlation_id, &error)) {
        PrintError(error);
        return 9;
    }

    std::vector<std::wstring> worker_arguments{
        L"--internal-elevated-worker",
        L"--pipe", MakePipePath(correlation_id),
        L"--nonce", nonce,
        L"--session", session_name,
        L"--output", output.wstring(),
        L"--log", worker_log,
        L"--guard-log", guard_log,
        L"--state", state_path,
        L"--correlation", correlation_id,
        L"--profile", options.profile,
        L"--profile-interval", std::to_wstring(options.profile_interval_100ns),
        L"--pmc-source", std::to_wstring(options.pmc_source),
        L"--period", std::to_wstring(options.pmc_interval),
        L"--pmc-source-set", options.pmc_source_set ? L"1" : L"0",
        L"--lbr", options.lbr ? L"1" : L"0",
        L"--call-graph", options.call_graph ? L"1" : L"0",
        L"--controller-pid", std::to_wstring(GetCurrentProcessId()),
        L"--context-registers", options.context_registers ? L"1" : L"0",
        L"--duration-ms", std::to_wstring(options.duration_ms),
    };
    if(options.stack_cache)worker_arguments.push_back(L"--stack-cache");
    if(!options.pmc_counter_sources.empty()) {
        std::wstring values;
        for(uint32_t source:options.pmc_counter_sources) { if(!values.empty())values+=L","; values+=std::to_wstring(source); }
        worker_arguments.push_back(L"--pmc-counters"); worker_arguments.push_back(values);
    }
    if (options.provider.has_value()) {
        wchar_t provider_text[64]{};
        StringFromGUID2(*options.provider, provider_text,
                        static_cast<int>(std::size(provider_text)));
        worker_arguments.push_back(L"--provider");
        worker_arguments.push_back(provider_text);
    }

    state.phase = L"ELEVATING";
    state_store.Write(state, nullptr);
    ProcessLaunchResult launch;
    if (!LaunchWorkerProcess(GetExecutablePath(), worker_arguments,
                             !elevated && options.elevate, launch, &error)) {
        state.phase = L"ELEVATION_FAILED";
        state.error_code = launch.error_code;
        state_store.Write(state, nullptr);
        PrintError(error);
        return launch.error_code == ERROR_CANCELLED ? 3 : 9;
    }
    state.worker_pid = launch.process_id;
    state_store.Write(state, nullptr);

    if (!pipe.WaitForClient(20000, &error)) {
        DWORD ignored = 0;
        if (!WaitForProcessExit(launch.process, 5000, ignored)) {
            TerminateProcess(launch.process, 9);
        }
        CloseHandle(launch.process);
        state.phase = L"ELEVATION_FAILED";
        state.error_code = 9;
        state_store.Write(state, nullptr);
        PrintError(error);
        return 9;
    }

    std::wstring line;
    if (!pipe.ReceiveLine(line, 10000, &error) ||
        line != L"HELLO " + nonce + L" " + std::to_wstring(launch.process_id)) {
        TerminateProcess(launch.process, 9);
        CloseHandle(launch.process);
        state.phase = L"ELEVATION_FAILED";
        state.error_code = 9;
        state_store.Write(state, nullptr);
        PrintError(L"worker handshake failed");
        return 9;
    }
    if (!pipe.SendLine(L"START", &error)) {
        TerminateProcess(launch.process, 9);
        CloseHandle(launch.process);
        PrintError(error);
        return 9;
    }

    if (!WaitForCaptureReady(pipe, launch.process, session_name, 20000, &error)) {
        pipe.SendLine(L"STOP", nullptr);
        DWORD ignored = 0;
        if (!WaitForProcessExit(launch.process, 10000, ignored)) TerminateProcess(launch.process, 9);
        CloseHandle(launch.process);
        state.phase = L"START_FAILED";
        state.error_code = 4;
        state_store.Write(state, nullptr);
        PrintError(error);
        return 4;
    }
    state.phase = L"CAPTURING";
    state_store.Write(state, nullptr);
    std::wcout << L"capturing: " << session_name << L"\n";

    ProcessLaunchResult target_launch;
    HANDLE observed_process = nullptr;
    uint32_t observed_pid = 0;
    if (!options.target.empty()) {
        if (!LaunchTargetProcess(options.target, target_launch, &error)) {
            pipe.SendLine(L"STOP", nullptr);
            PrintError(error);
            WaitForProcessExit(launch.process, 10000, target_launch.error_code);
            CloseHandle(launch.process);
            return 8;
        }
        observed_process = target_launch.process;
        observed_pid = target_launch.process_id;
    } else if (options.pid.has_value()) {
        observed_pid = *options.pid;
        observed_process = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, observed_pid);
        if (observed_process == nullptr) {
            pipe.SendLine(L"STOP", nullptr);
            PrintError(L"cannot open target pid " + std::to_wstring(observed_pid));
            WaitForProcessExit(launch.process, 10000, target_launch.error_code);
            CloseHandle(launch.process);
            return 8;
        }
    }
    if (captured_pid != nullptr) *captured_pid = observed_pid;
    if (observed_pid != 0) {
        state.target_pid = observed_pid;
        state_store.Write(state, nullptr);
    }

    g_interrupt_requested.store(false);
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
    bool finished = false;
    int worker_result = 9;
    bool stop_sent = false;
    DWORD target_exit_code = 0;
    const ULONGLONG deadline =
        options.duration_ms == 0 ? 0 : GetTickCount64() + options.duration_ms + 60000;
    while (deadline == 0 || GetTickCount64() < deadline) {
        if (observed_process != nullptr && !stop_sent &&
            WaitForSingleObject(observed_process, 0) == WAIT_OBJECT_0) {
            GetExitCodeProcess(observed_process, &target_exit_code);
            stop_sent = true;
            pipe.SendLine(L"STOP", nullptr);
        }
        if (g_interrupt_requested.load() && !stop_sent) {
            stop_sent = true;
            pipe.SendLine(L"STOP", nullptr);
        }
        if (pipe.HasData()) {
            if (!pipe.ReceiveLine(line, 1000, &error)) {
                break;
            }
            if (StartsWith(line, L"PHASE GUARD_STARTED ")) {
                uint32_t guard_pid = 0;
                if (ParseUint32(AfterSpace(line), guard_pid)) {
                    state.guard_pid = guard_pid;
                    state_store.Write(state, nullptr);
                }
            } else if (line == L"PHASE CAPTURING") {
                state.phase = L"CAPTURING";
                state_store.Write(state, nullptr);
            } else if (StartsWith(line, L"READY ")) {
                std::wcout << L"capturing: " << session_name << L"\n";
            } else if (StartsWith(line, L"STATUS ")) {
                std::wcout << L"status: " << line.substr(7) << L"\n";
            } else if (StartsWith(line, L"ERROR ")) {
                std::wcerr << line << L"\n";
            } else if (StartsWith(line, L"FINISHED ")) {
                uint64_t parsed = 0;
                if (ParseUnsigned(AfterSpace(line), parsed)) {
                    worker_result = static_cast<int>(parsed);
                }
                finished = true;
                break;
            }
        } else {
            DWORD exit_code = 0;
            if (GetExitCodeProcess(launch.process, &exit_code) &&
                exit_code != STILL_ACTIVE) {
                break;
            }
            Sleep(50);
        }
    }
    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);

    if (!finished) {
        pipe.SendLine(L"STOP", nullptr);
        DWORD ignored = 0;
        if (!WaitForProcessExit(launch.process, 10000, ignored)) {
            TerminateProcess(launch.process, 9);
        }
        worker_result = 9;
    }
    DWORD process_exit = 0;
    WaitForProcessExit(launch.process, 15000, process_exit);
    CloseHandle(launch.process);
    if (observed_process != nullptr) {
        if (target_exit_code == 0) {
            GetExitCodeProcess(observed_process, &target_exit_code);
        }
        CloseHandle(observed_process);
    }

    if (finished && worker_result == 0 && (options.pmc_source_set || options.profile == L"pmu" || options.lbr || !options.pmc_counter_sources.empty())) {
        TraceData captured;
        if (!ReadEtwFile(output, captured, &error)) { PrintError(error); worker_result = 6; }
        else if(!options.pmc_counter_sources.empty()&&captured.quality.pmc_counter_events==0) {
            PrintError(L"requested PMC snapshots produced no counter payloads; unavailable"); worker_result=5;
        }
        else if (options.lbr && captured.quality.lbr_events == 0) {
            PrintError(L"LBR configuration produced no LBR events; unavailable"); worker_result = 5;
        } else if ((options.pmc_source_set || options.profile == L"pmu") &&
                   std::none_of(captured.samples.begin(), captured.samples.end(), [&](const auto& sample) {
                       return sample.kind == SampleKind::Pmc && sample.profile_source == options.pmc_source;
                   })) {
            PrintError(L"requested PMC source produced no PMC interrupts; unavailable"); worker_result = 5;
        }
    }
    state.phase = finished ? L"FINISHED" : L"WORKER_LOST";
    state.error_code = static_cast<uint32_t>(worker_result);
    state_store.Write(state, nullptr);
    if (worker_result == 0 && finished) {
        std::wcout << L"trace: " << output.wstring() << L"\n";
    }
    if (target_exit_code != 0 && target_exit_code != STILL_ACTIVE) {
        return static_cast<int>(target_exit_code);
    }
    return worker_result;
}

}  // namespace

struct ProcessStats {
    uint64_t user_100ns = 0;
    uint64_t kernel_100ns = 0;
    uint64_t cycles = 0;
    uint64_t page_faults = 0;
    uint64_t read_operations = 0;
    uint64_t write_operations = 0;
    uint64_t read_bytes = 0;
    uint64_t write_bytes = 0;
    bool cycles_valid = false;
};

uint64_t FileTimeToUint64(const FILETIME& value) {
    ULARGE_INTEGER integer{};
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}

std::wstring ProcessImageName(HANDLE process) {
    wchar_t path[32768]{};
    const DWORD length = K32GetModuleFileNameExW(
        process, nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length == 0) {
        return {};
    }
    return std::filesystem::path(std::wstring(path, length)).filename().wstring();
}
bool ReadProcessStats(HANDLE process, ProcessStats& stats) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return false;
    }
    stats.user_100ns = FileTimeToUint64(user);
    stats.kernel_100ns = FileTimeToUint64(kernel);

    ULONG64 cycles = 0;
    if (QueryProcessCycleTime(process, &cycles)) {
        stats.cycles = cycles;
        stats.cycles_valid = true;
    }

    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             sizeof(memory))) {
        stats.page_faults = memory.PageFaultCount;
    }

    IO_COUNTERS io{};
    if (GetProcessIoCounters(process, &io)) {
        stats.read_operations = io.ReadOperationCount;
        stats.write_operations = io.WriteOperationCount;
        stats.read_bytes = io.ReadTransferCount;
        stats.write_bytes = io.WriteTransferCount;
    }
    return true;
}

uint64_t Delta(uint64_t after, uint64_t before) {
    return after >= before ? after - before : 0;
}

std::wstring FormatMilliseconds(uint64_t value_100ns) {
    return std::to_wstring(value_100ns / 10000) + L" ms";
}

void PrintThreadStats(uint32_t pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::wcout << L"  tid  user_ms  sys_ms  cycles\n";
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != pid) {
                continue;
            }
            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                       entry.th32ThreadID);
            if (thread == nullptr) {
                continue;
            }
            FILETIME creation{}, exit{}, kernel{}, user{};
            ULONG64 cycles = 0;
            if (GetThreadTimes(thread, &creation, &exit, &kernel, &user)) {
                const uint64_t user_ms = FileTimeToUint64(user) / 10000;
                const uint64_t kernel_ms = FileTimeToUint64(kernel) / 10000;
                if (QueryThreadCycleTime(thread, &cycles)) {
                    std::wcout << L"  " << entry.th32ThreadID << L"  " << user_ms
                               << L"  " << kernel_ms << L"  " << cycles << L"\n";
                } else {
                    std::wcout << L"  " << entry.th32ThreadID << L"  " << user_ms
                               << L"  " << kernel_ms << L"  n/a\n";
                }
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

int RunStat(const std::vector<std::wstring>& arguments) {
    std::optional<uint32_t> pid;
    uint64_t duration_ms = 1000;
    bool per_thread = false;
    bool pmu = false;
    std::wstring pmu_event;
    std::vector<std::wstring> target;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"--") {
            target.assign(arguments.begin() + static_cast<ptrdiff_t>(index + 1),
                          arguments.end());
            break;
        }
        if (arguments[index] == L"-p" || arguments[index] == L"--pid") {
            uint32_t value = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], value)) {
                PrintError(L"--pid requires a process id");
                return 2;
            }
            pid = value;
        } else if (arguments[index] == L"--duration") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], duration_ms) || duration_ms == 0) {
                PrintError(L"--duration requires a positive millisecond value");
                return 2;
            }
        } else if (arguments[index] == L"--per-thread") {
            per_thread = true;
        } else if (arguments[index] == L"--pmu") {
            pmu = true;
        } else if (arguments[index] == L"-e" || arguments[index] == L"--event" ||
                   arguments[index] == L"--pmc-source") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--event requires an ETW PMC source");
                return 2;
            }
            pmu_event = arguments[++index];
            pmu = true;
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            PrintCommandHelp(L"stat");
            return 0;
        } else {
            PrintError(L"unknown stat option: " + arguments[index]);
            return 2;
        }
    }
    if (pid.has_value() && !target.empty()) {
        PrintError(L"--pid and target command are mutually exclusive");
        return 2;
    }
    if (!pid.has_value() && target.empty()) {
        PrintError(L"stat requires -p PID or a target command");
        return 2;
    }

    if (pmu) {
        std::wstring error;
        const auto sources = QueryProfileSources(&error);
        if (!error.empty()) {
            PrintError(error);
            return 5;
        }
        uint32_t source = 0;
        bool source_found = false;
        for (const auto& candidate : sources) {
            if (candidate.description == L"TotalCycles") {
                source = candidate.source;
                source_found = true;
                break;
            }
        }
        if (!pmu_event.empty()) {
            uint32_t numeric = 0;
            if (ParseUint32(pmu_event, numeric)) {
                source = numeric;
                source_found = true;
            } else {
                for (const auto& candidate : sources) {
                    if (_wcsicmp(candidate.description.c_str(), pmu_event.c_str()) == 0) {
                        source = candidate.source;
                        source_found = true;
                        break;
                    }
                }
            }
        }
        if (!source_found) {
            PrintError(L"requested PMU source was not found");
            return 5;
        }

        const std::filesystem::path trace_path =
            GetWperfRootDirectory() / (L"pmu-" + NewId() + L".etl");
        std::vector<std::wstring> record_args{
            L"--profile", L"pmu", L"--pmc-source", std::to_wstring(source),
            L"--duration", std::to_wstring(duration_ms),
            L"--output", trace_path.wstring(),
        };
        if (pid.has_value()) {
            record_args.push_back(L"--pid");
            record_args.push_back(std::to_wstring(*pid));
        } else {
            record_args.push_back(L"--");
            record_args.insert(record_args.end(), target.begin(), target.end());
        }
        uint32_t pmu_target_pid = 0;
        const int record_result = RunRecord(record_args, &pmu_target_pid);
        if (record_result != 0) {
            std::error_code ignored;
            std::filesystem::remove(trace_path, ignored);
            return record_result;
        }

        TraceData trace;
        if (!ReadEtwFile(trace_path, trace, &error)) {
            PrintError(error);
            std::error_code ignored;
            std::filesystem::remove(trace_path, ignored);
            return 6;
        }
        Symbolizer symbolizer;
        if (!symbolizer.Initialize({}, &error)) { PrintError(error); return 6; }
        for (const auto& image : trace.images) {
            symbolizer.AddModule(image);
        }
        std::wstring source_name;
        for (const auto& candidate : sources) {
            if (candidate.source == source) {
                source_name = candidate.description;
                break;
            }
        }
        const uint64_t pmc_samples = static_cast<uint64_t>(std::count_if(
            trace.samples.begin(), trace.samples.end(), [&](const SampleRecord& sample) {
                return sample.kind == SampleKind::Pmc && sample.profile_source == source &&
                       (pmu_target_pid == 0 || sample.pid == pmu_target_pid);
            }));
        std::wcout << L"PMU profile (sample-derived semantics)\n  source: " << source
                   << L" (" << source_name << L")  target_pid: " << pmu_target_pid
                   << L"  samples: " << pmc_samples << L"\n";
        if (pmc_samples == 0) {
            PrintError(L"requested PMC source produced no target samples; unavailable");
            std::error_code ignored;
            std::filesystem::remove(trace_path, ignored);
            return 5;
        }
        std::wcout << L"  semantics: event sampling; no exact counter total or cross-CPU delta\n";
        CpuReportOptions report_options;
        report_options.percent_limit = 1.0;
        if (pmu_target_pid != 0) report_options.pid = pmu_target_pid;
        report_options.profile_source = source;
        WriteCpuReport(trace, symbolizer, report_options, std::wcout);
        std::error_code ignored;
        std::filesystem::remove(trace_path, ignored);
        return 0;
    }

    ProcessLaunchResult launched;
    HANDLE process = nullptr;
    uint32_t target_pid = pid.value_or(0);
    std::wstring error;
    if (!target.empty()) {
        if (!LaunchTargetProcess(target, launched, &error)) {
            PrintError(error);
            return 8;
        }
        process = launched.process;
        target_pid = launched.process_id;
    } else {
        process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
                                  PROCESS_VM_READ,
                              FALSE, target_pid);
        if (process == nullptr) {
            PrintError(L"cannot open pid " + std::to_wstring(target_pid));
            return 8;
        }
    }

    const std::wstring image_name = ProcessImageName(process);
    ProcessStats before;
    ProcessStats after;
    ReadProcessStats(process, before);
    const ULONGLONG start_tick = GetTickCount64();
    DWORD target_exit = 0;
    const DWORD wait = WaitForSingleObject(process, static_cast<DWORD>(duration_ms));
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(process, &target_exit);
    }
    const uint64_t elapsed_ms = GetTickCount64() - start_tick;
    ReadProcessStats(process, after);

    const uint64_t user_delta = Delta(after.user_100ns, before.user_100ns);
    const uint64_t kernel_delta = Delta(after.kernel_100ns, before.kernel_100ns);
    const uint64_t cycles_delta = Delta(after.cycles, before.cycles);
    std::wcout << L"Process: "
               << (image_name.empty() ? std::to_wstring(target_pid) : image_name)
               << L" (" << target_pid << L")\n";
    std::wcout << L"  elapsed:   " << elapsed_ms << L" ms\n";
    std::wcout << L"  user:      " << FormatMilliseconds(user_delta) << L"\n";
    std::wcout << L"  sys:       " << FormatMilliseconds(kernel_delta) << L"\n";
    std::wcout << L"  cycles:    "
               << (after.cycles_valid ? std::to_wstring(cycles_delta) : L"n/a")
               << L"\n";
    std::wcout << L"  pagefault: " << Delta(after.page_faults, before.page_faults)
               << L"\n";
    std::wcout << L"  io-read:   "
               << Delta(after.read_operations, before.read_operations)
               << L" ops / " << Delta(after.read_bytes, before.read_bytes) << L" bytes\n";
    std::wcout << L"  io-write:  "
               << Delta(after.write_operations, before.write_operations)
               << L" ops / " << Delta(after.write_bytes, before.write_bytes) << L" bytes\n";
    if (per_thread && wait != WAIT_OBJECT_0) {
        PrintThreadStats(target_pid);
    }

    CloseHandle(process);
    if (target_exit != 0) {
        return static_cast<int>(target_exit);
    }
    return 0;
}
int RunCommandLine(const std::vector<std::wstring>& arguments) {
    if (arguments.empty()) {
        PrintGeneralHelp();
        return 0;
    }

    const std::wstring command = arguments[0];
    std::vector<std::wstring> rest(arguments.begin() + 1, arguments.end());
    if (command == L"--version") {
        PrintVersion();
        return 0;
    }
    if (command == L"version") {
        if (!rest.empty()) {
            if (rest.size() == 1 && (rest[0] == L"--help" || rest[0] == L"-h")) {
                PrintCommandHelp(L"version");
                return 0;
            }
            PrintError(L"version does not accept arguments");
            return 2;
        }
        PrintVersion();
        return 0;
    }
    if (command == L"--help" || command == L"-h") {
        if (!rest.empty()) {
            if (!PrintCommandHelp(rest.front())) {
                PrintError(L"unknown command: " + rest.front());
                return 2;
            }
        } else {
            PrintGeneralHelp();
        }
        return 0;
    }
    if (command == L"help") {
        if (rest.empty()) {
            PrintGeneralHelp();
            return 0;
        }
        if (rest.size() != 1) {
            PrintError(L"help accepts at most one command name");
            return 2;
        }
        if (!PrintCommandHelp(rest.front())) {
            PrintError(L"unknown command: " + rest.front());
            return 2;
        }
        return 0;
    }
    if (command == L"list") {
        return RunList(rest);
    }
    if (command == L"doctor") {
        return RunDoctor(rest);
    }
    if (command == L"test") {
        if (!rest.empty()) {
            if (rest.size() == 1 && (rest[0] == L"--help" || rest[0] == L"-h")) {
                PrintCommandHelp(L"test");
                return 0;
            }
            PrintError(L"test does not accept arguments");
            return 2;
        }
        return RunTest();
    }
    if (command == L"recover") {
        return RunRecover(rest);
    }
    if (command == L"record") {
        return RunRecord(rest);
    }
    if (command == L"report") {
        return RunReport(rest);
    }
    if (command == L"trace") {
        return RunTrace(rest);
    }
    if (command == L"sched") {
        return RunSched(rest);
    }
    if (command == L"stat") {
        return RunStat(rest);
    }
    if (command == L"top") {
        return RunTop(rest);
    }
    if (command == L"script") {
        return RunScript(rest);
    }
    if (command == L"annotate") {
        return RunAnnotate(rest);
    }
    if (command == L"diff") {
        return RunDiff(rest);
    }
    if (command == L"archive") {
        return RunArchive(rest);
    }
    if (command == L"buildid-list") {
        return RunBuildIdList(rest);
    }
    if (command == L"--internal-live-worker") {
        return RunLiveWorker(rest);
    }
    if (command == L"--internal-elevated-worker") {
        return RunWorker(rest);
    }
    if (command == L"--internal-session-guard") {
        return RunGuard(rest);
    }
    if(command==L"--internal-stop-timeout-test") {
        if(GetEnvironmentVariableW(L"WPERF_ENABLE_TEST_FAULTS",nullptr,0)==0)return 2;
        WorkerStopWatchdog timeout(200);
        Sleep(INFINITE);
        return 9;
    }
    if (command == L"--internal-test-crash") {
        InstallCrashHandler(GetCrashesDirectory(), NewId());
        RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return 9;
    }

    PrintError(L"unknown command: " + command);
    PrintGeneralHelp();
    return 2;
}

}  // namespace wperf
