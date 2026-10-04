// os 的平台无关部分：停止标志本身，以及 cpu_count。
// 碰系统调用的在 os_posix.cpp / os_win.cpp 里。

#include <culib/os.h>

#include <thread>

namespace wavvix {
namespace os {

namespace detail {

// 停止标志。用 volatile sig_atomic_t 而不是 std::atomic<bool>：
// POSIX 只保证对 volatile sig_atomic_t 的赋值在信号处理函数里是安全的
// （signal-safety(7)），而 std::atomic 的无锁性是实现定义的，标准也没有
// 把它列进 async-signal-safe 的清单。单向翻转一个标志不需要更强的保证。
volatile std::sig_atomic_t stop_flag = 0;

}  // namespace detail

bool stop_requested() noexcept { return detail::stop_flag != 0; }

void request_stop() noexcept { detail::stop_flag = 1; }

unsigned cpu_count() noexcept {
    const unsigned reported = std::thread::hardware_concurrency();
    return reported != 0 ? reported : 1u;   // 0 对线程池没有意义
}

}  // namespace os
}  // namespace wavvix
