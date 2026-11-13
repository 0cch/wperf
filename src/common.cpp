#include "common.h"

#include <bcrypt.h>

#include <array>
#include <fstream>
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

std::string HexText(std::wstring_view value) {
    const auto text = ToUtf8(value);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : text) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
bool UnhexText(std::string_view text, std::wstring& value) {
    if (text.size() % 2 != 0 || text.size() > 131072) return false;
    const auto nibble = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' :
        ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; };
    std::string bytes;
    for (size_t i=0; i<text.size(); i+=2) {
        const int a=nibble(text[i]), b=nibble(text[i+1]);
        if (a<0 || b<0) return false;
        bytes += char((a<<4)|b);
    }
    value=ToUtf16(bytes);
    return bytes.empty() || !value.empty();
}

std::wstring TextSha256(std::wstring_view value) {
    const auto bytes=ToUtf8(value);
    BYTE digest[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                  reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                  static_cast<ULONG>(bytes.size()), digest, sizeof(digest)) < 0) return {};
    std::wstring result;
    for (BYTE byte : digest) { result += kHex[byte>>4]; result += kHex[byte&15]; }
    return result;
}

std::wstring FileSha256(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    BCRYPT_HASH_HANDLE hash=nullptr;
    DWORD object_size=0, returned=0;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return {};
    std::wstring result;
    std::vector<BYTE> object;
    do {
        if (BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&object_size),
                              sizeof(object_size),&returned,0)<0) break;
        object.resize(object_size);
        if (BCryptCreateHash(algorithm,&hash,object.data(),object_size,nullptr,0,0)<0) break;
        std::ifstream file(path,std::ios::binary);
        if (!file) break;
        std::array<char,65536> buffer{};
        bool valid=true;
        while (file) {
            file.read(buffer.data(),buffer.size());
            if (file.gcount()>0 && BCryptHashData(hash,reinterpret_cast<PUCHAR>(buffer.data()),
                                                static_cast<ULONG>(file.gcount()),0)<0) { valid=false; break; }
        }
        BYTE digest[32]{};
        if (!valid || file.bad() || BCryptFinishHash(hash,digest,sizeof(digest),0)<0) break;
        for (BYTE byte : digest) { result += kHex[byte>>4]; result += kHex[byte&15]; }
    } while (false);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm,0);
    return result;
}

WorkerStopWatchdog::WorkerStopWatchdog(uint32_t timeout_ms) {
    finished_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!finished_)TerminateProcess(GetCurrentProcess(),ERROR_NOT_ENOUGH_MEMORY);
    thread_=std::thread([signal=finished_,timeout_ms] {
        if(WaitForSingleObject(signal,timeout_ms)!=WAIT_OBJECT_0)
            TerminateProcess(GetCurrentProcess(),ERROR_TIMEOUT);
    });
}
WorkerStopWatchdog::~WorkerStopWatchdog() {
    SetEvent(finished_);
    if(thread_.joinable())thread_.join();
    CloseHandle(finished_);
}

}  // namespace wperf
