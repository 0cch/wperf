#include "dbghelp_loader.h"

#include "common.h"

#include <windows.h>
#include <delayimp.h>

#include <cstring>

namespace wperf {
namespace {

std::wstring ReadEnvironment(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) return {};
    value.resize(written);
    return value;
}

std::filesystem::path ExistingFile(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) return path;
    return {};
}

std::filesystem::path ExistingDirectoryFile(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        return ExistingFile(path / L"dbghelp.dll");
    }
    return ExistingFile(path);
}

}  // namespace

std::filesystem::path FindPreferredDbgHelpPath() {
    const std::wstring configured = ReadEnvironment(L"WPERF_DBGHELP_PATH");
    if (!configured.empty()) {
        const auto path = ExistingDirectoryFile(configured);
        if (!path.empty()) return path;
    }

    const auto executable_directory =
        std::filesystem::path(GetExecutablePath()).parent_path();
    const auto local = ExistingFile(executable_directory / L"dbghelp.dll");
    if (!local.empty()) return local;

    for (const wchar_t* variable : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
        const std::wstring root = ReadEnvironment(variable);
        if (root.empty()) continue;
        const auto candidate = ExistingFile(
            std::filesystem::path(root) / L"Windows Kits" / L"10" /
            L"Debuggers" / L"x64" / L"dbghelp.dll");
        if (!candidate.empty()) return candidate;
    }
    return {};
}

}  // namespace wperf

extern "C" FARPROC WINAPI WperfDelayLoadHook(unsigned notification,
                                             PDelayLoadInfo information) {
    if (notification == dliNotePreLoadLibrary && information != nullptr &&
        information->szDll != nullptr &&
        _stricmp(information->szDll, "dbghelp.dll") == 0) {
        const auto path = wperf::FindPreferredDbgHelpPath();
        if (!path.empty()) {
            if (HMODULE module = LoadLibraryW(path.c_str())) {
                return reinterpret_cast<FARPROC>(module);
            }
        }
    }
    return nullptr;
}

const PfnDliHook __pfnDliNotifyHook2 = WperfDelayLoadHook;
