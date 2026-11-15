#include "off_cpu_report.h"

#include "common.h"
#include "trace_events.h"
#include "json_writer.h"

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>

namespace wperf {
namespace {

struct ResolvedFrame {
    uint64_t address = 0;
    uint64_t rva = 0;
    std::wstring module;
    std::wstring module_identity;
    std::wstring symbol;
    std::vector<InlineFrame> inline_frames;
};

struct FrameKey {
    uint32_t module_id = 0;
    uint64_t rva = 0;
    bool operator==(const FrameKey&) const = default;
};

struct OffCpuKey {
    uint32_t kind = 0;
    uint32_t pid = 0;
    uint64_t process_start_ns = 0;
    uint32_t tid = 0;
    uint64_t thread_start_ns = 0;
    uint32_t wait_reason = 0;
    bool boundary_inferred = false;
    std::vector<FrameKey> stack;
    uint32_t wakeup_pid = UINT32_MAX;
    uint64_t wakeup_process_start_ns = 0;
    uint32_t wakeup_tid = 0;
    uint64_t wakeup_thread_start_ns = 0;
    std::vector<FrameKey> wakeup_stack;
    bool operator==(const OffCpuKey&) const = default;
};

size_t MixHash(size_t hash, uint64_t value) {
    constexpr uint64_t salt = 0x9e3779b97f4a7c15ull;
    hash ^= static_cast<size_t>(value + salt + (hash << 6) + (hash >> 2));
    return hash;
}

struct OffCpuKeyHash {
    size_t operator()(const OffCpuKey& key) const noexcept {
        size_t hash = 0;
        hash = MixHash(hash, key.kind);
        hash = MixHash(hash, key.pid);
        hash = MixHash(hash, key.process_start_ns);
        hash = MixHash(hash, key.tid);
        hash = MixHash(hash, key.thread_start_ns);
        hash = MixHash(hash, key.wait_reason);
        hash = MixHash(hash, key.boundary_inferred ? 1 : 0);
        hash = MixHash(hash, key.stack.size());
        for (const FrameKey& frame : key.stack) {
            hash = MixHash(hash, frame.module_id);
            hash = MixHash(hash, frame.rva);
        }
        hash = MixHash(hash, key.wakeup_pid);
        hash = MixHash(hash, key.wakeup_process_start_ns);
        hash = MixHash(hash, key.wakeup_tid);
        hash = MixHash(hash, key.wakeup_thread_start_ns);
        hash = MixHash(hash, key.wakeup_stack.size());
        for (const FrameKey& frame : key.wakeup_stack) {
            hash = MixHash(hash, frame.module_id);
            hash = MixHash(hash, frame.rva);
        }
        return hash;
    }
};

struct OffCpuRow {
    OffCpuKind kind = OffCpuKind::Blocked;
    uint32_t pid = 0;
    uint32_t tid = 0;
    uint64_t process_start_ns = 0;
    uint64_t thread_start_ns = 0;
    uint32_t wakeup_pid = UINT32_MAX;
    uint32_t wakeup_tid = 0;
    uint64_t wakeup_process_start_ns = 0;
    uint64_t wakeup_thread_start_ns = 0;
    uint32_t wait_reason = 0;
    uint64_t duration_ns = 0;
    uint32_t intervals = 0;
    StackQuality quality = StackQuality::Exact;
    bool stack_missing = false;
    bool stack_ambiguous = false;
    bool boundary_inferred = false;
    std::wstring comm;
    std::vector<ResolvedFrame> stack;
    std::vector<ResolvedFrame> wakeup_stack;
};

uint64_t SaturatingAdd(uint64_t left, uint64_t right) {
    return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

bool IsKernelAddress(uint64_t address, uint32_t pointer_size) {
    if (pointer_size == 4) {
        return address >= 0x80000000ull && address <= UINT32_MAX;
    }
    return address >= 0xffff800000000000ull;
}

StackQuality WorseQuality(StackQuality left, StackQuality right) {
    if (left == StackQuality::Missing || right == StackQuality::Missing)
        return StackQuality::Missing;
    if (left == StackQuality::Approximate || right == StackQuality::Approximate)
        return StackQuality::Approximate;
    return StackQuality::Exact;
}

std::wstring FoldedLabel(std::wstring value) {
    for (wchar_t& ch : value) {
        if (ch == L';' || ch == L'\n' || ch == L'\r') {
            ch = L'_';
        }
    }
    return value;
}

std::wstring Clamp(std::wstring value, size_t width) {
    if (width < 4 || value.size() <= width) {
        return value;
    }
    value.resize(width - 3);
    value += L"...";
    return value;
}

std::wstring CsvEscape(const std::wstring& value) {
    std::wstring result = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') result += L'"';
        result += ch;
    }
    return result + L'"';
}

std::wstring HexAddress(uint64_t address) {
    std::wostringstream output;
    output << L"0x" << std::hex << address;
    return output.str();
}

const wchar_t* ReasonName(const OffCpuRow& row) {
    return row.kind == OffCpuKind::Ready ? L"-" : TraceWaitReasonName(row.wait_reason);
}

const wchar_t* BoundaryName(bool inferred) {
    return inferred ? L"truncated" : L"confirmed";
}

bool IsDescendant(const TraceData& trace, uint32_t pid, uint32_t ancestor,
                  uint64_t timestamp_ns) {
    if (pid == ancestor) return true;
    const ProcessRecord* process = FindProcess(trace, pid, timestamp_ns);
    std::set<uint32_t> seen;
    while (process != nullptr && seen.insert(process->pid).second) {
        if (process->parent_pid == ancestor) return true;
        process = FindProcess(trace, process->parent_pid, process->start_ns);
    }
    return false;
}

std::optional<std::pair<uint64_t, uint64_t>> SelectedBounds(
    const TraceData& trace, const OffCpuInterval& interval,
    const CpuReportOptions& options) {
    uint64_t start = interval.start_ns;
    uint64_t end = interval.end_ns;
    const uint64_t origin = trace.start_ns == UINT64_MAX ? 0 : trace.start_ns;
    if (options.start_offset_ns) {
        const uint64_t selected = SaturatingAdd(origin, *options.start_offset_ns);
        start = std::max(start, selected);
    }
    if (options.end_offset_ns) {
        const uint64_t selected = SaturatingAdd(origin, *options.end_offset_ns);
        end = std::min(end, selected);
    }
    if (end <= start) return std::nullopt;
    return std::make_pair(start, end);
}

ResolvedFrame DescribeFrame(const TraceData& trace, Symbolizer& symbolizer,
                            uint32_t pid, uint64_t address, uint64_t timestamp_ns) {
    ResolvedFrame frame;
    frame.address = address;
    if (address == 0) {
        frame.symbol = L"[missing-stack]";
        return frame;
    }
    const ResolvedSymbol resolved =
        symbolizer.Resolve(trace, pid, address, timestamp_ns);
    frame.module = resolved.module;
    frame.module_identity = resolved.module_identity;
    frame.rva = resolved.rva;
    frame.symbol = resolved.symbol;
    frame.inline_frames = resolved.inline_frames;
    if (frame.module.empty() && !frame.module_identity.empty()) {
        const auto separator = frame.module_identity.find(L'|');
        frame.module = frame.module_identity.substr(0, separator);
    }
    if (frame.symbol.empty()) {
        frame.symbol = HexAddress(address);
    }
    if (frame.module.empty()) {
        frame.module = L"[unknown]";
    }
    return frame;
}

std::wstring FrameDisplay(const ResolvedFrame& frame) {
    const std::wstring address = HexAddress(frame.address);
    if (frame.symbol.empty()) return frame.module + L"!" + address;
    if (frame.module.empty() || frame.module == L"[unknown]") return frame.symbol;
    if (frame.symbol == address || frame.symbol == L"[unknown]") {
        return frame.module + L"!" + address;
    }
    return frame.module + L"!" + frame.symbol;
}

bool Matches(const TraceData& trace, const OffCpuInterval& interval,
             const CpuReportOptions& options, const std::wstring& comm,
             const ResolvedFrame& leaf) {
    if (options.pid) {
        if (options.include_children) {
            if (!IsDescendant(trace, interval.pid, *options.pid, interval.start_ns)) {
                return false;
            }
        } else if (interval.pid != *options.pid) {
            return false;
        }
    }
    if (options.tid && interval.tid != *options.tid) return false;
    if (options.cpu && interval.cpu != *options.cpu) return false;
    if (options.exact_stacks_only && interval.stack_quality != StackQuality::Exact) {
        return false;
    }
    const bool kernel = IsKernelAddress(leaf.address, trace.pointer_size);
    if (options.scope == CpuReportOptions::Scope::User && kernel) return false;
    if (options.scope == CpuReportOptions::Scope::Kernel && !kernel) return false;
    const auto lower = [](std::wstring value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        return value;
    };
    if (!options.comm_filter.empty() &&
        lower(comm).find(lower(options.comm_filter)) == std::wstring::npos) {
        return false;
    }
    if (!options.dso_filter.empty() &&
        lower(leaf.module).find(lower(options.dso_filter)) == std::wstring::npos) {
        return false;
    }
    if (!options.symbol_filter.empty() &&
        lower(leaf.symbol).find(lower(options.symbol_filter)) == std::wstring::npos) {
        return false;
    }
    return true;
}

uint32_t InternModuleId(const std::wstring& identity,
                                  std::unordered_map<std::wstring, uint32_t>& ids,
                                  uint32_t& next_id) {
    if (identity.empty()) return 0;
    const auto [entry, inserted] = ids.try_emplace(identity, next_id);
    if (inserted) ++next_id;
    return entry->second;
}

FrameKey MakeFrameKey(const ResolvedFrame& frame,
                      std::unordered_map<std::wstring, uint32_t>& module_ids,
                      uint32_t& next_module_id) {
    if (frame.module_identity.empty()) return FrameKey{0, frame.address};
    return FrameKey{InternModuleId(frame.module_identity, module_ids, next_module_id),
                    frame.rva};
}

std::vector<FrameKey> MakeFrameKeys(
    const std::vector<ResolvedFrame>& frames,
    std::unordered_map<std::wstring, uint32_t>& module_ids,
    uint32_t& next_module_id) {
    std::vector<FrameKey> keys;
    keys.reserve(frames.size());
    for (const ResolvedFrame& frame : frames) {
        keys.push_back(MakeFrameKey(frame, module_ids, next_module_id));
    }
    return keys;
}

OffCpuKey RowKey(const OffCpuInterval& interval,
                 const std::vector<ResolvedFrame>& stack,
                 const std::vector<ResolvedFrame>& wakeup_stack,
                 std::unordered_map<std::wstring, uint32_t>& module_ids,
                 uint32_t& next_module_id) {
    OffCpuKey key;
    key.kind = static_cast<uint32_t>(interval.kind);
    key.pid = interval.pid;
    key.process_start_ns = interval.process_start_ns;
    key.tid = interval.tid;
    key.thread_start_ns = interval.thread_start_ns;
    key.wait_reason = interval.wait_reason;
    key.boundary_inferred = interval.boundary_inferred;
    key.stack = MakeFrameKeys(stack, module_ids, next_module_id);
    key.wakeup_pid = interval.wakeup_pid;
    key.wakeup_process_start_ns = interval.wakeup_process_start_ns;
    key.wakeup_tid = interval.wakeup_tid;
    key.wakeup_thread_start_ns = interval.wakeup_thread_start_ns;
    key.wakeup_stack = MakeFrameKeys(wakeup_stack, module_ids, next_module_id);
    return key;
}

std::wstring StackText(const std::vector<ResolvedFrame>& frames) {
    std::wstring text;
    for (size_t index = 0; index < frames.size(); ++index) {
        if (index != 0) text += L";";
        text += FrameDisplay(frames[index]);
    }
    return text;
}

nlohmann::ordered_json FramesJson(const std::vector<ResolvedFrame>& frames) {
    nlohmann::ordered_json result = nlohmann::ordered_json::array();
    for (const auto& frame : frames) {
        nlohmann::ordered_json item;
        item["address"] = frame.address;
        item["module"] = ToUtf8(frame.module);
        item["module_identity"] = ToUtf8(frame.module_identity);
        item["rva"] = frame.rva;
        item["symbol"] = ToUtf8(frame.symbol);
        nlohmann::ordered_json inlines = nlohmann::ordered_json::array();
        for (const auto& inline_frame : frame.inline_frames) {
            nlohmann::ordered_json inline_item;
            inline_item["symbol"] = ToUtf8(inline_frame.symbol);
            inline_item["source_file"] = ToUtf8(inline_frame.source_file);
            inline_item["line"] = inline_frame.line;
            inlines.push_back(std::move(inline_item));
        }
        item["inline_frames"] = std::move(inlines);
        result.push_back(std::move(item));
    }
    return result;
}
}  // namespace

const wchar_t* OffCpuKindName(OffCpuKind kind) {
    return kind == OffCpuKind::Blocked ? L"blocked" : L"ready";
}

const wchar_t* StackQualityName(StackQuality quality) {
    switch (quality) {
        case StackQuality::Exact: return L"exact";
        case StackQuality::Approximate: return L"approximate";
        case StackQuality::Missing: return L"missing";
    }
    return L"missing";
}

void WriteOffCpuReport(const TraceData& trace, Symbolizer& symbolizer,
                       const CpuReportOptions& options, std::wostream& output,
                       JsonStreamWriter* json_writer) {
    std::unordered_map<OffCpuKey, OffCpuRow, OffCpuKeyHash> grouped;
    grouped.reserve(trace.off_cpu_intervals.size());
    std::unordered_map<std::wstring, uint32_t> module_ids;
    module_ids.reserve(trace.images.size());
    uint32_t next_module_id = 1;
    ProcessNameIndex process_names(trace);
    uint64_t total_ns = 0;
    uint64_t excluded_inferred = 0;
    for (const OffCpuInterval& interval : trace.off_cpu_intervals) {
        if (interval.boundary_inferred && !options.off_cpu_include_inferred) {
            ++excluded_inferred;
            continue;
        }
        const auto bounds = SelectedBounds(trace, interval, options);
        if (!bounds) continue;
        const uint64_t duration_ns = bounds->second - bounds->first;
        const std::vector<uint64_t> raw_stack = interval.stack.View();
        std::vector<ResolvedFrame> stack;
        stack.reserve(raw_stack.size());
        const uint64_t stack_time = interval.stack_timestamp_ns == 0
                                        ? interval.start_ns : interval.stack_timestamp_ns;
        for (uint64_t address : raw_stack) {
            stack.push_back(DescribeFrame(trace, symbolizer, interval.pid, address,
                                          stack_time));
        }
        const ResolvedFrame leaf =
            stack.empty()
                ? DescribeFrame(trace, symbolizer, interval.pid, 0, interval.start_ns)
                : stack.front();
        const std::wstring comm = process_names.Name(interval.pid, interval.start_ns);
        if (!Matches(trace, interval, options, comm, leaf)) continue;

        std::vector<ResolvedFrame> wakeup_stack;
        const std::vector<uint64_t> raw_wakeup = interval.wakeup_stack.View();
        wakeup_stack.reserve(raw_wakeup.size());
        const uint64_t wakeup_time = interval.wakeup_timestamp_ns == 0
                                         ? interval.start_ns
                                         : interval.wakeup_timestamp_ns;
        for (uint64_t address : raw_wakeup) {
            wakeup_stack.push_back(
                DescribeFrame(trace, symbolizer, interval.wakeup_pid, address, wakeup_time));
        }

        OffCpuKey key = RowKey(interval, stack, wakeup_stack, module_ids,
                                 next_module_id);
        auto [entry, inserted] = grouped.try_emplace(std::move(key));
        auto& row = entry->second;
        row.kind = interval.kind;
        row.pid = interval.pid;
        row.tid = interval.tid;
        row.process_start_ns = interval.process_start_ns;
        row.thread_start_ns = interval.thread_start_ns;
        row.wakeup_pid = interval.wakeup_pid;
        row.wakeup_tid = interval.wakeup_tid;
        row.wakeup_process_start_ns = interval.wakeup_process_start_ns;
        row.wakeup_thread_start_ns = interval.wakeup_thread_start_ns;
        row.wait_reason = interval.wait_reason;
        row.duration_ns += duration_ns;
        ++row.intervals;
        row.quality = row.intervals == 1
                          ? interval.stack_quality
                          : WorseQuality(row.quality, interval.stack_quality);
        row.stack_missing = row.stack_missing || interval.stack_missing;
        row.stack_ambiguous = row.stack_ambiguous || interval.stack_ambiguous;
        row.boundary_inferred = interval.boundary_inferred;
        if (inserted) {
            row.comm = comm;
            row.stack = std::move(stack);
            row.wakeup_stack = std::move(wakeup_stack);
        }
        total_ns += duration_ns;
    }

    std::vector<OffCpuRow> rows;
    rows.reserve(grouped.size());
    for (auto& [key, row] : grouped) {
        (void)key;
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const OffCpuRow& left, const OffCpuRow& right) {
        if (left.duration_ns != right.duration_ns) return left.duration_ns > right.duration_ns;
        if (left.pid != right.pid) return left.pid < right.pid;
        return left.tid < right.tid;
    });

