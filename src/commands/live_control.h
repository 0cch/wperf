#pragma once

#include "elevation.h"
#include "ipc.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wperf {

bool WaitForCaptureReady(PipeServer& pipe, HANDLE worker, const std::wstring& session,
                         uint32_t timeout_ms, std::wstring* error = nullptr);

struct LiveWorkerOptions {
    std::wstring mode;
    std::wstring session_name;
    std::wstring event_filter = L"all";
    std::wstring profile_source = L"cpu";
    std::wstring pid_text;
    std::wstring duration_text = L"0";
    std::wstring max_text = L"500";
    uint64_t interval_ms = 1000;
    bool json = false;
};

class LiveWorkerClient {
public:
    LiveWorkerClient() = default;
    ~LiveWorkerClient();

    LiveWorkerClient(const LiveWorkerClient&) = delete;
    LiveWorkerClient& operator=(const LiveWorkerClient&) = delete;

    bool Start(const LiveWorkerOptions& options, bool request_elevation,
               std::wstring* error = nullptr);
    bool StartWithExecutable(const LiveWorkerOptions& options,
                             const std::wstring& executable,
                             bool request_elevation,
                             std::wstring* error = nullptr);
    bool ReceiveLine(std::wstring& line, uint32_t timeout_ms,
                     std::wstring* error = nullptr);
    bool SendStop(std::wstring* error = nullptr);
    bool HasData() const;
    bool WaitForExit(uint32_t timeout_ms, DWORD& exit_code,
                     std::wstring* error = nullptr);
    bool StopAndDrain(uint32_t timeout_ms, DWORD& exit_code,
                       std::wstring* error = nullptr);
    void Close();
    DWORD WorkerPid() const;

private:
    PipeServer pipe_;
    ProcessLaunchResult worker_;
    std::wstring session_name_;
};

}  // namespace wperf
