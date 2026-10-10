#pragma once

#include <cstdio>
#include <cstdlib>
#include <type_traits>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#endif

namespace wavvix { namespace mem { namespace detail {

// 分配器锁必须活到 OS 退出回调结束，不登记 C++ 静态析构函数。
class allocator_mutex {
public:
    constexpr allocator_mutex() noexcept = default;
    allocator_mutex(const allocator_mutex&) = delete;
    allocator_mutex& operator=(const allocator_mutex&) = delete;

    void lock() noexcept {
#if defined(_WIN32)
        ::AcquireSRWLockExclusive(&native_);
#else
        if (::pthread_mutex_lock(&native_) != 0) fail();
#endif
    }

    void unlock() noexcept {
#if defined(_WIN32)
        ::ReleaseSRWLockExclusive(&native_);
#else
        if (::pthread_mutex_unlock(&native_) != 0) fail();
#endif
    }

private:
#if defined(_WIN32)
    SRWLOCK native_ = SRWLOCK_INIT;
#else
    pthread_mutex_t native_ = PTHREAD_MUTEX_INITIALIZER;
    static void fail() noexcept {
        std::fputs("culib mem: fatal: allocator mutex operation failed\n", stderr);
        std::abort();
    }
#endif
};

static_assert(std::is_trivially_destructible<allocator_mutex>::value,
              "allocator locks must remain alive through process teardown");

} } } // namespace wavvix::mem::detail
