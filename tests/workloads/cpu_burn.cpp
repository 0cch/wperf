#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace {
std::atomic_bool g_stop{false};
std::atomic<uint64_t> g_sink{0};

void Burn(std::uint64_t seed) {
    std::uint64_t value = seed ? seed : 1;
    std::uint64_t local = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        for (int index = 0; index < 4096; ++index) {
            value ^= value << 13;
            value ^= value >> 7;
            value ^= value << 17;
            local += value;
        }
    }
    g_sink.fetch_xor(local, std::memory_order_relaxed);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    uint64_t duration_ms = 2000;
    unsigned int thread_count = 0;
    if (argc >= 2) {
        duration_ms = wcstoull(argv[1], nullptr, 10);
    }
    if (argc >= 3) {
        thread_count = static_cast<unsigned int>(wcstoul(argv[2], nullptr, 10));
    }
    if (thread_count == 0) {
        thread_count = std::max(1u, std::thread::hardware_concurrency() / 2);
    }

    std::cout << "cpu_burn duration_ms=" << duration_ms
              << " threads=" << thread_count << "\n";
    std::vector<std::thread> threads;
    for (unsigned int index = 0; index < thread_count; ++index) {
        threads.emplace_back(Burn, static_cast<uint64_t>(index + 1));
    }
    Sleep(static_cast<DWORD>(duration_ms));
    g_stop.store(true, std::memory_order_relaxed);
    for (auto& thread : threads) {
        thread.join();
    }
    std::cout << "cpu_burn sink=" << g_sink.load(std::memory_order_relaxed) << "\n";
    return 0;
}
