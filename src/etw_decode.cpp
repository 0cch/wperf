#include "etw_decode.h"

#include <cstring>

namespace wperf {
namespace {
template<typename T>
T Read(const void* data, size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const BYTE*>(data) + offset, sizeof(value));
    return value;
}
uint64_t QpcToNs(uint64_t value, uint64_t frequency) {
    if (frequency == 0) frequency = 10000000;
    return (value / frequency) * 1000000000ull +
           ((value % frequency) * 1000000000ull) / frequency;
}
}

uint32_t EtwProcessorIndex(const EVENT_RECORD& record) {
    return (record.EventHeader.Flags & EVENT_HEADER_FLAG_PROCESSOR_INDEX) != 0
               ? record.BufferContext.ProcessorIndex
               : record.BufferContext.ProcessorNumber;
}

void CopyEtwPmcCounters(const EVENT_RECORD& record, std::vector<uint64_t>& counters,
                        TraceQuality& quality) {
    for (USHORT index = 0; index < record.ExtendedDataCount; ++index) {
        const auto& item = record.ExtendedData[index];
        if (item.ExtType != EVENT_HEADER_EXT_TYPE_PMC_COUNTERS) continue;
        if (item.DataPtr == 0 || item.DataSize == 0 || item.DataSize % 8 != 0) {
            ++quality.decode_errors;
            return;
        }
        counters.resize(item.DataSize / 8);
        std::memcpy(counters.data(), reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(item.DataPtr)), item.DataSize);
        ++quality.pmc_counter_events;
        return;
    }
}

bool DecodeProfileSample(const EVENT_RECORD& record, const TraceData& metadata,
                         SampleRecord& sample) {
    const uint32_t p = (record.EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER)
                           ? 4 : (record.EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER)
                                     ? 8 : metadata.pointer_size;
    const auto code = record.EventHeader.EventDescriptor.Opcode != 0
                          ? record.EventHeader.EventDescriptor.Opcode
                          : record.EventHeader.EventDescriptor.Id;
    if ((p != 4 && p != 8) || (code != 46 && code != 47) ||
        record.UserData == nullptr || record.UserDataLength < p + 8) return false;
    sample = {};
    sample.timestamp_ns = QpcToNs(static_cast<uint64_t>(
        record.EventHeader.TimeStamp.QuadPart), metadata.qpc_frequency);
    sample.cpu = EtwProcessorIndex(record);
    sample.pid = record.EventHeader.ProcessId;
    sample.tid = Read<uint32_t>(record.UserData, p);
    sample.instruction_pointer = p == 8 ? Read<uint64_t>(record.UserData, 0)
                                       : Read<uint32_t>(record.UserData, 0);
    if (code == 47) {
        sample.kind = SampleKind::Pmc;
        sample.profile_source = Read<uint16_t>(record.UserData, p + 4);
        const auto found = metadata.profile_intervals.find(sample.profile_source);
        if (found != metadata.profile_intervals.end()) sample.period = found->second;
    } else {
        sample.period = metadata.profile_interval_ns;
    }
    return true;
}

bool DecodeProfileInterval(const EVENT_RECORD& record, TraceData& metadata) {
    const auto code = record.EventHeader.EventDescriptor.Opcode != 0
                          ? record.EventHeader.EventDescriptor.Opcode
                          : record.EventHeader.EventDescriptor.Id;
    if ((code != 72 && code != 73 && code != 74) ||
        record.UserData == nullptr || record.UserDataLength < 12) return false;
    if (code == 74) return true;  // CollectionEnd is not a new sampling period.
    const uint32_t source = Read<uint32_t>(record.UserData, 0);
    const uint32_t interval = Read<uint32_t>(record.UserData, 4);
    if (interval == 0) return false;
    metadata.profile_intervals[source] = interval;
    if (source == 0) metadata.profile_interval_ns = uint64_t{interval} * 100;
    return true;
}

}  // namespace wperf
