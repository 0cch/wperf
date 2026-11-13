#pragma once

#include "trace_model.h"

namespace wperf {

const wchar_t* TraceWaitReasonName(uint32_t reason);
const wchar_t* TraceThreadStateName(uint32_t state);
void EnrichTraceEvents(TraceData& trace);

}  // namespace wperf