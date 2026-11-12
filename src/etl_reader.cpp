#include "etl_reader.h"

#include "common.h"

#include <evntcons.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

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
const GUID kStackWalkGuid = {0xdef2fe46, 0x7bd6, 0x4b80,
                             {0xbd, 0x94, 0xf5, 0x7f, 0xe2, 0x0d, 0x0c, 0xe3}};

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

struct StackRecord {
    uint32_t pid = 0;
    uint32_t tid = 0;
    uint32_t cpu = 0;
    uint64_t timestamp_ns = 0;
    uint64_t stack_key = 0;
    bool compressed = false;
    bool kernel = false;
    std::vector<uint64_t> frames;
};

struct StackKey {
    uint64_t tag = 0;
    uint32_t tid = 0;
    uint32_t cpu = 0;

    bool operator==(const StackKey& other) const {
        return tag == other.tag && tid == other.tid && cpu == other.cpu;
    }
};

struct StackKeyHash {
    size_t operator()(const StackKey& key) const noexcept {
        uint64_t hash = key.tag;
        hash ^= (static_cast<uint64_t>(key.tid) << 32) | key.cpu;
        hash ^= hash >> 33;
        hash *= 0xff51afd7ed558ccdULL;
        hash ^= hash >> 33;
        return static_cast<size_t>(hash);
    }
};

struct StackGroup {
    bool has_kernel = false;
    bool has_user = false;
    bool ambiguous = false;
    std::vector<std::vector<uint64_t>> direct_stacks;
    std::vector<uint64_t> kernel_frames;
    std::vector<uint64_t> user_frames;
};

struct ResolvedStack {
    std::vector<std::vector<uint64_t>> candidates;
    bool ambiguous = false;
};

