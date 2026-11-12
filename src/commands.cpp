#include "commands.h"

#include "common.h"
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
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace wperf {
namespace {

std::atomic_bool g_interrupt_requested{false};

BOOL WINAPI ConsoleControlHandler(DWORD control_type) {
    if (control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT ||
        control_type == CTRL_CLOSE_EVENT || control_type == CTRL_LOGOFF_EVENT ||
        control_type == CTRL_SHUTDOWN_EVENT) {
        g_interrupt_requested.store(true);
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
    uint64_t duration_ms = 5000;
    uint32_t profile_interval = 10000;
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
        } else if (argument == L"--profile") {
            if (index + 1 >= arguments.size() || arguments[++index] != L"cpu") {
                error = L"M2 only supports --profile cpu";
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
        L"--profile-interval", std::to_wstring(options.profile_interval_100ns),
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
