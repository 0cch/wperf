#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wperf {

struct LbrBranch {
    uint64_t from_address = 0;
    uint64_t to_address = 0;
};

struct LbrSample {
    uint64_t timestamp_qpc = 0;
    uint32_t pid = 0;
    uint32_t tid = 0;
    uint32_t filters = 0;
    std::vector<LbrBranch> branches;
};

bool DecodeLbrPayload(const BYTE* data, size_t size, uint32_t pointer_size,
                      LbrSample& sample);

}  // namespace wperf