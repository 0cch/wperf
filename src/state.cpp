#include "state.h"

#include "common.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

namespace wperf {
namespace {

std::wstring JsonStringValue(const nlohmann::ordered_json& value, const char* key) {
    if (!value.contains(key) || !value[key].is_string()) return {};
    return ToUtf16(value[key].get<std::string>());
}

uint32_t JsonNumberValue(const nlohmann::ordered_json& value, const char* key) {
    if (!value.contains(key) || !value[key].is_number_unsigned()) return 0;
    return value[key].get<uint32_t>();
}

}  // namespace

StateStore::StateStore(std::filesystem::path path) : path_(std::move(path)) {}

const std::filesystem::path& StateStore::Path() const {
    return path_;
}

std::filesystem::path StateStore::PathFor(const std::wstring& correlation_id) {
    return GetSessionsDirectory() / (correlation_id + L".json");
}

bool StateStore::Write(const RunState& state, std::wstring* error) const {
    if (!EnsureDirectory(path_.parent_path(), error)) {
        return false;
    }

    RunState copy = state;
    if (copy.correlation_id.empty()) {
        copy.correlation_id = path_.stem().wstring();
    }
    if (copy.updated_utc.empty()) {
        copy.updated_utc = UtcTimestamp();
    }

    const std::filesystem::path temporary = path_.wstring() + L".tmp." + NewId();
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        if (error != nullptr) {
            *error = L"cannot open state file: " + temporary.wstring();
        }
        return false;
    }

    nlohmann::ordered_json value;
    value["correlation_id"] = ToUtf8(copy.correlation_id);
    value["phase"] = ToUtf8(copy.phase);
    value["session_name"] = ToUtf8(copy.session_name);
    value["trace_path"] = ToUtf8(copy.trace_path);
    value["controller_role"] = ToUtf8(copy.controller_role);
    value["controller_pid"] = copy.controller_pid;
    value["worker_pid"] = copy.worker_pid;
    value["target_pid"] = copy.target_pid;
    value["guard_pid"] = copy.guard_pid;
    value["restore_source"] = copy.restore_source;
    value["restore_interval"] = copy.restore_interval;
    value["configured_interval"] = copy.configured_interval;
    value["restore_clear_pmc"] = copy.restore_clear_pmc;
    value["error_code"] = copy.error_code;
    value["updated_utc"] = ToUtf8(copy.updated_utc);
    stream << value.dump(2) << "\n";
    stream.close();
    if (!stream) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        if (error) *error = L"cannot persist state file: " + path_.wstring();
        return false;
    }

    if (!MoveFileExW(temporary.c_str(), path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        if (error != nullptr) {
            *error = L"MoveFileEx(state) failed: " + FormatWindowsError(code);
        }
        return false;
    }
    return true;
}

bool StateStore::WriteRecovery(const RunState& state, std::wstring* error) const {
    return StateStore(path_.wstring() + L".restore").Write(state, error);
}

std::optional<RunState> StateStore::ReadRecovery() const {
    std::ifstream stream(std::filesystem::path(path_.wstring() + L".restore"), std::ios::binary);
    if (!stream) return std::nullopt;
    const auto value = nlohmann::ordered_json::parse(stream, nullptr, false);
    if (value.is_discarded() || !value.is_object()) return std::nullopt;
    RunState state;
    state.correlation_id = JsonStringValue(value, "correlation_id");
    state.worker_pid = JsonNumberValue(value, "worker_pid");
    state.restore_source = JsonNumberValue(value, "restore_source");
    state.restore_interval = JsonNumberValue(value, "restore_interval");
    state.configured_interval = JsonNumberValue(value, "configured_interval");
    state.restore_clear_pmc = JsonNumberValue(value, "restore_clear_pmc");
    return state;
}

std::vector<RunState> StateStore::ReadAll(
    const std::filesystem::path& sessions_directory) {
    std::vector<RunState> result;
    std::error_code ec;
    if (!std::filesystem::exists(sessions_directory, ec)) {
        return result;
    }

    for (const auto& entry : std::filesystem::directory_iterator(sessions_directory, ec)) {
        if (ec || !entry.is_regular_file() || entry.path().extension() != L".json") {
            continue;
        }
        std::ifstream stream(entry.path(), std::ios::binary);
        if (!stream.is_open()) {
            continue;
        }
        std::ostringstream bytes;
        bytes << stream.rdbuf();
        const auto value = nlohmann::ordered_json::parse(bytes.str(), nullptr, false);
        if (value.is_discarded()) continue;

        RunState state;
        state.correlation_id = JsonStringValue(value, "correlation_id");
        state.phase = JsonStringValue(value, "phase");
        state.session_name = JsonStringValue(value, "session_name");
        state.trace_path = JsonStringValue(value, "trace_path");
        state.controller_role = JsonStringValue(value, "controller_role");
        state.updated_utc = JsonStringValue(value, "updated_utc");
        state.controller_pid = JsonNumberValue(value, "controller_pid");
        state.worker_pid = JsonNumberValue(value, "worker_pid");
        state.target_pid = JsonNumberValue(value, "target_pid");
        state.guard_pid = JsonNumberValue(value, "guard_pid");
        state.restore_source=JsonNumberValue(value,"restore_source");
        state.restore_interval=JsonNumberValue(value,"restore_interval");
        state.configured_interval=JsonNumberValue(value,"configured_interval");
        state.restore_clear_pmc=JsonNumberValue(value,"restore_clear_pmc");
        state.error_code = JsonNumberValue(value, "error_code");
        if (!state.correlation_id.empty()) {
            result.push_back(std::move(state));
        }
    }
    return result;
}

}  // namespace wperf
