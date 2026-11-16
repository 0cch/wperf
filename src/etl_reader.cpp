#include "etl_reader.h"

#include "analysis.h"
#include "common.h"
#include "etw_decode.h"
#include "lbr.h"
#include "trace_events.h"

#include <evntcons.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <map>
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
const GUID kLbrGuid = {0x99134383, 0x5248, 0x43fc,
                       {0x83, 0x4b, 0x52, 0x94, 0x54, 0xe7, 0x5d, 0xf3}};

std::wstring UnknownEventKey(const GUID& provider, uint32_t event_code) {
    wchar_t guid[64]{};
    swprintf_s(guid, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               provider.Data1, provider.Data2, provider.Data3, provider.Data4[0],
               provider.Data4[1], provider.Data4[2], provider.Data4[3],
               provider.Data4[4], provider.Data4[5], provider.Data4[6], provider.Data4[7]);
    return std::wstring(guid) + L":" + std::to_wstring(event_code);
}

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
    uint64_t order = 0;
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
    struct Definition { uint64_t order, end_ns; std::vector<uint64_t> frames; };
    std::unordered_map<uint64_t, std::vector<Definition>> definitions;
};

struct IndexKey {
    uint32_t pid = 0;
    uint64_t value = 0;

    bool operator==(const IndexKey& other) const {
        return pid == other.pid && value == other.value;
    }
};

struct IndexKeyHash {
    size_t operator()(const IndexKey& key) const noexcept {
        uint64_t hash = key.value;
        hash ^= static_cast<uint64_t>(key.pid) << 32;
        hash ^= hash >> 33;
        hash *= 0xff51afd7ed558ccdULL;
        hash ^= hash >> 33;
        return static_cast<size_t>(hash);
    }
};

