#include "etw.h"

#include "common.h"
#include "etw_decode.h"
#include "diagnostic_decode.h"
#include <sstream>

#include <algorithm>
#include <cstring>
#include <tdh.h>
#include <vector>

namespace wperf {
namespace {

constexpr ULONG kPropertiesBufferSize = 64 * 1024;
constexpr size_t kAlignment = 8;
const GUID kPerfInfoGuid = {0xce1dbfb4, 0x137e, 0x4da6,
                            {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
const GUID kThreadGuid = {0x3d6fa8d1, 0xfe05, 0x11d0,
                          {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};

size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

std::vector<BYTE> MakePropertiesBuffer(size_t extra_bytes) {
    return std::vector<BYTE>(sizeof(EVENT_TRACE_PROPERTIES) + extra_bytes, 0);
}

PEVENT_TRACE_PROPERTIES InitializeProperties(std::vector<BYTE>& buffer,
                                             const std::wstring& logger_name,
                                             const std::wstring& log_file) {
    auto* properties = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buffer.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(buffer.size());
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 1;
    properties->BufferSize = 64;
    properties->MinimumBuffers = 4;
    properties->MaximumBuffers = 64;
    properties->FlushTimer = 1;
    properties->LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL;
    properties->EnableFlags = EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD |
                              EVENT_TRACE_FLAG_IMAGE_LOAD | EVENT_TRACE_FLAG_PROFILE;

    const size_t log_offset = sizeof(EVENT_TRACE_PROPERTIES);
    const size_t logger_offset = AlignUp(log_offset + (log_file.size() + 1) * sizeof(wchar_t),
                                         kAlignment);
    properties->LogFileNameOffset = static_cast<ULONG>(log_offset);
    properties->LoggerNameOffset = static_cast<ULONG>(logger_offset);

    wchar_t* log_destination =
        reinterpret_cast<wchar_t*>(buffer.data() + properties->LogFileNameOffset);
    wcscpy_s(log_destination, log_file.size() + 1, log_file.c_str());
    wchar_t* logger_destination =
        reinterpret_cast<wchar_t*>(buffer.data() + properties->LoggerNameOffset);
    wcscpy_s(logger_destination, logger_name.size() + 1, logger_name.c_str());
    return properties;
}

std::wstring LastErrorWith(const wchar_t* operation, ULONG code) {
    return std::wstring(operation) + L" failed (" + std::to_wstring(code) +
           L"): " + FormatWindowsError(code);
}

}  // namespace

EtwSession::~EtwSession() {
    std::wstring ignored;
    Stop(&ignored);
    RestoreProfile(&ignored);
}

ULONG ConfigureLbrCompat(CONTROLTRACE_ID trace_id, TRACE_LBR_CONFIGURATION configuration,
                         const CLASSIC_EVENT_ID* events, ULONG event_count);
bool EtwSession::Start(const TraceOptions& options, std::wstring* error) {
    context_register_status_.clear();
    lbr_status_.clear();
    scheduler_stack_status_.clear();
    if (!IsActive() && original_interval_valid_ && !RestoreProfile(error)) return false;
    if (IsActive()) {
        if (error != nullptr) {
            *error = L"ETW session is already active";
        }
        return false;
    }
    if (options.session_name.empty() ||
        (!options.realtime && options.output_path.empty())) {
        if (error != nullptr) {
            *error = options.realtime
                         ? L"session name is required"
                         : L"session name and output path are required";
        }
        return false;
    }
    final_quality_ = {};
    std::wstring privilege_error;
    if (options.system_logger && !EnablePrivilege(L"SeSystemProfilePrivilege", &privilege_error)) {
        if (error != nullptr) {
            *error = L"EnablePrivilege(SeSystemProfilePrivilege) failed: " +
                     privilege_error;
        }
        return false;
    }

    const std::wstring log_file = options.output_path.wstring();
    const size_t log_bytes = (log_file.size() + 1) * sizeof(wchar_t);
    const size_t logger_bytes = (options.session_name.size() + 1) * sizeof(wchar_t);
    const size_t log_offset = sizeof(EVENT_TRACE_PROPERTIES);
    const size_t logger_offset = AlignUp(log_offset + log_bytes, kAlignment);
    const size_t total_size = logger_offset + logger_bytes + (options.heap_pid || options.enable_pool ? 96u : 0u);
    auto buffer = MakePropertiesBuffer(total_size - sizeof(EVENT_TRACE_PROPERTIES));
    auto* properties = InitializeProperties(buffer, options.session_name, log_file);
    properties->BufferSize = options.buffer_size_kb;
    properties->MinimumBuffers = options.minimum_buffers;
    properties->MaximumBuffers = options.maximum_buffers;
    properties->MaximumFileSize = options.maximum_file_size_mb;
    properties->FlushTimer = options.flush_timer_seconds;
    properties->EnableFlags = options.system_logger ? options.enable_flags : 0;
    if (options.system_logger && options.pmc_profile)
        properties->EnableFlags &= ~EVENT_TRACE_FLAG_PROFILE;
    properties->LogFileMode =
        (options.realtime ? EVENT_TRACE_REAL_TIME_MODE
                          : EVENT_TRACE_FILE_MODE_SEQUENTIAL) |
        EVENT_TRACE_USE_GLOBAL_SEQUENCE |
        (options.system_logger ? EVENT_TRACE_SYSTEM_LOGGER_MODE : 0);

    if (options.heap_pid != 0) {
        // Native heap sessions use the ETW properties extension protocol (PID
        // and stack hooks), also used by xperf/TraceEvent. No IFEO state is set.
        properties->Wnode.Guid = kHeapGuid;
        properties->LogFileMode &= ~EVENT_TRACE_SYSTEM_LOGGER_MODE;
        const size_t offset = AlignUp(properties->LoggerNameOffset +
            (options.session_name.size() + 1) * sizeof(wchar_t), sizeof(ULONG));
        std::vector<ULONG> extension{0, (2u << 16) | 2u, options.heap_pid};
        if (options.enable_stack_tracing) {
            extension.insert(extension.end(), {(3u << 16) | 3u, 0x1021u, 0x1022u});
        }
        extension[0] = static_cast<ULONG>(extension.size()) |
            ((options.enable_stack_tracing ? 2u : 1u) << 16);
        if (offset >= 65536 || offset + extension.size() * sizeof(ULONG) > buffer.size()) {
            if (error) *error = L"heap session properties exceed the extension limit";
            return false;
        }
        std::memcpy(buffer.data() + offset, extension.data(), extension.size() * sizeof(ULONG));
        properties->EnableFlags = 0x80ff0000u | static_cast<ULONG>(offset);
    }
    if (options.enable_pool) {
        if (!options.pool_tag) { if (error) *error = L"pool capture requires a tag filter"; return false; }
        const size_t offset = AlignUp(properties->LoggerNameOffset + (options.session_name.size() + 1) * sizeof(wchar_t), sizeof(ULONG));
        // ETW_EXT_ENABLE_FLAGS (eight group masks) and ETW_EXT_POOLTAG_FILTER.
        const ULONG extension[] = {(2u << 16) | 12u, (1u << 16) | 9u,
            options.enable_flags, 0x40u, 0, 0, 0, 0, 0, 0, (4u << 16) | 2u, options.pool_tag};
        if (offset >= 65536 || offset + sizeof(extension) > buffer.size()) { if (error) *error = L"pool extension exceeds properties limits"; return false; }
        std::memcpy(buffer.data() + offset, extension, sizeof(extension));
        properties->EnableFlags = 0x80ff0000u | static_cast<ULONG>(offset);
    }

    if(options.pmc_profile && !options.pmc_counter_sources.empty()) {
        if(error)*error=L"PMC interrupt sampling and event-counter snapshots require separate captures";
        return false;
    }
    if(!options.pmc_counter_sources.empty()) {
        if(options.pmc_counter_sources.size()>QueryMaxPmcCounters(error))return false;
        std::wstring query_error;
        const auto owners=QueryPmcCounterOwners(&query_error);
        if(!query_error.empty()||!owners.empty()) { if(error)*error=L"PMC counters are unavailable or already owned"; return false; }
        properties->EnableFlags|=EVENT_TRACE_FLAG_CSWITCH;
    }
    const bool needs_profile = options.system_logger && options.heap_pid == 0 &&
        (options.pmc_profile || (options.enable_flags & EVENT_TRACE_FLAG_PROFILE) != 0);
    if (needs_profile && !ConfigureProfile(options, error)) return false;
    TRACEHANDLE handle = 0;
    const ULONG start_result =
        StartTraceW(&handle, options.session_name.c_str(), properties);
    if (start_result != ERROR_SUCCESS) {
        RestoreProfile(nullptr);
        if (error != nullptr) {
            *error = LastErrorWith(L"StartTraceW", start_result);
        }
        return false;
    }

    handle_ = handle;
    name_ = options.session_name;

    if (options.enable_provider) {
        const ULONG enable_result = EnableTraceEx2(
            handle_, &options.provider_id, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
            options.provider_level, options.provider_match_any,
            options.provider_match_all, 0, nullptr);
        if (enable_result != ERROR_SUCCESS) {
            std::wstring stop_error;
            Stop(&stop_error);
            if (error != nullptr) {
                *error = LastErrorWith(L"EnableTraceEx2(provider)", enable_result);
            }
            return false;
        }
    }

    if (options.heap_pid != 0) return WriteCaptureMetadata(options, error);
    if (!options.system_logger) {
        return true;
    }


    if (options.pmc_profile) {
        struct GroupMask { ULONG masks[8]; } mask{};
        mask.masks[0] = properties->EnableFlags;
        mask.masks[1] = 0x00000400;  // PERF_PMC_PROFILE, not timer sampling.
        const ULONG result = TraceSetInformation(handle_, TraceSystemTraceEnableFlagsInfo,
                                                 &mask, sizeof(mask));
        if (result != ERROR_SUCCESS) {
            Stop(nullptr);
            if (error) *error = LastErrorWith(L"enable PMC profile events", result);
            return false;
        }
    }

    if(!options.pmc_counter_sources.empty()) {
        const auto sources=QueryProfileSources(error);
        for(uint32_t requested:options.pmc_counter_sources)
            if(requested==0 || std::none_of(sources.begin(),sources.end(),[&](const auto& source){return source.source==requested;})) {
                Stop(nullptr); if(error)*error=L"requested snapshot PMC source was not found"; return false;
            }
        CLASSIC_EVENT_ID hook{};
        hook.EventGuid={0x3d6fa8d1,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}}; hook.Type=36;
        ULONG status=TraceSetInformation(handle_,TracePmcCounterListInfo,
            const_cast<uint32_t*>(options.pmc_counter_sources.data()),static_cast<ULONG>(options.pmc_counter_sources.size()*sizeof(uint32_t)));
        if(status==ERROR_SUCCESS)status=TraceSetInformation(handle_,TracePmcEventListInfo,&hook,sizeof(hook));
        if(status!=ERROR_SUCCESS) { Stop(nullptr); if(error)*error=LastErrorWith(L"configure PMC event snapshots",status); return false; }
    }
    if(options.stack_cache) {
        TRACE_STACK_CACHING_INFO cache{}; cache.Enabled=TRUE; cache.CacheSize=4096; cache.BucketCount=4096;
        const ULONG status=TraceSetInformation(handle_,TraceStackCachingInfo,&cache,sizeof(cache));
        if(status!=ERROR_SUCCESS) { Stop(nullptr); if(error)*error=LastErrorWith(L"configure ETW stack cache",status); return false; }
    }
    if (options.enable_stack_tracing) {
        std::vector<CLASSIC_EVENT_ID> stack_events;
        const auto add = [&](const GUID& guid, UCHAR type) {
            CLASSIC_EVENT_ID event{}; event.EventGuid = guid; event.Type = type;
            stack_events.push_back(event);
        };
        if (needs_profile) add(kPerfInfoGuid, options.pmc_profile ? 47 : 46);
        if (options.enable_pool) { add(kPoolGuid, 32); add(kPoolGuid, 33); }
        // Driver events already contain the routine address. Stack walking
        // every driver call also amplifies ETL writer I/O and is unnecessary.
        const ULONG stack_result = stack_events.empty() ? ERROR_SUCCESS : TraceSetInformation(
            handle_, TraceStackTracingInfo, stack_events.data(),
            static_cast<ULONG>(stack_events.size() * sizeof(CLASSIC_EVENT_ID)));
        if (!stack_events.empty() && stack_result != ERROR_SUCCESS) {
            std::wstring stop_error;
            Stop(&stop_error);
            if (error != nullptr) {
                *error = LastErrorWith(L"TraceSetInformation(stack tracing)",
                                       stack_result);
            }
            return false;
        }
    }

    if (options.enable_scheduler_stack) {
        CLASSIC_EVENT_ID scheduler_events[2]{};
        scheduler_events[0].EventGuid = kThreadGuid;
        scheduler_events[0].Type = 36;
        scheduler_events[1].EventGuid = kThreadGuid;
        scheduler_events[1].Type = 50;
        const ULONG scheduler_result = TraceSetInformation(
            handle_, TraceStackTracingInfo, scheduler_events,
            sizeof(scheduler_events));
        scheduler_stack_status_ =
            scheduler_result == ERROR_SUCCESS
                ? L"enabled"
                : LastErrorWith(L"TraceSetInformation(scheduler stack tracing)",
                                scheduler_result);
    }

    if (options.enable_context_registers) {
        struct ContextRequest {
            TRACE_CONTEXT_REGISTER_INFO Info;
            CLASSIC_EVENT_ID Event;
        } request{};
        request.Info.RegisterTypes = static_cast<ETW_CONTEXT_REGISTER_TYPES>(
            EtwContextRegisterTypeControl | EtwContextRegisterTypeInteger);
        request.Event.EventGuid = kPerfInfoGuid;
        request.Event.Type = options.pmc_profile ? 47 : 46;
        const ULONG context_result = TraceSetInformation(
            handle_, TraceContextRegisterInfo, &request, sizeof(request));
        context_register_status_ =
            context_result == ERROR_SUCCESS
                ? L"enabled"
                : LastErrorWith(L"TraceSetInformation(TraceContextRegisterInfo)",
                                context_result);
    }

    if (options.enable_lbr) {
        CLASSIC_EVENT_ID lbr_event{};
        lbr_event.EventGuid = kPerfInfoGuid;
        lbr_event.Type = options.pmc_profile ? 47 : 46;
        ULONG lbr_result = ConfigureLbrCompat(
            handle_, TRACE_LBR_CONFIGURATION_CALLSTACK_ENABLE, &lbr_event, 1);
        if (lbr_result == ERROR_NOT_SUPPORTED || lbr_result == ERROR_INVALID_PARAMETER) {
            const ULONG raw_result = ConfigureLbrCompat(
                handle_, TRACE_LBR_CONFIGURATION_NONE, &lbr_event, 1);
            if (raw_result == ERROR_SUCCESS) {
                lbr_status_ = L"enabled (raw LBR; callstack mode unsupported)";
                lbr_result = ERROR_SUCCESS;
            }
        }
        if (lbr_result != ERROR_SUCCESS) {
            ULONG filters = TRACE_LBR_CONFIGURATION_CALLSTACK_ENABLE;
            ULONG legacy_result = TraceSetInformation(
                handle_, TraceLbrConfigurationInfo, &filters, sizeof(filters));
            if (legacy_result == ERROR_SUCCESS) {
                ULONG hook = 0x0F00 | lbr_event.Type;
                legacy_result = TraceSetInformation(
                    handle_, TraceLbrEventListInfo, &hook, sizeof(hook));
            }
            if (legacy_result == ERROR_SUCCESS) {
                lbr_status_ = L"enabled (legacy event-triggered LBR)";
                lbr_result = ERROR_SUCCESS;
            }
        }
        if (lbr_result == ERROR_SUCCESS && lbr_status_.empty()) {
            lbr_status_ = L"enabled";
        } else if (lbr_result != ERROR_SUCCESS) {
            lbr_status_ = L"unsupported: " + FormatWindowsError(lbr_result);
        }
        if (lbr_result != ERROR_SUCCESS) {
            std::wstring stop_error;
            Stop(&stop_error);
            if (error != nullptr) {
                *error = LastErrorWith(L"TraceConfigureLastBranchRecord", lbr_result);
            }
            return false;
        }
    }

    return WriteCaptureMetadata(options, error);
}

bool EtwSession::WriteCaptureMetadata(const TraceOptions& options, std::wstring* error) {
    ULONG metadata_status=EventRegister(&kWperfMetadataGuid,nullptr,nullptr,&metadata_provider_);
    if(metadata_status==ERROR_SUCCESS)metadata_status=EnableTraceEx2(handle_,&kWperfMetadataGuid,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,TRACE_LEVEL_INFORMATION,0,0,5000,nullptr);
    if(metadata_status==ERROR_SUCCESS) {
        std::ostringstream metadata; metadata<<"wperf-meta-v1\nsession "<<HexText(options.session_name)<<'\n';
        if (options.heap_pid) metadata << "heap-target " << options.heap_pid << '\n';
        if (options.enable_pool) metadata << "pool-tag " << options.pool_tag << '\n';
        if(configured_interval_!=0)metadata<<"period "<<original_interval_.Source<<' '<<configured_interval_<<'\n';
        const auto sources=QueryProfileSources(nullptr);
        for(uint32_t id:options.pmc_counter_sources) {
            const auto source=std::find_if(sources.begin(),sources.end(),[&](const auto& value){return value.source==id;});
            metadata<<"counter "<<id<<' '<<HexText(source==sources.end()?std::to_wstring(id):source->description)<<'\n';
        }
        const auto text=metadata.str(); EVENT_DESCRIPTOR descriptor{}; descriptor.Id=1; descriptor.Level=TRACE_LEVEL_INFORMATION;
        EVENT_DATA_DESCRIPTOR data{}; EventDataDescCreate(&data,text.data(),static_cast<ULONG>(text.size()));
        // Timeout=0 enablement is asynchronous; writing before enable completes
        // can succeed without logging the metadata required to interpret counters.
        metadata_status = EventEnabled(metadata_provider_, &descriptor)
            ? EventWrite(metadata_provider_, &descriptor, 1, &data) : ERROR_NOT_READY;
    }
    if(metadata_status!=ERROR_SUCCESS) { Stop(nullptr); if(error)*error=LastErrorWith(L"write capture metadata",metadata_status); return false; }
    return true;
}

ULONG ConfigureLbrCompat(CONTROLTRACE_ID trace_id, TRACE_LBR_CONFIGURATION configuration,
                         const CLASSIC_EVENT_ID* events, ULONG event_count) {
    using ConfigureLbrFn = ULONG(WINAPI*)(CONTROLTRACE_ID, TRACE_LBR_CONFIGURATION,
                                           const CLASSIC_EVENT_ID*, ULONG);
    HMODULE module = GetModuleHandleW(L"advapi32.dll");
    if (module == nullptr) {
        module = GetModuleHandleW(L"kernelbase.dll");
    }
    if (module == nullptr) {
        return ERROR_NOT_SUPPORTED;
    }
    const auto function = reinterpret_cast<ConfigureLbrFn>(
        GetProcAddress(module, "TraceConfigureLastBranchRecord"));
    if (function == nullptr) {
        return ERROR_NOT_SUPPORTED;
    }
    return function(trace_id, configuration, events, event_count);
}
bool EtwSession::Stop(std::wstring* error) {
    if (!IsActive()) return RestoreProfile(error);
    std::vector<BYTE> buffer(kPropertiesBufferSize, 0);
    auto* properties = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buffer.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(buffer.size());
    const ULONG result = ControlTraceW(handle_, name_.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    if (result != ERROR_SUCCESS && result != ERROR_WMI_INSTANCE_NOT_FOUND) {
        if (error) *error = LastErrorWith(L"ControlTraceW(stop)", result);
        return false;  // Preserve session identity for retry/recovery.
    }
    if (result == ERROR_SUCCESS) {
        final_quality_.lost_events = properties->EventsLost;
        final_quality_.lost_buffers = uint64_t{properties->LogBuffersLost} + properties->RealTimeBuffersLost;
    }
    if(metadata_provider_) { EventUnregister(metadata_provider_); metadata_provider_=0; }
    handle_ = 0;
    name_.clear();
    return RestoreProfile(error);
}

bool EtwSession::QueryQuality(TraceQuality& quality, std::wstring* error) const {
    quality = final_quality_;
    if (!IsActive()) return true;
    std::vector<BYTE> buffer(kPropertiesBufferSize, 0);
    auto* properties = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buffer.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(buffer.size());
    const ULONG result = ControlTraceW(handle_, name_.c_str(), properties, EVENT_TRACE_CONTROL_QUERY);
    if (result != ERROR_SUCCESS) {
        if (error) *error = LastErrorWith(L"ControlTraceW(query quality)", result);
        return false;
    }
    quality.lost_events = properties->EventsLost;
    quality.lost_buffers = uint64_t{properties->LogBuffersLost} + properties->RealTimeBuffersLost;
    quality.incomplete = quality.lost_events != 0 || quality.lost_buffers != 0;
    return true;
}

ProfileRestoreInfo EtwSession::RestoreInfo() const {
    return {original_interval_.Source, original_interval_valid_ ? original_interval_.Interval : 0,
            configured_interval_, pmc_config_changed_};
}

namespace {
constexpr wchar_t kProfileMutexName[] = L"Global\\wperf-profile-configuration-v1";
bool RestoreOwnedConfiguration(const ProfileRestoreInfo& info, std::wstring* error) {
    if (info.original_interval == 0) {
        if (!info.clear_pmc) return true;
        ULONG timer_source = 0;
        const ULONG result = TraceSetInformation(0, TraceProfileSourceConfigInfo,
                                              &timer_source, sizeof(timer_source));
        if (result != ERROR_SUCCESS && error) *error = LastErrorWith(L"restore timer profile source", result);
        return result == ERROR_SUCCESS;
    }
    TRACE_PROFILE_INTERVAL current{info.source, 0};
    ULONG returned = 0;
    ULONG result = TraceQueryInformation(0, TraceSampledProfileIntervalInfo, &current,
                                         sizeof(current), &returned);
    // Do not overwrite a configuration another profiler has changed.
    if (result == ERROR_SUCCESS && current.Interval != info.configured_interval &&
        current.Interval != info.original_interval) return true;
    if (result == ERROR_SUCCESS && current.Interval != info.original_interval) {
        TRACE_PROFILE_INTERVAL original{info.source, info.original_interval};
        result = TraceSetInformation(0, TraceSampledProfileIntervalInfo, &original, sizeof(original));
    }
    if (result == ERROR_SUCCESS && info.clear_pmc) {
        ULONG timer_source = 0;
        result = TraceSetInformation(0, TraceProfileSourceConfigInfo,
                                     &timer_source, sizeof(timer_source));
    }
    if (result != ERROR_SUCCESS && error) *error = LastErrorWith(L"restore profile configuration", result);
    return result == ERROR_SUCCESS;
}
}

bool RestoreProfileConfiguration(const ProfileRestoreInfo& info, std::wstring* error) {
    if (info.original_interval == 0 && !info.clear_pmc) return true;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kProfileMutexName);
    if (mutex == nullptr) return false;
    const DWORD wait = WaitForSingleObject(mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        if (error) *error = L"profile configuration is owned by another capture";
        return false;
    }
    const bool restored = RestoreOwnedConfiguration(info, error);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return restored;
}

bool EtwSession::RestoreProfile(std::wstring* error) {
    const bool restored = profile_mutex_ != nullptr
                              ? RestoreOwnedConfiguration(RestoreInfo(), error)
                              : (!original_interval_valid_ || RestoreProfileConfiguration(RestoreInfo(), error));
    if (restored) {
        original_interval_valid_ = false;
        pmc_config_changed_ = false;
        configured_interval_ = 0;
    }
    if (restored && profile_mutex_ != nullptr) {
        ReleaseMutex(profile_mutex_);
        CloseHandle(profile_mutex_);
        profile_mutex_ = nullptr;
    }
    return restored;
}

bool EtwSession::ConfigureProfile(const TraceOptions& options, std::wstring* error) {
    profile_mutex_ = CreateMutexW(nullptr, FALSE, kProfileMutexName);
    if (profile_mutex_ == nullptr) {
        if (error) *error = L"cannot create profile configuration lock";
        return false;
    }
    const DWORD wait = WaitForSingleObject(profile_mutex_, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(profile_mutex_); profile_mutex_ = nullptr;
        if (error) *error = L"another wperf capture owns the system sampling configuration";
        return false;
    }
    uint32_t interval = options.profile_interval_100ns;
    if (options.pmc_profile) {
        const auto sources = QueryProfileSources(error);
        const auto source = std::find_if(sources.begin(), sources.end(), [&](const auto& value) {
            return value.source == options.pmc_source;
        });
        if (source == sources.end() || source->source == 0 || source->min_interval > source->max_interval || source->max_interval == 0) {
            if (error) *error = L"a hardware PMC source is required";
            RestoreProfile(nullptr); return false;
        }
        interval = options.pmc_interval != 0 ? options.pmc_interval :
            std::clamp<uint32_t>(65536, source->min_interval, source->max_interval);
        if (interval < source->min_interval || interval > source->max_interval) {
            if (error) *error = L"PMC period is outside the source's supported interval range";
            RestoreProfile(nullptr); return false;
        }
        std::wstring owner_error;
        const auto owners = QueryPmcCounterOwners(&owner_error);
        if (!owner_error.empty() || !owners.empty()) {
            if (error) *error = owner_error.empty() ? L"hardware PMC counters are already in use" : owner_error;
            RestoreProfile(nullptr); return false;
        }
    }
    original_interval_ = {};
    original_interval_.Source = options.pmc_profile ? options.pmc_source : 0;
    ULONG returned = 0;
    ULONG result = TraceQueryInformation(0, TraceSampledProfileIntervalInfo, &original_interval_,
                                         sizeof(original_interval_), &returned);
    original_interval_valid_ = result == ERROR_SUCCESS;
    configured_interval_=interval;
    if(result==ERROR_SUCCESS&&recovery_writer_ &&
       !recovery_writer_({original_interval_.Source,original_interval_.Interval,interval,options.pmc_profile})) {
        configured_interval_=original_interval_.Interval;
        RestoreProfile(nullptr);
        if(error)*error=L"cannot persist sampling recovery checkpoint";
        return false;
    }
    if (result == ERROR_SUCCESS && options.pmc_profile) {
        ULONG source = options.pmc_source;
        result = TraceSetInformation(0, TraceProfileSourceConfigInfo, &source, sizeof(source));
        pmc_config_changed_ = result == ERROR_SUCCESS;
    }
    TRACE_PROFILE_INTERVAL configured{original_interval_.Source, interval};
    configured_interval_ = interval;
    if (result == ERROR_SUCCESS)
        result = TraceSetInformation(0, TraceSampledProfileIntervalInfo, &configured, sizeof(configured));
    if (result == ERROR_SUCCESS) {
        TRACE_PROFILE_INTERVAL actual{original_interval_.Source, 0};
        const ULONG query = TraceQueryInformation(0, TraceSampledProfileIntervalInfo, &actual,
                                                  sizeof(actual), &returned);
        if (query == ERROR_SUCCESS) configured_interval_ = actual.Interval;
        if(recovery_writer_&&!recovery_writer_(RestoreInfo())) {
            RestoreProfile(nullptr); if(error)*error=L"cannot persist configured sampling interval"; return false;
        }
    }
    if (result != ERROR_SUCCESS) {
        if (error) *error = LastErrorWith(L"configure profile interval/source", result);
        // The interval may still be original when source setup alone succeeded.
        configured_interval_ = original_interval_.Interval;
        RestoreProfile(nullptr);
        return false;
    }
    return true;
}

const std::wstring& EtwSession::ContextRegisterStatus() const {
    return context_register_status_;
}

const std::wstring& EtwSession::LbrStatus() const {
    return lbr_status_;
}

const std::wstring& EtwSession::SchedulerStackStatus() const {
    return scheduler_stack_status_;
}

bool EtwSession::IsActive() const {
    return handle_ != 0;
}

const std::wstring& EtwSession::Name() const {
    return name_;
}

ULONG TraceFlagsForProfile(std::wstring_view profile) {
    ULONG flags = EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD |
                  EVENT_TRACE_FLAG_IMAGE_LOAD | EVENT_TRACE_FLAG_PROFILE;
    if (profile == L"pmu") flags &= ~EVENT_TRACE_FLAG_PROFILE;
    if (profile == L"dpc" || profile == L"isr" || profile == L"heap" || profile == L"pool" || profile == L"driver")
        flags &= ~EVENT_TRACE_FLAG_PROFILE;
    if (profile == L"pool") flags |= EVENT_TRACE_FLAG_CSWITCH;
    if (profile == L"dpc") flags |= EVENT_TRACE_FLAG_DPC;
    if (profile == L"isr") flags |= EVENT_TRACE_FLAG_INTERRUPT;
    if (profile == L"driver") flags |= EVENT_TRACE_FLAG_DRIVER | EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_DISK_IO_INIT | EVENT_TRACE_FLAG_CSWITCH;
    if (profile == L"cpu-sched" || profile == L"sched") {
        flags |= EVENT_TRACE_FLAG_CSWITCH | EVENT_TRACE_FLAG_DISPATCHER;
    }
    if (profile == L"syscall") {
        flags |= EVENT_TRACE_FLAG_SYSTEMCALL;
    }
    if (profile == L"vm") {
        flags |= EVENT_TRACE_FLAG_MEMORY_PAGE_FAULTS |
                 EVENT_TRACE_FLAG_VIRTUAL_ALLOC | EVENT_TRACE_FLAG_VAMAP;
    }
    if (profile == L"io") {
        flags |= EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_DISK_IO_INIT | EVENT_TRACE_FLAG_DISK_FILE_IO |
                 EVENT_TRACE_FLAG_FILE_IO | EVENT_TRACE_FLAG_FILE_IO_INIT;
    }
    if (profile == L"kernel") {
        flags |= EVENT_TRACE_FLAG_DPC | EVENT_TRACE_FLAG_INTERRUPT |
                 EVENT_TRACE_FLAG_DRIVER;
    }
    if (profile == L"all") {
        flags |= EVENT_TRACE_FLAG_CSWITCH | EVENT_TRACE_FLAG_DISPATCHER |
                 EVENT_TRACE_FLAG_SYSTEMCALL | EVENT_TRACE_FLAG_MEMORY_PAGE_FAULTS |
                 EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_DISK_FILE_IO |
                 EVENT_TRACE_FLAG_FILE_IO | EVENT_TRACE_FLAG_FILE_IO_INIT;
    }
    return flags;
}
ULONG TraceFlagsForLiveFilter(std::wstring_view filter) {
    ULONG flags = EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD |
                  EVENT_TRACE_FLAG_IMAGE_LOAD;
    if (filter == L"lbr") {
        flags |= EVENT_TRACE_FLAG_PROFILE;
    } else if (filter == L"sched") {
        flags |= EVENT_TRACE_FLAG_CSWITCH | EVENT_TRACE_FLAG_DISPATCHER;
    } else if (filter == L"syscall") {
        flags |= EVENT_TRACE_FLAG_SYSTEMCALL;
    } else if (filter == L"pagefault" || filter == L"vm") {
        flags |= EVENT_TRACE_FLAG_MEMORY_PAGE_FAULTS;
    } else if (filter == L"io") {
        flags |= EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_DISK_IO_INIT | EVENT_TRACE_FLAG_DISK_FILE_IO |
                 EVENT_TRACE_FLAG_FILE_IO | EVENT_TRACE_FLAG_FILE_IO_INIT;
    } else if (filter == L"all" || filter.empty()) {
        flags |= EVENT_TRACE_FLAG_CSWITCH | EVENT_TRACE_FLAG_DISPATCHER |
                 EVENT_TRACE_FLAG_SYSTEMCALL | EVENT_TRACE_FLAG_MEMORY_PAGE_FAULTS |
                 EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_DISK_FILE_IO |
                 EVENT_TRACE_FLAG_FILE_IO | EVENT_TRACE_FLAG_FILE_IO_INIT;
    }
    return flags;
}

std::vector<ProfileSource> QueryProfileSources(std::wstring* error) {
    std::vector<ProfileSource> result;
    ULONG required = 0;
    ULONG query_result = TraceQueryInformation(
        0, TraceProfileSourceListInfo, nullptr, 0, &required);
    if (query_result != ERROR_INSUFFICIENT_BUFFER &&
        query_result != ERROR_BAD_LENGTH && query_result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = LastErrorWith(L"TraceQueryInformation(profile sources)",
                                   query_result);
        }
        return result;
    }
    if (required == 0) {
        return result;
    }

    std::vector<BYTE> buffer(required, 0);
    query_result = TraceQueryInformation(0, TraceProfileSourceListInfo, buffer.data(),
                                         static_cast<ULONG>(buffer.size()), &required);
    if (query_result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = LastErrorWith(L"TraceQueryInformation(profile sources)",
                                   query_result);
        }
        return result;
    }