    if (options.folded) {
        for (const OffCpuRow& row : rows) {
            const double percent = total_ns == 0 ? 0.0 : 100.0 * row.duration_ns / total_ns;
            if (percent < options.percent_limit) continue;
            std::wstring label = FoldedLabel(row.comm) + L";" + OffCpuKindName(row.kind);
            if (row.kind == OffCpuKind::Blocked && row.wait_reason != 0) {
                label += L";";
                label += FoldedLabel(TraceWaitReasonName(row.wait_reason));
            }
            if (row.boundary_inferred) label += L";[truncated-boundary]";
            if (row.stack.empty()) {
                label += row.stack_ambiguous ? L";[ambiguous-stack]" : L";[missing-stack]";
            } else {
                for (auto iterator = row.stack.rbegin(); iterator != row.stack.rend(); ++iterator) {
                    label += L";";
                    label += FoldedLabel(FrameDisplay(*iterator));
                }
            }
            output << label << L' ' << row.duration_ns << L'\n';
        }
        return;
    }

    if (options.json) {
        JsonStreamWriter local_writer(output);
        JsonStreamWriter& writer = json_writer == nullptr ? local_writer : *json_writer;
        writer.BeginArray();
        for (const OffCpuRow& row : rows) {
            const double percent = total_ns == 0 ? 0.0 : 100.0 * row.duration_ns / total_ns;
            if (percent < options.percent_limit) continue;
            const uint64_t inferred_ns = row.boundary_inferred ? row.duration_ns : 0;
            const uint64_t confirmed_ns = row.boundary_inferred ? 0 : row.duration_ns;
            nlohmann::ordered_json item;
            item["kind"] = ToUtf8(OffCpuKindName(row.kind));
            item["pid"] = row.pid;
            item["process_start_ns"] = row.process_start_ns;
            item["tid"] = row.tid;
            item["thread_start_ns"] = row.thread_start_ns;
            item["comm"] = ToUtf8(row.comm);
            item["duration_ns"] = row.duration_ns;
            item["confirmed_duration_ns"] = confirmed_ns;
            item["inferred_duration_ns"] = inferred_ns;
            item["intervals"] = row.intervals;
            item["inferred_intervals"] = row.boundary_inferred ? row.intervals : 0;
            item["boundary"] = ToUtf8(BoundaryName(row.boundary_inferred));
            item["boundary_inferred"] = row.boundary_inferred;
            item["overhead"] = percent;
            item["wait_reason"] = row.wait_reason;
            item["wait_reason_name"] = ToUtf8(ReasonName(row));
            item["stack_quality"] = ToUtf8(StackQualityName(row.quality));
            item["stack_missing"] = row.stack_missing;
            item["stack_ambiguous"] = row.stack_ambiguous;
            nlohmann::ordered_json stack = nlohmann::ordered_json::array();
            nlohmann::ordered_json wakeup = nlohmann::ordered_json::array();
            for (const auto& frame : row.stack) stack.push_back(frame.address);
            for (const auto& frame : row.wakeup_stack) wakeup.push_back(frame.address);
            item["stack"] = std::move(stack);
            item["stack_frames"] = FramesJson(row.stack);
            item["wakeup_pid"] = row.wakeup_pid;
            item["wakeup_tid"] = row.wakeup_tid;
            item["wakeup_process_start_ns"] = row.wakeup_process_start_ns;
            item["wakeup_thread_start_ns"] = row.wakeup_thread_start_ns;
            item["wakeup_stack"] = std::move(wakeup);
            item["wakeup_stack_frames"] = FramesJson(row.wakeup_stack);
            writer.Item(item);
        }
        writer.EndArray();
        output << L"\n";
        return;
    }
    if (options.csv) {
        output << L"kind,pid,process_start_ns,tid,thread_start_ns,comm,duration_ns,"
                  L"confirmed_duration_ns,inferred_duration_ns,intervals,"
                  L"inferred_intervals,boundary,overhead,wait_reason,stack_quality,"
                  L"stack_missing,stack_ambiguous,stack,wakeup_pid,wakeup_tid,"
                  L"wakeup_process_start_ns,wakeup_thread_start_ns,wakeup_stack\n";
        for (const OffCpuRow& row : rows) {
            const double percent = total_ns == 0 ? 0.0 : 100.0 * row.duration_ns / total_ns;
            if (percent < options.percent_limit) continue;
            const uint64_t inferred_ns = row.boundary_inferred ? row.duration_ns : 0;
            const uint64_t confirmed_ns = row.boundary_inferred ? 0 : row.duration_ns;
            output << OffCpuKindName(row.kind) << L',' << row.pid << L','
                   << row.process_start_ns << L',' << row.tid << L','
                   << row.thread_start_ns << L',' << CsvEscape(row.comm) << L','
                   << row.duration_ns << L',' << confirmed_ns << L',' << inferred_ns
                   << L',' << row.intervals << L','
                   << (row.boundary_inferred ? row.intervals : 0) << L','
                   << BoundaryName(row.boundary_inferred) << L',' << percent << L','
                   << row.wait_reason << L',' << StackQualityName(row.quality) << L','
                   << (row.stack_missing ? L"true" : L"false") << L','
                   << (row.stack_ambiguous ? L"true" : L"false") << L','
                   << CsvEscape(StackText(row.stack)) << L',' << row.wakeup_pid
                   << L',' << row.wakeup_tid << L',' << row.wakeup_process_start_ns
                   << L',' << row.wakeup_thread_start_ns << L','
                   << CsvEscape(StackText(row.wakeup_stack)) << L'\n';
        }
        return;
    }

