#pragma once

#include <string>
#include <vector>

namespace wperf {

int RunAnnotate(const std::vector<std::wstring>& arguments);
int RunDiff(const std::vector<std::wstring>& arguments);
int RunArchive(const std::vector<std::wstring>& arguments);
int RunBuildIdList(const std::vector<std::wstring>& arguments);

}  // namespace wperf