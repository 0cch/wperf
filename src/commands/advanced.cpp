#include "commands/advanced.h"

#include "commands/util.h"
#include "common.h"
#include "etl_reader.h"
#include "symbolizer.h"
#include "trace_model.h"

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace wperf {
using namespace commands;

struct AggregateValue {
    uint64_t samples = 0;
    uint64_t weight_ns = 0;
};

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring result;
    for (wchar_t ch : value) {
        if (ch == L'"' || ch == L'\\') {
            result.push_back(L'\\');
        }
        result.push_back(ch);
    }
    return result;
}

std::vector<uint64_t> SampleWeights(const TraceData& trace) {
    std::vector<uint64_t> weights(trace.samples.size(),
                                  trace.profile_interval_ns);
    std::unordered_map<uint32_t, std::vector<size_t>> by_cpu;
    for (size_t index = 0; index < trace.samples.size(); ++index) {
        by_cpu[trace.samples[index].cpu].push_back(index);
    }
    for (auto& [cpu, indices] : by_cpu) {
        (void)cpu;
        std::sort(indices.begin(), indices.end(), [&](size_t left, size_t right) {
            return trace.samples[left].timestamp_ns <
                   trace.samples[right].timestamp_ns;
        });
        for (size_t index = 0; index < indices.size(); ++index) {
            const size_t current = indices[index];
            uint64_t weight = trace.profile_interval_ns;
            if (index + 1 < indices.size()) {
                const uint64_t next = trace.samples[indices[index + 1]].timestamp_ns;
                const uint64_t now = trace.samples[current].timestamp_ns;
                if (next > now) {
                    weight = next - now;
                }
            }
            weights[current] = std::clamp<uint64_t>(weight, 1, 1000000000ull);
        }
    }
    return weights;
}

std::map<std::wstring, AggregateValue> AggregateSymbols(
    const TraceData& trace, Symbolizer& symbolizer,
    const std::optional<uint32_t>& pid, const std::wstring& dso_filter,
    bool calltree) {
    std::map<std::wstring, AggregateValue> result;
    const std::vector<uint64_t> weights = SampleWeights(trace);
    const std::wstring dso_lower = Lower(dso_filter);
    for (size_t index = 0; index < trace.samples.size(); ++index) {
        const auto& sample = trace.samples[index];
        if (pid.has_value() && sample.pid != *pid) {
            continue;
        }
        if (!calltree) {
            const uint64_t address = sample.stack.empty()
                                         ? sample.instruction_pointer
                                         : sample.stack.front();
            const ResolvedSymbol symbol =
                symbolizer.Resolve(trace, sample.pid, address, sample.timestamp_ns);
            if (!dso_lower.empty() &&
                Lower(symbol.module).find(dso_lower) == std::wstring::npos) {
                continue;
            }
            const std::wstring key = symbol.module + L"|" + symbol.symbol;
            auto& value = result[key];
            ++value.samples;
            value.weight_ns += weights[index];
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
            key += symbol.module + L"!" + symbol.symbol;
        }
        if (!leaf_matches) {
            continue;
        }
        auto& value = result[key];
        ++value.samples;
        value.weight_ns += weights[index];
    }
    return result;
}

