#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace wperf {
struct PoolTagSnapshot {
    uint32_t tag = 0, paged_allocs = 0, paged_frees = 0, nonpaged_allocs = 0, nonpaged_frees = 0;
    uint64_t paged_bytes = 0, nonpaged_bytes = 0;
};
std::vector<PoolTagSnapshot> QueryPoolTags(std::wstring* error);
std::wstring PoolTagText(uint32_t value);
}
