#include "logger.h"

#include "common.h"

#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>

namespace wperf {

std::wstring_view LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return L"debug";
        case LogLevel::Info:
            return L"info";
        case LogLevel::Warning:
            return L"warning";
        case LogLevel::Error:
            return L"error";
    }
    return L"info";
}

Logger::Logger(std::filesystem::path path, std::wstring correlation_id,
               std::wstring role)
    : path_(std::move(path)),
      correlation_id_(std::move(correlation_id)),
      role_(std::move(role)) {
    std::wstring ignored;
    EnsureDirectory(path_.parent_path(), &ignored);
    stream_.open(path_, std::ios::binary | std::ios::app);
}

bool Logger::IsOpen() const {
    return stream_.is_open();
}

void Logger::Log(LogLevel level, std::wstring_view phase,
                 std::wstring_view message) {
    std::lock_guard lock(mutex_);
    if (!stream_.is_open()) {
        return;
    }

    nlohmann::ordered_json entry;
    entry["ts"] = ToUtf8(UtcTimestamp());
    entry["level"] = ToUtf8(LogLevelName(level));
    entry["role"] = ToUtf8(role_);
    entry["correlation"] = ToUtf8(correlation_id_);
    entry["phase"] = ToUtf8(phase);
    entry["message"] = ToUtf8(message);
    stream_ << entry.dump() << "\n";
    stream_.flush();
}

}  // namespace wperf
