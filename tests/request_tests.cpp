#include "requests.h"
#include "common.h"

#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, std::wstring_view message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L"\n";
        ++g_failures;
    }
}

template <typename T>
const T* Get(const wperf::CommandRequest& request) {
    return std::get_if<T>(&request);
}

bool Build(const std::string& command, const wperf::OptionValues& values,
           const wperf::OptionFlags& flags, wperf::CommandRequest& request,
           std::wstring* error = nullptr,
           const std::vector<std::wstring>& target = {}) {
    return wperf::BuildCommandRequest(command, values, flags, target, request, error);
}

void TestDefaults() {
    wperf::CommandRequest request;
    Check(Build("record", {}, {}, request), L"record request should build");
    if (const auto* value = Get<wperf::RecordRequest>(request)) {
        Check(value->profile == L"cpu", L"record profile default must be cpu");
        Check(value->call_graph, L"record call graph must default on");
        Check(!value->elevate && !value->stack_cache && !value->lbr,
              L"record flags must default off");
    } else {
        Check(false, L"record must produce RecordRequest");
    }

    Check(Build("script", {}, {}, request), L"script request should build");
    if (const auto* value = Get<wperf::ScriptRequest>(request)) {
        Check(value->fields == L"timestamp,cpu,pid,tid,ip,comm,dso,symbol",
              L"script field default is wrong");
        Check(value->limit == 0, L"script limit default is wrong");
    }

    Check(Build("trace", {}, {}, request), L"trace request should build");
    if (const auto* value = Get<wperf::TraceRequest>(request)) {
        Check(value->event == L"all", L"trace event default is wrong");
        Check(value->max_events == 0, L"trace max default is wrong");
    }

    Check(Build("top", {}, {}, request), L"top request should build");
    if (const auto* value = Get<wperf::TopRequest>(request)) {
        Check(value->event == L"cpu", L"top event default is wrong");
        Check(value->interval_ms == 1000, L"top interval default is wrong");
        Check(value->percent_limit == 1.0, L"top percent default is wrong");
    }

    Check(Build("report", {}, {}, request), L"report request should build");
    if (const auto* value = Get<wperf::ReportRequest>(request)) {
        Check(value->scope == L"all", L"report scope default is wrong");
        Check(value->weight == L"period" && !value->weight_explicit && !value->source, L"report weighting defaults are wrong");
    }

    Check(Build("diff", {}, {}, request), L"diff request should build");
    if (const auto* value = Get<wperf::DiffRequest>(request)) {
        Check(value->normalization == L"weight", L"diff normalization default is wrong");
        Check(value->matching == L"symbol", L"diff matching default is wrong");
    }

    Check(Build("archive", {}, {}, request), L"archive request should build");
    if (const auto* value = Get<wperf::ArchiveRequest>(request)) {
        Check(value->max_size_mb == 512, L"archive max-size default is wrong");
    }

    Check(Build("list", {}, {}, request), L"list request should build");
    if (const auto* value = Get<wperf::ListRequest>(request)) {
        Check(!value->providers && value->max == 500, L"list defaults are wrong");
    }
}

