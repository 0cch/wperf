#include "symbolizer.h"

#include "common.h"

#include <dbghelp.h>
#include <combaseapi.h>
#include <winver.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <limits>
#include <vector>

namespace wperf {
namespace {

// Every DbgHelp entry point is single-threaded, including distinct sessions.
std::mutex g_dbghelp_mutex;

bool StartsWithIgnoreCase(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() &&
           _wcsnicmp(value.data(), prefix.data(), prefix.size()) == 0;
}

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

std::wstring LoadedModulePath(const wchar_t* module_name) {
    HMODULE module = GetModuleHandleW(module_name);
    if (module == nullptr) return {};
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return buffer;
}

std::wstring FileVersionString(const std::wstring& path) {
    if (path.empty()) return {};
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info),
                        &info_size) ||
        info == nullptr || info_size < sizeof(VS_FIXEDFILEINFO)) {
        return {};
    }
    wchar_t text[128]{};
    swprintf_s(text, L"%u.%u.%u.%u",
               HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
               HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return text;
}

}  // namespace

ModuleIdentity PublicIdentity(const PeImageIdentity& identity) {
    return {identity.valid, identity.timestamp, identity.checksum, identity.size_of_image,
            identity.machine, identity.pdb_guid, identity.pdb_age, identity.has_pdb_guid};
}

ModuleIdentity ReadModuleIdentity(const std::wstring& path) {
    PeImageIdentity identity;
    return ReadPeImageIdentity(path, identity) ? PublicIdentity(identity) : ModuleIdentity{};
}

Symbolizer::~Symbolizer() {
    Close();
}

bool Symbolizer::Initialize(std::wstring symbol_path, std::wstring* error, std::filesystem::path cache_directory) {
    std::scoped_lock lock(mutex_, g_dbghelp_mutex);
    if (initialized_) {
        return true;
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES |
                  SYMOPT_OMAP_FIND_NEAREST | SYMOPT_EXACT_SYMBOLS |
                  SYMOPT_FAIL_CRITICAL_ERRORS);
    session_key_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (session_key_ == nullptr) {
        if (error) *error = L"cannot allocate symbol session: " + FormatWindowsError(GetLastError());
        return false;
    }
    explicit_symbol_path_ = symbol_path;
    const DWORD required = GetEnvironmentVariableW(L"_NT_SYMBOL_PATH", nullptr, 0);
    if (required != 0) {
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(
            L"_NT_SYMBOL_PATH", value.data(), required);
        if (written != 0 && written < required) {
            value.resize(written);
            environment_symbol_path_ = std::move(value);
        }
    }
    if (!explicit_symbol_path_.empty() && !environment_symbol_path_.empty()) {
        symbol_path_ = explicit_symbol_path_ + L";" + environment_symbol_path_;
        symbol_path_source_ = L"option+environment";
    } else if (!explicit_symbol_path_.empty()) {
        symbol_path_ = explicit_symbol_path_;
        symbol_path_source_ = L"option";
    } else if (!environment_symbol_path_.empty()) {
        symbol_path_ = environment_symbol_path_;
        symbol_path_source_ = L"environment";
    } else {
        symbol_path_.clear();
        symbol_path_source_ = L"empty";
    }
    if (!SymInitializeW(session_key_,
                        symbol_path_.empty() ? nullptr : symbol_path_.c_str(), FALSE)) {
        if (error != nullptr) {
            *error = L"SymInitializeW failed: " + FormatWindowsError(GetLastError());
        }
        CloseHandle(session_key_);
        session_key_ = nullptr;
        return false;
    }
    cache_directory_ = cache_directory.empty() ? GetWperfRootDirectory()/L".symcache" : std::move(cache_directory);
    dbghelp_path_ = LoadedModulePath(L"dbghelp.dll");
    dbghelp_version_ = FileVersionString(dbghelp_path_);
    initialized_ = true;
    return true;
}

