#include "commands.h"
#include "commands/util.h"

#include "common.h"
#include "cpu_report.h"

#include <windows.h>
#include <shellapi.h>

#include <fcntl.h>
#include <io.h>

#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

struct ErrorDescriptor {
    const char* code;
    bool retryable;
    const char* action;
};

ErrorDescriptor DescribeError(int exit_code) {
    switch (exit_code) {
    case 2: return {"invalid_arguments", false, "fix_arguments"};
    case 3: return {"permission_required", false, "request_elevation"};
    case 4: return {"capture_failed", true, "retry_or_run_doctor"};
    case 5: return {"capability_unavailable", false, "run_capabilities"};
    case 6: return {"replay_or_quality_failed", false, "inspect_quality"};
    case 7: return {"target_launch_failed", false, "check_target"};
    case 8: return {"target_failed", false, "inspect_target_exit"};
    case 9: return {"internal_error", false, "retry_or_collect_doctor"};
    default: return {"unknown_error", false, "inspect_error"};
    }
}

std::wstring CommandName(const std::vector<std::wstring>& arguments) {
    return arguments.empty() ? std::wstring{} : arguments.front();
}

void WriteJsonError(int exit_code, const std::vector<std::wstring>& arguments,
                    const std::wstring& message) {
    const auto descriptor = DescribeError(exit_code);
    nlohmann::ordered_json output;
    output["ok"] = false;
    output["schema_version"] = 1;
    output["tool_version"] = wperf::ToUtf8(wperf::kVersion);
    output["command"] = wperf::ToUtf8(CommandName(arguments));
    output["error"] = {
        {"code", descriptor.code},
        {"exit_code", exit_code},
        {"retryable", descriptor.retryable},
        {"action", descriptor.action},
        {"message", wperf::ToUtf8(message)}
    };
    std::wcerr << wperf::ToUtf16(output.dump()) << L"\n";
}

}  // namespace

int wmain() {
    const int stdout_mode = _isatty(_fileno(stdout)) ? _O_U16TEXT : _O_U8TEXT;
    const int stderr_mode = _isatty(_fileno(stderr)) ? _O_U16TEXT : _O_U8TEXT;
    _setmode(_fileno(stdout), stdout_mode);
    _setmode(_fileno(stderr), stderr_mode);

    int argument_count = 0;
    LPWSTR* parsed = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (parsed == nullptr) {
        std::wcerr << L"wperf: CommandLineToArgvW failed\n";
        return 9;
    }

    std::vector<std::wstring> arguments;
    bool json_errors = false;
    bool after_separator = false;
    for (int index = 1; index < argument_count; ++index) {
        const std::wstring argument = parsed[index];
        if (argument == L"--") after_separator = true;
        if (!after_separator && argument == L"--json-errors") {
            json_errors = true;
            continue;
        }
        arguments.push_back(argument);
    }
    LocalFree(parsed);

    wperf::commands::SetJsonErrors(json_errors);
    std::wstring exception_message;
    int exit_code = 0;
    try {
        exit_code = wperf::RunCommandLine(arguments);
    } catch (const wperf::CpuReportLimitError& exception) {
        exception_message = wperf::ToUtf16(exception.what());
        if (!json_errors) std::wcerr << L"wperf: " << exception_message << L"\n";
        exit_code = 6;
    } catch (const std::exception& exception) {
        exception_message = L"unhandled exception: " + wperf::ToUtf16(exception.what());
        if (!json_errors) std::wcerr << L"wperf: " << exception_message << L"\n";
        exit_code = 9;
    } catch (...) {
        exception_message = L"unhandled unknown exception";
        if (!json_errors) std::wcerr << L"wperf: " << exception_message << L"\n";
        exit_code = 9;
    }

    if (json_errors && exit_code != 0) {
        std::wstring message = exception_message.empty()
                                   ? wperf::commands::LastErrorMessage()
                                   : exception_message;
        if (message.empty()) message = L"wperf failed without a diagnostic message";
        WriteJsonError(exit_code, arguments, message);
    }
    return exit_code;
}
