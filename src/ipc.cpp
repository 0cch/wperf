#include "ipc.h"

#include "common.h"

#include <sddl.h>

#include <array>

namespace wperf {
namespace {

constexpr DWORD kPipeBufferSize = 256 * 1024;

bool WaitOverlapped(HANDLE handle, OVERLAPPED& overlapped, DWORD timeout_ms,
                    DWORD* bytes_transferred, std::wstring* error) {
    const DWORD wait_result = WaitForSingleObject(overlapped.hEvent, timeout_ms);
    if (wait_result == WAIT_TIMEOUT) {
        CancelIoEx(handle, &overlapped);
        WaitForSingleObject(overlapped.hEvent, 1000);
        if (error != nullptr) {
            *error = L"named pipe operation timed out";
        }
        return false;
    }
    if (wait_result != WAIT_OBJECT_0) {
        if (error != nullptr) {
            *error = L"WaitForSingleObject(pipe) failed: " +
                     FormatWindowsError(GetLastError());
        }
        return false;
    }

    DWORD transferred = 0;
    if (!GetOverlappedResult(handle, &overlapped, &transferred, FALSE)) {
        const DWORD code = GetLastError();
        if (error != nullptr) {
            *error = L"GetOverlappedResult(pipe) failed: " +
                     FormatWindowsError(code);
        }
        return false;
    }
    if (bytes_transferred != nullptr) {
        *bytes_transferred = transferred;
    }
    return true;
}

bool WriteMessage(HANDLE pipe, std::wstring_view line, std::wstring* error) {
    std::wstring payload(line);
    payload.push_back(L'\n');
    const DWORD bytes = static_cast<DWORD>(payload.size() * sizeof(wchar_t));
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        if (error != nullptr) {
            *error = L"CreateEventW failed: " + FormatWindowsError(GetLastError());
        }
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(pipe, payload.data(), bytes, &written, &overlapped);
    const DWORD initial_error = ok ? ERROR_SUCCESS : GetLastError();
    bool result = false;
    if (ok) {
        result = true;
    } else if (initial_error == ERROR_IO_PENDING) {
        result = WaitOverlapped(pipe, overlapped, 5000, &written, error);
    } else if (error != nullptr) {
        *error = L"WriteFile(pipe) failed: " + FormatWindowsError(initial_error);
    }

    CloseHandle(overlapped.hEvent);
    return result && written == bytes;
}

bool ReadMessage(HANDLE pipe, std::wstring& line, DWORD timeout_ms,
                 std::wstring* error) {
    std::vector<wchar_t> buffer(64 * 1024);
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        if (error != nullptr) {
            *error = L"CreateEventW failed: " + FormatWindowsError(GetLastError());
        }
        return false;
    }

    DWORD read = 0;
    const BOOL ok = ReadFile(pipe, buffer.data(),
                             static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
                             &read, &overlapped);
    const DWORD initial_error = ok ? ERROR_SUCCESS : GetLastError();
    bool result = false;
    if (ok) {
        result = true;
    } else if (initial_error == ERROR_IO_PENDING) {
        result = WaitOverlapped(pipe, overlapped, timeout_ms, &read, error);
    } else if (initial_error == ERROR_MORE_DATA) {
        if (error != nullptr) {
            *error = L"named pipe message exceeds receive buffer";
        }
    } else if (error != nullptr) {
        *error = L"ReadFile(pipe) failed: " + FormatWindowsError(initial_error);
    }

    CloseHandle(overlapped.hEvent);
    if (!result) {
        return false;
    }

    line.assign(buffer.data(), read / sizeof(wchar_t));
    while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) {
        line.pop_back();
    }
    return true;
}

}  // namespace

std::wstring MakePipePath(std::wstring_view correlation_id) {
    return L"\\\\.\\pipe\\wperf-" + std::wstring(correlation_id);
}

PipeServer::~PipeServer() {
    Close();
}

bool PipeServer::Create(std::wstring_view correlation_id, std::wstring* error) {
    Close();
    path_ = MakePipePath(correlation_id);

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;OW)(A;;GA;;;BA)(A;;GA;;;SY)", SDDL_REVISION_1,
            &descriptor, nullptr)) {
        if (error != nullptr) {
            *error = L"ConvertStringSecurityDescriptorToSecurityDescriptorW failed: " +
                     FormatWindowsError(GetLastError());
        }
        return false;
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    pipe_ = CreateNamedPipeW(
        path_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                           FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
        kPipeBufferSize, kPipeBufferSize, 0, &attributes);
    LocalFree(descriptor);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = L"CreateNamedPipeW failed: " +
                     FormatWindowsError(GetLastError());
        }
        return false;
    }
    connected_ = false;
    return true;
}

