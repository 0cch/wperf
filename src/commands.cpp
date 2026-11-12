#include "commands.h"

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

std::wstring OptionValue(const std::vector<std::wstring>& arguments,
                         const std::wstring& name,
                         const std::wstring& fallback = {}) {
    for (size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == name) {
            return arguments[index + 1];
        }
    }
    return fallback;
}

bool HasFlag(const std::vector<std::wstring>& arguments,
             const std::wstring& flag) {
    for (const auto& argument : arguments) {
        if (argument == flag) {
            return true;
        }
    }
    return false;
}

bool ParseUnsigned(const std::wstring& value, uint64_t& result) {
    if (value.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long long parsed = wcstoull(value.c_str(), &end, 10);
    if (end == nullptr || *end != L'\0') {
        return false;
    }
    result = parsed;
    return true;
}

bool ParseUint32(const std::wstring& value, uint32_t& result) {
    uint64_t parsed = 0;
    if (!ParseUnsigned(value, parsed) || parsed > UINT32_MAX) {
        return false;
    }
    result = static_cast<uint32_t>(parsed);
    return true;
}

void PrintError(const std::wstring& message) {
    std::wcerr << L"wperf: " << message << L"\n";
}

void PrintVersion() {
    std::wcout << L"wperf " << kVersion << L"\n";
}

std::wstring AfterSpace(const std::wstring& value) {
    const size_t space = value.find(L' ');
    return space == std::wstring::npos ? std::wstring{} : value.substr(space + 1);
}

bool StartsWith(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

bool IsStale(const RunState& state) {
    if (state.phase == L"FINISHED") {
        return false;
    }
    return state.worker_pid == 0 || !IsProcessRunning(state.worker_pid);
}

int RunList() {
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

    const std::wstring bundle = OptionValue(arguments, L"--bundle");
    if (!bundle.empty()) {
        const std::filesystem::path destination(bundle);
        if (!EnsureDirectory(destination, &error)) {
            PrintError(error);
            return 9;
        }
        std::ofstream doctor_file(destination / L"doctor.txt", std::ios::binary);
        doctor_file << ToUtf8(text);
        doctor_file.close();

        std::error_code ec;
        const std::filesystem::path directories[] = {
            GetSessionsDirectory(), GetLogsDirectory(), GetCrashesDirectory()};
        for (const auto& directory : directories) {
            if (!std::filesystem::exists(directory, ec)) {
                continue;
            }
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
                if (!ec && entry.is_regular_file()) {
                    std::filesystem::copy_file(
                        entry.path(), destination / entry.path().filename(),
                        std::filesystem::copy_options::overwrite_existing, ec);
                }
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
    bool elevated = false;
    std::wstring error;
    if (!IsProcessElevated(elevated, &error) || !elevated) {
        PrintError(L"recover requires an elevated process");
        return 3;
    }

    std::vector<std::wstring> sessions;
    const std::wstring explicit_session = OptionValue(arguments, L"--session");
    if (!explicit_session.empty()) {
        sessions.push_back(explicit_session);
    } else if (HasFlag(arguments, L"--all-stale")) {
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
    if (!pipe.SendLine(L"HELLO " + nonce + L" " +
                           std::to_wstring(GetCurrentProcessId()), &error)) {
        logger.Log(LogLevel::Error, L"ELEVATING", error);
        return 9;
    }

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
    trace_options.enable_lbr = lbr_text == L"1";
    if (!session.Start(trace_options, &error)) {
        logger.Log(LogLevel::Error, L"WORKER_READY", error);
        state.phase = L"INTERNAL_ERROR";
        state.error_code = 4;
        state_store.Write(state, nullptr);
        pipe.SendLine(L"ERROR 4 " + error, nullptr);
        pipe.SendLine(L"FINISHED 4", nullptr);
        return 4;
    }

    state.phase = L"CAPTURING";
    state_store.Write(state, nullptr);
    pipe.SendLine(L"PHASE CAPTURING", nullptr);
    pipe.SendLine(L"READY " + session_name, nullptr);
    logger.Log(LogLevel::Info, L"CAPTURING", L"ETW session started");

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

    std::vector<std::wstring> guard_arguments{
        L"--internal-session-guard",
        L"--session", session_name,
        L"--event", clean_event_name,
        L"--worker-pid", std::to_wstring(GetCurrentProcessId()),
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

    const ULONGLONG deadline = duration_ms == 0 ? 0 : GetTickCount64() + duration_ms;
    while (deadline == 0 || GetTickCount64() < deadline) {
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
    int exit_code = session.Stop(&error) ? 0 : 4;
    SetEvent(clean_event);
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
        for (const auto& candidate : StateStore::ReadAll(GetSessionsDirectory())) {
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
    bool lbr = false;
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
        } else if (argument == L"--lbr" || argument == L"-b") {
            options.lbr = true;
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
        } else if (argument == L"--freq" || argument == L"-F") {
            uint64_t frequency = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], frequency) || frequency == 0 ||
                frequency > 100000) {
                error = L"--freq requires HZ between 1 and 100000";
                return false;
            }
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
            std::wcout << L"Usage: wperf record [--elevate] [-F HZ] [-p PID] "
                          L"[--duration MS] [-o FILE] [--] command\n";
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
    return true;
}
int RunRecord(const std::vector<std::wstring>& arguments) {
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
    if (!elevated && !options.elevate) {
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
        L"--pmc-source-set", options.pmc_source_set ? L"1" : L"0",
        L"--lbr", options.lbr ? L"1" : L"0",
        L"--duration-ms", std::to_wstring(options.duration_ms),
    };

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
                return 2;
            }
            pid = value;
        } else if (arguments[index] == L"--duration") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], duration_ms) || duration_ms == 0) {
                return 2;
            }
        } else if (arguments[index] == L"--per-thread") {
            per_thread = true;
        } else if (arguments[index] == L"--pmu") {
            pmu = true;
        } else if (arguments[index] == L"-e" || arguments[index] == L"--event" ||
                   arguments[index] == L"--pmc-source") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            pmu_event = arguments[++index];
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf stat [-p PID] [--duration MS] "
                          L"[--per-thread] [--pmu -e SOURCE] [--] command\n";
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
            L"-F", L"999", L"--duration", std::to_wstring(duration_ms),
            L"--output", trace_path.wstring(),
        };
        if (pid.has_value()) {
            record_args.push_back(L"--pid");
            record_args.push_back(std::to_wstring(*pid));
        } else {
            record_args.push_back(L"--");
            record_args.insert(record_args.end(), target.begin(), target.end());
        }
        const int record_result = RunRecord(record_args);
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
        symbolizer.Initialize({}, nullptr);
        for (const auto& image : trace.images) {
            symbolizer.AddModule(image);
        }
        std::wcout << L"PMU profile (experimental, not an exact counter total)\n";
        std::wcout << L"  source: " << source << L"  samples: "
                   << trace.samples.size() << L"\n";
        CpuReportOptions report_options;
        report_options.percent_limit = 1.0;
        if (pid.has_value()) {
            report_options.pid = pid;
        }
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
int RunTop(const std::vector<std::wstring>& arguments) {
    std::optional<uint32_t> pid;
    uint64_t interval_ms = 1000;
    uint64_t iterations = 0;
    bool elevate = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-p" || arguments[index] == L"--pid") {
            uint32_t value = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], value)) {
                return 2;
            }
            pid = value;
        } else if (arguments[index] == L"--interval") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], interval_ms) || interval_ms == 0) {
                return 2;
            }
        } else if (arguments[index] == L"--iterations") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], iterations)) {
                return 2;
            }
        } else if (arguments[index] == L"--elevate") {
            elevate = true;
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf top [-p PID] [--interval MS] "
                          L"[--iterations N]\n";
            return 0;
        } else {
            PrintError(L"unknown top option: " + arguments[index]);
            return 2;
        }
    }

    bool elevated = false;
    std::wstring elevation_error;
    IsProcessElevated(elevated, &elevation_error);
    if (!elevated) {
        PrintError(L"top requires an elevated terminal; use --elevate only for a "
                   L"single snapshot (UAC would repeat per interval)");
        return 3;
    }

    g_top_mode.store(true);
    g_top_interrupted.store(false);
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
    uint64_t iteration = 0;
    while (!g_top_interrupted.load() && (iterations == 0 || iteration < iterations)) {
        ++iteration;
        const std::wstring id = NewId();
        const std::filesystem::path trace_path =
            GetWperfRootDirectory() / (L"top-" + id + L".etl");
        std::vector<std::wstring> record_args{
            L"-F", L"999", L"--duration", std::to_wstring(interval_ms),
            L"--output", trace_path.wstring(), L"--profile", L"cpu",
        };
        if (pid.has_value()) {
            record_args.push_back(L"--pid");
            record_args.push_back(std::to_wstring(*pid));
        }

        if (elevate) {
            record_args.push_back(L"--elevate");
        }

        const int record_result = RunRecord(record_args);
        if (record_result != 0) {
            PrintError(L"top capture failed with code " + std::to_wstring(record_result));
            break;
        }

        TraceData trace;
        std::wstring error;
        if (!ReadEtwFile(trace_path, trace, &error)) {
            PrintError(error);
            std::error_code ignored;
            std::filesystem::remove(trace_path, ignored);
            break;
        }

        Symbolizer symbolizer;
        symbolizer.Initialize({}, nullptr);
        for (const auto& image : trace.images) {
            symbolizer.AddModule(image);
        }
        CpuReportOptions report_options;
        report_options.percent_limit = 1.0;
        if (pid.has_value()) {
            report_options.pid = pid;
            report_options.comm_filter.clear();
        }

        std::wcout << L"\x1b[2J\x1b[H";
        std::wcout << L"wperf top  interval=" << interval_ms
                   << L"ms  sample=" << iteration << L"\n";
        WriteCpuReport(trace, symbolizer, report_options, std::wcout);
        std::wcout.flush();

        std::error_code ignored;
        std::filesystem::remove(trace_path, ignored);
        SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
    }
    g_top_mode.store(false);
    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    return 0;
}

