#include "commands/advanced.h"

#include "commands/help.h"
#include "commands/util.h"
#include "common.h"
#include "archive.h"
#include "output.h"
#include "json_writer.h"
#include "disassembler.h"
#include "etl_reader.h"
#include "symbolizer.h"
#include "trace_model.h"

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cwctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace wperf {
using namespace commands;

struct AggregateValue {
    uint64_t samples = 0;
    uint64_t weight_ns = 0;
    uint64_t known_period_samples=0;
};

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}


std::vector<uint64_t> SampleWeights(const TraceData& trace) {
    std::vector<uint64_t> weights;
    weights.reserve(trace.samples.size());
    for (const auto& sample : trace.samples) weights.push_back(SampleWeightNs(trace, sample));
    return weights;
}

std::map<std::wstring, AggregateValue> AggregateSymbols(
    const TraceData& trace, Symbolizer& symbolizer,
    const std::optional<uint32_t>& pid, const std::wstring& dso_filter,
    bool calltree, bool strict_identity) {
    std::map<std::wstring, AggregateValue> result;
    symbolizer.PrepareTrace(trace);
    const std::vector<uint64_t> weights = SampleWeights(trace);
    const std::wstring dso_lower = Lower(dso_filter);
    const std::wstring trace_key = Lower(std::filesystem::absolute(trace.source_path).lexically_normal().wstring());
    const auto comparison_key = [&](const ResolvedSymbol& symbol, const SampleRecord& sample, uint64_t address) {
        const auto scope = trace_key + L"|" + std::to_wstring(sample.pid) + L"|" + std::to_wstring(address);
        return SymbolComparisonKey(symbol, strict_identity, scope);
    };
    for (size_t index = 0; index < trace.samples.size(); ++index) {
        const auto& sample = trace.samples[index];
        if (pid.has_value() && sample.pid != *pid) {
            continue;
        }
        if (!calltree) {
            const uint64_t address = sample.instruction_pointer;
            const ResolvedSymbol symbol =
                symbolizer.Resolve(trace, sample.pid, address, sample.timestamp_ns);
            if (!dso_lower.empty() &&
                Lower(symbol.module).find(dso_lower) == std::wstring::npos) {
                continue;
            }
            const std::wstring key = comparison_key(symbol, sample, address);
            auto& value = result[key];
            ++value.samples;
            value.weight_ns += weights[index];
            value.known_period_samples+=weights[index]!=0;
            continue;
        }

        std::vector<uint64_t> stack = sample.stack;
        if (stack.empty()) {
            stack.push_back(sample.instruction_pointer);
        } else if (sample.instruction_pointer != 0 && stack.front() != sample.instruction_pointer) {
            stack.insert(stack.begin(), sample.instruction_pointer);
        }
        std::reverse(stack.begin(), stack.end());
        std::wstring key;
        bool first = true;
        bool leaf_matches = dso_lower.empty();
        for (size_t frame = 0; frame < stack.size(); ++frame) {
            const ResolvedSymbol symbol =
                symbolizer.Resolve(trace, sample.pid, stack[frame], sample.timestamp_ns);
            if (frame == stack.size() - 1 && !dso_lower.empty()) {
                leaf_matches =
                    Lower(symbol.module).find(dso_lower) != std::wstring::npos;
            }
            if (!first) {
                key += L";";
            }
            first = false;
            key += comparison_key(symbol, sample, stack[frame]);
        }
        if (!leaf_matches) {
            continue;
        }
        auto& value = result[key];
        ++value.samples;
        value.weight_ns += weights[index];
        value.known_period_samples+=weights[index]!=0;
    }
    return result;
}

