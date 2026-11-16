#pragma once

#include "symbolizer.h"
#include "trace_model.h"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>
#include <functional>

namespace wperf {

class JsonStreamWriter;

class CpuReportLimitError : public std::runtime_error {
public:
    explicit CpuReportLimitError(const char* message) : std::runtime_error(message) {}
};

struct CpuReportOptions {
    enum class WeightMode { Period, Samples } weight_mode = WeightMode::Period;
    bool quiet = false;
    std::optional<uint32_t> pid;
    std::optional<uint32_t> tid;
    std::optional<uint32_t> profile_source;
    bool exact_stacks_only = false;
    bool off_cpu_include_inferred = false;
    std::optional<uint32_t> cpu;
    std::optional<uint64_t> start_offset_ns, end_offset_ns;
    bool include_children = false;
    enum class Scope { All, User, Kernel } scope = Scope::All;
    std::wstring comm_filter;
    std::wstring dso_filter;
    std::wstring symbol_filter;
    double percent_limit = 0.0;
    bool folded = false;
    bool call_tree = false;
    bool call_tree_caller = false;
    bool csv = false;
    bool json = false;
    std::vector<std::wstring> sort_fields;
};

void WriteCpuReport(const TraceData& trace, Symbolizer& symbolizer,
                    const CpuReportOptions& options, std::wostream& output,
                    JsonStreamWriter* json_writer = nullptr);
bool MatchesSampleSelection(const TraceData& trace, const SampleRecord& sample,
                             const CpuReportOptions& options);
void WriteCpuReportFeed(const TraceData& metadata, Symbolizer& symbolizer,
                        const CpuReportOptions& options, std::wostream& output,
                        const std::function<void(const std::function<void(const SampleRecord&)>&)>& feed,
                        JsonStreamWriter* json_writer = nullptr);

}  // namespace wperf
