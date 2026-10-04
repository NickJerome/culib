// coroutine 的 Windows 栈分配。和 POSIX 那边一样只做一件事，但 API 形状不同：
// VirtualAlloc 先 RESERVE 再 COMMIT，保护页用 PAGE_NOACCESS。

#include <cstddef>

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
    const std::size_t total = ((usable + page - 1) / page + 1) * page;

    // 一步到位 RESERVE | COMMIT：Windows 的 commit 不等于占物理内存，没碰到的页
    // 只记在页表里。分成两步只在「想按需增长栈」时才有意义，而我们不增长。
    void* const raw = ::VirtualAlloc(0, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!raw) return out;

    char* const base = static_cast<char*>(raw);
    // 栈往低地址长，所以保护页放在最低那一页。
    //
    // 用 PAGE_NOACCESS 而不是 PAGE_GUARD：PAGE_GUARD 是**一次性**的，第一次触碰抛
    // STATUS_GUARD_PAGE_VIOLATION 之后这一页就变成普通可写页了——那是给「按需增长
    // 的线程栈」设计的语义，对我们正相反：我们要的是每次都挡住。
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
    // MEM_RELEASE 时大小必须传 0，传别的会失败
    if (memory.base) ::VirtualFree(memory.base, 0, MEM_RELEASE);
}

}  // namespace detail
}  // namespace wavvix
