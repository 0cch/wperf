#include "off_cpu_report.h"
#include "trace_events.h"


#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <map>
#include <sstream>

namespace wperf {
namespace {

struct OffCpuRow {
    OffCpuKind kind = OffCpuKind::Blocked;
    uint32_t pid = 0;
    uint32_t tid = 0;
    uint32_t wait_reason = 0;
    uint64_t duration_ns = 0;
    uint32_t intervals = 0;
    StackQuality quality = StackQuality::Missing;
    bool stack_missing = true;
    std::wstring comm;
    std::vector<uint64_t> stack;
    std::vector<uint64_t> wakeup_stack;
};

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring result;
    for (wchar_t ch : value) {
        switch (ch) {
            case L'"': result += L"\\\""; break;
            case L'\\': result += L"\\\\"; break;
            case L'\n': result += L"\\n"; break;
            case L'\r': result += L"\\r"; break;
            case L'\t': result += L"\\t"; break;
            default: result.push_back(ch); break;
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

const wchar_t* ReasonName(const OffCpuRow& row) {
    return row.kind == OffCpuKind::Ready ? L"-" : TraceWaitReasonName(row.wait_reason);
}

std::wstring StackText(const std::vector<std::wstring>& frames) {
    std::wstring text;
    for (size_t index = 0; index < frames.size(); ++index) {
        if (index != 0) {
            text += L";";
        }
        text += frames[index];
    }
    return text;
}

bool Matches(const OffCpuInterval& interval, const CpuReportOptions& options,
             const std::wstring& comm, const ResolvedSymbol& leaf) {
    if (options.pid && interval.pid != *options.pid) return false;
    if (options.tid && interval.tid != *options.tid) return false;
    if (options.cpu && interval.cpu != *options.cpu) return false;
    if (options.start_offset_ns && interval.start_ns < *options.start_offset_ns) return false;
    if (options.end_offset_ns && interval.start_ns > *options.end_offset_ns) return false;
    if (options.exact_stacks_only && interval.stack_quality != StackQuality::Exact) return false;
    const auto lower = [](std::wstring value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        return value;
    };
    if (!options.comm_filter.empty() &&
        lower(comm).find(lower(options.comm_filter)) == std::wstring::npos) return false;
    if (!options.dso_filter.empty() &&
        lower(leaf.module).find(lower(options.dso_filter)) == std::wstring::npos) return false;
    if (!options.symbol_filter.empty() &&
        lower(leaf.symbol).find(lower(options.symbol_filter)) == std::wstring::npos) return false;
    return true;
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
    for (const OffCpuInterval& interval : trace.off_cpu_intervals) {
        const uint64_t address = interval.stack.empty()
                                     ? 0
                                     : interval.stack.View().front();
        const ResolvedSymbol leaf = symbolizer.Resolve(
            trace, interval.pid, address, interval.start_ns);
        const std::wstring comm =
            ProcessDisplayName(trace, interval.pid, interval.start_ns);
        if (!Matches(interval, options, comm, leaf)) {
            continue;
        }
        const std::vector<uint64_t> stack = interval.stack.View();
        std::wostringstream key;
        key << static_cast<uint32_t>(interval.kind) << L'|' << interval.pid << L'|'
            << interval.tid << L'|' << interval.wait_reason;
        for (uint64_t frame : stack) key << L'|' << std::hex << frame;
        auto& row = grouped[key.str()];
        row.kind = interval.kind;
        row.pid = interval.pid;
        row.tid = interval.tid;
        row.wait_reason = interval.wait_reason;
        row.duration_ns += interval.duration_ns;
        ++row.intervals;
        row.quality = interval.stack_quality;
        row.stack_missing = interval.stack_missing;
        row.comm = comm;
        row.stack = stack;
        row.wakeup_stack = interval.wakeup_stack.View();
        total_ns += interval.duration_ns;
    }
    std::vector<OffCpuRow> rows;
    rows.reserve(grouped.size());
    for (auto& [key, row] : grouped) {
        (void)key;
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(),
              [](const OffCpuRow& left, const OffCpuRow& right) {
                  return left.duration_ns > right.duration_ns;
              });

    if (options.folded) {
        for (const OffCpuRow& row : rows) {
            const double percent =
                total_ns == 0 ? 0.0 : 100.0 * row.duration_ns / total_ns;
            if (percent < options.percent_limit) continue;
            std::wstring label = FoldedLabel(row.comm) + L";" +
                                 OffCpuKindName(row.kind);
            if (row.kind == OffCpuKind::Blocked && row.wait_reason != 0) {
                label += L";";
                label += FoldedLabel(TraceWaitReasonName(row.wait_reason));
            }
            if (row.stack.empty()) {
                label += L";[missing-stack]";
            } else {
                for (auto iterator = row.stack.rbegin(); iterator != row.stack.rend();
                     ++iterator) {
                    const ResolvedSymbol symbol = symbolizer.Resolve(
                        trace, row.pid, *iterator, 0);
                    label += L";";
                    label += FoldedLabel(symbol.symbol);
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
            const double percent =
                total_ns == 0 ? 0.0 : 100.0 * row.duration_ns / total_ns;
            if (percent < options.percent_limit) continue;
            if (!first) output << L",\n";
            first = false;
            output << L"{\"kind\":\"" << OffCpuKindName(row.kind)
                   << L"\",\"pid\":" << row.pid << L",\"tid\":" << row.tid
                   << L",\"comm\":\"" << JsonEscape(row.comm)
                   << L"\",\"duration_ns\":" << row.duration_ns
                   << L",\"intervals\":" << row.intervals
                   << L",\"overhead\":" << percent
                   << L",\"wait_reason\":" << row.wait_reason
                   << L",\"wait_reason_name\":\""
                   << ReasonName(row)
                   << L"\",\"stack_quality\":\""
                   << StackQualityName(row.quality)
                   << L"\",\"stack_missing\":" << (row.stack_missing ? L"true" : L"false")
                   << L",\"stack\":[";
            for (size_t index = 0; index < row.stack.size(); ++index) {
                if (index != 0) output << L',';
                output << row.stack[index];
            }
            output << L"],\"wakeup_stack\":[";
            for (size_t index = 0; index < row.wakeup_stack.size(); ++index) {
                if (index != 0) output << L',';
                output << row.wakeup_stack[index];
            }
            output << L"]}";
        }
        output << L"\n]\n";
        return;
    }

    if (options.csv) {
        output << L"kind,pid,tid,comm,duration_ns,intervals,overhead,wait_reason,stack_quality,stack\n";
        for (const OffCpuRow& row : rows) {
            const double percent =
                total_ns == 0 ? 0.0 : 100.0 * row.duration_ns / total_ns;
            if (percent < options.percent_limit) continue;
            output << OffCpuKindName(row.kind) << L',' << row.pid << L','
                   << row.tid << L',' << CsvEscape(row.comm) << L','
                   << row.duration_ns << L',' << row.intervals << L',' << percent
                   << L',' << row.wait_reason << L','
                   << StackQualityName(row.quality) << L",\"";
            for (size_t index = 0; index < row.stack.size(); ++index) {
                if (index != 0) output << L';';
                output << std::hex << row.stack[index] << std::dec;
            }
            output << L"\"\n";
        }
        return;
    }

    output << L"Off-CPU blocked=" << trace.quality.off_cpu_blocked_intervals
           << L" ready=" << trace.quality.off_cpu_ready_intervals
           << L" preemptions=" << trace.quality.off_cpu_preemptions
           << L" unclosed=" << trace.quality.off_cpu_unclosed
           << L" missing_stacks=" << trace.quality.off_cpu_stack_missing
           << L" evicted=" << trace.quality.off_cpu_evicted << L"\n";
    output << std::left << std::setw(8) << L"Kind" << std::setw(8) << L"PID"
           << std::setw(8) << L"TID" << std::setw(22) << L"Command"
           << std::right << std::setw(13) << L"WaitMs" << std::setw(10)
           << L"Percent" << std::setw(9) << L"Reason" << L"  Stack\n";
    for (const OffCpuRow& row : rows) {
        const double percent =
            total_ns == 0 ? 0.0 : 100.0 * row.duration_ns / total_ns;
        if (percent < options.percent_limit) continue;
        std::vector<std::wstring> frames;
        for (auto iterator = row.stack.rbegin(); iterator != row.stack.rend(); ++iterator) {
            frames.push_back(symbolizer.Resolve(trace, row.pid, *iterator, 0).symbol);
        }
        if (frames.empty()) frames.push_back(L"[missing-stack]");
        output << std::left << std::setw(8) << OffCpuKindName(row.kind)
               << std::setw(8) << row.pid << std::setw(8) << row.tid
               << std::setw(22) << Clamp(row.comm, 22) << std::right
               << std::setw(13) << std::fixed << std::setprecision(3)
               << static_cast<double>(row.duration_ns) / 1000000.0
               << std::setw(9) << std::setprecision(2) << percent << L"%  "
               << std::left << std::setw(12) << ReasonName(row) << L"  "
               << StackText(frames) << L"\n";
    }
}

}  // namespace wperf