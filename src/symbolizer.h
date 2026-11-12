#pragma once

#include "trace_model.h"

#include <mutex>
#include <string>
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
    std::wstring NormalizePath(const std::wstring& path) const;
    std::mutex mutex_;
    bool initialized_ = false;
    std::unordered_set<std::wstring> loaded_modules_;
    std::unordered_set<uint64_t> rejected_bases_;
};

}  // namespace wperf
