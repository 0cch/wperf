#pragma once

#include <functional>
#include <string>
#include <vector>
#include "requests.h"

namespace wperf::cli {

// Receives a fully typed public command request.
using Dispatch = std::function<int(const CommandRequest&)>;

// Validates one public command, handles built-in help/version forms, and dispatches.
// Internal worker commands are handled by the caller before this function is called.
int Run(const std::vector<std::wstring>& arguments, const Dispatch& dispatch);
std::wstring GeneralHelp();
std::wstring CommandHelp(const std::wstring& command);
std::string MarkdownReference();

}  // namespace wperf::cli