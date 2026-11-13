#include "lbr.h"

#include <algorithm>
#include <cstring>
#include <windows.h>

namespace wperf {
namespace {

uint32_t ReadU32(const BYTE* data, size_t size, size_t offset) {
    if (data == nullptr || offset + sizeof(uint32_t) > size) {
        return 0;
    }
    uint32_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

uint64_t ReadU64(const BYTE* data, size_t size, size_t offset) {
    if (data == nullptr || offset + sizeof(uint64_t) > size) {
        return 0;
    }
    uint64_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

uint64_t ReadPointer(const BYTE* data, size_t size, size_t offset,
                     uint32_t pointer_size) {
    return pointer_size == 8 ? ReadU64(data, size, offset)
                             : ReadU32(data, size, offset);
}

}  // namespace

bool DecodeLbrPayload(const BYTE* data, size_t size, uint32_t pointer_size,
                      LbrSample& sample) {
    sample = {};
    const size_t header_size = pointer_size == 8 ? 24 : 20;
    const size_t entry_size = static_cast<size_t>(pointer_size) * 3;
    if (data == nullptr || (pointer_size != 4 && pointer_size != 8) ||
        size < header_size || entry_size == 0) {
        return false;
    }
    sample.timestamp_qpc = ReadU64(data, size, 0);
    sample.pid = ReadU32(data, size, 8);
    sample.tid = ReadU32(data, size, 12);
    sample.filters = ReadU32(data, size, 16);
    const size_t available = (size - header_size) / entry_size;
    const size_t count = std::min<size_t>(available, 128);
    sample.branches.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const size_t offset = header_size + index * entry_size;
        const uint64_t from =
            ReadPointer(data, size, offset, pointer_size);
        const uint64_t to =
            ReadPointer(data, size, offset + pointer_size, pointer_size);
        if (from != 0 || to != 0) {
            sample.branches.push_back(LbrBranch{from, to});
        }
    }
    return true;
}

}  // namespace wperf