int RunScript(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::optional<uint32_t> pid;
    std::wstring fields = L"timestamp,cpu,pid,tid,ip,comm,dso,symbol";
    std::wstring symbol_path;
    uint64_t limit = 0;
    bool csv = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-i" || arguments[index] == L"--input") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            input = arguments[++index];
        } else if (arguments[index] == L"-p" || arguments[index] == L"--pid") {
            uint32_t value = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], value)) {
                return 2;
            }
            pid = value;
        } else if (arguments[index] == L"--fields") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            fields = arguments[++index];
        } else if (arguments[index] == L"--limit") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], limit)) {
                return 2;
            }
        } else if (arguments[index] == L"--symbol-path") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            symbol_path = arguments[++index];
        } else if (arguments[index] == L"--csv") {
            csv = true;
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf script -i FILE [-p PID] [--fields LIST] "
                          L"[--limit N] [--csv]\n";
            return 0;
        } else {
            PrintError(L"unknown script option: " + arguments[index]);
            return 2;
        }
    }
    if (input.empty()) {
        PrintError(L"script requires -i FILE");
        return 2;
    }

    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }
    Symbolizer symbolizer;
    symbolizer.Initialize({}, nullptr);
    for (const auto& image : trace.images) {
        symbolizer.AddModule(image);
    }

    std::wcout << fields << L"\n";
    uint64_t emitted = 0;
    for (const auto& sample : trace.samples) {
        if (pid.has_value() && sample.pid != *pid) {
            continue;
        }
        if (limit != 0 && emitted++ >= limit) {
            break;
        }
        const ResolvedSymbol leaf = symbolizer.Resolve(
            trace, sample.pid, sample.instruction_pointer, sample.timestamp_ns);
        const std::wstring comm = ProcessDisplayName(trace, sample.pid);
        std::wostringstream row;
        size_t start = 0;
        bool first = true;
        while (start < fields.size()) {
            const size_t comma = fields.find(L',', start);
            const std::wstring field = fields.substr(
                start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
            if (!first) {
                row << (csv ? L"," : L" ");
            }
            first = false;
            if (field == L"timestamp") {
                row << sample.timestamp_ns;
            } else if (field == L"cpu") {
                row << sample.cpu;
            } else if (field == L"pid") {
                row << sample.pid;
            } else if (field == L"tid") {
                row << sample.tid;
            } else if (field == L"ip") {
                row << std::hex << sample.instruction_pointer << std::dec;
            } else if (field == L"comm") {
                row << comm;
            } else if (field == L"dso") {
                row << leaf.module;
            } else if (field == L"symbol") {
                row << leaf.symbol;
            } else if (field == L"stack") {
                for (size_t frame = 0; frame < sample.stack.size(); ++frame) {
                    if (frame != 0) {
                        row << L";";
                    }
                    row << std::hex << sample.stack[frame] << std::dec;
                }
            } else {
                row << L"";
            }
            if (comma == std::wstring::npos) {
                break;
            }
            start = comma + 1;
        }
        std::wcout << row.str() << L"\n";
    }
    return 0;
}
std::map<std::wstring, uint64_t> AggregateSymbols(const TraceData& trace,
                                                  Symbolizer& symbolizer,
                                                  std::optional<uint32_t> pid) {
    std::map<std::wstring, uint64_t> result;
    for (const auto& sample : trace.samples) {
        if (pid.has_value() && sample.pid != *pid) {
            continue;
        }
        const uint64_t address = sample.stack.empty() ? sample.instruction_pointer
                                                      : sample.stack.front();
        const ResolvedSymbol symbol =
            symbolizer.Resolve(trace, sample.pid, address, sample.timestamp_ns);
        const std::wstring key = symbol.module + L"|" + symbol.symbol;
        ++result[key];
    }
    return result;
}

