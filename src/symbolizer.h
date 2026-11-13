#pragma once

#include "trace_model.h"

#include <windows.h>

#include <mutex>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wperf {

struct InlineFrame {
    std::wstring symbol;
    std::wstring source_file;
    uint32_t line = 0;
};

struct ResolvedSymbol {
    std::wstring module;
    std::wstring symbol;
    std::wstring source_file;
    uint32_t line = 0;
    uint64_t displacement = 0;
    bool resolved = false;
    std::wstring module_identity;
    std::wstring module_path;
    uint64_t rva = 0;
    uint64_t function_rva = 0;
    uint64_t function_size = 0;
    std::vector<InlineFrame> inline_frames;
};

std::wstring SymbolAggregateKey(const ResolvedSymbol& symbol);

struct ModuleIdentity {
    bool valid = false;
    uint32_t timestamp = 0;
    uint32_t checksum = 0;
    uint32_t size_of_image = 0;
    uint16_t machine = 0;
    GUID pdb_guid{};
    uint32_t pdb_age = 0;
    bool has_pdb_guid = false;
};

ModuleIdentity ReadModuleIdentity(const std::wstring& path);
bool ModuleIdentityMatches(const ImageRecord& image, const ModuleIdentity& identity);
bool PdbIdentityMatches(const std::wstring& path, const ModuleIdentity& identity);
std::wstring NormalizeModulePath(const std::wstring& path);
std::wstring ModuleFilePath(const ImageRecord& image);

struct SymbolizerStats {
    uint64_t cache_hits = 0;
    uint64_t cache_misses = 0;
    uint64_t cache_bytes = 0;
};

class Symbolizer {
public:
    Symbolizer() = default;
    ~Symbolizer();

    Symbolizer(const Symbolizer&) = delete;
    Symbolizer& operator=(const Symbolizer&) = delete;

    bool Initialize(std::wstring symbol_path, std::wstring* error = nullptr,
                    std::filesystem::path cache_directory = {});
    void AddModule(const ImageRecord& image);
    void PrepareTrace(const TraceData& trace, bool discard_unused = false);
    std::wstring PdbPath(const ImageRecord& image);
    ResolvedSymbol Resolve(const TraceData& trace, uint32_t pid, uint64_t address,
                           uint64_t timestamp_ns);
    SymbolizerStats Stats() const;
    void Close();

private:
    struct ModuleLoad {
        uint64_t synthetic_base = 0;
        std::wstring cache_identity;
        std::map<uint64_t, ResolvedSymbol> persistent_symbols;
        bool dirty = false;
    };
    void ReadCache(ModuleLoad& module);
    void WriteCache(const ModuleLoad& module);
    void CacheSymbol(uint64_t address, const ResolvedSymbol& symbol);

    std::wstring NormalizePath(const std::wstring& path) const;
    std::wstring ModuleKey(const ImageRecord& image,
                           const std::wstring& normalized_path) const;

    mutable std::mutex mutex_;
    bool initialized_ = false;
    HANDLE session_key_ = nullptr;
    ImageIndex image_index_;
    const TraceData* indexed_trace_ = nullptr;
    const ImageRecord* indexed_images_ = nullptr;
    size_t indexed_image_count_ = 0;
    std::unordered_map<std::wstring, ModuleLoad> loaded_modules_;
    std::unordered_set<std::wstring> rejected_modules_;
    std::unordered_map<uint64_t, ResolvedSymbol> symbol_cache_;
    SymbolizerStats stats_;
    uint64_t next_synthetic_base_ = 0x100000000ull;
    std::filesystem::path cache_directory_;
    size_t symbol_cache_bytes_ = 0;
    size_t persistent_cache_bytes_ = 0;
};

}  // namespace wperf
