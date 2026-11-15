#include "commands.h"
#include "commands/live_control.h"

#include "commands/advanced.h"
#include "cli.h"
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
#include "output.h"
#include "json_writer.h"
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
#include <type_traits>
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

int RunList(const ListRequest& request) {
    if (request.providers) {
        std::wstring error;
        const auto providers = QueryProviders(&error);
        if (!error.empty()) {
            PrintError(error);
            return ExitStatus(ExitCode::CapabilityUnavailable);
        }
        if (request.json) {
            JsonStreamWriter writer(std::wcout);
            writer.BeginObject();
            writer.Key(L"provider_count");
            writer.Unsigned(providers.size());
            writer.Key(L"providers");
            writer.BeginArray();
            for (size_t index = 0; index < providers.size() && index < request.max; ++index) {
                nlohmann::ordered_json item;
                item["guid"] = ToUtf8(FormatGuid(providers[index].id));
                item["schema_source"] = providers[index].schema_source;
                item["name"] = ToUtf8(providers[index].name);
                writer.Item(item);
            }
            writer.EndArray();
            writer.EndObject();
            std::wcout << L"\n";
            return ExitStatus(ExitCode::Success);
        }
        std::wcout << L"ETW providers: " << providers.size() << L"\n";
        std::wcout << L"GUID                                  SchemaSource  Name\n";
        for (size_t index = 0; index < providers.size() && index < request.max; ++index) {
            std::wcout << FormatGuid(providers[index].id) << L"  " << std::setw(12)
                       << providers[index].schema_source << L"  " << providers[index].name << L"\n";
        }
        return ExitStatus(ExitCode::Success);
    }

    std::wstring error;
    const auto sources = QueryProfileSources(&error);
    if (!error.empty()) {
        PrintError(error);
        return ExitStatus(ExitCode::CapabilityUnavailable);
    }
    std::wstring session_error;
    const auto sessions = QueryActiveSessions(&session_error);

    if (request.json) {
        JsonStreamWriter writer(std::wcout);
        writer.BeginObject();
        writer.Key(L"profile_source_count");
        writer.Unsigned(sources.size());
        writer.Key(L"profile_sources");
        writer.BeginArray();
        for (const auto& source : sources) {
            nlohmann::ordered_json item;
            item["source"] = source.source;
            item["min_interval"] = source.min_interval;
            item["max_interval"] = source.max_interval;
            item["description"] = ToUtf8(source.description);
            writer.Item(item);
        }
        writer.EndArray();
        writer.Key(L"active_sessions");
        writer.BeginArray();
        for (const auto& session : sessions) writer.String(session);
        writer.EndArray();
        writer.Key(L"session_query_error");
        writer.String(session_error);
        writer.EndObject();
        std::wcout << L"\n";
        return ExitStatus(ExitCode::Success);
    }

    std::wcout << L"Profile sources:\n";
    std::wcout << L"  Source  MinInterval  MaxInterval  Description\n";
    for (const auto& source : sources) {
        std::wcout << L"  " << std::setw(6) << source.source << L"  "
                   << std::setw(11) << source.min_interval << L"  "
                   << std::setw(11) << source.max_interval << L"  "
                   << source.description << L"\n";
    }
    std::wcout << L"\nActive ETW sessions: " << sessions.size();
    if (!session_error.empty()) std::wcout << L" (" << session_error << L")";
    std::wcout << L"\n";
    for (const auto& session : sessions) std::wcout << L"  " << session << L"\n";
    return ExitStatus(ExitCode::Success);
}
int RunDoctor(const DoctorRequest& request) {
    bool elevated = false;
    std::wstring error;
    const bool elevation_ok = IsProcessElevated(elevated, &error);
    const auto sources = QueryProfileSources(&error);
    const uint32_t max_pmc = QueryMaxPmcCounters(&error);
    const auto sessions = QueryActiveSessions(&error);
    const auto states = StateStore::ReadAll(GetSessionsDirectory());
    std::wstring pmc_owner_error;
    const auto pmc_owners = QueryPmcCounterOwners(&pmc_owner_error);
    std::wstring pmc_session_error;
    const auto pmc_sessions = QueryPmcSessions(&pmc_session_error);

    if (request.json) {
        JsonStreamWriter writer(std::wcout);
        writer.BeginObject();
        writer.Key(L"version"); writer.String(kVersion);
        writer.Key(L"executable"); writer.String(GetExecutablePath());
        writer.Key(L"elevated");
        if (elevation_ok) writer.Bool(elevated); else writer.Null();
        writer.Key(L"state_root"); writer.String(GetWperfRootDirectory().wstring());
        writer.Key(L"profile_source_count"); writer.Unsigned(sources.size());
        writer.Key(L"max_pmc_counters"); writer.Unsigned(max_pmc);
        writer.Key(L"pmc_owner_count"); writer.Unsigned(pmc_owners.size());
        writer.Key(L"pmc_session_count"); writer.Unsigned(pmc_sessions.size());
        writer.Key(L"pmc_owner_error"); writer.String(pmc_owner_error);
        writer.Key(L"pmc_session_error"); writer.String(pmc_session_error);
        writer.Key(L"active_sessions");
        writer.BeginArray();
        for (const auto& session : sessions) writer.String(session);
        writer.EndArray();
        writer.Key(L"recorded_runs");
        writer.BeginArray();
        for (const auto& state : states) {
            nlohmann::ordered_json item;
            item["correlation_id"] = ToUtf8(state.correlation_id);
            item["phase"] = ToUtf8(state.phase);
            item["session"] = ToUtf8(state.session_name);
            item["worker"] = state.worker_pid;
            writer.Item(item);
        }
        writer.EndArray();
        writer.EndObject();
        std::wcout << L"\n";
        return ExitStatus(ExitCode::Success);
    }

    std::wostringstream report;
    report << L"wperf " << kVersion << L"\n";
    report << L"Executable: " << GetExecutablePath() << L"\n";
    report << L"Elevated: " << (elevation_ok ? (elevated ? L"yes" : L"no")
                                              : L"unknown") << L"\n";
    report << L"State root: " << GetWperfRootDirectory().wstring() << L"\n";
    report << L"Profile sources: " << sources.size() << L"\n";
    report << L"Max PMC counters: " << max_pmc << L"\n";
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

    if (!request.bundle.empty()) {
        const std::filesystem::path destination(request.bundle);
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
                if (!request.include_dumps && entry.path().extension() == L".dmp") {
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

int RunRecover(const RecoverRequest& request) {
    if (!request.session.empty() && request.all_stale) {
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
    if (!request.session.empty()) {
        sessions.push_back(request.session);
    } else if (request.all_stale) {
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

struct WorkerRequest {
    std::wstring pipe_path;
    std::wstring nonce;
    std::wstring session_name;
    std::wstring output;
    std::wstring log_path;
    std::wstring guard_log;
    std::wstring state_path;
    std::wstring correlation_id;
    std::wstring profile = L"cpu";
    std::wstring provider;
    uint64_t duration_ms = 5000;
    uint32_t profile_interval = 10000;
    uint32_t pmc_source = 0;
    uint32_t pmc_interval = 0;
    uint32_t controller_pid = 0;
    bool pmc_source_set = false;
    bool lbr = false;
    bool context_registers = false;
    bool call_graph = true;
    bool stack_cache = false;
    std::vector<uint32_t> pmc_counter_sources;
};

bool ParseWorkerRequest(const std::vector<std::wstring>& arguments,
                        WorkerRequest& request) {
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        auto value = [&](std::wstring& target) {
            if (index + 1 >= arguments.size()) return false;
            target = arguments[++index];
            return true;
        };
        std::wstring text;
        if (argument == L"--pipe") { if (!value(request.pipe_path)) return false; }
        else if (argument == L"--nonce") { if (!value(request.nonce)) return false; }
        else if (argument == L"--session") { if (!value(request.session_name)) return false; }
        else if (argument == L"--output") { if (!value(request.output)) return false; }
        else if (argument == L"--log") { if (!value(request.log_path)) return false; }
        else if (argument == L"--guard-log") { if (!value(request.guard_log)) return false; }
        else if (argument == L"--state") { if (!value(request.state_path)) return false; }
        else if (argument == L"--correlation") { if (!value(request.correlation_id)) return false; }
        else if (argument == L"--profile") { if (!value(request.profile)) return false; }
        else if (argument == L"--provider") { if (!value(request.provider)) return false; }
        else if (argument == L"--stack-cache") { request.stack_cache = true; }
        else if (argument == L"--controller-pid") { if (!value(text) || !ParseUint32(text, request.controller_pid)) return false; }
        else if (argument == L"--duration-ms") { if (!value(text) || !ParseUnsigned(text, request.duration_ms)) return false; }
        else if (argument == L"--profile-interval") { if (!value(text) || !ParseUint32(text, request.profile_interval)) return false; }
        else if (argument == L"--pmc-source") { if (!value(text) || !ParseUint32(text, request.pmc_source)) return false; }
        else if (argument == L"--period") { if (!value(text) || !ParseUint32(text, request.pmc_interval)) return false; }
        else if (argument == L"--pmc-source-set") { if (!value(text)) return false; request.pmc_source_set = text == L"1"; }
        else if (argument == L"--lbr") { if (!value(text)) return false; request.lbr = text == L"1"; }
        else if (argument == L"--context-registers") { if (!value(text)) return false; request.context_registers = text == L"1"; }
        else if (argument == L"--call-graph") { if (!value(text)) return false; request.call_graph = text == L"1"; }
        else if (argument == L"--pmc-counters") {
            if (!value(text)) return false;
            std::wistringstream input(text);
            std::wstring token;
            while (std::getline(input, token, L',')) {
                uint32_t source = 0;
                if (!ParseUint32(token, source) || source == 0) return false;
                request.pmc_counter_sources.push_back(source);
            }
        } else {
            return false;
        }
    }
    return !request.pipe_path.empty() && !request.nonce.empty() &&
           !request.session_name.empty() && !request.output.empty() &&
           !request.log_path.empty() && !request.guard_log.empty() &&
           !request.state_path.empty() && !request.correlation_id.empty() &&
           request.controller_pid != 0;
}

struct GuardRequest {
    std::wstring session_name;
    std::wstring event_name;
    std::wstring log_path;
    std::wstring state_path;
    std::wstring correlation_id;
    uint32_t worker_pid = 0;
    uint32_t restore_source = 0;
    uint32_t restore_interval = 0;
    uint32_t configured_interval = 0;
    bool clear_pmc = false;
};

bool ParseGuardRequest(const std::vector<std::wstring>& arguments,
                       GuardRequest& request) {
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        auto value = [&](std::wstring& target) {
            if (index + 1 >= arguments.size()) return false;
            target = arguments[++index];
            return true;
        };
        std::wstring text;
        if (argument == L"--session") { if (!value(request.session_name)) return false; }
        else if (argument == L"--event") { if (!value(request.event_name)) return false; }
        else if (argument == L"--log") { if (!value(request.log_path)) return false; }
        else if (argument == L"--state") { if (!value(request.state_path)) return false; }
        else if (argument == L"--correlation") { if (!value(request.correlation_id)) return false; }
        else if (argument == L"--worker-pid") { if (!value(text) || !ParseUint32(text, request.worker_pid)) return false; }
        else if (argument == L"--restore-source") { if (!value(text) || !ParseUint32(text, request.restore_source)) return false; }
        else if (argument == L"--restore-interval") { if (!value(text) || !ParseUint32(text, request.restore_interval)) return false; }
        else if (argument == L"--configured-interval") { if (!value(text) || !ParseUint32(text, request.configured_interval)) return false; }
        else if (argument == L"--clear-pmc") { if (!value(text)) return false; request.clear_pmc = text == L"1"; }
        else return false;
    }
    return !request.session_name.empty() && !request.log_path.empty() &&
           !request.state_path.empty() && !request.correlation_id.empty();
}


int RunWorker(const std::vector<std::wstring>& arguments) {
    WorkerRequest internal;
    if (!ParseWorkerRequest(arguments, internal)) {
        PrintError(L"invalid internal worker arguments");
        return 2;
    }
    const std::wstring& pipe_path = internal.pipe_path;
    const std::wstring& nonce = internal.nonce;
    const std::wstring& session_name = internal.session_name;
    const std::wstring& output = internal.output;
    const std::wstring& log_path = internal.log_path;
    const std::wstring& guard_log = internal.guard_log;
    const std::wstring& state_path = internal.state_path;
    const std::wstring& correlation_id = internal.correlation_id;
    const std::wstring& profile_text = internal.profile;
    const std::wstring& provider_text = internal.provider;
    const uint64_t duration_ms = internal.duration_ms;
    const uint32_t profile_interval = internal.profile_interval;
    const uint32_t pmc_source = internal.pmc_source;
    const uint32_t expected_controller_pid = internal.controller_pid;

    InstallCrashHandler(GetCrashesDirectory(), correlation_id);
    Logger logger(log_path, correlation_id, L"worker");
    logger.Log(LogLevel::Info, L"ELEVATING", L"worker started");

    PipeClient pipe;
    std::wstring error;
    if (!pipe.Connect(pipe_path, 15000, &error)) {
        logger.Log(LogLevel::Error, L"ELEVATING", error);
        return 3;
    }
    uint32_t actual_controller_pid = 0;
    if (!pipe.ServerProcessId(actual_controller_pid) ||
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
    trace_options.pmc_profile = profile_text == L"pmu" || internal.pmc_source_set;
    trace_options.pmc_source = pmc_source;
    trace_options.pmc_interval = internal.pmc_interval;
    trace_options.enable_lbr = internal.lbr;
    trace_options.enable_context_registers = internal.context_registers;
    trace_options.stack_cache = internal.stack_cache;
    trace_options.pmc_counter_sources = internal.pmc_counter_sources;
    trace_options.enable_stack_tracing = internal.call_graph;
    trace_options.enable_scheduler_stack =
        internal.call_graph &&
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
    GuardRequest internal;
    if (!ParseGuardRequest(arguments, internal) || internal.worker_pid == 0) {
        return 2;
    }
    const std::wstring& session_name = internal.session_name;
    const std::wstring& event_name = internal.event_name;
    const std::wstring& log_path = internal.log_path;
    const std::wstring& state_path = internal.state_path;
    const std::wstring& correlation_id = internal.correlation_id;
    const uint32_t worker_pid = internal.worker_pid;

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
        restore.source = internal.restore_source;
        restore.original_interval = internal.restore_interval;
        restore.configured_interval = internal.configured_interval;
        restore.clear_pmc = internal.clear_pmc;
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

bool BuildRecordOptions(const RecordRequest& request, RecordOptions& options,
                        std::wstring& error) {
    options.output = request.output;
    options.requested_session = request.session;
    options.pid = request.pid;
    options.target = request.target;
    options.profile = request.profile.empty() ? L"cpu" : request.profile;
    options.pmc_source = request.pmc_source.value_or(0);
    options.pmc_source_set = request.pmc_source.has_value();
    if (request.period && *request.period > UINT32_MAX) { error = L"--period value is out of range"; return false; }
    options.pmc_interval = static_cast<uint32_t>(request.period.value_or(0));
    options.stack_cache = request.stack_cache;
    options.lbr = request.lbr;
    options.context_registers = request.context_registers;
    options.call_graph = request.call_graph;
    options.provider = request.provider;
    options.elevate = request.elevate;
    options.duration_ms = request.duration_ms.value_or(0);
    options.duration_explicit = request.duration_ms.has_value();
    if (options.duration_explicit && options.duration_ms == 0) {
        error = L"--duration requires a positive millisecond value";
        return false;
    }
    if (request.frequency) {
        if (*request.frequency == 0 || *request.frequency > 100000) {
            error = L"--freq requires HZ between 1 and 100000";
            return false;
        }
        options.frequency_explicit = true;
        options.profile_interval_100ns = static_cast<uint32_t>(
            std::max<uint64_t>(1, 10000000 / *request.frequency));
    }
    if (!request.event.empty()) {
        uint32_t numeric_source = 0;
        if (ParseUint32(request.event, numeric_source)) {
            options.pmc_source = numeric_source;
            options.pmc_source_set = true;
        } else if (request.event == L"cpu" || request.event == L"cpu-sched" ||
                   request.event == L"sched" || request.event == L"syscall" ||
                   request.event == L"vm" || request.event == L"io" ||
                   request.event == L"kernel" || request.event == L"all" ||
                   request.event == L"pmu") {
            options.profile = request.event;
        } else {
            std::wstring source_error;
            const auto sources = QueryProfileSources(&source_error);
            bool found = false;
            for (const auto& candidate : sources) {
                if (_wcsicmp(candidate.description.c_str(), request.event.c_str()) == 0) {
                    options.pmc_source = candidate.source;
                    options.pmc_source_set = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                error = L"unsupported record event: " + request.event;
                return false;
            }
        }
    }
    if (options.pid && !options.target.empty()) {
        error = L"--pid and a target command are mutually exclusive";
        return false;
    }
    if (options.stack_cache && !options.call_graph) {
        error = L"--stack-cache requires call graphs";
        return false;
    }
    const bool pmc = options.profile == L"pmu" || options.pmc_source_set;
    if ((pmc && options.frequency_explicit) || (!pmc && options.pmc_interval != 0)) {
        error = L"use --period for PMC event counts and -F for CPU sampling frequency";
        return false;
    }
    if (pmc && !options.pmc_source_set) {
        error = L"PMC sampling requires -e SOURCE or --pmc-source ID";
        return false;
    }
    return true;
}
int RunRecord(const RecordRequest& request, uint32_t* captured_pid = nullptr, bool quiet = false) {
    RecordOptions options;
    std::wstring error;
    if (!BuildRecordOptions(request, options, error)) {
        PrintError(error);
        return 2;
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
    if (!quiet) std::wcout << L"capturing: " << session_name << L"\n";

    ProcessLaunchResult target_launch;
    HANDLE observed_process = nullptr;
    uint32_t observed_pid = 0;
    if (!options.target.empty()) {
        if (!LaunchTargetProcess(options.target, target_launch, &error)) {
            pipe.SendLine(L"STOP", nullptr);
            PrintError(error);
            WaitForProcessExit(launch.process, 10000, target_launch.error_code);
            CloseHandle(launch.process);
            return ExitStatus(ExitCode::TargetLaunchFailure);
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
            return ExitStatus(ExitCode::TargetLaunchFailure);
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
                if (!quiet) std::wcout << L"capturing: " << session_name << L"\n";
            } else if (StartsWith(line, L"STATUS ")) {
                if (!quiet) std::wcout << L"status: " << line.substr(7) << L"\n";
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
        if (!ReadEtwFile(output, captured, &error, false)) { PrintError(error); worker_result = 6; }
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
        if (!quiet) std::wcout << L"trace: " << output.wstring() << L"\n";
    }
    if (target_exit_code != 0 && target_exit_code != STILL_ACTIVE) {
        PrintError(L"target exited with code " + std::to_wstring(target_exit_code));
        return ExitStatus(ExitCode::TargetProcessFailure);
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

int RunStat(const StatRequest& request) {
    const std::optional<uint32_t> pid = request.pid;
    const uint64_t duration_ms = request.duration_ms;
    const uint64_t interval_ms = request.interval_ms.value_or(0);
    const uint64_t pmu_period = request.period;
    const bool pmu_period_explicit = request.period_explicit;
    const bool per_thread = request.per_thread;
    const bool pmu = request.pmu;
    const bool json = request.json;
    const bool csv = request.csv;
    const std::wstring pmu_event = request.event;
    const std::vector<std::wstring>& target = request.target;
    if (pid.has_value() && !target.empty()) {
        PrintError(L"--pid and target command are mutually exclusive");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (!pid.has_value() && target.empty()) {
        PrintError(L"stat requires -p PID or a target command");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (json && csv) {
        PrintError(L"--json and --csv are mutually exclusive");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if ((pmu && interval_ms != 0) || (pmu && per_thread)) {
        PrintError(L"PMC stat currently does not support --interval or --per-thread");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (!pmu && pmu_period_explicit) {
        PrintError(L"--period requires --pmu or -e PMC_SOURCE");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (pmu && pmu_period == 0) {
        PrintError(L"--period requires a positive event count");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (interval_ms == 0 && request.interval_ms.has_value()) {
        PrintError(L"--interval requires a positive millisecond value");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (duration_ms == 0) {
        PrintError(L"--duration requires a positive millisecond value");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (pmu) {
        std::wstring error;
        const auto sources = QueryProfileSources(&error);
        if (!error.empty()) {
            PrintError(error);
            return ExitStatus(ExitCode::CapabilityUnavailable);
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
            return ExitStatus(ExitCode::CapabilityUnavailable);
        }

        const std::filesystem::path trace_path =
            GetWperfRootDirectory() / (L"pmu-" + NewId() + L".etl");
        RecordRequest record_request;
        record_request.profile = L"pmu";
        record_request.pmc_source = source;
        record_request.duration_ms = duration_ms;
        record_request.period = pmu_period;
        record_request.output = trace_path;
        record_request.pid = pid;
        record_request.target = target;
        uint32_t pmu_target_pid = 0;
        const int record_result = RunRecord(record_request, &pmu_target_pid, json || csv);
        if (record_result != 0) {
            std::error_code ignored;
            std::filesystem::remove(trace_path, ignored);
            return record_result;
        }

        TraceData trace;
        if (!ReadEtwFile(trace_path, trace, &error, false)) {
            PrintError(error);
            std::error_code ignored;
            std::filesystem::remove(trace_path, ignored);
            return ExitStatus(ExitCode::ReplayOrQualityFailure);
        }
        Symbolizer symbolizer;
        if (!symbolizer.Initialize({}, &error)) {
            PrintError(error);
            return ExitStatus(ExitCode::ReplayOrQualityFailure);
        }
        for (const auto& image : trace.images) symbolizer.AddModule(image);
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
        if (json || csv) {
            std::wcerr << L"PMU profile (sample-derived semantics): source=" << source
                       << L" (" << source_name << L") target_pid=" << pmu_target_pid
                       << L" samples=" << pmc_samples << L"\n";
        } else {
            std::wcout << L"PMU profile (sample-derived semantics)\n  source: " << source
                       << L" (" << source_name << L")  target_pid: " << pmu_target_pid
                       << L"  samples: " << pmc_samples << L"\n";
        }
        if (pmc_samples == 0) {
            PrintError(L"requested PMC source produced no target samples; unavailable");
            std::error_code ignored;
            std::filesystem::remove(trace_path, ignored);
            return ExitStatus(ExitCode::CapabilityUnavailable);
        }
        if (json || csv) {
            if (json) {
                JsonStreamWriter writer(std::wcout);
                writer.BeginObject();
                writer.Key(L"source");
                writer.String(L"etw-pmc-sampling");
                writer.Key(L"exact");
                writer.Bool(false);
                writer.Key(L"profile_source");
                writer.Unsigned(source);
                writer.Key(L"source_name");
                writer.String(source_name);
                writer.Key(L"sampling_period");
                writer.Unsigned(trace.profile_intervals.contains(source) ? trace.profile_intervals.at(source) : 0);
                writer.Key(L"target_pid");
                writer.Unsigned(pmu_target_pid);
                writer.Key(L"sample_count");
                writer.Unsigned(pmc_samples);
                writer.Key(L"samples");
                writer.BeginArray();
                for (const auto& sample : trace.samples) {
                    if (sample.kind != SampleKind::Pmc || sample.profile_source != source ||
                        (pmu_target_pid != 0 && sample.pid != pmu_target_pid)) continue;
                    nlohmann::ordered_json item;
                    item["timestamp_ns"] = sample.timestamp_ns;
                    item["cpu"] = sample.cpu;
                    item["pid"] = sample.pid;
                    item["tid"] = sample.tid;
                    item["ip"] = sample.instruction_pointer;
                    item["period"] = sample.period;
                    writer.Item(item);
                }
                writer.EndArray();
                writer.Key(L"quality");
                writer.Item(QualityJson(trace));
                writer.EndObject();
                std::wcout << L"\n";
            } else {
                std::wcout << L"pid,tid,cpu,ip,period,stack_quality\n";
                for (const auto& sample : trace.samples) {
                    if (sample.kind != SampleKind::Pmc || sample.profile_source != source ||
                        (pmu_target_pid != 0 && sample.pid != pmu_target_pid)) continue;
                    std::wcout << sample.pid << L"," << sample.tid << L"," << sample.cpu << L","
                               << sample.instruction_pointer << L"," << sample.period << L","
                               << (sample.stack_quality == StackQuality::Exact ? L"exact" :
                                   sample.stack_quality == StackQuality::Approximate ? L"approximate" : L"missing") << L"\n";
                }
            }
            std::error_code ignored;
            std::filesystem::remove(trace_path, ignored);
            return ExitStatus(ExitCode::Success);
        }
        std::wcout << L"  source_kind: etw-pmc-sampling\n"
                   << L"  exact: false\n"
                   << L"  semantics: event sampling; no exact counter total or cross-CPU delta\n";
        CpuReportOptions report_options;
        report_options.percent_limit = 1.0;
        if (pmu_target_pid != 0) report_options.pid = pmu_target_pid;
        report_options.profile_source = source;
        WriteCpuReport(trace, symbolizer, report_options, std::wcout);
        std::error_code ignored;
        std::filesystem::remove(trace_path, ignored);
        return ExitStatus(ExitCode::Success);
    }

    ProcessLaunchResult launched;
    HANDLE process = nullptr;
    uint32_t target_pid = pid.value_or(0);
    std::wstring error;
    if (!target.empty()) {
        if (!LaunchTargetProcess(target, launched, &error)) {
            PrintError(error);
            return ExitStatus(ExitCode::TargetLaunchFailure);
        }
        process = launched.process;
        target_pid = launched.process_id;
    } else {
        process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
                                  PROCESS_VM_READ,
                              FALSE, target_pid);
        if (process == nullptr) {
            PrintError(L"cannot open pid " + std::to_wstring(target_pid));
            return ExitStatus(ExitCode::TargetLaunchFailure);
        }
    }

    const std::wstring image_name = ProcessImageName(process);
    ProcessStats before;
    ProcessStats after;
    if (!ReadProcessStats(process, before)) {
        PrintError(L"cannot read process counters");
        CloseHandle(process);
        return ExitStatus(ExitCode::InternalError);
    }

    struct StatSample {
        uint64_t elapsed_ms = 0;
        uint64_t user_100ns = 0;
        uint64_t kernel_100ns = 0;
        uint64_t cycles = 0;
        bool cycles_valid = false;
        uint64_t page_faults = 0;
        uint64_t read_operations = 0;
        uint64_t read_bytes = 0;
        uint64_t write_operations = 0;
        uint64_t write_bytes = 0;
    };
    const auto make_sample = [&](const ProcessStats& current, uint64_t elapsed) {
        StatSample sample;
        sample.elapsed_ms = elapsed;
        sample.user_100ns = Delta(current.user_100ns, before.user_100ns);
        sample.kernel_100ns = Delta(current.kernel_100ns, before.kernel_100ns);
        sample.cycles = Delta(current.cycles, before.cycles);
        sample.cycles_valid = before.cycles_valid && current.cycles_valid;
        sample.page_faults = Delta(current.page_faults, before.page_faults);
        sample.read_operations = Delta(current.read_operations, before.read_operations);
        sample.read_bytes = Delta(current.read_bytes, before.read_bytes);
        sample.write_operations = Delta(current.write_operations, before.write_operations);
        sample.write_bytes = Delta(current.write_bytes, before.write_bytes);
        return sample;
    };

    std::vector<StatSample> samples;
    const ULONGLONG start_tick = GetTickCount64();
    DWORD target_exit = STILL_ACTIVE;
    uint64_t elapsed_ms = 0;
    while (true) {
        const uint64_t remaining = elapsed_ms >= duration_ms ? 0 : duration_ms - elapsed_ms;
        const uint64_t wait_ms = interval_ms == 0 ? remaining : std::min(interval_ms, remaining);
        const DWORD wait = WaitForSingleObject(
            process, static_cast<DWORD>(std::min<uint64_t>(wait_ms == 0 ? 1 : wait_ms,
                                                            UINT32_MAX)));
        elapsed_ms = GetTickCount64() - start_tick;
        DWORD exit_code = STILL_ACTIVE;
        GetExitCodeProcess(process, &exit_code);
        if (exit_code != STILL_ACTIVE) target_exit = exit_code;
        ReadProcessStats(process, after);
        samples.push_back(make_sample(after, elapsed_ms));
        if (wait == WAIT_OBJECT_0 || target_exit != STILL_ACTIVE || elapsed_ms >= duration_ms) break;
    }
    const StatSample total = make_sample(after, elapsed_ms);
    const std::optional<uint32_t> target_exit_json =
        target_exit == STILL_ACTIVE ? std::nullopt : std::optional<uint32_t>(target_exit);

    if (json) {
        JsonStreamWriter writer(std::wcout);
        writer.BeginObject();
        writer.Key(L"source");
        writer.String(L"windows-api");
        writer.Key(L"target_pid");
        writer.Unsigned(target_pid);
        writer.Key(L"target_exit_code");
        if (target_exit_json) writer.Unsigned(*target_exit_json); else writer.Null();
        writer.Key(L"elapsed_ms");
        writer.Unsigned(total.elapsed_ms);
        writer.Key(L"user_ms");
        writer.Unsigned(total.user_100ns / 10000);
        writer.Key(L"sys_ms");
        writer.Unsigned(total.kernel_100ns / 10000);
        writer.Key(L"cycles");
        if (total.cycles_valid) writer.Unsigned(total.cycles); else writer.Null();
        writer.Key(L"page_faults");
        writer.Unsigned(total.page_faults);
        writer.Key(L"io_read_ops");
        writer.Unsigned(total.read_operations);
        writer.Key(L"io_read_bytes");
        writer.Unsigned(total.read_bytes);
        writer.Key(L"io_write_ops");
        writer.Unsigned(total.write_operations);
        writer.Key(L"io_write_bytes");
        writer.Unsigned(total.write_bytes);
        writer.Key(L"samples");
        writer.BeginArray();
        for (const auto& sample : samples) {
            nlohmann::ordered_json item;
            item["elapsed_ms"] = sample.elapsed_ms;
            item["user_ms"] = sample.user_100ns / 10000;
            item["sys_ms"] = sample.kernel_100ns / 10000;
            if (sample.cycles_valid) item["cycles"] = sample.cycles; else item["cycles"] = nullptr;
            item["page_faults"] = sample.page_faults;
            item["io_read_ops"] = sample.read_operations;
            item["io_read_bytes"] = sample.read_bytes;
            item["io_write_ops"] = sample.write_operations;
            item["io_write_bytes"] = sample.write_bytes;
            writer.Item(item);
        }
        writer.EndArray();
        writer.EndObject();
        std::wcout << L"\n";
    } else if (csv) {
        std::wcout << L"elapsed_ms,user_ms,sys_ms,cycles,page_faults,io_read_ops,io_read_bytes,io_write_ops,io_write_bytes,target_exit_code\n";
        for (const auto& sample : samples) {
            std::wcout << sample.elapsed_ms << L',' << sample.user_100ns / 10000 << L','
                       << sample.kernel_100ns / 10000 << L',';
            if (sample.cycles_valid) std::wcout << sample.cycles; else std::wcout << L"n/a";
            std::wcout << L',' << sample.page_faults << L',' << sample.read_operations
                       << L',' << sample.read_bytes << L',' << sample.write_operations
                       << L',' << sample.write_bytes << L',';
            if (target_exit_json) std::wcout << *target_exit_json; else std::wcout << L"n/a";
            std::wcout << L'\n';
        }
    } else {
        std::wcout << L"Process: "
                   << (image_name.empty() ? std::to_wstring(target_pid) : image_name)
                   << L" (" << target_pid << L")\n";
        std::wcout << L"  source:   windows-api\n";
        for (const auto& sample : samples) {
            std::wcout << L"  sample:   elapsed=" << sample.elapsed_ms << L" ms"
                       << L" user=" << sample.user_100ns / 10000 << L" ms"
                       << L" sys=" << sample.kernel_100ns / 10000 << L" ms"
                       << L" cycles=";
            if (sample.cycles_valid) std::wcout << sample.cycles; else std::wcout << L"n/a";
            std::wcout << L" pagefault=" << sample.page_faults
                       << L" io-read=" << sample.read_operations << L"/" << sample.read_bytes
                       << L" io-write=" << sample.write_operations << L"/" << sample.write_bytes
                       << L"\n";
        }
        if (per_thread && target_exit_json == std::nullopt) PrintThreadStats(target_pid);
    }

    CloseHandle(process);
    if (target_exit_json && *target_exit_json != 0) {
        PrintError(L"target exited with code " + std::to_wstring(*target_exit_json));
        return ExitStatus(ExitCode::TargetProcessFailure);
    }
    return ExitStatus(ExitCode::Success);
}

int RunPublicCommand(const CommandRequest& request) {
    return std::visit([](const auto& value) -> int {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, RecordRequest>) return RunRecord(value);
        else if constexpr (std::is_same_v<T, ReportRequest>) return RunReport(value);
        else if constexpr (std::is_same_v<T, ScriptRequest>) return RunScript(value);
        else if constexpr (std::is_same_v<T, TraceRequest>) return RunTrace(value);
        else if constexpr (std::is_same_v<T, SchedRequest>) return RunSched(value);
        else if constexpr (std::is_same_v<T, StatRequest>) return RunStat(value);
        else if constexpr (std::is_same_v<T, TopRequest>) return RunTop(value);
        else if constexpr (std::is_same_v<T, AnnotateRequest>) return RunAnnotate(value);
        else if constexpr (std::is_same_v<T, DiffRequest>) return RunDiff(value);
        else if constexpr (std::is_same_v<T, ArchiveRequest>) return RunArchive(value);
        else if constexpr (std::is_same_v<T, BuildIdListRequest>) return RunBuildIdList(value);
        else if constexpr (std::is_same_v<T, ListRequest>) return RunList(value);
        else if constexpr (std::is_same_v<T, DoctorRequest>) return RunDoctor(value);
        else if constexpr (std::is_same_v<T, RecoverRequest>) return RunRecover(value);
        else if constexpr (std::is_same_v<T, TestRequest>) return RunTest();
        else return 2;
    }, request);
}
int RunCommandLine(const std::vector<std::wstring>& arguments) {
    if (!arguments.empty()) {
        const std::wstring& command = arguments[0];
        std::vector<std::wstring> rest(arguments.begin() + 1, arguments.end());
        if (command == L"--internal-live-worker") return RunLiveWorker(rest);
        if (command == L"--internal-elevated-worker") return RunWorker(rest);
        if (command == L"--internal-session-guard") return RunGuard(rest);
        if (command == L"--internal-stop-timeout-test") {
            if (GetEnvironmentVariableW(L"WPERF_ENABLE_TEST_FAULTS", nullptr, 0) == 0) {
                return 2;
            }
            WorkerStopWatchdog timeout(200);
            Sleep(INFINITE);
            return 9;
        }
        if (command == L"--internal-test-crash") {
            InstallCrashHandler(GetCrashesDirectory(), NewId());
            RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
            return 9;
        }
    }
    return cli::Run(arguments, [](const CommandRequest& request) {
        return RunPublicCommand(request);
    });
}
}  // namespace wperf
