#include "symbolizer.h"

#include "common.h"

#include <dbghelp.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace wperf {
namespace {

std::wstring ToHex(uint64_t value) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%llx", static_cast<unsigned long long>(value));
    return buffer;
}

uint32_t ReadPeTimestamp(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    if (!ReadFile(file, &dos, sizeof(dos), &read, nullptr) ||
        read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(file);
        return 0;
    }
    if (SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(file);
        return 0;
    }
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadFile(file, &nt, sizeof(nt), &read, nullptr) ||
        read != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return nt.FileHeader.TimeDateStamp;
}
std::wstring FileNameForImage(const ImageRecord& image) {
    if (image.path.empty()) {
        return L"[unknown]";
    }
    return std::filesystem::path(image.path).filename().wstring();
}

}  // namespace

Symbolizer::~Symbolizer() {
    Close();
}

bool Symbolizer::Initialize(std::wstring symbol_path, std::wstring* error) {
    std::lock_guard lock(mutex_);
    if (initialized_) {
        return true;
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES |
                  SYMOPT_OMAP_FIND_NEAREST);
    if (!SymInitializeW(GetCurrentProcess(),
                        symbol_path.empty() ? nullptr : symbol_path.c_str(), FALSE)) {
        if (error != nullptr) {
            *error = L"SymInitializeW failed: " + FormatWindowsError(GetLastError());
        }
        return false;
    }
    initialized_ = true;
    return true;
}

std::wstring Symbolizer::NormalizePath(const std::wstring& path) const {
    std::wstring value = path;
    if (value.rfind(L"\\SystemRoot\\", 0) == 0) {
        const DWORD required = GetEnvironmentVariableW(L"SystemRoot", nullptr, 0);
        if (required > 0) {
            std::wstring system_root(required, L'\0');
            const DWORD written = GetEnvironmentVariableW(
                L"SystemRoot", system_root.data(), required);
            if (written > 0 && written < required) {
                system_root.resize(written);
                value = system_root + value.substr(11);
            }
        }
    }
    if (value.rfind(L"\\??\\", 0) == 0) {
        value.erase(0, 4);
    }
    if (value.rfind(L"\\Device\\", 0) == 0) {
        for (wchar_t drive = L'C'; drive <= L'Z'; ++drive) {
            const std::wstring drive_name{drive, L':'};
            wchar_t target[1024]{};
            if (QueryDosDeviceW(drive_name.c_str(), target,
                                static_cast<DWORD>(std::size(target))) == 0) {
                continue;
            }
            const std::wstring device(target);
            if (value.rfind(device + L"\\", 0) == 0) {
                value = drive_name + value.substr(device.size());
                break;
            }
        }
    }
    return value;
}

void Symbolizer::AddModule(const ImageRecord& image) {
    std::lock_guard lock(mutex_);
    if (!initialized_ || image.base == 0 || image.size == 0 || image.path.empty()) {
        return;
    }
    const std::wstring path = NormalizePath(image.path);
    const std::wstring key = path + L"|" + std::to_wstring(image.base) + L"|" +
                             std::to_wstring(image.size) + L"|" +
                             std::to_wstring(image.timestamp);
    if (!loaded_modules_.insert(key).second) {
        return;
    }

    const uint32_t file_timestamp = ReadPeTimestamp(path);
    if (image.timestamp != 0 && file_timestamp != 0 &&
        image.timestamp != file_timestamp) {
        rejected_bases_.insert(image.base);
        return;
    }

    DWORD64 loaded_base = SymLoadModuleExW(
        GetCurrentProcess(), nullptr, path.c_str(), nullptr, image.base,
        static_cast<DWORD>(std::min<uint64_t>(image.size, UINT32_MAX)),
        nullptr, 0);
    if (loaded_base == 0) {
        loaded_modules_.erase(key);
        rejected_bases_.insert(image.base);
    }
}

ResolvedSymbol Symbolizer::Resolve(const TraceData& trace, uint32_t pid,
                                   uint64_t address, uint64_t timestamp_ns) {
    ResolvedSymbol result;
    const ImageRecord* image = FindImage(trace, pid, address, timestamp_ns);
    result.module = image != nullptr ? FileNameForImage(*image) : L"[unknown]";
    if (image != nullptr) {
        result.displacement = address >= image->base ? address - image->base : 0;
    }

    std::lock_guard lock(mutex_);
    if (!initialized_ || image == nullptr ||
        rejected_bases_.find(image->base) != rejected_bases_.end()) {
        result.symbol = result.module == L"[unknown]"
                            ? L"[unknown]"
                            : result.module + L"+0x" + ToHex(result.displacement);
        return result;
    }

    BYTE buffer[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (SymFromAddrW(GetCurrentProcess(), address, &displacement, symbol) &&
        symbol->NameLen != 0) {
        result.symbol.assign(symbol->Name, symbol->NameLen);
        result.displacement = displacement;
        result.resolved = true;
    } else {
        result.symbol = result.module + L"+0x" + ToHex(result.displacement);
    }

    IMAGEHLP_LINEW64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    if (SymGetLineFromAddrW64(GetCurrentProcess(), address, &line_displacement, &line)) {
        result.source_file = line.FileName != nullptr ? line.FileName : L"";
        result.line = line.LineNumber;
    }
    return result;
}

void Symbolizer::Close() {
    std::lock_guard lock(mutex_);
    if (initialized_) {
        SymCleanup(GetCurrentProcess());
        initialized_ = false;
        loaded_modules_.clear();
    }
}

}  // namespace wperf
