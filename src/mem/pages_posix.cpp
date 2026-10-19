#include "pages.h"

#include <atomic>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>

namespace wavvix { namespace mem { namespace detail { namespace pages {

namespace {

// Linux 默认允许 overcommit：保留时直接映射成可读写，第一次写才分配物理页，提交什么都不用做，
// 归还用 MADV_DONTNEED。全程不改映射，也就不用拿进程级的映射写锁——改映射期间其他线程的
// 缺页都得等，多线程反复归还、复用内存时会慢 3 倍（见 docs/mem-internals.md）。
// 严格 overcommit（vm.overcommit_memory=2）下可读写的映射会立刻全额记账，所以和 macOS 一样
// 先映射成 PROT_NONE、用到再 mprotect。glibc、jemalloc、mimalloc 都读这个文件做同样的判断。
#if defined(__linux__)
char overcommit_mode() {
    const int fd = ::open("/proc/sys/vm/overcommit_memory", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return '0';
    }

    char mode = '0';
    if (::read(fd, &mode, 1) != 1) {
        mode = '0';
    }

    ::close(fd);
    return mode;
}

// 读一次就记住。不用函数内的静态变量：那要靠 libstdc++ 的 __cxa_guard 加锁，
// malloc 覆盖库就得为它依赖 libstdc++；几个线程同时第一次读也只是重复读一遍，结果一样。
bool lazy_commit() {
    static std::atomic<int> known(0);   // 0：还没读；1：允许 overcommit；2：严格模式
    int mode = known.load(std::memory_order_relaxed);
    if (mode == 0) {
        mode = overcommit_mode() == '2' ? 2 : 1;
        known.store(mode, std::memory_order_relaxed);
    }
    return mode == 1;
}
#else
bool lazy_commit() {
    return false;
}
#endif

}  // namespace

std::size_t page_size() noexcept {
    const long size = ::sysconf(_SC_PAGESIZE);
    if (size <= 0) {
        return 4096;
    }

    return static_cast<std::size_t>(size);
}

void* allocate(std::size_t size) noexcept {
    if (size == 0) {
        return 0;
    }

    void* const address = ::mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED) {
        return 0;
    }

    return address;
}

bool release(void* address, std::size_t size) noexcept {
    if (!address) {
        return true;
    }

    return ::munmap(address, size) == 0;
}

void* reserve(std::size_t size, std::size_t alignment) noexcept {
    if (size == 0 || size > (std::numeric_limits<std::size_t>::max)() - alignment) {
        return 0;
    }

    // 多映射 alignment 字节，再把两头不对齐的部分还掉
    const std::size_t total = size + alignment;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE) && !defined(__APPLE__)
    flags |= MAP_NORESERVE;
#endif
    const int protection = lazy_commit() ? PROT_READ | PROT_WRITE : PROT_NONE;
    void* const address = ::mmap(0, total, protection, flags, -1, 0);
    if (address == MAP_FAILED) {
        return 0;
    }

    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t aligned = (raw + alignment - 1) & ~(std::uintptr_t(alignment) - 1);
    const std::size_t head = static_cast<std::size_t>(aligned - raw);
    const std::size_t tail = total - head - size;
    if (head != 0) {
        ::munmap(address, head);
    }
    if (tail != 0) {
        ::munmap(reinterpret_cast<void*>(aligned + size), tail);
    }
    return reinterpret_cast<void*>(aligned);
}

bool commit(void* address, std::size_t size) noexcept {
    if (lazy_commit()) {
        return true;
    }

    return ::mprotect(address, size, PROT_READ | PROT_WRITE) == 0;
}

bool decommit(void* address, std::size_t size) noexcept {
    if (lazy_commit()) {
        return ::madvise(address, size, MADV_DONTNEED) == 0;
    }

    // 换成一段新的 PROT_NONE 映射：物理页立刻释放，严格 overcommit 下的记账也一并退回
    void* const fresh = ::mmap(address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return fresh == address;
}

bool protect(void* address, std::size_t size) noexcept {
    return ::mprotect(address, size, PROT_NONE) == 0;
}

bool unprotect(void* address, std::size_t size) noexcept {
    return ::mprotect(address, size, PROT_READ | PROT_WRITE) == 0;
}

void no_huge_pages(void* address, std::size_t size) noexcept {
#if defined(MADV_NOHUGEPAGE)
    (void)::madvise(address, size, MADV_NOHUGEPAGE);
#else
    (void)address;
    (void)size;
#endif
}

} } } } // namespace wavvix::mem::detail::pages