bool PipeServer::WaitForClient(uint32_t timeout_ms, std::wstring* error) {
    if (pipe_ == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = L"pipe has not been created";
        }
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        if (error != nullptr) {
            *error = L"CreateEventW failed: " + FormatWindowsError(GetLastError());
        }
        return false;
    }

    const BOOL connected = ConnectNamedPipe(pipe_, &overlapped);
    const DWORD code = connected ? ERROR_SUCCESS : GetLastError();
    bool ok = connected != FALSE;
    if (!ok && code == ERROR_IO_PENDING) {
        ok = WaitOverlapped(pipe_, overlapped, timeout_ms, nullptr, error);
    } else if (!ok && code == ERROR_PIPE_CONNECTED) {
        ok = true;
    } else if (!ok && error != nullptr) {
        *error = L"ConnectNamedPipe failed: " + FormatWindowsError(code);
    }
    CloseHandle(overlapped.hEvent);
    connected_ = ok;
    return ok;
}

bool PipeServer::SendLine(std::wstring_view line, std::wstring* error) {
    if (!IsConnected()) {
        if (error != nullptr) {
            *error = L"pipe client is not connected";
        }
        return false;
    }
    return WriteMessage(pipe_, line, error);
}

bool PipeServer::ReceiveLine(std::wstring& line, uint32_t timeout_ms,
                             std::wstring* error) {
    if (!IsConnected()) {
        if (error != nullptr) {
            *error = L"pipe client is not connected";
        }
        return false;
    }
    return ReadMessage(pipe_, line, timeout_ms, error);
}

bool PipeServer::HasData() const {
    if (!IsConnected()) {
        return false;
    }
    DWORD available = 0;
    return PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr) != FALSE &&
           available != 0;
}

bool PipeServer::IsConnected() const {
    return pipe_ != INVALID_HANDLE_VALUE && connected_;
}

void PipeServer::Close() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        if (connected_) {
            FlushFileBuffers(pipe_);
            DisconnectNamedPipe(pipe_);
        }
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    connected_ = false;
}

PipeClient::~PipeClient() {
    Close();
}

bool PipeClient::Connect(std::wstring_view pipe_path, uint32_t timeout_ms,
                         std::wstring* error) {
    Close();
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() <= deadline) {
        pipe_ = CreateFileW(pipe_path.data(), GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr);
            return true;
        }
        const DWORD code = GetLastError();
        if (code != ERROR_PIPE_BUSY && code != ERROR_FILE_NOT_FOUND) {
            if (error != nullptr) {
                *error = L"CreateFileW(pipe) failed: " + FormatWindowsError(code);
            }
            return false;
        }
        WaitNamedPipeW(pipe_path.data(), 100);
    }
    if (error != nullptr) {
        *error = L"timed out connecting to wperf control pipe";
    }
    return false;
}

bool PipeClient::SendLine(std::wstring_view line, std::wstring* error) {
    if (!IsConnected()) {
        if (error != nullptr) {
            *error = L"pipe server is not connected";
        }
        return false;
    }
    return WriteMessage(pipe_, line, error);
}

bool PipeClient::ReceiveLine(std::wstring& line, uint32_t timeout_ms,
                             std::wstring* error) {
    if (!IsConnected()) {
        if (error != nullptr) {
            *error = L"pipe server is not connected";
        }
        return false;
    }
    return ReadMessage(pipe_, line, timeout_ms, error);
}

bool PipeClient::HasData() const {
    if (!IsConnected()) {
        return false;
    }
    DWORD available = 0;
    return PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr) != FALSE &&
           available != 0;
}

bool PipeClient::ServerProcessId(uint32_t& process_id) const {
    process_id = 0;
    if (pipe_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    ULONG server_pid = 0;
    if (GetNamedPipeServerProcessId(pipe_, &server_pid) == FALSE) {
        return false;
    }
    process_id = server_pid;
    return true;
}

bool PipeClient::IsConnected() const {
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    DWORD available = 0;
    return PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr) != FALSE;
}

void PipeClient::Close() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

}  // namespace wperf
