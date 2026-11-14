#include "commands/advanced.h"

#include "commands/help.h"
#include "commands/util.h"
#include "common.h"
#include "archive.h"
#include "output.h"
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
    bool calltree) {
    std::map<std::wstring, AggregateValue> result;
    symbolizer.PrepareTrace(trace);
    const std::vector<uint64_t> weights = SampleWeights(trace);
    const std::wstring dso_lower = Lower(dso_filter);
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
            const std::wstring key = SymbolAggregateKey(symbol);
            auto& value = result[key];
            ++value.samples;
            value.weight_ns += weights[index];
            value.known_period_samples+=weights[index]!=0;
            continue;
        }

        std::vector<uint64_t> stack = sample.stack;
        if (stack.empty()) {
            stack.push_back(sample.instruction_pointer);
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
            key += SymbolAggregateKey(symbol);
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

int RunDiff(const std::vector<std::wstring>& arguments) {
    std::filesystem::path first;
    std::filesystem::path second;
    std::wstring symbol_path;
    std::filesystem::path archive_directory;
    std::wstring dso_filter;
    std::optional<uint32_t> pid;
    bool calltree = false;
    bool json = false;
    bool csv = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-1" || arguments[index] == L"--first") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            first = arguments[++index];
        } else if (arguments[index] == L"-2" || arguments[index] == L"--second") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            second = arguments[++index];
        } else if (arguments[index] == L"--archive") {
            if(index+1>=arguments.size())return 2;
            archive_directory=arguments[++index];
        } else if (arguments[index] == L"--symbol-path") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            symbol_path = arguments[++index];
        } else if (arguments[index] == L"--pid") {
            uint32_t value = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], value)) {
                return 2;
            }
            pid = value;
        } else if (arguments[index] == L"--dso") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            dso_filter = arguments[++index];
        } else if (arguments[index] == L"--calltree" ||
                   arguments[index] == L"--weighted") {
            calltree = calltree || arguments[index] == L"--calltree";
        } else if (arguments[index] == L"--json") {
            json = true;
        } else if (arguments[index] == L"--csv") {
            csv = true;
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            PrintCommandHelp(L"diff");
            return 0;
        } else {
            PrintError(L"unknown diff option: " + arguments[index]);
            return 2;
        }
    }
    if (json && csv) {
        PrintError(L"--json and --csv are mutually exclusive");
        return 2;
    }
    if (first.empty() || second.empty()) {
        PrintError(L"diff requires -1 FILE and -2 FILE");
        return 2;
    }

    TraceData first_trace;
    TraceData second_trace;
    std::wstring error;
    if (!ReadEtwFile(first, first_trace, &error) ||
        !ReadEtwFile(second, second_trace, &error)) {
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
    const auto before = AggregateSymbols(first_trace, first_symbolizer, pid,
                                         dso_filter, calltree);
    const auto after = AggregateSymbols(second_trace, second_symbolizer, pid,
                                        dso_filter, calltree);

    uint64_t before_total=0,after_total=0;
    for(const auto& [key,value]:before) { (void)key; before_total+=value.samples; }
    for(const auto& [key,value]:after) { (void)key; after_total+=value.samples; }
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

    if (json) {
        std::wcout << L"[\n";
    } else if (csv) {
        std::wcout << L"samples_before,samples_after,delta_samples,"
                      L"weight_before_ns,weight_after_ns,delta_weight_ns,delta_pp,approximate_95_margin_pp,key\n";
    } else {
        std::wcout << std::right << std::setw(13) << L"DeltaSamples"
                   << std::setw(11) << L"DeltaUs" << std::setw(8) << L"Before"
                   << std::setw(8) << L"After" << L"  Key\n";
    }
    bool first_row = true;
    for (const auto& key : keys) {
        const AggregateValue old_value = before.count(key) ? before.at(key)
                                                           : AggregateValue{};
        const AggregateValue new_value = after.count(key) ? after.at(key)
                                                          : AggregateValue{};
        if (old_value.samples == new_value.samples &&
            old_value.weight_ns == new_value.weight_ns && before_total==after_total) {
            continue;
        }
        const int64_t sample_delta = static_cast<int64_t>(new_value.samples) -
                                     static_cast<int64_t>(old_value.samples);
        const int64_t weight_delta = static_cast<int64_t>(new_value.weight_ns) -
                                     static_cast<int64_t>(old_value.weight_ns);
        const double before_share=before_total ? double(old_value.samples)/before_total : 0;
        const double after_share=after_total ? double(new_value.samples)/after_total : 0;
        const double delta_pp=100*(after_share-before_share);
        const double margin=196*std::sqrt((before_total?before_share*(1-before_share)/before_total:0)+
                                         (after_total?after_share*(1-after_share)/after_total:0));
        if (json) {
            if (!first_row) {
                std::wcout << L",\n";
            }
            first_row = false;
            std::wcout << L"{\"key\":\"" << EscapeJson(key)
                       << L"\",\"samples_before\":" << old_value.samples
                       << L",\"samples_after\":" << new_value.samples
                       << L",\"delta_samples\":" << sample_delta
                       << L",\"weight_before_ns\":" << old_value.weight_ns
                       << L",\"weight_after_ns\":" << new_value.weight_ns
                       << L",\"delta_weight_ns\":" << weight_delta
                       << L",\"share_before\":" << before_share << L",\"share_after\":" << after_share
                       << L",\"delta_pp\":" << delta_pp << L",\"approximate_95_margin_pp\":" << margin
                       << L",\"uncertainty_model\":\"independent-binomial-samples\""
                       << L",\"known_period_samples_before\":" << old_value.known_period_samples
                       << L",\"known_period_samples_after\":" << new_value.known_period_samples
                       << L",\"trace_duration_before_ns\":" << (first_trace.start_ns<=first_trace.end_ns?first_trace.end_ns-first_trace.start_ns:0)
                       << L",\"trace_duration_after_ns\":" << (second_trace.start_ns<=second_trace.end_ns?second_trace.end_ns-second_trace.start_ns:0)
                       << L",\"exact_time\":false}";
        } else if (csv) {
            std::wcout << old_value.samples << L"," << new_value.samples << L","
                       << sample_delta << L"," << old_value.weight_ns << L","
                       << new_value.weight_ns << L"," << weight_delta << L","
                       << delta_pp << L"," << margin << L"," << EscapeCsv(key) << L"\n";
        } else {
            std::wcout << std::showpos << std::right << std::setw(13)
                       << sample_delta << std::noshowpos << std::setw(11)
                       << weight_delta / 1000 << L"  " << delta_pp << L"pp +/-" << margin << L"  " << std::setw(8)
                       << old_value.samples << std::setw(8)
                       << new_value.samples << L"  " << key << L"\n";
        }
    }
    if (json) {
        std::wcout << L"\n]\n";
    }
    return 0;
}