int RunDiff(const DiffRequest& request) {
    std::filesystem::path first = request.first;
    std::filesystem::path second = request.second;
    std::wstring symbol_path = request.symbol_path;
    std::filesystem::path archive_directory = request.archive;
    std::wstring dso_filter = request.dso;
    std::optional<uint32_t> pid = request.pid;
    std::optional<uint32_t> first_pid = request.first_pid;
    std::optional<uint32_t> second_pid = request.second_pid;
    std::wstring normalization = request.normalization;
    const bool calltree = request.calltree;
    const bool strict_identity = request.matching == L"identity";
    const bool json = request.json;
    const bool csv = request.csv;
    if (json && csv) {
        PrintError(L"--json and --csv are mutually exclusive");
        return 2;
    }
    if (normalization != L"weight" && normalization != L"samples") {
        PrintError(L"--normalize requires weight or samples");
        return 2;
    }
    if (first.empty() || second.empty()) {
        PrintError(L"diff requires -1 FILE and -2 FILE");
        return 2;
    }
    TraceData first_trace;
    TraceData second_trace;
    std::wstring error;
    if (!ReadEtwFile(first, first_trace, &error, false) ||
        !ReadEtwFile(second, second_trace, &error, false)) {
        PrintError(error);
        return 6;
    }
    if(!archive_directory.empty()) {
        if(!LoadArchiveModules(archive_directory,first_trace,&error)) { PrintError(error); return 6; }
        symbol_path+=L";"+ArchiveSymbolPath(archive_directory,first_trace);
        if(!LoadArchiveModules(archive_directory,second_trace,&error)) { PrintError(error); return 6; }
        symbol_path+=L";"+ArchiveSymbolPath(archive_directory,second_trace);
    }
    Symbolizer first_symbolizer;
    Symbolizer second_symbolizer;
    if (!first_symbolizer.Initialize(symbol_path, &error) ||
        !second_symbolizer.Initialize(symbol_path, &error)) {
        PrintError(error);
        return 6;
    }
    if (first_trace.profile_interval_ns == 0 || second_trace.profile_interval_ns == 0)
        std::wcerr << L"wperf: sampling period unavailable; unknown time weights remain zero\n";
    for (const auto& image : first_trace.images) {
        first_symbolizer.AddModule(image);
    }
    for (const auto& image : second_trace.images) {
        second_symbolizer.AddModule(image);
    }
    const std::optional<uint32_t> before_pid = first_pid ? first_pid : pid;
    const std::optional<uint32_t> after_pid = second_pid ? second_pid : pid;
    const auto before = AggregateSymbols(first_trace, first_symbolizer, before_pid,
                                         dso_filter, calltree, strict_identity);
    const auto after = AggregateSymbols(second_trace, second_symbolizer, after_pid,
                                        dso_filter, calltree, strict_identity);

    uint64_t before_total=0,after_total=0,before_weight_total=0,after_weight_total=0;
    for(const auto& [key,value]:before) { (void)key; before_total+=value.samples; before_weight_total+=value.weight_ns; }
    for(const auto& [key,value]:after) { (void)key; after_total+=value.samples; after_weight_total+=value.weight_ns; }
    bool normalization_fallback = false;
    uint64_t before_known=0, after_known=0;
    for (const auto& [key,value]:before) { (void)key; before_known+=value.known_period_samples; }
    for (const auto& [key,value]:after) { (void)key; after_known+=value.known_period_samples; }
    if (normalization == L"weight" && (before_weight_total == 0 || after_weight_total == 0 ||
        before_known != before_total || after_known != after_total)) {
        normalization = L"samples"; normalization_fallback = true;
    }
    std::set<std::pair<SampleKind,uint32_t>> first_sources,second_sources;
    for(const auto& sample:first_trace.samples)first_sources.emplace(sample.kind,sample.profile_source);
    for(const auto& sample:second_trace.samples)second_sources.emplace(sample.kind,sample.profile_source);
    if(first_sources!=second_sources) { PrintError(L"diff requires compatible sample kinds and sources"); return 2; }
    std::set<std::wstring> keys;
    for (const auto& [key, value] : before) {
        (void)value;
        keys.insert(key);
    }
    for (const auto& [key, value] : after) {
        (void)value;
        keys.insert(key);
    }

    struct DiffRow {
        std::wstring key;
        AggregateValue old_value;
        AggregateValue new_value;
        int64_t sample_delta = 0;
        int64_t weight_delta = 0;
        double before_share = 0.0;
        double after_share = 0.0;
        double delta_pp = 0.0;
        double margin = 0.0;
    };
    std::vector<DiffRow> diff_rows;
    for (const auto& key : keys) {
        DiffRow row;
        row.key = key;
        row.old_value = before.count(key) ? before.at(key) : AggregateValue{};
        row.new_value = after.count(key) ? after.at(key) : AggregateValue{};
        if (row.old_value.samples == row.new_value.samples &&
            row.old_value.weight_ns == row.new_value.weight_ns && before_total==after_total) {
            continue;
        }
        row.sample_delta = static_cast<int64_t>(row.new_value.samples) - static_cast<int64_t>(row.old_value.samples);
        row.weight_delta = static_cast<int64_t>(row.new_value.weight_ns) - static_cast<int64_t>(row.old_value.weight_ns);
        const uint64_t before_denominator = normalization == L"weight" ? before_weight_total : before_total;
        const uint64_t after_denominator = normalization == L"weight" ? after_weight_total : after_total;
        row.before_share = before_denominator ? double(normalization == L"weight" ? row.old_value.weight_ns : row.old_value.samples)/before_denominator : 0;
        row.after_share = after_denominator ? double(normalization == L"weight" ? row.new_value.weight_ns : row.new_value.samples)/after_denominator : 0;
        row.delta_pp = 100*(row.after_share-row.before_share);
        row.margin = 196*std::sqrt((before_total?row.before_share*(1-row.before_share)/before_total:0)+
                                         (after_total?row.after_share*(1-row.after_share)/after_total:0));
        diff_rows.push_back(std::move(row));
    }

    if (json) {
        JsonStreamWriter writer(std::wcout);
        writer.BeginArray();
        for (const auto& row : diff_rows) {
            nlohmann::ordered_json item;
            item["key"] = ToUtf8(row.key);
            item["matching"] = ToUtf8(request.matching);
            item["match_status"] = row.old_value.samples == 0 ? "added" :
                                   row.new_value.samples == 0 ? "removed" : "matched";
            item["samples_before"] = row.old_value.samples;
            item["samples_after"] = row.new_value.samples;
            item["delta_samples"] = row.sample_delta;
            item["weight_before_ns"] = row.old_value.weight_ns;
            item["weight_after_ns"] = row.new_value.weight_ns;
            item["delta_weight_ns"] = row.weight_delta;
            item["share_before"] = row.before_share;
            item["share_after"] = row.after_share;
            item["delta_pp"] = row.delta_pp;
            item["approximate_95_margin_pp"] = row.margin;
            item["uncertainty_model"] = "independent-binomial-samples";
            item["known_period_samples_before"] = row.old_value.known_period_samples;
            item["known_period_samples_after"] = row.new_value.known_period_samples;
            item["trace_duration_before_ns"] = first_trace.start_ns<=first_trace.end_ns?first_trace.end_ns-first_trace.start_ns:0;
            item["trace_duration_after_ns"] = second_trace.start_ns<=second_trace.end_ns?second_trace.end_ns-second_trace.start_ns:0;
            item["profile_interval_before_ns"] = first_trace.profile_interval_ns;
            item["profile_interval_after_ns"] = second_trace.profile_interval_ns;
            item["normalization"] = ToUtf8(normalization);
            item["normalization_fallback"] = normalization_fallback;
            item["compatible"] = true;
            item["exact_time"] = false;
            writer.Item(item);
        }
        writer.EndArray();
        std::wcout << L"\n";
        return 0;
    }
    if (csv) {
        std::wcout << L"samples_before,samples_after,delta_samples,"
                      L"weight_before_ns,weight_after_ns,delta_weight_ns,delta_pp,approximate_95_margin_pp,key,matching,match_status\n";
        for (const auto& row : diff_rows) {
            std::wcout << row.old_value.samples << L"," << row.new_value.samples << L","
                       << row.sample_delta << L"," << row.old_value.weight_ns << L","
                       << row.new_value.weight_ns << L"," << row.weight_delta << L","
                       << row.delta_pp << L"," << row.margin << L"," << EscapeCsv(row.key) << L"," << request.matching << L","
                       << (row.old_value.samples == 0 ? L"added" : row.new_value.samples == 0 ? L"removed" : L"matched") << L"\n";
        }
        return 0;
    }
    std::wcout << std::right << std::setw(13) << L"DeltaSamples"
               << std::setw(11) << L"DeltaUs" << std::setw(8) << L"Before"
               << std::setw(8) << L"After" << L"  Key\n";
    for (const auto& row : diff_rows) {
        std::wcout << std::showpos << std::right << std::setw(13)
                   << row.sample_delta << std::noshowpos << std::setw(11)
                   << row.weight_delta / 1000 << L"  " << row.delta_pp << L"pp +/-" << row.margin << L"  " << std::setw(8)
                   << row.old_value.samples << std::setw(8)
                   << row.new_value.samples << L"  " << row.key << L"\n";
    }
    return 0;
}


