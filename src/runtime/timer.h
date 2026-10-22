#pragma once

// 定时器：整个进程的定时器在一个最小堆里，一把锁护着。系统线程等 I/O 时最多等到最早的那个，
// 到点以后在系统线程上调它的 fire。定时器放在等待者自己那里（协程的栈上），堆里只存指针。

#include <cstddef>
#include <cstdint>
#include <limits>

namespace cu {
namespace detail {

// 时刻：steady_clock 的纳秒数。
typedef std::int64_t timer_time;

const timer_time timer_never = std::numeric_limits<timer_time>::max();

timer_time timer_now() noexcept;

// now 往后 duration 纳秒，算出来超过 timer_never 就是 timer_never。
timer_time timer_after(timer_time now, std::int64_t duration) noexcept;

struct timer_node {
    timer_node() noexcept : when(timer_never), index(0), fire(0) {}

    timer_time when;
    std::size_t index;                  // 在堆里的位置加 1；不在堆里为 0
    void (*fire)(timer_node* node);     // 到点时在系统线程上调，这时堆的锁已经放开
};

// 放进堆里，到 node.when 时调 node.fire。它成了最早的那个，就叫醒系统线程重新算要等多久。
void timer_add(timer_node& node) noexcept;

// 系统线程用：取下所有到 now 为止到点的定时器，按时刻先后调它们的 fire。返回下一个的时刻，没有时为 timer_never。
timer_time timers_run(timer_time now) noexcept;

}  // namespace detail
}  // namespace cu
