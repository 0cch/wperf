#include "live_top.h"

#include "common.h"
#include "etw_decode.h"
#include "lbr.h"
#include "trace_events.h"

#include <algorithm>
#include <cstring>
#include <cerrno>
#include <iterator>
#include <utility>
#include <evntcons.h>

namespace wperf {
namespace {


const GUID kProcessGuid = {0x3d6fa8d0, 0xfe05, 0x11d0,
    {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
const GUID kThreadGuid = {0x3d6fa8d1, 0xfe05, 0x11d0,
                          {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
const GUID kImageGuid = {0x2cb15d1d, 0x5fc1, 0x11d2,
                         {0xab, 0xe1, 0x00, 0xa0, 0xc9, 0x11, 0xf5, 0x18}};
const GUID kPerfInfoGuid = {0xce1dbfb4, 0x137e, 0x4da6,
                            {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
const GUID kSystemTraceGuid = {0x9e814aad, 0x3204, 0x11d2,
                               {0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39}};
const GUID kPageFaultGuid = {0x3d6fa8d3, 0xfe05, 0x11d0,
                             {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
const GUID kDiskIoGuid = {0x3d6fa8d4, 0xfe05, 0x11d0,
                          {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
const GUID kFileIoGuid = {0x90cbdc39, 0x4a3e, 0x11d1,
                          {0x84, 0xf4, 0x00, 0x00, 0xf8, 0x04, 0x64, 0xe3}};
const GUID kLbrGuid = {0x99134383, 0x5248, 0x43fc,
                       {0x83, 0x4b, 0x52, 0x94, 0x54, 0xe7, 0x5d, 0xf3}};

GUID GetClassGuid(const EVENT_RECORD& record) {
    GUID result{};
    for (USHORT index = 0; index < record.ExtendedDataCount; ++index) {
        const auto& item = record.ExtendedData[index];
        if (item.ExtType == EVENT_HEADER_EXT_TYPE_CONTROL_GUID &&
            item.DataSize >= sizeof(GUID) && item.DataPtr != 0) {
            std::memcpy(&result, reinterpret_cast<const void*>(
                                     static_cast<uintptr_t>(item.DataPtr)),
                        sizeof(result));
            return result;
        }
    }
    return result;
}

bool IsZeroGuid(const GUID& guid) {
    static const GUID zero{};
    return IsEqualGUID(guid, zero) != FALSE;
}

uint8_t ReadU8(const BYTE* data, size_t size, size_t offset) {
    if (data == nullptr || offset >= size) {
        return 0;
    }
    return data[offset];
}

uint32_t ReadU32(const BYTE* data, size_t size, size_t offset) {
    if (data == nullptr || offset + sizeof(uint32_t) > size) {
        return 0;
    }
    uint32_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

uint64_t ReadU64(const BYTE* data, size_t size, size_t offset) {
    if (data == nullptr || offset + sizeof(uint64_t) > size) {
        return 0;
    }
    uint64_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

uint64_t ReadPointer(const BYTE* data, size_t size, size_t offset,
                     uint32_t pointer_size) {
    return pointer_size == 8 ? ReadU64(data, size, offset)
                             : ReadU32(data, size, offset);
}

std::wstring ReadWideString(const BYTE* data, size_t size, size_t offset) {
    if (data == nullptr || offset >= size) {
        return {};
    }
    const size_t chars = (size - offset) / sizeof(wchar_t);
    const auto* text = reinterpret_cast<const wchar_t*>(data + offset);
    size_t length = 0;
    while (length < chars && text[length] != L'\0') {
        ++length;
    }
    return length == 0 ? std::wstring{} : std::wstring(text, length);
}

uint64_t QpcToNs(uint64_t qpc, uint64_t frequency) {
    if (frequency == 0) {
        frequency = 10000000;
    }
    return (qpc / frequency) * 1000000000ull +
           ((qpc % frequency) * 1000000000ull) / frequency;
}

}  // namespace

LiveTopCapture::~LiveTopCapture() {
    Stop();
}

bool LiveTopCapture::Start(std::wstring session_name,
                           std::wstring profile_source, ULONG enable_flags,
                           bool enable_lbr, std::wstring* error) {
    if (running_.load()) {
        if (error != nullptr) {
            *error = L"live top capture is already running";
        }
        return false;
    }

    if (worker_.joinable()) Stop();
    {
        std::lock_guard lock(mutex_);
        samples_.clear(); events_.clear(); images_.clear(); threads_.clear(); processes_.clear();
        thread_to_process_.clear(); process_threads_.clear(); metadata_changes_.clear(); buffered_bytes_=0; sample_metadata_ = {};
        events_total_ = unknown_events_ = ring_dropped_ = decode_errors_ = metadata_dropped_ = 0;
        lost_events_ = lost_buffers_ = lbr_events_ = lbr_branches_ = 0;
        last_error_.clear();
    }
    correlation_.Reset();
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        qpc_frequency_ = static_cast<uint64_t>(frequency.QuadPart);
    }

    TraceOptions options;
    options.session_name = session_name;
    options.realtime = true;
    options.enable_flags =
        enable_flags != 0
            ? enable_flags
            : (EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD |
               EVENT_TRACE_FLAG_IMAGE_LOAD | EVENT_TRACE_FLAG_PROFILE);
    options.profile_interval_100ns = 10000;
    options.enable_stack_tracing = false;
    options.enable_lbr = enable_lbr;
    options.buffer_size_kb = 64;
    options.minimum_buffers = 8;
    options.maximum_buffers = 128;
    options.flush_timer_seconds = 0;
    if (!profile_source.empty() && profile_source != L"cpu") {
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long source =
            wcstoul(profile_source.c_str(), &end, 10);
        options.pmc_profile = true;
        if (errno != ERANGE && end != profile_source.c_str() && *end == L'\0' &&
            profile_source.front() >= L'0' && profile_source.front() <= L'9') {
            options.pmc_source = static_cast<uint32_t>(source);
        } else {
            const auto sources = QueryProfileSources(error);
            const auto found = std::find_if(sources.begin(), sources.end(), [&](const auto& entry) {
                return _wcsicmp(entry.description.c_str(), profile_source.c_str()) == 0;
            });
            if (found == sources.end()) {
                if (error) *error = L"requested top PMC source was not found";
                return false;
            }
            options.pmc_source = found->source;
        }
    }
    if (!session_.Start(options, error)) {
        return false;
    }
    session_name_ = std::move(session_name);

    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LoggerName = session_name_.data();
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD |
                               PROCESS_TRACE_MODE_REAL_TIME |
                               PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    logfile.Context = this;
    logfile.EventRecordCallback = &LiveTopCapture::EventCallback;
    trace_handle_ = OpenTraceW(&logfile);
    if (trace_handle_ == INVALID_PROCESSTRACE_HANDLE) {
        if (error != nullptr) {
            *error = L"OpenTraceW failed: " + FormatWindowsError(GetLastError());
        }
        std::wstring ignored;
        session_.Stop(&ignored);
        return false;
    }

    pointer_size_ = logfile.LogfileHeader.PointerSize == 4 ||
                            logfile.LogfileHeader.PointerSize == 32 ? 4 : 8;
    if (logfile.LogfileHeader.PerfFreq.QuadPart > 0) {
        qpc_frequency_ = static_cast<uint64_t>(logfile.LogfileHeader.PerfFreq.QuadPart);
    }
    lost_events_ = logfile.LogfileHeader.EventsLost;
    sample_metadata_.session_name=session_name_;
    sample_metadata_.pointer_size = pointer_size_;
    sample_metadata_.qpc_frequency = qpc_frequency_;
    profile_source_id_ = options.pmc_profile ? options.pmc_source : 0;
    sample_metadata_.profile_intervals[profile_source_id_] = session_.RestoreInfo().configured_interval;
    if (!options.pmc_profile) sample_metadata_.profile_interval_ns =
        uint64_t{session_.RestoreInfo().configured_interval} * 100;
    events_total_ = 0;
    stopping_.store(false);
    running_.store(true);
    worker_ = std::thread([this]() {
        TRACEHANDLE handle = trace_handle_.load();
        const ULONG result = ProcessTrace(&handle, 1, nullptr, nullptr);
        const TRACEHANDLE owned = trace_handle_.exchange(INVALID_PROCESSTRACE_HANDLE);
        if (owned != INVALID_PROCESSTRACE_HANDLE) CloseTrace(owned);
        if (!stopping_.load()) {
            std::lock_guard lock(mutex_);
            last_error_ = result == ERROR_SUCCESS ? L"live ETW consumer stopped unexpectedly"
                : L"ProcessTrace failed: " + FormatWindowsError(result);
        }
        running_.store(false);
    });
    return true;
}

bool LiveTopCapture::Stop() {
    if (stopping_.exchange(true)) {
        return !session_.IsActive();
    }
    std::wstring stop_error;
    session_.Stop(&stop_error);
    // CloseTrace also cancels ProcessTrace if stopping the session failed.
    const TRACEHANDLE consumer = trace_handle_.exchange(INVALID_PROCESSTRACE_HANDLE);
    if (consumer != INVALID_PROCESSTRACE_HANDLE) CloseTrace(consumer);
    if (worker_.joinable()) worker_.join();
    running_.store(false);
    if (!stop_error.empty()) {
        std::lock_guard lock(mutex_);
        last_error_ = stop_error;
    }
    return stop_error.empty();
}

ProfileRestoreInfo LiveTopCapture::RestoreInfo() const { return session_.RestoreInfo(); }
uint32_t LiveTopCapture::ProfileSourceId() const { return profile_source_id_; }

bool LiveTopCapture::Running() const {
    return running_.load();
}

std::wstring LiveTopCapture::LastError() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

TraceData LiveTopCapture::Snapshot() {
    std::lock_guard snapshot_lock(snapshot_mutex_);
    TraceData trace;
    std::deque<SampleRecord> samples;
    std::deque<TraceEvent> events;
    std::deque<MetadataChange> changes;
    TraceQuality session_quality;
    const bool quality_available = session_.QueryQuality(session_quality, nullptr);
    {
        std::lock_guard lock(mutex_);
        trace.pointer_size = pointer_size_;
        trace.qpc_frequency = qpc_frequency_;
        trace.profile_interval_ns = sample_metadata_.profile_interval_ns;
        trace.profile_intervals = sample_metadata_.profile_intervals;
        trace.pmc_counter_sources=sample_metadata_.pmc_counter_sources; trace.pmc_source_names=sample_metadata_.pmc_source_names;
        changes.swap(metadata_changes_);
        buffered_bytes_=0;
        samples.swap(samples_);
        events.swap(events_);
        trace.quality.events_total = std::exchange(events_total_, 0);
        trace.quality.unknown_events = std::exchange(unknown_events_, 0);
        trace.quality.ring_dropped = std::exchange(ring_dropped_, 0);
        trace.quality.decode_errors = std::exchange(decode_errors_, 0);
        trace.quality.pmc_counter_events=std::exchange(pmc_counter_events_,0);
        trace.quality.incomplete = !quality_available;
        trace.quality.metadata_dropped = std::exchange(metadata_dropped_, 0);
        trace.quality.lbr_events = std::exchange(lbr_events_, 0);
        trace.quality.lbr_branches = std::exchange(lbr_branches_, 0);
        trace.quality.lost_events = session_quality.lost_events >= lost_events_
            ? session_quality.lost_events - lost_events_ : 0;
        trace.quality.lost_buffers = session_quality.lost_buffers >= lost_buffers_
            ? session_quality.lost_buffers - lost_buffers_ : 0;
        if(quality_available) { lost_events_ = session_quality.lost_events; lost_buffers_ = session_quality.lost_buffers; }
    }
    ApplyMetadata(changes,trace.quality);
    trace.images=images_;
    trace.threads=threads_;
    trace.processes=processes_;
    trace.samples.assign(std::make_move_iterator(samples.begin()), std::make_move_iterator(samples.end()));
    trace.events.assign(std::make_move_iterator(events.begin()), std::make_move_iterator(events.end()));
    trace.quality.samples_total = trace.samples.size();
    ThreadIndex thread_index; thread_index.Build(trace);
    for (auto& sample : trace.samples) {
        if (sample.pid == 0 || sample.pid == UINT32_MAX) {
            if (const auto* thread = thread_index.Find(sample.tid,sample.timestamp_ns)) sample.pid = thread->pid;
        }
        if (sample.kind == SampleKind::Pmc) ++trace.quality.pmc_samples;
    }
    if (trace.quality.ring_dropped || trace.quality.lost_events || trace.quality.lost_buffers ||
        trace.quality.metadata_dropped)
        correlation_.Reset();
    correlation_.Enrich(trace);
    return trace;
}

void LiveTopCapture::QueueMetadata(MetadataChange change) {
    const size_t bytes=sizeof(change)+2*(change.image.path.size()+change.image.resolved_path.size()+change.process.image_name.size()+change.process.command_line.size());
    if(metadata_changes_.size()>=200000 || buffered_bytes_+bytes>buffer_budget_bytes_) {
        ++metadata_dropped_; return;
    }
    buffered_bytes_+=bytes;
    metadata_changes_.push_back(std::move(change));
}
void LiveTopCapture::QueueEvent(TraceEvent event) {
    const size_t bytes=sizeof(event)+2*event.file_path.size()+8*event.pmc_counters.size();
    if(buffered_bytes_+bytes>buffer_budget_bytes_ || events_.size()>=200000) { ++ring_dropped_; return; }
    buffered_bytes_+=bytes;
    events_.push_back(std::move(event));
}
void LiveTopCapture::ApplyMetadata(std::deque<MetadataChange>& changes,TraceQuality& quality) {
    uint64_t now=0;
    for(auto& change:changes) {
        now=std::max(now,change.time);
        if(change.kind==MetadataChange::Kind::Image) {
            auto& image=change.image;
            if(change.opcode==2||change.opcode==4) {
                for(auto it=images_.rbegin();it!=images_.rend();++it)
                    if(it->pid==image.pid&&it->base==image.base&&it->unload_ns==0) { it->unload_ns=image.load_ns; break; }
            } else {
                const auto old=std::find_if(images_.begin(),images_.end(),[&](const auto& value) {
                    return value.pid==image.pid&&value.base==image.base&&value.unload_ns==0&&ModuleIdentityKey(value)==ModuleIdentityKey(image);
                });
                if(old==images_.end())images_.push_back(std::move(image));
            }
        } else if(change.kind==MetadataChange::Kind::Thread) {
            auto& thread=change.thread;
            auto old=std::find_if(threads_.rbegin(),threads_.rend(),[&](const auto& value) {
                return value.pid==thread.pid&&value.tid==thread.tid&&value.end_ns==0;
            });
            if(change.opcode==2||change.opcode==4) { if(old!=threads_.rend())old->end_ns=thread.start_ns; }
            else if(old==threads_.rend())threads_.push_back(thread);
            else if(change.opcode==1 && old->start_ns!=thread.start_ns) { old->end_ns=thread.start_ns; threads_.push_back(thread); }
        } else {
            auto previous=std::find_if(processes_.rbegin(),processes_.rend(),[&](const auto& value){return value.pid==change.pid&&value.end_ns==0;});
            if(change.opcode==2||change.opcode==4) {
                if(previous!=processes_.rend())previous->end_ns=change.time;
                for(auto& image:images_)if(image.pid==change.pid&&image.unload_ns==0)image.unload_ns=change.time;
                for(auto& thread:threads_)if(thread.pid==change.pid&&thread.end_ns==0)thread.end_ns=change.time;
            } else if(previous==processes_.rend())processes_.push_back(std::move(change.process));
            else if(change.opcode==1&&previous->start_ns!=change.time) { previous->end_ns=change.time; processes_.push_back(std::move(change.process)); }
        }
    }
    constexpr uint64_t retention=10000000000ull;
    std::erase_if(images_,[&](const auto& image){return image.unload_ns&&now>image.unload_ns&&now-image.unload_ns>retention;});
    std::erase_if(processes_,[&](const auto& process){return process.end_ns&&now>process.end_ns&&now-process.end_ns>retention;});
    std::erase_if(threads_,[&](const auto& thread){return thread.end_ns&&now>thread.end_ns&&now-thread.end_ns>retention;});
    size_t metadata_bytes=threads_.size()*sizeof(ThreadRecord)+images_.size()*sizeof(ImageRecord)+processes_.size()*sizeof(ProcessRecord);
    for(const auto& process:processes_)metadata_bytes+=2*(process.image_name.size()+process.command_line.size());
    for(const auto& image:images_)metadata_bytes+=2*(image.path.size()+image.resolved_path.size());
    constexpr size_t budget=64*1024*1024;
    size_t erase=0;
    while(metadata_bytes>budget && erase<images_.size()) {
        metadata_bytes-=sizeof(ImageRecord)+2*(images_[erase].path.size()+images_[erase].resolved_path.size()); ++erase;
    }
    if(erase) { quality.metadata_dropped+=erase; images_.erase(images_.begin(),images_.begin()+erase); }
    erase=0;
    while(metadata_bytes>budget&&erase<processes_.size()) {
        metadata_bytes-=sizeof(ProcessRecord)+2*(processes_[erase].image_name.size()+processes_[erase].command_line.size()); ++erase;
    }
    if(erase) { quality.metadata_dropped+=erase; processes_.erase(processes_.begin(),processes_.begin()+erase); }
    if(images_.size()>200000) { quality.metadata_dropped+=images_.size()-200000; images_.erase(images_.begin(),images_.end()-200000); }
    if(processes_.size()>200000) { quality.metadata_dropped+=processes_.size()-200000; processes_.erase(processes_.begin(),processes_.end()-200000); }
    if(threads_.size()>200000) { quality.metadata_dropped+=threads_.size()-200000; threads_.erase(threads_.begin(),threads_.end()-200000); }
}

void WINAPI LiveTopCapture::EventCallback(PEVENT_RECORD record) {
    if (record == nullptr || record->UserContext == nullptr) {
        return;
    }
    static_cast<LiveTopCapture*>(record->UserContext)->OnEvent(record);
}

void LiveTopCapture::OnEvent(PEVENT_RECORD record) {
    if (record == nullptr || record->UserData == nullptr || stopping_.load()) {
        return;
    }
    std::lock_guard lock(mutex_);
    ++events_total_;
    if(IsEqualGUID(record->EventHeader.ProviderId,kWperfMetadataGuid)) {
        if(!DecodeWperfMetadata(*record,sample_metadata_))++decode_errors_;
        return;
    }
    if ((record->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) != 0) {
        pointer_size_ = 4;
    } else if ((record->EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER) != 0) {
        pointer_size_ = 8;
    }

    GUID class_guid = GetClassGuid(*record);
    if (IsZeroGuid(class_guid)) {
        class_guid = record->EventHeader.ProviderId;
    }
    const uint32_t event_code = record->EventHeader.EventDescriptor.Opcode != 0
                                    ? record->EventHeader.EventDescriptor.Opcode
                                    : record->EventHeader.EventDescriptor.Id;

    const bool thread_event =
        IsEqualGUID(class_guid, kThreadGuid) ||
        IsEqualGUID(record->EventHeader.ProviderId, kThreadGuid);
    const bool perf_event =
        IsEqualGUID(class_guid, kPerfInfoGuid) ||
        IsEqualGUID(record->EventHeader.ProviderId, kPerfInfoGuid) ||
        IsEqualGUID(record->EventHeader.ProviderId, kSystemTraceGuid);

    if (IsEqualGUID(class_guid, kProcessGuid)) {
        ParseProcessExit(record);
    } else if ((IsEqualGUID(class_guid, kLbrGuid) ||
         IsEqualGUID(record->EventHeader.ProviderId, kLbrGuid)) &&
        event_code == 32) {
        ParseLbr(record);
    } else if (IsEqualGUID(class_guid, kImageGuid) ||
        IsEqualGUID(record->EventHeader.ProviderId, kImageGuid)) {
        ParseImage(record);
    } else if (thread_event) {
        if (event_code >= 1 && event_code <= 4) {
            ParseThread(record);
        } else if (event_code == 36 || event_code == 50) {
            ParseEvent(record, event_code == 36 ? TraceEventKind::ContextSwitch
                                                : TraceEventKind::ReadyThread);
        } else {
            ++unknown_events_;
        }
    } else if (perf_event && (event_code == 46 || event_code == 47)) {
        ParseSample(record);
    } else if (perf_event && event_code >= 72 && event_code <= 74) {
        if (!DecodeProfileInterval(*record, sample_metadata_)) ++decode_errors_;
    } else if (perf_event && event_code == 51) {
        const uint64_t address = ReadPointer(
            static_cast<const BYTE*>(record->UserData), record->UserDataLength, 0,
            pointer_size_);
        ParseEvent(record, TraceEventKind::SyscallEnter, address);
    } else if (perf_event && event_code == 52) {
        ParseEvent(record, TraceEventKind::SyscallExit);
    } else if (IsEqualGUID(class_guid, kPageFaultGuid) ||
               IsEqualGUID(record->EventHeader.ProviderId, kPageFaultGuid)) {
        if (event_code >= 10 && event_code <= 15) {
            const auto* data = static_cast<const BYTE*>(record->UserData);
            const uint64_t address =
                ReadPointer(data, record->UserDataLength, 0, pointer_size_);
            const uint64_t program_counter = ReadPointer(
                data, record->UserDataLength, pointer_size_, pointer_size_);
            ParseEvent(record, TraceEventKind::PageFault, address, program_counter);
        } else {
            ++unknown_events_;
        }
    } else if (IsEqualGUID(class_guid, kDiskIoGuid) ||
               IsEqualGUID(record->EventHeader.ProviderId, kDiskIoGuid)) {
        ParseEvent(record, TraceEventKind::DiskIo);
    } else if (IsEqualGUID(class_guid, kFileIoGuid) ||
               IsEqualGUID(record->EventHeader.ProviderId, kFileIoGuid)) {
        ParseEvent(record, TraceEventKind::FileIo);
    } else {
        ++unknown_events_;
    }
}

void LiveTopCapture::ParseThread(PEVENT_RECORD record) {
    ThreadRecord thread;
    if(!DecodeThreadPayload(*record,qpc_frequency_,thread)) { ++decode_errors_; return; }
    if(thread.pid==UINT32_MAX || thread.tid==0 || thread.tid==UINT32_MAX)return;
    const auto code=record->EventHeader.EventDescriptor.Opcode;
    const auto existing=thread_to_process_.find(thread.tid);
    if(code==2||code==4) {
        if(existing!=thread_to_process_.end()&&existing->second.pid==thread.pid&&existing->second.start_ns<=thread.start_ns) {
            thread_to_process_.erase(existing);
            auto owner=process_threads_.find(thread.pid);
            if(owner!=process_threads_.end()) { owner->second.erase(thread.tid); if(owner->second.empty())process_threads_.erase(owner); }
        }
    } else {
        if(existing!=thread_to_process_.end()&&existing->second.pid!=thread.pid)process_threads_[existing->second.pid].erase(thread.tid);
        if(code!=3||existing==thread_to_process_.end())thread_to_process_[thread.tid]=thread;
        process_threads_[thread.pid].insert(thread.tid);
        if(thread_to_process_.size()>200000) {
            thread_to_process_.clear(); process_threads_.clear(); ++metadata_dropped_;
        }
    }
    MetadataChange change; change.kind=MetadataChange::Kind::Thread; change.opcode=code;
    change.time=thread.start_ns; change.thread=thread; QueueMetadata(std::move(change));
}

void LiveTopCapture::ParseLbr(PEVENT_RECORD record) {
    LbrSample decoded;
    if (!DecodeLbrPayload(static_cast<const BYTE*>(record->UserData),
                          record->UserDataLength, pointer_size_, decoded)) {
        ++decode_errors_; return;
    }
    ++lbr_events_;
    lbr_branches_ += decoded.branches.size();
    for (const LbrBranch& branch : decoded.branches) {
        TraceEvent event;
        event.timestamp_ns = QpcToNs(decoded.timestamp_qpc, qpc_frequency_);
        event.cpu = EtwProcessorIndex(*record);
        event.pid = decoded.pid;
        event.tid = decoded.tid;
        event.kind = TraceEventKind::LastBranchRecord;
        event.value1 = branch.from_address;
        event.value2 = branch.to_address;
        event.flags = decoded.filters;
        event.event_id = record->EventHeader.EventDescriptor.Opcode;
        QueueEvent(std::move(event));
    }
}

void LiveTopCapture::ParseEvent(PEVENT_RECORD record, TraceEventKind kind,
                                uint64_t value1, uint64_t value2) {
    (void)value1; (void)value2;
    TraceEvent event;
    if(!DecodeKernelEvent(*record,pointer_size_,qpc_frequency_,kind,event)) { ++decode_errors_; return; }
    TraceQuality quality;
    CopyEtwPmcCounters(*record,event.pmc_counters,quality);
    decode_errors_+=quality.decode_errors;
    pmc_counter_events_+=quality.pmc_counter_events;
    QueueEvent(std::move(event));
}

void LiveTopCapture::ParseImage(PEVENT_RECORD record) {
    ImageRecord image;
    if(!DecodeImagePayload(*record,pointer_size_,qpc_frequency_,image)) { ++decode_errors_; return; }
    MetadataChange change; change.kind=MetadataChange::Kind::Image;
    change.opcode=record->EventHeader.EventDescriptor.Opcode; change.time=image.load_ns;
    change.image=std::move(image); QueueMetadata(std::move(change));
}

void LiveTopCapture::ParseSample(PEVENT_RECORD record) {
    sample_metadata_.pointer_size = pointer_size_;
    sample_metadata_.qpc_frequency = qpc_frequency_;
    SampleRecord sample;
    if (!DecodeProfileSample(*record, sample_metadata_, sample)) {
        ++decode_errors_;
        return;
    }
    if (sample.pid == 0 || sample.pid == UINT32_MAX) {
        const auto found = thread_to_process_.find(sample.tid);
        if (found != thread_to_process_.end() && sample.timestamp_ns >= found->second.start_ns)
            sample.pid = found->second.pid;
    }
    if(buffered_bytes_+sizeof(sample)>buffer_budget_bytes_) { ++ring_dropped_; return; }
    buffered_bytes_+=sizeof(sample);
    samples_.push_back(std::move(sample));
    constexpr size_t kMaximumSamples = 200000;
    if (samples_.size() > kMaximumSamples) {
        samples_.pop_front();
        ++ring_dropped_;
    }
}

void LiveTopCapture::ParseProcessExit(PEVENT_RECORD record) {
    const auto code=record->EventHeader.EventDescriptor.Opcode;
    if(code!=1&&code!=2&&code!=3&&code!=4)return;
    ProcessRecord process;
    if(!DecodeProcessPayload(*record,pointer_size_,qpc_frequency_,process)) { ++decode_errors_; return; }
    if(code==2||code==4) {
        const auto owner=process_threads_.find(process.pid);
        if(owner!=process_threads_.end()) {
            for(uint32_t tid:owner->second)thread_to_process_.erase(tid);
            process_threads_.erase(owner);
        }
    }
    MetadataChange change; change.kind=MetadataChange::Kind::Process; change.pid=process.pid;
    change.opcode=code; change.time=process.start_ns; change.process=std::move(process);
    QueueMetadata(std::move(change));
}

uint64_t LiveTopCapture::TimestampToNs(const EVENT_RECORD& record) const {
    return QpcToNs(static_cast<uint64_t>(record.EventHeader.TimeStamp.QuadPart),
                   qpc_frequency_);
}

}  // namespace wperf
