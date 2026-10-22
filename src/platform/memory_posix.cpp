// 虚拟内存的 Linux / macOS 部分。

#include "memory.h"

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

bool commit_pages(void* address, std::size_t size) noexcept {
    return ::mprotect(address, size, PROT_READ | PROT_WRITE) == 0;
}

void release_pages(void* address, std::size_t size) noexcept {
    ::munmap(address, size);
}

}  // namespace detail
}  // namespace cu
