// 虚拟内存的 Windows 部分。

#include "memory.h"

#include "win32.h"

namespace cu {
namespace detail {

std::size_t page_size() noexcept {
    SYSTEM_INFO info;
    ::GetSystemInfo(&info);
    return info.dwPageSize != 0 ? info.dwPageSize : 4096;
}

void* reserve_pages(std::size_t size) noexcept {
    return ::VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS);
}

bool commit_pages(void* address, std::size_t size) noexcept {
    return ::VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != 0;
}

void release_pages(void* address, std::size_t) noexcept {
    ::VirtualFree(address, 0, MEM_RELEASE);
}

}  // namespace detail
}  // namespace cu
