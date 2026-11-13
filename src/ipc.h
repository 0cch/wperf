#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace wperf {

std::wstring MakePipePath(std::wstring_view correlation_id);

class PipeServer {
public:
    PipeServer() = default;
    ~PipeServer();

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    bool Create(std::wstring_view correlation_id, std::wstring* error = nullptr);
    bool WaitForClient(uint32_t timeout_ms, std::wstring* error = nullptr);
    bool SendLine(std::wstring_view line, std::wstring* error = nullptr,
                  uint32_t timeout_ms = 5000);
    bool ReceiveLine(std::wstring& line, uint32_t timeout_ms,
                     std::wstring* error = nullptr);
    bool HasData() const;
    bool IsConnected() const;
    void Close();

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    bool connected_ = false;
};

class PipeClient {
public:
    PipeClient() = default;
    ~PipeClient();

    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    bool Connect(std::wstring_view pipe_path, uint32_t timeout_ms,
                 std::wstring* error = nullptr);
    bool SendLine(std::wstring_view line, std::wstring* error = nullptr,
                  uint32_t timeout_ms = 5000);
    bool ReceiveLine(std::wstring& line, uint32_t timeout_ms,
                     std::wstring* error = nullptr);
    bool HasData() const;
    bool IsConnected() const;
    bool ServerProcessId(uint32_t& process_id) const;
    void Close();

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

}  // namespace wperf
