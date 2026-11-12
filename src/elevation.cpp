#include "elevation.h"

#include "common.h"

#include <shellapi.h>

namespace wperf {

bool LaunchWorkerProcess(const std::wstring& executable,
                         const std::vector<std::wstring>& arguments,
                         bool request_elevation,
                         ProcessLaunchResult& result,
                         std::wstring* error) {
    result = {};
    const std::wstring parameters = JoinCommandLineArguments(arguments);
    if (request_elevation) {
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpVerb = L"runas";
        info.lpFile = executable.c_str();
        info.lpParameters = parameters.c_str();
        info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info) || info.hProcess == nullptr) {
            const DWORD code = GetLastError();
            result.error_code = code;
            if (error != nullptr) {
                *error = L"ShellExecuteExW(runas) failed: " +
                         FormatWindowsError(code);
            }
            return false;
        }
        result.process = info.hProcess;
        result.process_id = GetProcessId(info.hProcess);
        return true;
    }

    std::wstring command_line = QuoteCommandLineArgument(executable);
    if (!parameters.empty()) {
        command_line.push_back(L' ');
        command_line += parameters;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process)) {
        const DWORD code = GetLastError();
        result.error_code = code;
        if (error != nullptr) {
            *error = L"CreateProcessW(worker) failed: " +
                     FormatWindowsError(code);
        }
        return false;
    }
    CloseHandle(process.hThread);
    result.process = process.hProcess;
    result.process_id = process.dwProcessId;
    return true;
}

bool IsProcessRunning(uint32_t process_id) {
    if (process_id == 0) {
        return false;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, process_id);
    if (process == nullptr) {
        return false;
    }
    DWORD exit_code = 0;
    const bool alive = GetExitCodeProcess(process, &exit_code) &&
                       exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

bool WaitForProcessExit(HANDLE process, uint32_t timeout_ms, DWORD& exit_code) {
    exit_code = 0;
    if (process == nullptr) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(process, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        return false;
    }
    return GetExitCodeProcess(process, &exit_code) != FALSE;
}

}  // namespace wperf
