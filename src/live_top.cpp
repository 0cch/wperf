#include "live_top.h"

#include "common.h"

#include <algorithm>
#include <cstring>
#include <evntcons.h>

namespace wperf {
namespace {


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
                           std::wstring profile_source, std::wstring* error) {
    if (running_.load()) {
        if (error != nullptr) {
            *error = L"live top capture is already running";
        }
        return false;
    }

    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        qpc_frequency_ = static_cast<uint64_t>(frequency.QuadPart);
    }

    TraceOptions options;
    options.session_name = session_name;
    options.realtime = true;
    options.enable_flags = EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD |
                           EVENT_TRACE_FLAG_IMAGE_LOAD | EVENT_TRACE_FLAG_PROFILE;
    options.profile_interval_100ns = 10000;
    options.enable_stack_tracing = false;
    options.buffer_size_kb = 64;
    options.minimum_buffers = 8;
    options.maximum_buffers = 128;
    options.flush_timer_seconds = 0;
    if (!profile_source.empty() && profile_source != L"cpu") {
        wchar_t* end = nullptr;
        const unsigned long source =
            wcstoul(profile_source.c_str(), &end, 10);
        if (end == nullptr || *end != L'\0') {
            if (error != nullptr) {
                *error = L"top event must be cpu or a numeric profile source";
            }
            return false;
        }
        options.pmc_profile = true;
        options.pmc_source = static_cast<uint32_t>(source);
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

    pointer_size_ = logfile.LogfileHeader.PointerSize == 32 ? 4 : 8;
    if (logfile.LogfileHeader.PerfFreq.QuadPart > 0) {
        qpc_frequency_ = static_cast<uint64_t>(logfile.LogfileHeader.PerfFreq.QuadPart);
    }
    lost_events_ = logfile.LogfileHeader.EventsLost;
    events_total_ = 0;
    stopping_.store(false);
    running_.store(true);
    worker_ = std::thread([this]() {
        TRACEHANDLE handle = trace_handle_;
        const ULONG result = ProcessTrace(&handle, 1, nullptr, nullptr);
        CloseTrace(handle);
        trace_handle_ = INVALID_PROCESSTRACE_HANDLE;
        running_.store(false);
        if (result != ERROR_SUCCESS && !stopping_.load()) {
            std::lock_guard lock(mutex_);
            last_error_ = L"ProcessTrace failed: " + FormatWindowsError(result);
        }
    });
    return true;
}

void LiveTopCapture::Stop() {
    if (stopping_.exchange(true)) {
        return;
    }
    std::wstring stop_error;
    session_.Stop(&stop_error);
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
    if (!stop_error.empty()) {
        std::lock_guard lock(mutex_);
        last_error_ = stop_error;
    }
}

bool LiveTopCapture::Running() const {
    return running_.load();
}

std::wstring LiveTopCapture::LastError() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

TraceData LiveTopCapture::Snapshot() {
    TraceData trace;
    std::lock_guard lock(mutex_);
    trace.pointer_size = pointer_size_;
    trace.qpc_frequency = qpc_frequency_;
    trace.profile_interval_ns = 1000000;
    trace.images = images_;
    trace.threads = threads_;
    trace.quality.events_total = events_total_;
    trace.quality.lost_events = lost_events_;
    trace.quality.ring_dropped = ring_dropped_;
    trace.quality.samples_total = samples_.size();
    trace.samples.assign(samples_.begin(), samples_.end());
    trace.events.assign(events_.begin(), events_.end());
    for (SampleRecord& sample : trace.samples) {
        if (sample.pid == 0 || sample.pid == UINT32_MAX) {
            const auto found = thread_to_process_.find(sample.tid);
            if (found != thread_to_process_.end()) {
                sample.pid = found->second;
            }
        }
    }
    samples_.clear();
    events_.clear();
    events_total_ = 0;
    ring_dropped_ = 0;
    return trace;
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

    if (IsEqualGUID(class_guid, kImageGuid) ||
        IsEqualGUID(record->EventHeader.ProviderId, kImageGuid)) {
        ParseImage(record);
    } else if (thread_event) {
        ParseThread(record);
        if (event_code == 36 || event_code == 50) {
            ParseEvent(record, event_code == 36 ? TraceEventKind::ContextSwitch
                                                : TraceEventKind::ReadyThread);
        }
    } else if (perf_event && event_code == 46) {
        ParseSample(record);
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
        }
    } else if (IsEqualGUID(class_guid, kDiskIoGuid) ||
               IsEqualGUID(record->EventHeader.ProviderId, kDiskIoGuid)) {
        ParseEvent(record, TraceEventKind::DiskIo);
    } else if (IsEqualGUID(class_guid, kFileIoGuid) ||
               IsEqualGUID(record->EventHeader.ProviderId, kFileIoGuid)) {
        ParseEvent(record, TraceEventKind::FileIo);
    }
}

