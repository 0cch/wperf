#include "session_guard.h"
#include "common.h"
#include "elevation.h"
#include "state.h"

namespace wperf {
SessionGuard::~SessionGuard() {
    if (clean_event_) CloseHandle(clean_event_);
}
bool SessionGuard::Start(const std::wstring& session, const ProfileRestoreInfo& restore,
                         std::wstring* error) {
    const auto id = NewId();
    const auto event_name = L"Local\\wperf-clean-" + id;
    clean_event_ = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
    if (!clean_event_) {
        if (error) *error = L"cannot create session guard event: " + FormatWindowsError(GetLastError());
        return false;
    }
    auto& state = state_;
    state.correlation_id = id;
    state.session_name = session;
    state.worker_pid = GetCurrentProcessId();
    state.phase = L"CAPTURING";
    StateStore store(StateStore::PathFor(id));
    if (!store.Write(state, error)) return false;
    const std::vector<std::wstring> arguments{
        L"--internal-session-guard", L"--session", session,
        L"--event", event_name, L"--worker-pid", std::to_wstring(state.worker_pid),
        L"--state", store.Path().wstring(), L"--correlation", id,
        L"--log", (GetLogsDirectory() / (id + L"-guard.jsonl")).wstring(),
        L"--restore-source", std::to_wstring(restore.source),
        L"--restore-interval", std::to_wstring(restore.original_interval),
        L"--configured-interval", std::to_wstring(restore.configured_interval),
        L"--clear-pmc", restore.clear_pmc ? L"1" : L"0"};
    ProcessLaunchResult guard;
    if (!LaunchWorkerProcess(GetExecutablePath(), arguments, false, guard, error)) return false;
    state.guard_pid = guard.process_id;
    store.Write(state, nullptr);
    CloseHandle(guard.process);
    return true;
}
bool SessionGuard::UpdateRestore(const ProfileRestoreInfo& restore,std::wstring* error) {
    state_.restore_source=restore.source; state_.restore_interval=restore.original_interval;
    state_.configured_interval=restore.configured_interval; state_.restore_clear_pmc=restore.clear_pmc;
    state_.phase=L"CONFIGURING";
    return StateStore(StateStore::PathFor(state_.correlation_id)).Write(state_,error);
}
void SessionGuard::Complete() {
    if (clean_event_) {
        state_.phase = L"FINISHED";
        StateStore(StateStore::PathFor(state_.correlation_id)).Write(state_, nullptr);
        SetEvent(clean_event_);
    }
}
}
