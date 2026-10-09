#include "test_util.h"

#include <mem_core.h>

#include <atomic>
#include <cstdlib>
#include <thread>

int main() {
#if defined(_WIN32)
    CHECK(_putenv_s("WAVVIX_MEM_ARENA_RESERVE", "2M") == 0);
#else
    CHECK(setenv("WAVVIX_MEM_ARENA_RESERVE", "2M", 1) == 0);
#endif

    // 2 MiB arena 恰有 32 个 64 KiB slice，先全部占住再同时触发扩容。
    void* initial[32];
    for (int i = 0; i < 32; ++i) {
        initial[i] = wavvix::mem::alloc(60000);
        CHECK(initial[i] != NULL);
    }
    mem_stats_t stats;
    mem_stats_get(&stats);
    CHECK(stats.arena_count == 1);

    const int threads = 16;
    std::atomic<int> ready(0);
    std::atomic<bool> start(false);
    void* grown[threads] = {};
    std::thread workers[threads];
    for (int i = 0; i < threads; ++i) {
        workers[i] = std::thread([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            grown[i] = wavvix::mem::alloc(60000);
        });
    }
    while (ready.load(std::memory_order_acquire) != threads) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (int i = 0; i < threads; ++i) workers[i].join();

    for (int i = 0; i < threads; ++i) CHECK(grown[i] != NULL);
    mem_stats_get(&stats);
    CHECK_MSG(stats.arena_count == 2, "concurrent growth created redundant arenas");

    for (int i = 0; i < threads; ++i) wavvix::mem::free(grown[i]);
    for (int i = 0; i < 32; ++i) wavvix::mem::free(initial[i]);
    return memtest::summary("test_arena_growth");
}
