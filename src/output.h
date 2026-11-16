#pragma once

#include <nlohmann/json.hpp>

#include <ostream>
#include <string>
#include <string_view>

#include "common.h"
#include "trace_model.h"
#include "pool_tags.h"

namespace wperf {

inline std::wstring EscapeCsv(std::wstring_view value) {
    std::wstring result = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') result += L'"';
        result += ch;
    }
    return result + L'"';
}

inline nlohmann::ordered_json QualityJson(const TraceData& trace) {
    const auto& q = trace.quality;
    nlohmann::ordered_json value;
    value["events_total"] = q.events_total;
    value["samples"] = q.samples_total;
    value["samples_dropped"] = q.samples_dropped;
    value["pmc_samples"] = q.pmc_samples;
    value["stack_events_total"] = q.stack_events_total;
    value["stack_ref_events_total"] = q.stack_ref_events_total;
    value["stack_def_events_total"] = q.stack_def_events_total;
    value["stack_key_mismatch"] = q.stack_key_mismatch;
    value["exact_stacks"] = q.stack_exact_total;
    value["approximate_stacks"] = q.stack_approximate_total;
    value["missing_stacks"] = q.samples_without_stack;
    value["lost_events"] = q.lost_events;
    value["lost_buffers"] = q.lost_buffers;
    value["ring_dropped"] = q.ring_dropped;
    value["metadata_dropped"] = q.metadata_dropped;
    value["context_register_events"] = q.context_register_events;
    value["off_cpu_blocked_intervals"] = q.off_cpu_blocked_intervals;
    value["off_cpu_ready_intervals"] = q.off_cpu_ready_intervals;
    value["off_cpu_exact_stacks"] = q.off_cpu_stack_exact;
    value["off_cpu_approximate_stacks"] = q.off_cpu_stack_approximate;
    value["off_cpu_ambiguous_stacks"] = q.off_cpu_stack_ambiguous;
    value["off_cpu_missing_stacks"] = q.off_cpu_stack_missing;
    value["off_cpu_preemptions"] = q.off_cpu_preemptions;
    value["off_cpu_duplicate_ready"] = q.off_cpu_duplicate_ready;
    value["off_cpu_unclosed"] = q.off_cpu_unclosed;
    value["off_cpu_inferred_intervals"] = q.off_cpu_inferred_intervals;
    value["off_cpu_evicted"] = q.off_cpu_evicted;
    value["event_stack_exact"] = q.event_stack_exact;
    value["event_stack_approximate"] = q.event_stack_approximate;
    value["event_stack_ambiguous"] = q.event_stack_ambiguous;
    value["event_stack_missing"] = q.event_stack_missing;
    value["pmc_counter_events"] = q.pmc_counter_events;
    value["pmc_counter_resets"] = q.pmc_counter_resets;
    value["lbr_events"] = q.lbr_events;
    value["lbr_branches"] = q.lbr_branches;
    value["sched_unknown_pid"] = q.sched_unknown_pid;
    value["io_init_events"] = q.io_init_events;
    value["io_complete_events"] = q.io_complete_events;
    value["io_orphaned"] = q.io_orphaned;
    value["decode_errors"] = q.decode_errors;
    value["diagnostic_unsupported"] = q.diagnostic_unsupported;
    if (trace.heap_target_pid) value["heap_module_lifecycle"] = trace.heap_snapshot_metadata ? "attach-snapshot" : "unavailable";
    if (trace.pool_tag_filter) value["pool_tag_filter"] = ToUtf8(PoolTagText(trace.pool_tag_filter));
    value["correlation_evicted"] = q.correlation_evicted;
    value["syscall_orphaned"] = q.syscall_orphaned;
    value["unknown_events"] = q.unknown_events;
    nlohmann::ordered_json unknown = nlohmann::ordered_json::object();
    for (const auto& [key, count] : trace.unknown_event_keys) {
        unknown[ToUtf8(key)] = count;
    }
    value["unknown_event_keys"] = std::move(unknown);
    value["profile_interval_ns"] = trace.profile_interval_ns;
    value["budget_limit_bytes"] = q.budget_limit_bytes;
    value["budget_estimated_bytes"] = q.budget_estimated_bytes;
    value["budget_exceeded"] = q.budget_exceeded;
    value["stacks_loaded"] = trace.stacks_loaded;
    value["stack_assessment"] = trace.stacks_loaded ? "loaded" : "not-loaded";
    value["incomplete"] = q.incomplete;
    return value;
}

inline void WriteQualityJson(const TraceData& trace, std::wostream& output) {
    output << ToUtf16(QualityJson(trace).dump());
}

}  // namespace wperf
