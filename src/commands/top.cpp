#include "commands/top.h"

#include "commands/help.h"
#include "commands/live_control.h"
#include "commands/util.h"
#include "top_renderer.h"
#include "common.h"
#include "live_data.h"
#include "symbolizer.h"
#include "elevation.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <iostream>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wperf {
using namespace commands;

namespace {

std::atomic_bool g_live_top_interrupted{false};

BOOL WINAPI LiveTopControlHandler(DWORD control_type) {
    if (control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT ||
        control_type == CTRL_CLOSE_EVENT || control_type == CTRL_LOGOFF_EVENT ||
        control_type == CTRL_SHUTDOWN_EVENT) {
        g_live_top_interrupted.store(true);
        return TRUE;
    }
    return FALSE;
}

bool ParseExitCode(const std::wstring& line, int& code) {
    if (line.rfind(L"FINISHED ", 0) != 0) {
        return false;
    }
    code = _wtoi(line.c_str() + 9);
    return true;
}

}  // namespace

int RunTop(const TopRequest& request) {
    const std::optional<uint32_t> pid = request.pid;
    const uint64_t interval_ms = request.interval_ms;
    const uint64_t iterations = request.iterations;
    const std::wstring profile_source = request.event.empty() ? L"cpu" : request.event;
    const std::wstring sort_field = request.sort;
    std::wstring symbol_path = request.symbol_path;
    std::filesystem::path archive_directory = request.archive;
    const double percent_limit = request.percent_limit;
    const bool elevate = request.elevate;
    if (interval_ms == 0) {
        PrintError(L"--interval requires a positive millisecond value");
        return 2;
    }
    if (!sort_field.empty() && sort_field != L"samples" && sort_field != L"comm" &&
        sort_field != L"dso" && sort_field != L"symbol") {
        PrintError(L"unsupported top sort field: " + sort_field);
        return 2;
    }
    if (!std::isfinite(percent_limit) || percent_limit < 0.0 ||
        percent_limit > 100.0) {
        PrintError(L"--percent-limit requires a number from 0 to 100");
        return 2;
    }
    bool elevated = false;
    std::wstring error;
    IsProcessElevated(elevated, &error);
    if (!elevated && !elevate) {
        PrintError(L"top requires administrator rights; rerun with --elevate");
        return 3;
    }

    LiveWorkerOptions options;
    options.mode = L"top";
    options.session_name = L"wperf-top-" + NewId();
    options.interval_ms = interval_ms;
    options.profile_source = profile_source;
    options.duration_text =
        iterations == 0 ? L"0" : std::to_wstring(interval_ms * iterations);
    if (pid.has_value()) {
        options.pid_text = std::to_wstring(*pid);
    }

    if (!archive_directory.empty()) {
        std::filesystem::path archive_path = archive_directory;
        if (archive_path.is_relative()) {
            archive_path = std::filesystem::absolute(archive_path);
        }
        if (!symbol_path.empty()) {
            symbol_path += L";";
        }
        symbol_path += L"srv*" + (archive_path / L"symbols").wstring();
    }
    auto symbolizer = std::make_shared<Symbolizer>();
    if (!symbolizer->Initialize(symbol_path, &error) && !error.empty()) {
        PrintError(L"live symbolization unavailable: " + error);
    }
    LiveWindowDecoder decoder;

    LiveWorkerClient worker;
    if (!worker.Start(options, !elevated && elevate, &error)) {
        PrintError(error);
        return 4;
    }
    AsyncTopRenderer renderer(symbolizer, archive_directory, pid, sort_field,
                              percent_limit, profile_source);

    g_live_top_interrupted.store(false);
    SetConsoleCtrlHandler(LiveTopControlHandler, TRUE);
    int exit_code = 0;
    bool finished = false;
    bool stop_sent = false;
    auto renderer_failed = [&] {
        std::wstring render_error;
        if (!renderer.Failed(&render_error)) return false;
        PrintError(render_error);
        exit_code = 5;
        if (!stop_sent) {
            worker.SendStop(nullptr);
            stop_sent = true;
        }
        return true;
    };
    while (!finished) {
        if (g_live_top_interrupted.load() && !stop_sent) {
            worker.SendStop(nullptr);
            stop_sent = true;
        }
        if (renderer_failed()) break;

        if (!worker.HasData()) {
            DWORD worker_exit = 0;
            std::wstring ignored;
            if (worker.WaitForExit(0, worker_exit, &ignored)) {
                if (!finished) {
                    PrintError(L"live worker exited unexpectedly with code " +
                               std::to_wstring(worker_exit));
                    exit_code = static_cast<int>(worker_exit);
                }
                break;
            }
            Sleep(20);
            continue;
        }

        std::wstring line;
        if (!worker.ReceiveLine(line, 5000, &error)) {
            PrintError(error);
            exit_code = 5;
            break;
        }
        if (line.rfind(L"TOP2_BEGIN ", 0) == 0) {
            if (!decoder.Begin(line, &error)) {
                PrintError(error);
                exit_code = 5;
                break;
            }
        } else if (line == L"TOP2_END") {
            TraceData window;
            uint64_t window_interval = interval_ms;
            if (!decoder.Finish(window, window_interval, &error)) {
                PrintError(error);
                exit_code = 5;
                break;
            }
            renderer.Push(std::move(window), window_interval);
        } else if (line.rfind(L"TOP2_", 0) == 0) {
            if (!decoder.Consume(line, &error)) {
                PrintError(error);
                exit_code = 5;
                break;
            }
        } else if (line.rfind(L"ERROR ", 0) == 0) {
            PrintError(line.substr(6));
        } else if (line.rfind(L"WARN ", 0) == 0) {
            std::wcerr << L"wperf: " << line.substr(5) << L"\n";
        } else if (ParseExitCode(line, exit_code)) {
            finished = true;
        } else if (line.rfind(L"STATUS ", 0) == 0) {
            std::wcerr << L"status: " << line.substr(7) << L"\n";
        }
    }

    SetConsoleCtrlHandler(LiveTopControlHandler, FALSE);
    worker.Close();
    const bool interrupted = g_live_top_interrupted.load();
    const bool drain = exit_code == 0 && !interrupted;
    const bool rendered = renderer.Close(drain, drain ? 30000 : 0);
    if (!rendered && drain) {
        if (!renderer_failed()) {
            PrintError(L"live top output is incomplete");
            exit_code = 5;
        }
    } else if (exit_code == 0) {
        renderer_failed();
    }
    if (const uint64_t dropped = renderer.DroppedWindows(); dropped != 0) {
        std::wcerr << L"wperf: dropped " << dropped
                   << L" top windows that could not be rendered\n";
    }
    return exit_code;
}

}  // namespace wperf
