#pragma once

#include <string>
#include <string_view>
#include <ostream>
#include "trace_model.h"

namespace wperf {
inline std::wstring EscapeJson(std::wstring_view value) {
    std::wstring result;
    for (wchar_t ch : value) {
        switch (ch) {
            case L'"': result += L"\\\""; break;
            case L'\\': result += L"\\\\"; break;
            case L'\n': result += L"\\n"; break;
            case L'\r': result += L"\\r"; break;
            case L'\t': result += L"\\t"; break;
            default:
                if (ch < 0x20) {
                    constexpr wchar_t hex[] = L"0123456789abcdef";
                    result += L"\\u00";
                    result += hex[(ch >> 4) & 15];
                    result += hex[ch & 15];
                } else result += ch;
        }
    }
    return result;
}
inline std::wstring EscapeCsv(std::wstring_view value) {
    std::wstring result = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') result += L'"';
        result += ch;
    }
    return result + L'"';
}
inline void WriteQualityJson(const TraceData& trace, std::wostream& output) {
    const auto& q = trace.quality;
    output << L"{\"samples\":" << q.samples_total
           << L",\"samples_dropped\":" << q.samples_dropped
           << L",\"pmc_samples\":" << q.pmc_samples
           << L",\"exact_stacks\":" << q.stack_exact_total
           << L",\"approximate_stacks\":" << q.stack_approximate_total
           << L",\"missing_stacks\":" << q.samples_without_stack
           << L",\"lost_events\":" << q.lost_events << L",\"lost_buffers\":" << q.lost_buffers
           << L",\"ring_dropped\":" << q.ring_dropped
           << L",\"metadata_dropped\":" << q.metadata_dropped
           << L",\"off_cpu_blocked_intervals\":" << q.off_cpu_blocked_intervals
           << L",\"off_cpu_ready_intervals\":" << q.off_cpu_ready_intervals
           << L",\"off_cpu_exact_stacks\":" << q.off_cpu_stack_exact
           << L",\"off_cpu_approximate_stacks\":" << q.off_cpu_stack_approximate
           << L",\"off_cpu_ambiguous_stacks\":" << q.off_cpu_stack_ambiguous
           << L",\"off_cpu_missing_stacks\":" << q.off_cpu_stack_missing
           << L",\"off_cpu_preemptions\":" << q.off_cpu_preemptions
           << L",\"off_cpu_duplicate_ready\":" << q.off_cpu_duplicate_ready
           << L",\"off_cpu_unclosed\":" << q.off_cpu_unclosed
           << L",\"off_cpu_inferred_intervals\":" << q.off_cpu_inferred_intervals
           << L",\"off_cpu_evicted\":" << q.off_cpu_evicted
           << L",\"event_stack_exact\":" << q.event_stack_exact
           << L",\"event_stack_approximate\":" << q.event_stack_approximate
           << L",\"event_stack_ambiguous\":" << q.event_stack_ambiguous
           << L",\"event_stack_missing\":" << q.event_stack_missing
           << L",\"decode_errors\":" << q.decode_errors
           << L",\"correlation_evicted\":" << q.correlation_evicted
           << L",\"syscall_orphaned\":" << q.syscall_orphaned
           << L",\"unknown_events\":" << q.unknown_events
           << L",\"profile_interval_ns\":" << trace.profile_interval_ns
           << L",\"stacks_loaded\":" << (trace.stacks_loaded?L"true":L"false")
           << L",\"incomplete\":" << (q.incomplete ? L"true" : L"false") << L"}";
}
}
