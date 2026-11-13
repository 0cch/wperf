#pragma once

#include "trace_model.h"
#include <windows.h>
#include <evntcons.h>

namespace wperf {
inline constexpr GUID kWperfMetadataGuid={0x21ae8bf0,0xf030,0x4b48,{0xaf,0x11,0x92,0x9c,0x67,0x04,0x51,0x1e}};
bool DecodeWperfMetadata(const EVENT_RECORD& record, TraceData& metadata);

uint32_t EtwProcessorIndex(const EVENT_RECORD& record);
bool DecodeProfileSample(const EVENT_RECORD& record, const TraceData& metadata,
                         SampleRecord& sample);
bool DecodeProfileInterval(const EVENT_RECORD& record, TraceData& metadata);
bool DecodePmcCounterMetadata(const EVENT_RECORD& record, TraceData& metadata);
bool DecodeContextRegisters(const EVENT_RECORD& record, uint32_t pointer_size,
                             ContextRegisterRecord& registers);
void CopyEtwPmcCounters(const EVENT_RECORD& record, std::vector<uint64_t>& counters,
                        TraceQuality& quality);
uint64_t EtwTimestampNs(const EVENT_RECORD& record, uint64_t frequency);
uint32_t EtwPointerSize(const EVENT_RECORD& record, uint32_t fallback);
bool DecodeKernelEvent(const EVENT_RECORD& record, uint32_t pointer_size, uint64_t frequency,
                        TraceEventKind kind, TraceEvent& event);
bool DecodeImagePayload(const EVENT_RECORD& record, uint32_t pointer_size, uint64_t frequency,
                         ImageRecord& image);
bool DecodeThreadPayload(const EVENT_RECORD& record, uint64_t frequency, ThreadRecord& thread);
bool DecodeProcessPayload(const EVENT_RECORD& record, uint32_t pointer_size, uint64_t frequency,
                           ProcessRecord& process);

}  // namespace wperf