int RunDiff(const std::vector<std::wstring>& arguments) {
    std::filesystem::path first;
    std::filesystem::path second;
    std::wstring symbol_path;
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
            std::wcout << L"Usage: wperf diff -1 FIRST.etl -2 SECOND.etl "
                          L"[--symbol-path DIR] [--pid PID] [--dso TEXT] "
                          L"[--calltree] [--json|--csv]\n";
            return 0;
        }
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
    Symbolizer first_symbolizer;
    Symbolizer second_symbolizer;
    first_symbolizer.Initialize(symbol_path, nullptr);
    second_symbolizer.Initialize(symbol_path, nullptr);
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
                      L"weight_before_ns,weight_after_ns,delta_weight_ns,key\n";
    } else {
        std::wcout << L"DeltaSamples  DeltaUs  Before  After  Key\n";
    }
    bool first_row = true;
    for (const auto& key : keys) {
        const AggregateValue old_value = before.count(key) ? before.at(key)
                                                           : AggregateValue{};
        const AggregateValue new_value = after.count(key) ? after.at(key)
                                                          : AggregateValue{};
        if (old_value.samples == new_value.samples &&
            old_value.weight_ns == new_value.weight_ns) {
            continue;
        }
        const int64_t sample_delta = static_cast<int64_t>(new_value.samples) -
                                     static_cast<int64_t>(old_value.samples);
        const int64_t weight_delta = static_cast<int64_t>(new_value.weight_ns) -
                                     static_cast<int64_t>(old_value.weight_ns);
        if (json) {
            if (!first_row) {
                std::wcout << L",\n";
            }
            first_row = false;
            std::wcout << L"{\"key\":\"" << JsonEscape(key)
                       << L"\",\"samples_before\":" << old_value.samples
                       << L",\"samples_after\":" << new_value.samples
                       << L",\"delta_samples\":" << sample_delta
                       << L",\"weight_before_ns\":" << old_value.weight_ns
                       << L",\"weight_after_ns\":" << new_value.weight_ns
                       << L",\"delta_weight_ns\":" << weight_delta << L"}";
        } else if (csv) {
            std::wcout << old_value.samples << L"," << new_value.samples << L","
                       << sample_delta << L"," << old_value.weight_ns << L","
                       << new_value.weight_ns << L"," << weight_delta << L",\""
                       << key << L"\"\n";
        } else {
            std::wcout << (sample_delta >= 0 ? L"+" : L"") << sample_delta
                       << L"  " << (weight_delta >= 0 ? L"+" : L"")
                       << weight_delta / 1000 << L"us  " << old_value.samples
                       << L" -> " << new_value.samples << L"  " << key << L"\n";
        }
    }
    if (json) {
        std::wcout << L"\n]\n";
    }
    return 0;
}

std::filesystem::path NormalizeArchivePath(const std::wstring& value) {
    std::wstring path = value;
    if (path.rfind(L"\\SystemRoot\\", 0) == 0) {
        wchar_t system_root[32768]{};
        const DWORD length = GetEnvironmentVariableW(
            L"SystemRoot", system_root, static_cast<DWORD>(std::size(system_root)));
        if (length != 0) {
            path = std::wstring(system_root, length) + path.substr(11);
        }
    }
    if (path.rfind(L"\\??\\", 0) == 0) {
        path.erase(0, 4);
    }
    if (path.rfind(L"\\Device\\", 0) == 0) {
        for (wchar_t drive = L'C'; drive <= L'Z'; ++drive) {
            const std::wstring drive_name{drive, L':'};
            wchar_t device[1024]{};
            if (QueryDosDeviceW(drive_name.c_str(), device,
                                static_cast<DWORD>(std::size(device))) != 0) {
                const std::wstring prefix = std::wstring(device) + L"\\";
                if (path.rfind(prefix, 0) == 0) {
                    path = drive_name + path.substr(prefix.size() - 1);
                    break;
                }
            }
        }
    }
    return path;
}

