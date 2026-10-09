// POSIX 运行时线程退出槽。

#include <atomic>

#include <pthread.h>

#include "coroutine_detail.h"

namespace wavvix {
namespace detail {

namespace {

pthread_once_t runtime_exit_once = PTHREAD_ONCE_INIT;
pthread_key_t runtime_exit_key;
std::atomic<bool> runtime_exit_ready(false);

void init_runtime_exit_key() {
    if (::pthread_key_create(&runtime_exit_key, &runtime_thread_exit) == 0)
        runtime_exit_ready.store(true, std::memory_order_release);
}

}  // namespace

void* runtime_thread_state_current() noexcept {
    if (!runtime_exit_ready.load(std::memory_order_acquire)) return 0;
    return ::pthread_getspecific(runtime_exit_key);
}

bool runtime_thread_state_set(void* state) noexcept {
    return ::pthread_once(&runtime_exit_once, &init_runtime_exit_key) == 0 &&
           runtime_exit_ready.load(std::memory_order_acquire) &&
           ::pthread_setspecific(runtime_exit_key, state) == 0;
}

}  // namespace detail
}  // namespace wavvix
