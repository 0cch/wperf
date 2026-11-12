#pragma once

#include <string>
#include <vector>

namespace wperf {

int RunCommandLine(const std::vector<std::wstring>& arguments);
void PrintUsage();

}  // namespace wperf
