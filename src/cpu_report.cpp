#include "cpu_report.h"

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace wperf {
namespace {

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

bool ContainsInsensitive(const std::wstring& value, const std::wstring& filter) {
    if (filter.empty()) {
        return true;
    }
    return ToLower(value).find(ToLower(filter)) != std::wstring::npos;
}

bool Matches(const SampleRecord& sample,
             const CpuReportOptions& options, const std::wstring& comm,
             const std::wstring& dso, const std::wstring& symbol) {
    if (options.pid.has_value() && sample.pid != *options.pid) {
        return false;
    }
    if (options.tid.has_value() && sample.tid != *options.tid) {
        return false;
    }
    if (!ContainsInsensitive(comm, options.comm_filter) ||
        !ContainsInsensitive(dso, options.dso_filter) ||
        !ContainsInsensitive(symbol, options.symbol_filter)) {
        return false;
    }
    return true;
}

struct Row {
    std::wstring comm;
    std::wstring dso;
    std::wstring symbol;
    uint64_t samples = 0;
};

struct TreeNode {
    std::wstring name;
    uint64_t self = 0;
    uint64_t inclusive = 0;
    std::map<std::wstring, TreeNode> children;
};

std::vector<uint64_t> StackForSample(const SampleRecord& sample) {
    if (!sample.stack.empty()) {
        return sample.stack;
    }
    return {sample.instruction_pointer};
}

std::wstring FoldedLabel(std::wstring value) {
    for (wchar_t& ch : value) {
        if (ch == L';' || ch == L'\n' || ch == L'\r') {
            ch = L'_';
        }
    }
    return value;
}

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring result;
    for (wchar_t ch : value) {
        switch (ch) {
            case L'"':
                result += L"\\\"";
                break;
            case L'\\':
                result += L"\\\\";
                break;
            case L'\n':
                result += L"\\n";
                break;
            case L'\r':
                result += L"\\r";
                break;
            case L'\t':
                result += L"\\t";
                break;
            default:
                result.push_back(ch);
                break;
        }
    }
    return result;
}

void PrintTree(const TreeNode& node, uint64_t total, int depth,
               const CpuReportOptions& options, std::wostream& output) {
    if (depth == 0 && node.name.empty()) {
        for (const auto& [name, child] : node.children) {
            (void)name;
            PrintTree(child, total, 0, options, output);
        }
        return;
    }
    const double percent = total == 0 ? 0.0 : 100.0 * node.inclusive / total;
    if (percent < options.percent_limit) {
        return;
    }
    output << std::wstring(static_cast<size_t>(depth * 2), L' ') << node.name
           << L"  " << std::fixed << std::setprecision(2) << percent << L"%\n";
    for (const auto& [name, child] : node.children) {
        (void)name;
        PrintTree(child, total, depth + 1, options, output);
    }
}

}  // namespace

