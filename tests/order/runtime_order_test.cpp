// 只有一根工作线程时的调度次序：全局队列不饿死、让出与睡眠的轮转、定时器按时限唤醒。
//
// 要断言次序的测试都由一根协程把其余的起出来：协程里 go() 起的协程排在本线程上，
// 要等这根协程挂起才开始跑，所以它们在同一轮里起步，次序是确定的。

#include <culib/coroutine.h>
#include <culib/stopwatch.h>
#include <culib/test.h>

#include <algorithm>
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
    // 一直让出的协程不会让全局队列里的协程等到它结束：工作线程每 61 次从全局队列里取一根。
    // 两根都从 main 起，所以都排在全局队列里。main 排第二根之前，第一根可能已经让出了很多次，
    // 所以等第二根排进去以后才开始数。
    std::atomic<bool> queued(false);
    std::atomic<bool> started(false);
    int rounds = 0;
    wavvix::wait_group group;
    group.go([&] {
        while (!queued.load()) {
            wavvix::yield();
        }
        while (!started.load()) {
            ++rounds;
            wavvix::yield();
        }
    });
    group.go([&] { started.store(true); });
    queued.store(true);
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

TEST(runtime_order, a_short_sleep_lasts_about_as_long_as_asked) {
    // Windows 上等待的超时按系统定时器的节拍到期，默认约 15.6 毫秒；事件引擎另设了高精度定时器，
    // 所以睡 1 毫秒在三个平台上都是 1 毫秒出头。看中位数，个别几次被系统拖慢不要紧。
    std::vector<long long> slept;   // 微秒
    in_coroutine([&] {
        for (int i = 0; i < 21; ++i) {
            const wavvix::stopwatch watch;
            wavvix::sleep_for(ms(1));
            slept.push_back(std::chrono::duration_cast<std::chrono::microseconds>(watch.elapsed()).count());
        }
    });

    std::sort(slept.begin(), slept.end());
    EXPECT(slept[10], <, 5000);
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

// ---- 卡住的工作线程 --------------------------------------------------------------------
//
// 这个程序只有一个名额：一根协程把它所在的线程卡住（阻塞线程，不是挂起协程），监控线程把名额
// 交给另一根线程，排着的协程照样跑。

namespace {

void block_thread_until(const std::atomic<bool>& done) {
    const wavvix::stopwatch watch;
    while (!done.load() && watch.elapsed() < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(ms(1));
    }
}

}  // namespace

TEST(runtime_order, a_worker_stuck_in_a_blocking_call_hands_its_place_over) {
    std::atomic<bool> ran(false);
    bool ran_while_stuck = false;
    in_coroutine([&] {
        wavvix::wait_group children;
        children.go([&] { ran.store(true); });   // 排在本线程上，这根协程不让出它就没人跑
        block_thread_until(ran);
        ran_while_stuck = ran.load();
        children.wait();
    });

    EXPECT(ran_while_stuck, ==, true);
}

TEST(runtime_order, a_timer_fires_while_the_only_worker_is_stuck) {
    std::atomic<bool> woke(false);
    bool woke_while_stuck = false;
    in_coroutine([&] {
        wavvix::wait_group children;
        children.go([&] {
            wavvix::sleep_for(ms(5));
            woke.store(true);
        });
        wavvix::yield();   // 让它先睡下
        block_thread_until(woke);
        woke_while_stuck = woke.load();
        children.wait();
    });

    EXPECT(woke_while_stuck, ==, true);
}

TEST(runtime_order, a_worker_busy_for_a_few_milliseconds_keeps_its_place) {
    // 停住不到 10 毫秒不算卡住：排在后面的协程等它跑完，不交接名额。机器忙的时候这根线程可能被系统
    // 挂起得更久、交接一次，所以试几轮，有一轮没交接就行。阈值要是太低，每一轮都会交接。
    bool kept = false;
    for (int attempt = 0; attempt < 10 && !kept; ++attempt) {
        std::atomic<bool> ran(false);
        bool ran_while_busy = true;
        in_coroutine([&] {
            wavvix::wait_group children;
            children.go([&] { ran.store(true); });   // 排在本线程上，交接了才会在别的线程上先跑
            const wavvix::stopwatch busy;
            while (busy.elapsed() < ms(4)) {
                // 故意不让出
            }
            ran_while_busy = ran.load();
            children.wait();
        });
        kept = !ran_while_busy;
    }

    EXPECT(kept, ==, true);
}

TEST(runtime_order, goes_back_to_one_coroutine_at_a_time_after_a_hand_off) {
    // 交接以后多了一根线程，但卡住的那根跑完手上的协程就去当备用，同一时刻跑协程的又只剩一根。
    // 机器忙的时候，跑协程的线程被系统挂起 10 毫秒以上也会被当成卡住、交接一次，那一轮里会看到
    // 两根同时在跑；所以试几轮，有一轮只看到一根就行。卡住的那根要是一直不交还名额，每一轮都是两根。
    std::atomic<bool> ran(false);
    in_coroutine([&] {
        wavvix::wait_group children;
        children.go([&] { ran.store(true); });
        block_thread_until(ran);
        children.wait();
    });

    int peak = 0;
    for (int attempt = 0; attempt < 5 && peak != 1; ++attempt) {
        std::atomic<int> inside(0);
        std::atomic<int> seen_peak(0);
        wavvix::wait_group group;
        for (int i = 0; i < 8; ++i) {
            group.go([&] {
                for (int round = 0; round < 50; ++round) {
                    const int now = inside.fetch_add(1) + 1;
                    int seen = seen_peak.load();
                    while (now > seen && !seen_peak.compare_exchange_weak(seen, now)) {
                    }
                    const wavvix::stopwatch busy;
                    while (busy.elapsed() < std::chrono::microseconds(2)) {
                    }
                    inside.fetch_sub(1);
                    wavvix::yield();
                }
            });
        }
        group.wait();
        peak = seen_peak.load();
    }

    EXPECT(ran.load(), ==, true);
    EXPECT(peak, ==, 1);
}
