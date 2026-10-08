// Windows 协程栈分配；保护页使用 PAGE_NOACCESS。

#include <cstddef>
#include <limits>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include "coroutine_detail.h"

namespace wavvix {
namespace detail {

namespace {

std::size_t page_size() noexcept {
    SYSTEM_INFO info;
    ::GetSystemInfo(&info);
    return info.dwPageSize != 0 ? static_cast<std::size_t>(info.dwPageSize) : 4096;
}

}  // namespace

stack_memory allocate_stack(std::size_t usable) noexcept {
    stack_memory out;
    out.base = 0;
    out.total = 0;
    out.guard = 0;

    const std::size_t page = page_size();
    if (page > (std::numeric_limits<std::size_t>::max)() / 2 ||
        usable > (std::numeric_limits<std::size_t>::max)() - (2 * page - 1))
        return out;
    const std::size_t total = ((usable + page - 1) / page + 1) * page;

    // 固定大小的栈一次性提交，不做按需增长。
    void* const raw = ::VirtualAlloc(0, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!raw) return out;

    char* const base = static_cast<char*>(raw);
    // 栈向低地址增长；最低一页始终不可访问。
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
    // MEM_RELEASE 要求大小为 0。
    if (memory.base) ::VirtualFree(memory.base, 0, MEM_RELEASE);
}

}  // namespace detail
}  // namespace wavvix
