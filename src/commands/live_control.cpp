#include "commands/live_control.h"

#include "common.h"
#include "etw.h"

#include <iostream>

namespace wperf {

bool WaitForCaptureReady(PipeServer& pipe, HANDLE worker, const std::wstring& session,
                         uint32_t timeout_ms, std::wstring* error) {
    const uint64_t deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        if (pipe.HasData()) {
            std::wstring line;
            if (!pipe.ReceiveLine(line, 1000, error)) return false;
            if (line == L"READY " + session) return true;
            if (line.rfind(L"ERROR ", 0) == 0 || line.rfind(L"FINISHED ", 0) == 0) {
                if (error) *error = L"capture did not become ready: " + line;
                return false;
            }
        } else {
            if (WaitForSingleObject(worker, 0) != WAIT_TIMEOUT) {
                if (error) *error = L"capture worker exited before READY";
                return false;
            }
            Sleep(10);
        }
    }
    if (error) *error = L"capture READY timed out";
    return false;
}

LiveWorkerClient::~LiveWorkerClient() {
    Close();
}

bool LiveWorkerClient::Start(const LiveWorkerOptions& options,
                             bool request_elevation, std::wstring* error) {
    return StartWithExecutable(options, GetExecutablePath(), request_elevation,
                               error);
}

bool LiveWorkerClient::StartWithExecutable(const LiveWorkerOptions& options,
                                           const std::wstring& executable,
                                           bool request_elevation,
                                           std::wstring* error) {
    Close();
    const std::wstring correlation_id = NewId();
    const std::wstring nonce = NewId();
    session_name_ = options.session_name;
    if (!pipe_.Create(correlation_id, error)) {
        return false;
    }

    std::vector<std::wstring> arguments{
        L"--internal-live-worker",
        L"--pipe", MakePipePath(correlation_id),
        L"--nonce", nonce,
        L"--controller-pid", std::to_wstring(GetCurrentProcessId()),
        L"--mode", options.mode,
        L"--session", options.session_name,
        L"--interval", std::to_wstring(options.interval_ms),
        L"--duration", options.duration_text,
        L"--max", options.max_text,
        L"--event", options.event_filter,
        L"--profile-source", options.profile_source,
        L"--percent-limit", std::to_wstring(options.percent_limit),
    };
    if (!options.sort_field.empty()) {
        arguments.push_back(L"--sort");
        arguments.push_back(options.sort_field);
    }
    if (!options.symbol_path.empty()) {
        arguments.push_back(L"--symbol-path");
        arguments.push_back(options.symbol_path);
    }
    if (!options.pid_text.empty()) {
        arguments.push_back(L"--pid");
        arguments.push_back(options.pid_text);
    }
    if (options.json) {
        arguments.push_back(L"--json");
    }

    if (!LaunchWorkerProcess(executable, arguments, request_elevation, worker_,
                             error)) {
        pipe_.Close();
        return false;
    }
    if (!pipe_.WaitForClient(20000, error)) {
        Close();
        return false;
    }
    std::wstring hello;
    if (!pipe_.ReceiveLine(hello, 10000, error)) {
        Close();
        return false;
    }
    const std::wstring expected =
        L"HELLO " + nonce + L" " + std::to_wstring(worker_.process_id);
    if (hello != expected) {
        if (error != nullptr) {
            *error = L"live worker handshake mismatch";
        }
        Close();
        return false;
    }
    if (!pipe_.SendLine(L"START", error)) {
        Close();
        return false;
    }
    return true;
}

bool LiveWorkerClient::ReceiveLine(std::wstring& line, uint32_t timeout_ms,
                                   std::wstring* error) {
    return pipe_.ReceiveLine(line, timeout_ms, error);
}

bool LiveWorkerClient::SendStop(std::wstring* error) {
    return pipe_.SendLine(L"STOP", error);
}

bool LiveWorkerClient::HasData() const {
    return pipe_.HasData();
}

bool LiveWorkerClient::WaitForExit(uint32_t timeout_ms, DWORD& exit_code,
                                   std::wstring* error) {
    if (!WaitForProcessExit(worker_.process, timeout_ms, exit_code)) {
        if (error != nullptr) {
            *error = L"live worker did not exit before timeout";
        }
        return false;
    }
    return true;
}

bool LiveWorkerClient::StopAndDrain(uint32_t timeout_ms,DWORD& exit_code,std::wstring* error) {
    pipe_.SendLine(L"STOP",nullptr);
    const auto end=GetTickCount64()+timeout_ms;
    while(GetTickCount64()<end) {
        if(WaitForProcessExit(worker_.process,0,exit_code))return true;
        if(pipe_.HasData()) { std::wstring ignored; pipe_.ReceiveLine(ignored,1000,nullptr); }
        else Sleep(10);
    }
    if(error)*error=L"live worker teardown did not finish within its deadline";
    return false;
}

void LiveWorkerClient::Close() {
    if (worker_.process != nullptr) {
        std::wstring ignored;
        pipe_.SendLine(L"STOP", &ignored);
        DWORD exit_code = 0;
        if (!StopAndDrain(15000, exit_code, &ignored)) {
            StopSessionByName(session_name_, &ignored);
            TerminateProcess(worker_.process, 9);
            WaitForProcessExit(worker_.process, 5000, exit_code);
        }
        CloseHandle(worker_.process);
        worker_ = {};
    }
    pipe_.Close();
    session_name_.clear();
}

DWORD LiveWorkerClient::WorkerPid() const {
    return worker_.process_id;
}

}  // namespace wperf
