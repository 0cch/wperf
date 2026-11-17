#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wperf {

struct FlameGraphOptions {
    std::wstring title = L"wperf flame graph";
    std::wstring weight_name = L"samples";
    std::wstring source;
    std::vector<std::pair<std::wstring, std::wstring>> details;
};

struct FlameGraphResult {
    std::wstring html;
    uint64_t total_weight = 0;
    uint64_t stacks = 0;
    uint64_t nodes = 0;
};

bool RenderFoldedFlameGraph(std::wstring_view folded,
                            const FlameGraphOptions& options,
                            FlameGraphResult& result,
                            std::wstring* error = nullptr);

}  // namespace wperf
