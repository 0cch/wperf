#pragma once

#include "common.h"
#include "elevation.h"
#include "state.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace wperf::commands {

inline std::wstring FormatGuid(const GUID& guid) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
               guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
               guid.Data4[6], guid.Data4[7]);
    return buffer;
}

inline std::wstring OptionValue(const std::vector<std::wstring>& arguments,
                                const std::wstring& name,
                                const std::wstring& fallback = {}) {
    for (size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == name) {
            return arguments[index + 1];
        }
    }
    return fallback;
}

inline bool HasFlag(const std::vector<std::wstring>& arguments,
                    const std::wstring& flag) {
    for (const auto& argument : arguments) {
        if (argument == flag) {
            return true;
        }
    }
    return false;
}

inline bool ParseUnsigned(const std::wstring& value, uint64_t& result) {
    if (value.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long long parsed = wcstoull(value.c_str(), &end, 10);
    if (end == nullptr || *end != L'\0') {
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
    std::wcerr << L"wperf: " << message << L"\n";
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