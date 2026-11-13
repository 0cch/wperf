#include "etw.h"

#include "common.h"

#include <algorithm>
#include <vector>

namespace wperf {
namespace {

constexpr ULONG kPropertiesBufferSize = 64 * 1024;
constexpr size_t kAlignment = 8;
const GUID kPerfInfoGuid = {0xce1dbfb4, 0x137e, 0x4da6,
                            {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};

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
}

ULONG ConfigureLbrCompat(CONTROLTRACE_ID trace_id, TRACE_LBR_CONFIGURATION configuration,
                         const CLASSIC_EVENT_ID* events, ULONG event_count);
bool EtwSession::Start(const TraceOptions& options, std::wstring* error) {
    context_register_status_.clear();
    lbr_status_.clear();
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
    std::wstring privilege_error;
    if (!EnablePrivilege(L"SeSystemProfilePrivilege", &privilege_error)) {
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
    const size_t total_size = logger_offset + logger_bytes;
    auto buffer = MakePropertiesBuffer(total_size - sizeof(EVENT_TRACE_PROPERTIES));
    auto* properties = InitializeProperties(buffer, options.session_name, log_file);
    properties->BufferSize = options.buffer_size_kb;
    properties->MinimumBuffers = options.minimum_buffers;
    properties->MaximumBuffers = options.maximum_buffers;
    properties->MaximumFileSize = options.maximum_file_size_mb;
    properties->FlushTimer = options.flush_timer_seconds;
    properties->EnableFlags = options.enable_flags;
    if (options.pmc_profile) {
        properties->EnableFlags |= EVENT_TRACE_FLAG_CSWITCH |
                                   EVENT_TRACE_FLAG_DISPATCHER |
                                   EVENT_TRACE_FLAG_SYSTEMCALL;
    }
    properties->LogFileMode =
        (options.realtime ? EVENT_TRACE_REAL_TIME_MODE
                          : EVENT_TRACE_FILE_MODE_SEQUENTIAL) |
        EVENT_TRACE_USE_GLOBAL_SEQUENCE | EVENT_TRACE_SYSTEM_LOGGER_MODE;

    TRACEHANDLE handle = 0;
    const ULONG start_result =
        StartTraceW(&handle, options.session_name.c_str(), properties);
    if (start_result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = LastErrorWith(L"StartTraceW", start_result);
        }
        return false;
    }

    handle_ = handle;
    name_ = options.session_name;

    if (options.pmc_profile) {
        ULONG source = options.pmc_source;
        const ULONG source_result = TraceSetInformation(
            0, TraceProfileSourceConfigInfo, &source, sizeof(source));
        if (source_result != ERROR_SUCCESS) {
            pmc_config_changed_ = false;
            std::wstring stop_error;
            Stop(&stop_error);
            if (error != nullptr) {
                *error = LastErrorWith(L"TraceSetInformation(PMC source)",
                                       source_result);
            }
            return false;
        }
    }

    TRACE_PROFILE_INTERVAL interval{};
    interval.Source = options.pmc_profile ? options.pmc_source : 0;
    interval.Interval = options.profile_interval_100ns;
    ULONG returned = 0;
    const ULONG query_result = TraceQueryInformation(
        0, TraceSampledProfileIntervalInfo, &original_interval_,
        sizeof(original_interval_), &returned);
    original_interval_valid_ = query_result == ERROR_SUCCESS;

    const ULONG interval_result = TraceSetInformation(
        0, TraceSampledProfileIntervalInfo, &interval, sizeof(interval));
    if (interval_result != ERROR_SUCCESS) {
        std::wstring stop_error;
        Stop(&stop_error);
        if (error != nullptr) {
            *error = LastErrorWith(L"TraceSetInformation(profile interval)",
                                   interval_result);
        }
        return false;
    }

    if (options.enable_stack_tracing) {
        CLASSIC_EVENT_ID stack_event{};
        stack_event.EventGuid = kPerfInfoGuid;
        stack_event.Type = 46;
        const ULONG stack_result = TraceSetInformation(
            handle_, TraceStackTracingInfo, &stack_event, sizeof(stack_event));
        if (stack_result != ERROR_SUCCESS) {
            std::wstring stop_error;
            Stop(&stop_error);
            if (error != nullptr) {
                *error = LastErrorWith(L"TraceSetInformation(stack tracing)",
                                       stack_result);
            }
            return false;
        }
    }

    if (options.enable_context_registers) {
        struct ContextRequest {
            TRACE_CONTEXT_REGISTER_INFO Info;
            CLASSIC_EVENT_ID Event;
        } request{};
        request.Info.RegisterTypes = static_cast<ETW_CONTEXT_REGISTER_TYPES>(
            EtwContextRegisterTypeControl | EtwContextRegisterTypeInteger);
        request.Event.EventGuid = kPerfInfoGuid;
        request.Event.Type = 46;
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
        lbr_event.Type = 46;
        const ULONG lbr_result = ConfigureLbrCompat(
            handle_, TRACE_LBR_CONFIGURATION_CALLSTACK_ENABLE, &lbr_event, 1);
        lbr_status_ = lbr_result == ERROR_SUCCESS
                          ? L"enabled"
                          : L"unsupported: " +
                                FormatWindowsError(lbr_result);
        if (lbr_result != ERROR_SUCCESS) {
            std::wstring stop_error;
            Stop(&stop_error);
            if (error != nullptr) {
                *error = LastErrorWith(L"TraceConfigureLastBranchRecord", lbr_result);
            }
            return false;
        }
    }

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
    if (!IsActive()) {
        return true;
    }

    std::vector<BYTE> buffer(kPropertiesBufferSize, 0);
    auto* properties = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buffer.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(buffer.size());
    const ULONG stop_result = ControlTraceW(handle_, name_.c_str(), properties,
                                            EVENT_TRACE_CONTROL_STOP);

    if (original_interval_valid_) {
        TraceSetInformation(0, TraceSampledProfileIntervalInfo, &original_interval_,
                            sizeof(original_interval_));
        original_interval_valid_ = false;
    }

    handle_ = 0;
    name_.clear();
    if (stop_result != ERROR_SUCCESS && stop_result != ERROR_WMI_INSTANCE_NOT_FOUND) {
        if (error != nullptr) {
            *error = LastErrorWith(L"ControlTraceW(stop)", stop_result);
        }
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

bool EtwSession::IsActive() const {
    return handle_ != 0;
}

const std::wstring& EtwSession::Name() const {
    return name_;
}

ULONG TraceFlagsForProfile(std::wstring_view profile) {
    ULONG flags = EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD |
                  EVENT_TRACE_FLAG_IMAGE_LOAD | EVENT_TRACE_FLAG_PROFILE;
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
        flags |= EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_DISK_FILE_IO |
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
    if (filter == L"sched") {
        flags |= EVENT_TRACE_FLAG_CSWITCH | EVENT_TRACE_FLAG_DISPATCHER;
    } else if (filter == L"syscall") {
        flags |= EVENT_TRACE_FLAG_SYSTEMCALL;
    } else if (filter == L"pagefault" || filter == L"vm") {
        flags |= EVENT_TRACE_FLAG_MEMORY_PAGE_FAULTS;
    } else if (filter == L"io") {
        flags |= EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_DISK_FILE_IO |
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
    while (offset + sizeof(ETW_PMC_SESSION_INFO) <= buffer.size()) {
        const auto* info =
            reinterpret_cast<const ETW_PMC_SESSION_INFO*>(buffer.data() + offset);
        PmcSessionInfo session;
        session.logger_id = info->LoggerId;
        const auto* sources = reinterpret_cast<const ULONG*>(info + 1);
        for (ULONG index = 0; index < info->ProfileSourceCount; ++index) {
            session.sources.push_back(sources[index]);
        }
        const auto* hooks = reinterpret_cast<const USHORT*>(
            sources + info->ProfileSourceCount);
        for (ULONG index = 0; index < info->HookIdCount; ++index) {
            session.hook_ids.push_back(hooks[index]);
        }
        result.push_back(std::move(session));
        if (info->NextEntryOffset == 0) {
            break;
        }
        offset += info->NextEntryOffset;
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