#include "trace_events.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace wperf {
namespace {

class ThreadResolver {
public:
    explicit ThreadResolver(const std::vector<ThreadRecord>& threads) {
        for (const ThreadRecord& thread : threads) {
            threads_[thread.tid].push_back(&thread);
        }
        for (auto& [tid, records] : threads_) {
            (void)tid;
            std::sort(records.begin(), records.end(),
                      [](const ThreadRecord* left, const ThreadRecord* right) {
                          return left->start_ns < right->start_ns;
                      });
        }
    }

    const ThreadRecord* Find(uint32_t tid, uint64_t timestamp_ns) const {
        const auto found = threads_.find(tid);
        if (found == threads_.end()) {
            return nullptr;
        }
        const auto& records = found->second;
        auto upper = std::upper_bound(
            records.begin(), records.end(), timestamp_ns,
            [](uint64_t timestamp, const ThreadRecord* thread) {
                return timestamp < thread->start_ns;
            });
        while (upper != records.begin()) {
            --upper;
            const ThreadRecord* thread = *upper;
            if ((thread->start_ns == 0 || thread->start_ns <= timestamp_ns) &&
                (thread->end_ns == 0 || thread->end_ns >= timestamp_ns)) {
                return thread;
            }
        }
        return nullptr;
    }
    uint32_t Resolve(uint32_t tid, uint64_t timestamp_ns) const {
        const auto* thread = Find(tid, timestamp_ns);
        return thread ? thread->pid : 0;
    }

private:
    std::unordered_map<uint32_t, std::vector<const ThreadRecord*>> threads_;
};


}  // namespace

const wchar_t* TraceWaitReasonName(uint32_t reason) {
    static const wchar_t* kNames[] = {
        L"Executive",       L"FreePage",     L"PageIn",
        L"SystemAllocation", L"ExecutionDelay", L"Suspended",
        L"UserRequest",     L"EventPairHigh", L"EventPairLow",
        L"LpcReceive",      L"LpcReply",      L"VirtualMemory",
        L"PageOut",         L"Unknown",
    };
    return reason < std::size(kNames) ? kNames[reason] : L"Unknown";
}

const wchar_t* TraceThreadStateName(uint32_t state) {
    static const wchar_t* kNames[] = {
        L"Initialized", L"Ready", L"Running", L"Standby",
        L"Terminated",  L"Wait",  L"Transition", L"Unknown",
    };
    return state < std::size(kNames) ? kNames[state] : L"Unknown";
}

void TraceCorrelationState::Reset() {
    ready_.clear(); syscalls_.clear(); files_.clear(); disks_.clear(); file_names_.clear();
    last_prune_ns_ = 0;
}