struct StackStore {
    std::vector<StackRecord> records;
    std::unordered_map<uint64_t, std::vector<uint64_t>> definitions;
};
uint32_t ReadU32(const BYTE* data, size_t size, size_t offset) {
    if (offset + sizeof(uint32_t) > size) {
        return 0;
    }
    uint32_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

uint64_t ReadU64(const BYTE* data, size_t size, size_t offset) {
    if (offset + sizeof(uint64_t) > size) {
        return 0;
    }
    uint64_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

uint64_t ReadPointer(const BYTE* data, size_t size, size_t offset,
                     uint32_t pointer_size) {
    if (pointer_size == 8) {
        if (offset + sizeof(uint64_t) > size) {
            return 0;
        }
        uint64_t value = 0;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    }
    return ReadU32(data, size, offset);
}

std::wstring ReadWideString(const BYTE* data, size_t size, size_t offset) {
    if (offset >= size) {
        return {};
    }
    const size_t chars = (size - offset) / sizeof(wchar_t);
    const auto* text = reinterpret_cast<const wchar_t*>(data + offset);
    size_t length = 0;
    while (length < chars && text[length] != L'\0') {
        ++length;
    }
    if (length == 0) {
        return {};
    }
    return std::wstring(text, length);
}

size_t SidLength(const BYTE* data, size_t size, size_t offset) {
    if (offset + 8 > size) {
        return 0;
    }
    const uint8_t subauthority_count = data[offset + 1];
    const size_t length = 8 + static_cast<size_t>(subauthority_count) * 4;
    return offset + length <= size ? length : 0;
}

uint64_t QpcToNs(uint64_t qpc, uint64_t frequency) {
    if (frequency == 0) {
        frequency = 10000000;
    }
    return (qpc / frequency) * 1000000000ull +
           ((qpc % frequency) * 1000000000ull) / frequency;
}

uint64_t TimestampToNs(const EVENT_RECORD& record, const TraceData& trace) {
    return QpcToNs(static_cast<uint64_t>(record.EventHeader.TimeStamp.QuadPart),
                   trace.qpc_frequency);
}

void ParseProcess(const EVENT_RECORD& record, TraceData& trace) {
    const BYTE* data = static_cast<const BYTE*>(record.UserData);
    const size_t size = record.UserDataLength;
    const uint32_t p = trace.pointer_size;
    if (size < p + 24) {
        return;
    }

    ProcessRecord process;
    process.pid = ReadU32(data, size, p);
    process.parent_pid = ReadU32(data, size, p + 4);
    process.start_ns = TimestampToNs(record, trace);

    const size_t fixed_before_dtb = p + 16;
    const size_t dtb_offset = static_cast<size_t>((fixed_before_dtb + p - 1) / p) * p;
    const size_t sid_offset = dtb_offset + p;
    const size_t sid_length = SidLength(data, size, sid_offset);
    if (sid_length == 0) {
        trace.processes.push_back(process);
        return;
    }

    const size_t image_offset = sid_offset + sid_length;
    process.image_name = ReadWideString(data, size, image_offset);
    trace.processes.push_back(process);
}

void ParseThread(const EVENT_RECORD& record, TraceData& trace) {
    const BYTE* data = static_cast<const BYTE*>(record.UserData);
    const size_t size = record.UserDataLength;
    if (size < 8) {
        return;
    }
    ThreadRecord thread;
    thread.pid = ReadU32(data, size, 0);
    thread.tid = ReadU32(data, size, 4);
    thread.start_ns = TimestampToNs(record, trace);
    trace.threads.push_back(thread);
}

void ParseImage(const EVENT_RECORD& record, TraceData& trace) {
    const BYTE* data = static_cast<const BYTE*>(record.UserData);
    const size_t size = record.UserDataLength;
    const uint32_t p = trace.pointer_size;
    const uint16_t version = record.EventHeader.EventDescriptor.Version;
    const uint8_t opcode = record.EventHeader.EventDescriptor.Opcode;

    ImageRecord image;
    image.load_ns = TimestampToNs(record, trace);
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
        image.pid = record.EventHeader.ProcessId;
    }
    image.path = ReadWideString(data, size, filename_offset);

    if (opcode == 2 || opcode == 4) {
        for (auto iterator = trace.images.rbegin(); iterator != trace.images.rend();
             ++iterator) {
            if (iterator->pid == image.pid && iterator->base == image.base &&
                iterator->unload_ns == 0) {
                iterator->unload_ns = image.load_ns;
                break;
            }
        }
        return;
    }

    for (const ImageRecord& existing : trace.images) {
        if (existing.pid == image.pid && existing.base == image.base &&
            existing.unload_ns == 0 && existing.path == image.path &&
            existing.timestamp == image.timestamp &&
            existing.checksum == image.checksum) {
            return;
        }
    }
    trace.images.push_back(std::move(image));
}

void AddEvent(const EVENT_RECORD& record, TraceData& trace, TraceEventKind kind,
              uint64_t value1 = 0, uint64_t value2 = 0) {
    TraceEvent event;
    event.timestamp_ns = TimestampToNs(record, trace);
    event.cpu = record.BufferContext.ProcessorNumber;
    event.pid = record.EventHeader.ProcessId;
    event.tid = record.EventHeader.ThreadId;
    event.kind = kind;
    event.value1 = value1;
    event.value2 = value2;
    event.event_id = record.EventHeader.EventDescriptor.Opcode;
    trace.events.push_back(event);
}

void ParseThreadEvent(const EVENT_RECORD& record, TraceData& trace,
                      TraceEventKind kind) {
    const BYTE* data = static_cast<const BYTE*>(record.UserData);
    const size_t size = record.UserDataLength;
    uint64_t value1 = 0;
    uint64_t value2 = 0;
    if (size >= 8) {
        value1 = ReadU32(data, size, 0);
        value2 = ReadU32(data, size, 4);
    }
    AddEvent(record, trace, kind, value1, value2);
}

void ParsePageFault(const EVENT_RECORD& record, TraceData& trace) {
    const BYTE* data = static_cast<const BYTE*>(record.UserData);
    const size_t size = record.UserDataLength;
    const uint64_t virtual_address = ReadPointer(data, size, 0, trace.pointer_size);
    const uint64_t program_counter = ReadPointer(data, size, trace.pointer_size,
                                                 trace.pointer_size);
    AddEvent(record, trace, TraceEventKind::PageFault, virtual_address,
             program_counter);
}
void ParseSample(const EVENT_RECORD& record, TraceData& trace) {
    const BYTE* data = static_cast<const BYTE*>(record.UserData);
    const size_t size = record.UserDataLength;
    const uint32_t p = trace.pointer_size;
    if (size < p + 8) {
        return;
    }

    SampleRecord sample;
    sample.timestamp_ns = TimestampToNs(record, trace);
    sample.cpu = record.BufferContext.ProcessorNumber;
    sample.pid = record.EventHeader.ProcessId;
    sample.instruction_pointer = ReadPointer(data, size, 0, p);
    sample.tid = ReadU32(data, size, p);

    trace.samples.push_back(std::move(sample));
}

void AddStackFrames(std::vector<uint64_t>& target, bool& present,
                    bool& ambiguous, const std::vector<uint64_t>& frames) {
    if (frames.empty()) {
        return;
    }
    if (!present) {
        target = frames;
        present = true;
    } else if (target != frames) {
        ambiguous = true;
    }
}

void AddDistinctStack(std::vector<std::vector<uint64_t>>& target,
                      const std::vector<uint64_t>& frames) {
    if (frames.empty()) {
        return;
    }
    if (std::find(target.begin(), target.end(), frames) == target.end()) {
        target.push_back(frames);
    }
}

void ParseStack(const EVENT_RECORD& record, TraceData& trace, StackStore& stacks) {
    const BYTE* data = static_cast<const BYTE*>(record.UserData);
    const size_t size = record.UserDataLength;
    if (size < 16) {
        return;
    }

    StackRecord stack;
    stack.timestamp_ns =
        QpcToNs(ReadU64(data, size, 0), trace.qpc_frequency);
    stack.pid = ReadU32(data, size, 8);
    stack.tid = ReadU32(data, size, 12);
    stack.cpu = record.BufferContext.ProcessorNumber;

    const uint32_t p = trace.pointer_size;
    for (size_t offset = 16; offset + p <= size; offset += p) {
        const uint64_t frame = ReadPointer(data, size, offset, p);
        if (frame != 0) {
            stack.frames.push_back(frame);
        }
    }
    stacks.records.push_back(std::move(stack));
}

void ParseStackDefinition(const EVENT_RECORD& record, TraceData& trace,
                          StackStore& stacks) {
    const BYTE* data = static_cast<const BYTE*>(record.UserData);
    const size_t size = record.UserDataLength;
    const uint32_t p = trace.pointer_size;
    if (size < p || p == 0) {
        return;
    }
    const uint64_t key = ReadPointer(data, size, 0, p);
    std::vector<uint64_t> frames;
    for (size_t offset = p; offset + p <= size; offset += p) {
        const uint64_t frame = ReadPointer(data, size, offset, p);
        if (frame != 0) {
            frames.push_back(frame);
        }
    }
    if (key != 0 && !frames.empty()) {
        stacks.definitions[key] = std::move(frames);
    }
}

void ParseStackReference(const EVENT_RECORD& record, TraceData& trace,
                         StackStore& stacks, bool kernel) {
    const BYTE* data = static_cast<const BYTE*>(record.UserData);
    const size_t size = record.UserDataLength;
    const uint32_t p = trace.pointer_size;
    if (size < 16 + p || p == 0) {
        return;
    }

    StackRecord stack;
    stack.timestamp_ns =
        QpcToNs(ReadU64(data, size, 0), trace.qpc_frequency);
    stack.pid = ReadU32(data, size, 8);
    stack.tid = ReadU32(data, size, 12);
    stack.cpu = record.BufferContext.ProcessorNumber;
    stack.stack_key = ReadPointer(data, size, 16, p);
    stack.compressed = true;
    stack.kernel = kernel;
    stacks.records.push_back(std::move(stack));
}

std::unordered_map<StackKey, ResolvedStack, StackKeyHash> ResolveStacks(
    const StackStore& stacks, TraceQuality& quality) {
    std::unordered_map<StackKey, StackGroup, StackKeyHash> groups;
    groups.reserve(stacks.records.size());

    for (const StackRecord& stack : stacks.records) {
        std::vector<uint64_t> frames = stack.frames;
        if (stack.compressed) {
            auto definition = stacks.definitions.find(stack.stack_key);
            if (definition == stacks.definitions.end()) {
                ++quality.stack_key_mismatch;
                continue;
            }
            frames = definition->second;
        }
        if (frames.empty()) {
            continue;
        }

        const StackKey key{stack.timestamp_ns, stack.tid, stack.cpu};
        StackGroup& group = groups[key];
        if (stack.compressed) {
            if (stack.kernel) {
                AddStackFrames(group.kernel_frames, group.has_kernel,
                               group.ambiguous, frames);
            } else {
                AddStackFrames(group.user_frames, group.has_user,
                               group.ambiguous, frames);
            }
        } else {
            AddDistinctStack(group.direct_stacks, frames);
        }
    }

    std::unordered_map<StackKey, ResolvedStack, StackKeyHash> resolved;
    resolved.reserve(groups.size());
    for (auto& [key, group] : groups) {
        ResolvedStack stack;
        stack.ambiguous = group.ambiguous;
        if (group.has_kernel || group.has_user) {
            std::vector<uint64_t> combined = group.kernel_frames;
            combined.insert(combined.end(), group.user_frames.begin(),
                            group.user_frames.end());
            AddDistinctStack(stack.candidates, combined);
        }
        for (const auto& candidate : group.direct_stacks) {
            AddDistinctStack(stack.candidates, candidate);
        }
        if (!stack.candidates.empty()) {
            resolved.emplace(key, std::move(stack));
        }
    }
    return resolved;
}

struct ReplayContext {
    TraceData* trace = nullptr;
    StackStore stacks;
};

void EventRecordCallback(PEVENT_RECORD record) {
    if (record == nullptr || record->UserData == nullptr) {
        return;
    }
    auto* context = static_cast<ReplayContext*>(record->UserContext);
    if (context == nullptr || context->trace == nullptr) {
        return;
    }
    auto* trace = context->trace;

    ++trace->quality.events_total;
    if ((record->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) != 0) {
        trace->pointer_size = 4;
    } else if ((record->EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER) != 0) {
        trace->pointer_size = 8;
    }

    GUID class_guid = GetClassGuid(*record);
    if (IsZeroGuid(class_guid)) {
        class_guid = record->EventHeader.ProviderId;
    }
    const uint8_t opcode = record->EventHeader.EventDescriptor.Opcode;
    const uint16_t id = record->EventHeader.EventDescriptor.Id;
    const uint32_t event_code = opcode != 0 ? opcode : id;
    const bool is_stack_event =
        IsEqualGUID(class_guid, kStackWalkGuid) ||
        IsEqualGUID(record->EventHeader.ProviderId, kStackWalkGuid);

    if (IsEqualGUID(class_guid, kProcessGuid) &&
        (opcode == 1 || opcode == 2 || opcode == 3 || opcode == 4 || opcode == 39)) {
        ParseProcess(*record, *trace);
    } else if (IsEqualGUID(class_guid, kThreadGuid) &&
               (opcode == 1 || opcode == 2 || opcode == 3 || opcode == 4)) {
        ParseThread(*record, *trace);
    } else if (IsEqualGUID(class_guid, kImageGuid) &&
               (opcode == 2 || opcode == 3 || opcode == 4 || opcode == 10)) {
        ParseImage(*record, *trace);
    } else if (IsEqualGUID(class_guid, kPerfInfoGuid) &&
               (opcode == 46 || id == 46)) {
        ++trace->quality.samples_total;
        ParseSample(*record, *trace);
    } else if (is_stack_event && event_code == 32) {
        ++trace->quality.stack_events_total;
        ParseStack(*record, *trace, context->stacks);
    } else if (is_stack_event && event_code >= 34 && event_code <= 36) {
        ++trace->quality.stack_def_events_total;
        ParseStackDefinition(*record, *trace, context->stacks);
    } else if (is_stack_event && (event_code == 37 || event_code == 38)) {
        ++trace->quality.stack_ref_events_total;
        ParseStackReference(*record, *trace, context->stacks,
                            event_code == 37);
    } else if ((IsEqualGUID(class_guid, kThreadGuid) ||
                IsEqualGUID(record->EventHeader.ProviderId, kThreadGuid)) &&
               opcode == 36) {
        ParseThreadEvent(*record, *trace, TraceEventKind::ContextSwitch);
    } else if ((IsEqualGUID(class_guid, kThreadGuid) ||
                IsEqualGUID(record->EventHeader.ProviderId, kThreadGuid)) &&
               opcode == 50) {
        ParseThreadEvent(*record, *trace, TraceEventKind::ReadyThread);
    } else if ((IsEqualGUID(class_guid, kPageFaultGuid) ||
                IsEqualGUID(record->EventHeader.ProviderId, kPageFaultGuid)) &&
               opcode >= 10 && opcode <= 15) {
        ParsePageFault(*record, *trace);
    } else if ((IsEqualGUID(class_guid, kPerfInfoGuid) ||
                IsEqualGUID(record->EventHeader.ProviderId, kPerfInfoGuid)) &&
               opcode == 51) {
        const uint64_t address = ReadPointer(
            static_cast<const BYTE*>(record->UserData), record->UserDataLength, 0,
            trace->pointer_size);
        AddEvent(*record, *trace, TraceEventKind::SyscallEnter, address);
    } else if ((IsEqualGUID(class_guid, kPerfInfoGuid) ||
                IsEqualGUID(record->EventHeader.ProviderId, kPerfInfoGuid)) &&
               opcode == 52) {
        AddEvent(*record, *trace, TraceEventKind::SyscallExit);
    } else if (IsEqualGUID(class_guid, kDiskIoGuid) ||
               IsEqualGUID(record->EventHeader.ProviderId, kDiskIoGuid)) {
        AddEvent(*record, *trace, TraceEventKind::DiskIo);
    } else if (IsEqualGUID(class_guid, kFileIoGuid) ||
               IsEqualGUID(record->EventHeader.ProviderId, kFileIoGuid)) {
        AddEvent(*record, *trace, TraceEventKind::FileIo);
    } else if (IsEqualGUID(record->EventHeader.ProviderId, kSystemTraceGuid) &&
               opcode == 46) {
        ++trace->quality.samples_total;
        ParseSample(*record, *trace);
    } else {
        ++trace->quality.unknown_events;
    }
}
}  // namespace

