#pragma once
#include "cpu_report.h"
#include "requests.h"
#include <map>

namespace wperf {
struct ExecutionRow {
    std::wstring kind, module_identity, context_basis = L"system";
    uint32_t cpu = 0, pid = UINT32_MAX, tid = UINT32_MAX, major = 0, minor = 0;
    uint64_t process_start_ns = 0, timestamp_ns = 0, routine = 0;
    uint64_t inclusive_ns = 0, busy_ns = 0, max_ns = 0, p50_ns = 0, p95_ns = 0, p99_ns = 0;
    std::vector<uint64_t> durations;
    std::vector<std::pair<uint64_t, uint64_t>> spans;
};
struct MemoryRow {
    uint32_t pid = UINT32_MAX, tag = 0, pool_type = 0, session_id = UINT32_MAX;
    uint64_t process_start_ns = 0, heap = 0, timestamp_ns = 0, routine = 0;
    SharedStack stack;
    uint64_t allocations = 0, allocated_bytes = 0, frees = 0, freed_bytes = 0;
    uint64_t live_allocations = 0, live_bytes = 0, peak_live_bytes = 0;
    uint64_t lifecycle_released_bytes = 0, invalidated_bytes = 0, missing_stacks = 0;
};
struct DiagnosticAnalysis {
    std::vector<ExecutionRow> execution;
    std::vector<MemoryRow> memory;
    uint64_t unmatched_frees = 0, address_reuse = 0, unmatched_entries = 0, unmatched_returns = 0;
    uint64_t duplicate_entries = 0, completion_points = 0, budget_dropped = 0, missing_stacks = 0;
    uint64_t realloc_notifications = 0, driver_context_unknown = 0;
    bool incomplete = false;
};
bool IsDiagnosticKind(std::wstring_view kind);
DiagnosticAnalysis AnalyzeDiagnostics(const TraceData& trace, std::wstring_view kind, const CpuReportOptions& options);
uint64_t WriteDiagnosticRows(const TraceData& trace, const DiagnosticAnalysis& analysis,
    std::wstring_view kind, Symbolizer& symbols, const CpuReportOptions& options,
    std::wostream& output, JsonStreamWriter* envelope = nullptr);
int RunDiagnosticReport(const ReportRequest& request);
}  // namespace wperf
