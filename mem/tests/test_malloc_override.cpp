#include <culib/mem.h>

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <unistd.h>

extern "C" void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept;
extern "C" void* memalign(std::size_t alignment, std::size_t size) noexcept;
extern "C" void* valloc(std::size_t size) noexcept;

namespace {
struct early_allocation {
    unsigned char* block;
    early_allocation() : block(static_cast<unsigned char*>(std::malloc(37))) {
        if (!block) std::abort();
        block[0] = 0x31;
        block[36] = 0x79;
    }
    ~early_allocation() { std::free(block); }
};
early_allocation early;
}

int main() {
    if (early.block[0] != 0x31 || early.block[36] != 0x79 ||
        wavvix::mem::usable_size(early.block) < 37) {
        std::fprintf(stderr, "malloc override failed before main\n");
        return 1;
    }
    unsigned char* p = static_cast<unsigned char*>(std::malloc(37));
    if (!p || wavvix::mem::usable_size(p) < 37) {
        std::fprintf(stderr, "malloc did not use culib allocator\n");
        return 1;
    }
    p[0] = 0x31;
    p[36] = 0x79;
    std::printf("malloc block: %p %u %u\n", static_cast<void*>(p),
                static_cast<unsigned>(p[0]), static_cast<unsigned>(p[36]));
    std::free(p);

    unsigned char* q = new unsigned char[37];
    if (wavvix::mem::usable_size(q) < 37) {
        std::fprintf(stderr, "new did not use culib allocator\n");
        return 1;
    }
    q[0] = 0x32;
    q[36] = 0x78;
    std::printf("new block: %p %u %u\n", static_cast<void*>(q),
                static_cast<unsigned>(q[0]), static_cast<unsigned>(q[36]));
    delete[] q;

    volatile std::size_t impossible = static_cast<std::size_t>(-1);
    void* (*volatile allocate)(std::size_t) = &std::malloc;
    errno = 0;
    void* rejected = allocate(impossible);
    if (rejected || errno != ENOMEM) {
        std::fprintf(stderr, "malloc failure did not set ENOMEM\n");
        return 1;
    }

    errno = 0;
    void* (*volatile aligned_allocate)(std::size_t, std::size_t) = &::aligned_alloc;
    rejected = aligned_allocate(64, 100);
    if (rejected || errno != EINVAL) {
        std::fprintf(stderr, "C11 aligned_alloc accepted a non-multiple size\n");
        return 1;
    }
    void* aligned = ::aligned_alloc(64, 128);
    if (!aligned || (reinterpret_cast<std::uintptr_t>(aligned) & 63) != 0) {
        std::fprintf(stderr, "C11 aligned_alloc rejected a valid request\n");
        return 1;
    }
    std::memset(aligned, 0x31, 128);
    std::free(aligned);

    aligned = ::memalign(64, 100);
    if (!aligned || (reinterpret_cast<std::uintptr_t>(aligned) & 63) != 0) {
        std::fprintf(stderr, "memalign rejected an arbitrary size\n");
        return 1;
    }
    std::memset(aligned, 0x32, 100);
    std::free(aligned);

    aligned = ::valloc(100);
    const long page = ::sysconf(_SC_PAGESIZE);
    if (!aligned || page <= 0 ||
        reinterpret_cast<std::uintptr_t>(aligned) % static_cast<std::size_t>(page) != 0) {
        std::fprintf(stderr, "valloc rejected an arbitrary size\n");
        return 1;
    }
    std::memset(aligned, 0x33, 100);
    std::free(aligned);
    return 0;
}
