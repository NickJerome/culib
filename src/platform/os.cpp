// os 的平台无关部分：停止标志本身，以及 cpu_count。
// 碰系统调用的在 os_posix.cpp / os_win.cpp 里。

#include <culib/os.h>

#include <atomic>
#include <thread>

namespace wavvix {
namespace os {

namespace {

static_assert(ATOMIC_INT_LOCK_FREE == 2, "stop flag requires always-lock-free atomic int");
std::atomic<int> stop_flag(0);

}  // namespace

bool stop_requested() noexcept {
    return std::atomic_load_explicit(&stop_flag, std::memory_order_relaxed) != 0;
}

void request_stop() noexcept {
    // C++11 signal handlers allow non-member lock-free atomic operations.
    std::atomic_store_explicit(&stop_flag, 1, std::memory_order_relaxed);
}

unsigned cpu_count() noexcept {
    const unsigned reported = std::thread::hardware_concurrency();
    return reported != 0 ? reported : 1u;   // 0 对线程池没有意义
}

}  // namespace os
}  // namespace wavvix