struct ReplayIndexes {
    std::unordered_map<uint32_t, std::vector<size_t>> processes_by_pid;
    std::unordered_map<IndexKey, std::vector<size_t>, IndexKeyHash>
        threads_by_pid_tid;
    std::unordered_map<IndexKey, std::vector<size_t>, IndexKeyHash>
        images_by_pid_base;
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

void ParseProcess(const EVENT_RECORD& record, TraceData& trace,
                  ReplayIndexes& indexes) {
    ProcessRecord process;
    if(!DecodeProcessPayload(record,trace.pointer_size,trace.qpc_frequency,process)) { ++trace.quality.decode_errors; return; }
    const uint8_t opcode=record.EventHeader.EventDescriptor.Opcode;
    const uint32_t pid=process.pid;
    const uint64_t timestamp_ns=process.start_ns;
    auto& active = indexes.processes_by_pid[pid];
    if (opcode == 2 || opcode == 4) {
        for (auto iterator = active.rbegin(); iterator != active.rend();
             ++iterator) {
            if (*iterator < trace.processes.size() &&
                trace.processes[*iterator].end_ns == 0) {
                trace.processes[*iterator].end_ns = timestamp_ns;
                active.erase(std::next(iterator).base());
                break;
            }
        }
        return;
    }

    for (const size_t index : active) {
        if (index < trace.processes.size() &&
            trace.processes[index].end_ns == 0 &&
            trace.processes[index].image_name == process.image_name) {
            return;
        }
    }
    const size_t index = trace.processes.size();
    trace.processes.push_back(std::move(process));
    active.push_back(index);
}

void ParseThread(const EVENT_RECORD& record, TraceData& trace,
                 ReplayIndexes& indexes) {
    ThreadRecord thread;
    if(!DecodeThreadPayload(record,trace.qpc_frequency,thread)) { ++trace.quality.decode_errors; return; }
    const uint8_t opcode=record.EventHeader.EventDescriptor.Opcode;
    const uint32_t pid=thread.pid,tid=thread.tid;
    const uint64_t timestamp_ns=thread.start_ns;
    auto& active = indexes.threads_by_pid_tid[IndexKey{pid, tid}];
    if (opcode == 2 || opcode == 4) {
        for (auto iterator = active.rbegin(); iterator != active.rend();
             ++iterator) {
            if (*iterator < trace.threads.size() &&
                trace.threads[*iterator].end_ns == 0) {
                trace.threads[*iterator].end_ns = timestamp_ns;
                active.erase(std::next(iterator).base());
                break;
            }
        }
        return;
    }

    for (const size_t index : active) {
        if (index < trace.threads.size() &&
            trace.threads[index].end_ns == 0) {
            return;
        }
    }

    const size_t index = trace.threads.size();
    trace.threads.push_back(std::move(thread));
    active.push_back(index);
}

void ParseImage(const EVENT_RECORD& record, TraceData& trace,
                ReplayIndexes& indexes) {
    ImageRecord image;
    if(!DecodeImagePayload(record,trace.pointer_size,trace.qpc_frequency,image)) { ++trace.quality.decode_errors; return; }
    const uint8_t opcode=record.EventHeader.EventDescriptor.Opcode;
    auto& active =
        indexes.images_by_pid_base[IndexKey{image.pid, image.base}];
    if (opcode == 2 || opcode == 4) {
        for (auto iterator = active.rbegin(); iterator != active.rend();
             ++iterator) {
            if (*iterator < trace.images.size() &&
                trace.images[*iterator].unload_ns == 0) {
                trace.images[*iterator].unload_ns = image.load_ns;
                active.erase(std::next(iterator).base());
                break;
            }
        }
        return;
    }

    for (const size_t index : active) {
        if (index >= trace.images.size()) {
            continue;
        }
        const ImageRecord& existing = trace.images[index];
        if (existing.unload_ns == 0 && existing.path == image.path &&
            existing.timestamp == image.timestamp &&
            existing.checksum == image.checksum) {
            return;
        }
    }
    const size_t index = trace.images.size();
    trace.images.push_back(std::move(image));
    active.push_back(index);
}



void ParseKernelEvent(const EVENT_RECORD& record, TraceData& trace, TraceEventKind kind) {
    TraceEvent event;
    if(!DecodeKernelEvent(record,trace.pointer_size,trace.qpc_frequency,kind,event)) {
        ++trace.quality.decode_errors; return;
    }
    CopyEtwPmcCounters(record,event.pmc_counters,trace.quality);
    trace.events.push_back(std::move(event));
}

void ParseLbr(const EVENT_RECORD& record, TraceData& trace) {
    LbrSample decoded;
    if (!DecodeLbrPayload(static_cast<const BYTE*>(record.UserData),
                          record.UserDataLength, trace.pointer_size, decoded)) {
        ++trace.quality.decode_errors;
        return;
    }
    LbrSampleRecord sample;
    sample.timestamp_ns = decoded.timestamp_qpc != 0
                              ? QpcToNs(decoded.timestamp_qpc, trace.qpc_frequency)
                              : EtwTimestampNs(record, trace.qpc_frequency);
    sample.cpu = EtwProcessorIndex(record);
    sample.pid = decoded.pid;
    sample.tid = decoded.tid;
    sample.filters = decoded.filters;
    sample.event_id = record.EventHeader.EventDescriptor.Opcode;
    sample.group_id = static_cast<uint64_t>(trace.lbr_samples.size());
    sample.branches.reserve(decoded.branches.size());
    for (const LbrBranch& branch : decoded.branches) {
        sample.branches.push_back({branch.from_address, branch.to_address});
    }
    ++trace.quality.lbr_events;
    trace.quality.lbr_branches += sample.branches.size();
    trace.lbr_samples.push_back(std::move(sample));
}

void ParseContextRegisters(const EVENT_RECORD& record, TraceData& trace) {
    ContextRegisterRecord registers;
    if (!DecodeContextRegisters(record, trace.pointer_size, registers)) {
        ++trace.quality.decode_errors;
        return;
    }
    const uint32_t index = static_cast<uint32_t>(trace.context_registers.size());
    trace.context_registers.push_back(registers);
    TraceEvent event;
    event.timestamp_ns = registers.timestamp_qpc != 0
                             ? QpcToNs(registers.timestamp_qpc, trace.qpc_frequency)
                             : EtwTimestampNs(record, trace.qpc_frequency);
    event.cpu = EtwProcessorIndex(record);
    event.pid = record.EventHeader.ProcessId;
    event.tid = record.EventHeader.ThreadId;
    event.kind = TraceEventKind::ContextRegister;
    event.context_register_index = index;
    event.value1 = registers.rip;
    event.value2 = registers.rsp;
    trace.events.push_back(std::move(event));
    ++trace.quality.context_register_events;
}

void ParseSample(const EVENT_RECORD& record, TraceData& trace) {
    SampleRecord sample;
    if (!DecodeProfileSample(record, trace, sample)) {
        ++trace.quality.decode_errors;
        return;
    }
    CopyEtwPmcCounters(record, sample.pmc_counters, trace.quality);
    if (sample.kind == SampleKind::Pmc) ++trace.quality.pmc_samples;
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
    if(size<16 || trace.pointer_size==0 || (size-16)%trace.pointer_size!=0) { ++trace.quality.decode_errors; return; }

    StackRecord stack;
    stack.timestamp_ns =
        QpcToNs(ReadU64(data, size, 0), trace.qpc_frequency);
    stack.pid = ReadU32(data, size, 8);
    stack.tid = ReadU32(data, size, 12);
    stack.cpu = EtwProcessorIndex(record);

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
    if(size<p || (p!=4&&p!=8) || (size-p)%p!=0) { ++trace.quality.decode_errors; return; }
    const uint64_t key = ReadPointer(data, size, 0, p);
    std::vector<uint64_t> frames;
    for (size_t offset = p; offset + p <= size; offset += p) {
        const uint64_t frame = ReadPointer(data, size, offset, p);
        if (frame != 0) {
            frames.push_back(frame);
        }
    }
    if (key != 0 && !frames.empty()) {
        stacks.definitions[key].push_back({trace.quality.events_total,TimestampToNs(record,trace),std::move(frames)});
    }
}

void ParseStackReference(const EVENT_RECORD& record, TraceData& trace,
                         StackStore& stacks, bool kernel) {
    const BYTE* data = static_cast<const BYTE*>(record.UserData);
    const size_t size = record.UserDataLength;
    const uint32_t p = trace.pointer_size;
    if(size<16+p || (p!=4&&p!=8)) { ++trace.quality.decode_errors; return; }

    StackRecord stack;
    stack.timestamp_ns =
        QpcToNs(ReadU64(data, size, 0), trace.qpc_frequency);
    stack.pid = ReadU32(data, size, 8);
    stack.tid = ReadU32(data, size, 12);
    stack.cpu = EtwProcessorIndex(record);
    stack.stack_key = ReadPointer(data, size, 16, p);
    stack.order = trace.quality.events_total;
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
            const auto& definitions=definition->second;
            const auto next=std::lower_bound(definitions.begin(),definitions.end(),stack.timestamp_ns,
                [](const auto& value,uint64_t time){return value.end_ns<time;});
            if(next==definitions.end()) { ++quality.stack_key_mismatch; continue; }
            frames=next->frames;
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
    ReplayIndexes indexes;
    StackStore stacks;
    bool metadata_only=false;
    const std::function<void(const SampleRecord&)>* sample_consumer=nullptr;
    TraceData sampling_metadata;
    ThreadIndex thread_index;
    uint64_t charged_bytes=0;
    const wchar_t* failure=nullptr;
};

void ConsumeEventRecord(PEVENT_RECORD record) {
    if (record == nullptr || record->UserData == nullptr) {
        return;
    }
    auto* context = static_cast<ReplayContext*>(record->UserContext);
    if (context == nullptr || context->trace == nullptr) {
        return;
    }
    auto* trace = context->trace;

    if(context->failure)return;
    if(context->sample_consumer) {
        const auto code=record->EventHeader.EventDescriptor.Opcode;
        GUID provider=GetClassGuid(*record);
        if(IsZeroGuid(provider))provider=record->EventHeader.ProviderId;
        if(IsEqualGUID(record->EventHeader.ProviderId,kWperfMetadataGuid)) {
            if(!DecodeWperfMetadata(*record,context->sampling_metadata))++trace->quality.decode_errors;
        } else if(IsEqualGUID(provider,kPerfInfoGuid) && code>=72 && code<=74) {
            if(!DecodeProfileInterval(*record,context->sampling_metadata))++trace->quality.decode_errors;
        } else if((IsEqualGUID(provider,kPerfInfoGuid)||IsEqualGUID(provider,kSystemTraceGuid))&&(code==46||code==47)) {
            SampleRecord sample;
            if(!DecodeProfileSample(*record,context->sampling_metadata,sample)) { ++trace->quality.decode_errors; return; }
            if(sample.pid==0||sample.pid==UINT32_MAX) {
                if(const auto* thread=context->thread_index.Find(sample.tid,sample.timestamp_ns))sample.pid=thread->pid;
            }
            ++trace->quality.samples_total;
            if(sample.kind==SampleKind::Pmc)++trace->quality.pmc_samples;
            (*context->sample_consumer)(sample);
        }
        return;
    }
    ++trace->quality.events_total;
    const auto event_time=EtwTimestampNs(*record,trace->qpc_frequency);
    trace->start_ns=std::min(trace->start_ns,event_time); trace->end_ns=std::max(trace->end_ns,event_time);
    if(IsEqualGUID(record->EventHeader.ProviderId,kWperfMetadataGuid)) {
        if(!DecodeWperfMetadata(*record,*trace))++trace->quality.decode_errors;
        return;
    }
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
    if(context->metadata_only) {
        const bool lifecycle=(IsEqualGUID(class_guid,kProcessGuid)&&(opcode==1||opcode==2||opcode==3||opcode==4||opcode==39)) ||
            (IsEqualGUID(class_guid,kThreadGuid)&&(opcode==1||opcode==2||opcode==3||opcode==4)) || IsEqualGUID(class_guid,kImageGuid);
        if(IsEqualGUID(class_guid,kPerfInfoGuid)&&opcode>=72&&opcode<=74) {
            DecodeProfileInterval(*record,*trace); return;
        }
        if(!lifecycle)return;
    }
    const bool stored=IsEqualGUID(class_guid,kProcessGuid)||IsEqualGUID(class_guid,kThreadGuid)||
        IsEqualGUID(class_guid,kImageGuid)||IsEqualGUID(class_guid,kStackWalkGuid)||IsEqualGUID(class_guid,kPageFaultGuid)||
        IsEqualGUID(class_guid,kDiskIoGuid)||IsEqualGUID(class_guid,kFileIoGuid)||IsEqualGUID(class_guid,kLbrGuid)||
        (IsEqualGUID(class_guid,kPerfInfoGuid)&&(opcode==46||opcode==47||opcode==51||opcode==52||opcode==122));
    if(stored)context->charged_bytes+=(IsEqualGUID(class_guid,kStackWalkGuid)?4:2)*
        (uint64_t{record->UserDataLength}+sizeof(SampleRecord)+sizeof(TraceEvent));
    trace->quality.budget_estimated_bytes=context->charged_bytes;
    if(context->charged_bytes>512ull*1024*1024) {
        context->failure=L"replay exceeded 512 MiB managed budget; use report --stream or a shorter capture";
        trace->quality.budget_exceeded=true; trace->quality.budget_limit_bytes=512ull*1024*1024; trace->quality.incomplete=true; return;
    }
    const bool is_stack_event =
        IsEqualGUID(class_guid, kStackWalkGuid) ||
        IsEqualGUID(record->EventHeader.ProviderId, kStackWalkGuid);

    if (IsEqualGUID(class_guid, kProcessGuid) &&
        (opcode == 1 || opcode == 2 || opcode == 3 || opcode == 4 || opcode == 39)) {
        ParseProcess(*record, *trace, context->indexes);
    } else if (IsEqualGUID(class_guid, kThreadGuid) &&
               (opcode == 1 || opcode == 2 || opcode == 3 || opcode == 4)) {
        ParseThread(*record, *trace, context->indexes);
    } else if (IsEqualGUID(class_guid, kImageGuid) &&
               (opcode == 2 || opcode == 3 || opcode == 4 || opcode == 10)) {
        ParseImage(*record, *trace, context->indexes);
    } else if (IsEqualGUID(class_guid, kPerfInfoGuid) &&
               (opcode == 46 || opcode == 47 || id == 46 || id == 47)) {
        ++trace->quality.samples_total;
        ParseSample(*record, *trace);
    } else if (IsEqualGUID(class_guid, kPerfInfoGuid) && opcode == 122) {
        ParseContextRegisters(*record, *trace);
    } else if (IsEqualGUID(class_guid, kPerfInfoGuid) && opcode == 49) {
        if (!DecodePmcCounterMetadata(*record, *trace)) ++trace->quality.decode_errors;
    } else if (IsEqualGUID(class_guid, kPerfInfoGuid) && opcode >= 72 && opcode <= 74) {
        if (!DecodeProfileInterval(*record, *trace)) ++trace->quality.decode_errors;
    } else if ((IsEqualGUID(class_guid, kLbrGuid) ||
                IsEqualGUID(record->EventHeader.ProviderId, kLbrGuid)) &&
               event_code == 32) {
        const size_t before = trace->lbr_samples.size();
        ParseLbr(*record, *trace);
        if (trace->lbr_samples.size() > before) {
            const auto& sample = trace->lbr_samples.back();
            context->charged_bytes += sizeof(LbrSampleRecord) +
                sample.branches.size() * sizeof(LbrBranchRecord);
            trace->quality.budget_estimated_bytes = context->charged_bytes;
            if (context->charged_bytes > 512ull * 1024 * 1024) {
                context->failure = L"replay exceeded 512 MiB managed budget; use report --stream or a shorter capture";
                trace->quality.budget_exceeded = true;
                trace->quality.budget_limit_bytes = 512ull * 1024 * 1024;
                trace->quality.incomplete = true;
                return;
            }
        }
    } else if (is_stack_event && event_code == 32) {
        ++trace->quality.stack_events_total;
        ParseStack(*record, *trace, context->stacks);
    } else if (is_stack_event && (event_code == 35 || event_code == 36)) {
        ++trace->quality.stack_def_events_total;
        ParseStackDefinition(*record, *trace, context->stacks);
    } else if (is_stack_event && (event_code == 37 || event_code == 38)) {
        ++trace->quality.stack_ref_events_total;
        ParseStackReference(*record, *trace, context->stacks,
                            event_code == 37);
    } else if ((IsEqualGUID(class_guid, kThreadGuid) ||
                IsEqualGUID(record->EventHeader.ProviderId, kThreadGuid)) &&
               opcode == 36) {
        ParseKernelEvent(*record, *trace, TraceEventKind::ContextSwitch);
    } else if ((IsEqualGUID(class_guid, kThreadGuid) ||
                IsEqualGUID(record->EventHeader.ProviderId, kThreadGuid)) &&
               opcode == 50) {
        ParseKernelEvent(*record, *trace, TraceEventKind::ReadyThread);
    } else if ((IsEqualGUID(class_guid, kPageFaultGuid) ||
                IsEqualGUID(record->EventHeader.ProviderId, kPageFaultGuid)) &&
               opcode >= 10 && opcode <= 15) {
        ParseKernelEvent(*record,*trace,TraceEventKind::PageFault);
    } else if ((IsEqualGUID(class_guid, kPerfInfoGuid) ||
                IsEqualGUID(record->EventHeader.ProviderId, kPerfInfoGuid)) &&
               opcode == 51) {
        ParseKernelEvent(*record,*trace,TraceEventKind::SyscallEnter);
    } else if ((IsEqualGUID(class_guid, kPerfInfoGuid) ||
                IsEqualGUID(record->EventHeader.ProviderId, kPerfInfoGuid)) &&
               opcode == 52) {
        ParseKernelEvent(*record,*trace,TraceEventKind::SyscallExit);
    } else if (IsEqualGUID(class_guid, kDiskIoGuid) ||
               IsEqualGUID(record->EventHeader.ProviderId, kDiskIoGuid)) {
        ParseKernelEvent(*record,*trace,TraceEventKind::DiskIo);
    } else if (IsEqualGUID(class_guid, kFileIoGuid) ||
               IsEqualGUID(record->EventHeader.ProviderId, kFileIoGuid)) {
        ParseKernelEvent(*record,*trace,TraceEventKind::FileIo);
    } else if (IsEqualGUID(record->EventHeader.ProviderId, kSystemTraceGuid) &&
               (opcode == 46 || opcode == 47)) {
        ++trace->quality.samples_total;
        ParseSample(*record, *trace);
    } else {
        ++trace->quality.unknown_events;
        const std::wstring key = UnknownEventKey(record->EventHeader.ProviderId, event_code);
        if (trace->unknown_event_keys.size() < 256 || trace->unknown_event_keys.contains(key))
            ++trace->unknown_event_keys[key];
    }
}
void WINAPI EventRecordCallback(PEVENT_RECORD record) {
    try { ConsumeEventRecord(record); }
    catch(...) {
        if(record&&record->UserContext)static_cast<ReplayContext*>(record->UserContext)->failure=L"replay allocation or consumer failed";
    }
}
ULONG WINAPI ReplayBufferCallback(PEVENT_TRACE_LOGFILEW logfile) {
    return !static_cast<ReplayContext*>(logfile->Context)->failure;
}

void FinishReplay(ReplayContext& context, TraceData& trace, bool derive_off_cpu) {
    std::unordered_map<uint32_t, std::vector<const ThreadRecord*>> threads_by_tid;
    for (const ThreadRecord& thread : trace.threads) {
        threads_by_tid[thread.tid].push_back(&thread);
    }
    for (auto& [tid, threads] : threads_by_tid) {
        (void)tid;
        std::sort(threads.begin(), threads.end(),
                  [](const ThreadRecord* left, const ThreadRecord* right) {
                      return left->start_ns < right->start_ns;
                  });
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

    std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, const ResolvedStack*>>>
        scheduler_stack_index;
    for (const auto& [key, stack] : resolved_stacks) {
        const uint64_t thread_cpu =
            (static_cast<uint64_t>(key.tid) << 32) | key.cpu;
        scheduler_stack_index[thread_cpu].push_back({key.tag, &stack});
    }
    for (auto& [thread_cpu, candidates] : scheduler_stack_index) {
        (void)thread_cpu;
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& left, const auto& right) {
                      return left.first < right.first;
                  });
    }