int RunDiff(const std::vector<std::wstring>& arguments) {
    std::filesystem::path first;
    std::filesystem::path second;
    std::wstring symbol_path;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-1" || arguments[index] == L"--first") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            first = arguments[++index];
        } else if (arguments[index] == L"-2" || arguments[index] == L"--second") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            second = arguments[++index];
        } else if (arguments[index] == L"--symbol-path") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            symbol_path = arguments[++index];
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf diff -1 FIRST.etl -2 SECOND.etl "
                          L"[--symbol-path DIR]\n";
            return 0;
        }
    }
    if (first.empty() || second.empty()) {
        PrintError(L"diff requires -1 FILE and -2 FILE");
        return 2;
    }

    TraceData first_trace;
    TraceData second_trace;
    std::wstring error;
    if (!ReadEtwFile(first, first_trace, &error) ||
        !ReadEtwFile(second, second_trace, &error)) {
        PrintError(error);
        return 6;
    }
    Symbolizer first_symbolizer;
    Symbolizer second_symbolizer;
    first_symbolizer.Initialize(symbol_path, nullptr);
    second_symbolizer.Initialize(symbol_path, nullptr);
    for (const auto& image : first_trace.images) {
        first_symbolizer.AddModule(image);
    }
    for (const auto& image : second_trace.images) {
        second_symbolizer.AddModule(image);
    }
    const auto before = AggregateSymbols(first_trace, first_symbolizer, std::nullopt);
    const auto after = AggregateSymbols(second_trace, second_symbolizer, std::nullopt);

    std::vector<std::wstring> keys;
    for (const auto& [key, value] : before) {
        (void)value;
        keys.push_back(key);
    }
    for (const auto& [key, value] : after) {
        (void)value;
        if (before.find(key) == before.end()) {
            keys.push_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());
    std::wcout << L"Delta Samples Symbol\n";
    for (const auto& key : keys) {
        const uint64_t old_count = before.count(key) ? before.at(key) : 0;
        const uint64_t new_count = after.count(key) ? after.at(key) : 0;
        if (old_count == new_count) {
            continue;
        }
        std::wcout << (new_count >= old_count ? L"+ " : L"- ")
                   << (new_count >= old_count ? new_count - old_count
                                              : old_count - new_count)
                   << L"  " << old_count << L" -> " << new_count << L"  "
                   << key << L"\n";
    }
    return 0;
}

