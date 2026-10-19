#include "pages.h"

#include <cstdint>
#include <limits>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wavvix { namespace mem { namespace detail { namespace pages {

std::size_t page_size() noexcept {
    SYSTEM_INFO info;
    ::GetSystemInfo(&info);
    if (info.dwPageSize == 0) {
        return 4096;
    }

    return static_cast<std::size_t>(info.dwPageSize);
}

void* allocate(std::size_t size) noexcept {
    if (size == 0) {
        return 0;
    }

    return ::VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

bool release(void* address, std::size_t) noexcept {
    if (!address) {
        return true;
    }

    return ::VirtualFree(address, 0, MEM_RELEASE) != 0;
}

void* reserve(std::size_t size, std::size_t alignment) noexcept {
    if (size == 0 || size > (std::numeric_limits<std::size_t>::max)() - alignment) {
        return 0;
    }

    // 保留区的起点本来就按 64 KiB 对齐
    const std::size_t granularity = 64 * 1024;
    if (alignment <= granularity) {
        return ::VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS);
    }

    // Windows 不能只释放保留区的一部分：先多保留 alignment 字节找到对齐的地址，整段放掉，
    // 再在那个地址上重新保留。两步之间别的线程可能占走这段地址，所以多试几次。
    for (int attempt = 0; attempt < 8; ++attempt) {
        void* const raw = ::VirtualAlloc(0, size + alignment, MEM_RESERVE, PAGE_NOACCESS);
        if (!raw) {
            return 0;
        }

        const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(raw);
        const std::uintptr_t aligned = (start + alignment - 1) & ~(std::uintptr_t(alignment) - 1);
        ::VirtualFree(raw, 0, MEM_RELEASE);
        void* const address = ::VirtualAlloc(reinterpret_cast<void*>(aligned), size, MEM_RESERVE, PAGE_NOACCESS);
        if (address) {
            return address;
        }
    }
    return 0;
}

bool commit(void* address, std::size_t size) noexcept {
    return ::VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != 0;
}

bool decommit(void* address, std::size_t size) noexcept {
    return ::VirtualFree(address, size, MEM_DECOMMIT) != 0;
}

bool protect(void* address, std::size_t size) noexcept {
    DWORD previous = 0;
    return ::VirtualProtect(address, size, PAGE_NOACCESS, &previous) != 0;
}

bool unprotect(void* address, std::size_t size) noexcept {
    DWORD previous = 0;
    return ::VirtualProtect(address, size, PAGE_READWRITE, &previous) != 0;
}

void no_huge_pages(void* address, std::size_t size) noexcept {
    (void)address;
    (void)size;
}

} } } } // namespace wavvix::mem::detail::pages