int RunAnnotate(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::wstring symbol_path;
    std::optional<uint32_t> pid;
    uint64_t limit = 50;
    bool json = false;
    bool csv = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-i" || arguments[index] == L"--input") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            input = arguments[++index];
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
        } else if (arguments[index] == L"--json") {
            json = true;
        } else if (arguments[index] == L"--csv") {
            csv = true;
        } else if (arguments[index] == L"--limit") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], limit)) {
                return 2;
            }
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf annotate -i FILE [--symbol-path DIR] "
                          L"[--pid PID] [--limit N] [--json|--csv]\n";
            return 0;
        }
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
    Symbolizer symbolizer;
    symbolizer.Initialize(symbol_path, nullptr);
    for (const auto& image : trace.images) {
        symbolizer.AddModule(image);
    }
    struct Heat {
        std::wstring module;
        std::wstring symbol;
        std::wstring source_file;
        uint64_t displacement = 0;
        uint32_t line = 0;
        uint64_t samples = 0;
    };
    std::map<std::wstring, Heat> heat;
    for (const auto& sample : trace.samples) {
        if (pid.has_value() && sample.pid != *pid) {
            continue;
        }
        const ResolvedSymbol symbol = symbolizer.Resolve(
            trace, sample.pid, sample.instruction_pointer, sample.timestamp_ns);
        const std::wstring key = symbol.module + L"!" + symbol.symbol + L"+0x" +
                                 std::to_wstring(symbol.displacement);
        auto& value = heat[key];
        value.module = symbol.module;
        value.symbol = symbol.symbol;
        value.source_file = symbol.source_file;
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
        return a.samples > b.samples;
    });
    if (json) {
        std::wcout << L"[\n";
        bool first = true;
        for (size_t index = 0; index < rows.size() && index < limit; ++index) {
            if (!first) {
                std::wcout << L",\n";
            }
            first = false;
            std::wcout << L"{\"samples\":" << rows[index].samples
                       << L",\"module\":\"" << JsonEscape(rows[index].module)
                       << L"\",\"symbol\":\"" << JsonEscape(rows[index].symbol)
                       << L"\",\"displacement\":" << rows[index].displacement
                       << L",\"source_file\":\""
                       << JsonEscape(rows[index].source_file)
                       << L"\",\"line\":" << rows[index].line << L"}";
        }
        std::wcout << L"\n]\n";
        return 0;
    }
    if (csv) {
        std::wcout << L"samples,module,symbol,displacement,source_file,line\n";
        for (size_t index = 0; index < rows.size() && index < limit; ++index) {
            std::wcout << rows[index].samples << L",\"" << rows[index].module
                       << L"\",\"" << rows[index].symbol << L"\","
                       << rows[index].displacement << L",\""
                       << rows[index].source_file << L"\"," << rows[index].line
                       << L"\n";
        }
        return 0;
    }
    std::wcout << L"Samples  Module  Symbol  Source  Line\n";
    for (size_t index = 0; index < rows.size() && index < limit; ++index) {
        std::wcout << rows[index].samples << L"  " << rows[index].module << L"!"
                   << rows[index].symbol << L"+0x" << std::hex
                   << rows[index].displacement << std::dec;
        if (!rows[index].source_file.empty()) {
            std::wcout << L"  " << rows[index].source_file << L":" << rows[index].line;
        }
        std::wcout << L"\n";
    }
    return 0;
}

std::string JsonFileEscape(const std::wstring& value) {
    std::string result = ToUtf8(value);
    std::string escaped;
    escaped.reserve(result.size() + 8);
    for (char ch : result) {
        if (ch == '\\' || ch == '"') {
            escaped.push_back('\\');
        }
        if (ch == '\n') {
            escaped += "\\n";
        } else if (ch == '\r') {
            escaped += "\\r";
        } else if (ch == '\t') {
            escaped += "\\t";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

std::wstring Sha256File(const std::filesystem::path& path,
                         std::wstring* error = nullptr) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD result_size = 0;
    std::vector<BYTE> hash_object;
    std::vector<BYTE> digest;
    std::wstring result;
    do {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                        nullptr, 0) < 0 ||
            BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&object_size),
                              sizeof(object_size), &result_size, 0) < 0 ||
            BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                              reinterpret_cast<PUCHAR>(&hash_size),
                              sizeof(hash_size), &result_size, 0) < 0) {
            break;
        }
        hash_object.resize(object_size);
        digest.resize(hash_size);
        if (BCryptCreateHash(algorithm, &hash, hash_object.data(), object_size,
                             nullptr, 0, 0) < 0) {
            break;
        }
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            break;
        }
        std::vector<char> buffer(1024 * 1024);
        while (stream) {
            stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = stream.gcount();
            if (count > 0 &&
                BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                               static_cast<ULONG>(count), 0) < 0) {
                stream.setstate(std::ios::failbit);
                break;
            }
        }
        if (stream.bad() || BCryptFinishHash(hash, digest.data(), hash_size, 0) < 0) {
            break;
        }
        for (BYTE byte : digest) {
            wchar_t text[3]{};
            swprintf_s(text, L"%02X", byte);
            result += text;
        }
    } while (false);

    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    if (result.empty() && error != nullptr) {
        *error = L"SHA256 failed for " + path.wstring();
    }
    return result;
}

