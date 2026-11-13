#include "off_cpu_report.h"

#include "trace_events.h"

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace wperf {
namespace {

struct ResolvedFrame {
    uint64_t address = 0;
    uint64_t rva = 0;
    std::wstring module;
    std::wstring module_identity;
    std::wstring symbol;
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

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring result;
    for (wchar_t ch : value) {
        switch (ch) {
            case L'"': result += L"\\\""; break;
            case L'\\': result += L"\\\\"; break;
            case L'\n': result += L"\\n"; break;
            case L'\r': result += L"\\r"; break;
            case L'\t': result += L"\\t"; break;
            default:
                if (ch < 0x20) {
                    constexpr wchar_t hex[] = L"0123456789abcdef";
                    result += L"\\u00";
                    result.push_back(hex[(ch >> 4) & 0xf]);
                    result.push_back(hex[ch & 0xf]);
                } else {
                    result.push_back(ch);
                }
                break;
        }
    }
    return result;
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
    if (frame.module_identity.empty()) {
        if (const ImageRecord* image = FindImage(trace, pid, address, timestamp_ns)) {
            frame.module_identity = ModuleIdentityKey(*image);
            frame.rva = address - image->base;
        }
    }
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

void AppendKeyPart(std::wostringstream& key, const std::wstring& value) {
    key << value.size() << L':' << value << L'|';
}

std::wstring RowKey(const OffCpuInterval& interval,
                    const std::vector<ResolvedFrame>& stack,
                    const std::vector<ResolvedFrame>& wakeup_stack) {
    std::wostringstream key;
    key << static_cast<uint32_t>(interval.kind) << L'|' << interval.pid << L'|'
        << interval.process_start_ns << L'|' << interval.tid << L'|'
        << interval.thread_start_ns << L'|' << interval.wait_reason << L'|'
        << (interval.boundary_inferred ? 1 : 0) << L'|' << stack.size() << L'|';
    for (const ResolvedFrame& frame : stack) {
        AppendKeyPart(key, frame.module_identity.empty()
                               ? HexAddress(frame.address)
                               : frame.module_identity);
        key << std::hex << frame.rva << std::dec << L'|';
    }
    key << interval.wakeup_pid << L'|' << interval.wakeup_process_start_ns << L'|'
        << interval.wakeup_tid << L'|' << interval.wakeup_thread_start_ns << L'|'
        << wakeup_stack.size() << L'|';
    for (const ResolvedFrame& frame : wakeup_stack) {
        AppendKeyPart(key, frame.module_identity.empty()
                               ? HexAddress(frame.address)
                               : frame.module_identity);
        key << std::hex << frame.rva << std::dec << L'|';
    }
    return key.str();
}

std::wstring StackText(const std::vector<ResolvedFrame>& frames) {
    std::wstring text;
    for (size_t index = 0; index < frames.size(); ++index) {
        if (index != 0) text += L";";
        text += FrameDisplay(frames[index]);
    }
    return text;
}

void WriteFramesJson(const std::vector<ResolvedFrame>& frames, std::wostream& output) {
    for (size_t index = 0; index < frames.size(); ++index) {
        if (index != 0) output << L',';
        const ResolvedFrame& frame = frames[index];
        output << L"{\"address\":" << frame.address
               << L",\"module\":\"" << JsonEscape(frame.module)
               << L"\",\"module_identity\":\"" << JsonEscape(frame.module_identity)
               << L"\",\"rva\":" << frame.rva
               << L",\"symbol\":\"" << JsonEscape(frame.symbol) << L"\"}";
    }
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
                       const CpuReportOptions& options, std::wostream& output) {
    std::map<std::wstring, OffCpuRow> grouped;
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
        const std::wstring comm =
            ProcessDisplayName(trace, interval.pid, interval.start_ns);
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

        const std::wstring key = RowKey(interval, stack, wakeup_stack);
        auto& row = grouped[key];
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
        row.comm = comm;
        row.stack = std::move(stack);
        row.wakeup_stack = std::move(wakeup_stack);
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
        output << L"[\n";
        bool first = true;
        for (const OffCpuRow& row : rows) {
            const double percent = total_ns == 0 ? 0.0 : 100.0 * row.duration_ns / total_ns;
            if (percent < options.percent_limit) continue;
            if (!first) output << L",\n";
            first = false;
            const uint64_t inferred_ns = row.boundary_inferred ? row.duration_ns : 0;
            const uint64_t confirmed_ns = row.boundary_inferred ? 0 : row.duration_ns;
            output << L"{\"kind\":\"" << OffCpuKindName(row.kind)
                   << L"\",\"pid\":" << row.pid
                   << L",\"process_start_ns\":" << row.process_start_ns
                   << L",\"tid\":" << row.tid
                   << L",\"thread_start_ns\":" << row.thread_start_ns
                   << L",\"comm\":\"" << JsonEscape(row.comm)
                   << L"\",\"duration_ns\":" << row.duration_ns
                   << L",\"confirmed_duration_ns\":" << confirmed_ns
                   << L",\"inferred_duration_ns\":" << inferred_ns
                   << L",\"intervals\":" << row.intervals
                   << L",\"inferred_intervals\":" << (row.boundary_inferred ? row.intervals : 0)
                   << L",\"boundary\":\"" << BoundaryName(row.boundary_inferred)
                   << L"\",\"boundary_inferred\":"
                   << (row.boundary_inferred ? L"true" : L"false")
                   << L",\"overhead\":" << percent
                   << L",\"wait_reason\":" << row.wait_reason
                   << L",\"wait_reason_name\":\"" << JsonEscape(ReasonName(row))
                   << L"\",\"stack_quality\":\"" << StackQualityName(row.quality)
                   << L"\",\"stack_missing\":" << (row.stack_missing ? L"true" : L"false")
                   << L",\"stack_ambiguous\":"
                   << (row.stack_ambiguous ? L"true" : L"false")
                   << L",\"stack\":[";
            for (size_t index = 0; index < row.stack.size(); ++index) {
                if (index != 0) output << L',';
                output << row.stack[index].address;
            }
            output << L"],\"stack_frames\":[";
            WriteFramesJson(row.stack, output);
            output << L"],\"wakeup_pid\":" << row.wakeup_pid
                   << L",\"wakeup_tid\":" << row.wakeup_tid
                   << L",\"wakeup_process_start_ns\":" << row.wakeup_process_start_ns
                   << L",\"wakeup_thread_start_ns\":" << row.wakeup_thread_start_ns
                   << L",\"wakeup_stack\":[";
            for (size_t index = 0; index < row.wakeup_stack.size(); ++index) {
                if (index != 0) output << L',';
                output << row.wakeup_stack[index].address;
            }
            output << L"],\"wakeup_stack_frames\":[";
            WriteFramesJson(row.wakeup_stack, output);
            output << L"]}";
        }
        output << L"\n]\n";
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
