#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic_bool g_stop{false};
std::atomic<std::uint64_t> g_sink{0};

__declspec(noinline) void BurnInner(std::uint64_t& value, std::uint64_t& local) {
    for (int index = 0; index < 4096; ++index) {
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
        local += value;
    }
}

__declspec(noinline) void BurnOuter(std::uint64_t seed) {
    std::uint64_t value = seed ? seed : 1;
    std::uint64_t local = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        BurnInner(value, local);
    }
    g_sink.fetch_xor(local, std::memory_order_relaxed);
}

int RunCpu() {
    constexpr DWORD duration_ms = 1000;
    std::cout << "wperf fixture cpu pid=" << GetCurrentProcessId() << "\n";
    std::vector<std::thread> threads;
    const unsigned int thread_count = std::max(2u, std::thread::hardware_concurrency() / 2);
    for (unsigned int index = 0; index < thread_count; ++index) {
        threads.emplace_back(BurnOuter, static_cast<std::uint64_t>(index + 1));
    }
    Sleep(duration_ms);
    g_stop.store(true, std::memory_order_relaxed);
    for (auto& thread : threads) {
        thread.join();
    }
    std::cout << "wperf fixture cpu sink=" << g_sink.load(std::memory_order_relaxed) << "\n";
    return 0;
}

int RunScheduler() {
    constexpr DWORD duration_ms = 600;
    std::cout << "wperf fixture sched pid=" << GetCurrentProcessId() << "\n";
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wake == nullptr) {
        return 3;
    }
    std::atomic_bool stop{false};
    std::thread waiter([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            WaitForSingleObject(wake, 2);
            SwitchToThread();
        }
    });
    std::thread signaller([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            SetEvent(wake);
            Sleep(0);
            SwitchToThread();
            Sleep(1);
        }
    });
    Sleep(duration_ms);
    stop.store(true, std::memory_order_relaxed);
    SetEvent(wake);
    waiter.join();
    signaller.join();
    CloseHandle(wake);
    return 0;
}

int RunSyscall() {
    constexpr DWORD duration_ms = 50;
    std::cout << "wperf fixture syscall pid=" << GetCurrentProcessId() << "\n";
    const ULONGLONG deadline = GetTickCount64() + duration_ms;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        return 4;
    }
    std::uint64_t counter = 0;
    while (GetTickCount64() < deadline) {
        DWORD written = 0;
        char byte = static_cast<char>(counter);
        WriteFile(nul, &byte, 1, &written, nullptr);
        FILETIME time{};
        GetSystemTimeAsFileTime(&time);
        LARGE_INTEGER performance{};
        QueryPerformanceCounter(&performance);
        void* allocation = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (allocation != nullptr) {
            VirtualFree(allocation, 0, MEM_RELEASE);
        }
        Sleep(0);
        ++counter;
    }
    CloseHandle(nul);
    g_sink.fetch_add(counter, std::memory_order_relaxed);
    return 0;
}

int RunIo(const std::wstring& directory) {
    constexpr DWORD duration_ms = 500;
    constexpr DWORD buffer_size = 4096;
    std::cout << "wperf fixture io pid=" << GetCurrentProcessId() << "\n";
    const std::wstring path = directory + L"\\wperf-fixture-io-" +
                              std::to_wstring(GetCurrentProcessId()) + L".dat";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH |
                                  FILE_FLAG_NO_BUFFERING | FILE_FLAG_DELETE_ON_CLOSE,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 5;
    }
    void* buffer = VirtualAlloc(nullptr, buffer_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (buffer == nullptr) {
        CloseHandle(file);
        return 6;
    }
    std::memset(buffer, 0x5a, buffer_size);
    const ULONGLONG deadline = GetTickCount64() + duration_ms;
    std::uint64_t operations = 0;
    while (GetTickCount64() < deadline) {
        LARGE_INTEGER offset{};
        DWORD transferred = 0;
        if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) ||
            !WriteFile(file, buffer, buffer_size, &transferred, nullptr) ||
            transferred != buffer_size || !FlushFileBuffers(file) ||
            !SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) ||
            !ReadFile(file, buffer, buffer_size, &transferred, nullptr) ||
            transferred != buffer_size) {
            VirtualFree(buffer, 0, MEM_RELEASE);
            CloseHandle(file);
            return 7;
        }
        ++operations;
        Sleep(1);
    }
    VirtualFree(buffer, 0, MEM_RELEASE);
    CloseHandle(file);
    g_sink.fetch_add(operations, std::memory_order_relaxed);
    return 0;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::cerr << "usage: wperf_etw_fixture_workload <cpu|sched|syscall|io> <work-directory>\n";
        return 2;
    }
    const std::wstring mode = argv[1];
    if (mode == L"cpu") {
        return RunCpu();
    }
    if (mode == L"sched") {
        return RunScheduler();
    }
    if (mode == L"syscall") {
        return RunSyscall();
    }
    if (mode == L"io") {
        return RunIo(argv[2]);
    }
    return 2;
}
