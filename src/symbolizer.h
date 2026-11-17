#pragma once

#include "trace_model.h"

#include <windows.h>

#include <mutex>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
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
std::wstring SymbolComparisonKey(const ResolvedSymbol& symbol, bool strict_identity,
                                 std::wstring_view unmapped_scope = {});

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
    // cache_hits/cache_misses remain aggregate counters for older callers.
    // The detailed counters distinguish the cache layer and DbgHelp work.
    uint64_t cache_hits = 0;
    uint64_t cache_misses = 0;
    uint64_t memory_hits = 0;
    uint64_t persistent_hits = 0;
    uint64_t dbghelp_resolves = 0;
    uint64_t dbghelp_resolved = 0;
    uint64_t unresolved = 0;
    uint64_t cache_bytes = 0;
    uint64_t cache_evictions = 0;
    uint64_t modules_total = 0;
    uint64_t modules_loaded = 0;
    uint64_t modules_rejected = 0;
};

struct SymbolModuleDiagnostic {
    std::wstring module;
    std::wstring path;
    std::wstring recorded_path;
    std::wstring pdb_path;
    std::wstring pdb_status;
    std::wstring status;
    std::wstring reason;
    uint64_t resolved = 0;
    uint64_t unresolved = 0;
};

struct SymbolizerDiagnostics {
    std::wstring symbol_path;
    std::wstring explicit_symbol_path;
    std::wstring environment_symbol_path;
    std::wstring symbol_path_source;
    std::wstring symcache_path;
    std::wstring dbghelp_path;
    std::wstring dbghelp_version;
    SymbolizerStats stats;
    std::vector<SymbolModuleDiagnostic> modules;
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
    SymbolizerDiagnostics Diagnostics() const;
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
    void RecordResolution(const std::wstring& module_key, bool resolved);

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
    std::map<std::wstring, SymbolModuleDiagnostic> module_diagnostics_;
    std::unordered_map<uint64_t, ResolvedSymbol> symbol_cache_;
    SymbolizerStats stats_;
    uint64_t next_synthetic_base_ = 0x100000000ull;
    std::filesystem::path cache_directory_;
    std::wstring symbol_path_;
    std::wstring explicit_symbol_path_;
    std::wstring environment_symbol_path_;
    std::wstring symbol_path_source_;
    std::wstring dbghelp_path_;
    std::wstring dbghelp_version_;
    size_t symbol_cache_bytes_ = 0;
    size_t persistent_cache_bytes_ = 0;
};

}  // namespace wperf