int RunAnnotate(const AnnotateRequest& request) {
    std::filesystem::path input = request.input;
    std::wstring symbol_path = request.symbol_path;
    std::filesystem::path archive_directory = request.archive;
    std::optional<uint32_t> pid = request.pid;
    uint64_t limit = request.limit;
    bool json = request.json;
    bool csv = request.csv;
    bool disasm = request.disasm;
    bool stdio_output = request.stdio_output;
    if (json && csv) {
        PrintError(L"--json and --csv are mutually exclusive");
        return 2;
    }
    if (stdio_output && (json || csv)) {
        PrintError(L"--stdio cannot be combined with --json or --csv");
        return 2;
    }
    if (input.empty()) {
        PrintError(L"annotate requires -i FILE");
        return 2;
    }
    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error, false)) {
        PrintError(error);
        return 6;
    }
    if(!archive_directory.empty()) {
        if(!LoadArchiveModules(archive_directory,trace,&error)) { PrintError(error); return 6; }
        symbol_path+=L";"+ArchiveSymbolPath(archive_directory,trace);
    }
    Symbolizer symbolizer;
    if (!symbolizer.Initialize(symbol_path, &error)) { PrintError(error); return 6; }
    symbolizer.PrepareTrace(trace);
    for (const auto& image : trace.images) {
        symbolizer.AddModule(image);
    }
    struct InstructionHeat {
        uint64_t instruction_rva = 0;
        std::wstring bytes;
        std::wstring text;
        uint64_t samples = 0;
        double overhead = 0.0;
    };
    struct Heat {
        std::wstring module;
        std::wstring symbol;
        std::wstring source_file;
        std::wstring source_text;
        std::wstring disassembly;
        const ImageRecord* image = nullptr;
        uint64_t address = 0;
        uint64_t displacement = 0;
        uint32_t line = 0;
        uint64_t samples = 0;
        uint64_t function_rva = 0;
        uint64_t function_size = 0;
        std::vector<InstructionHeat> instruction_rows;
        std::map<uint64_t,uint64_t> instruction_samples;
    };
    std::unordered_map<std::wstring, Heat> heat;
    heat.reserve(trace.samples.size());
    ImageIndex image_index;
    image_index.Build(trace);
    for (const auto& sample : trace.samples) {
        if (pid.has_value() && sample.pid != *pid) {
            continue;
        }
        const ResolvedSymbol symbol = symbolizer.Resolve(
            trace, sample.pid, sample.instruction_pointer, sample.timestamp_ns);
        const std::wstring key = SymbolAggregateKey(symbol);
        auto& value = heat[key];
        value.module = symbol.module;
        value.symbol = symbol.symbol;
        value.source_file = symbol.source_file;
        value.image = image_index.Find(sample.pid, sample.instruction_pointer,
                                        sample.timestamp_ns);
        value.address=value.image ? value.image->base+symbol.function_rva : sample.instruction_pointer;
        value.function_rva=symbol.function_rva;
        value.function_size=symbol.function_size;
        ++value.instruction_samples[sample.instruction_pointer];
        value.displacement = symbol.displacement;
        value.line = symbol.line;
        ++value.samples;
    }
    std::vector<Heat> rows;
    for (auto& [key, value] : heat) {
        (void)key;
        rows.push_back(value);
    }
    std::sort(rows.begin(), rows.end(), [](const Heat& a, const Heat& b) {
        if (a.samples != b.samples) return a.samples > b.samples;
        if (a.symbol != b.symbol) return a.symbol < b.symbol;
        if (a.module != b.module) return a.module < b.module;
        return a.address < b.address;
    });
    for (auto& row : rows) {
        if (row.source_file.empty() || row.line == 0) continue;
        std::wifstream source(std::filesystem::path(row.source_file));
        if (!source) continue;
        std::wstring line;
        for (uint32_t current = 1; current <= row.line && std::getline(source, line); ++current) {
            if (current == row.line) row.source_text = line;
        }
    }

    bool disassembly_failed = false;
    if (disasm) {
        for (size_t index = 0; index < rows.size() && index < limit; ++index) {
            Heat& row = rows[index];
            if (row.image == nullptr) {
                continue;
            }
            std::vector<DisassembledInstruction> decoded;
            std::wstring decode_error;
            if (!DisassembleImageRange(*row.image,row.address,row.function_size ? std::min<uint64_t>(row.function_size,65536) : 64,
                                         trace.pointer_size,row.function_size ? 8192 : 8,decoded,&decode_error)) {
                disassembly_failed = true;
                continue;
            }
            for (const auto& instruction : decoded) {
                if (!row.disassembly.empty()) {
                    row.disassembly += L" | ";
                }
                wchar_t address_text[32]{};
                swprintf_s(address_text, L"%llx",
                           static_cast<unsigned long long>(instruction.address));
                uint64_t hits=0;
                for(const auto& [address,count]:row.instruction_samples)
                    if(address>=instruction.address&&address-instruction.address<instruction.length)hits+=count;
                row.disassembly+=L"samples="+std::to_wstring(hits)+L" ";
                row.disassembly += address_text;
                row.disassembly += L"  ";
                row.disassembly += ToUtf16(instruction.bytes);
                row.disassembly += L"  ";
                row.disassembly += ToUtf16(instruction.text);
                InstructionHeat item;
                item.instruction_rva = row.image->base != 0 && instruction.address >= row.image->base
                                           ? instruction.address - row.image->base : instruction.address;
                item.bytes = ToUtf16(instruction.bytes);
                item.text = ToUtf16(instruction.text);
                item.samples = hits;
                item.overhead = row.samples == 0 ? 0.0 : 100.0 * hits / row.samples;
                row.instruction_rows.push_back(std::move(item));
            }
        }
        if (disassembly_failed && !DisassemblyAvailable()) {
            std::wcerr << L"wperf: disassembly support is not enabled\n";
        }
    }
    if (json) {
        JsonStreamWriter writer(std::wcout);
        writer.BeginArray();
        for (size_t index = 0; index < rows.size() && index < limit; ++index) {
            const auto& row = rows[index];
            nlohmann::ordered_json item;
            item["samples"] = row.samples;
            item["module"] = ToUtf8(row.module);
            item["symbol"] = ToUtf8(row.symbol);
            item["displacement"] = row.displacement;
            item["source_file"] = ToUtf8(row.source_file);
            item["source_text"] = ToUtf8(row.source_text);
            item["line"] = row.line;
            item["function_rva"] = row.function_rva;
            item["disassembly"] = ToUtf8(row.disassembly);
            nlohmann::ordered_json instructions = nlohmann::ordered_json::array();
            for (const auto& instruction : row.instruction_rows) {
                nlohmann::ordered_json value;
                value["instruction_rva"] = instruction.instruction_rva;
                value["bytes"] = ToUtf8(instruction.bytes);
                value["text"] = ToUtf8(instruction.text);
                value["samples"] = instruction.samples;
                value["overhead"] = instruction.overhead;
                instructions.push_back(std::move(value));
            }
            item["instruction_rows"] = std::move(instructions);
            writer.Item(item);
        }
        writer.EndArray();
        std::wcout << L"\n";
        return 0;
    }
    if (csv) {
        std::wcout << L"samples,module,symbol,displacement,source_file,line,source_text,disassembly,function_rva,instruction_rva,instruction_bytes,instruction_samples,instruction_overhead\n";
        for (size_t index = 0; index < rows.size() && index < limit; ++index) {
            const auto& row = rows[index];
            if (row.instruction_rows.empty()) {
                std::wcout << row.samples << L"," << EscapeCsv(row.module) << L","
                           << EscapeCsv(row.symbol) << L"," << row.displacement << L","
                           << EscapeCsv(row.source_file) << L"," << row.line << L"," << EscapeCsv(row.source_text) << L","
                           << EscapeCsv(row.disassembly) << L"," << row.function_rva << L",,,,\n";
                continue;
            }
            for (const auto& item : row.instruction_rows) {
                std::wcout << row.samples << L"," << EscapeCsv(row.module) << L","
                           << EscapeCsv(row.symbol) << L"," << row.displacement << L","
                           << EscapeCsv(row.source_file) << L"," << row.line << L"," << EscapeCsv(row.source_text) << L","
                           << EscapeCsv(row.disassembly) << L"," << row.function_rva << L","
                           << item.instruction_rva << L"," << EscapeCsv(item.bytes) << L","
                           << item.samples << L"," << item.overhead << L"\n";
            }
        }
        return 0;
    }
    constexpr size_t kLocationWidth = 56;
    std::wcout << std::left << std::setw(8) << L"Samples" << L"  "
               << std::setw(kLocationWidth) << L"Location" << L"  Source\n";
    for (size_t index = 0; index < rows.size() && index < limit; ++index) {
        std::wostringstream location;
        location << rows[index].module << L"!" << rows[index].symbol << L"+0x"
                 << std::hex << rows[index].displacement;
        std::wcout << std::right << std::setw(8) << rows[index].samples << L"  "
                   << std::left << std::setw(kLocationWidth)
                   << ClampText(location.str(), kLocationWidth) << L"  ";
        if (!rows[index].source_file.empty()) {
            std::wcout << rows[index].source_file << L":" << std::dec
                       << rows[index].line;
        }
        std::wcout << L"\n";
        if (!rows[index].disassembly.empty()) {
            std::wcout << L"  " << rows[index].disassembly << L"\n";
        }
    }
    return 0;
}