    std::map<std::vector<uint64_t>,std::shared_ptr<std::vector<uint64_t>>> interned;
    const auto attach=[&](SampleRecord& sample,const std::vector<uint64_t>& frames) {
        auto& shared=interned[frames];
        if(!shared)shared=std::make_shared<std::vector<uint64_t>>(frames);
        sample.stack.Share(shared);
    };
    for (auto& sample : trace.samples) {
        if (sample.pid == 0 || sample.pid == UINT32_MAX) {
            auto found = threads_by_tid.find(sample.tid);
            if (found != threads_by_tid.end()) {
                const auto& threads = found->second;
                auto upper = std::upper_bound(
                    threads.begin(), threads.end(), sample.timestamp_ns,
                    [](uint64_t timestamp, const ThreadRecord* thread) {
                        return timestamp < thread->start_ns;
                    });
                while (upper != threads.begin()) {
                    --upper;
                    const ThreadRecord* thread = *upper;
                    if ((thread->start_ns == 0 ||
                         thread->start_ns <= sample.timestamp_ns) &&
                        (thread->end_ns == 0 ||
                         thread->end_ns >= sample.timestamp_ns)) {
                        sample.pid = thread->pid;
                        break;
                    }
                }
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
                    attach(sample,candidates[index]);
                    sample.stack_quality = StackQuality::Exact;
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
            if (fallback != fallback_by_leaf.end() && fallback->second.size() == 1 &&
                sample.timestamp_ns >= fallback->second.front().key.tag &&
                sample.timestamp_ns - fallback->second.front().key.tag <= 1000000 &&
                FindThread(trace, sample.tid, sample.timestamp_ns) ==
                    FindThread(trace, sample.tid, fallback->second.front().key.tag)) {
                const StackCandidateRef& reference = fallback->second.front();
                const auto candidate = resolved_stacks.find(reference.key);
                if (candidate != resolved_stacks.end() &&
                    reference.index < candidate->second.candidates.size()) {
                    attach(sample,candidate->second.candidates[reference.index]);
                    sample.stack_quality = StackQuality::Approximate;
                    ++trace.quality.stack_approximate_total;
                    attached = true;
                }
            } else if (fallback != fallback_by_leaf.end() && fallback->second.size() > 1) {
                ++trace.quality.stack_ambiguous;
            }
        }

        if (!attached) {
            sample.stack_missing = true;
            ++trace.quality.samples_without_stack;
        }
    }

    for (TraceEvent& event : trace.events) {
        if (event.kind != TraceEventKind::ContextSwitch &&
            event.kind != TraceEventKind::ReadyThread) {
            continue;
        }
        const uint32_t stack_tid =
            event.kind == TraceEventKind::ContextSwitch ? event.related_tid : event.tid;
        const StackKey key{event.timestamp_ns, stack_tid, event.cpu};
        const auto found = resolved_stacks.find(key);
        const ResolvedStack* selected = nullptr;
        if (found != resolved_stacks.end() && !found->second.ambiguous &&
            found->second.candidates.size() == 1) {
            selected = &found->second;
            event.stack_quality = StackQuality::Exact;
            ++trace.quality.event_stack_exact;
        } else if (found != resolved_stacks.end() && found->second.ambiguous) {
            event.stack_missing = true;
            event.stack_ambiguous = true;
            ++trace.quality.event_stack_ambiguous;
            continue;
        } else {
            const uint64_t thread_cpu =
                (static_cast<uint64_t>(stack_tid) << 32) | event.cpu;
            const auto index = scheduler_stack_index.find(thread_cpu);
            const ResolvedStack* nearest = nullptr;
            uint64_t nearest_delta = UINT64_MAX;
            if (index != scheduler_stack_index.end()) {
                for (const auto& [timestamp, candidate] : index->second) {
                    if (candidate->ambiguous || candidate->candidates.size() != 1) {
                        continue;
                    }
                    const uint64_t delta = timestamp >= event.timestamp_ns
                                               ? timestamp - event.timestamp_ns
                                               : event.timestamp_ns - timestamp;
                    if (delta <= 1000000 && delta < nearest_delta) {
                        nearest = candidate;
                        nearest_delta = delta;
                    }
                }
            }
            if (nearest != nullptr) {
                selected = nearest;
                event.stack_quality = StackQuality::Approximate;
                ++trace.quality.event_stack_approximate;
            }
        }
        if (selected == nullptr) {
            event.stack_missing = true;
            ++trace.quality.event_stack_missing;
            continue;
        }
        const auto& frames = selected->candidates.front();
        auto& shared = interned[frames];
        if (!shared) {
            shared = std::make_shared<std::vector<uint64_t>>(frames);
        }
        event.stack.Share(shared);
        event.stack_missing = false;
    }

    EnrichTraceEvents(trace);
    if (derive_off_cpu) BuildOffCpuIntervals(trace);
    FinalizeTraceQuality(trace);
}

}  // namespace

