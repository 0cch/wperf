#pragma once

#include "symbolizer.h"
#include "trace_model.h"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace wperf {

struct CpuReportOptions {
    std::optional<uint32_t> pid;
    std::optional<uint32_t> tid;
    std::optional<uint32_t> profile_source;
    bool exact_stacks_only = false;
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
                    const CpuReportOptions& options, std::wostream& output);

}  // namespace wperf
