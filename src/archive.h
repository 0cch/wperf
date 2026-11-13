#pragma once
#include "trace_model.h"
#include <filesystem>

namespace wperf {
bool LoadArchiveModules(const std::filesystem::path& directory, TraceData& trace,
                        std::wstring* error = nullptr);
std::wstring ArchiveSymbolPath(const std::filesystem::path& directory, const TraceData& trace);
}
