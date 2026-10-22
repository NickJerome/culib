// 多根工作线程时的调度：一直不让出、阻塞在系统调用里的协程不挡别的协程，协程会分到几根线程上，
// 跨线程叫醒不丢。机器只有一个核时只有一根工作线程，这些情形靠监控补线程，最多多等约 10 ms。

#include <cu/coroutine.h>
#include <cu/test.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

typedef std::chrono::steady_clock clock_type;

// 在普通线程上等 flag 变真，最多等 limit。
bool wait_until(const std::atomic<bool>& flag, std::chrono::milliseconds limit) {
    const clock_type::time_point deadline = clock_type::now() + limit;
    while (!flag.load()) {
        if (clock_type::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// 一直算，不让出，算 duration 这么久。
void compute_for(std::chrono::milliseconds duration) {
    const clock_type::time_point until = clock_type::now() + duration;
    while (clock_type::now() < until) {
    }
}

}  // namespace

TEST(scheduler, a_coroutine_that_never_waits_does_not_stop_the_others) {
    std::atomic<bool> stop(false);
    std::atomic<bool> gave_up(false);
    cu::coroutine spinner([&stop, &gave_up] {
        const clock_type::time_point deadline = clock_type::now() + std::chrono::seconds(10);
        while (!stop.load()) {
            if (clock_type::now() > deadline) {
                gave_up.store(true);
                return;
            }
        }
    });
    cu::coroutine stopper([&stop] { stop.store(true); });
    stopper.join();
    spinner.join();
    EXPECT(gave_up.load(), ==, false);
}

TEST(scheduler, a_coroutine_blocked_in_a_system_call_does_not_stop_the_one_it_started) {
    std::atomic<bool> ran(false);
    cu::coroutine blocker([&ran] {
        cu::coroutine started([&ran] { ran.store(true); });   // 排进这根线程的格子
        std::this_thread::sleep_for(std::chrono::milliseconds(500));   // 整根线程停住
        started.join();
    });
    EXPECT(wait_until(ran, std::chrono::milliseconds(400)), ==, true);
    blocker.join();
}

TEST(scheduler, coroutines_spread_over_several_threads) {
    std::mutex lock;
    std::set<std::thread::id> threads;
    std::vector<cu::coroutine> group;
    for (int i = 0; i < 8; ++i) {
        group.emplace_back([&lock, &threads] {
            {
                std::lock_guard<std::mutex> held(lock);
                threads.insert(std::this_thread::get_id());
            }
            compute_for(std::chrono::milliseconds(20));
        });
    }
    for (std::size_t i = 0; i < group.size(); ++i) {
        group[i].join();
    }
    EXPECT(threads.size(), >=, 2u);
}

TEST(scheduler, joins_across_threads_never_lose_a_wakeup) {
    static const int roots = 64;
    static const int rounds = 300;
    std::atomic<int> finished(0);
    std::vector<cu::coroutine> group;
    for (int i = 0; i < roots; ++i) {
        group.emplace_back([&finished] {
            for (int round = 0; round < rounds; ++round) {
                cu::coroutine child([round] {
                    for (int k = 0; k < round % 4; ++k) {
                        cu::this_coroutine::yield();
                    }
                });
                child.join();
            }
            finished.fetch_add(1);
        });
    }
    for (std::size_t i = 0; i < group.size(); ++i) {
        group[i].join();
    }
    EXPECT(finished.load(), ==, roots);
}

TEST(scheduler, plain_threads_and_coroutines_wake_each_other) {
    static const int threads = 4;
    static const int each = 500;
    std::atomic<int> finished(0);
    std::vector<std::thread> starters;
    for (int t = 0; t < threads; ++t) {
        starters.emplace_back([&finished] {
            for (int i = 0; i < each; ++i) {
                cu::coroutine worker([&finished] { finished.fetch_add(1); });
                worker.join();   // 普通线程等协程
            }
        });
    }
    for (std::size_t t = 0; t < starters.size(); ++t) {
        starters[t].join();
    }
    EXPECT(finished.load(), ==, threads * each);
}

TEST(scheduler, a_thousand_sleepers_all_wake_on_time) {
    static const int sleepers = 1000;
    std::atomic<int> woken(0);
    std::atomic<int> early(0);
    const clock_type::time_point start = clock_type::now();
    std::vector<cu::coroutine> group;
    for (int i = 0; i < sleepers; ++i) {
        group.emplace_back([&woken, &early, start] {
            cu::this_coroutine::sleep_until(start + std::chrono::milliseconds(50));
            if (clock_type::now() < start + std::chrono::milliseconds(50)) {
                early.fetch_add(1);
            }
            woken.fetch_add(1);
        });
    }
    for (std::size_t i = 0; i < group.size(); ++i) {
        group[i].join();
    }
    EXPECT(woken.load(), ==, sleepers);
    EXPECT(early.load(), ==, 0);
    EXPECT(clock_type::now() - start < std::chrono::seconds(5), ==, true);
}
