#pragma once

#include "cpu_report.h"
#include "symbolizer.h"
#include "trace_model.h"

#include <ostream>

namespace wperf {

class JsonStreamWriter;

const wchar_t* OffCpuKindName(OffCpuKind kind);
const wchar_t* StackQualityName(StackQuality quality);
void WriteOffCpuReport(const TraceData& trace, Symbolizer& symbolizer,
                       const CpuReportOptions& options, std::wostream& output,
                       JsonStreamWriter* json_writer = nullptr);

}  // namespace wperf