    size_t offset = 0;
    while (offset + sizeof(PROFILE_SOURCE_INFO) <= buffer.size()) {
        const auto* source =
            reinterpret_cast<const PROFILE_SOURCE_INFO*>(buffer.data() + offset);
        result.push_back(ProfileSource{
            source->Source,
            source->MinInterval,
            source->MaxInterval,
            std::wstring(source->Description),
        });
        if (source->NextEntryOffset == 0) {
            break;
        }
        offset += source->NextEntryOffset;
    }
    return result;
}

std::vector<ProviderInfo> QueryProviders(std::wstring* error) {
    std::vector<ProviderInfo> result;
    ULONG required = 0;
    TDHSTATUS status = TdhEnumerateProviders(nullptr, &required);
    if (status != ERROR_INSUFFICIENT_BUFFER && status != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = L"TdhEnumerateProviders failed: " +
                     FormatWindowsError(status);
        }
        return result;
    }
    if (required == 0) {
        return result;
    }
    std::vector<BYTE> buffer(required, 0);
    status = TdhEnumerateProviders(
        reinterpret_cast<PPROVIDER_ENUMERATION_INFO>(buffer.data()), &required);
    if (status != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = L"TdhEnumerateProviders failed: " +
                     FormatWindowsError(status);
        }
        return result;
    }
    const auto* providers =
        reinterpret_cast<const PROVIDER_ENUMERATION_INFO*>(buffer.data());
    for (ULONG index = 0; index < providers->NumberOfProviders; ++index) {
        const auto& provider = providers->TraceProviderInfoArray[index];
        ProviderInfo info;
        info.id = provider.ProviderGuid;
        info.schema_source = provider.SchemaSource;
        if (provider.ProviderNameOffset < buffer.size()) {
            const auto* name = reinterpret_cast<const wchar_t*>(
                buffer.data() + provider.ProviderNameOffset);
            const size_t remaining =
                (buffer.size() - provider.ProviderNameOffset) / sizeof(wchar_t);
            size_t length = 0;
            while (length < remaining && name[length] != L'\0') {
                ++length;
            }
            info.name.assign(name, length);
        }
        result.push_back(std::move(info));
    }
    return result;
}

