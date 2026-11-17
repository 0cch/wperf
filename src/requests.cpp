#include "requests.h"

#include "common.h"

#include <combaseapi.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace wperf {
namespace {

using Values = std::unordered_map<std::string, std::string>;
using Flags = std::unordered_map<std::string, bool>;

std::wstring W(const Values& values, const char* key) {
    const auto found = values.find(key);
    return found == values.end() ? std::wstring{} : ToUtf16(found->second);
}

bool Has(const Flags& flags, const char* key) {
    const auto found = flags.find(key);
    return found != flags.end() && found->second;
}

bool Bad(std::wstring* error, std::wstring message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

bool ReadU64(const Values& values, const char* key, std::optional<uint64_t>& result,
             std::wstring* error) {
    result.reset();
    const auto found = values.find(key);
    if (found == values.end()) return true;
    if (found->second.empty()) {
        return Bad(error, ToUtf16(key) + L" requires a non-negative integer");
    }
    uint64_t value = 0;
    const char* begin = found->second.data();
    const char* end = begin + found->second.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return Bad(error, ToUtf16(key) + L" requires a non-negative integer");
    }
    result = value;
    return true;
}

bool ReadU32(const Values& values, const char* key, std::optional<uint32_t>& result,
             std::wstring* error) {
    std::optional<uint64_t> parsed;
    if (!ReadU64(values, key, parsed, error)) return false;
    if (!parsed) {
        result.reset();
        return true;
    }
    if (*parsed > UINT32_MAX) {
        return Bad(error, ToUtf16(key) + L" value is out of range");
    }
    result = static_cast<uint32_t>(*parsed);
    return true;
}

bool ReadNumber(const Values& values, const char* key, std::optional<double>& result,
                std::wstring* error) {
    result.reset();
    const auto found = values.find(key);
    if (found == values.end()) return true;
    if (found->second.empty()) {
        return Bad(error, ToUtf16(key) + L" requires a finite number");
    }
    char* end = nullptr;
    const double value = std::strtod(found->second.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(value)) {
        return Bad(error, ToUtf16(key) + L" requires a finite number");
    }
    result = value;
    return true;
}

bool ParseIds(const std::string& text, std::vector<uint32_t>& result) {
    if (text.empty() || text.front() == ',' || text.back() == ',') return false;
    std::istringstream input(text);
    std::string token;
    while (std::getline(input, token, ',')) {
        uint32_t value = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
            value == 0 ||
            std::find(result.begin(), result.end(), value) != result.end()) {
            return false;
        }
        result.push_back(value);
    }
    return !result.empty();
}

std::vector<std::wstring> SplitComma(const std::wstring& text) {
    std::vector<std::wstring> result;
    if (text.empty()) return result;
    std::wistringstream input(text);
    std::wstring token;
    while (std::getline(input, token, L',')) result.push_back(std::move(token));
    return result;
}

std::wstring TextOr(const Values& values, const char* key, const wchar_t* fallback) {
    return values.contains(key) ? W(values, key) : std::wstring(fallback);
}

}  // namespace

