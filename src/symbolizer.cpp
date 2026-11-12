#include "symbolizer.h"

#include "common.h"

#include <dbghelp.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

namespace wperf {
namespace {

struct SectionRange {
    DWORD virtual_address = 0;
    DWORD virtual_size = 0;
    DWORD raw_size = 0;
    DWORD raw_offset = 0;
};

struct PeImageIdentity {
    bool valid = false;
    uint32_t timestamp = 0;
    uint32_t checksum = 0;
    uint32_t size_of_image = 0;
    uint16_t machine = 0;
    GUID pdb_guid{};
    uint32_t pdb_age = 0;
    bool has_pdb_guid = false;
};

std::wstring ToHex(uint64_t value) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%llx", static_cast<unsigned long long>(value));
    return buffer;
}

bool ReadFileAt(HANDLE file, uint64_t offset, void* data, size_t size) {
    if (size > std::numeric_limits<DWORD>::max()) {
        return false;
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        return false;
    }
    DWORD read = 0;
    return ReadFile(file, data, static_cast<DWORD>(size), &read, nullptr) != FALSE &&
           read == size;
}

bool IsZeroGuid(const GUID& guid) {
    static const GUID zero{};
    return IsEqualGUID(guid, zero) != FALSE;
}

uint32_t RvaToFileOffset(uint32_t rva,
                         const std::vector<SectionRange>& sections) {
    for (const SectionRange& section : sections) {
        const uint32_t span = std::max(section.virtual_size, section.raw_size);
        if (rva >= section.virtual_address &&
            rva - section.virtual_address < span) {
            return section.raw_offset + (rva - section.virtual_address);
        }
    }
    return UINT32_MAX;
}

bool ReadPeImageIdentity(const std::wstring& path, PeImageIdentity& identity) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool success = false;
    do {
        IMAGE_DOS_HEADER dos{};
        if (!ReadFileAt(file, 0, &dos, sizeof(dos)) ||
            dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
            break;
        }

        const uint32_t nt_offset = static_cast<uint32_t>(dos.e_lfanew);
        DWORD signature = 0;
        IMAGE_FILE_HEADER file_header{};
        if (!ReadFileAt(file, nt_offset, &signature, sizeof(signature)) ||
            signature != IMAGE_NT_SIGNATURE ||
            !ReadFileAt(file, nt_offset + sizeof(signature), &file_header,
                        sizeof(file_header))) {
            break;
        }

        std::vector<BYTE> optional(file_header.SizeOfOptionalHeader);
        if (optional.size() < sizeof(WORD) ||
            !ReadFileAt(file, nt_offset + sizeof(signature) +
                                  sizeof(file_header),
                        optional.data(), optional.size())) {
            break;
        }

        WORD magic = 0;
        std::memcpy(&magic, optional.data(), sizeof(magic));
        std::vector<SectionRange> sections;
        uint32_t debug_rva = 0;
        uint32_t debug_size = 0;
        if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
            optional.size() >= sizeof(IMAGE_OPTIONAL_HEADER32)) {
            IMAGE_OPTIONAL_HEADER32 header{};
            std::memcpy(&header, optional.data(), sizeof(header));
            identity.timestamp = file_header.TimeDateStamp;
            identity.checksum = header.CheckSum;
            identity.size_of_image = header.SizeOfImage;
            identity.machine = file_header.Machine;
            if (header.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DEBUG) {
                debug_rva = header.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]
                                .VirtualAddress;
                debug_size = header.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]
                                 .Size;
            }
        } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                   optional.size() >= sizeof(IMAGE_OPTIONAL_HEADER64)) {
            IMAGE_OPTIONAL_HEADER64 header{};
            std::memcpy(&header, optional.data(), sizeof(header));
            identity.timestamp = file_header.TimeDateStamp;
            identity.checksum = header.CheckSum;
            identity.size_of_image = header.SizeOfImage;
            identity.machine = file_header.Machine;
            if (header.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DEBUG) {
                debug_rva = header.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]
                                .VirtualAddress;
                debug_size = header.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]
                                 .Size;
            }
        } else {
            break;
        }

        if (file_header.NumberOfSections > 96) {
            break;
        }
        const uint32_t section_offset = nt_offset + sizeof(signature) +
                                        sizeof(file_header) +
                                        file_header.SizeOfOptionalHeader;
        for (WORD index = 0; index < file_header.NumberOfSections; ++index) {
            IMAGE_SECTION_HEADER section{};
            if (!ReadFileAt(file, section_offset +
                                      static_cast<uint32_t>(index) *
                                          sizeof(section),
                            &section, sizeof(section))) {
                break;
            }
            sections.push_back(SectionRange{
                section.VirtualAddress, section.Misc.VirtualSize,
                section.SizeOfRawData, section.PointerToRawData});
        }

        const uint32_t debug_offset = RvaToFileOffset(debug_rva, sections);
        const uint32_t entries =
            std::min<uint32_t>(debug_size / sizeof(IMAGE_DEBUG_DIRECTORY), 64);
        if (debug_offset != UINT32_MAX) {
            for (uint32_t index = 0; index < entries; ++index) {
                IMAGE_DEBUG_DIRECTORY debug{};
                if (!ReadFileAt(file, debug_offset +
                                          index * sizeof(debug),
                                &debug, sizeof(debug))) {
                    break;
                }
                if (debug.Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
                    debug.SizeOfData < 24 || debug.SizeOfData > 1024 * 1024) {
                    continue;
                }
                std::vector<BYTE> codeview(debug.SizeOfData);
                if (!ReadFileAt(file, debug.PointerToRawData, codeview.data(),
                                codeview.size())) {
                    continue;
                }
                constexpr DWORD kRsdsSignature = 0x53445352;
                DWORD codeview_signature = 0;
                std::memcpy(&codeview_signature, codeview.data(),
                            sizeof(codeview_signature));
                if (codeview_signature == kRsdsSignature) {
                    std::memcpy(&identity.pdb_guid, codeview.data() + 4,
                                sizeof(identity.pdb_guid));
                    std::memcpy(&identity.pdb_age, codeview.data() + 20,
                                sizeof(identity.pdb_age));
                    identity.has_pdb_guid = !IsZeroGuid(identity.pdb_guid);
                }
            }
        }

        identity.valid = true;
        success = true;
    } while (false);

    CloseHandle(file);
    return success;
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
                  SYMOPT_OMAP_FIND_NEAREST | SYMOPT_EXACT_SYMBOLS |
                  SYMOPT_FAIL_CRITICAL_ERRORS);
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

