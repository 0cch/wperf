#pragma once
#include <cstdint>
#include <string>
#include <windows.h>

namespace wperf {
// Ordinary-integrity attach snapshot; no elevated PE/file parsing.
bool WriteHeapAttachSnapshot(uint32_t pid, const std::wstring& session, std::wstring* error);
bool ReadLoadedPeIdentity(HANDLE process, uint64_t base, uint64_t mapped_size, uint32_t& timestamp, uint32_t& checksum);
}
