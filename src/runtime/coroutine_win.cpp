// Windows 协程栈分配；保护页使用 PAGE_NOACCESS。

#include <atomic>
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

DWORD runtime_exit_slot = FLS_OUT_OF_INDEXES;
DWORD runtime_state_slot = TLS_OUT_OF_INDEXES;
INIT_ONCE runtime_exit_once = INIT_ONCE_STATIC_INIT;
std::atomic<bool> runtime_exit_ready(false);

VOID CALLBACK runtime_exit_callback(PVOID state) {
    runtime_thread_exit(state);
}

BOOL CALLBACK init_runtime_exit_slot(PINIT_ONCE, PVOID, PVOID*) {
    runtime_state_slot = ::TlsAlloc();
    if (runtime_state_slot == TLS_OUT_OF_INDEXES) return TRUE;
    runtime_exit_slot = ::FlsAlloc(&runtime_exit_callback);
    if (runtime_exit_slot == FLS_OUT_OF_INDEXES) {
        ::TlsFree(runtime_state_slot);
        runtime_state_slot = TLS_OUT_OF_INDEXES;
    } else {
        runtime_exit_ready.store(true, std::memory_order_release);
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

void* runtime_thread_state_current() noexcept {
    if (!runtime_exit_ready.load(std::memory_order_acquire)) return 0;
    return ::TlsGetValue(runtime_state_slot);
}

bool runtime_thread_state_set(void* state) noexcept {
    if (!::InitOnceExecuteOnce(&runtime_exit_once, &init_runtime_exit_slot, 0, 0) ||
        !runtime_exit_ready.load(std::memory_order_acquire)) return false;
    if (state == &runtime_thread_exit_marker) {
        if (!::TlsSetValue(runtime_state_slot, state)) return false;
        return ::FlsSetValue(runtime_exit_slot, 0) != 0;
    }
    if (!::FlsSetValue(runtime_exit_slot, state)) return false;
    if (::TlsSetValue(runtime_state_slot, state)) return true;
    ::FlsSetValue(runtime_exit_slot, 0);
    return false;
}

}  // namespace detail
}  // namespace wavvix