std::wstring Symbolizer::ModuleKey(const ImageRecord& image,
                                   const std::wstring& normalized_path) const {
    return normalized_path + L"|" + std::to_wstring(image.pid) + L"|" +
           std::to_wstring(image.base) + L"|" + std::to_wstring(image.size) +
           L"|" + std::to_wstring(image.timestamp) + L"|" +
           std::to_wstring(image.checksum);
}

void Symbolizer::AddModule(const ImageRecord& image) {
    std::lock_guard lock(mutex_);
    if (!initialized_ || image.base == 0 || image.size == 0 || image.path.empty()) {
        return;
    }
    const std::wstring path = NormalizePath(image.path);
    const std::wstring key = ModuleKey(image, path);
    if (loaded_modules_.find(key) != loaded_modules_.end() ||
        rejected_modules_.find(key) != rejected_modules_.end()) {
        return;
    }

    PeImageIdentity identity;
    if (!ReadPeImageIdentity(path, identity) ||
        (image.timestamp != 0 && identity.timestamp != 0 &&
         image.timestamp != identity.timestamp) ||
        (image.checksum != 0 && identity.checksum != 0 &&
         image.checksum != identity.checksum)) {
        rejected_modules_.insert(key);
        return;
    }

    const uint64_t aligned_size =
        std::max<uint64_t>((image.size + 0xFFFFull) & ~0xFFFFull, 0x10000ull);
    const DWORD64 synthetic_base = next_synthetic_base_;
    next_synthetic_base_ += aligned_size;
    const DWORD64 loaded_base = SymLoadModuleExW(
        GetCurrentProcess(), nullptr, path.c_str(), nullptr, synthetic_base,
        static_cast<DWORD>(std::min<uint64_t>(image.size, UINT32_MAX)), nullptr,
        0);
    if (loaded_base == 0) {
        rejected_modules_.insert(key);
        return;
    }

    IMAGEHLP_MODULEW64 module_info{};
    module_info.SizeOfStruct = sizeof(module_info);
    const bool have_module_info =
        SymGetModuleInfoW64(GetCurrentProcess(), loaded_base, &module_info) != FALSE;
    const bool identity_mismatch =
        have_module_info &&
        (module_info.PdbUnmatched != FALSE || module_info.DbgUnmatched != FALSE ||
         (identity.has_pdb_guid && !IsZeroGuid(module_info.PdbSig70) &&
          !IsEqualGUID(identity.pdb_guid, module_info.PdbSig70)) ||
         (identity.has_pdb_guid && module_info.PdbAge != 0 &&
          identity.pdb_age != module_info.PdbAge));
    if (identity_mismatch) {
        SymUnloadModule64(GetCurrentProcess(), loaded_base);
        rejected_modules_.insert(key);
        return;
    }

    loaded_modules_.emplace(key, ModuleLoad{loaded_base});
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
    const std::wstring key =
        image != nullptr ? ModuleKey(*image, NormalizePath(image->path)) : L"";
    auto module = loaded_modules_.find(key);
    if (!initialized_ || image == nullptr || module == loaded_modules_.end()) {
        result.symbol = result.module == L"[unknown]"
                            ? L"[unknown]"
                            : result.module + L"+0x" + ToHex(result.displacement);
        return result;
    }

    const uint64_t symbol_address = module->second.synthetic_base + result.displacement;
    BYTE buffer[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (SymFromAddrW(GetCurrentProcess(), symbol_address, &displacement, symbol) &&
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
    if (SymGetLineFromAddrW64(GetCurrentProcess(), symbol_address, &line_displacement,
                              &line)) {
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
    }
    loaded_modules_.clear();
    rejected_modules_.clear();
    next_synthetic_base_ = 0x100000000ull;
}

}  // namespace wperf