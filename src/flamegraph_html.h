#pragma once

#include "flamegraph.h"

#include <cstdint>
#include <string>

namespace wperf::detail {

std::wstring RenderFlameGraphHtml(std::string data_json,
                                  const FlameGraphOptions& options,
                                  uint64_t total_weight,
                                  uint64_t stacks,
                                  uint64_t nodes);

}  // namespace wperf::detail
