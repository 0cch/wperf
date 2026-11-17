#include "cli.h"
#include "common.h"

#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

int g_failures = 0;

struct Invocation {
    bool dispatched = false;
    std::wstring command;
    wperf::CommandRequest request;
};

std::wstring CommandName(const wperf::CommandRequest& request) {
    return std::visit([](const auto& value) -> std::wstring {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, wperf::RecordRequest>) return L"record";
        else if constexpr (std::is_same_v<T, wperf::ReportRequest>) return L"report";
        else if constexpr (std::is_same_v<T, wperf::FlameGraphRequest>) return L"flamegraph";
        else if constexpr (std::is_same_v<T, wperf::ScriptRequest>) return L"script";
        else if constexpr (std::is_same_v<T, wperf::TraceRequest>) return L"trace";
        else if constexpr (std::is_same_v<T, wperf::SchedRequest>) return L"sched";
        else if constexpr (std::is_same_v<T, wperf::StatRequest>) return L"stat";
        else if constexpr (std::is_same_v<T, wperf::TopRequest>) return L"top";
        else if constexpr (std::is_same_v<T, wperf::AnnotateRequest>) return L"annotate";
        else if constexpr (std::is_same_v<T, wperf::DiffRequest>) return L"diff";
        else if constexpr (std::is_same_v<T, wperf::ArchiveRequest>) return L"archive";
        else if constexpr (std::is_same_v<T, wperf::BuildIdListRequest>) return L"buildid-list";
        else if constexpr (std::is_same_v<T, wperf::ListRequest>) return L"list";
        else if constexpr (std::is_same_v<T, wperf::DoctorRequest>) return L"doctor";
        else if constexpr (std::is_same_v<T, wperf::RecoverRequest>) return L"recover";
        else if constexpr (std::is_same_v<T, wperf::TestRequest>) return L"test";
        else return L"unknown";
    }, request);
}

void Check(bool condition, std::wstring_view message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L"\n";
        ++g_failures;
    }
}

int Run(const std::vector<std::wstring>& arguments, Invocation* invocation) {
    *invocation = {};
    return wperf::cli::Run(arguments, [invocation](const wperf::CommandRequest& request) {
        invocation->dispatched = true;
        invocation->command = CommandName(request);
        invocation->request = request;
        return 0;
    });
}

std::wstring Join(const std::vector<std::wstring>& arguments) {
    std::wstring result;
    for (const auto& argument : arguments) {
        if (!result.empty()) result += L' ';
        result += argument;
    }
    return result;
}

struct OptionCase {
    std::vector<std::wstring> arguments;
    std::wstring command;
};

