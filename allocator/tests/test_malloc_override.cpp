#include <edumalloc/edumalloc.h>

#include <cstdio>
#include <cstdlib>
#include <cerrno>

int main() {
    ed_stats_t before = {};
    ed_stats_get(&before);

    unsigned char* p = static_cast<unsigned char*>(std::malloc(37));
    if (!p) return 1;
    p[0] = 0x31;
    p[36] = 0x79;
    std::printf("malloc block: %p %u %u\n", static_cast<void*>(p),
                static_cast<unsigned>(p[0]), static_cast<unsigned>(p[36]));
    std::free(p);

    unsigned char* q = new unsigned char[37];
    q[0] = 0x32;
    q[36] = 0x78;
    std::printf("new block: %p %u %u\n", static_cast<void*>(q),
                static_cast<unsigned>(q[0]), static_cast<unsigned>(q[36]));
    delete[] q;

    ed_stats_t after = {};
    ed_stats_get(&after);
    if (after.thread_alloc_count < before.thread_alloc_count + 2 ||
        after.thread_free_count < before.thread_free_count + 2) {
        std::fprintf(stderr, "malloc/new did not use culib allocator\n");
        return 1;
    }

    volatile std::size_t impossible = static_cast<std::size_t>(-1);
    errno = 0;
    void* rejected = std::malloc(impossible);
    if (rejected || errno != ENOMEM) {
        std::fprintf(stderr, "malloc failure did not set ENOMEM\n");
        return 1;
    }
    return 0;
}
