#include "native.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

namespace wavvix { namespace mem { namespace detail { namespace native {

namespace {

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_key_t g_exit_key;
bool g_exit_key_ready = false;          // 只在锁内写
thread_local bool t_exiting = false;    // 退出回调跑过之后为真，一直保持到线程结束

void on_key_destroyed(void* heap) {
    t_exiting = true;
    on_thread_exit(heap);
}

void before_fork() {
    lock();
}

void after_fork_in_parent() {
    unlock();
}

void after_fork_in_child() {
    // 子进程里只剩调用 fork 的那个线程，锁直接重建
    pthread_mutex_init(&g_lock, 0);
}

void write_text(const char* text) {
    const ssize_t written = ::write(STDERR_FILENO, text, std::strlen(text));
    (void)written;
}

}  // namespace

void lock() noexcept {
    pthread_mutex_lock(&g_lock);
}

void unlock() noexcept {
    pthread_mutex_unlock(&g_lock);
}

void watch_fork() noexcept {
    // 先占位再登记：pthread_atfork 自己可能分配内存，嵌套回到这里时直接返回
    static std::atomic<bool> watched(false);
    if (watched.exchange(true)) {
        return;
    }

    pthread_atfork(&before_fork, &after_fork_in_parent, &after_fork_in_child);
}

bool watch_thread_exit(void* heap) noexcept {
    if (t_exiting) {
        return false;
    }

    lock();
    if (!g_exit_key_ready) {
        g_exit_key_ready = pthread_key_create(&g_exit_key, &on_key_destroyed) == 0;
    }
    const bool ready = g_exit_key_ready;
    unlock();
    if (!ready) {
        return false;
    }

    return pthread_setspecific(g_exit_key, heap) == 0;
}

std::uint64_t now_ms() noexcept {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000 + static_cast<std::uint64_t>(now.tv_nsec) / 1000000;
}

std::uint64_t random_seed() noexcept {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int local = 0;
    return (static_cast<std::uint64_t>(now.tv_sec) << 32) ^ static_cast<std::uint64_t>(now.tv_nsec) ^
           (static_cast<std::uint64_t>(getpid()) << 48) ^ reinterpret_cast<std::uintptr_t>(&local);
}

void die(const char* line) noexcept {
    // 先解除线程退出回调：已经发现内存被写坏，不能再在退出路径上整理它
    if (g_exit_key_ready) {
        pthread_setspecific(g_exit_key, 0);
    }

    write_text("culib mem: fatal: ");
    write_text(line);
    write_text("\n");
    std::abort();
}

} } } } // namespace wavvix::mem::detail::native
