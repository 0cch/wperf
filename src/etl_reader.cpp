#include "etl_reader.h"

#include "common.h"

#include <evntcons.h>

#include <algorithm>
#include <cstring>
#include <deque>
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
    std::vector<uint64_t> frames;
};

struct StackStore {
    std::vector<StackRecord> records;
    std::unordered_map<uint64_t, std::deque<size_t>> by_instruction_pointer;
    std::unordered_map<uint64_t, std::deque<size_t>> by_thread_cpu;
    std::vector<bool> used;
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

uint64_t TimestampToNs(const EVENT_RECORD& record) {
    return static_cast<uint64_t>(record.EventHeader.TimeStamp.QuadPart) * 100ull;
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
    process.start_ns = TimestampToNs(record);

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
    thread.start_ns = TimestampToNs(record);
    trace.threads.push_back(thread);
}

void ParseImage(const EVENT_RECORD& record, TraceData& trace) {
    const BYTE* data = static_cast<const BYTE*>(record.UserData);
    const size_t size = record.UserDataLength;
    const uint32_t p = trace.pointer_size;
    const uint16_t version = record.EventHeader.EventDescriptor.Version;

    ImageRecord image;
    image.load_ns = TimestampToNs(record);
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
    image.path = ReadWideString(data, size, filename_offset);
    trace.images.push_back(std::move(image));
}

void ParseSample(const EVENT_RECORD& record, TraceData& trace) {
    const BYTE* data = static_cast<const BYTE*>(record.UserData);
    const size_t size = record.UserDataLength;
    const uint32_t p = trace.pointer_size;
    if (size < p + 8) {
        return;
    }

    SampleRecord sample;
    sample.timestamp_ns = TimestampToNs(record);
    sample.cpu = record.BufferContext.ProcessorNumber;
    sample.pid = record.EventHeader.ProcessId;
    sample.instruction_pointer = ReadPointer(data, size, 0, p);
    sample.tid = ReadU32(data, size, p);

    trace.samples.push_back(std::move(sample));
}

void ParseStack(const EVENT_RECORD& record, TraceData& trace, StackStore& stacks) {
    const BYTE* data = static_cast<const BYTE*>(record.UserData);
    const size_t size = record.UserDataLength;
    if (size < 16) {
        return;
    }

    StackRecord stack;
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
    const size_t index = stacks.records.size();
    if (!stack.frames.empty()) {
        stacks.by_instruction_pointer[stack.frames.front()].push_back(index);
    }
    const uint64_t thread_key =
        (static_cast<uint64_t>(stack.tid) << 32) | stack.cpu;
    stacks.by_thread_cpu[thread_key].push_back(index);
    stacks.records.push_back(std::move(stack));
    stacks.used.push_back(false);
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


    if (IsEqualGUID(class_guid, kProcessGuid)) {
        ParseProcess(*record, *trace);
    } else if (IsEqualGUID(class_guid, kThreadGuid)) {
        ParseThread(*record, *trace);
    } else if (IsEqualGUID(class_guid, kImageGuid)) {
        ParseImage(*record, *trace);
    } else if (IsEqualGUID(class_guid, kPerfInfoGuid) &&
               (opcode == 46 || id == 46)) {
        ++trace->quality.samples_total;
        ParseSample(*record, *trace);
    } else if (IsEqualGUID(class_guid, kStackWalkGuid) &&
               (opcode == 32 || id == 32)) {
        ++trace->quality.stack_events_total;
        ParseStack(*record, *trace, context->stacks);
    } else if (IsEqualGUID(record->EventHeader.ProviderId, kSystemTraceGuid) &&
               opcode == 46) {
        ++trace->quality.samples_total;
        ParseSample(*record, *trace);
    } else if (IsEqualGUID(record->EventHeader.ProviderId, kSystemTraceGuid) &&
               opcode == 32) {
        ++trace->quality.stack_events_total;
        ParseStack(*record, *trace, context->stacks);
    } else {
        ++trace->quality.unknown_events;
    }
}
void StackAwareEventRecordCallback(PEVENT_RECORD record) {
    if (record == nullptr || record->UserData == nullptr) {
        return;
    }
    auto* context = static_cast<ReplayContext*>(record->UserContext);
    if (context == nullptr || context->trace == nullptr) {
        return;
    }

    const GUID& provider = record->EventHeader.ProviderId;
    if (IsEqualGUID(provider, kStackWalkGuid) &&
        record->EventHeader.EventDescriptor.Id == 32) {
        ++context->trace->quality.stack_events_total;
        ParseStack(*record, *context->trace, context->stacks);
        return;
    }
    EventRecordCallback(record);
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
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = StackAwareEventRecordCallback;
    logfile.Context = &context;

    TRACEHANDLE handle = OpenTraceW(&logfile);
    if (handle == INVALID_PROCESSTRACE_HANDLE) {
        if (error != nullptr) {
            *error = L"OpenTraceW failed: " + FormatWindowsError(GetLastError());
        }
        return false;
    }

    trace.quality.lost_events = logfile.LogfileHeader.EventsLost;
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
    for (auto& sample : trace.samples) {
        if (sample.pid == 0 || sample.pid == UINT32_MAX) {
            auto found = thread_to_process.find(sample.tid);
            if (found != thread_to_process.end()) {
                sample.pid = found->second;
            }
        }

        size_t stack_index = SIZE_MAX;
        auto by_ip = context.stacks.by_instruction_pointer.find(
            sample.instruction_pointer);
        while (by_ip != context.stacks.by_instruction_pointer.end() &&
               !by_ip->second.empty()) {
            const size_t candidate = by_ip->second.front();
            by_ip->second.pop_front();
            if (!context.stacks.used[candidate]) {
                stack_index = candidate;
                break;
            }
        }
        if (stack_index == SIZE_MAX) {
            const uint64_t key =
                (static_cast<uint64_t>(sample.tid) << 32) | sample.cpu;
            auto by_thread = context.stacks.by_thread_cpu.find(key);
            while (by_thread != context.stacks.by_thread_cpu.end() &&
                   !by_thread->second.empty()) {
                const size_t candidate = by_thread->second.front();
                by_thread->second.pop_front();
                if (!context.stacks.used[candidate]) {
                    stack_index = candidate;
                    break;
                }
            }
        }
        if (stack_index != SIZE_MAX) {
            context.stacks.used[stack_index] = true;
            sample.stack = context.stacks.records[stack_index].frames;
        } else {
            sample.stack_missing = true;
            ++trace.quality.samples_without_stack;
        }
    }

    return true;
}

}  // namespace wperf
