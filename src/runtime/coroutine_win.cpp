// Windows 运行时线程退出槽。

#include <atomic>

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
