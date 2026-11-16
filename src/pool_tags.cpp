#include "pool_tags.h"
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace wperf {
std::wstring PoolTagText(uint32_t value) {
    std::wstring result;
    for (unsigned index = 0; index < 4; ++index) {
        const unsigned ch = (value >> (index * 8)) & 255u;
        result += ch >= 32 && ch <= 126 ? static_cast<wchar_t>(ch) : L'.';
    }
    return result;
}
std::vector<PoolTagSnapshot> QueryPoolTags(std::wstring* error) {
    using QueryFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
    const auto function = reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!function) { if (error) *error = L"system pool tag query is unavailable"; return {}; }
    struct NativeTag { ULONG tag, paged_allocs, paged_frees; SIZE_T paged_bytes; ULONG nonpaged_allocs, nonpaged_frees; SIZE_T nonpaged_bytes; };
    struct NativeInfo { ULONG count; NativeTag tags[1]; };
    std::vector<BYTE> buffer(65536);
    ULONG returned = 0;
    LONG status = 0;
    for (;;) {
        status = function(22, buffer.data(), static_cast<ULONG>(buffer.size()), &returned);  // SystemPoolTagInformation
        if (status >= 0) break;
        if (static_cast<ULONG>(status) != 0xc0000004u && static_cast<ULONG>(status) != 0xc0000023u) {
            if (error) *error = L"system pool tag query failed with NTSTATUS " + std::to_wstring(static_cast<ULONG>(status));
            return {};
        }
        const size_t size = std::max(buffer.size() * 2, static_cast<size_t>(returned));
        if (size > 16 * 1024 * 1024) { if (error) *error = L"pool tag snapshot exceeded 16 MiB budget"; return {}; }
        buffer.resize(size);
    }
    ULONG count = 0; std::memcpy(&count, buffer.data(), sizeof(count));
    const size_t available = returned ? std::min(buffer.size(), static_cast<size_t>(returned)) : buffer.size();
    constexpr size_t offset = offsetof(NativeInfo, tags);
    if (available < offset || count > (available - offset) / sizeof(NativeTag)) { if (error) *error = L"invalid pool tag snapshot length"; return {}; }
    std::vector<PoolTagSnapshot> result;
    result.reserve(count);
    for (ULONG index = 0; index < count; ++index) {
        NativeTag entry{}; std::memcpy(&entry, buffer.data() + offset + index * sizeof(entry), sizeof(entry));
        result.push_back({entry.tag, entry.paged_allocs, entry.paged_frees, entry.nonpaged_allocs, entry.nonpaged_frees,
                          entry.paged_bytes, entry.nonpaged_bytes});
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.paged_bytes + a.nonpaged_bytes > b.paged_bytes + b.nonpaged_bytes;
    });
    return result;
}
}
