#include "test_util.h"

#include <mem_core.h>

#include <atomic>
#include <thread>

using wavvix::mem::detail::mem_stats_get;
using wavvix::mem::detail::mem_stats_t;

int main() {
    // 默认首个 arena 为 64 MiB；先占住全部 slice，再并发触发扩容。
    const int initial_count = 1024;
    void* initial[initial_count];
    for (int i = 0; i < initial_count; ++i) {
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
    for (int i = 0; i < initial_count; ++i) wavvix::mem::free(initial[i]);
    wavvix::mem::collect();
    mem_stats_get(&stats);
    CHECK(stats.committed_bytes == 0);
    return memtest::summary("test_arena_growth");
}
