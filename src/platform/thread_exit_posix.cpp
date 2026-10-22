// 线程退出时的通知，Linux / macOS：pthread 键的析构函数。主线程不经过它，进程退出时什么也不做。

#include "thread_exit.h"

#include <atomic>

#include <pthread.h>

namespace cu {
namespace detail {

namespace {

pthread_once_t key_once = PTHREAD_ONCE_INIT;
pthread_key_t key;
bool key_ready = false;
std::atomic<void (*)(void*)> registered(nullptr);
thread_local bool exiting = false;   // 通知发过以后为真，一直到线程结束

void on_key_destroyed(void* value) {
    exiting = true;
    registered.load(std::memory_order_relaxed)(value);
}

void create_key() {
    key_ready = pthread_key_create(&key, &on_key_destroyed) == 0;
}

}  // namespace

bool notify_at_thread_exit(void (*callback)(void*), void* value) noexcept {
    if (exiting) {
        return false;
    }

    registered.store(callback, std::memory_order_relaxed);
    pthread_once(&key_once, &create_key);
    return key_ready && pthread_setspecific(key, value) == 0;
}

}  // namespace detail
}  // namespace cu
