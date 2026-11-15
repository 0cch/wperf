#include "commands.h"

#include "common.h"
#include "cpu_report.h"

#include <windows.h>
#include <shellapi.h>

#include <fcntl.h>
#include <io.h>

#include <iostream>
#include <string>
#include <vector>

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
    for (int index = 1; index < argument_count; ++index) {
        arguments.emplace_back(parsed[index]);
    }
    LocalFree(parsed);

    try {
        return wperf::RunCommandLine(arguments);
    } catch (const wperf::CpuReportLimitError& exception) {
        std::wcerr << L"wperf: " << wperf::ToUtf16(exception.what()) << L"\n";
        return 6;
    } catch (const std::exception& exception) {
        std::wcerr << L"wperf: unhandled exception: "
                   << wperf::ToUtf16(exception.what()) << L"\n";
        return 9;
    } catch (...) {
        std::wcerr << L"wperf: unhandled unknown exception\n";
        return 9;
    }
}
