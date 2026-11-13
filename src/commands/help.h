#pragma once

#include <string_view>

namespace wperf::commands {

bool PrintCommandHelp(std::wstring_view command);
void PrintGeneralHelp();

}  // namespace wperf::commands