void LiveTopCapture::ParseThread(PEVENT_RECORD record) {
    if (record->UserDataLength < 8) {
        return;
    }
    const auto* data = static_cast<const BYTE*>(record->UserData);
    const uint32_t pid = ReadU32(data, record->UserDataLength, 0);
    const uint32_t tid = ReadU32(data, record->UserDataLength, 4);
    if (pid == 0 || pid == UINT32_MAX || tid == 0 || tid == UINT32_MAX) {
        return;
    }
    const uint32_t opcode = record->EventHeader.EventDescriptor.Opcode;
    const uint64_t timestamp_ns = TimestampToNs(*record);
    if (opcode == 2 || opcode == 4) {
        for (auto iterator = threads_.rbegin(); iterator != threads_.rend(); ++iterator) {
            if (iterator->pid == pid && iterator->tid == tid &&
                iterator->end_ns == 0) {
                iterator->end_ns = timestamp_ns;
                break;
            }
        }
        thread_to_process_.erase(tid);
        return;
    }
    for (const ThreadRecord& existing : threads_) {
        if (existing.pid == pid && existing.tid == tid && existing.end_ns == 0) {
            thread_to_process_[tid] = pid;
            return;
        }
    }
    threads_.push_back(ThreadRecord{pid, tid, timestamp_ns, 0});
    thread_to_process_[tid] = pid;
}

void LiveTopCapture::ParseEvent(PEVENT_RECORD record, TraceEventKind kind,
                                uint64_t value1, uint64_t value2) {
    if (kind == TraceEventKind::ContextSwitch ||
        kind == TraceEventKind::ReadyThread) {
        const auto* data = static_cast<const BYTE*>(record->UserData);
        value1 = ReadU32(data, record->UserDataLength, 0);
        value2 = ReadU32(data, record->UserDataLength, 4);
    }
    TraceEvent event;
    event.timestamp_ns = TimestampToNs(*record);
    event.cpu = record->BufferContext.ProcessorNumber;
    event.pid = record->EventHeader.ProcessId;
    event.tid = record->EventHeader.ThreadId;
    event.kind = kind;
    event.value1 = value1;
    event.value2 = value2;
    event.event_id = record->EventHeader.EventDescriptor.Opcode;
    events_.push_back(event);
    constexpr size_t kMaximumEvents = 200000;
    if (events_.size() > kMaximumEvents) {
        events_.pop_front();
        ++ring_dropped_;
    }
}

void LiveTopCapture::ParseImage(PEVENT_RECORD record) {
    const auto* data = static_cast<const BYTE*>(record->UserData);
    const size_t size = record->UserDataLength;
    const uint32_t p = pointer_size_;
    const uint16_t version = record->EventHeader.EventDescriptor.Version;
    const uint32_t opcode = record->EventHeader.EventDescriptor.Opcode;

    ImageRecord image;
    image.load_ns = TimestampToNs(*record);
    size_t filename_offset = 0;
    if (version == 0) {
        if (size < p + 4) {
            return;
        }
        image.base = ReadPointer(data, size, 0, p);
        image.size = ReadU32(data, size, p);
        filename_offset = p + 4;
    } else if (version == 1) {
        if (size < 2 * p + 4) {
            return;
        }
        image.base = ReadPointer(data, size, 0, p);
        image.size = ReadPointer(data, size, p, p);
        image.pid = ReadU32(data, size, 2 * p);
        filename_offset = 2 * p + 4;
    } else {
        if (size < 3 * p + 32) {
            return;
        }
        image.base = ReadPointer(data, size, 0, p);
        image.size = ReadPointer(data, size, p, p);
        image.pid = ReadU32(data, size, 2 * p);
        image.checksum = ReadU32(data, size, 2 * p + 4);
        image.timestamp = ReadU32(data, size, 2 * p + 8);
        filename_offset = 3 * p + 32;
    }
    if (image.pid == 0 || image.pid == UINT32_MAX) {
        image.pid = record->EventHeader.ProcessId;
    }
    image.path = ReadWideString(data, size, filename_offset);

    if (opcode == 2 || opcode == 4) {
        for (auto iterator = images_.rbegin(); iterator != images_.rend(); ++iterator) {
            if (iterator->pid == image.pid && iterator->base == image.base &&
                iterator->unload_ns == 0) {
                iterator->unload_ns = image.load_ns;
                break;
            }
        }
        return;
    }

    for (const ImageRecord& existing : images_) {
        if (existing.pid == image.pid && existing.base == image.base &&
            existing.unload_ns == 0 && existing.path == image.path &&
            existing.timestamp == image.timestamp &&
            existing.checksum == image.checksum) {
            return;
        }
    }
    images_.push_back(std::move(image));
}

void LiveTopCapture::ParseSample(PEVENT_RECORD record) {
    const auto* data = static_cast<const BYTE*>(record->UserData);
    const size_t size = record->UserDataLength;
    const uint32_t p = pointer_size_;
    if (size < p + 8) {
        return;
    }
    SampleRecord sample;
    sample.timestamp_ns = TimestampToNs(*record);
    sample.cpu = record->BufferContext.ProcessorNumber;
    sample.pid = record->EventHeader.ProcessId;
    sample.instruction_pointer = ReadPointer(data, size, 0, p);
    sample.tid = ReadU32(data, size, p);
    samples_.push_back(std::move(sample));
    constexpr size_t kMaximumSamples = 200000;
    if (samples_.size() > kMaximumSamples) {
        samples_.pop_front();
        ++ring_dropped_;
    }
}

uint64_t LiveTopCapture::TimestampToNs(const EVENT_RECORD& record) const {
    return QpcToNs(static_cast<uint64_t>(record.EventHeader.TimeStamp.QuadPart),
                   qpc_frequency_);
}

}  // namespace wperf