void TestPublicMappings() {
    std::wstring target_command = L"app.exe";
    wperf::CommandRequest request;
    Check(Build("report", {{"--weight","samples"},{"--source","19"}}, {}, request), L"weight/source request mapping failed");
    if (const auto* value=Get<wperf::ReportRequest>(request))
        Check(value->weight==L"samples" && value->weight_explicit && value->source==19, L"weight/source typed values were lost");
    Check(Build("diff", {{"--match","identity"}}, {}, request), L"identity matching request failed");
    if (const auto* value=Get<wperf::DiffRequest>(request)) Check(value->matching==L"identity", L"matching typed value was lost");
    Check(Build("buildid-list", {}, {{"--hash",true}}, request), L"hash request failed");
    if (const auto* value=Get<wperf::BuildIdListRequest>(request)) Check(value->hash, L"hash typed value was lost");
    const wperf::OptionValues record_values{
        {"--freq", "999"},
        {"--pid", "42"},
        {"--duration", "1500"},
        {"--output", "trace.etl"},
        {"--event", "sched"},
        {"--profile", "sched"},
        {"--pmc-source", "19"},
        {"--period", "100000"},
        {"--pmc-counters", "19,26"},
        {"--session", "demo"},
        {"--provider", "{01234567-89ab-cdef-0123-456789abcdef}"},
    };
    const wperf::OptionFlags record_flags{
        {"--elevate", true}, {"--no-call-graph", true},
        {"--stack-cache", true}, {"--lbr", true},
        {"--context-registers", true},
    };
    Check(Build("record", record_values, record_flags, request, nullptr,
                {target_command}),
          L"record mapping should build");
    if (const auto* value = Get<wperf::RecordRequest>(request)) {
        Check(value->frequency == 999 && value->pid == 42 &&
                  value->duration_ms == 1500 && value->period == 100000,
              L"record numeric mapping is wrong");
        Check(value->output == L"trace.etl" && value->event == L"sched" &&
                  value->profile == L"sched" && value->session == L"demo",
              L"record string mapping is wrong");
        Check(value->pmc_source == 19 && value->pmc_counters ==
                  std::vector<uint32_t>({19, 26}),
              L"record PMC mapping is wrong");
        Check(value->provider.has_value(), L"record provider GUID was not parsed");
        Check(value->target == std::vector<std::wstring>({target_command}),
              L"record target forwarding is wrong");
        Check(value->elevate && !value->call_graph && value->stack_cache &&
                  value->lbr && value->context_registers,
              L"record flag mapping is wrong");
    }

    const wperf::OptionValues report_values{
        {"--input", "trace.etl"}, {"--output", "out.json"},
        {"--pid", "1"}, {"--tid", "2"}, {"--comm", "app"},
        {"--dso", "app.exe"}, {"--symbol", "main"}, {"--symbol-path", "symbols"},
        {"--archive", "archive"}, {"--percent-limit", "0.5"},
        {"--sort", "samples,symbol"}, {"--call-graph", "caller"},
        {"--max-lost-events", "3"}, {"--max-missing-stacks", "4"},
        {"--cpu", "7"}, {"--time-start", "10"}, {"--time-end", "20"},
        {"--scope", "kernel"}, {"--kind", "cpu"},
    };
    const wperf::OptionFlags report_flags{
        {"--metadata", true}, {"--strict-quality", true}, {"--exact-stacks", true},
        {"--stream", true}, {"--include-children", true}, {"--folded", true},
        {"--off-cpu", true}, {"--off-cpu-include-inferred", true},
        {"--stdio", true}, {"--quiet", true}, {"--verbose", true},
        {"--csv", true}, {"--json", true},
    };
    Check(Build("report", report_values, report_flags, request),
          L"report mapping should build");
    if (const auto* value = Get<wperf::ReportRequest>(request)) {
        Check(value->pid == 1 && value->tid == 2 && value->percent_limit == 0.5 &&
                  value->max_lost_events == 3 && value->max_missing_stacks == 4 &&
                  value->cpu == 7 && value->time_start_ms == 10 &&
                  value->time_end_ms == 20,
              L"report numeric mapping is wrong");
        Check(value->sort_fields == std::vector<std::wstring>({L"samples", L"symbol"}),
              L"report sort mapping is wrong");
        Check(value->call_graph == L"caller" && value->scope == L"kernel" &&
                  value->kind == L"cpu",
              L"report string mapping is wrong");
        Check(value->metadata && value->strict_quality && value->exact_stacks &&
                  value->stream && value->include_children && value->folded &&
                  value->off_cpu && value->off_cpu_include_inferred &&
                  value->stdio_output && value->quiet && value->verbose &&
                  value->csv && value->json,
              L"report flag mapping is wrong");
    }

    const wperf::OptionValues stat_values{
        {"--pid", "5"}, {"--duration", "2000"}, {"--interval", "100"},
        {"--event", "TotalCycles"}, {"--period", "123"},
    };
    const wperf::OptionFlags stat_flags{
        {"--per-thread", true}, {"--pmu", true}, {"--json", true}, {"--csv", true},
    };
    Check(Build("stat", stat_values, stat_flags, request, nullptr, {L"helper.exe"}),
          L"stat mapping should build");
    if (const auto* value = Get<wperf::StatRequest>(request)) {
        Check(value->pid == 5 && value->duration_ms == 2000 &&
                  value->interval_ms == 100 && value->period == 123 &&
                  value->period_explicit,
              L"stat numeric mapping is wrong");
        Check(value->event == L"TotalCycles" &&
                  value->target == std::vector<std::wstring>({L"helper.exe"}),
              L"stat string/target mapping is wrong");
        Check(value->per_thread && value->pmu && value->json && value->csv,
              L"stat flag mapping is wrong");
    }
    Check(Build("stat", {{"--event", "TotalCycles"}}, {}, request),
          L"stat event request should build");
    if (const auto* value = Get<wperf::StatRequest>(request)) {
        Check(value->pmu && value->event == L"TotalCycles",
              L"stat event must imply PMC mode");
    }

    const wperf::OptionValues diff_values{
        {"--first", "before.etl"}, {"--second", "after.etl"},
        {"--symbol-path", "symbols"}, {"--archive", "archive"},
        {"--pid", "1"}, {"--first-pid", "2"}, {"--second-pid", "3"},
        {"--dso", "app.exe"}, {"--normalize", "samples"},
    };
    const wperf::OptionFlags diff_flags{
        {"--calltree", true}, {"--json", true}, {"--csv", true},
    };
    Check(Build("diff", diff_values, diff_flags, request), L"diff mapping should build");
    if (const auto* value = Get<wperf::DiffRequest>(request)) {
        Check(value->first == L"before.etl" && value->second == L"after.etl" &&
                  value->normalization == L"samples" && value->dso == L"app.exe",
              L"diff string mapping is wrong");
        Check(value->pid == 1 && value->first_pid == 2 && value->second_pid == 3,
              L"diff PID mapping is wrong");
        Check(value->calltree && value->json && value->csv,
              L"diff flag mapping is wrong");
    }

    const wperf::OptionValues archive_values{
        {"--input", "trace.etl"}, {"--output", "archive"}, {"--pid", "9"},
        {"--symbol-path", "symbols"}, {"--max-size-mb", "32"},
    };
    Check(Build("archive", archive_values, {{"--include-trace", true}}, request),
          L"archive mapping should build");
    if (const auto* value = Get<wperf::ArchiveRequest>(request)) {
        Check(value->input == L"trace.etl" && value->output == L"archive" &&
                  value->symbol_path == L"symbols" && value->pid == 9 &&
                  value->max_size_mb == 32 && value->include_trace,
              L"archive mapping is wrong");
    }

    Check(Build("list", {{"@mode", "providers"}, {"--max", "7"}},
                {{"--json", true}}, request),
          L"list providers mapping should build");
    if (const auto* value = Get<wperf::ListRequest>(request)) {
        Check(value->providers && value->max == 7 && value->json,
              L"list providers mapping is wrong");
    }
}

