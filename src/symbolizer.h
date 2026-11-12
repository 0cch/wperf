#pragma once

#include "trace_model.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wperf {

struct ResolvedSymbol {
    std::wstring module;
    std::wstring symbol;
    std::wstring source_file;
    uint32_t line = 0;
    uint64_t displacement = 0;
    bool resolved = false;
};

class Symbolizer {
public:
    Symbolizer() = default;
    ~Symbolizer();

    Symbolizer(const Symbolizer&) = delete;
    Symbolizer& operator=(const Symbolizer&) = delete;

    bool Initialize(std::wstring symbol_path, std::wstring* error = nullptr);
    void AddModule(const ImageRecord& image);
    ResolvedSymbol Resolve(const TraceData& trace, uint32_t pid, uint64_t address,
                           uint64_t timestamp_ns);
    void Close();

private:
    struct ModuleLoad {
        uint64_t synthetic_base = 0;
    };

    std::wstring NormalizePath(const std::wstring& path) const;
    std::wstring ModuleKey(const ImageRecord& image,
                           const std::wstring& normalized_path) const;

    std::mutex mutex_;
    bool initialized_ = false;
    std::unordered_map<std::wstring, ModuleLoad> loaded_modules_;
    std::unordered_set<std::wstring> rejected_modules_;
    uint64_t next_synthetic_base_ = 0x100000000ull;
};

}  // namespace wperf