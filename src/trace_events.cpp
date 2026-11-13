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

    uint32_t Resolve(uint32_t tid, uint64_t timestamp_ns) const {
        const auto found = threads_.find(tid);
        if (found == threads_.end()) {
            return 0;
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
                return thread->pid;
            }
        }
        return 0;
    }

private:
    std::unordered_map<uint32_t, std::vector<const ThreadRecord*>> threads_;
};

struct PendingIo {
    uint64_t timestamp_ns = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
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

void EnrichTraceEvents(TraceData& trace) {
    ThreadResolver threads(trace.threads);
    std::unordered_map<uint32_t, uint64_t> ready_time;
    std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> syscall_pending;
    std::unordered_map<uint64_t, PendingIo> io_pending;
    std::unordered_map<uint64_t, uint64_t> disk_pending;

    for (TraceEvent& event : trace.events) {
        if (event.related_tid != 0 && event.related_pid == 0) {
            event.related_pid = threads.Resolve(event.related_tid, event.timestamp_ns);
        }

        switch (event.kind) {
            case TraceEventKind::ContextSwitch: {
                const uint32_t new_tid =
                    event.tid != 0 && event.tid != UINT32_MAX
                        ? event.tid
                        : static_cast<uint32_t>(event.value2);
                if (new_tid != 0 && new_tid != UINT32_MAX) {
                    event.tid = new_tid;
                    if (event.pid == 0 || event.pid == UINT32_MAX) {
                        event.pid = threads.Resolve(new_tid, event.timestamp_ns);
                    }
                    if (event.pid == 0) {
                        ++trace.quality.sched_unknown_pid;
                    }
                    const auto ready = ready_time.find(new_tid);
                    if (ready != ready_time.end()) {
                        event.duration_ns = event.timestamp_ns >= ready->second
                                                ? event.timestamp_ns - ready->second
                                                : 0;
                        event.has_duration = true;
                        ready_time.erase(ready);
                    }
                }
                if (event.related_tid == 0) {
                    event.related_tid = static_cast<uint32_t>(event.value1);
                }
                if (event.related_tid != 0 && event.related_pid == 0) {
                    event.related_pid =
                        threads.Resolve(event.related_tid, event.timestamp_ns);
                    if (event.related_pid == 0) {
                        ++trace.quality.sched_unknown_pid;
                    }
                }
                break;
            }
            case TraceEventKind::ReadyThread: {
                if (event.related_tid == 0) {
                    event.related_tid = static_cast<uint32_t>(event.value1);
                }
                if (event.related_tid != 0) {
                    event.related_pid =
                        threads.Resolve(event.related_tid, event.timestamp_ns);
                    if (event.related_pid == 0) {
                        ++trace.quality.sched_unknown_pid;
                    }
                    ready_time[event.related_tid] = event.timestamp_ns;
                }
                break;
            }
            case TraceEventKind::SyscallEnter:
                if (event.tid != 0 && event.tid != UINT32_MAX) {
                    syscall_pending[event.tid] = {event.timestamp_ns, 0};
                }
                break;
            case TraceEventKind::SyscallExit: {
                const auto pending = syscall_pending.find(event.tid);
                if (pending != syscall_pending.end()) {
                    event.duration_ns = event.timestamp_ns >= pending->second.first
                                            ? event.timestamp_ns - pending->second.first
                                            : 0;
                    event.has_duration = true;
                    syscall_pending.erase(pending);
                }
                break;
            }
            case TraceEventKind::FileIo:
                if (event.correlation_id != 0) {
                    if (!event.is_completion && event.event_id >= 64 &&
                        event.event_id <= 77) {
                        ++trace.quality.io_init_events;
                        io_pending[event.correlation_id] =
                            PendingIo{event.timestamp_ns, event.offset, event.size};
                        if (io_pending.size() > 200000) {
                            io_pending.erase(io_pending.begin());
                        }
                    } else if (event.is_completion) {
                        ++trace.quality.io_complete_events;
                        const auto pending = io_pending.find(event.correlation_id);
                        if (pending != io_pending.end()) {
                            event.duration_ns =
                                event.timestamp_ns >= pending->second.timestamp_ns
                                    ? event.timestamp_ns - pending->second.timestamp_ns
                                    : 0;
                            event.has_duration = true;
                            if (event.offset == 0) {
                                event.offset = pending->second.offset;
                            }
                            if (event.size == 0) {
                                event.size = pending->second.size;
                            }
                            io_pending.erase(pending);
                        } else {
                            ++trace.quality.io_orphaned;
                        }
                    }
                }
                break;
            case TraceEventKind::DiskIo:
                if (event.correlation_id != 0) {
                    if (event.is_completion) {
                        ++trace.quality.io_complete_events;
                        const auto pending = disk_pending.find(event.correlation_id);
                        if (pending != disk_pending.end()) {
                            if (!event.has_duration) {
                                event.duration_ns =
                                    event.timestamp_ns >= pending->second
                                        ? event.timestamp_ns - pending->second
                                        : 0;
                                event.has_duration = true;
                            }
                            disk_pending.erase(pending);
                        } else {
                            ++trace.quality.io_orphaned;
                        }
                    } else if (event.event_id >= 12 && event.event_id <= 15) {
                        ++trace.quality.io_init_events;
                        disk_pending[event.correlation_id] = event.timestamp_ns;
                    }
                }
                break;
            default:
                break;
        }
    }
}

}  // namespace wperf