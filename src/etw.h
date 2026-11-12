#pragma once

#include <windows.h>
#include <evntrace.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wperf {

struct ProfileSource {
    uint32_t source = 0;
    uint32_t min_interval = 0;
    uint32_t max_interval = 0;
    std::wstring description;
};

struct TraceOptions {
    std::wstring session_name;
    std::filesystem::path output_path;
    ULONG buffer_size_kb = 64;
    ULONG minimum_buffers = 4;
    ULONG maximum_buffers = 64;
    ULONG maximum_file_size_mb = 0;
    ULONG flush_timer_seconds = 1;
    ULONG enable_flags = EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD |
                         EVENT_TRACE_FLAG_IMAGE_LOAD | EVENT_TRACE_FLAG_PROFILE;
    uint32_t profile_interval_100ns = 10000;
    bool enable_stack_tracing = true;
    bool pmc_profile = false;
    uint32_t pmc_source = 0;
    bool enable_lbr = false;
};

class EtwSession {
public:
    EtwSession() = default;
    ~EtwSession();

    EtwSession(const EtwSession&) = delete;
    EtwSession& operator=(const EtwSession&) = delete;

    bool Start(const TraceOptions& options, std::wstring* error = nullptr);
    bool Stop(std::wstring* error = nullptr);
    bool IsActive() const;
    const std::wstring& Name() const;

private:
    TRACEHANDLE handle_ = 0;
    std::wstring name_;
    TRACE_PROFILE_INTERVAL original_interval_{};
    bool original_interval_valid_ = false;
    bool pmc_config_changed_ = false;
};

ULONG TraceFlagsForProfile(std::wstring_view profile);
std::vector<ProfileSource> QueryProfileSources(std::wstring* error = nullptr);
uint32_t QueryMaxLoggers(std::wstring* error = nullptr);
uint32_t QueryMaxPmcCounters(std::wstring* error = nullptr);
std::vector<std::wstring> QueryActiveSessions(std::wstring* error = nullptr);
bool SessionExists(const std::wstring& session_name);
bool StopSessionByName(const std::wstring& session_name,
                       std::wstring* error = nullptr);

}  // namespace wperf
