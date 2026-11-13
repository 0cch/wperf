#pragma once

#include "trace_model.h"
#include <windows.h>
#include <evntcons.h>

namespace wperf {

uint32_t EtwProcessorIndex(const EVENT_RECORD& record);
bool DecodeProfileSample(const EVENT_RECORD& record, const TraceData& metadata,
                         SampleRecord& sample);
bool DecodeProfileInterval(const EVENT_RECORD& record, TraceData& metadata);
void CopyEtwPmcCounters(const EVENT_RECORD& record, std::vector<uint64_t>& counters,
                        TraceQuality& quality);

}  // namespace wperf