uint32_t QueryMaxLoggers(std::wstring* error) {
    ULONG value = 0;
    ULONG returned = 0;
    const ULONG result = TraceQueryInformation(
        0, TraceMaxLoggersQuery, &value, sizeof(value), &returned);
    if (result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = LastErrorWith(L"TraceQueryInformation(max loggers)", result);
        }
        return 0;
    }
    return value;
}

uint32_t QueryMaxPmcCounters(std::wstring* error) {
    ULONG value = 0;
    ULONG returned = 0;
    const ULONG result = TraceQueryInformation(
        0, TraceMaxPmcCounterQuery, &value, sizeof(value), &returned);
    if (result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = LastErrorWith(L"TraceQueryInformation(max PMC counters)",
                                   result);
        }
        return 0;
    }
    return value;
}

std::vector<PmcCounterOwnerInfo> QueryPmcCounterOwners(
    std::wstring* error) {
    std::vector<PmcCounterOwnerInfo> result;
    const uint32_t maximum = QueryMaxPmcCounters(nullptr);
    const DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (maximum == 0 || processors == 0) {
        return result;
    }

    const size_t header_size =
        offsetof(ETW_PMC_COUNTER_OWNERSHIP_STATUS, CounterOwners);
    std::vector<BYTE> buffer(
        header_size + static_cast<size_t>(maximum) *
                          sizeof(ETW_PMC_COUNTER_OWNER),
        0);
    bool queried = false;
    for (DWORD processor = 0; processor < processors; ++processor) {
        std::fill(buffer.begin(), buffer.end(), BYTE{0});
        auto* status =
            reinterpret_cast<ETW_PMC_COUNTER_OWNERSHIP_STATUS*>(buffer.data());
        status->ProcessorNumber = processor;
        ULONG returned = 0;
        const ULONG query_result = TraceQueryInformation(
            0, TracePmcCounterOwners, buffer.data(),
            static_cast<ULONG>(buffer.size()), &returned);
        if (query_result != ERROR_SUCCESS) {
            if (!queried && error != nullptr) {
                *error = LastErrorWith(L"TraceQueryInformation(PMC owners)",
                                       query_result);
            }
            continue;
        }
        queried = true;
        const uint32_t count =
            std::min<uint32_t>(status->NumberOfCounters, maximum);
        for (uint32_t index = 0; index < count; ++index) {
            const auto& owner = status->CounterOwners[index];
            if (owner.OwnerType == EtwPmcOwnerFree) {
                continue;
            }
            result.push_back(PmcCounterOwnerInfo{
                processor, static_cast<uint32_t>(owner.OwnerType),
                owner.ProfileSource, owner.OwnerTag});
        }
    }
    if (queried && error != nullptr) {
        error->clear();
    }
    return result;
}

