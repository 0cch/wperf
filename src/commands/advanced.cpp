#include "commands/advanced.h"

#include "commands/util.h"
#include "common.h"
#include "etl_reader.h"
#include "symbolizer.h"
#include "trace_model.h"

#include <algorithm>
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

std::map<std::wstring, uint64_t> AggregateSymbols(const TraceData& trace,
                                                  Symbolizer& symbolizer,
                                                  std::optional<uint32_t> pid) {
    std::map<std::wstring, uint64_t> result;
    for (const auto& sample : trace.samples) {
        if (pid.has_value() && sample.pid != *pid) {
            continue;
        }
        const uint64_t address = sample.stack.empty() ? sample.instruction_pointer
                                                      : sample.stack.front();
        const ResolvedSymbol symbol =
            symbolizer.Resolve(trace, sample.pid, address, sample.timestamp_ns);
        const std::wstring key = symbol.module + L"|" + symbol.symbol;
        ++result[key];
    }
    return result;
}

int RunDiff(const std::vector<std::wstring>& arguments) {
    std::filesystem::path first;
    std::filesystem::path second;
    std::wstring symbol_path;
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
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf diff -1 FIRST.etl -2 SECOND.etl "
                          L"[--symbol-path DIR]\n";
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
    const auto before = AggregateSymbols(first_trace, first_symbolizer, std::nullopt);
    const auto after = AggregateSymbols(second_trace, second_symbolizer, std::nullopt);

    std::vector<std::wstring> keys;
    for (const auto& [key, value] : before) {
        (void)value;
        keys.push_back(key);
    }
    for (const auto& [key, value] : after) {
        (void)value;
        if (before.find(key) == before.end()) {
            keys.push_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());
    std::wcout << L"Delta Samples Symbol\n";
    for (const auto& key : keys) {
        const uint64_t old_count = before.count(key) ? before.at(key) : 0;
        const uint64_t new_count = after.count(key) ? after.at(key) : 0;
        if (old_count == new_count) {
            continue;
        }
        std::wcout << (new_count >= old_count ? L"+ " : L"- ")
                   << (new_count >= old_count ? new_count - old_count
                                              : old_count - new_count)
                   << L"  " << old_count << L" -> " << new_count << L"  "
                   << key << L"\n";
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
        } else if (arguments[index] == L"--limit") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], limit)) {
                return 2;
            }
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf annotate -i FILE [--symbol-path DIR] "
                          L"[--pid PID] [--limit N]\n";
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
        std::wstring symbol;
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
        value.symbol = symbol.symbol;
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
    std::wcout << L"Samples  Address heat\n";
    for (size_t index = 0; index < rows.size() && index < limit; ++index) {
        std::wcout << rows[index].samples << L"  " << rows[index].symbol << L"\n";
    }
    return 0;
}

int RunArchive(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::filesystem::path output;
    std::optional<uint32_t> pid;
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
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf archive -i FILE -o DIRECTORY [-p PID]\n";
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

    std::wofstream manifest(output / L"manifest.txt");
    for (const auto& path : needed_images) {
        const auto* image = FindImage(
            trace, pid.value_or(0), 0, 0);
        (void)image;
        const std::filesystem::path source = NormalizeArchivePath(path);
        manifest << path << L"\n";
        std::error_code ec;
        if (std::filesystem::exists(source, ec) &&
            std::filesystem::is_regular_file(source, ec)) {
            const std::filesystem::path destination =
                output / source.filename().wstring();
            std::filesystem::copy_file(source, destination,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            const std::filesystem::path pdb = source.parent_path() /
                                              (source.stem().wstring() + L".pdb");
            if (std::filesystem::exists(pdb, ec) &&
                std::filesystem::is_regular_file(pdb, ec)) {
                std::filesystem::copy_file(
                    pdb, output / pdb.filename(),
                    std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
    }
    manifest.close();
    std::wcout << L"archive: " << output.wstring() << L"\n";
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