std::filesystem::path NormalizeArchivePath(const std::wstring& value) {
    std::wstring path = value;
    if (path.rfind(L"\\SystemRoot\\", 0) == 0) {
        wchar_t system_root[32768]{};
        const DWORD length = GetEnvironmentVariableW(
            L"SystemRoot", system_root, static_cast<DWORD>(std::size(system_root)));
        if (length != 0) {
            path = std::wstring(system_root, length) + path.substr(11);
        }
    }
    if (path.rfind(L"\\??\\", 0) == 0) {
        path.erase(0, 4);
    }
    if (path.rfind(L"\\Device\\", 0) == 0) {
        for (wchar_t drive = L'C'; drive <= L'Z'; ++drive) {
            const std::wstring drive_name{drive, L':'};
            wchar_t device[1024]{};
            if (QueryDosDeviceW(drive_name.c_str(), device,
                                static_cast<DWORD>(std::size(device))) != 0) {
                const std::wstring prefix = std::wstring(device) + L"\\";
                if (path.rfind(prefix, 0) == 0) {
                    path = drive_name + path.substr(prefix.size() - 1);
                    break;
                }
            }
        }
    }
    return path;
}

int RunAnnotate(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::wstring symbol_path;
    std::optional<uint32_t> pid;
    uint64_t limit = 50;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-i" || arguments[index] == L"--input") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            input = arguments[++index];
        } else if (arguments[index] == L"--symbol-path") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            symbol_path = arguments[++index];
        } else if (arguments[index] == L"-p" || arguments[index] == L"--pid") {
            uint32_t value = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], value)) {
                return 2;
            }
            pid = value;
        } else if (arguments[index] == L"--limit") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], limit)) {
                return 2;
            }
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf annotate -i FILE [--symbol-path DIR] "
                          L"[--pid PID] [--limit N]\n";
            return 0;
        }
    }
    if (input.empty()) {
        PrintError(L"annotate requires -i FILE");
        return 2;
    }
    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }
    Symbolizer symbolizer;
    symbolizer.Initialize(symbol_path, nullptr);
    for (const auto& image : trace.images) {
        symbolizer.AddModule(image);
    }
    struct Heat {
        std::wstring symbol;
        uint64_t samples = 0;
    };
    std::map<std::wstring, Heat> heat;
    for (const auto& sample : trace.samples) {
        if (pid.has_value() && sample.pid != *pid) {
            continue;
        }
        const ResolvedSymbol symbol = symbolizer.Resolve(
            trace, sample.pid, sample.instruction_pointer, sample.timestamp_ns);
        const std::wstring key = symbol.module + L"!" + symbol.symbol + L"+0x" +
                                 std::to_wstring(symbol.displacement);
        auto& value = heat[key];
        value.symbol = symbol.symbol;
        ++value.samples;
    }
    std::vector<Heat> rows;
    for (auto& [key, value] : heat) {
        (void)key;
        rows.push_back(value);
    }
    std::sort(rows.begin(), rows.end(), [](const Heat& a, const Heat& b) {
        return a.samples > b.samples;
    });
    std::wcout << L"Samples  Address heat\n";
    for (size_t index = 0; index < rows.size() && index < limit; ++index) {
        std::wcout << rows[index].samples << L"  " << rows[index].symbol << L"\n";
    }
    return 0;
}