bool ReadEtwPass(const std::filesystem::path& path, TraceData& trace,
                 std::wstring* error,bool metadata_only,const std::function<void(const SampleRecord&)>* consumer,bool derive_off_cpu) {
    if(!consumer)trace = TraceData{};
    trace.source_path = path.wstring();
    trace.quality.budget_limit_bytes = 512ull * 1024 * 1024;

    ReplayContext context;
    context.trace = &trace;
    if(consumer)context.thread_index.Build(trace);
    context.metadata_only=metadata_only; context.sample_consumer=consumer;
    context.sampling_metadata.pointer_size=trace.pointer_size; context.sampling_metadata.qpc_frequency=trace.qpc_frequency;
    context.sampling_metadata.session_name=trace.session_name;

    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LogFileName = const_cast<wchar_t*>(path.c_str());
    logfile.ProcessTraceMode =
        PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    logfile.EventRecordCallback = EventRecordCallback;
    logfile.BufferCallback=ReplayBufferCallback;
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
    if (logfile.LogfileHeader.PointerSize == 4 ||
        logfile.LogfileHeader.PointerSize == 8 ||
        logfile.LogfileHeader.PointerSize == 32 ||
        logfile.LogfileHeader.PointerSize == 64) {
        trace.pointer_size = logfile.LogfileHeader.PointerSize <= 8
                                 ? logfile.LogfileHeader.PointerSize : logfile.LogfileHeader.PointerSize / 8;
    }
    const ULONG result = ProcessTrace(&handle, 1, nullptr, nullptr);
    CloseTrace(handle);
    if (context.failure) { if(error)*error=context.failure; return false; }
    if (result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = L"ProcessTrace failed: " + FormatWindowsError(result);
        }
        return false;
    }

    if(!metadata_only&&!consumer) FinishReplay(context,trace,derive_off_cpu);
    else FinalizeTraceQuality(trace);
    return true;
}

