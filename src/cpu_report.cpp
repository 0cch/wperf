#include "cpu_report.h"
#include "output.h"
#include "json_writer.h"

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <map>
#include <set>
#include <stdexcept>
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
    (void)sample;
    if (!ContainsInsensitive(comm, options.comm_filter) ||
        !ContainsInsensitive(dso, options.dso_filter) ||
        !ContainsInsensitive(symbol, options.symbol_filter)) {
        return false;
    }
    return true;
}

struct Row {
    uint32_t pid = 0;
    uint64_t process_start_ns = 0;
    std::wstring module_identity;
    std::wstring module_path;
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
        std::vector<uint64_t> stack = sample.stack.View();
        if (sample.instruction_pointer != 0 && stack.front() != sample.instruction_pointer)
            stack.insert(stack.begin(), sample.instruction_pointer);
        return stack;
    }
    return {sample.instruction_pointer};
}

std::wstring ClampField(std::wstring value, size_t width) {
    if (width < 4 || value.size() <= width) {
        return value;
    }
    value.resize(width - 3);
    value += L"...";
    return value;
}

std::wstring FoldedLabel(std::wstring value) {
    for (wchar_t& ch : value) {
        if (ch == L';' || ch == L'\n' || ch == L'\r') {
            ch = L'_';
        }
    }
    return value;
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

bool MatchesSampleSelection(const TraceData& trace,const SampleRecord& sample,const CpuReportOptions& options) {
    if(options.pid&&sample.pid!=*options.pid) {
        if(!options.include_children)return false;
        const auto* process=FindProcess(trace,sample.pid,sample.timestamp_ns);
        std::set<uint32_t> seen;
        bool descendant=false;
        while(process&&seen.insert(process->pid).second) {
            if(process->parent_pid==*options.pid) { descendant=true; break; }
            process=FindProcess(trace,process->parent_pid,process->start_ns);
        }
        if(!descendant)return false;
    }
    if(options.tid&&sample.tid!=*options.tid)return false;
    if(options.cpu&&sample.cpu!=*options.cpu)return false;
    const uint64_t origin=trace.start_ns==UINT64_MAX?0:trace.start_ns;
    const uint64_t relative=sample.timestamp_ns>=origin?sample.timestamp_ns-origin:0;
    if(options.start_offset_ns&&relative<*options.start_offset_ns)return false;
    if(options.end_offset_ns&&relative>*options.end_offset_ns)return false;
    const bool kernel=sample.instruction_pointer>=0xffff800000000000ull ||
        (trace.pointer_size==4&&sample.instruction_pointer>=0x80000000ull&&sample.instruction_pointer<=UINT32_MAX);
    if(options.scope==CpuReportOptions::Scope::User&&kernel)return false;
    if(options.scope==CpuReportOptions::Scope::Kernel&&!kernel)return false;
    if(options.profile_source&&sample.profile_source!=*options.profile_source)return false;
    if(options.exact_stacks_only&&sample.stack_quality!=StackQuality::Exact)return false;
    return true;
}

void WriteCpuReportFeed(const TraceData& trace, Symbolizer& symbolizer,
                    const CpuReportOptions& options, std::wostream& output,
                    const std::function<void(const std::function<void(const SampleRecord&)>&)>& feed,
                    JsonStreamWriter* json_writer) {
    uint64_t total_samples = 0;
    std::map<std::wstring, uint64_t> folded;
    std::unordered_map<std::wstring, Row> rows;
    TreeNode root;

    symbolizer.PrepareTrace(trace);
    ProcessNameIndex process_names(trace);
    feed([&](const SampleRecord& sample) {
        if(!MatchesSampleSelection(trace,sample,options))return;
        const std::wstring comm = process_names.Name(sample.pid, sample.timestamp_ns);
        const auto leaf_address = sample.instruction_pointer;
        const ResolvedSymbol leaf = symbolizer.Resolve(
            trace, sample.pid, leaf_address, sample.timestamp_ns);
        if (!Matches(sample, options, comm, leaf.module, leaf.symbol)) {
            return;
        }

        ++total_samples;
        const uint64_t instance = process_names.InstanceStart(sample.pid, sample.timestamp_ns);
        const std::wstring process_key = std::to_wstring(sample.pid) + L"@" + std::to_wstring(instance);
        const std::wstring key = process_key + L"|" + SymbolAggregateKey(leaf);
        if(rows.size()>=200000&&!rows.contains(key))throw CpuReportLimitError("report aggregation exceeds 200000 rows");
        auto& row = rows[key];
        row.pid = sample.pid;
        row.process_start_ns = instance;
        row.module_identity = leaf.module_identity;
        row.module_path = leaf.module_path;
        row.comm = comm;
        row.dso = leaf.module;
        row.symbol = leaf.symbol;
        ++row.samples;

        if (!options.folded && !options.call_tree) return;
        auto stack = StackForSample(sample);
        const bool reverse_stack =
            !(options.call_tree && options.call_tree_caller);
        if (reverse_stack) {
            std::reverse(stack.begin(), stack.end());
        }
        std::wstring folded_stack = FoldedLabel(comm) + L"[" + process_key + L"]";
        TreeNode* node = &root;
        if (options.call_tree) {
            auto& process_node = root.children[process_key];
            process_node.name = comm + L"[" + process_key + L"]";
            ++process_node.inclusive;
            node = &process_node;
        }
        for (size_t index = 0; index < stack.size(); ++index) {
            const ResolvedSymbol resolved =
                symbolizer.Resolve(trace, sample.pid, stack[index], sample.timestamp_ns);
            const auto emit=[&](const std::wstring& frame_key,const std::wstring& label) {
                if(options.folded) { folded_stack+=L";"; folded_stack+=FoldedLabel(label); }
                if(options.call_tree) {
                    auto [it,inserted]=node->children.try_emplace(frame_key);
                    node=&it->second;
                    if(inserted)node->name=label;
                    ++node->inclusive;
                }
            };
            const std::wstring suffix=resolved.module_identity.empty()?L"":L" ["+resolved.module_identity+L"]";
            const auto emit_inline=[&](const InlineFrame& frame) {
                emit(resolved.module_identity+L"|inline|"+frame.symbol,
                     resolved.module+L"!"+frame.symbol+L" (inline)"+suffix);
            };
            if(!reverse_stack)for(const auto& frame:resolved.inline_frames)emit_inline(frame);
            emit(SymbolAggregateKey(resolved),resolved.module+L"!"+resolved.symbol+suffix);
            if(reverse_stack)for(auto frame=resolved.inline_frames.rbegin();frame!=resolved.inline_frames.rend();++frame)emit_inline(*frame);
        }
        if (options.call_tree) ++node->self;
        if (options.folded) ++folded[folded_stack];
    });

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
        JsonStreamWriter local_writer(output);
        JsonStreamWriter& writer = json_writer == nullptr ? local_writer : *json_writer;
        writer.BeginArray();
        for (const auto& row : sorted) {
            const double percent = total_samples == 0
                                       ? 0.0
                                       : 100.0 * row.samples / total_samples;
            if (percent < options.percent_limit) continue;
            nlohmann::ordered_json item;
            item["overhead"] = percent;
            item["samples"] = row.samples;
            item["comm"] = ToUtf8(row.comm);
            item["dso"] = ToUtf8(row.dso);
            item["symbol"] = ToUtf8(row.symbol);
            item["pid"] = row.pid;
            item["process_start_ns"] = row.process_start_ns;
            item["module_identity"] = ToUtf8(row.module_identity);
            item["module_path"] = ToUtf8(row.module_path);
            writer.Item(item);
        }
        writer.EndArray();
        output << L"\n";
        return;
    }

    if (options.csv) {
        output << L"overhead,samples,comm,dso,symbol,pid,process_start_ns,module_identity\n";
        for (const auto& row : sorted) {
            const double percent = total_samples == 0
                                       ? 0.0
                                       : 100.0 * row.samples / total_samples;
            if (percent < options.percent_limit) {
                continue;
            }
            output << percent << L"," << row.samples << L"," << EscapeCsv(row.comm) << L","
                   << EscapeCsv(row.dso) << L"," << EscapeCsv(row.symbol) << L"," << row.pid << L","
                   << row.process_start_ns << L"," << EscapeCsv(row.module_identity) << L"\n";
        }
        return;
    }

    constexpr size_t kCommandWidth = 24;
    constexpr size_t kDsoWidth = 28;
    output << std::left << std::setw(9) << L"Overhead" << L"  "
           << std::setw(7) << L"Samples" << L"  " << std::setw(kCommandWidth)
           << L"Command" << L"  " << std::setw(kDsoWidth) << L"Shared Object"
           << L"  Symbol\n";
    for (const auto& row : sorted) {
        const double percent = total_samples == 0 ? 0.0
                                                   : 100.0 * row.samples / total_samples;
        if (percent < options.percent_limit) {
            continue;
        }
        output << std::right << std::fixed << std::setprecision(2) << std::setw(8)
               << percent << L"%  " << std::setw(7) << row.samples << L"  "
               << std::left << std::setw(kCommandWidth)
               << ClampField(row.comm, kCommandWidth) << L"  "
               << std::setw(kDsoWidth) << ClampField(row.dso, kDsoWidth) << L"  "
               << row.symbol << L"\n";
    }
}

void WriteCpuReport(const TraceData& trace,Symbolizer& symbols,const CpuReportOptions& options,std::wostream& output,
                    JsonStreamWriter* json_writer) {
    WriteCpuReportFeed(trace,symbols,options,output,[&](const auto& observe) { for(const auto& sample:trace.samples)observe(sample); }, json_writer);
}

}  // namespace wperf
