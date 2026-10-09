// POSIX 协程栈分配：mmap 加最低一页保护页。

#include <atomic>
#include <cstddef>
#include <limits>

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include "coroutine_detail.h"

namespace wavvix {
namespace detail {

namespace {

std::size_t page_size() noexcept {
    const long reported = ::sysconf(_SC_PAGESIZE);
    return reported > 0 ? static_cast<std::size_t>(reported) : 4096;
}

pthread_once_t runtime_exit_once = PTHREAD_ONCE_INIT;
pthread_key_t runtime_exit_key;
std::atomic<bool> runtime_exit_ready(false);

void init_runtime_exit_key() {
    if (::pthread_key_create(&runtime_exit_key, &runtime_thread_exit) == 0)
        runtime_exit_ready.store(true, std::memory_order_release);
}

}  // namespace

stack_memory allocate_stack(std::size_t usable) noexcept {
    stack_memory out;
    out.base = 0;
    out.total = 0;
    out.guard = 0;
    out.bucket = -1;

    const std::size_t page = page_size();
    if (page > (std::numeric_limits<std::size_t>::max)() / 2 ||
        usable > (std::numeric_limits<std::size_t>::max)() - (2 * page - 1))
        return out;
    // 向上取整，再为保护页多分一页。
    const std::size_t total = ((usable + page - 1) / page + 1) * page;

    void* const raw =
        ::mmap(0, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return out;

    char* const base = static_cast<char*>(raw);
    // 栈向低地址增长。
    if (::mprotect(base, page, PROT_NONE) != 0) {
        ::munmap(raw, total);
        return out;
    }

    out.base = base;
    out.total = total;
    out.guard = page;
    return out;
}

void release_stack(const stack_memory& memory) noexcept {
    if (memory.base) ::munmap(memory.base, memory.total);
}

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
