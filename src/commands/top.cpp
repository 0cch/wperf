#include "commands/top.h"

#include "commands/help.h"
#include "commands/live_control.h"
#include "commands/util.h"
#include "common.h"
#include "elevation.h"

#include <atomic>
#include <iostream>
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

void BeginTopRender() {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode)) {
        if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 ||
            SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            std::wcout << L"\x1b[2J\x1b[H";
            return;
        }
    }
    std::wcout << L"\n===== wperf top =====\n";
}

bool ParseExitCode(const std::wstring& line, int& code) {
    if (line.rfind(L"FINISHED ", 0) != 0) {
        return false;
    }
    code = _wtoi(line.c_str() + 9);
    return true;
}

}  // namespace

int RunTop(const std::vector<std::wstring>& arguments) {
    std::optional<uint32_t> pid;
    uint64_t interval_ms = 1000;
    uint64_t iterations = 0;
    std::wstring profile_source = L"cpu";
    std::wstring sort_field;
    std::wstring symbol_path;
    double percent_limit = 1.0;
    bool elevate = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-p" || arguments[index] == L"--pid") {
            uint32_t value = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], value)) {
                PrintError(L"--pid requires a process id");
                return 2;
            }
            pid = value;
        } else if (arguments[index] == L"--interval") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], interval_ms) || interval_ms == 0) {
                PrintError(L"--interval requires a positive millisecond value");
                return 2;
            }
        } else if (arguments[index] == L"--iterations") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], iterations)) {
                PrintError(L"--iterations requires a non-negative count");
                return 2;
            }
        } else if (arguments[index] == L"-e" || arguments[index] == L"--event") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--event requires cpu or an ETW PMC source");
                return 2;
            }
            profile_source = arguments[++index];
        } else if (arguments[index] == L"--sort") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--sort requires samples, comm, dso, or symbol");
                return 2;
            }
            sort_field = arguments[++index];
            if (sort_field != L"samples" && sort_field != L"comm" &&
                sort_field != L"dso" && sort_field != L"symbol") {
                PrintError(L"unsupported top sort field: " + sort_field);
                return 2;
            }
        } else if (arguments[index] == L"--percent-limit") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--percent-limit requires a number");
                return 2;
            }
            wchar_t* end = nullptr;
            percent_limit = wcstod(arguments[++index].c_str(), &end);
            if (end == nullptr || *end != L'\0' || percent_limit < 0.0 ||
                percent_limit > 100.0) {
                PrintError(L"--percent-limit requires a number from 0 to 100");
                return 2;
            }
        } else if (arguments[index] == L"--symbol-path") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--symbol-path requires a directory");
                return 2;
            }
            symbol_path = arguments[++index];
        } else if (arguments[index] == L"--elevate") {
            elevate = true;
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            PrintCommandHelp(L"top");
            return 0;
        } else {
            PrintError(L"unknown top option: " + arguments[index]);
            return 2;
        }
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
    options.sort_field = sort_field;
    if (!symbol_path.empty()) {
        std::filesystem::path path(symbol_path);
        if (path.is_relative()) {
            path = std::filesystem::absolute(path);
        }
        options.symbol_path = path.wstring();
    }
    options.percent_limit = percent_limit;
    options.duration_text =
        iterations == 0 ? L"0" : std::to_wstring(interval_ms * iterations);
    if (pid.has_value()) {
        options.pid_text = std::to_wstring(*pid);
    }

    LiveWorkerClient worker;
    if (!worker.Start(options, !elevated && elevate, &error)) {
        PrintError(error);
        return 4;
    }

    g_live_top_interrupted.store(false);
    SetConsoleCtrlHandler(LiveTopControlHandler, TRUE);
    int exit_code = 0;
    bool finished = false;
    bool stop_sent = false;
    while (!finished) {
        if (g_live_top_interrupted.load() && !stop_sent) {
            worker.SendStop(nullptr);
            stop_sent = true;
        }

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
        if (line == L"TOP_BEGIN " || line.rfind(L"TOP_BEGIN ", 0) == 0) {
            BeginTopRender();
        } else if (line.rfind(L"TOP_LINE ", 0) == 0) {
            std::wcout << line.substr(9) << L"\n";
            std::wcout.flush();
        } else if (line == L"TOP_END") {
            std::wcout.flush();
        } else if (line.rfind(L"ERROR ", 0) == 0) {
            PrintError(line.substr(6));
        } else if (ParseExitCode(line, exit_code)) {
            finished = true;
        }
    }

    SetConsoleCtrlHandler(LiveTopControlHandler, FALSE);
    worker.Close();
    return exit_code;
}

}  // namespace wperf