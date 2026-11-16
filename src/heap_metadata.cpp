#include "heap_metadata.h"
#include "common.h"
#include "etw_decode.h"
#include <tlhelp32.h>
#include <sstream>

namespace wperf {
bool ReadLoadedPeIdentity(HANDLE process, uint64_t base, uint64_t mapped_size, uint32_t& timestamp, uint32_t& checksum) {
    timestamp = checksum = 0;
    if (!process || mapped_size < sizeof(IMAGE_DOS_HEADER) || base > UINTPTR_MAX - mapped_size) return false;
    const auto read = [&](uint64_t offset, void* data, size_t size) {
        if (offset > mapped_size || size > mapped_size - offset) return false;
        SIZE_T returned = 0;
        return ReadProcessMemory(process, reinterpret_cast<const void*>(static_cast<uintptr_t>(base + offset)), data, size, &returned) && returned == size;
    };
    IMAGE_DOS_HEADER dos{};
    if (!read(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 1024 * 1024) return false;
    const uint64_t offset = static_cast<uint64_t>(dos.e_lfanew);
    DWORD signature = 0; IMAGE_FILE_HEADER file{}; WORD magic = 0;
    const uint64_t optional = offset + sizeof(signature) + sizeof(file);
    if (!read(offset, &signature, sizeof(signature)) || signature != IMAGE_NT_SIGNATURE ||
        !read(offset + sizeof(signature), &file, sizeof(file)) || !read(optional, &magic, sizeof(magic))) return false;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 header{};
        if (file.SizeOfOptionalHeader < sizeof(header) || !read(optional, &header, sizeof(header)) || header.SizeOfImage != mapped_size) return false;
        checksum = header.CheckSum;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 header{};
        if (file.SizeOfOptionalHeader < sizeof(header) || !read(optional, &header, sizeof(header)) || header.SizeOfImage != mapped_size) return false;
        checksum = header.CheckSum;
    } else return false;
    timestamp = file.TimeDateStamp;
    return true;
}

bool WriteHeapAttachSnapshot(uint32_t pid, const std::wstring& session, std::wstring* error) {
    ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!process.get()) { if (error) *error = L"heap module identity access is unavailable"; return false; }
    ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        if (error) *error = L"heap module snapshot unavailable: " + FormatWindowsError(GetLastError());
        return false;
    }
    REGHANDLE provider = 0;
    ULONG status = EventRegister(&kWperfMetadataGuid, nullptr, nullptr, &provider);
    if (status != ERROR_SUCCESS) return false;
    EVENT_DESCRIPTOR descriptor{}; descriptor.Id = 1; descriptor.Level = TRACE_LEVEL_INFORMATION;
    const ULONGLONG deadline = GetTickCount64() + 1000;
    while (!EventEnabled(provider, &descriptor) && GetTickCount64() < deadline) Sleep(5);
    const auto write = [&](const std::string& fields) {
        const std::string text = "wperf-meta-v1\nsession " + HexText(session) + "\n" + fields + "\n";
        if (text.size() > 8192) return ULONG{ERROR_BUFFER_OVERFLOW};
        EVENT_DATA_DESCRIPTOR data{}; EventDataDescCreate(&data, text.data(), static_cast<ULONG>(text.size()));
        return EventEnabled(provider, &descriptor) ? EventWrite(provider, &descriptor, 1, &data) : ULONG{ERROR_NOT_READY};
    };
    MODULEENTRY32W module{}; module.dwSize = sizeof(module);
    if (!Module32FirstW(snapshot.get(), &module)) status = GetLastError();
    else {
        status = write("snapshot-process " + std::to_string(pid) + " " + HexText(module.szModule));
        size_t count = 0;
        do {
            if (status != ERROR_SUCCESS) break;
            if (++count > 512) { status = ERROR_BUFFER_OVERFLOW; break; }
            uint32_t timestamp = 0, checksum = 0;
            const uint64_t base = reinterpret_cast<uintptr_t>(module.modBaseAddr);
            if (!ReadLoadedPeIdentity(process.get(), base, module.modBaseSize, timestamp, checksum)) continue;
            std::ostringstream fields;
            fields << "snapshot-image " << pid << ' ' << reinterpret_cast<uintptr_t>(module.modBaseAddr) << ' '
                   << module.modBaseSize << ' ' << timestamp << ' ' << checksum << ' ' << HexText(module.szExePath);
            status = write(fields.str());
        } while (Module32NextW(snapshot.get(), &module));
    }
    EventUnregister(provider);
    if (status != ERROR_SUCCESS && error) *error = L"heap attach metadata failed: " + FormatWindowsError(status);
    return status == ERROR_SUCCESS;
}
}  // namespace wperf
