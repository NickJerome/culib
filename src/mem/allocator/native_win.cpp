#include "native.h"

#include <cstdlib>
#include <cstring>

#include "platform/win32.h"

namespace wavvix { namespace mem { namespace detail { namespace native {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
DWORD g_fls = FLS_OUT_OF_INDEXES;
// 退出标志放在原生 TLS 槽里：FLS 回调跑的时候，MinGW 模拟的 C++ TLS 可能已经清掉了
DWORD g_exiting = TLS_OUT_OF_INDEXES;
bool g_ready = false;                    // 只在锁内写

VOID NTAPI on_fls_destroyed(PVOID heap) {
    if (!heap) {
        return;
    }

    ::TlsSetValue(g_exiting, reinterpret_cast<LPVOID>(1));
    on_thread_exit(heap);
}

void write_text(const char* text) {
    HANDLE const out = ::GetStdHandle(STD_ERROR_HANDLE);
    if (out == 0 || out == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    ::WriteFile(out, text, static_cast<DWORD>(std::strlen(text)), &written, 0);
}

}  // namespace

void lock() noexcept {
    ::AcquireSRWLockExclusive(&g_lock);
}

void unlock() noexcept {
    ::ReleaseSRWLockExclusive(&g_lock);
}

void watch_fork() noexcept {
}

bool watch_thread_exit(void* heap) noexcept {
    lock();
    if (!g_ready) {
        if (g_exiting == TLS_OUT_OF_INDEXES) {
            g_exiting = ::TlsAlloc();
        }
        if (g_fls == FLS_OUT_OF_INDEXES) {
            g_fls = ::FlsAlloc(&on_fls_destroyed);
        }
        g_ready = g_exiting != TLS_OUT_OF_INDEXES && g_fls != FLS_OUT_OF_INDEXES;
    }
    const bool ready = g_ready;
    unlock();
    if (!ready || ::TlsGetValue(g_exiting)) {
        return false;
    }

    return ::FlsSetValue(g_fls, heap) != 0;
}

std::uint64_t now_ms() noexcept {
    return ::GetTickCount64();
}

std::uint64_t random_seed() noexcept {
    LARGE_INTEGER counter;
    ::QueryPerformanceCounter(&counter);
    const int local = 0;
    return static_cast<std::uint64_t>(counter.QuadPart) ^ (static_cast<std::uint64_t>(::GetCurrentProcessId()) << 32) ^
           (static_cast<std::uint64_t>(::GetCurrentThreadId()) << 48) ^ reinterpret_cast<std::uintptr_t>(&local);
}

void die(const char* line) noexcept {
    // 先解除线程退出回调：abort 之后 ExitProcess 还会跑本线程的 FLS 回调，
    // 不能在已经写坏的内存上再整理一遍（实测会把退出码从 3 改成 0xC0000005）
    if (g_fls != FLS_OUT_OF_INDEXES) {
        ::FlsSetValue(g_fls, 0);
    }

    write_text("culib mem: fatal: ");
    write_text(line);
    write_text("\n");
    std::abort();
}

} } } } // namespace wavvix::mem::detail::native