bool ReadEtwFile(const std::filesystem::path& path,TraceData& trace,std::wstring* error,bool derive_off_cpu) {
    return ReadEtwPass(path,trace,error,false,nullptr,derive_off_cpu);
}
bool ReadEtwMetadata(const std::filesystem::path& path,TraceData& trace,std::wstring* error) {
    trace.stacks_loaded=false;
    const bool result=ReadEtwPass(path,trace,error,true,nullptr,false);
    trace.stacks_loaded=false;
    return result;
}
bool StreamEtwSamples(const std::filesystem::path& path,TraceData& trace,
    const std::function<void(const SampleRecord&)>& consumer,std::wstring* error) {
    trace.quality.samples_total=trace.quality.samples_without_stack=trace.quality.pmc_samples=0;
    return ReadEtwPass(path,trace,error,false,&consumer,false);
}

bool ReadEtwRecords(std::span<const EVENT_RECORD> records,TraceData& trace,uint64_t frequency) {
    trace={}; trace.qpc_frequency=frequency;
    ReplayContext context; context.trace=&trace;
    for(const auto& source:records) {
        EVENT_RECORD record=source; record.UserContext=&context;
        EventRecordCallback(&record);
    }
    FinishReplay(context,trace,true);
    return true;
}

}  // namespace wperf
