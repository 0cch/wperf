#pragma once

#include "trace_model.h"

#include <filesystem>
#include <string>

namespace wperf {

bool ReadEtwFile(const std::filesystem::path& path, TraceData& trace,
                 std::wstring* error = nullptr);

}  // namespace wperf
