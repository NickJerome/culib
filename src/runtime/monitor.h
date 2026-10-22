#pragma once

// 监控：在系统线程上，有工作线程忙时每 1 ms 看一遍，工作线程都闲着时一直睡。每次看三件事：
//
// - 一根线程这一个间隔里一根协程都没跑完，就把它格子里的协程挪进全局队列，别的线程能接着跑；
// - 有线程在同一根协程里待了 10 ms 以上、还有活排着、又没有闲着的线程，就让线程池补一根；卡住的恢复以后，
//   多出来的线程闲下来就退出；
// - 有活没人干、又没线程在找活，连着两次看到就叫醒一根：放活的一方本该叫的，这里兜底。

#include <atomic>

#include "thread_pool.h"
#include "timer.h"

namespace cu {
namespace detail {

struct runtime;

class monitor {
public:
    explicit monitor(runtime& rt) noexcept;
    monitor(const monitor&) = delete;
    monitor& operator=(const monitor&) = delete;

    // 系统线程每一轮调：到了时候就看一遍。返回下一次要看的时刻；工作线程都闲着时返回 timer_never，一直睡到
    // 有线程忙起来（wake）。
    timer_time turn(timer_time now) noexcept;

    // 工作线程从闲着变成忙时调：监控在睡的话叫醒它。
    void wake() noexcept;

private:
    bool busy() const noexcept;
    unsigned check_progress() noexcept;
    void adjust_threads(unsigned stalled) noexcept;
    void rescue() noexcept;

    runtime& rt_;
    std::atomic<bool> sleeping_;   // 为真时系统线程一直等，有线程忙起来要叫醒它

    // 只有系统线程读写
    timer_time next_tick_;
    bool unattended_;                    // 上一次看到有活没人干
    unsigned seen_[most_threads];        // 每根线程上次看到的进度
    unsigned stalled_[most_threads];     // 每根线程连着几次一根协程都没跑完
};

}  // namespace detail
}  // namespace cu
