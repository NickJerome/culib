// 线程退出时的通知，Windows：FLS 的回调。退出标志放在原生 TLS 槽里：FLS 回调跑的时候，MinGW 模拟的 C++
// 线程局部变量可能已经清掉了。

#include "thread_exit.h"

#include "win32.h"

#include <atomic>

namespace cu {
namespace detail {

namespace {

INIT_ONCE slots_once = INIT_ONCE_STATIC_INIT;
DWORD fls_slot = FLS_OUT_OF_INDEXES;
DWORD exiting_slot = TLS_OUT_OF_INDEXES;
std::atomic<void (*)(void*)> registered(nullptr);

VOID NTAPI on_fls_destroyed(PVOID value) {
    if (!value) {
        return;
    }

    ::TlsSetValue(exiting_slot, reinterpret_cast<LPVOID>(1));
    registered.load(std::memory_order_relaxed)(value);
}

BOOL CALLBACK create_slots(PINIT_ONCE, PVOID, PVOID*) {
    exiting_slot = ::TlsAlloc();
    fls_slot = ::FlsAlloc(&on_fls_destroyed);
    return TRUE;
}

}  // namespace

bool notify_at_thread_exit(void (*callback)(void*), void* value) noexcept {
    registered.store(callback, std::memory_order_relaxed);
    ::InitOnceExecuteOnce(&slots_once, &create_slots, 0, 0);
    if (exiting_slot == TLS_OUT_OF_INDEXES || fls_slot == FLS_OUT_OF_INDEXES || ::TlsGetValue(exiting_slot)) {
        return false;
    }

    return ::FlsSetValue(fls_slot, value) != 0;
}

}  // namespace detail
}  // namespace cu
