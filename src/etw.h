#pragma once

#include <windows.h>
#include <evntrace.h>
#include <evntprov.h>
#include "trace_model.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <functional>

namespace wperf {

struct ProviderInfo {
    GUID id{};
    uint32_t schema_source = 0;
    std::wstring name;
};

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
    bool enable_scheduler_stack = false;
    bool enable_context_registers = false;
    bool realtime = false;
    bool system_logger = true;
    bool enable_provider = false;
    GUID provider_id{};
    uint8_t provider_level = 0;
    uint64_t provider_match_any = 0;
    uint64_t provider_match_all = 0;
    bool pmc_profile = false;
    uint32_t pmc_source = 0;
    uint32_t pmc_interval = 0;
    bool enable_lbr = false;
    bool stack_cache = false;
    std::vector<uint32_t> pmc_counter_sources;
    bool enable_pool = false;
    uint32_t pool_tag = 0;
    uint32_t heap_pid = 0;
};

struct ProfileRestoreInfo {
    uint32_t source = 0;
    uint32_t original_interval = 0;
    uint32_t configured_interval = 0;
    bool clear_pmc = false;
};
bool RestoreProfileConfiguration(const ProfileRestoreInfo& info, std::wstring* error = nullptr);

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
    const std::wstring& ContextRegisterStatus() const;
    const std::wstring& LbrStatus() const;
    const std::wstring& SchedulerStackStatus() const;
    bool QueryQuality(TraceQuality& quality, std::wstring* error = nullptr) const;
    ProfileRestoreInfo RestoreInfo() const;
    void SetRecoveryWriter(std::function<bool(const ProfileRestoreInfo&)> writer) { recovery_writer_=std::move(writer); }

private:
    TRACEHANDLE handle_ = 0;
    std::wstring name_;
    TRACE_PROFILE_INTERVAL original_interval_{};
    bool original_interval_valid_ = false;
    bool pmc_config_changed_ = false;
    HANDLE profile_mutex_ = nullptr;
    uint32_t configured_interval_ = 0;
    TraceQuality final_quality_;
    bool ConfigureProfile(const TraceOptions& options, std::wstring* error);
    bool WriteCaptureMetadata(const TraceOptions& options, std::wstring* error);
    bool RestoreProfile(std::wstring* error);
    std::wstring context_register_status_;
    std::wstring lbr_status_;
    std::wstring scheduler_stack_status_;
    REGHANDLE metadata_provider_ = 0;
    std::function<bool(const ProfileRestoreInfo&)> recovery_writer_;
};

struct PmcCounterOwnerInfo {
    uint32_t processor = 0;
    uint32_t owner_type = 0;
    uint32_t source = 0;
    uint32_t tag = 0;
};

struct PmcSessionInfo {
    uint16_t logger_id = 0;
    std::vector<uint32_t> sources;
    std::vector<uint16_t> hook_ids;
};

ULONG TraceFlagsForProfile(std::wstring_view profile);
ULONG TraceFlagsForLiveFilter(std::wstring_view filter);
std::vector<ProfileSource> QueryProfileSources(std::wstring* error = nullptr);
std::vector<ProviderInfo> QueryProviders(std::wstring* error = nullptr);
uint32_t QueryMaxLoggers(std::wstring* error = nullptr);
uint32_t QueryMaxPmcCounters(std::wstring* error = nullptr);
std::vector<PmcCounterOwnerInfo> QueryPmcCounterOwners(
    std::wstring* error = nullptr);
std::vector<PmcSessionInfo> QueryPmcSessions(std::wstring* error = nullptr);
std::vector<std::wstring> QueryActiveSessions(std::wstring* error = nullptr);
bool SessionExists(const std::wstring& session_name);
bool StopSessionByName(const std::wstring& session_name,
                       std::wstring* error = nullptr);

}  // namespace wperf
