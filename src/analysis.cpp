#include "analysis.h"
#include "trace_events.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace wperf {
std::vector<PmcDeltaRow> AggregatePmcCounters(const TraceData& trace,
    const std::function<bool(uint64_t, const TraceEvent&)>& select) {
    std::map<std::pair<uint32_t,uint32_t>,std::pair<uint64_t,uint64_t>> previous;
    std::map<std::tuple<uint32_t,uint32_t,uint32_t,uint32_t>,PmcDeltaRow> rows;
    for(const auto& event:trace.events) {
        if(event.pmc_counters.size()!=trace.pmc_counter_sources.size())continue;
        for(size_t slot=0;slot<event.pmc_counters.size();++slot) {
            const uint32_t source=trace.pmc_counter_sources[slot];
            const uint64_t value=event.pmc_counters[slot];
            auto found=previous.find({event.cpu,source});
            if(found!=previous.end()&&event.timestamp_ns>found->second.first&&value>=found->second.second &&
               (!select || select(found->second.first, event))) {
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
    trace.quality.off_cpu_blocked_intervals = trace.quality.off_cpu_ready_intervals = 0;
    trace.quality.off_cpu_stack_exact = trace.quality.off_cpu_stack_approximate = 0;
    trace.quality.off_cpu_stack_ambiguous = trace.quality.off_cpu_stack_missing = 0;
    trace.quality.off_cpu_preemptions = trace.quality.off_cpu_unclosed = 0;
    trace.quality.off_cpu_inferred_intervals = trace.quality.off_cpu_duplicate_ready = 0;
    trace.quality.off_cpu_evicted = 0;
    ThreadIndex threads;
    threads.Build(trace);
    struct Pending {
        uint32_t pid = UINT32_MAX;
        uint32_t tid = 0;
        uint32_t cpu = 0;
        uint64_t process_start_ns = 0;
        uint64_t thread_start_ns = 0;
        uint64_t thread_end_ns = 0;
        uint64_t stack_timestamp_ns = 0;
        uint64_t blocked_start_ns = 0;
        uint64_t ready_start_ns = 0;
        uint32_t wait_reason = 0;
        SharedStack stack;
        StackQuality stack_quality = StackQuality::Missing;
        bool stack_missing = true;
        bool stack_ambiguous = false;
        uint32_t wakeup_tid = 0;
        uint32_t wakeup_pid = UINT32_MAX;
        uint64_t wakeup_process_start_ns = 0;
        uint64_t wakeup_thread_start_ns = 0;
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
        const auto* thread = threads.Find(tid, timestamp_ns);
        return thread == nullptr ? UINT32_MAX : thread->pid;
    };
    const auto same_lifetime = [&](const Pending& item, uint32_t pid,
                                   uint64_t timestamp_ns) {
        if (item.thread_end_ns != 0 && timestamp_ns > item.thread_end_ns) return false;
        const auto* thread = threads.Find(item.tid, timestamp_ns);
        if (thread != nullptr) {
            if (item.thread_start_ns != 0 && item.thread_start_ns != thread->start_ns)
                return false;
            pid = thread->pid;
        }
        if (valid_pid(item.pid) && valid_pid(pid) && item.pid != pid) return false;
        const uint64_t process_start = ProcessInstanceStart(trace, pid, timestamp_ns);
        return item.process_start_ns == 0 || process_start == 0 ||
               item.process_start_ns == process_start;
    };
    const auto set_wakeup = [&](Pending& item, const TraceEvent& event) {
        item.wakeup_tid = event.tid;
        const auto* thread = threads.Find(event.tid, event.timestamp_ns);
        item.wakeup_pid = thread != nullptr ? thread->pid
                            : (valid_pid(event.pid) ? event.pid : UINT32_MAX);
        item.wakeup_thread_start_ns = thread != nullptr ? thread->start_ns : 0;
        item.wakeup_process_start_ns =
            ProcessInstanceStart(trace, item.wakeup_pid, event.timestamp_ns);
        item.wakeup_stack = event.stack;
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
        interval.stack_timestamp_ns = item.stack_timestamp_ns;
        interval.wakeup_pid = item.wakeup_pid;
        interval.wakeup_process_start_ns = item.wakeup_process_start_ns;
        interval.wakeup_thread_start_ns = item.wakeup_thread_start_ns;
        interval.wakeup_timestamp_ns = item.ready_start_ns;
        interval.kind = kind;
        interval.stack = item.stack;
        interval.wakeup_stack = item.wakeup_stack;
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
                         OffCpuKind::Blocked, inferred);
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
                    if (same_lifetime(found->second, event->related_pid, event->timestamp_ns)) {
                        close_pending(found->second, event->timestamp_ns, true);
                        ++trace.quality.off_cpu_unclosed;
                    } else {
                        ++trace.quality.off_cpu_evicted;
                    }
                    pending.erase(found);
                }
                Pending item;
                item.pid = valid_pid(event->related_pid)
                               ? event->related_pid
                               : pid_for(old_tid, event->timestamp_ns);
                item.tid = old_tid;
                item.cpu = event->cpu;
                const auto* thread = threads.Find(old_tid, event->timestamp_ns);
                item.thread_start_ns = thread == nullptr ? 0 : thread->start_ns;
                item.thread_end_ns = thread == nullptr ? 0 : thread->end_ns;
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
                    item.stack = event->stack;
                    item.stack_timestamp_ns = event->timestamp_ns;
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
                    if (same_lifetime(found->second, event->pid, event->timestamp_ns)) {
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
            if (found != pending.end() &&
                !same_lifetime(found->second, event->related_pid, event->timestamp_ns)) {
                ++trace.quality.off_cpu_evicted;
                pending.erase(found);
                found = pending.end();
            }
            if (found == pending.end()) {
                Pending item;
                item.pid = valid_pid(event->related_pid)
                               ? event->related_pid
                               : pid_for(tid, event->timestamp_ns);
                item.tid = tid;
                item.cpu = event->cpu;
                const auto* thread = threads.Find(tid, event->timestamp_ns);
                item.thread_start_ns = thread == nullptr ? 0 : thread->start_ns;
                item.thread_end_ns = thread == nullptr ? 0 : thread->end_ns;
                item.process_start_ns =
                    ProcessInstanceStart(trace, item.pid, event->timestamp_ns);
                item.ready_start_ns = event->timestamp_ns;
                set_wakeup(item, *event);
                if (pending.size() >= 200000) ++trace.quality.off_cpu_evicted;
                else pending[tid] = std::move(item);
            } else if (found->second.ready_start_ns == 0) {
                found->second.ready_start_ns = event->timestamp_ns;
                set_wakeup(found->second, *event);
            } else {
                ++trace.quality.off_cpu_duplicate_ready;
            }
        }
    }
    const uint64_t end_time =
        trace.end_ns != 0 ? trace.end_ns : (ordered.empty() ? 0 : ordered.back()->timestamp_ns);
    for (const auto& [tid, item] : pending) {
        (void)tid;
        uint64_t boundary = end_time;
        if (item.thread_end_ns != 0) boundary = std::min(boundary, item.thread_end_ns);
        if (const auto* process = FindProcess(trace, item.pid, item.stack_timestamp_ns != 0 ?
                                             item.stack_timestamp_ns : item.ready_start_ns)) {
            if (process->end_ns != 0) boundary = std::min(boundary, process->end_ns);
        }
        close_pending(item, boundary, true);
        ++trace.quality.off_cpu_unclosed;
    }
    FinalizeTraceQuality(trace);
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