int RunArchive(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::filesystem::path output;
    std::optional<uint32_t> pid;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-i" || arguments[index] == L"--input") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            input = arguments[++index];
        } else if (arguments[index] == L"-o" || arguments[index] == L"--output") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            output = arguments[++index];
        } else if (arguments[index] == L"-p" || arguments[index] == L"--pid") {
            uint32_t value = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], value)) {
                return 2;
            }
            pid = value;
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf archive -i FILE -o DIRECTORY [-p PID]\n";
            return 0;
        }
    }
    if (input.empty() || output.empty()) {
        PrintError(L"archive requires -i FILE and -o DIRECTORY");
        return 2;
    }
    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }
    if (!EnsureDirectory(output, &error)) {
        PrintError(error);
        return 9;
    }
    std::set<std::wstring> needed_images;
    for (const auto& sample : trace.samples) {
        if (pid.has_value() && sample.pid != *pid) {
            continue;
        }
        if (const auto* image = FindImage(trace, sample.pid,
                                          sample.instruction_pointer,
                                          sample.timestamp_ns)) {
            if (!image->path.empty()) {
                needed_images.insert(image->path);
            }
        }
        for (uint64_t frame : sample.stack) {
            if (const auto* image =
                    FindImage(trace, sample.pid, frame, sample.timestamp_ns)) {
                if (!image->path.empty()) {
                    needed_images.insert(image->path);
                }
            }
        }
    }

    std::wofstream manifest(output / L"manifest.txt");
    for (const auto& path : needed_images) {
        const auto* image = FindImage(
            trace, pid.value_or(0), 0, 0);
        (void)image;
        const std::filesystem::path source = NormalizeArchivePath(path);
        manifest << path << L"\n";
        std::error_code ec;
        if (std::filesystem::exists(source, ec) &&
            std::filesystem::is_regular_file(source, ec)) {
            const std::filesystem::path destination =
                output / source.filename().wstring();
            std::filesystem::copy_file(source, destination,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            const std::filesystem::path pdb = source.parent_path() /
                                              (source.stem().wstring() + L".pdb");
            if (std::filesystem::exists(pdb, ec) &&
                std::filesystem::is_regular_file(pdb, ec)) {
                std::filesystem::copy_file(
                    pdb, output / pdb.filename(),
                    std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
    }
    manifest.close();
    std::wcout << L"archive: " << output.wstring() << L"\n";
    return 0;
}
bool MatchesEventFilter(const TraceEvent& event, const std::wstring& filter) {
    if (filter.empty() || filter == L"all") {
        return true;
    }
    const std::wstring kind = TraceEventKindName(event.kind);
    if (filter == L"sched") {
        return event.kind == TraceEventKind::ContextSwitch ||
               event.kind == TraceEventKind::ReadyThread;
    }
    if (filter == L"syscall") {
        return event.kind == TraceEventKind::SyscallEnter ||
               event.kind == TraceEventKind::SyscallExit;
    }
    if (filter == L"pagefault" || filter == L"vm") {
        return event.kind == TraceEventKind::PageFault;
    }
    if (filter == L"io") {
        return event.kind == TraceEventKind::DiskIo ||
               event.kind == TraceEventKind::FileIo;
    }
    return kind == filter;
}

int RunTrace(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::wstring filter = L"all";
    std::wstring pid_text;
    std::wstring max_text = L"500";
    bool json = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-i" || arguments[index] == L"--input") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            input = arguments[++index];
        } else if (arguments[index] == L"-e" || arguments[index] == L"--event") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            filter = arguments[++index];
        } else if (arguments[index] == L"-p" || arguments[index] == L"--pid") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            pid_text = arguments[++index];
        } else if (arguments[index] == L"--max") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            max_text = arguments[++index];
        } else if (arguments[index] == L"--json") {
            json = true;
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf trace -i FILE [-e sched|syscall|pagefault|io] "
                          L"[-p PID] [--max N] [--json]\n";
            return 0;
        } else {
            PrintError(L"unknown trace option: " + arguments[index]);
            return 2;
        }
    }
    if (input.empty()) {
        PrintError(L"trace requires -i FILE for the offline M5 path");
        return 2;
    }

    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }
    std::optional<uint32_t> pid;
    if (!pid_text.empty()) {
        uint32_t value = 0;
        if (!ParseUint32(pid_text, value)) {
            return 2;
        }
        pid = value;
    }
    uint64_t max_events = 500;
    ParseUnsigned(max_text, max_events);

    uint64_t first_timestamp = UINT64_MAX;
    size_t emitted = 0;
    if (json) {
        std::wcout << L"[\n";
    } else {
        std::wcout << L"timestamp_ns cpu pid tid event values comm\n";
    }
    bool first_json = true;
    for (const auto& event : trace.events) {
        uint32_t display_pid = event.pid;
        uint32_t display_tid = event.tid;
        if ((display_pid == 0 || display_pid == UINT32_MAX) &&
            (event.kind == TraceEventKind::ContextSwitch ||
             event.kind == TraceEventKind::ReadyThread)) {
            display_tid = static_cast<uint32_t>(event.value1);
        }
        if (display_pid == 0 || display_pid == UINT32_MAX) {
            if (const auto* thread =
                    FindThread(trace, display_tid, event.timestamp_ns)) {
                display_pid = thread->pid;
            }
        }
        if (pid.has_value() && display_pid != *pid) {
            continue;
        }
        if (!MatchesEventFilter(event, filter)) {
            continue;
        }
        if (emitted++ >= max_events) {
            break;
        }
        if (first_timestamp == UINT64_MAX) {
            first_timestamp = event.timestamp_ns;
        }
        const uint64_t relative =
            event.timestamp_ns >= first_timestamp ? event.timestamp_ns - first_timestamp
                                                  : 0;
        const std::wstring comm = ProcessDisplayName(trace, display_pid);
        if (json) {
            if (!first_json) {
                std::wcout << L",\n";
            }
            first_json = false;
            std::wcout << L"{\"ts\":" << relative << L",\"cpu\":" << event.cpu
                       << L",\"pid\":" << display_pid << L",\"tid\":" << display_tid
                       << L",\"event\":\"" << TraceEventKindName(event.kind)
                       << L"\",\"value1\":" << event.value1
                       << L",\"value2\":" << event.value2
                       << L",\"comm\":\"" << comm << L"\"}";
        } else {
            std::wcout << relative << L" " << event.cpu << L" " << display_pid << L" "
                       << display_tid << L" " << TraceEventKindName(event.kind) << L" "
                       << event.value1 << L" " << event.value2 << L" " << comm << L"\n";
        }
    }
    if (json) {
        std::wcout << L"\n]\n";
    }
    return 0;
}

