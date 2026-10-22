// 虚拟内存的 macOS 部分。macOS 的 MADV_DONTNEED 不一定当场还物理页，所以在原地重新映射一段不可访问的页：
// 物理页当场释放，之后要用再改成可读写。

#include "memory.h"

#include <sys/mman.h>

namespace cu {
namespace detail {

void decommit_pages(void* address, std::size_t size) noexcept {
    ::mmap(address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
}

bool reuse_pages(void* address, std::size_t size) noexcept {
    return commit_pages(address, size);
}

void no_huge_pages(void*, std::size_t) noexcept {}

}  // namespace detail
}  // namespace cu