std::wstring Sha256File(const std::filesystem::path& path, std::wstring* error = nullptr) {
    const auto digest=FileSha256(path);
    if(digest.empty() && error)*error=L"cannot hash file: "+path.wstring();
    return digest;
}

int RunArchive(const ArchiveRequest& request) {
    const std::filesystem::path input = request.input;
    const std::filesystem::path output = request.output;
    const std::optional<uint32_t> pid = request.pid;
    const std::wstring symbol_path = request.symbol_path;
    const uint64_t max_size_mb = request.max_size_mb;
    const bool include_trace = request.include_trace;
    if (input.empty() || output.empty()) {
        PrintError(L"archive requires -i FILE and -o DIRECTORY");
        return 2;
    }
    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error, false)) {
        PrintError(error);
        return 6;
    }
    if (!EnsureDirectory(output, &error)) {
        PrintError(error);
        return 9;
    }
    Symbolizer archive_symbols;
    if (!archive_symbols.Initialize(symbol_path, &error)) { PrintError(error); return 6; }
    ImageIndex archive_images;
    archive_images.Build(trace);
    std::map<std::wstring, ImageRecord> needed_images;
    const auto require_image = [&](const ImageRecord& image) {
        const std::wstring key = image.path + L"|" + std::to_wstring(image.timestamp) +
                                 L"|" + std::to_wstring(image.checksum) + L"|" + std::to_wstring(image.size);
        needed_images.emplace(key, image);
    };
    for (const auto& sample : trace.samples) {
        if (pid.has_value() && sample.pid != *pid) {
            continue;
        }
        if (const auto* image = archive_images.Find(sample.pid,
                                          sample.instruction_pointer,
                                          sample.timestamp_ns)) {
            if (!image->path.empty()) {
                require_image(*image);
            }
        }
        for (uint64_t frame : sample.stack) {
            if (const auto* image =
                    archive_images.Find(sample.pid, frame, sample.timestamp_ns)) {
                if (!image->path.empty()) {
                    require_image(*image);
                }
            }
        }
    }

    struct ArchiveEntry {
        std::wstring source;
        std::wstring archived;
        bool copied = false;
        std::wstring reason;
        uint64_t size = 0;
        std::wstring sha256;
        ModuleIdentity identity;
        std::wstring pdb_source;
        std::wstring pdb_archived;
        bool pdb_copied = false;
        std::wstring pdb_sha256;
    };
    std::vector<ArchiveEntry> entries;
    if (max_size_mb > UINT64_MAX / (1024ull * 1024ull)) return 2;
    const uint64_t max_bytes = max_size_mb == 0
                                   ? UINT64_MAX
                                   : max_size_mb * 1024ull * 1024ull;
    for (const auto& [key, image] : needed_images) {
        (void)key;
        ArchiveEntry entry;
        entry.source = image.path;
        const std::filesystem::path source = ModuleFilePath(image);
        std::error_code ec;
        if (!std::filesystem::exists(source, ec) ||
            !std::filesystem::is_regular_file(source, ec)) {
            entry.reason = L"missing";
            entries.push_back(std::move(entry));
            continue;
        }
        entry.size = std::filesystem::file_size(source, ec);
        entry.identity = ReadModuleIdentity(source.wstring());
        if (!ModuleIdentityMatches(image, entry.identity)) {
            entry.reason = L"identity_mismatch";
            entries.push_back(std::move(entry));
            continue;
        }
        if (entry.size > max_bytes) {
            entry.reason = L"oversized";
            entries.push_back(std::move(entry));
            continue;
        }
        const std::wstring source_hash = Sha256File(source);
        if (source_hash.empty()) { entry.reason = L"hash_failed"; entries.push_back(std::move(entry)); continue; }
        const std::filesystem::path destination = output / L"modules" / source_hash / source.filename();
        if (!EnsureDirectory(destination.parent_path(), &error)) { PrintError(error); return 9; }
        std::filesystem::copy_file(source, destination,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        if (ec) {
            entry.reason = L"copy_failed";
            entries.push_back(std::move(entry));
            continue;
        }
        entry.archived = std::filesystem::relative(destination, output).generic_wstring();
        entry.sha256 = Sha256File(destination);
        if (entry.sha256 != source_hash ||
            !ModuleIdentityMatches(image, ReadModuleIdentity(destination.wstring()))) {
            entry.reason = L"identity_changed";
            entries.push_back(std::move(entry));
            continue;
        }
        entry.copied = true;

        const std::filesystem::path pdb = archive_symbols.PdbPath(image);
        entry.pdb_source = pdb.wstring();
        if (!pdb.empty() && std::filesystem::is_regular_file(pdb, ec) &&
            std::filesystem::file_size(pdb, ec) <= max_bytes) {
            std::wstring guid = FormatGuid(entry.identity.pdb_guid);
            std::erase_if(guid, [](wchar_t ch) { return ch == L'{' || ch == L'}' || ch == L'-'; });
            wchar_t age[16]{};
            swprintf_s(age, L"%x", entry.identity.pdb_age);
            const std::filesystem::path pdb_destination = output / L"symbols" / pdb.filename() /
                                                         (guid + age) / pdb.filename();
            if (!EnsureDirectory(pdb_destination.parent_path(), &error)) { PrintError(error); return 9; }
            std::filesystem::copy_file(pdb, pdb_destination,
                                      std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec && PdbIdentityMatches(pdb_destination.wstring(), entry.identity)) {
                entry.pdb_archived = std::filesystem::relative(pdb_destination, output).generic_wstring();
                entry.pdb_copied = true;
                entry.pdb_sha256 = Sha256File(pdb_destination);
                std::filesystem::copy_file(pdb_destination,destination.parent_path()/pdb.filename(),
                    std::filesystem::copy_options::overwrite_existing,ec);
                if(ec) { PrintError(L"cannot place matching PDB next to archived module"); return 9; }
            }
        }
        entries.push_back(std::move(entry));
    }

    {
        std::ofstream map(output/L"modules.tsv",std::ios::binary);
        if(!map) { PrintError(L"cannot write archive map"); return 9; }
        map<<"wperf-archive-map-v1\n";
        for(const auto& entry:entries) if(entry.copied)
            map<<HexText(entry.source)<<' '<<entry.identity.timestamp<<' '<<entry.identity.checksum<<' '
               <<entry.identity.size_of_image<<' '<<HexText(entry.archived)<<' '<<ToUtf8(entry.sha256)<<'\n';
        if(!map) { PrintError(L"cannot finish archive map"); return 9; }
    }
    if(include_trace) {
        std::error_code ec;
        if(std::filesystem::file_size(input,ec)>max_bytes || ec) { PrintError(L"ETL exceeds archive file limit"); return 9; }
        std::filesystem::copy_file(input,output/L"trace.etl",std::filesystem::copy_options::overwrite_existing,ec);
        if(ec) { PrintError(L"cannot archive trace.etl"); return 9; }
    }

    auto write_manifest = [&](const std::filesystem::path& path, bool json,
                              bool csv) {
        std::ofstream manifest(path, std::ios::binary);
        if (!manifest) throw std::runtime_error("cannot write archive manifest");
        if (json) {
            std::wostringstream json_output;
            JsonStreamWriter writer(json_output);
            writer.BeginObject();
            writer.Key(L"trace"); writer.String(input.wstring());
            writer.Key(L"symbol_path"); writer.String(symbol_path);
            writer.Key(L"max_size_mb"); writer.Unsigned(max_size_mb);
            writer.Key(L"entries"); writer.BeginArray();
            for (const auto& entry : entries) {
                const std::wstring guid = entry.identity.has_pdb_guid
                                              ? FormatGuid(entry.identity.pdb_guid)
                                              : L"";
                nlohmann::ordered_json item;
                item["source"] = ToUtf8(entry.source);
                item["archived"] = ToUtf8(entry.archived);
                item["copied"] = entry.copied;
                item["reason"] = ToUtf8(entry.reason);
                item["size"] = entry.size;
                item["sha256"] = ToUtf8(entry.sha256);
                item["pe_timestamp"] = entry.identity.timestamp;
                item["pe_checksum"] = entry.identity.checksum;
                item["pe_size_of_image"] = entry.identity.size_of_image;
                item["machine"] = entry.identity.machine;
                item["pdb_guid"] = ToUtf8(guid);
                item["pdb_age"] = entry.identity.pdb_age;
                item["pdb_source"] = ToUtf8(entry.pdb_source);
                item["pdb_archived"] = ToUtf8(entry.pdb_archived);
                item["pdb_copied"] = entry.pdb_copied;
                item["pdb_sha256"] = ToUtf8(entry.pdb_sha256);
                writer.Item(item);
            }
            writer.EndArray();
            writer.EndObject();
            manifest << ToUtf8(json_output.str()) << "\n";
        } else if (csv) {
            manifest << "source,archived,copied,reason,size,sha256,pe_timestamp,"
                        "pe_checksum,pe_size_of_image,machine,pdb_guid,pdb_age,"
                        "pdb_source,pdb_archived,pdb_copied,pdb_sha256\n";
            for (const auto& entry : entries) {
                const std::wstring guid = entry.identity.has_pdb_guid
                                              ? FormatGuid(entry.identity.pdb_guid)
                                              : L"";
                manifest << ToUtf8(EscapeCsv(entry.source)) << ','
                         << ToUtf8(EscapeCsv(entry.archived)) << ','
                         << (entry.copied ? "true" : "false") << ','
                         << ToUtf8(EscapeCsv(entry.reason)) << ',' << entry.size << ','
                         << ToUtf8(entry.sha256) << ',' << entry.identity.timestamp
                         << ',' << entry.identity.checksum << ','
                         << entry.identity.size_of_image << ','
                         << entry.identity.machine << ',' << ToUtf8(guid) << ','
                         << entry.identity.pdb_age << ','
                         << ToUtf8(EscapeCsv(entry.pdb_source)) << ','
                         << ToUtf8(EscapeCsv(entry.pdb_archived)) << ','
                         << (entry.pdb_copied ? "true" : "false") << ','
                         << ToUtf8(entry.pdb_sha256) << '\n';
            }
        } else {
            manifest << "wperf archive\n"
                     << "trace=" << ToUtf8(input.wstring()) << "\n"
                     << "symbol_path=" << ToUtf8(symbol_path) << "\n"
                     << "entries=" << entries.size() << "\n";
            for (const auto& entry : entries) {
                manifest << "source=" << ToUtf8(entry.source)
                         << " archived=" << ToUtf8(entry.archived)
                         << " copied=" << (entry.copied ? "true" : "false")
                         << " reason=" << ToUtf8(entry.reason)
                         << " size=" << entry.size
                         << " sha256=" << ToUtf8(entry.sha256)
                         << " pe_timestamp=" << entry.identity.timestamp
                         << " pe_checksum=" << entry.identity.checksum
                         << " pdb_copied=" << (entry.pdb_copied ? "true" : "false")
                         << " pdb_sha256=" << ToUtf8(entry.pdb_sha256) << "\n";
            }
        }
    };
    write_manifest(output / L"manifest.json", true, false);
    write_manifest(output / L"manifest.csv", false, true);
    write_manifest(output / L"manifest.txt", false, false);

    size_t copied = 0;
    for (const auto& entry : entries) {
        copied += entry.copied ? 1 : 0;
    }
    std::wcout << L"archive: " << output.wstring() << L"\n";
    std::wcout << L"  modules: " << entries.size() << L"  copied: " << copied
               << L"  manifest: manifest.json/csv/txt\n";
    return 0;
}

