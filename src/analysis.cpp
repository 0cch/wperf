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
void BuildOffCpuIntervals(TraceData& trace) {
    trace.off_cpu_intervals.clear();
    struct Pending {
        uint32_t pid = UINT32_MAX;
        uint32_t tid = 0;
        uint32_t cpu = 0;
        uint64_t process_start_ns = 0;
        uint64_t thread_start_ns = 0;
        uint64_t blocked_start_ns = 0;
        uint64_t ready_start_ns = 0;
        uint32_t wait_reason = 0;
        SharedStack stack;
        StackQuality stack_quality = StackQuality::Missing;
        bool stack_missing = true;
        bool stack_ambiguous = false;
        uint32_t wakeup_tid = 0;
        SharedStack wakeup_stack;
    };
    std::unordered_map<uint32_t, Pending> pending;
    const auto valid_tid = [](uint32_t tid) {
        return tid != 0 && tid != UINT32_MAX;
    };
    const auto valid_pid = [](uint32_t pid) {
        return pid != 0 && pid != UINT32_MAX;
    };
    const auto pid_for = [&](uint32_t tid, uint64_t timestamp_ns) {
        const auto* thread = FindThread(trace, tid, timestamp_ns);
        return thread == nullptr ? UINT32_MAX : thread->pid;
    };
    const auto add_interval = [&](const Pending& item, uint64_t start,
                                  uint64_t end, OffCpuKind kind,
                                  bool inferred) {
        if (start == 0 || end <= start) {
            return;
        }
        OffCpuInterval interval;
        interval.start_ns = start;
        interval.end_ns = end;
        interval.duration_ns = end - start;
        interval.cpu = item.cpu;
        interval.pid = item.pid;
        interval.tid = item.tid;
        interval.wakeup_tid = item.wakeup_tid;
        interval.wait_reason = item.wait_reason;
        interval.process_start_ns = item.process_start_ns;
        interval.thread_start_ns = item.thread_start_ns;
        interval.wakeup_timestamp_ns = item.ready_start_ns;
        interval.kind = kind;
        interval.stack = item.stack.View();
        interval.wakeup_stack = item.wakeup_stack.View();
        interval.stack_quality = item.stack_quality;
        interval.stack_missing = item.stack_missing;
        interval.stack_ambiguous = item.stack_ambiguous;
        interval.boundary_inferred = inferred;
        if (inferred) {
            ++trace.quality.off_cpu_inferred_intervals;
        }
        if (kind == OffCpuKind::Blocked) {
            ++trace.quality.off_cpu_blocked_intervals;
        } else {
            ++trace.quality.off_cpu_ready_intervals;
        }
        if (item.stack_ambiguous) {
            ++trace.quality.off_cpu_stack_ambiguous;
        }
        if (item.stack_missing) {
            ++trace.quality.off_cpu_stack_missing;
        } else if (item.stack_quality == StackQuality::Exact) {
            ++trace.quality.off_cpu_stack_exact;
        } else if (item.stack_quality == StackQuality::Approximate) {
            ++trace.quality.off_cpu_stack_approximate;
        }
        trace.off_cpu_intervals.push_back(std::move(interval));
    };
    const auto close_pending = [&](const Pending& item, uint64_t end,
                                   bool end_inferred) {
        if (item.blocked_start_ns != 0) {
            const bool inferred = item.ready_start_ns == 0;
            add_interval(item, item.blocked_start_ns,
                         inferred ? end : item.ready_start_ns,
                         OffCpuKind::Blocked, inferred || end_inferred);
        }
        if (item.ready_start_ns != 0) {
            add_interval(item, item.ready_start_ns, end, OffCpuKind::Ready,
                         end_inferred);
        }
    };
    std::vector<const TraceEvent*> ordered;
    ordered.reserve(trace.events.size());
    for (const TraceEvent& event : trace.events) {
        if (event.kind == TraceEventKind::ContextSwitch ||
            event.kind == TraceEventKind::ReadyThread) {
            ordered.push_back(&event);
        }
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const TraceEvent* left, const TraceEvent* right) {
                         return left->timestamp_ns < right->timestamp_ns;
                     });
    for (const TraceEvent* event : ordered) {
        if (event->kind == TraceEventKind::ContextSwitch) {
            const uint32_t old_tid = event->related_tid;
            const uint32_t new_tid = event->tid;
            if (valid_tid(old_tid)) {
                auto found = pending.find(old_tid);
                if (found != pending.end()) {
                    close_pending(found->second, event->timestamp_ns, true);
                    ++trace.quality.off_cpu_unclosed;
                    pending.erase(found);
                }
                Pending item;
                item.pid = valid_pid(event->related_pid)
                               ? event->related_pid
                               : pid_for(old_tid, event->timestamp_ns);
                item.tid = old_tid;
                item.cpu = event->cpu;
                const auto* thread = FindThread(trace, old_tid, event->timestamp_ns);
                item.thread_start_ns = thread == nullptr ? 0 : thread->start_ns;
                item.process_start_ns =
                    ProcessInstanceStart(trace, item.pid, event->timestamp_ns);
                if (event->previous_state == 5) {
                    item.blocked_start_ns = event->timestamp_ns;
                    item.wait_reason = event->wait_reason;
                } else if (event->previous_state == 1 || event->previous_state == 2 ||
                           event->previous_state == 3) {
                    item.ready_start_ns = event->timestamp_ns;
                    if (event->previous_state == 2) {
                        ++trace.quality.off_cpu_preemptions;
                    }
                }
                if (item.blocked_start_ns != 0 || item.ready_start_ns != 0) {
                    item.stack = event->stack.View();
                    item.stack_quality = event->stack_quality;
                    item.stack_missing = event->stack_missing;
                    item.stack_ambiguous = event->stack_ambiguous;
                    if (pending.size() >= 200000) {
                        ++trace.quality.off_cpu_evicted;
                    } else {
                        pending[old_tid] = std::move(item);
                    }
                }
            }
            if (valid_tid(new_tid)) {
                auto found = pending.find(new_tid);
                if (found != pending.end()) {
                    const auto* thread = FindThread(trace, new_tid, event->timestamp_ns);
                    const bool same_lifetime =
                        thread == nullptr || found->second.thread_start_ns == 0 ||
                        thread->start_ns == found->second.thread_start_ns;
                    if (same_lifetime) {
                        close_pending(found->second, event->timestamp_ns, false);
                    } else {
                        ++trace.quality.off_cpu_evicted;
                    }
                    pending.erase(found);
                }
            }
        } else if (event->kind == TraceEventKind::ReadyThread) {
            const uint32_t tid = event->related_tid;
            if (!valid_tid(tid)) {
                continue;
            }
            auto found = pending.find(tid);
            if (found == pending.end()) {
                Pending item;
                item.pid = valid_pid(event->related_pid)
                               ? event->related_pid
                               : pid_for(tid, event->timestamp_ns);
                item.tid = tid;
                item.cpu = event->cpu;
                const auto* thread = FindThread(trace, tid, event->timestamp_ns);
                item.thread_start_ns = thread == nullptr ? 0 : thread->start_ns;
                item.process_start_ns =
                    ProcessInstanceStart(trace, item.pid, event->timestamp_ns);
                item.ready_start_ns = event->timestamp_ns;
                item.wakeup_tid = event->tid;
                item.wakeup_stack = event->stack.View();
                pending[tid] = std::move(item);
            } else if (found->second.ready_start_ns == 0) {
                found->second.ready_start_ns = event->timestamp_ns;
                found->second.wakeup_tid = event->tid;
                found->second.wakeup_stack = event->stack.View();
            } else {
                ++trace.quality.off_cpu_duplicate_ready;
            }
        }
    }
    const uint64_t end_time =
        trace.end_ns != 0 ? trace.end_ns : (ordered.empty() ? 0 : ordered.back()->timestamp_ns);
    for (const auto& [tid, item] : pending) {
        (void)tid;
        close_pending(item, end_time, true);
        ++trace.quality.off_cpu_unclosed;
    }
    std::stable_sort(trace.off_cpu_intervals.begin(), trace.off_cpu_intervals.end(),
                     [](const OffCpuInterval& left, const OffCpuInterval& right) {
                         if (left.start_ns != right.start_ns) {
                             return left.start_ns < right.start_ns;
                         }
                         return left.tid < right.tid;
                     });
}

uint64_t Percentile(std::vector<uint64_t> values,double percentile) {
    if(values.empty())return 0;
    std::sort(values.begin(),values.end());
    const auto index=static_cast<size_t>(std::ceil((values.size()-1)*std::clamp(percentile,0.0,1.0)));
    return values[index];
}
}
