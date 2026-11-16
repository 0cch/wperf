#pragma once
#include "trace_model.h"
#include <map>
#include <tuple>
#include <functional>

namespace wperf {
struct PmcDeltaRow {
    uint32_t cpu=0, source=0, pid=UINT32_MAX, tid=0;
    uint64_t delta=0, intervals=0;
};
std::vector<PmcDeltaRow> AggregatePmcCounters(const TraceData& trace,
    const std::function<bool(uint64_t, const TraceEvent&)>& select = {});
struct SchedulerRow {
    uint32_t pid=UINT32_MAX;
    uint64_t running_ns=0, ready_ns=0, blocked_ns=0;
    std::vector<uint64_t> wakeup_ns;
};
std::map<uint32_t,SchedulerRow> AnalyzeScheduler(const TraceData& trace);
void BuildOffCpuIntervals(TraceData& trace);
uint64_t Percentile(std::vector<uint64_t> values,double percentile);
}