bool ReadEtwFile(const std::filesystem::path& path, TraceData& trace,
                 std::wstring* error) {
    trace = TraceData{};
    trace.source_path = path.wstring();

    ReplayContext context;
    context.trace = &trace;

    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LogFileName = const_cast<wchar_t*>(path.c_str());
    logfile.ProcessTraceMode =
        PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    logfile.EventRecordCallback = EventRecordCallback;
    logfile.Context = &context;

    TRACEHANDLE handle = OpenTraceW(&logfile);
    if (handle == INVALID_PROCESSTRACE_HANDLE) {
        if (error != nullptr) {
            *error = L"OpenTraceW failed: " + FormatWindowsError(GetLastError());
        }
        return false;
    }

    trace.quality.lost_events = logfile.LogfileHeader.EventsLost;
    if (logfile.LogfileHeader.PerfFreq.QuadPart > 0) {
        trace.qpc_frequency =
            static_cast<uint64_t>(logfile.LogfileHeader.PerfFreq.QuadPart);
    }
    if (logfile.LogfileHeader.PointerSize == 32 ||
        logfile.LogfileHeader.PointerSize == 64) {
        trace.pointer_size = logfile.LogfileHeader.PointerSize / 8;
    }

    const ULONG result = ProcessTrace(&handle, 1, nullptr, nullptr);
    CloseTrace(handle);
    if (result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = L"ProcessTrace failed: " + FormatWindowsError(result);
        }
        return false;
    }

    std::unordered_map<uint32_t, uint32_t> thread_to_process;
    for (const auto& thread : trace.threads) {
        thread_to_process[thread.tid] = thread.pid;
    }

    struct StackCandidateRef {
        StackKey key;
        size_t index = 0;
    };

    const auto resolved_stacks = ResolveStacks(context.stacks, trace.quality);
    std::unordered_map<StackKey, std::vector<StackCandidateRef>, StackKeyHash>
        fallback_by_leaf;
    fallback_by_leaf.reserve(resolved_stacks.size());
    for (const auto& [key, stack] : resolved_stacks) {
        if (stack.ambiguous) {
            continue;
        }
        for (size_t index = 0; index < stack.candidates.size(); ++index) {
            const auto& candidate = stack.candidates[index];
            if (!candidate.empty()) {
                fallback_by_leaf[StackKey{candidate.front(), key.tid, key.cpu}]
                    .push_back(StackCandidateRef{key, index});
            }
        }
    }

    for (auto& sample : trace.samples) {
        if (sample.pid == 0 || sample.pid == UINT32_MAX) {
            auto found = thread_to_process.find(sample.tid);
            if (found != thread_to_process.end()) {
                sample.pid = found->second;
            }
        }

        bool attached = false;
        const StackKey exact_key{sample.timestamp_ns, sample.tid, sample.cpu};
        const auto exact = resolved_stacks.find(exact_key);
        if (exact != resolved_stacks.end()) {
            if (exact->second.ambiguous) {
                ++trace.quality.stack_ambiguous;
            } else {
                const auto& candidates = exact->second.candidates;
                size_t matched = SIZE_MAX;
                size_t match_count = 0;
                for (size_t index = 0; index < candidates.size(); ++index) {
                    if (!candidates[index].empty() &&
                        candidates[index].front() == sample.instruction_pointer) {
                        matched = index;
                        ++match_count;
                    }
                }
                if (candidates.size() == 1 || match_count == 1) {
                    const size_t index =
                        candidates.size() == 1 ? 0 : matched;
                    sample.stack = candidates[index];
                    ++trace.quality.stack_exact_total;
                    attached = true;
                } else {
                    ++trace.quality.stack_ambiguous;
                }
            }
        } else {
            const StackKey fallback_key{
                sample.instruction_pointer, sample.tid, sample.cpu};
            const auto fallback = fallback_by_leaf.find(fallback_key);
            if (fallback != fallback_by_leaf.end() &&
                fallback->second.size() == 1) {
                const StackCandidateRef& reference = fallback->second.front();
                const auto candidate = resolved_stacks.find(reference.key);
                if (candidate != resolved_stacks.end() &&
                    reference.index < candidate->second.candidates.size()) {
                    sample.stack = candidate->second.candidates[reference.index];
                    ++trace.quality.stack_approximate_total;
                    attached = true;
                }
            } else if (fallback != fallback_by_leaf.end()) {
                ++trace.quality.stack_ambiguous;
            }
        }

        if (!attached) {
            sample.stack_missing = true;
            ++trace.quality.samples_without_stack;
        }
    }

    return true;
}

}  // namespace wperf
