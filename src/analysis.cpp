#include "analysis.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace wperf {
std::vector<PmcDeltaRow> AggregatePmcCounters(const TraceData& trace) {
    std::map<std::pair<uint32_t,uint32_t>,std::pair<uint64_t,uint64_t>> previous;
    std::map<std::tuple<uint32_t,uint32_t,uint32_t,uint32_t>,PmcDeltaRow> rows;
    for(const auto& event:trace.events) {
        if(event.pmc_counters.size()!=trace.pmc_counter_sources.size())continue;
        for(size_t slot=0;slot<event.pmc_counters.size();++slot) {
            const uint32_t source=trace.pmc_counter_sources[slot];
            const uint64_t value=event.pmc_counters[slot];
            auto found=previous.find({event.cpu,source});
            if(found!=previous.end()&&event.timestamp_ns>found->second.first&&value>=found->second.second) {
                const uint32_t tid=event.kind==TraceEventKind::ContextSwitch?event.related_tid:UINT32_MAX;
                const uint32_t pid=tid==0?0:(event.related_pid!=0?event.related_pid:UINT32_MAX);
                auto& row=rows[{event.cpu,source,pid,tid}];
                row.cpu=event.cpu; row.source=source; row.pid=pid; row.tid=tid;
                row.delta+=value-found->second.second; ++row.intervals;
            }
            previous[{event.cpu,source}]={event.timestamp_ns,value};
        }
    }
    std::vector<PmcDeltaRow> result;
    for(const auto& [key,row]:rows) { (void)key; result.push_back(row); }
    return result;
}
std::map<uint32_t,SchedulerRow> AnalyzeScheduler(const TraceData& trace) {
    struct Run { uint64_t time; uint32_t pid,tid; };
    struct Block { uint64_t time; uint32_t pid; };
    std::unordered_map<uint32_t,Run> cpus;
    std::unordered_map<uint32_t,Block> blocked;
    std::map<uint32_t,SchedulerRow> rows;
    const auto row_for=[&](uint32_t pid)->SchedulerRow& { auto& row=rows[pid]; row.pid=pid; return row; };
    for(const auto& event:trace.events) {
        if(event.kind==TraceEventKind::ContextSwitch) {
            const auto previous=cpus.find(event.cpu);
            if(previous!=cpus.end()&&previous->second.tid==event.related_tid&&event.timestamp_ns>=previous->second.time)
                row_for(previous->second.pid).running_ns+=event.timestamp_ns-previous->second.time;
            const uint32_t pid=event.tid==0?0:(event.pid==0?UINT32_MAX:event.pid);
            cpus[event.cpu]={event.timestamp_ns,pid,event.tid};
            if(event.has_duration) { row_for(pid).ready_ns+=event.duration_ns; row_for(pid).wakeup_ns.push_back(event.duration_ns); }
            if(event.previous_state==5&&event.related_tid!=0&&event.related_tid!=UINT32_MAX)
                blocked[event.related_tid]={event.timestamp_ns,event.related_pid==0?UINT32_MAX:event.related_pid};
        } else if(event.kind==TraceEventKind::ReadyThread) {
            const auto old=blocked.find(event.related_tid);
            if(old!=blocked.end()) {
                if(event.timestamp_ns>=old->second.time && old->second.pid==event.related_pid)
                    row_for(old->second.pid).blocked_ns+=event.timestamp_ns-old->second.time;
                blocked.erase(old);
            }
        }
    }
    return rows;
}
uint64_t Percentile(std::vector<uint64_t> values,double percentile) {
    if(values.empty())return 0;
    std::sort(values.begin(),values.end());
    const auto index=static_cast<size_t>(std::ceil((values.size()-1)*std::clamp(percentile,0.0,1.0)));
    return values[index];
}
}
