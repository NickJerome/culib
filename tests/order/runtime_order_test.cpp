// 只有一根工作线程时的调度次序：全局队列不饿死、让出与睡眠的轮转、定时器按时限唤醒。
//
// 要断言次序的测试都由一根协程把其余的起出来：协程里 go() 起的协程排在本线程上，
// 要等这根协程挂起才开始跑，所以它们在同一轮里起步，次序是确定的。

#include <culib/coroutine.h>
#include <culib/stopwatch.h>
#include <culib/test.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

typedef std::chrono::milliseconds ms;

// 在协程里跑 body，等它结束。
template <typename F>
void in_coroutine(F body) {
    wavvix::wait_group group;
    group.go(body);
    group.wait();
}

}  // namespace

TEST(runtime_order, runs_every_coroutine_on_the_one_worker) {
    std::mutex guard;
    std::set<std::thread::id> threads;
    wavvix::wait_group group;
    for (int i = 0; i < 16; ++i) {
        group.go([&] {
            wavvix::yield();
            std::lock_guard<std::mutex> held(guard);
            threads.insert(std::this_thread::get_id());
        });
    }
    group.wait();

    EXPECT(threads.size(), ==, std::size_t(1));
}

TEST(runtime_order, starts_a_queued_coroutine_while_another_keeps_yielding) {
    // 一直让出的协程不会让全局队列里的协程等到它结束：工作线程每 61 轮从全局队列里取一根。
    // 两根都从 main 起，所以都排在全局队列里。
    std::atomic<bool> started(false);
    int rounds = 0;
    wavvix::wait_group group;
    group.go([&] {
        while (!started.load()) {
            ++rounds;
            wavvix::yield();
        }
    });
    group.go([&] { started.store(true); });
    group.wait();

    EXPECT(started.load(), ==, true);
    EXPECT(rounds < 100, ==, true);
}

TEST(runtime_order, interleaves_coroutines_that_yield) {
    std::string trace;
    in_coroutine([&] {
        wavvix::wait_group children;
        children.go([&] {
            for (int i = 0; i < 3; ++i) {
                trace += "甲";
                wavvix::yield();
            }
        });
        children.go([&] {
            for (int i = 0; i < 3; ++i) {
                trace += "乙";
                wavvix::yield();
            }
        });
        children.wait();
    });

    EXPECT(trace, ==, "甲乙甲乙甲乙");
}

TEST(runtime_order, keeps_each_coroutines_own_stack) {
    std::vector<int> results(3, 0);
    in_coroutine([&] {
        wavvix::wait_group children;
        for (int i = 0; i < 3; ++i) {
            children.go([&results, i] {
                int accumulated = 0;
                char filler[2048];
                for (int round = 0; round < 5; ++round) {
                    filler[round] = static_cast<char>(i + round);
                    accumulated += i + round;
                    wavvix::yield();
                }
                // 让出五次之后，自己栈上的东西还在
                EXPECT(filler[4], ==, static_cast<char>(i + 4));
                results[static_cast<std::size_t>(i)] = accumulated;
            });
        }
        children.wait();
    });

    EXPECT(results[0], ==, 0 + 1 + 2 + 3 + 4);
    EXPECT(results[1], ==, 1 + 2 + 3 + 4 + 5);
    EXPECT(results[2], ==, 2 + 3 + 4 + 5 + 6);
}

TEST(runtime_order, sleeping_coroutine_leaves_the_worker_to_others) {
    // 睡的是协程不是线程：睡着的那根不占着工作线程，另一根在这段时间里能跑很多轮。
    int spins = 0;
    bool slept = false;
    std::chrono::nanoseconds elapsed(0);
    in_coroutine([&] {
        const wavvix::stopwatch watch;
        wavvix::wait_group children;
        children.go([&] {
            wavvix::sleep_for(ms(30));
            slept = true;
        });
        children.go([&] {
            while (!slept) {
                ++spins;
                wavvix::yield();
            }
        });
        children.wait();
        elapsed = watch.elapsed();
    });

    EXPECT(slept, ==, true);
    EXPECT(spins > 10, ==, true);
    // 至少睡够了；不设上限，CI 机器可能很慢
    EXPECT(elapsed >= ms(25), ==, true);
}

TEST(runtime_order, wakes_sleepers_in_time_order) {
    std::string order;
    in_coroutine([&] {
        wavvix::wait_group children;
        children.go([&] {
            wavvix::sleep_for(ms(30));
            order += "C";
        });
        children.go([&] {
            wavvix::sleep_for(ms(10));
            order += "A";
        });
        children.go([&] {
            wavvix::sleep_for(ms(20));
            order += "B";
        });
        children.wait();
    });

    EXPECT(order, ==, "ABC");
}

TEST(runtime_order, wakes_sleepers_in_time_order_when_they_expire_in_one_round) {
    // 定时器粒度粗的平台上，一次睡眠会 overshoot 到好几个一起到期；它们要按时限排回就绪队列，
    // 不能按挂起的次序。这里让另一根协程不让出地占住工作线程，直到三个时限都过去，
    // 所以在定时器精细的 Linux 上也走得到这条路。
    std::string order;
    in_coroutine([&] {
        wavvix::wait_group children;
        children.go([&] {
            wavvix::sleep_for(ms(30));
            order += "C";
        });
        children.go([&] {
            wavvix::sleep_for(ms(10));
            order += "A";
        });
        children.go([&] {
            wavvix::sleep_for(ms(20));
            order += "B";
        });
        children.go([] {
            const wavvix::stopwatch watch;
            while (watch.elapsed() < ms(45)) {
                // 故意不让出：这一根跑完之前工作线程不会去看定时器
            }
        });
        children.wait();
    });

    EXPECT(order, ==, "ABC");
}

TEST(runtime_order, nonpositive_sleep_returns_without_suspending) {
    std::string trace;
    in_coroutine([&] {
        wavvix::wait_group children;
        children.go([&] {
            trace += "a";
            wavvix::sleep_for(ms(-1));
            trace += "a";
            wavvix::sleep_for(ms(0));
            trace += "a";
        });
        children.go([&] { trace += "b"; });
        children.wait();
    });

    EXPECT(trace, ==, "aaab");   // 挂起了的话 b 会插到中间
}

TEST(runtime_order, wait_inside_a_coroutine_suspends_only_that_coroutine) {
    bool finished = false;
    bool seen_after_wait = false;
    int spins = 0;
    in_coroutine([&] {
        wavvix::wait_group target;
        target.go([&] {
            wavvix::sleep_for(ms(20));
            finished = true;
        });
        wavvix::wait_group spinner;
        spinner.go([&] {
            while (!finished) {
                ++spins;
                wavvix::yield();
            }
        });
        target.wait();
        seen_after_wait = finished;
        spinner.wait();
    });

    EXPECT(seen_after_wait, ==, true);
    EXPECT(spins > 10, ==, true);   // wait 期间工作线程照常跑别的协程
}