int RunSched(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-i" || arguments[index] == L"--input") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            input = arguments[++index];
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf sched -i FILE\n";
            return 0;
        }
    }
    if (input.empty()) {
        PrintError(L"sched requires -i FILE");
        return 2;
    }
    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }

    struct Counts {
        uint64_t context_switches = 0;
        uint64_t ready_threads = 0;
    };
    std::map<uint32_t, Counts> counts;
    for (const auto& event : trace.events) {
        uint32_t pid = event.pid;
        if ((pid == 0 || pid == UINT32_MAX) &&
            (event.kind == TraceEventKind::ContextSwitch ||
             event.kind == TraceEventKind::ReadyThread)) {
            if (const auto* thread = FindThread(
                    trace, static_cast<uint32_t>(event.value1), event.timestamp_ns)) {
                pid = thread->pid;
            }
        }
        if (event.kind == TraceEventKind::ContextSwitch) {
            ++counts[pid].context_switches;
        } else if (event.kind == TraceEventKind::ReadyThread) {
            ++counts[pid].ready_threads;
        }
    }
    std::wcout << L"Context switches / ready events by process\n";
    std::wcout << L"PID  Command  ContextSwitches  ReadyThreads\n";
    for (const auto& [pid, value] : counts) {
        std::wcout << pid << L"  " << ProcessDisplayName(trace, pid) << L"  "
                   << value.context_switches << L"  " << value.ready_threads << L"\n";
    }
    return 0;
}
int RunBuildIdList(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-i" || arguments[index] == L"--input") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            input = arguments[++index];
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf buildid-list -i FILE\n";
            return 0;
        }
    }
    if (input.empty()) {
        PrintError(L"buildid-list requires -i FILE");
        return 2;
    }

    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }
    for (const auto& image : trace.images) {
        if (image.path.empty()) {
            continue;
        }
        std::wcout << std::hex << std::setw(16) << std::setfill(L'0') << image.base
                   << L" " << std::setw(8) << image.size << std::dec
                   << std::setfill(L' ') << L" pid=" << image.pid
                   << L" timestamp=" << image.timestamp
                   << L" checksum=" << image.checksum << L" " << image.path << L"\n";
    }
    return 0;
}
int RunReport(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::wstring symbol_path;
    CpuReportOptions report_options;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"-i" || argument == L"--input") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--input requires a trace file");
                return 2;
            }
            input = arguments[++index];
        } else if (argument == L"--pid") {
            uint32_t pid = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], pid)) {
                PrintError(L"--pid requires a process id");
                return 2;
            }
            report_options.pid = pid;
        } else if (argument == L"--tid") {
            uint32_t tid = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], tid)) {
                PrintError(L"--tid requires a thread id");
                return 2;
            }
            report_options.tid = tid;
        } else if (argument == L"--comm") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            report_options.comm_filter = arguments[++index];
        } else if (argument == L"--dso") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            report_options.dso_filter = arguments[++index];
        } else if (argument == L"--symbol") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            report_options.symbol_filter = arguments[++index];
        } else if (argument == L"--symbol-path") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            symbol_path = arguments[++index];
        } else if (argument == L"--percent-limit") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            report_options.percent_limit = wcstod(arguments[++index].c_str(), nullptr);
        } else if (argument == L"--folded") {
            report_options.folded = true;
        } else if (argument == L"--call-graph") {
            report_options.call_tree = true;
            if (index + 1 < arguments.size() &&
                (arguments[index + 1] == L"caller" || arguments[index + 1] == L"callee" ||
                 arguments[index + 1] == L"both")) {
                ++index;
            }
        } else if (argument == L"--csv") {
            report_options.csv = true;
        } else if (argument == L"--json") {
            report_options.json = true;
        } else if (argument == L"--sort") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            const std::wstring value = arguments[++index];
            size_t start = 0;
            while (start < value.size()) {
                const size_t comma = value.find(L',', start);
                report_options.sort_fields.push_back(
                    value.substr(start, comma == std::wstring::npos
                                            ? std::wstring::npos
                                            : comma - start));
                if (comma == std::wstring::npos) {
                    break;
                }
                start = comma + 1;
            }
        } else if (argument == L"--fields") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            ++index;
        } else if (argument == L"--help" || argument == L"-h") {
            std::wcout << L"Usage: wperf report -i FILE [--pid PID] [--tid TID] "
                          L"[--comm TEXT] [--dso TEXT] [--symbol TEXT] "
                          L"[--percent-limit N] [--folded|--call-graph] [--csv|--json]\n";
            return 0;
        } else {
            PrintError(L"unknown report option: " + argument);
            return 2;
        }
    }

    if (input.empty()) {
        PrintError(L"report requires -i FILE");
        return 2;
    }
    if (input.is_relative()) {
        input = std::filesystem::absolute(input);
    }

    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }

    Symbolizer symbolizer;
    std::wstring symbol_error;
    const bool symbols_ready = symbolizer.Initialize(symbol_path, &symbol_error);
    if (!symbols_ready && !symbol_error.empty()) {
        std::wcerr << L"wperf: symbolization disabled: " << symbol_error << L"\n";
    }
    for (const auto& image : trace.images) {
        symbolizer.AddModule(image);
    }


    std::wcerr << L"events=" << trace.quality.events_total
               << L" unknown=" << trace.quality.unknown_events
               << L" samples=" << trace.quality.samples_total
               << L" stack_events=" << trace.quality.stack_events_total
               << L" samples_without_stack=" << trace.quality.samples_without_stack
               << L" lost=" << trace.quality.lost_events
               << L"\n";
    WriteCpuReport(trace, symbolizer, report_options, std::wcout);
    return trace.samples.empty() ? 6 : 0;
}
void PrintUsage() {
    std::wcout
        << L"wperf " << kVersion << L"\n\n"
        << L"Usage: wperf <command> [options]\n\n"
        << L"Commands:\n"
        << L"  list       List ETW profile sources and active sessions\n"
        << L"  doctor     Check environment and wperf state\n"
        << L"  test       Run a lightweight self-test\n"
        << L"  record     Capture a system ETW trace\n"
        << L"  report     Analyze an ETL trace\n"
        << L"  trace      Print scheduler/syscall/pagefault/IO events\n"
        << L"  sched      Summarize scheduler events\n"
        << L"  stat       Collect process time/cycle/I/O counters\n"
        << L"  top        Snapshot CPU hot functions per interval\n"
        << L"  script     Export samples from an ETL trace\n"
        << L"  annotate   Show sampled address heat\n"
        << L"  diff       Compare two CPU traces\n"
        << L"  archive    Copy module files and manifest\n"
        << L"  buildid-list List module identities from an ETL\n"
        << L"  recover    Stop a stale wperf ETW session\n"
        << L"  version    Print version\n\n"
        << L"Record: --elevate -F HZ -p PID --duration MS -o FILE -- command\n"
        << L"Report: -i FILE --pid PID --folded --call-graph --csv --json\n"
        << L"Doctor: --bundle DIR\n";
}

int RunCommandLine(const std::vector<std::wstring>& arguments) {
    if (arguments.empty()) {
        PrintUsage();
        return 0;
    }

    const std::wstring command = arguments[0];
    std::vector<std::wstring> rest(arguments.begin() + 1, arguments.end());
    if (command == L"--version" || command == L"version") {
        PrintVersion();
        return 0;
    }
    if (command == L"--help" || command == L"help" || command == L"-h") {
        PrintUsage();
        return 0;
    }
    if (command == L"list") {
        return RunList();
    }
    if (command == L"doctor") {
        return RunDoctor(rest);
    }
    if (command == L"test") {
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
    if (command == L"--internal-elevated-worker") {
        return RunWorker(rest);
    }
    if (command == L"--internal-session-guard") {
        return RunGuard(rest);
    }
    if (command == L"--internal-test-crash") {
        InstallCrashHandler(GetCrashesDirectory(), NewId());
        RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return 9;
    }

    PrintError(L"unknown command: " + command);
    PrintUsage();
    return 2;
}

}  // namespace wperf
