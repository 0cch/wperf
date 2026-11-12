#include "logger.h"

#include "common.h"

#include <iomanip>
#include <sstream>

namespace wperf {
namespace {

std::string EscapeJson(std::wstring_view value) {
    const std::string utf8 = ToUtf8(value);
    std::string result;
    result.reserve(utf8.size() + 8);
    for (unsigned char ch : utf8) {
        switch (ch) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    std::ostringstream escaped;
                    escaped << "\\u" << std::hex << std::setw(4)
                            << std::setfill('0') << static_cast<int>(ch);
                    result += escaped.str();
                } else {
                    result.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return result;
}

}  // namespace

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

    stream_ << "{\"ts\":\"" << EscapeJson(UtcTimestamp()) << "\","
            << "\"level\":\"" << EscapeJson(LogLevelName(level)) << "\","
            << "\"role\":\"" << EscapeJson(role_) << "\","
            << "\"correlation\":\"" << EscapeJson(correlation_id_) << "\","
            << "\"phase\":\"" << EscapeJson(phase) << "\","
            << "\"message\":\"" << EscapeJson(message) << "\"}\n";
    stream_.flush();
}

}  // namespace wperf
