#include <mem_core.h>
#include <culib/scheduler.h>

#include <atomic>
#include <cstdio>
#include <thread>

namespace {
struct early_allocation {
    unsigned char* block;
    early_allocation() : block(new unsigned char[37]) {
        block[0] = 0x31;
        block[36] = 0x79;
    }
    ~early_allocation() { delete[] block; }
};
early_allocation early;
}

int main() {
    if (early.block[0] != 0x31 || early.block[36] != 0x79 ||
        wavvix::mem::usable_size(early.block) < 37) {
        std::fprintf(stderr, "global new failed before main\n");
        return 1;
    }
    mem_stats_t before = {};
    mem_stats_get(&before);

    unsigned char* p = new unsigned char[37];
    p[0] = 0x31;
    p[36] = 0x79;
    std::printf("new block: %p %u %u\n", static_cast<void*>(p),
                static_cast<unsigned>(p[0]), static_cast<unsigned>(p[36]));
    delete[] p;

    mem_stats_t after = {};
    mem_stats_get(&after);
    if (after.thread_alloc_count <= before.thread_alloc_count ||
        after.thread_free_count <= before.thread_free_count) {
        std::fprintf(stderr, "global new/delete did not use culib allocator\n");
        return 1;
    }

    std::atomic<int> completed(0);
    for (int round = 0; round < 4; ++round) {
        std::thread carriers[16];
        for (int i = 0; i < 16; ++i) {
            carriers[i] = std::thread([&] {
                wavvix::spawn([&] { completed.fetch_add(1); });
                wavvix::run();
            });
        }
        for (int i = 0; i < 16; ++i) carriers[i].join();
    }
    if (completed.load() != 64) {
        std::fprintf(stderr, "scheduler lost work with global new/delete override\n");
        return 1;
    }
    return 0;
}
