#pragma once

#include <filesystem>
#include <string>

namespace wperf {

void InstallCrashHandler(const std::filesystem::path& crash_directory,
                         const std::wstring& correlation_id);

}  // namespace wperf