void TestPublicOptions() {
    const std::vector<OptionCase> cases = {
        {{L"record", L"--elevate"}, L"record"},
        {{L"report", L"--weight", L"period"}, L"report"},
        {{L"report", L"--source", L"19"}, L"report"},
        {{L"diff", L"--match", L"symbol"}, L"diff"},
        {{L"buildid-list", L"--hash"}, L"buildid-list"},
        {{L"record", L"-F", L"99"}, L"record"},
        {{L"record", L"--freq", L"99"}, L"record"},
        {{L"record", L"-g"}, L"record"},
        {{L"record", L"--call-graph"}, L"record"},
        {{L"record", L"--no-call-graph"}, L"record"},
        {{L"record", L"-p", L"42"}, L"record"},
        {{L"record", L"--pid", L"42"}, L"record"},
        {{L"record", L"-d", L"1000"}, L"record"},
        {{L"record", L"--duration", L"1000"}, L"record"},
        {{L"record", L"-o", L"trace.etl"}, L"record"},
        {{L"record", L"--json"}, L"record"},
        {{L"record", L"--output", L"trace.etl"}, L"record"},
        {{L"record", L"-e", L"cpu"}, L"record"},
        {{L"record", L"--event", L"cpu"}, L"record"},
        {{L"record", L"--profile", L"cpu"}, L"record"},
        {{L"record", L"--profile", L"pool", L"--pool-tag", L"File"}, L"record"},
        {{L"record", L"--pmc-source", L"19"}, L"record"},
        {{L"record", L"--period", L"1000"}, L"record"},
        {{L"record", L"--pmc-counters", L"19,26"}, L"record"},
        {{L"record", L"--stack-cache"}, L"record"},
        {{L"record", L"-b"}, L"record"},
        {{L"record", L"--lbr"}, L"record"},
        {{L"record", L"--context-registers"}, L"record"},
        {{L"record", L"--provider", L"{00000000-0000-0000-0000-000000000000}"}, L"record"},
        {{L"record", L"--session", L"name"}, L"record"},

        {{L"report", L"-i", L"trace.etl"}, L"report"},
        {{L"report", L"--input", L"trace.etl"}, L"report"},
        {{L"report", L"-o", L"report.txt"}, L"report"},
        {{L"report", L"--output", L"report.txt"}, L"report"},
        {{L"report", L"-p", L"42"}, L"report"},
        {{L"report", L"--pid", L"42"}, L"report"},
        {{L"report", L"-t", L"7"}, L"report"},
        {{L"report", L"--tid", L"7"}, L"report"},
        {{L"report", L"--comm", L"app"}, L"report"},
        {{L"report", L"--dso", L"app.exe"}, L"report"},
        {{L"report", L"--symbol", L"main"}, L"report"},
        {{L"report", L"--symbol-path", L"symbols"}, L"report"},
        {{L"report", L"--archive", L"archive"}, L"report"},
        {{L"report", L"--percent-limit", L"1"}, L"report"},
        {{L"report", L"-s", L"samples"}, L"report"},
        {{L"report", L"--sort", L"samples"}, L"report"},
        {{L"report", L"-g"}, L"report"},
        {{L"report", L"-g", L"caller"}, L"report"},
        {{L"report", L"--call-graph"}, L"report"},
        {{L"report", L"--call-graph", L"caller"}, L"report"},
        {{L"report", L"--metadata"}, L"report"},
        {{L"report", L"--strict-quality"}, L"report"},
        {{L"report", L"--max-lost-events", L"0"}, L"report"},
        {{L"report", L"--max-missing-stacks", L"0"}, L"report"},
        {{L"report", L"--exact-stacks"}, L"report"},
        {{L"report", L"--stream"}, L"report"},
        {{L"report", L"--include-children"}, L"report"},
        {{L"report", L"--cpu", L"0"}, L"report"},
        {{L"report", L"--time-start", L"0"}, L"report"},
        {{L"report", L"--time-end", L"100"}, L"report"},
        {{L"report", L"--scope", L"user"}, L"report"},
        {{L"report", L"--folded"}, L"report"},
        {{L"report", L"--collapsed"}, L"report"},
        {{L"report", L"--flamegraph"}, L"report"},
        {{L"report", L"--title", L"Profile"}, L"report"},
        {{L"report", L"--countname", L"nanoseconds"}, L"report"},
        {{L"report", L"--off-cpu"}, L"report"},
        {{L"report", L"--off-cpu-include-inferred"}, L"report"},
        {{L"report", L"--stdio"}, L"report"},
        {{L"report", L"--quiet"}, L"report"},
        {{L"report", L"--verbose"}, L"report"},
        {{L"report", L"--csv"}, L"report"},
        {{L"report", L"--json"}, L"report"},
        {{L"report", L"--kind", L"cpu"}, L"report"},

        {{L"flamegraph", L"-i", L"stacks.folded"}, L"flamegraph"},
        {{L"flamegraph", L"--input", L"stacks.folded"}, L"flamegraph"},
        {{L"flamegraph", L"-o", L"graph.html"}, L"flamegraph"},
        {{L"flamegraph", L"--output", L"graph.html"}, L"flamegraph"},
        {{L"flamegraph", L"--title", L"Profile"}, L"flamegraph"},
        {{L"flamegraph", L"--countname", L"nanoseconds"}, L"flamegraph"},
        {{L"flamegraph", L"--countname=events"}, L"flamegraph"},

        {{L"script", L"-i", L"trace.etl"}, L"script"},
        {{L"script", L"--input", L"trace.etl"}, L"script"},
        {{L"script", L"-o", L"script.txt"}, L"script"},
        {{L"script", L"--output", L"script.txt"}, L"script"},
        {{L"script", L"-p", L"42"}, L"script"},
        {{L"script", L"--pid", L"42"}, L"script"},
        {{L"script", L"-F", L"timestamp,pid"}, L"script"},
        {{L"script", L"--fields", L"timestamp,pid"}, L"script"},
        {{L"script", L"--symbol-path", L"symbols"}, L"script"},
        {{L"script", L"--archive", L"archive"}, L"script"},
        {{L"script", L"--limit", L"10"}, L"script"},
        {{L"script", L"--csv"}, L"script"},

        {{L"trace", L"-i", L"trace.etl"}, L"trace"},
        {{L"trace", L"--input", L"trace.etl"}, L"trace"},
        {{L"trace", L"--live"}, L"trace"},
        {{L"trace", L"-e", L"sched"}, L"trace"},
        {{L"trace", L"--event", L"sched"}, L"trace"},
        {{L"trace", L"-p", L"42"}, L"trace"},
        {{L"trace", L"--pid", L"42"}, L"trace"},
        {{L"trace", L"--max", L"10"}, L"trace"},
        {{L"trace", L"--duration", L"1000"}, L"trace"},
        {{L"trace", L"--json"}, L"trace"},
        {{L"trace", L"--elevate"}, L"trace"},

        {{L"sched", L"-i", L"trace.etl"}, L"sched"},
        {{L"sched", L"--input", L"trace.etl"}, L"sched"},

        {{L"stat", L"-p", L"42"}, L"stat"},
        {{L"stat", L"--pid", L"42"}, L"stat"},
        {{L"stat", L"--duration", L"1000"}, L"stat"},
        {{L"stat", L"--per-thread"}, L"stat"},
        {{L"stat", L"--pmu"}, L"stat"},
        {{L"stat", L"-e", L"TotalCycles"}, L"stat"},
        {{L"stat", L"--event", L"TotalCycles"}, L"stat"},

        {{L"top", L"-p", L"42"}, L"top"},
        {{L"top", L"--pid", L"42"}, L"top"},
        {{L"top", L"-e", L"cpu"}, L"top"},
        {{L"top", L"--event", L"cpu"}, L"top"},
        {{L"top", L"--interval", L"1000"}, L"top"},
        {{L"top", L"--iterations", L"2"}, L"top"},
        {{L"top", L"--sort", L"samples"}, L"top"},
        {{L"top", L"--symbol-path", L"symbols"}, L"top"},
        {{L"top", L"--archive", L"archive"}, L"top"},
        {{L"top", L"--percent-limit", L"1"}, L"top"},
        {{L"top", L"--elevate"}, L"top"},

        {{L"annotate", L"-i", L"trace.etl"}, L"annotate"},
        {{L"annotate", L"--input", L"trace.etl"}, L"annotate"},
        {{L"annotate", L"--symbol-path", L"symbols"}, L"annotate"},
        {{L"annotate", L"--archive", L"archive"}, L"annotate"},
        {{L"annotate", L"-p", L"42"}, L"annotate"},
        {{L"annotate", L"--pid", L"42"}, L"annotate"},
        {{L"annotate", L"--limit", L"10"}, L"annotate"},
        {{L"annotate", L"--disasm"}, L"annotate"},
        {{L"annotate", L"--stdio"}, L"annotate"},
        {{L"annotate", L"--json"}, L"annotate"},
        {{L"annotate", L"--csv"}, L"annotate"},

        {{L"diff", L"-1", L"before.etl", L"-2", L"after.etl"}, L"diff"},
        {{L"diff", L"--first", L"before.etl", L"--second", L"after.etl"}, L"diff"},
        {{L"diff", L"--symbol-path", L"symbols", L"-1", L"a", L"-2", L"b"}, L"diff"},
        {{L"diff", L"--archive", L"archive", L"-1", L"a", L"-2", L"b"}, L"diff"},
        {{L"diff", L"--pid", L"42", L"-1", L"a", L"-2", L"b"}, L"diff"},
        {{L"diff", L"--dso", L"app.exe", L"-1", L"a", L"-2", L"b"}, L"diff"},
        {{L"diff", L"--calltree", L"-1", L"a", L"-2", L"b"}, L"diff"},
        {{L"diff", L"--weighted", L"-1", L"a", L"-2", L"b"}, L"diff"},
        {{L"diff", L"--json", L"-1", L"a", L"-2", L"b"}, L"diff"},
        {{L"diff", L"--csv", L"-1", L"a", L"-2", L"b"}, L"diff"},

        {{L"archive", L"-i", L"trace.etl", L"-o", L"archive"}, L"archive"},
        {{L"archive", L"--input", L"trace.etl", L"--output", L"archive"}, L"archive"},
        {{L"archive", L"-p", L"42", L"-i", L"trace.etl", L"-o", L"archive"}, L"archive"},
        {{L"archive", L"--pid", L"42", L"-i", L"trace.etl", L"-o", L"archive"}, L"archive"},
        {{L"archive", L"--symbol-path", L"symbols", L"-i", L"a", L"-o", L"b"}, L"archive"},
        {{L"archive", L"--max-size-mb", L"10", L"-i", L"a", L"-o", L"b"}, L"archive"},
        {{L"archive", L"--include-trace", L"-i", L"a", L"-o", L"b"}, L"archive"},

        {{L"buildid-list", L"-i", L"trace.etl"}, L"buildid-list"},
        {{L"buildid-list", L"--input", L"trace.etl"}, L"buildid-list"},
        {{L"list", L"--max", L"10"}, L"list"},
        {{L"list", L"--pool-tags"}, L"list"},
        {{L"doctor", L"--bundle", L"bundle"}, L"doctor"},
        {{L"doctor", L"--include-dumps"}, L"doctor"},
        {{L"recover", L"--session", L"name"}, L"recover"},
        {{L"recover", L"--all-stale"}, L"recover"},
    };

    for (const auto& test : cases) {
        Invocation invocation;
        const int code = Run(test.arguments, &invocation);
        Check(code == 0 && invocation.dispatched && invocation.command == test.command,
              L"public option rejected: " + Join(test.arguments));
    }
}