void WriteCpuReport(const TraceData& trace, Symbolizer& symbolizer,
                    const CpuReportOptions& options, std::wostream& output) {
    uint64_t total_samples = 0;
    std::unordered_map<std::wstring, Row> rows;
    std::map<std::wstring, uint64_t> folded;
    TreeNode root;

    for (const auto& sample : trace.samples) {
        const std::wstring comm = ProcessDisplayName(trace, sample.pid);
        const auto leaf_address = sample.stack.empty() ? sample.instruction_pointer
                                                        : sample.stack.front();
        const ResolvedSymbol leaf = symbolizer.Resolve(
            trace, sample.pid, leaf_address, sample.timestamp_ns);
        if (!Matches(sample, options, comm, leaf.module, leaf.symbol)) {
            continue;
        }

        ++total_samples;
        const std::wstring key = comm + L"|" + leaf.module + L"|" + leaf.symbol;
        auto& row = rows[key];
        row.comm = comm;
        row.dso = leaf.module;
        row.symbol = leaf.symbol;
        ++row.samples;

        auto stack = StackForSample(sample);
        const bool reverse_stack =
            !(options.call_tree && options.call_tree_caller);
        if (reverse_stack) {
            std::reverse(stack.begin(), stack.end());
        }
        std::wstring folded_stack = FoldedLabel(comm);
        TreeNode* node = &root;
        for (size_t index = 0; index < stack.size(); ++index) {
            const ResolvedSymbol resolved =
                symbolizer.Resolve(trace, sample.pid, stack[index], sample.timestamp_ns);
            folded_stack += L";";
            folded_stack += FoldedLabel(resolved.symbol);
            auto [it, inserted] = node->children.try_emplace(resolved.symbol);
            node = &it->second;
            if (inserted) {
                node->name = resolved.symbol;
            }
            ++node->inclusive;
        }
        ++node->self;
        ++folded[folded_stack];
    }

    if (options.folded) {
        for (const auto& [stack, count] : folded) {
            const double percent = total_samples == 0
                                       ? 0.0
                                       : 100.0 * count / total_samples;
            if (percent < options.percent_limit) {
                continue;
            }
            output << stack << L" " << count << L"\n";
        }
        return;
    }
    if (options.call_tree) {
        PrintTree(root, total_samples, 0, options, output);
        return;
    }

    std::vector<Row> sorted;
    sorted.reserve(rows.size());
    for (auto& [key, row] : rows) {
        (void)key;
        sorted.push_back(row);
    }
    std::vector<std::wstring> sort_fields = options.sort_fields;
    if (sort_fields.empty()) {
        sort_fields.push_back(L"samples");
    }
    std::sort(sorted.begin(), sorted.end(),
              [&](const Row& left, const Row& right) {
                  for (const std::wstring& field : sort_fields) {
                      if (field == L"comm" && left.comm != right.comm) {
                          return left.comm < right.comm;
                      }
                      if (field == L"dso" && left.dso != right.dso) {
                          return left.dso < right.dso;
                      }
                      if (field == L"symbol" && left.symbol != right.symbol) {
                          return left.symbol < right.symbol;
                      }
                      if (field == L"samples" && left.samples != right.samples) {
                          return left.samples > right.samples;
                      }
                  }
                  return left.samples > right.samples;
              });

    if (options.json) {
        output << L"[\n";
        bool first = true;
        for (const auto& row : sorted) {
            const double percent = total_samples == 0
                                       ? 0.0
                                       : 100.0 * row.samples / total_samples;
            if (percent < options.percent_limit) {
                continue;
            }
            if (!first) {
                output << L",\n";
            }
            first = false;
            output << L"  {\"overhead\":" << percent << L",\"samples\":"
                   << row.samples << L",\"comm\":\"" << JsonEscape(row.comm)
                   << L"\",\"dso\":\"" << JsonEscape(row.dso) << L"\",\"symbol\":\""
                   << JsonEscape(row.symbol) << L"\"}";
        }
        output << L"\n]\n";
        return;
    }

    if (options.csv) {
        output << L"overhead,samples,comm,dso,symbol\n";
        for (const auto& row : sorted) {
            const double percent = total_samples == 0
                                       ? 0.0
                                       : 100.0 * row.samples / total_samples;
            if (percent < options.percent_limit) {
                continue;
            }
            output << percent << L"," << row.samples << L",\"" << row.comm << L"\",\""
                   << row.dso << L"\",\"" << row.symbol << L"\"\n";
        }
        return;
    }

    output << L"Overhead  Samples  Command  Shared Object  Symbol\n";
    for (const auto& row : sorted) {
        const double percent = total_samples == 0 ? 0.0
                                                   : 100.0 * row.samples / total_samples;
        if (percent < options.percent_limit) {
            continue;
        }
        output << std::fixed << std::setprecision(2) << std::setw(8) << percent << L"%  "
               << std::setw(7) << row.samples << L"  " << row.comm << L"  "
               << row.dso << L"  " << row.symbol << L"\n";
    }
}

}  // namespace wperf