bool BuildCommandRequest(const std::string& command, const Values& values,
                         const Flags& flags, const std::vector<std::wstring>& target,
                         CommandRequest& request, std::wstring* error) {
    if (command == "record") {
        RecordRequest value;
        value.elevate = Has(flags, "--elevate");
        value.call_graph = !Has(flags, "--no-call-graph");
        value.output = W(values, "--output");
        value.json = Has(flags, "--json");
        value.event = W(values, "--event");
        value.profile = TextOr(values, "--profile", L"cpu");
        value.stack_cache = Has(flags, "--stack-cache");
        value.lbr = Has(flags, "--lbr");
        value.context_registers = Has(flags, "--context-registers");
        value.session = W(values, "--session");
        value.target = target;
        if (!ReadU32(values, "--freq", value.frequency, error) ||
            !ReadU32(values, "--pid", value.pid, error) ||
            !ReadU64(values, "--duration", value.duration_ms, error) ||
            !ReadU32(values, "--pmc-source", value.pmc_source, error) ||
            !ReadU64(values, "--period", value.period, error)) {
            return false;
        }
        if (values.contains("--pmc-counters") &&
            !ParseIds(values.at("--pmc-counters"), value.pmc_counters)) {
            return Bad(error, L"--pmc-counters requires unique comma-separated positive IDs");
        }
        if (values.contains("--period") && *value.period == 0) {
            return Bad(error, L"--period requires a positive PMC event count");
        }
        if (value.profile != L"cpu" && value.profile != L"cpu-sched" &&
            value.profile != L"sched" && value.profile != L"syscall" &&
            value.profile != L"vm" && value.profile != L"io" &&
            value.profile != L"kernel" && value.profile != L"all" &&
            value.profile != L"pmu" && value.profile != L"dpc" && value.profile != L"isr" &&
            value.profile != L"heap" && value.profile != L"pool" && value.profile != L"driver") {
            return Bad(error, L"unsupported record profile: " + value.profile);
        }
        if (values.contains("--provider")) {
            GUID guid{};
            if (FAILED(IIDFromString(W(values, "--provider").c_str(), &guid))) {
                return Bad(error, L"--provider requires a valid GUID");
            }
            value.provider = guid;
        }
        if (values.contains("--pool-tag")) {
            const auto& tag = values.at("--pool-tag");
            if (tag.size() != 4 || std::any_of(tag.begin(), tag.end(), [](unsigned char ch) { return ch < 32 || ch > 126; }))
                return Bad(error, L"--pool-tag requires exactly four printable ASCII characters");
            uint32_t packed = 0;
            for (size_t index = 0; index < 4; ++index) packed |= static_cast<uint32_t>(static_cast<unsigned char>(tag[index])) << (8 * index);
            value.pool_tag = packed;
        }
        request = std::move(value);
        return true;
    }
    if (command == "report") {
        ReportRequest value;
        value.input = W(values, "--input");
        value.output = W(values, "--output");
        value.comm = W(values, "--comm");
        value.dso = W(values, "--dso");
        value.symbol = W(values, "--symbol");
        value.symbol_path = W(values, "--symbol-path");
        value.archive = W(values, "--archive");
        value.sort_fields = SplitComma(W(values, "--sort"));
        for (const auto& field : value.sort_fields)
            if (field != L"weight" && field != L"samples" && field != L"comm" && field != L"dso" && field != L"symbol")
                return Bad(error, L"unsupported report sort field: " + field);
        if (values.contains("--call-graph")) {
            value.call_graph = W(values, "--call-graph");
            if (value.call_graph->empty()) value.call_graph = L"callee";
            if (*value.call_graph != L"caller" && *value.call_graph != L"callee" &&
                *value.call_graph != L"both") {
                return Bad(error, L"--call-graph requires caller or callee");
            }
        }
        value.metadata = Has(flags, "--metadata");
        value.strict_quality = Has(flags, "--strict-quality");
        value.exact_stacks = Has(flags, "--exact-stacks");
        value.stream = Has(flags, "--stream");
        value.include_children = Has(flags, "--include-children");
        value.scope = TextOr(values, "--scope", L"all");
        value.folded = Has(flags, "--folded");
        value.off_cpu = Has(flags, "--off-cpu");
        value.off_cpu_include_inferred = Has(flags, "--off-cpu-include-inferred");
        value.stdio_output = Has(flags, "--stdio");
        value.quiet = Has(flags, "--quiet");
        value.verbose = Has(flags, "--verbose");
        value.csv = Has(flags, "--csv");
        value.json = Has(flags, "--json");
        value.kind = W(values, "--kind");
        value.weight = TextOr(values, "--weight", L"period");
        value.weight_explicit = values.contains("--weight");
        if (value.weight != L"period" && value.weight != L"samples")
            return Bad(error, L"--weight requires period or samples");
        if (!ReadU32(values, "--source", value.source, error)) return false;
        if (!ReadU32(values, "--pid", value.pid, error) ||
            !ReadU32(values, "--tid", value.tid, error) ||
            !ReadNumber(values, "--percent-limit", value.percent_limit, error) ||
            !ReadU64(values, "--max-lost-events", value.max_lost_events, error) ||
            !ReadU64(values, "--max-missing-stacks", value.max_missing_stacks, error) ||
            !ReadU32(values, "--cpu", value.cpu, error) ||
            !ReadU64(values, "--time-start", value.time_start_ms, error) ||
            !ReadU64(values, "--time-end", value.time_end_ms, error)) {
            return false;
        }
        if (value.percent_limit && (*value.percent_limit < 0.0 || *value.percent_limit > 100.0)) {
            return Bad(error, L"--percent-limit requires a number from 0 to 100");
        }
        if ((value.time_start_ms && *value.time_start_ms > UINT64_MAX / 1000000) ||
            (value.time_end_ms && *value.time_end_ms > UINT64_MAX / 1000000))
            return Bad(error, L"report time offset exceeds nanosecond range");
        request = std::move(value);
        return true;
    }
    if (command == "script") {
        ScriptRequest value;
        value.input = W(values, "--input");
        value.output = W(values, "--output");
        value.fields = TextOr(values, "--fields", L"timestamp,cpu,pid,tid,ip,comm,dso,symbol");
        value.symbol_path = W(values, "--symbol-path");
        value.archive = W(values, "--archive");
        value.csv = Has(flags, "--csv");
        std::optional<uint64_t> limit;
        if (!ReadU32(values, "--pid", value.pid, error) ||
            !ReadU64(values, "--limit", limit, error)) {
            return false;
        }
        value.limit = limit.value_or(0);
        request = std::move(value);
        return true;
    }
    if (command == "trace") {
        TraceRequest value;
        value.live = Has(flags, "--live");
        value.json = Has(flags, "--json");
        value.metadata = Has(flags, "--metadata");
        value.elevate = Has(flags, "--elevate");
        value.event = TextOr(values, "--event", L"all");
        value.input = W(values, "--input");
        std::optional<uint64_t> maximum;
        if (!ReadU32(values, "--pid", value.pid, error) ||
            !ReadU64(values, "--max", maximum, error) ||
            !ReadU64(values, "--duration", value.duration_ms, error)) {
            return false;
        }
        value.max_events = maximum.value_or(0);
        request = std::move(value);
        return true;
    }
    if (command == "sched") {
        request = SchedRequest{W(values, "--input"), Has(flags, "--json"), Has(flags, "--csv")};
        return true;
    }
    if (command == "stat") {
        StatRequest value;
        value.per_thread = Has(flags, "--per-thread");
        value.pmu = Has(flags, "--pmu") || values.contains("--event");
        value.json = Has(flags, "--json");
        value.csv = Has(flags, "--csv");
        value.event = W(values, "--event");
        value.period_explicit = values.contains("--period");
        value.target = target;
        std::optional<uint64_t> duration;
        std::optional<uint64_t> period;
        if (!ReadU32(values, "--pid", value.pid, error) ||
            !ReadU64(values, "--duration", duration, error) ||
            !ReadU64(values, "--interval", value.interval_ms, error) ||
            !ReadU64(values, "--period", period, error)) {
            return false;
        }
        value.duration_ms = duration.value_or(1000);
        value.period = period.value_or(1000000);
        request = std::move(value);
        return true;
    }
    if (command == "top") {
        TopRequest value;
        value.event = TextOr(values, "--event", L"cpu");
        value.sort = W(values, "--sort");
        value.symbol_path = W(values, "--symbol-path");
        value.archive = W(values, "--archive");
        value.elevate = Has(flags, "--elevate");
        std::optional<uint64_t> interval;
        std::optional<uint64_t> iterations;
        std::optional<double> percent;
        if (!ReadU32(values, "--pid", value.pid, error) ||
            !ReadU64(values, "--interval", interval, error) ||
            !ReadU64(values, "--iterations", iterations, error) ||
            !ReadNumber(values, "--percent-limit", percent, error)) {
            return false;
        }
        value.interval_ms = interval.value_or(1000);
        value.iterations = iterations.value_or(0);
        value.percent_limit = percent.value_or(1.0);
        if (value.percent_limit < 0.0 || value.percent_limit > 100.0) {
            return Bad(error, L"--percent-limit requires a number from 0 to 100");
        }
        request = std::move(value);
        return true;
    }
    if (command == "annotate") {
        AnnotateRequest value;
        value.input = W(values, "--input");
        value.symbol_path = W(values, "--symbol-path");
        value.archive = W(values, "--archive");
        value.disasm = Has(flags, "--disasm");
        value.stdio_output = Has(flags, "--stdio");
        value.json = Has(flags, "--json");
        value.csv = Has(flags, "--csv");
        std::optional<uint64_t> limit;
        if (!ReadU32(values, "--pid", value.pid, error) ||
            !ReadU64(values, "--limit", limit, error)) {
            return false;
        }
        value.limit = limit.value_or(50);
        request = std::move(value);
        return true;
    }
    if (command == "diff") {
        DiffRequest value;
        value.first = W(values, "--first");
        value.second = W(values, "--second");
        value.symbol_path = W(values, "--symbol-path");
        value.archive = W(values, "--archive");
        value.dso = W(values, "--dso");
        value.normalization = TextOr(values, "--normalize", L"weight");
        value.matching = TextOr(values, "--match", L"symbol");
        if (value.matching != L"symbol" && value.matching != L"identity")
            return Bad(error, L"--match requires symbol or identity");
        value.calltree = Has(flags, "--calltree");
        value.json = Has(flags, "--json");
        value.csv = Has(flags, "--csv");
        if (!ReadU32(values, "--pid", value.pid, error) ||
            !ReadU32(values, "--first-pid", value.first_pid, error) ||
            !ReadU32(values, "--second-pid", value.second_pid, error)) {
            return false;
        }
        request = std::move(value);
        return true;
    }
    if (command == "archive") {
        ArchiveRequest value;
        value.input = W(values, "--input");
        value.output = W(values, "--output");
        value.symbol_path = W(values, "--symbol-path");
        value.include_trace = Has(flags, "--include-trace");
        std::optional<uint64_t> maximum;
        if (!ReadU32(values, "--pid", value.pid, error) ||
            !ReadU64(values, "--max-size-mb", maximum, error)) {
            return false;
        }
        value.max_size_mb = maximum.value_or(512);
        request = std::move(value);
        return true;
    }
    if (command == "buildid-list") {
        request = BuildIdListRequest{W(values, "--input"), Has(flags, "--json"), Has(flags, "--csv"), Has(flags, "--hash")};
        return true;
    }
    if (command == "list") {
        ListRequest value;
        value.providers = values.contains("@mode") && values.at("@mode") == "providers";
        value.pool_tags = Has(flags, "--pool-tags");
        if (value.providers && value.pool_tags) return Bad(error, L"list providers and --pool-tags are mutually exclusive");
        value.json = Has(flags, "--json");
        std::optional<uint64_t> maximum;
        if (!ReadU64(values, "--max", maximum, error)) return false;
        value.max = maximum.value_or(500);
        request = std::move(value);
        return true;
    }
    if (command == "doctor") {
        DoctorRequest value;
        value.bundle = W(values, "--bundle");
        value.include_dumps = Has(flags, "--include-dumps");
        value.json = Has(flags, "--json");
        request = std::move(value);
        return true;
    }
    if (command == "recover") {
        RecoverRequest value;
        value.session = W(values, "--session");
        value.all_stale = Has(flags, "--all-stale");
        request = std::move(value);
        return true;
    }
    if (command == "test") {
        request = TestRequest{};
        return true;
    }
    return Bad(error, L"unsupported command request");
}

}  // namespace wperf
