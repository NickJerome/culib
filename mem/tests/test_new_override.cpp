#include <mem_core.h>

#include <cstdio>

int main() {
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
    return 0;
}
