#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <optional>
#include <vector>

namespace wperf {

struct RunState {
    std::wstring correlation_id;
    std::wstring phase;
    std::wstring session_name;
    std::wstring trace_path;
    std::wstring controller_role;
    uint32_t controller_pid = 0;
    uint32_t worker_pid = 0;
    uint32_t target_pid = 0;
    uint32_t guard_pid = 0;
    uint32_t error_code = 0;
    uint32_t restore_source = 0;
    uint32_t restore_interval = 0;
    uint32_t configured_interval = 0;
    uint32_t restore_clear_pmc = 0;
    std::wstring updated_utc;
};

class StateStore {
public:
    explicit StateStore(std::filesystem::path path);

    const std::filesystem::path& Path() const;
    bool Write(const RunState& state, std::wstring* error = nullptr) const;
    // Worker-owned checkpoint, independent of controller status updates.
    bool WriteRecovery(const RunState& state, std::wstring* error = nullptr) const;
    std::optional<RunState> ReadRecovery() const;

    static std::vector<RunState> ReadAll(
        const std::filesystem::path& sessions_directory);
    static std::filesystem::path PathFor(const std::wstring& correlation_id);

private:
    std::filesystem::path path_;
};

}  // namespace wperf