    output << L"Off-CPU blocked=" << trace.quality.off_cpu_blocked_intervals
           << L" ready=" << trace.quality.off_cpu_ready_intervals
           << L" preemptions=" << trace.quality.off_cpu_preemptions
           << L" unclosed=" << trace.quality.off_cpu_unclosed
           << L" inferred=" << trace.quality.off_cpu_inferred_intervals
           << L" missing_stacks=" << trace.quality.off_cpu_stack_missing
           << L" ambiguous_stacks=" << trace.quality.off_cpu_stack_ambiguous
           << L" duplicate_ready=" << trace.quality.off_cpu_duplicate_ready
           << L" evicted=" << trace.quality.off_cpu_evicted << L"\n";
    if (excluded_inferred != 0) {
        output << L"Off-CPU inferred boundary intervals excluded=" << excluded_inferred
               << L" (use --off-cpu-include-inferred to include them)\n";
    }
    output << std::left << std::setw(8) << L"Kind" << std::setw(8) << L"PID"
           << std::setw(8) << L"TID" << std::setw(22) << L"Command"
           << std::right << std::setw(13) << L"WaitMs" << std::setw(10)
           << L"Percent" << std::setw(9) << L"Reason" << std::setw(12)
           << L"Boundary" << std::setw(18) << L"Waker PID/TID" << L"  Stack\n";
    for (const OffCpuRow& row : rows) {
        const double percent = total_ns == 0 ? 0.0 : 100.0 * row.duration_ns / total_ns;
        if (percent < options.percent_limit) continue;
        std::vector<std::wstring> frames;
        for (auto iterator = row.stack.rbegin(); iterator != row.stack.rend(); ++iterator) {
            frames.push_back(FrameDisplay(*iterator));
        }
        if (frames.empty()) {
            frames.push_back(row.stack_ambiguous ? L"[ambiguous-stack]" : L"[missing-stack]");
        }
        output << std::left << std::setw(8) << OffCpuKindName(row.kind)
               << std::setw(8) << row.pid << std::setw(8) << row.tid
               << std::setw(22) << Clamp(row.comm, 22) << std::right
               << std::setw(13) << std::fixed << std::setprecision(3)
               << static_cast<double>(row.duration_ns) / 1000000.0
               << std::setw(9) << std::setprecision(2) << percent << L"%  "
               << std::left << std::setw(12) << ReasonName(row)
               << std::setw(12) << BoundaryName(row.boundary_inferred)
               << std::setw(18) << (row.wakeup_tid == 0 ? L"-"
                     : std::to_wstring(row.wakeup_pid) + L"/" + std::to_wstring(row.wakeup_tid))
               << L"  "
               << StackText(row.stack) << L"\n";
    }
}

}  // namespace wperf
