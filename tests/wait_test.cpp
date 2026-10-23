#include <cu/coroutine.h>
#include <cu/test.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "runtime/timer.h"
#include "runtime/wait.h"

namespace {

using cu::detail::deadline_timer;
using cu::detail::timer_after;
using cu::detail::timer_now;
using cu::detail::waiter;

typedef std::chrono::steady_clock steady_clock;

void wake_up(void* context) noexcept {
    static_cast<waiter*>(context)->wake();
}

void count_up(void* context) noexcept {
    static_cast<std::atomic<int>*>(context)->fetch_add(1);
}

// 睡到定时器把自己叫醒，返回睡了多久。
steady_clock::duration sleep_on_a_timer(std::chrono::milliseconds duration) {
    const steady_clock::time_point start = steady_clock::now();
    waiter self;
    {
        deadline_timer timer(timer_after(timer_now(), std::chrono::nanoseconds(duration).count()), &wake_up, &self);
        self.sleep();
    }
    return steady_clock::now() - start;
}

// 回调先记下开始了，再慢慢跑完。
struct slow_callback {
    slow_callback() : started(0), finished(0) {}

    std::atomic<int> started;
    std::atomic<int> finished;
};

void run_slowly(void* context) noexcept {
    slow_callback* const self = static_cast<slow_callback*>(context);
    self->started.store(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    self->finished.store(1);
}

}  // namespace

TEST(wait, a_timer_wakes_a_plain_thread) {
    EXPECT(sleep_on_a_timer(std::chrono::milliseconds(20)) >= std::chrono::milliseconds(20), ==, true);
}

TEST(wait, a_timer_wakes_a_coroutine) {
    steady_clock::duration slept(0);
    cu::coroutine sleeping([&slept] { slept = sleep_on_a_timer(std::chrono::milliseconds(20)); });
    sleeping.join();
    EXPECT(slept >= std::chrono::milliseconds(20), ==, true);
}

// 到点之前收掉的定时器不会再调回调。
TEST(wait, a_timer_removed_before_its_deadline_never_fires) {
    std::atomic<int> fired(0);
    {
        deadline_timer timer(timer_after(timer_now(), 30000000), &count_up, &fired);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT(fired.load(), ==, 0);
}

// 回调正在系统线程上跑的时候收掉定时器：析构要等它跑完，之后它不会再碰 context。
TEST(wait, removing_a_timer_waits_for_a_running_callback) {
    slow_callback seen;
    {
        deadline_timer timer(timer_after(timer_now(), 1000000), &run_slowly, &seen);
        const steady_clock::time_point give_up = steady_clock::now() + std::chrono::seconds(5);
        while (seen.started.load() == 0 && steady_clock::now() < give_up) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    EXPECT(seen.started.load(), ==, 1);
    EXPECT(seen.finished.load(), ==, 1);
}

// 没有截止时刻的定时器什么也不放，也就不会响。
TEST(wait, a_timer_without_a_deadline_does_nothing) {
    std::atomic<int> fired(0);
    {
        deadline_timer timer(cu::detail::timer_never, &count_up, &fired);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT(fired.load(), ==, 0);
}