void TestInvalidValues() {
    wperf::CommandRequest request;
    std::wstring error;
    Check(!Build("record", {{"--provider", "not-a-guid"}}, {}, request, &error) &&
              !error.empty(),
          L"invalid provider GUID must be rejected");
    Check(!Build("record", {{"--pmc-counters", "19,0"}}, {}, request, &error) &&
              !error.empty(),
          L"zero PMC counter source must be rejected");

    const std::vector<std::pair<std::string, std::pair<std::string, std::string>>> cases{
        {"record", {"--duration", "nope"}},
        {"report", {"--pid", "4294967296"}},
        {"script", {"--limit", "-1"}},
        {"trace", {"--max", "1x"}},
        {"stat", {"--duration", "0x10"}},
        {"top", {"--iterations", "-1"}},
        {"annotate", {"--limit", "1.5"}},
        {"archive", {"--max-size-mb", "-1"}},
        {"list", {"--max", "-1"}},
        {"report", {"--percent-limit", "101"}},
        {"top", {"--percent-limit", "101"}},
        {"record", {"--profile", "not-a-profile"}},
        {"record", {"--period", "0"}},
        {"record", {"--pmc-counters", "19,"}},
        {"report", {"--call-graph", "sideways"}},
        {"report", {"--weight", "bogus"}},
        {"report", {"--source", "-1"}},
        {"report", {"--sort", "bogus"}},
        {"report", {"--time-start", "18446744073709551615"}},
        {"diff", {"--match", "bogus"}},
    };
    for (const auto& [command, value] : cases) {
        error.clear();
        Check(!Build(command, {{value.first, value.second}}, {}, request, &error) &&
                  !error.empty(),
              L"invalid numeric option must be rejected: " +
                  wperf::ToUtf16(std::string(command + " " + value.first)));
    }
}

}  // namespace

int main() {
    TestDefaults();
    TestPublicMappings();
    TestInvalidValues();
    if (g_failures != 0) {
        std::cerr << g_failures << " request test(s) failed\n";
        return 1;
    }
    std::cout << "Request tests: PASS\n";
    return 0;
}
