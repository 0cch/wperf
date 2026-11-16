#include <windows.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) return 2;
    const std::wstring mode = argv[1], go = argv[2], stop = argv[3], data = argv[4];
    const ULONGLONG deadline = GetTickCount64() + 60000;
    while (GetFileAttributesW(go.c_str()) == INVALID_FILE_ATTRIBUTES && GetTickCount64() < deadline) Sleep(5);
    if (GetTickCount64() >= deadline) return 3;
    if (mode == L"heap") {
        HANDLE heap = HeapCreate(0, 0, 0);
        if (!heap) return 4;
        std::cout << reinterpret_cast<uintptr_t>(heap) << std::endl;
        std::vector<void*> held;
        for (unsigned i = 0; i < 16; ++i) held.push_back(HeapAlloc(heap, 0, 4096));
        while (GetFileAttributesW(stop.c_str()) == INVALID_FILE_ATTRIBUTES && GetTickCount64() < deadline) {
            for (unsigned i = 0; i < 64; ++i) {
                void* allocation = HeapAlloc(heap, 0, 1024);
                if (!allocation) return 5;
                void* resized = HeapReAlloc(heap, 0, allocation, 2048);
                if (!resized) { HeapFree(heap, 0, allocation); return 6; }
                static_cast<volatile BYTE*>(resized)[0] = 1;
                HeapFree(heap, 0, resized);
            }
            Sleep(5);
        }
        for (void* allocation : held) if (allocation) HeapFree(heap, 0, allocation);
        HeapDestroy(heap);
    } else if (mode == L"io") {
        HANDLE file = CreateFileW(data.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 7;
        void* buffer = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buffer) { CloseHandle(file); return 8; }
        while (GetFileAttributesW(stop.c_str()) == INVALID_FILE_ATTRIBUTES && GetTickCount64() < deadline) {
            LARGE_INTEGER offset{}; SetFilePointerEx(file, offset, nullptr, FILE_BEGIN);
            DWORD bytes = 0;
            if (!WriteFile(file, buffer, 4096, &bytes, nullptr) || bytes != 4096) return 9;
            SetFilePointerEx(file, offset, nullptr, FILE_BEGIN);
            if (!ReadFile(file, buffer, 4096, &bytes, nullptr) || bytes != 4096) return 10;
            Sleep(5);
        }
        VirtualFree(buffer, 0, MEM_RELEASE); CloseHandle(file);
    } else return 2;
    return 0;
}
