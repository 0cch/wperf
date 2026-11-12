#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wperf {

inline constexpr wchar_t kVersion[] = L"0.3.0-m4-m5";
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

}  // namespace wperf
