#include "pages.h"

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
    return info.dwPageSize ? static_cast<std::size_t>(info.dwPageSize) : 4096;
}

void* allocate(std::size_t size) noexcept {
    return size ? ::VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : 0;
}

bool release(void* address, std::size_t) noexcept {
    return !address || ::VirtualFree(address, 0, MEM_RELEASE) != 0;
}

void* reserve(std::size_t size) noexcept {
    return size ? ::VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS) : 0;
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

} } } } // namespace wavvix::mem::detail::pages