std::vector<PmcSessionInfo> QueryPmcSessions(std::wstring* error) {
    std::vector<PmcSessionInfo> result;
    ULONG required = 0;
    ULONG query_result = TraceQueryInformation(
        0, TracePmcSessionInformation, nullptr, 0, &required);
    if (query_result != ERROR_INSUFFICIENT_BUFFER &&
        query_result != ERROR_BAD_LENGTH && query_result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = LastErrorWith(L"TraceQueryInformation(PMC sessions)",
                                   query_result);
        }
        return result;
    }
    if (required == 0) {
        return result;
    }
    std::vector<BYTE> buffer(required, 0);
    query_result = TraceQueryInformation(
        0, TracePmcSessionInformation, buffer.data(),
        static_cast<ULONG>(buffer.size()), &required);
    if (query_result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = LastErrorWith(L"TraceQueryInformation(PMC sessions)",
                                   query_result);
        }
        return result;
    }

    size_t offset = 0;
    const size_t length = std::min<size_t>(required, buffer.size());
    while (offset < length) {
        if (length - offset < sizeof(ETW_PMC_SESSION_INFO)) {
            if (error) *error = L"truncated PMC session header";
            return {};
        }
        ETW_PMC_SESSION_INFO info{};
        std::memcpy(&info, buffer.data() + offset, sizeof(info));
        const size_t available = info.NextEntryOffset == 0 ? length - offset : info.NextEntryOffset;
        const uint64_t bytes = sizeof(info) + uint64_t{info.ProfileSourceCount} * sizeof(ULONG) +
                               uint64_t{info.HookIdCount} * sizeof(USHORT);
        if (available > length - offset || available < bytes) {
            if (error) *error = L"invalid PMC session entry size";
            return {};
        }
        PmcSessionInfo session;
        session.logger_id = info.LoggerId;
        const BYTE* source = buffer.data() + offset + sizeof(info);
        for (ULONG i = 0; i < info.ProfileSourceCount; ++i) {
            ULONG value = 0;
            std::memcpy(&value, source + i * sizeof(ULONG), sizeof(value));
            session.sources.push_back(value);
        }
        const BYTE* hook = source + info.ProfileSourceCount * sizeof(ULONG);
        for (ULONG i = 0; i < info.HookIdCount; ++i) {
            USHORT value = 0;
            std::memcpy(&value, hook + i * sizeof(USHORT), sizeof(value));
            session.hook_ids.push_back(value);
        }
        result.push_back(std::move(session));
        if (info.NextEntryOffset == 0) break;
        offset += info.NextEntryOffset;
    }
    return result;
}