int RunBuildIdList(const BuildIdListRequest& request) {
    std::filesystem::path input = request.input;
    bool json = request.json;
    bool csv = request.csv;
    if (input.empty()) {
        PrintError(L"buildid-list requires -i FILE");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (json && csv) {
        PrintError(L"--json and --csv are mutually exclusive");
        return ExitStatus(ExitCode::InvalidArguments);
    }

    TraceData trace;
    std::wstring error;
    if (!ReadEtwMetadata(input, trace, &error)) {
        PrintError(error);
        return ExitStatus(ExitCode::ReplayOrQualityFailure);
    }

    struct Row {
        const ImageRecord* image = nullptr;
        ModuleIdentity identity;
        std::wstring pdb_guid;
        std::wstring sha256;
        std::wstring status;
    };
    struct LocalFile { ModuleIdentity identity; std::wstring sha256; };
    std::map<std::wstring, LocalFile> local_files;
    std::map<std::wstring, std::wstring> normalized_paths;
    std::vector<Row> rows;
    rows.reserve(trace.images.size());
    for (const auto& image : trace.images) {
        if (image.path.empty()) continue;
        Row row;
        row.image = &image;
        auto [path_it,new_path] = normalized_paths.try_emplace(image.path);
        if (new_path) path_it->second = ModuleFilePath(image);
        const auto& path = path_it->second;
        auto [local_it,new_file] = local_files.try_emplace(Lower(path));
        if (new_file) {
            local_it->second.identity = ReadModuleIdentity(path);
            if (request.hash && local_it->second.identity.valid)
                local_it->second.sha256 = FileSha256(path);
        }
        row.identity = local_it->second.identity;
        row.sha256 = local_it->second.sha256;
        row.pdb_guid = row.identity.has_pdb_guid ? FormatGuid(row.identity.pdb_guid) : L"";
        row.status = !row.identity.valid ? L"missing" :
            ModuleIdentityMatches(image, row.identity) ? L"matching" : L"mismatched";
        rows.push_back(std::move(row));
    }

    if (json) {
        JsonStreamWriter writer(std::wcout);
        writer.BeginArray();
        for (const auto& row : rows) {
            nlohmann::ordered_json item;
            item["base"] = row.image->base;
            item["size"] = row.image->size;
            item["pid"] = row.image->pid;
            item["pe_timestamp"] = row.image->timestamp;
            item["pe_checksum"] = row.image->checksum;
            item["machine"] = row.identity.machine;
            item["pdb_guid"] = ToUtf8(row.pdb_guid);
            item["pdb_age"] = row.identity.pdb_age;
            item["module_sha256"] = ToUtf8(row.sha256);
            item["path"] = ToUtf8(row.image->path);
            item["status"] = ToUtf8(row.status);
            item["recorded_identity"] = {{"pe_timestamp",row.image->timestamp},{"pe_checksum",row.image->checksum},{"size_of_image",row.image->size}};
            item["local_identity"] = {{"valid",row.identity.valid},{"pe_timestamp",row.identity.timestamp},
                {"pe_checksum",row.identity.checksum},{"size_of_image",row.identity.size_of_image},
                {"machine",row.identity.machine},{"pdb_guid",ToUtf8(row.pdb_guid)},{"pdb_age",row.identity.pdb_age}};
            item["module_sha256_source"] = "local-file";
            item["hash_status"] = !request.hash ? "not-requested" : row.sha256.empty() ? "unavailable" : "computed";
            writer.Item(item);
        }
        writer.EndArray();
        std::wcout << L"\n";
        return ExitStatus(ExitCode::Success);
    }

    if (csv) {
        std::wcout << L"base,size,pid,pe_timestamp,pe_checksum,machine,pdb_guid,pdb_age,module_sha256,path,status,local_pe_timestamp,local_pe_checksum,local_size_of_image,hash_status\n";
        for (const auto& row : rows) {
            std::wcout << row.image->base << L',' << row.image->size << L','
                       << row.image->pid << L',' << row.image->timestamp << L','
                       << row.image->checksum << L',' << row.identity.machine << L','
                       << EscapeCsv(row.pdb_guid) << L',' << row.identity.pdb_age << L','
                       << row.sha256 << L',' << EscapeCsv(row.image->path) << L','
                       << EscapeCsv(row.status) << L',' << row.identity.timestamp << L',' << row.identity.checksum
                       << L',' << row.identity.size_of_image << L','
                       << (!request.hash ? L"not-requested" : row.sha256.empty() ? L"unavailable" : L"computed") << L"\n";
        }
        return ExitStatus(ExitCode::Success);
    }

    std::wcout << L"Base              Size      PID       Timestamp  Checksum   GUID/age                         Status     Module\n";
    for (const auto& row : rows) {
        std::wcout << std::hex << std::setw(16) << std::setfill(L'0') << row.image->base
                   << L" " << std::setw(8) << row.image->size << std::dec
                   << std::setfill(L' ') << L" " << std::setw(8) << row.image->pid
                   << L" " << std::setw(10) << row.image->timestamp
                   << L" " << std::setw(10) << row.image->checksum
                   << L" " << std::setw(32) << row.pdb_guid << L" " << std::setw(10)
                   << row.status << L" " << row.image->path << L"\n";
    }
    return ExitStatus(ExitCode::Success);
}
}  // namespace wperf
