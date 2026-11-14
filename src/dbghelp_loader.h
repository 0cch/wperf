#pragma once

#include <filesystem>

namespace wperf {

// Returns the preferred DbgHelp DLL for symbol-server support, or an empty path
// to let Windows use its default dbghelp.dll.
std::filesystem::path FindPreferredDbgHelpPath();

}  // namespace wperf
