// 虚拟内存的 Windows 部分。保留的地址按分配粒度对齐，粒度是 64 KiB。

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

void* reserve_aligned_pages(std::size_t size, std::size_t alignment) noexcept {
    SYSTEM_INFO info;
    ::GetSystemInfo(&info);
    if (alignment > info.dwAllocationGranularity) {
        return 0;
    }
    return reserve_pages(size);
}

bool commit_pages(void* address, std::size_t size) noexcept {
    return ::VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != 0;
}

void decommit_pages(void* address, std::size_t size) noexcept {
    ::VirtualFree(address, size, MEM_DECOMMIT);
}

bool reuse_pages(void* address, std::size_t size) noexcept {
    return commit_pages(address, size);
}

void release_pages(void* address, std::size_t) noexcept {
    ::VirtualFree(address, 0, MEM_RELEASE);
}

void no_huge_pages(void*, std::size_t) noexcept {}

}  // namespace detail
}  // namespace cu