int RunAnnotate(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::wstring symbol_path;
    std::filesystem::path archive_directory;
    std::optional<uint32_t> pid;
    uint64_t limit = 50;
    bool json = false;
    bool csv = false;
    bool disasm = false;
    bool stdio_output = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-i" || arguments[index] == L"--input") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            input = arguments[++index];
        } else if (arguments[index] == L"--archive") {
            if(index+1>=arguments.size())return 2;
            archive_directory=arguments[++index];
        } else if (arguments[index] == L"--symbol-path") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            symbol_path = arguments[++index];
        } else if (arguments[index] == L"-p" || arguments[index] == L"--pid") {
            uint32_t value = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], value)) {
                return 2;
            }
            pid = value;
        } else if (arguments[index] == L"--disasm") {
            disasm = true;
        } else if (arguments[index] == L"--json") {
            json = true;
        } else if (arguments[index] == L"--csv") {
            csv = true;
        } else if (arguments[index] == L"--limit") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], limit)) {
                return 2;
            }
        } else if (arguments[index] == L"--stdio") {
            stdio_output = true;
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            PrintCommandHelp(L"annotate");
            return 0;
        } else {
            PrintError(L"unknown annotate option: " + arguments[index]);
            return 2;
        }
    }
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
    if (!ReadEtwFile(input, trace, &error)) {
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
    struct Heat {
        std::wstring module;
        std::wstring symbol;
        std::wstring source_file;
        std::wstring disassembly;
        const ImageRecord* image = nullptr;
        uint64_t address = 0;
        uint64_t displacement = 0;
        uint32_t line = 0;
        uint64_t samples = 0;
        uint64_t function_size=0;
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
            }
        }
        if (disassembly_failed && !DisassemblyAvailable()) {
            std::wcerr << L"wperf: disassembly support is not enabled\n";
        }
    }
    if (json) {
        std::wcout << L"[\n";
        bool first = true;
        for (size_t index = 0; index < rows.size() && index < limit; ++index) {
            if (!first) {
                std::wcout << L",\n";
            }
            first = false;
            std::wcout << L"{\"samples\":" << rows[index].samples
                       << L",\"module\":\"" << EscapeJson(rows[index].module)
                       << L"\",\"symbol\":\"" << EscapeJson(rows[index].symbol)
                       << L"\",\"displacement\":" << rows[index].displacement
                       << L",\"source_file\":\""
                       << EscapeJson(rows[index].source_file)
                       << L"\",\"line\":" << rows[index].line
                       << L",\"disassembly\":\""
                       << EscapeJson(rows[index].disassembly) << L"\"}";
        }
        std::wcout << L"\n]\n";
        return 0;
    }
    if (csv) {
        std::wcout << L"samples,module,symbol,displacement,source_file,line,disassembly\n";
        for (size_t index = 0; index < rows.size() && index < limit; ++index) {
            std::wcout << rows[index].samples << L"," << EscapeCsv(rows[index].module)
                       << L"," << EscapeCsv(rows[index].symbol) << L"," << rows[index].displacement
                       << L"," << EscapeCsv(rows[index].source_file) << L"," << rows[index].line
                       << L"," << EscapeCsv(rows[index].disassembly) << L"\n";
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

std::string JsonFileEscape(const std::wstring& value) {
    return ToUtf8(EscapeJson(value));
}

std::wstring Sha256File(const std::filesystem::path& path, std::wstring* error = nullptr) {
    const auto digest=FileSha256(path);
    if(digest.empty() && error)*error=L"cannot hash file: "+path.wstring();
    return digest;
}

int RunArchive(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::filesystem::path output;
    std::optional<uint32_t> pid;
    std::wstring symbol_path;
    uint64_t max_size_mb = 512;
    bool include_trace = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-i" || arguments[index] == L"--input") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            input = arguments[++index];
        } else if (arguments[index] == L"-o" || arguments[index] == L"--output") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            output = arguments[++index];
        } else if (arguments[index] == L"-p" || arguments[index] == L"--pid") {
            uint32_t value = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], value)) {
                return 2;
            }
            pid = value;
        } else if (arguments[index] == L"--symbol-path") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            symbol_path = arguments[++index];
        } else if (arguments[index] == L"--include-trace") {
            include_trace = true;
        } else if (arguments[index] == L"--max-size-mb") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], max_size_mb)) {
                return 2;
            }
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            PrintCommandHelp(L"archive");
            return 0;
        } else {
            PrintError(L"unknown archive option: " + arguments[index]);
            return 2;
        }
    }
    if (input.empty() || output.empty()) {
        PrintError(L"archive requires -i FILE and -o DIRECTORY");
        return 2;
    }
    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
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
            manifest << "{\n  \"trace\": \"" << JsonFileEscape(input.wstring())
                     << "\",\n  \"symbol_path\": \"" << JsonFileEscape(symbol_path)
                     << "\",\n  \"max_size_mb\": " << max_size_mb
                     << ",\n  \"entries\": [\n";
            for (size_t index = 0; index < entries.size(); ++index) {
                const auto& entry = entries[index];
                const std::wstring guid = entry.identity.has_pdb_guid
                                              ? FormatGuid(entry.identity.pdb_guid)
                                              : L"";
                manifest << "    {\"source\":\"" << JsonFileEscape(entry.source)
                         << "\",\"archived\":\"" << JsonFileEscape(entry.archived)
                         << "\",\"copied\":" << (entry.copied ? "true" : "false")
                         << ",\"reason\":\"" << JsonFileEscape(entry.reason)
                         << "\",\"size\":" << entry.size
                         << ",\"sha256\":\"" << JsonFileEscape(entry.sha256)
                         << "\",\"pe_timestamp\":" << entry.identity.timestamp
                         << ",\"pe_checksum\":" << entry.identity.checksum
                         << ",\"pe_size_of_image\":" << entry.identity.size_of_image
                         << ",\"machine\":" << entry.identity.machine
                         << ",\"pdb_guid\":\"" << JsonFileEscape(guid)
                         << "\",\"pdb_age\":" << entry.identity.pdb_age
                         << ",\"pdb_source\":\"" << JsonFileEscape(entry.pdb_source)
                         << "\",\"pdb_archived\":\"" << JsonFileEscape(entry.pdb_archived)
                         << "\",\"pdb_copied\":" << (entry.pdb_copied ? "true" : "false")
                         << ",\"pdb_sha256\":\"" << JsonFileEscape(entry.pdb_sha256)
                         << "\"}" << (index + 1 == entries.size() ? "\n" : ",\n");
            }
            manifest << "  ]\n}\n";
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

int RunBuildIdList(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-i" || arguments[index] == L"--input") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            input = arguments[++index];
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            PrintCommandHelp(L"buildid-list");
            return 0;
        } else {
            PrintError(L"unknown buildid-list option: " + arguments[index]);
            return 2;
        }
    }
    if (input.empty()) {
        PrintError(L"buildid-list requires -i FILE");
        return 2;
    }

    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }
    std::wcout << L"Base              Size      Details\n";
    for (const auto& image : trace.images) {
        if (image.path.empty()) {
            continue;
        }
        std::wcout << std::hex << std::setw(16) << std::setfill(L'0') << image.base
                   << L" " << std::setw(8) << image.size << std::dec
                   << std::setfill(L' ') << L" pid=" << image.pid
                   << L" timestamp=" << image.timestamp
                   << L" checksum=" << image.checksum << L" " << image.path << L"\n";
    }
    return 0;
}
}  // namespace wperf