std::wstring NormalizeModulePath(const std::wstring& path) {
    std::wstring value = path;
    if (StartsWithIgnoreCase(value, L"\\SystemRoot\\")) {
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
    if (StartsWithIgnoreCase(value, L"\\??\\")) {
        value.erase(0, 4);
    }
    if (StartsWithIgnoreCase(value, L"\\Device\\")) {
        for (wchar_t drive = L'C'; drive <= L'Z'; ++drive) {
            const std::wstring drive_name{drive, L':'};
            wchar_t target[1024]{};
            if (QueryDosDeviceW(drive_name.c_str(), target,
                                static_cast<DWORD>(std::size(target))) == 0) {
                continue;
            }
            const std::wstring device(target);
            if (StartsWithIgnoreCase(value, device + L"\\")) {
                value = drive_name + value.substr(device.size());
                break;
            }
        }
    }
    return value;
}

std::wstring Symbolizer::NormalizePath(const std::wstring& path) const {
    return NormalizeModulePath(path);
}

namespace {
size_t SymbolBytes(const ResolvedSymbol& symbol) {
    size_t bytes=sizeof(symbol)+2*(symbol.module.size()+symbol.symbol.size()+symbol.source_file.size()+
                                symbol.module_path.size()+symbol.module_identity.size());
    for(const auto& frame:symbol.inline_frames)bytes+=sizeof(frame)+2*(frame.symbol.size()+frame.source_file.size());
    return bytes;
}
constexpr size_t kSymbolCacheBudget=32*1024*1024;
}

bool ModuleIdentityMatches(const ImageRecord& image, const ModuleIdentity& identity) {
    return identity.valid &&
           (image.timestamp == 0 || image.timestamp == identity.timestamp) &&
           (image.checksum == 0 || image.checksum == identity.checksum) &&
           (image.size == 0 || image.size == identity.size_of_image);
}

bool PdbIdentityMatches(const std::wstring& path, const ModuleIdentity& identity) {
    if (!identity.has_pdb_guid) return false;
    std::lock_guard lock(g_dbghelp_mutex);
    SYMSRV_INDEX_INFOW info{};
    info.sizeofstruct = sizeof(info);
    return SymSrvGetFileIndexInfoW(path.c_str(), &info, 0) != FALSE &&
           IsEqualGUID(identity.pdb_guid, info.guid) && identity.pdb_age == info.age;
}

void Symbolizer::PrepareTrace(const TraceData& trace, bool discard_unused) {
    std::scoped_lock lock(mutex_, g_dbghelp_mutex);
    image_index_.Build(trace);
    indexed_trace_ = &trace;
    indexed_images_ = trace.images.data();
    indexed_image_count_ = trace.images.size();
    if (!discard_unused) return;
    std::unordered_set<std::wstring> needed;
    for (const auto& image : trace.images) needed.insert(ModuleKey(image, NormalizePath(image.path)));
    bool removed = false;
    for (auto it = loaded_modules_.begin(); it != loaded_modules_.end();) {
        if (!needed.contains(it->first)) {
            WriteCache(it->second);
            SymUnloadModule64(session_key_, it->second.synthetic_base);
            it = loaded_modules_.erase(it);
            removed = true;
        } else ++it;
    }
    std::erase_if(rejected_modules_, [&](const auto& key) { return !needed.contains(key); });
    if (removed) { symbol_cache_.clear(); symbol_cache_bytes_=0;
        persistent_cache_bytes_=0;
        for(const auto& [key,module]:loaded_modules_) { (void)key; for(const auto& [rva,value]:module.persistent_symbols) { (void)rva; persistent_cache_bytes_+=SymbolBytes(value); } }
    }
}

std::wstring ModuleFilePath(const ImageRecord& image) {
    return image.resolved_path.empty() ? NormalizeModulePath(image.path) : image.resolved_path;
}

std::wstring Symbolizer::ModuleKey(const ImageRecord& image,
                                   const std::wstring& normalized_path) const {
    return normalized_path + L"|" + std::to_wstring(image.size) +
           L"|" + std::to_wstring(image.timestamp) + L"|" +
           std::to_wstring(image.checksum);
}

void Symbolizer::AddModule(const ImageRecord& image) {
    std::scoped_lock lock(mutex_, g_dbghelp_mutex);
    if (!initialized_ || image.base == 0 || image.size == 0 || image.path.empty()) {
        return;
    }
    const std::wstring path = ModuleFilePath(image);
    const std::wstring key = ModuleKey(image, NormalizePath(image.path));
    if (loaded_modules_.find(key) != loaded_modules_.end() ||
        rejected_modules_.find(key) != rejected_modules_.end()) {
        return;
    }

    auto [diagnostic, inserted] = module_diagnostics_.try_emplace(key);
    if (inserted) {
        diagnostic->second.module = FileNameForImage(image);
        diagnostic->second.path = path;
        diagnostic->second.recorded_path = image.path;
        diagnostic->second.status = L"pending";
        ++stats_.modules_total;
    }
    auto& module_diagnostic = diagnostic->second;

    PeImageIdentity identity;
    if (!ReadPeImageIdentity(path, identity)) {
        std::error_code error;
        module_diagnostic.status = L"rejected";
        module_diagnostic.reason = std::filesystem::exists(path, error)
                                      ? L"cannot-read-local-pe"
                                      : L"local-file-missing";
        rejected_modules_.insert(key);
        ++stats_.modules_rejected;
        return;
    }
    if (!ModuleIdentityMatches(image, PublicIdentity(identity))) {
        module_diagnostic.status = L"rejected";
        module_diagnostic.reason = L"pe-identity-mismatch";
        rejected_modules_.insert(key);
        ++stats_.modules_rejected;
        return;
    }

    const uint64_t aligned_size =
        std::max<uint64_t>((image.size + 0xFFFFull) & ~0xFFFFull, 0x10000ull);
    const DWORD64 synthetic_base = next_synthetic_base_;
    next_synthetic_base_ += aligned_size;
    const DWORD64 loaded_base = SymLoadModuleExW(
        session_key_, nullptr, path.c_str(), nullptr, synthetic_base,
        static_cast<DWORD>(std::min<uint64_t>(image.size, UINT32_MAX)), nullptr,
        0);
    if (loaded_base == 0) {
        module_diagnostic.status = L"rejected";
        module_diagnostic.reason = L"dbghelp-load-failed: " +
                                   FormatWindowsError(GetLastError());
        rejected_modules_.insert(key);
        ++stats_.modules_rejected;
        return;
    }

    IMAGEHLP_MODULEW64 module_info{};
    module_info.SizeOfStruct = sizeof(module_info);
    const bool have_module_info =
        SymGetModuleInfoW64(session_key_, loaded_base, &module_info) != FALSE;
    if (have_module_info) {
        if (module_info.LoadedPdbName != nullptr && module_info.LoadedPdbName[0] != L'\0') {
            module_diagnostic.pdb_path = module_info.LoadedPdbName;
            module_diagnostic.pdb_status = L"loaded";
        } else {
            module_diagnostic.pdb_status = L"not-loaded";
        }
    } else {
        module_diagnostic.pdb_status = L"unknown";
    }
    const bool identity_mismatch =
        have_module_info &&
        (module_info.PdbUnmatched != FALSE || module_info.DbgUnmatched != FALSE ||
         (identity.has_pdb_guid && !IsZeroGuid(module_info.PdbSig70) &&
          !IsEqualGUID(identity.pdb_guid, module_info.PdbSig70)) ||
         (identity.has_pdb_guid && module_info.PdbAge != 0 &&
          identity.pdb_age != module_info.PdbAge));
    if (identity_mismatch) {
        SymUnloadModule64(session_key_, loaded_base);
        module_diagnostic.status = L"rejected";
        module_diagnostic.reason = L"pdb-identity-mismatch";
        module_diagnostic.pdb_status = L"mismatched";
        rejected_modules_.insert(key);
        ++stats_.modules_rejected;
        return;
    }

    ModuleLoad module;
    module.synthetic_base=loaded_base;
    const auto pe=PublicIdentity(identity);
    wchar_t guid[64]{}; StringFromGUID2(pe.pdb_guid,guid,64);
    module.cache_identity=TextSha256(key+L"|"+guid+L"|"+std::to_wstring(pe.pdb_age)+L"|"+std::to_wstring(pe.machine));
    ReadCache(module);
    module_diagnostic.status = L"loaded";
    ++stats_.modules_loaded;
    loaded_modules_.emplace(key,std::move(module));
}

std::wstring Symbolizer::PdbPath(const ImageRecord& image) {
    AddModule(image);
    std::scoped_lock lock(mutex_, g_dbghelp_mutex);
    const auto found = loaded_modules_.find(ModuleKey(image, NormalizePath(image.path)));
    if (found == loaded_modules_.end()) return {};
    BYTE buffer[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    SymFromAddrW(session_key_, found->second.synthetic_base + 1, &displacement, symbol);
    IMAGEHLP_MODULEW64 info{};
    info.SizeOfStruct = sizeof(info);
    const auto identity = ReadModuleIdentity(ModuleFilePath(image));
    if (!SymGetModuleInfoW64(session_key_, found->second.synthetic_base, &info) ||
        !identity.has_pdb_guid || info.PdbUnmatched || info.DbgUnmatched ||
        !IsEqualGUID(identity.pdb_guid, info.PdbSig70) || identity.pdb_age != info.PdbAge)
        return {};
    return info.LoadedPdbName;
}

ResolvedSymbol Symbolizer::Resolve(const TraceData& trace, uint32_t pid,
                                   uint64_t address, uint64_t timestamp_ns) {
    ResolvedSymbol result;
    std::scoped_lock lock(mutex_, g_dbghelp_mutex);
    if (indexed_trace_ != &trace || indexed_images_ != trace.images.data() ||
        indexed_image_count_ != trace.images.size()) {
        image_index_.Build(trace);
        indexed_trace_ = &trace;
        indexed_images_ = trace.images.data();
        indexed_image_count_ = trace.images.size();
    }
    const ImageRecord* image = image_index_.Find(pid, address, timestamp_ns);
    result.module = image != nullptr ? FileNameForImage(*image) : L"[unknown]";
    if (image != nullptr) {
        result.module_identity = ModuleIdentityKey(*image);
        result.module_path = image->path;
        result.rva = result.function_rva = address - image->base;
        result.displacement = address >= image->base ? address - image->base : 0;
    }

    const std::wstring key =
        image != nullptr ? ModuleKey(*image, NormalizePath(image->path)) : L"";
    auto module = loaded_modules_.find(key);
    if (!initialized_ || image == nullptr || module == loaded_modules_.end()) {
        result.symbol = result.module == L"[unknown]"
                            ? L"[unknown]"
                            : result.module + L"+0x" + ToHex(result.displacement);
        RecordResolution(key, false);
        return result;
    }

    const uint64_t symbol_address = module->second.synthetic_base + result.displacement;
    const auto cached = symbol_cache_.find(symbol_address);
    if (cached != symbol_cache_.end()) {
        ++stats_.cache_hits;
        ++stats_.memory_hits;
        RecordResolution(key, cached->second.resolved);
        return cached->second;
    }
    const auto persisted=module->second.persistent_symbols.find(result.rva);
    if(persisted!=module->second.persistent_symbols.end()) {
        auto value=persisted->second;
        value.module=result.module; value.module_identity=result.module_identity; value.module_path=result.module_path;
        ++stats_.cache_hits;
        ++stats_.persistent_hits;
        RecordResolution(key, true);
        CacheSymbol(symbol_address,value);
        return value;
    }
    ++stats_.cache_misses;
    ++stats_.dbghelp_resolves;

    BYTE buffer[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (SymFromAddrW(session_key_, symbol_address, &displacement, symbol) &&
        symbol->NameLen != 0) {
        result.symbol.assign(symbol->Name, symbol->NameLen);
        result.displacement = displacement;
        result.resolved = true;
        result.function_rva = result.rva - displacement;
        result.function_size = symbol->Size;
        ++stats_.dbghelp_resolved;
    } else {
        result.symbol = result.module + L"+0x" + ToHex(result.displacement);
    }
    RecordResolution(key, result.resolved);

    IMAGEHLP_LINEW64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    if (SymGetLineFromAddrW64(session_key_, symbol_address, &line_displacement,
                              &line)) {
        result.source_file = line.FileName != nullptr ? line.FileName : L"";
        result.line = line.LineNumber;
    }
    const DWORD inline_count=std::min<DWORD>(SymAddrIncludeInlineTrace(session_key_,symbol_address),32);
    DWORD context=0, frame_index=0;
    if(inline_count>0 && SymQueryInlineTrace(session_key_,symbol_address,0,symbol_address,symbol_address,&context,&frame_index)) {
        for(DWORD i=0;i<inline_count;++i) {
            BYTE storage[sizeof(SYMBOL_INFOW)+MAX_SYM_NAME*sizeof(wchar_t)]{};
            auto* info=reinterpret_cast<SYMBOL_INFOW*>(storage);
            info->SizeOfStruct=sizeof(SYMBOL_INFOW); info->MaxNameLen=MAX_SYM_NAME;
            DWORD64 offset=0;
            if(!SymFromInlineContextW(session_key_,symbol_address,context+i,&offset,info)) continue;
            InlineFrame entry; entry.symbol.assign(info->Name,info->NameLen);
            IMAGEHLP_LINEW64 source{}; source.SizeOfStruct=sizeof(source); DWORD ignored=0;
            if(SymGetLineFromInlineContextW(session_key_,symbol_address,context+i,module->second.synthetic_base,&ignored,&source)) {
                entry.source_file=source.FileName ? source.FileName : L""; entry.line=source.LineNumber;
            }
            result.inline_frames.push_back(std::move(entry));
        }
    }
    if(result.resolved && module->second.persistent_symbols.size()<10000 &&
       persistent_cache_bytes_+SymbolBytes(result)<=kSymbolCacheBudget) {
        persistent_cache_bytes_+=SymbolBytes(result);
        module->second.persistent_symbols[result.rva]=result;
        module->second.dirty=true;
    }
    CacheSymbol(symbol_address,result);
    return result;
}

void Symbolizer::CacheSymbol(uint64_t address,const ResolvedSymbol& value) {
    const auto symbol_bytes=SymbolBytes(value);
    if(symbol_cache_bytes_+symbol_bytes>kSymbolCacheBudget || symbol_cache_.size()>=200000) {
        stats_.cache_evictions += symbol_cache_.size();
        symbol_cache_.clear(); symbol_cache_bytes_=0;
    }
    if(symbol_bytes<=kSymbolCacheBudget && symbol_cache_.emplace(address,value).second)symbol_cache_bytes_+=symbol_bytes;
}

void Symbolizer::RecordResolution(const std::wstring& module_key, bool resolved) {
    if (module_key.empty()) return;
    const auto found = module_diagnostics_.find(module_key);
    if (found == module_diagnostics_.end()) return;
    if (resolved) {
        ++found->second.resolved;
    } else {
        ++found->second.unresolved;
        ++stats_.unresolved;
    }
}

void Symbolizer::ReadCache(ModuleLoad& module) {
    const auto path=cache_directory_/module.cache_identity/L"symbols.v1";
    std::error_code ec;
    const auto bytes=std::filesystem::file_size(path,ec);
    if(ec || bytes>16*1024*1024)return;
    std::ifstream file(path,std::ios::binary);
    std::string header;
    if(!std::getline(file,header)||header!="wperf-symcache-v1 "+ToUtf8(module.cache_identity))return;
    std::map<uint64_t,ResolvedSymbol> values;
    std::string line;
    while(std::getline(file,line)) {
        if(values.size()>=10000)return;
        std::istringstream fields(line); ResolvedSymbol value; uint32_t count=0;
        std::string symbol,source;
        if(!(fields>>value.rva>>value.function_rva>>value.function_size>>value.displacement>>value.line>>symbol>>source>>count)||count>32)return;
        if(!UnhexText(symbol,value.symbol)||!UnhexText(source=="-"?"":source,value.source_file))return;
        for(uint32_t i=0;i<count;++i) {
            InlineFrame frame; std::string name,path_text;
            if(!(fields>>frame.line>>name>>path_text)||!UnhexText(name,frame.symbol)||
               !UnhexText(path_text=="-"?"":path_text,frame.source_file))return;
            value.inline_frames.push_back(std::move(frame));
        }
        value.resolved=true;
        const auto symbol_bytes=SymbolBytes(value);
        if(persistent_cache_bytes_+symbol_bytes>kSymbolCacheBudget)break;
        persistent_cache_bytes_+=symbol_bytes;
        values.emplace(value.rva,std::move(value));
    }
    module.persistent_symbols=std::move(values);
}
void Symbolizer::WriteCache(const ModuleLoad& module) {
    if(!module.dirty||module.cache_identity.empty())return;
    const auto directory=cache_directory_/module.cache_identity;
    if(!EnsureDirectory(directory,nullptr))return;
    const auto temporary=directory/(L"symbols-"+NewId()+L".tmp");
    std::ofstream file(temporary,std::ios::binary);
    if(!file)return;
    file<<"wperf-symcache-v1 "<<ToUtf8(module.cache_identity)<<'\n';
    size_t count=0;
    const auto field=[](const std::wstring& text){auto value=HexText(text);return value.empty()?std::string("-"):value;};
    for(const auto& [rva,value]:module.persistent_symbols) {
        if(++count>10000)break;
        file<<rva<<' '<<value.function_rva<<' '<<value.function_size<<' '<<value.displacement<<' '
            <<value.line<<' '<<field(value.symbol)<<' '<<field(value.source_file)<<' '<<value.inline_frames.size();
        for(const auto& frame:value.inline_frames) file<<' '<<frame.line<<' '<<field(frame.symbol)<<' '<<field(frame.source_file);
        file<<'\n';
    }
    file.close();
    if(file) MoveFileExW(temporary.c_str(),(directory/L"symbols.v1").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    std::error_code ignored; std::filesystem::remove(temporary,ignored);
}

std::wstring SymbolAggregateKey(const ResolvedSymbol& symbol) {
    return symbol.module_identity + L"|" + std::to_wstring(symbol.function_rva) + L"|" + symbol.symbol;
}

std::wstring SymbolComparisonKey(const ResolvedSymbol& symbol, bool strict_identity,
                                 std::wstring_view unmapped_scope) {
    if (!symbol.resolved && symbol.module_identity.empty() && !unmapped_scope.empty())
        return L"unmapped|" + std::wstring(unmapped_scope);
    if (strict_identity || !symbol.resolved)
        return L"identity|" + SymbolAggregateKey(symbol);
    std::wstring module = symbol.module;
    std::transform(module.begin(), module.end(), module.begin(),
                   [](wchar_t ch) { return wchar_t(std::towlower(ch)); });
    return L"symbol|" + module + L"!" + symbol.symbol;
}

SymbolizerStats Symbolizer::Stats() const {
    std::scoped_lock lock(mutex_, g_dbghelp_mutex);
    auto stats=stats_; stats.cache_bytes=symbol_cache_bytes_+persistent_cache_bytes_; return stats;
}

SymbolizerDiagnostics Symbolizer::Diagnostics() const {
    std::scoped_lock lock(mutex_, g_dbghelp_mutex);
    SymbolizerDiagnostics result;
    result.symbol_path = symbol_path_;
    result.explicit_symbol_path = explicit_symbol_path_;
    result.environment_symbol_path = environment_symbol_path_;
    result.symbol_path_source = symbol_path_source_;
    result.symcache_path = cache_directory_.wstring();
    result.dbghelp_path = dbghelp_path_;
    result.dbghelp_version = dbghelp_version_;
    result.stats = stats_;
    result.stats.cache_bytes = symbol_cache_bytes_ + persistent_cache_bytes_;
    result.modules.reserve(module_diagnostics_.size());
    for (const auto& [key, module] : module_diagnostics_) {
        (void)key;
        if (module.status == L"loaded" && module.resolved == 0 &&
            module.unresolved == 0) {
            continue;
        }
        result.modules.push_back(module);
    }
    std::sort(result.modules.begin(), result.modules.end(),
              [](const SymbolModuleDiagnostic& left,
                 const SymbolModuleDiagnostic& right) {
                  const bool left_rejected = left.status == L"rejected";
                  const bool right_rejected = right.status == L"rejected";
                  if (left_rejected != right_rejected) return left_rejected;
                  if (left.unresolved != right.unresolved)
                      return left.unresolved > right.unresolved;
                  return _wcsicmp(left.module.c_str(), right.module.c_str()) < 0;
              });
    return result;
}

void Symbolizer::Close() {
    std::scoped_lock lock(mutex_, g_dbghelp_mutex);
    if (initialized_) {
        SymCleanup(session_key_);
        initialized_ = false;
    }
    if (session_key_ != nullptr) CloseHandle(session_key_);
    session_key_ = nullptr;
    indexed_trace_ = nullptr;
    indexed_images_ = nullptr;
    indexed_image_count_ = 0;
    for(const auto& [key,module]:loaded_modules_) { (void)key; WriteCache(module); }
    loaded_modules_.clear();
    rejected_modules_.clear();
    module_diagnostics_.clear();
    symbol_cache_.clear();
    symbol_cache_bytes_=persistent_cache_bytes_=0;
    stats_ = {};
    next_synthetic_base_ = 0x100000000ull;
}

}  // namespace wperf
