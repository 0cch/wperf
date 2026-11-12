#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace wperf {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
};

class Logger {
public:
    Logger(std::filesystem::path path, std::wstring correlation_id,
           std::wstring role);

    bool IsOpen() const;
    void Log(LogLevel level, std::wstring_view phase, std::wstring_view message);

private:
    std::filesystem::path path_;
    std::wstring correlation_id_;
    std::wstring role_;
    std::ofstream stream_;
    mutable std::mutex mutex_;
};

std::wstring_view LogLevelName(LogLevel level);

}  // namespace wperf
