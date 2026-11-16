#pragma once

#include "trace_model.h"

#include <filesystem>
#include <string>
#include <span>
#include <functional>
#include <windows.h>
#include <evntcons.h>

namespace wperf {

bool ReadEtwFile(const std::filesystem::path& path, TraceData& trace,
                 std::wstring* error = nullptr, bool derive_off_cpu = true, std::wstring_view diagnostic_kind = {});
bool ReadEtwRecords(std::span<const EVENT_RECORD> records, TraceData& trace,
                     uint64_t frequency = 10000000, std::wstring_view diagnostic_kind = {});
bool ReadEtwMetadata(const std::filesystem::path& path, TraceData& trace,
                     std::wstring* error = nullptr);
bool StreamEtwSamples(const std::filesystem::path& path, TraceData& metadata,
                      const std::function<void(const SampleRecord&)>& consumer,
                      std::wstring* error = nullptr);

}  // namespace wperf
