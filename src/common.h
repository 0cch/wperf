#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <thread>

namespace wperf {

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) : handle_(handle) {}
    ~ScopedHandle() { if(handle_ && handle_!=INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    ScopedHandle(const ScopedHandle&)=delete;
    ScopedHandle& operator=(const ScopedHandle&)=delete;
    HANDLE get() const { return handle_; }
private:
    HANDLE handle_;
};

// Used only in the authenticated profiler worker: abort it if teardown cannot finish.
class WorkerStopWatchdog {
public:
    explicit WorkerStopWatchdog(uint32_t timeout_ms = 5000);
    ~WorkerStopWatchdog();
    WorkerStopWatchdog(const WorkerStopWatchdog&)=delete;
    WorkerStopWatchdog& operator=(const WorkerStopWatchdog&)=delete;
private:
    HANDLE finished_ = nullptr;
    std::thread thread_;
};

#ifndef WPERF_VERSION_STRING
#define WPERF_VERSION_STRING "0.0.0.0"
#endif
inline constexpr wchar_t kVersion[] = L"" WPERF_VERSION_STRING;
inline constexpr wchar_t kToolName[] = L"wperf";

std::string ToUtf8(std::wstring_view value);
std::wstring ToUtf16(std::string_view value);
std::wstring FormatWindowsError(DWORD error);
std::wstring NewId();
std::wstring UtcTimestamp();
std::wstring QuoteCommandLineArgument(std::wstring_view argument);
std::wstring JoinCommandLineArguments(const std::vector<std::wstring>& arguments);
std::wstring GetExecutablePath();
std::filesystem::path GetLocalAppDataDirectory();
std::filesystem::path GetWperfRootDirectory();
std::filesystem::path GetSessionsDirectory();
std::filesystem::path GetLogsDirectory();
std::filesystem::path GetCrashesDirectory();
bool EnsureDirectory(const std::filesystem::path& path, std::wstring* error = nullptr);
bool IsProcessElevated(bool& elevated, std::wstring* error = nullptr);
bool EnablePrivilege(std::wstring_view privilege, std::wstring* error = nullptr);
std::wstring FileSha256(const std::filesystem::path& path);
std::wstring TextSha256(std::wstring_view text);
std::string HexText(std::wstring_view text);
bool UnhexText(std::string_view text, std::wstring& value);

}  // namespace wperf
