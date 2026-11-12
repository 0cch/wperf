#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace wperf {

struct ProcessLaunchResult {
    HANDLE process = nullptr;
    DWORD process_id = 0;
    DWORD error_code = 0;
};

bool LaunchWorkerProcess(const std::wstring& executable,
                         const std::vector<std::wstring>& arguments,
                         bool request_elevation,
                         ProcessLaunchResult& result,
                         std::wstring* error = nullptr);
bool LaunchTargetProcess(const std::vector<std::wstring>& command,
                         ProcessLaunchResult& result,
                         std::wstring* error = nullptr);
bool IsProcessRunning(uint32_t process_id);
bool WaitForProcessExit(HANDLE process, uint32_t timeout_ms, DWORD& exit_code);

}  // namespace wperf