void TraceCorrelationState::Enrich(TraceData& trace) {
    ThreadResolver threads(trace.threads);
    const auto valid_tid = [](uint32_t tid) { return tid != 0 && tid != UINT32_MAX; };
    const auto make_pending = [&](const TraceEvent& event, uint32_t tid, uint32_t pid) {
        Pending pending{event.timestamp_ns,event.offset,event.size,0,pid,event.file_key,event.file_path,event.operation_id,tid};
        if (const auto* thread = threads.Find(tid, event.timestamp_ns)) {
            pending.thread_start_ns = thread->start_ns;
            pending.pid = thread->pid;
        }
        return pending;
    };
    const auto matches = [&](const Pending& pending, const TraceEvent& event, uint32_t tid) {
        if (event.timestamp_ns < pending.timestamp_ns ||
            event.timestamp_ns - pending.timestamp_ns > maximum_age_ns_) return false;
        if (const auto* thread = threads.Find(tid, event.timestamp_ns))
            return pending.pid == thread->pid && pending.thread_start_ns == thread->start_ns;
        return pending.thread_start_ns == 0 &&
               (pending.pid == 0 || pending.pid == UINT32_MAX ||
                event.pid == 0 || event.pid == UINT32_MAX || pending.pid == event.pid);
    };
    const auto remember = [&](auto& map, uint64_t key, Pending pending) {
        if (maximum_pending_ == 0) { ++trace.quality.correlation_evicted; return; }
        if (map.size() >= maximum_pending_ && !map.contains(key)) {
            trace.quality.correlation_evicted += map.size();
            map.clear();
        }
        map[key] = pending;
    };
    for (TraceEvent& event : trace.events) {
        if (event.timestamp_ns >= last_prune_ns_ &&
            event.timestamp_ns - last_prune_ns_ >= 1000000000ull) {
            for (auto* map : {&ready_, &syscalls_, &files_, &disks_}) {
                const auto old_size = map->size();
                std::erase_if(*map, [&](const auto& pair) {
                    return event.timestamp_ns >= pair.second.timestamp_ns &&
                           event.timestamp_ns - pair.second.timestamp_ns > maximum_age_ns_;
                });
                trace.quality.correlation_evicted += old_size - map->size();
            }
            last_prune_ns_ = event.timestamp_ns;
        }
        if (valid_tid(event.related_tid) && event.related_pid == 0)
            event.related_pid = threads.Resolve(event.related_tid, event.timestamp_ns);
        if(event.pid==0 || event.pid==UINT32_MAX) {
            if(const auto* thread=threads.Find(event.tid,event.timestamp_ns))event.pid=thread->pid;
        }
        if(event.kind==TraceEventKind::FileIo && !event.file_path.empty()) {
            if(file_names_.size()>=maximum_pending_ && maximum_pending_!=0) {
                trace.quality.correlation_evicted+=file_names_.size(); file_names_.clear();
            }
            if(maximum_pending_!=0)file_names_[event.file_key ? event.file_key : event.file_object]=event.file_path;
        }
        if(event.file_path.empty()) {
            auto name=file_names_.find(event.file_key ? event.file_key : event.file_object);
            if(name!=file_names_.end())event.file_path=name->second;
        }
        switch (event.kind) {
            case TraceEventKind::ContextSwitch: {
                const uint32_t tid = valid_tid(event.tid) ? event.tid : static_cast<uint32_t>(event.value2);
                if (valid_tid(tid)) {
                    event.tid = tid;
                    if (event.pid == 0 || event.pid == UINT32_MAX)
                        event.pid = threads.Resolve(tid, event.timestamp_ns);
                    if (event.pid == 0) ++trace.quality.sched_unknown_pid;
                    const auto pending = ready_.find(tid);
                    if (pending != ready_.end()) {
                        if (matches(pending->second, event, tid)) {
                            event.duration_ns = event.timestamp_ns - pending->second.timestamp_ns;
                            event.has_duration = true;
                        }
                        ready_.erase(pending);
                    }
                }
                if (event.related_tid == 0) event.related_tid = static_cast<uint32_t>(event.value1);
                if (valid_tid(event.related_tid) && event.related_pid == 0) {
                    event.related_pid = threads.Resolve(event.related_tid, event.timestamp_ns);
                    if (event.related_pid == 0) ++trace.quality.sched_unknown_pid;
                }
                break;
            }
            case TraceEventKind::ReadyThread:
                if (event.related_tid == 0) event.related_tid = static_cast<uint32_t>(event.value1);
                if (valid_tid(event.related_tid)) {
                    event.related_pid = threads.Resolve(event.related_tid, event.timestamp_ns);
                    if (event.related_pid == 0) ++trace.quality.sched_unknown_pid;
                    remember(ready_, event.related_tid, make_pending(event, event.related_tid, event.related_pid));
                }
                break;
            case TraceEventKind::SyscallEnter:
                if (valid_tid(event.tid)) {
                    if (syscalls_.contains(event.tid)) ++trace.quality.syscall_orphaned;
                    remember(syscalls_, event.tid, make_pending(event, event.tid, event.pid));
                }
                break;
            case TraceEventKind::SyscallExit: {
                if (!valid_tid(event.tid)) break;
                const auto pending = syscalls_.find(event.tid);
                if (pending != syscalls_.end()) {
                    if (matches(pending->second, event, event.tid)) {
                        event.duration_ns = event.timestamp_ns - pending->second.timestamp_ns;
                        event.has_duration = true;
                    } else ++trace.quality.syscall_orphaned;
                    syscalls_.erase(pending);
                } else ++trace.quality.syscall_orphaned;
                break;
            }
            case TraceEventKind::FileIo:
            case TraceEventKind::DiskIo: {
                if (event.correlation_id == 0) break;
                auto& pending_map = event.kind == TraceEventKind::FileIo ? files_ : disks_;
                if (event.is_completion) {
                    ++trace.quality.io_complete_events;
                    const auto pending = pending_map.find(event.correlation_id);
                    if (pending != pending_map.end()) {
                        if (event.timestamp_ns >= pending->second.timestamp_ns &&
                            event.timestamp_ns - pending->second.timestamp_ns <= maximum_age_ns_) {
                            if (!event.has_duration) {
                                event.duration_ns = event.timestamp_ns - pending->second.timestamp_ns;
                                event.has_duration = true;
                            }
                            if(event.file_path.empty())event.file_path=pending->second.file_path;
                            event.operation_id=pending->second.operation_id;
                            if(event.file_key==0)event.file_key=pending->second.file_key;
                            if(pending->second.pid!=0&&pending->second.pid!=UINT32_MAX)event.pid=pending->second.pid;
                            if(pending->second.tid!=0&&pending->second.tid!=UINT32_MAX)event.tid=pending->second.tid;
                            if (event.size == 0) event.size = pending->second.size;
                            if (event.offset == 0) event.offset = pending->second.offset;
                        } else ++trace.quality.io_orphaned;
                        pending_map.erase(pending);
                    } else ++trace.quality.io_orphaned;
                } else if ((event.kind == TraceEventKind::FileIo && event.event_id >= 64 && event.event_id <= 77) ||
                           (event.kind == TraceEventKind::DiskIo && event.event_id >= 12 && event.event_id <= 15)) {
                    ++trace.quality.io_init_events;
                    remember(pending_map, event.correlation_id, make_pending(event, event.tid, event.pid));
                }
                break;
            }
            default: break;
        }
    }
    trace.quality.incomplete = trace.quality.incomplete || trace.quality.lost_events != 0 ||
        trace.quality.lost_buffers != 0 || trace.quality.ring_dropped != 0 ||
        trace.quality.decode_errors != 0 || trace.quality.metadata_dropped != 0 ||
        trace.quality.correlation_evicted != 0;
}

void EnrichTraceEvents(TraceData& trace) {
    TraceCorrelationState state;
    state.Enrich(trace);
}

}  // namespace wperf
