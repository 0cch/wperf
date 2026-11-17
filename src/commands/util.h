#pragma once

#include <cerrno>

#include "common.h"
#include "elevation.h"
#include "state.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace wperf {
enum class ExitCode : int {
    Success = 0,
    InvalidArguments = 2,
    PermissionDenied = 3,
    CaptureFailure = 4,
    CapabilityUnavailable = 5,
    ReplayOrQualityFailure = 6,
    TargetLaunchFailure = 7,
    TargetProcessFailure = 8,
    InternalError = 9,
};
inline int ExitStatus(ExitCode code) { return static_cast<int>(code); }
}

namespace wperf::commands {

inline thread_local bool g_json_errors = false;
inline thread_local std::wstring g_last_error;

inline void SetJsonErrors(bool enabled) {
    g_json_errors = enabled;
    g_last_error.clear();
}

inline std::wstring LastErrorMessage() { return g_last_error; }

inline std::wstring FormatGuid(const GUID& guid) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
               guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
               guid.Data4[6], guid.Data4[7]);
    return buffer;
}

inline bool ParseUnsigned(const std::wstring& value, uint64_t& result) {
    if (value.empty()) {
        return false;
    }
    if (value.front() < L'0' || value.front() > L'9') return false;
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long parsed = wcstoull(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || *end != L'\0') {
        return false;
    }
    result = parsed;
    return true;
}

inline bool ParseUint32(const std::wstring& value, uint32_t& result) {
    uint64_t parsed = 0;
    if (!ParseUnsigned(value, parsed) || parsed > UINT32_MAX) {
        return false;
    }
    result = static_cast<uint32_t>(parsed);
    return true;
}

inline void PrintError(const std::wstring& message) {
    g_last_error = message;
    if (!g_json_errors) std::wcerr << L"wperf: " << message << L"\n";
}

inline std::wstring ClampText(std::wstring value, size_t maximum) {
    if (maximum < 4 || value.size() <= maximum) {
        return value;
    }
    value.resize(maximum - 3);
    value += L"...";
    return value;
}

inline std::wstring CsvEscape(const std::wstring& value) {
    const bool quote = value.find_first_of(L",\"\r\n") != std::wstring::npos;
    if (!quote) {
        return value;
    }
    std::wstring escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back(L'"');
    for (const wchar_t ch : value) {
        if (ch == L'"') {
            escaped += L"\"\"";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back(L'"');
    return escaped;
}

inline void PrintVersion() {
    std::wcout << L"wperf " << kVersion << L"\n";
}

inline std::wstring AfterSpace(const std::wstring& value) {
    const size_t space = value.find(L' ');
    return space == std::wstring::npos ? std::wstring{} : value.substr(space + 1);
}

inline bool StartsWith(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

inline bool IsStale(const RunState& state) {
    if (state.phase == L"FINISHED") {
        return false;
    }
    return state.worker_pid == 0 || !IsProcessRunning(state.worker_pid);
}

}  // namespace wperf::commands