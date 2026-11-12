#include "common.h"

#include <bcrypt.h>

#include <array>
#include <system_error>

namespace wperf {
namespace {

constexpr wchar_t kHex[] = L"0123456789abcdef";

std::wstring FormatSystemError(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return L"error " + std::to_wstring(error);
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

std::wstring GetEnvironmentValue(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return {};
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) {
        return {};
    }
    value.resize(written);
    return value;
}

bool NeedsQuotes(wchar_t value) {
    return value == L' ' || value == L'\t' || value == L'"';
}

}  // namespace

std::string ToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                             static_cast<int>(value.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), required, nullptr, nullptr);
    return result;
}

std::wstring ToUtf16(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                              static_cast<int>(value.size()),
                                              nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), required);
    return result;
}

std::wstring FormatWindowsError(DWORD error) {
    return FormatSystemError(error);
}

std::wstring NewId() {
    std::array<unsigned char, 16> bytes{};
    const NTSTATUS status = BCryptGenRandom(
        nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        return L"00000000000000000000000000000000";
    }

    std::wstring result;
    result.reserve(bytes.size() * 2);
    for (unsigned char value : bytes) {
        result.push_back(kHex[value >> 4]);
        result.push_back(kHex[value & 0x0f]);
    }
    return result;
}

std::wstring UtcTimestamp() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear,
               time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
               time.wMilliseconds);
    return buffer;
}

std::wstring QuoteCommandLineArgument(std::wstring_view argument) {
    if (argument.empty()) {
        return L"\"\"";
    }

    bool needs_quotes = false;
    for (wchar_t value : argument) {
        if (NeedsQuotes(value)) {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return std::wstring(argument);
    }

    std::wstring result;
    result.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t value : argument) {
        if (value == L'\\') {
            ++backslashes;
            continue;
        }
        if (value == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(value);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring JoinCommandLineArguments(const std::vector<std::wstring>& arguments) {
    std::wstring result;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0) {
            result.push_back(L' ');
        }
        result += QuoteCommandLineArgument(arguments[index]);
    }
    return result;
}

std::wstring GetExecutablePath() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return buffer;
}

std::filesystem::path GetLocalAppDataDirectory() {
    const std::wstring localAppData = GetEnvironmentValue(L"LOCALAPPDATA");
    if (!localAppData.empty()) {
        return std::filesystem::path(localAppData);
    }
    return std::filesystem::temp_directory_path();
}

std::filesystem::path GetWperfRootDirectory() {
    return GetLocalAppDataDirectory() / L"wperf";
}

std::filesystem::path GetSessionsDirectory() {
    return GetWperfRootDirectory() / L"sessions";
}

std::filesystem::path GetLogsDirectory() {
    return GetWperfRootDirectory() / L"logs";
}

std::filesystem::path GetCrashesDirectory() {
    return GetWperfRootDirectory() / L"crashes";
}

bool EnsureDirectory(const std::filesystem::path& path, std::wstring* error) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return std::filesystem::is_directory(path, ec);
    }
    if (!std::filesystem::create_directories(path, ec)) {
        if (error != nullptr) {
            *error = L"cannot create directory '" + path.wstring() + L"': " +
                     ToUtf16(ec.message());
        }
        return false;
    }
    return true;
}

bool IsProcessElevated(bool& elevated, std::wstring* error) {
    elevated = false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (error != nullptr) {
            *error = L"OpenProcessToken failed: " +
                     FormatWindowsError(GetLastError());
        }
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &returned);
    CloseHandle(token);
    if (!ok) {
        if (error != nullptr) {
            *error = L"GetTokenInformation(TokenElevation) failed: " +
                     FormatWindowsError(GetLastError());
        }
        return false;
    }
    elevated = elevation.TokenIsElevated != 0;
    return true;
}

bool EnablePrivilege(std::wstring_view privilege, std::wstring* error) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &token)) {
        if (error != nullptr) {
            *error = L"OpenProcessToken failed: " +
                     FormatWindowsError(GetLastError());
        }
        return false;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, std::wstring(privilege).c_str(),
                               &privileges.Privileges[0].Luid)) {
        const DWORD code = GetLastError();
        CloseHandle(token);
        if (error != nullptr) {
            *error = L"LookupPrivilegeValue failed: " + FormatWindowsError(code);
        }
        return false;
    }

    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &privileges,
                                          sizeof(privileges), nullptr, nullptr);
    const DWORD code = GetLastError();
    CloseHandle(token);
    if (!ok || code == ERROR_NOT_ALL_ASSIGNED) {
        if (error != nullptr) {
            *error = L"AdjustTokenPrivileges failed: " + FormatWindowsError(code);
        }
        return false;
    }
    return true;
}

}  // namespace wperf