void TestCommandAliases() {
    const std::vector<std::pair<std::wstring, std::wstring>> aliases = {
        {L"rec", L"record"}, {L"rep", L"report"}, {L"flame", L"flamegraph"}, {L"scr", L"script"},
        {L"ann", L"annotate"}, {L"dif", L"diff"},
    };
    for (const auto& [alias, canonical] : aliases) {
        Invocation invocation;
        const int code = Run({alias}, &invocation);
        Check(code == 0 && invocation.dispatched && invocation.command == canonical,
              L"command alias not resolved: " + alias);
    }
}

void TestCompactOptions() {
    const std::vector<std::vector<std::wstring>> cases = {
        {L"record", L"-F99"},
        {L"report", L"--percent-limit=0"},
        {L"trace", L"-e", L"sched", L"--max=10"},
    };
    for (const auto& arguments : cases) {
        Invocation invocation;
        const int code = Run(arguments, &invocation);
        Check(code == 0 && invocation.dispatched,
              L"compact option form rejected: " + Join(arguments));
    }
}

void TestNormalizedArguments() {
    Invocation invocation;
    Check(Run({L"record", L"-F99"}, &invocation) == 0 &&
              std::get_if<wperf::RecordRequest>(&invocation.request) != nullptr &&
              std::get<wperf::RecordRequest>(invocation.request).frequency == 99,
          L"attached short option value was not parsed");
    Check(Run({L"report", L"--percent-limit=0"}, &invocation) == 0 &&
              std::get_if<wperf::ReportRequest>(&invocation.request) != nullptr &&
              std::get<wperf::ReportRequest>(invocation.request).percent_limit == 0.0,
          L"equals option value was not parsed");
    Check(Run({L"report", L"--symbol-path", L"alpha", L"--symbol-path", L"beta"}, &invocation) == 0 &&
              std::get_if<wperf::ReportRequest>(&invocation.request) != nullptr &&
              std::get<wperf::ReportRequest>(invocation.request).symbol_path == L"alpha;beta",
          L"repeated --symbol-path values were not joined");
    Check(Run({L"diff", L"-1", L"before.etl", L"-2", L"after.etl"}, &invocation) == 0 &&
              std::get_if<wperf::DiffRequest>(&invocation.request) != nullptr &&
              std::get<wperf::DiffRequest>(invocation.request).first == L"before.etl" &&
              std::get<wperf::DiffRequest>(invocation.request).second == L"after.etl",
          L"perf-style diff operands were not parsed");
}
void TestErrorsAndSeparators() {
    Invocation invocation;
    Check(Run({L"does-not-exist"}, &invocation) == 2 && !invocation.dispatched,
          L"unknown command accepted");
    Check(Run({L"report", L"--definitely-invalid"}, &invocation) == 2 &&
              !invocation.dispatched,
          L"unknown command option accepted");
    Check(Run({L"record", L"--definitely-invalid", L"--help"}, &invocation) == 2 &&
              !invocation.dispatched,
          L"help bypassed validation for an unknown option");
    Check(Run({L"record", L"-F"}, &invocation) == 2 && !invocation.dispatched,
          L"missing option value accepted");
    Check(Run({L"report", L"--json=false"}, &invocation) == 2 && !invocation.dispatched,
          L"undocumented flag value override accepted");

    Check(Run({L"record", L"--", L"app.exe", L"--help"}, &invocation) == 0 &&
              invocation.dispatched && invocation.command == L"record" &&
              std::get_if<wperf::RecordRequest>(&invocation.request) != nullptr &&
              std::get<wperf::RecordRequest>(invocation.request).target ==
                  std::vector<std::wstring>({L"app.exe", L"--help"}),
          L"target arguments after -- were not preserved");
}

