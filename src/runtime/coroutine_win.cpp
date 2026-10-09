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

DWORD pool_exit_slot = FLS_OUT_OF_INDEXES;
DWORD pool_state_slot = TLS_OUT_OF_INDEXES;
INIT_ONCE pool_exit_once = INIT_ONCE_STATIC_INIT;

VOID CALLBACK pool_exit_callback(PVOID state) {
    stack_pool_thread_exit(state);
}

BOOL CALLBACK init_pool_exit_slot(PINIT_ONCE, PVOID, PVOID*) {
    pool_state_slot = ::TlsAlloc();
    if (pool_state_slot == TLS_OUT_OF_INDEXES) return TRUE;
    pool_exit_slot = ::FlsAlloc(&pool_exit_callback);
    if (pool_exit_slot == FLS_OUT_OF_INDEXES) {
        ::TlsFree(pool_state_slot);
        pool_state_slot = TLS_OUT_OF_INDEXES;
    }
    return TRUE;
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

void* stack_pool_current() noexcept {
    if (!::InitOnceExecuteOnce(&pool_exit_once, &init_pool_exit_slot, 0, 0) ||
        pool_state_slot == TLS_OUT_OF_INDEXES) return 0;
    return ::TlsGetValue(pool_state_slot);
}

bool stack_pool_set(void* state) noexcept {
    if (!::InitOnceExecuteOnce(&pool_exit_once, &init_pool_exit_slot, 0, 0) ||
        pool_state_slot == TLS_OUT_OF_INDEXES) return false;
    if (state == &stack_pool_exit_marker) return ::TlsSetValue(pool_state_slot, state) != 0;
    if (!::FlsSetValue(pool_exit_slot, state)) return false;
    if (::TlsSetValue(pool_state_slot, state)) return true;
    ::FlsSetValue(pool_exit_slot, 0);
    return false;
}

}  // namespace detail
}  // namespace wavvix
