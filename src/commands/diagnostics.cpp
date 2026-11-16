#include "diagnostic_report.h"
#include "commands/util.h"
#include "archive.h"
#include "etl_reader.h"
#include "json_writer.h"
#include "output.h"
#include <iostream>
#include <sstream>

namespace wperf {
bool WriteReportFile(const std::filesystem::path&, const std::wstring&, std::wstring*);

int RunDiagnosticReport(const ReportRequest& request) {
    using commands::PrintError;
    const bool memory = request.kind == L"heap" || request.kind == L"pool";
    const bool system = request.kind == L"pool" || request.kind == L"dpc" ||
                        request.kind == L"isr" || request.kind == L"interrupt";
    if (request.input.empty() || (request.json && request.csv) ||
        (request.metadata && !request.json) || (request.stdio_output && (request.json || request.csv)) ||
        (request.quiet && request.verbose) || request.stream || request.folded || request.call_graph ||
        request.off_cpu || request.off_cpu_include_inferred || request.source || request.weight_explicit ||
        request.scope != L"all" || request.percent_limit || !request.sort_fields.empty() ||
        (!memory && (request.exact_stacks || request.max_missing_stacks)) ||
        (request.include_children && !request.pid) ||
        (request.time_start_ms && request.time_end_ms && *request.time_start_ms > *request.time_end_ms)) {
        PrintError(L"invalid diagnostic report options; use text/JSON/CSV, supported selectors, symbols, and quality gates");
        return 2;
    }
    if (system && (request.pid || request.tid || !request.comm.empty() || request.include_children)) {
        PrintError(L"DPC/ISR and pool are system-owned; PID/TID/comm/children filters cannot establish ownership");
        return 2;
    }
    CpuReportOptions options;
    options.pid = request.pid; options.tid = request.tid; options.cpu = request.cpu;
    options.include_children = request.include_children; options.comm_filter = request.comm;
    options.dso_filter = request.dso; options.symbol_filter = request.symbol;
    options.exact_stacks_only = request.exact_stacks;
    if (request.time_start_ms) options.start_offset_ns = *request.time_start_ms * 1000000;
    if (request.time_end_ms) options.end_offset_ns = *request.time_end_ms * 1000000;
    options.json = request.json; options.csv = request.csv;
    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(std::filesystem::absolute(request.input), trace, &error, false, request.kind)) { PrintError(error); return 6; }
    std::wstring symbol_path = request.symbol_path;
    if (!request.archive.empty()) {
        if (!LoadArchiveModules(request.archive, trace, &error)) { PrintError(error); return 6; }
        symbol_path += L";" + ArchiveSymbolPath(request.archive, trace);
    }
    Symbolizer symbols;
    if (!symbols.Initialize(symbol_path, &error) && !request.quiet) std::wcerr << L"wperf: symbolization disabled: " << error << L"\n";
    for (const auto& image : trace.images) symbols.AddModule(image);
    DiagnosticAnalysis analysis;
    try { analysis = AnalyzeDiagnostics(trace, request.kind, options); }
    catch (const CpuReportLimitError& failure) { PrintError(ToUtf16(failure.what())); return 6; }
    nlohmann::ordered_json quality = QualityJson(trace);
    quality["incomplete"] = analysis.incomplete;
    quality["analysis"] = {
        {"kind", ToUtf8(request.kind)}, {"baseline", memory ? "allocations-observed-in-capture" : "event-bounded"},
        {"unmatched_frees", analysis.unmatched_frees}, {"address_reuse", analysis.address_reuse},
        {"unmatched_entries", analysis.unmatched_entries}, {"unmatched_returns", analysis.unmatched_returns},
        {"duplicate_entries", analysis.duplicate_entries}, {"completion_points", analysis.completion_points},
        {"budget_dropped", analysis.budget_dropped}, {"missing_stacks", analysis.missing_stacks},
        {"realloc_notifications", analysis.realloc_notifications}, {"driver_context_unknown", analysis.driver_context_unknown}};
    if (!request.quiet) {
        if (request.verbose) std::wcerr << ToUtf16(quality.dump()) << L"\n";
        else std::wcerr << L"wperf: " << request.kind << L" quality incomplete=" << analysis.incomplete
            << L" lost=" << trace.quality.lost_events << L" unmatched_frees=" << analysis.unmatched_frees
            << L" unmatched_driver=" << analysis.unmatched_entries + analysis.unmatched_returns << L"\n";
        if (memory) std::wcerr << L"wperf: live bytes describe observed allocations still unfreed at the selected end; pre-capture allocations are unknown, and this is not proof of a leak\n";
        else if (request.kind == L"driver") std::wcerr << L"wperf: driver spans may include nested work and waits; no per-minifilter or device service-time decomposition is inferred\n";
        else std::wcerr << L"wperf: inclusive intervals can overlap; busy_ns is their union within each row, and rows must not be summed as exclusive CPU time\n";
    }
    std::wostringstream file;
    std::wostream& output = request.output.empty() ? std::wcout : file;
    std::unique_ptr<JsonStreamWriter> envelope;
    if (request.metadata) {
        envelope = std::make_unique<JsonStreamWriter>(output);
        envelope->BeginObject(); envelope->Key(L"rows");
    }
    const uint64_t emitted = WriteDiagnosticRows(trace, analysis, request.kind, symbols, options, output, envelope.get());
    if (envelope) {
        envelope->Key(L"quality"); envelope->Item(quality); envelope->EndObject(); output << L"\n";
    }
    if (!request.output.empty() && !WriteReportFile(request.output, file.str(), &error)) { PrintError(error); return 9; }
    if ((request.strict_quality && analysis.incomplete) ||
        (request.max_lost_events && trace.quality.lost_events > *request.max_lost_events) ||
        (request.max_missing_stacks && analysis.missing_stacks > *request.max_missing_stacks)) return 6;
    return emitted ? 0 : 5;
}
}  // namespace wperf