void TestHelpSurfaces() {
    const auto general = wperf::cli::GeneralHelp();
    for (const wchar_t* command : {L"record", L"report", L"flamegraph", L"script", L"stat", L"top",
                                   L"trace", L"annotate", L"diff", L"sched", L"archive",
                                   L"buildid-list", L"list", L"doctor", L"test", L"recover",
                                   L"capabilities", L"help", L"version"}) {
        Check(general.find(command) != std::wstring::npos,
              std::wstring(L"general help is missing command: ") + command);
    }
    Check(general.find(L"Common perf-style options") != std::wstring::npos,
          L"general help is missing common option guidance");
    Check(general.find(L"Flame graph") != std::wstring::npos &&
              general.find(L"flamegraph.pl") != std::wstring::npos,
          L"general help is missing the flame graph workflows");
    const auto reference = wperf::cli::MarkdownReference();
    Check(reference.starts_with("# wperf Command Reference"),
          L"markdown reference heading is missing");
    Check(reference.find("## Conventions") != std::string::npos &&
              reference.find("## Command index") != std::string::npos,
          L"markdown reference is missing generated navigation");
    Check(reference.find("-1, --first FILE") != std::string::npos,
          L"markdown reference does not render alias names consistently");
    Check(reference.find("flamegraph.pl") != std::string::npos,
          L"markdown reference is missing the external SVG workflow");
    for (const wchar_t* option : {L"--first", L"--second", L"--weighted", L"--json",
                                  L"--csv", L"--off-cpu", L"--flamegraph", L"--countname",
                                  L"--context-registers"}) {
        Check(reference.find(wperf::ToUtf8(option)) != std::string::npos,
              std::wstring(L"markdown reference is missing option: ") + option);
    }
    const auto capabilities = wperf::cli::CapabilitiesJson();
    Check(capabilities.find("\"schema_version\"") != std::string::npos &&
              capabilities.find("\"--json-errors\"") != std::string::npos &&
              capabilities.find("\"record\"") != std::string::npos,
          L"agent capabilities JSON is missing required metadata");
}

}  // namespace

int main() {
    TestPublicOptions();
    TestCommandAliases();
    TestCompactOptions();
    TestNormalizedArguments();
    TestErrorsAndSeparators();
    TestHelpSurfaces();
    if (g_failures != 0) {
        std::cerr << g_failures << " CLI test(s) failed\n";
        return 1;
    }
    std::cout << "CLI tests: PASS\n";
    return 0;
}
