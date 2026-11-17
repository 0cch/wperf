#pragma once

#include "requests.h"

namespace wperf {
int RunFlameGraph(const FlameGraphRequest& request);
int RunScript(const ScriptRequest& request);
int RunTrace(const TraceRequest& request);
int RunSched(const SchedRequest& request);
int RunReport(const ReportRequest& request);
}
