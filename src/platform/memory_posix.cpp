// 虚拟内存的 Linux / macOS 部分。还内存的做法两边不一样，在 memory_linux.cpp / memory_macos.cpp。

#include "memory.h"

#include <cstdint>

#include <sys/mman.h>
#include <unistd.h>

namespace cu {
namespace detail {

std::size_t page_size() noexcept {
    const long size = ::sysconf(_SC_PAGESIZE);
    return size > 0 ? static_cast<std::size_t>(size) : 4096;
}

void* reserve_pages(std::size_t size) noexcept {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;   // 提交以后也只有用到的页才算进提交量；严格 overcommit 的系统上不管用
#endif
    void* const address = ::mmap(0, size, PROT_NONE, flags, -1, 0);
    return address == MAP_FAILED ? 0 : address;
}

// 多保留 alignment 字节，再把对不齐的头尾还掉。
void* reserve_aligned_pages(std::size_t size, std::size_t alignment) noexcept {
    if (alignment <= page_size()) {
        return reserve_pages(size);
    }

    char* const start = static_cast<char*>(reserve_pages(size + alignment));
    if (!start) {
        return 0;
    }
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(start);
    char* const aligned = start + (alignment - value % alignment) % alignment;
    if (aligned != start) {
        ::munmap(start, static_cast<std::size_t>(aligned - start));
    }

    char* const end = aligned + size;
    char* const reserved_end = start + size + alignment;
    if (end != reserved_end) {
        ::munmap(end, static_cast<std::size_t>(reserved_end - end));
    }
    return aligned;
}

bool commit_pages(void* address, std::size_t size) noexcept {
    return ::mprotect(address, size, PROT_READ | PROT_WRITE) == 0;
}

void release_pages(void* address, std::size_t size) noexcept {
    ::munmap(address, size);
}

}  // namespace detail
}  // namespace cu