int RunArchive(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::filesystem::path output;
    std::optional<uint32_t> pid;
    std::wstring symbol_path;
    uint64_t max_size_mb = 512;
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
        } else if (arguments[index] == L"--max-size-mb") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], max_size_mb)) {
                return 2;
            }
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf archive -i FILE -o DIRECTORY [-p PID] "
                          L"[--symbol-path DIR] [--max-size-mb N]\n";
            return 0;
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
    std::set<std::wstring> needed_images;
    for (const auto& sample : trace.samples) {
        if (pid.has_value() && sample.pid != *pid) {
            continue;
        }
        if (const auto* image = FindImage(trace, sample.pid,
                                          sample.instruction_pointer,
                                          sample.timestamp_ns)) {
            if (!image->path.empty()) {
                needed_images.insert(image->path);
            }
        }
        for (uint64_t frame : sample.stack) {
            if (const auto* image =
                    FindImage(trace, sample.pid, frame, sample.timestamp_ns)) {
                if (!image->path.empty()) {
                    needed_images.insert(image->path);
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
    const uint64_t max_bytes = max_size_mb == 0
                                   ? UINT64_MAX
                                   : max_size_mb * 1024ull * 1024ull;
    for (const auto& path : needed_images) {
        ArchiveEntry entry;
        entry.source = path;
        const std::filesystem::path source = NormalizeArchivePath(path);
        std::error_code ec;
        if (!std::filesystem::exists(source, ec) ||
            !std::filesystem::is_regular_file(source, ec)) {
            entry.reason = L"missing";
            entries.push_back(std::move(entry));
            continue;
        }
        entry.size = std::filesystem::file_size(source, ec);
        entry.identity = ReadModuleIdentity(source.wstring());
        if (entry.size > max_bytes) {
            entry.reason = L"oversized";
            entries.push_back(std::move(entry));
            continue;
        }
        std::filesystem::path destination = output / source.filename().wstring();
        if (std::filesystem::exists(destination, ec) &&
            !entry.identity.valid == false) {
            destination = output /
                          (source.stem().wstring() + L"." +
                           std::to_wstring(entry.identity.timestamp) +
                           source.extension().wstring());
        }
        std::filesystem::copy_file(source, destination,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        if (ec) {
            entry.reason = L"copy_failed";
            entries.push_back(std::move(entry));
            continue;
        }
        entry.archived = destination.filename().wstring();
        entry.copied = true;
        entry.sha256 = Sha256File(destination);

        const std::filesystem::path pdb = source.parent_path() /
                                          (source.stem().wstring() + L".pdb");
        entry.pdb_source = pdb.wstring();
        if (std::filesystem::exists(pdb, ec) &&
            std::filesystem::is_regular_file(pdb, ec) &&
            std::filesystem::file_size(pdb, ec) <= max_bytes) {
            const std::filesystem::path pdb_destination = output / pdb.filename();
            std::filesystem::copy_file(
                pdb, pdb_destination,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) {
                entry.pdb_archived = pdb_destination.filename().wstring();
                entry.pdb_copied = true;
                entry.pdb_sha256 = Sha256File(pdb_destination);
            }
        }
        entries.push_back(std::move(entry));
    }

    auto write_manifest = [&](const std::filesystem::path& path, bool json,
                              bool csv) {
        std::ofstream manifest(path, std::ios::binary);
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
                manifest << ToUtf8(entry.source) << ','
                         << ToUtf8(entry.archived) << ','
                         << (entry.copied ? "true" : "false") << ','
                         << ToUtf8(entry.reason) << ',' << entry.size << ','
                         << ToUtf8(entry.sha256) << ',' << entry.identity.timestamp
                         << ',' << entry.identity.checksum << ','
                         << entry.identity.size_of_image << ','
                         << entry.identity.machine << ',' << ToUtf8(guid) << ','
                         << entry.identity.pdb_age << ','
                         << ToUtf8(entry.pdb_source) << ','
                         << ToUtf8(entry.pdb_archived) << ','
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
            std::wcout << L"Usage: wperf buildid-list -i FILE\n";
            return 0;
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