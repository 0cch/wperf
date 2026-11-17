#pragma once

#include <windows.h>

#include <filesystem>
#include <unordered_map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace wperf {

struct RecordRequest {
    bool elevate = false;
    std::optional<uint32_t> frequency;
    bool call_graph = true;
    std::optional<uint32_t> pid;
    std::optional<uint64_t> duration_ms;
    std::filesystem::path output;
    bool json = false;
    std::wstring event;
    std::wstring profile = L"cpu";
    std::optional<uint32_t> pmc_source;
    std::optional<uint64_t> period;
    std::vector<uint32_t> pmc_counters;
    bool stack_cache = false;
    bool lbr = false;
    bool context_registers = false;
    std::optional<GUID> provider;
    std::optional<uint32_t> pool_tag;
    std::wstring session;
    std::vector<std::wstring> target;
};

struct ReportRequest {
    std::filesystem::path input;
    std::filesystem::path output;
    std::optional<uint32_t> pid;
    std::optional<uint32_t> tid;
    std::wstring comm, dso, symbol, symbol_path;
    std::filesystem::path archive;
    std::optional<double> percent_limit;
    std::vector<std::wstring> sort_fields;
    std::optional<std::wstring> call_graph;
    bool metadata = false, strict_quality = false;
    std::optional<uint64_t> max_lost_events, max_missing_stacks;
    bool exact_stacks = false, stream = false, include_children = false;
    std::optional<uint32_t> cpu;
    std::optional<uint64_t> time_start_ms, time_end_ms;
    std::wstring scope = L"all";
    bool folded = false, off_cpu = false, off_cpu_include_inferred = false;
    bool stdio_output = false, quiet = false, verbose = false, csv = false, json = false;
    std::wstring kind;
    std::wstring weight = L"period";
    bool weight_explicit = false;
    std::optional<uint32_t> source;
};

struct ScriptRequest {
    std::filesystem::path input, output;
    std::optional<uint32_t> pid;
    std::wstring fields = L"timestamp,cpu,pid,tid,ip,comm,dso,symbol";
    std::wstring symbol_path;
    std::filesystem::path archive;
    uint64_t limit = 0;
    bool csv = false;
};

struct TraceRequest {
    std::filesystem::path input;
    bool live = false, json = false, metadata = false, elevate = false;
    std::wstring event = L"all";
    std::optional<uint32_t> pid;
    uint64_t max_events = 0;
    std::optional<uint64_t> duration_ms;
};

struct SchedRequest {
    std::filesystem::path input;
    bool json = false, csv = false;
};

struct StatRequest {
    std::optional<uint32_t> pid;
    uint64_t duration_ms = 1000;
    std::optional<uint64_t> interval_ms;
    bool per_thread = false, pmu = false, json = false, csv = false;
    std::wstring event;
    uint64_t period = 1000000;
    bool period_explicit = false;
    std::vector<std::wstring> target;
};

struct TopRequest {
    std::optional<uint32_t> pid;
    std::wstring event = L"cpu";
    uint64_t interval_ms = 1000;
    uint64_t iterations = 0;
    std::wstring sort;
    std::wstring symbol_path;
    std::filesystem::path archive;
    double percent_limit = 1.0;
    bool elevate = false;
};

struct AnnotateRequest {
    std::filesystem::path input;
    std::wstring symbol_path;
    std::filesystem::path archive;
    std::optional<uint32_t> pid;
    uint64_t limit = 50;
    bool disasm = false, stdio_output = false, json = false, csv = false;
};

struct DiffRequest {
    std::filesystem::path first, second;
    std::wstring symbol_path;
    std::filesystem::path archive;
    std::optional<uint32_t> pid, first_pid, second_pid;
    std::wstring dso;
    std::wstring normalization = L"weight";
    std::wstring matching = L"symbol";
    bool calltree = false, json = false, csv = false;
};

struct ArchiveRequest {
    std::filesystem::path input, output;
    std::optional<uint32_t> pid;
    std::wstring symbol_path;
    uint64_t max_size_mb = 512;
    bool include_trace = false;
};

struct BuildIdListRequest {
    std::filesystem::path input;
    bool json = false, csv = false;
    bool hash = false;
};

struct ListRequest {
    bool providers = false;
    bool pool_tags = false;
    uint64_t max = 500;
    bool json = false;
};

struct DoctorRequest {
    std::filesystem::path bundle;
    bool include_dumps = false, json = false;
};

struct RecoverRequest {
    std::wstring session;
    bool all_stale = false;
};

struct TestRequest {};

using OptionValues = std::unordered_map<std::string, std::string>;
using OptionFlags = std::unordered_map<std::string, bool>;
using CommandRequest = std::variant<
    RecordRequest, ReportRequest, ScriptRequest, TraceRequest, SchedRequest,
    StatRequest, TopRequest, AnnotateRequest, DiffRequest, ArchiveRequest,
    BuildIdListRequest, ListRequest, DoctorRequest, RecoverRequest, TestRequest>;

bool BuildCommandRequest(const std::string& command, const OptionValues& values,
                         const OptionFlags& flags, const std::vector<std::wstring>& target,
                         CommandRequest& request, std::wstring* error = nullptr);

}  // namespace wperf
