#pragma once

#include <string>
#include <vector>

namespace wperf {

int RunScript(const std::vector<std::wstring>& arguments);
int RunTrace(const std::vector<std::wstring>& arguments);
int RunSched(const std::vector<std::wstring>& arguments);
int RunReport(const std::vector<std::wstring>& arguments);

}  // namespace wperf
