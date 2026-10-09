#include "stack_pool.h"

#include <cstddef>
#include <limits>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace wavvix {
namespace mem {
namespace detail {

namespace {

std::size_t page_size() noexcept {
    SYSTEM_INFO info;
    ::GetSystemInfo(&info);
    return info.dwPageSize != 0 ? static_cast<std::size_t>(info.dwPageSize) : 4096;
}

}  // namespace

stack_memory allocate_stack(std::size_t usable) noexcept {
    stack_memory out = {0, 0, 0, -1};
    const std::size_t page = page_size();
    if (page > (std::numeric_limits<std::size_t>::max)() / 2 ||
        usable > (std::numeric_limits<std::size_t>::max)() - (2 * page - 1))
        return out;
    const std::size_t total = ((usable + page - 1) / page + 1) * page;
    void* const raw = ::VirtualAlloc(0, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!raw) return out;

    char* const base = static_cast<char*>(raw);
    DWORD previous = 0;
    if (!::VirtualProtect(base, page, PAGE_NOACCESS, &previous)) {
        ::VirtualFree(raw, 0, MEM_RELEASE);
        return out;
    }
    out.base = base;
    out.total = total;
    out.guard = page;
    return out;
}

void release_stack(const stack_memory& memory) noexcept {
    if (memory.base) ::VirtualFree(memory.base, 0, MEM_RELEASE);
}

}  // namespace detail
}  // namespace mem
}  // namespace wavvix
