#include "crash.h"

#include "common.h"

#include <dbghelp.h>

namespace wperf {
namespace {

wchar_t g_dump_path[32768]{};

LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* exception_pointers) {
    if (g_dump_path[0] == L'\0') {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    HANDLE file = CreateFileW(g_dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION information{};
        information.ThreadId = GetCurrentThreadId();
        information.ExceptionPointers = exception_pointers;
        information.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          static_cast<MINIDUMP_TYPE>(
                              MiniDumpNormal | MiniDumpWithThreadInfo |
                              MiniDumpWithUnloadedModules),
                          exception_pointers != nullptr ? &information : nullptr,
                          nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void InstallCrashHandler(const std::filesystem::path& crash_directory,
                         const std::wstring& correlation_id) {
    std::wstring ignored;
    EnsureDirectory(crash_directory, &ignored);
    const std::filesystem::path dump =
        crash_directory / (L"wperf-" + correlation_id + L"-" +
                           std::to_wstring(GetCurrentProcessId()) + L".dmp");
    wcsncpy_s(g_dump_path, dump.c_str(), _TRUNCATE);
    SetUnhandledExceptionFilter(UnhandledExceptionFilter);
}

}  // namespace wperf
