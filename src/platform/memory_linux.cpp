// 虚拟内存的 Linux 部分。MADV_DONTNEED 当场把物理页还给系统，映射不变：页一直可读写，之后读到全零，再用不用
// 改映射。改映射（mprotect、MAP_FIXED）要拿进程的映射写锁，那期间别的线程缺页都得等。

#include "memory.h"

#include <sys/mman.h>

namespace cu {
namespace detail {

void decommit_pages(void* address, std::size_t size) noexcept {
    ::madvise(address, size, MADV_DONTNEED);
}

bool reuse_pages(void*, std::size_t) noexcept {
    return true;
}

// 内核不支持透明大页时 madvise 失败，那本来就没有大页，不管。
void no_huge_pages(void* address, std::size_t size) noexcept {
#if defined(MADV_NOHUGEPAGE)
    ::madvise(address, size, MADV_NOHUGEPAGE);
#else
    (void)address;
    (void)size;
#endif
}

}  // namespace detail
}  // namespace cu