std::vector<std::wstring> QueryActiveSessions(std::wstring* error) {
    std::vector<std::wstring> result;
    ULONG count = 0;
    ULONG query_result = QueryAllTracesW(nullptr, 0, &count);
    if (query_result != ERROR_SUCCESS && query_result != ERROR_MORE_DATA) {
        if (error != nullptr) {
            *error = LastErrorWith(L"QueryAllTracesW(count)", query_result);
        }
        return result;
    }
    if (count == 0) {
        return result;
    }

    std::vector<std::vector<BYTE>> buffers(
        count, std::vector<BYTE>(kPropertiesBufferSize, 0));
    std::vector<PEVENT_TRACE_PROPERTIES> properties(count, nullptr);
    for (size_t index = 0; index < count; ++index) {
        properties[index] =
            reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buffers[index].data());
        properties[index]->Wnode.BufferSize = kPropertiesBufferSize;
    }

    query_result = QueryAllTracesW(properties.data(), count, &count);
    if (query_result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = LastErrorWith(L"QueryAllTracesW", query_result);
        }
        return result;
    }

    for (size_t index = 0; index < count; ++index) {
        const auto* item = properties[index];
        if (item->LoggerNameOffset != 0 &&
            item->LoggerNameOffset < buffers[index].size()) {
            const wchar_t* name = reinterpret_cast<const wchar_t*>(
                buffers[index].data() + item->LoggerNameOffset);
            if (name[0] != L'\0') {
                result.emplace_back(name);
            }
        }
    }
    return result;
}

bool SessionExists(const std::wstring& session_name) {
    std::vector<BYTE> buffer(kPropertiesBufferSize, 0);
    auto* properties = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buffer.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(buffer.size());
    const ULONG result = ControlTraceW(0, session_name.c_str(), properties,
                                       EVENT_TRACE_CONTROL_QUERY);
    return result == ERROR_SUCCESS;
}

bool StopSessionByName(const std::wstring& session_name, std::wstring* error) {
    std::vector<BYTE> buffer(kPropertiesBufferSize, 0);
    auto* properties = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buffer.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(buffer.size());
    const ULONG result = ControlTraceW(0, session_name.c_str(), properties,
                                       EVENT_TRACE_CONTROL_STOP);
    if (result != ERROR_SUCCESS && result != ERROR_WMI_INSTANCE_NOT_FOUND) {
        if (error != nullptr) {
            *error = LastErrorWith(L"ControlTraceW(stop by name)", result);
        }
        return false;
    }
    return true;
}

}  // namespace wperf
