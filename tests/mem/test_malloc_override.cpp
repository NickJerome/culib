// 共享库只导出 C 入口，所以用 malloc_usable_size 判断块是不是本分配器给的：37 字节在本分配器
// 落进 48 字节那一档，glibc 给 40。只检查 >= 37 的话，覆盖没生效时 glibc 也会让它通过。
#include <malloc.h>

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
const std::size_t culib_size_for_37 = 48;

struct early_allocation {
    unsigned char* block;
    early_allocation() : block(static_cast<unsigned char*>(std::malloc(37))) {
        if (!block) {
            std::abort();
        }
        block[0] = 0x31;
        block[36] = 0x79;
    }
    ~early_allocation() {
        std::free(block);
    }
};
early_allocation early;
}

int main() {
    if (early.block[0] != 0x31 || early.block[36] != 0x79 ||
        ::malloc_usable_size(early.block) != culib_size_for_37) {
        std::fprintf(stderr, "malloc override failed before main\n");
        return 1;
    }
    unsigned char* p = static_cast<unsigned char*>(std::malloc(37));
    if (!p || ::malloc_usable_size(p) != culib_size_for_37) {
        std::fprintf(stderr, "malloc did not use culib allocator\n");
        return 1;
    }
    p[0] = 0x31;
    p[36] = 0x79;
    std::printf("malloc block: %p %u %u\n", static_cast<void*>(p),
                static_cast<unsigned>(p[0]), static_cast<unsigned>(p[36]));
    std::free(p);

    unsigned char* q = new unsigned char[37];
    if (::malloc_usable_size(q) != culib_size_for_37) {
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

    // posix_memalign：对齐必须是 2 的幂且是指针大小的倍数，失败时不写出参
    void* out = NULL;
    if (::posix_memalign(&out, 4, 64) != EINVAL || ::posix_memalign(&out, 24, 64) != EINVAL || out != NULL) {
        std::fprintf(stderr, "posix_memalign accepted an invalid alignment\n");
        return 1;
    }
    if (::posix_memalign(&out, 256, 100) != 0 || !out || (reinterpret_cast<std::uintptr_t>(out) & 255) != 0) {
        std::fprintf(stderr, "posix_memalign rejected a valid request\n");
        return 1;
    }
    std::memset(out, 0x34, 100);
    std::free(out);
    return 0;
}
