// 协程运行时：工作线程、全局等待队列、派到指定线程、join / detach、让出与睡眠。
//
// 要断言次序的测试都只开一根工作线程，而且由一根协程把其余的派到 scheduler::current()：
// 派来的协程要等这根协程挂起才开始跑，所以它们在同一轮里起步，次序是确定的。

#include <culib/coroutine.h>
#include <culib/runtime.h>
#include <culib/stopwatch.h>
#include <culib/test.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

typedef std::chrono::milliseconds ms;

struct move_only_task {
    explicit move_only_task(int& result) : result_(&result), value_(new int(7)) {}
    move_only_task(move_only_task&&) = default;
    move_only_task(const move_only_task&) = delete;

    void operator()() { *result_ = *value_; }

    int* result_;
    std::unique_ptr<int> value_;
};

struct observable_task {
    explicit observable_task(std::atomic<bool>& destroyed) : destroyed_(&destroyed), marker_(new int(1)) {}
    observable_task(observable_task&&) = default;
    observable_task(const observable_task&) = delete;
    ~observable_task() {
        if (marker_) {
            destroyed_->store(true);
        }
    }

    void operator()() {}

    std::atomic<bool>* destroyed_;
    std::unique_ptr<int> marker_;
};

// 一根工作线程的 runtime 里跑 body，等它结束。
template <typename F>
void on_one_worker(F body) {
    wavvix::runtime runtime(1);
    wavvix::coroutine first(body);
    first.join();
}

}  // namespace

// ---- runtime ---------------------------------------------------------------------

TEST(runtime, runs_coroutines_on_worker_threads) {
    wavvix::runtime runtime(2);
    std::thread::id ran_on;
    wavvix::coroutine worker([&] { ran_on = std::this_thread::get_id(); });
    worker.join();

    EXPECT(ran_on != std::thread::id(), ==, true);
    EXPECT(ran_on != std::this_thread::get_id(), ==, true);
}

TEST(runtime, starts_again_after_the_previous_one_ended) {
    for (int round = 0; round < 3; ++round) {
        wavvix::runtime runtime(2);
        int ran = 0;
        wavvix::coroutine worker([&] { ++ran; });
        worker.join();
        EXPECT(ran, ==, 1);
    }
}

TEST(runtime, waits_for_detached_coroutines_before_it_ends) {
    std::atomic<int> finished(0);
    {
        wavvix::runtime runtime(2);
        for (int i = 0; i < 256; ++i) {
            wavvix::coroutine task([&] {
                wavvix::yield();
                finished.fetch_add(1);
            });
            task.detach();
        }
    }
    EXPECT(finished.load(), ==, 256);
}

TEST(runtime, waits_for_coroutines_started_by_detached_ones) {
    std::atomic<bool> parent_finished(false);
    std::atomic<bool> child_finished(false);
    {
        wavvix::runtime runtime(1);
        wavvix::coroutine parent([&] {
            wavvix::sleep_for(ms(10));
            wavvix::coroutine([&] { child_finished.store(true); }).detach();
            parent_finished.store(true);
        });
        parent.detach();
    }
    EXPECT(parent_finished.load(), ==, true);
    EXPECT(child_finished.load(), ==, true);
}

TEST(runtime, an_idle_worker_takes_the_next_queued_coroutine) {
    // 两根协程都占着工作线程等对方进来：只有分到两根线程上，双方才都等得到。
    wavvix::runtime runtime(2);
    std::mutex gate;
    std::condition_variable changed;
    int entered = 0;
    bool met[2] = {false, false};
    std::thread::id threads[2];
    const auto meet = [&](int index) {
        threads[index] = std::this_thread::get_id();
        std::unique_lock<std::mutex> lock(gate);
        ++entered;
        changed.notify_all();
        met[index] = changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered == 2; });
    };

    wavvix::coroutine first(meet, 0);
    wavvix::coroutine second(meet, 1);
    first.join();
    second.join();

    EXPECT(met[0] && met[1], ==, true);
    EXPECT(threads[0] != threads[1], ==, true);
}

TEST(runtime, sends_a_coroutine_to_the_chosen_worker) {
    wavvix::runtime runtime(2);
    std::thread::id first_thread;
    std::thread::id second_thread;
    std::thread::id first_again;

    wavvix::coroutine first(runtime.worker(0), [&] { first_thread = std::this_thread::get_id(); });
    wavvix::coroutine second(runtime.worker(1), [&] { second_thread = std::this_thread::get_id(); });
    first.join();
    second.join();
    wavvix::coroutine again(runtime.worker(0), [&] { first_again = std::this_thread::get_id(); });
    again.join();

    EXPECT(first_thread != second_thread, ==, true);
    EXPECT(first_again == first_thread, ==, true);
}

TEST(runtime, current_scheduler_keeps_a_child_on_the_same_worker) {
    wavvix::runtime runtime(4);
    std::thread::id parent_thread;
    std::thread::id child_thread;

    wavvix::coroutine parent([&] {
        parent_thread = std::this_thread::get_id();
        wavvix::coroutine child(wavvix::scheduler::current(), [&] { child_thread = std::this_thread::get_id(); });
        child.join();
    });
    parent.join();

    EXPECT(child_thread == parent_thread, ==, true);
}

TEST(runtime, accepts_coroutines_from_several_threads_at_once) {
    wavvix::runtime runtime(2);
    std::atomic<int> finished(0);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.push_back(std::thread([&] {
            for (int i = 0; i < 32; ++i) {
                wavvix::coroutine task([&] { finished.fetch_add(1); });
                task.join();
            }
        }));
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    EXPECT(finished.load(), ==, 256);
}

TEST(runtime, starts_a_queued_coroutine_while_another_keeps_yielding) {
    // 一直让出的协程不会让全局队列里的协程等到它结束：工作线程每 61 轮从队列里取一根。
    bool started = false;
    int rounds = 0;
    on_one_worker([&] {
        wavvix::coroutine spinner([&] {
            while (!started) {
                ++rounds;
                wavvix::yield();
            }
        });
        wavvix::coroutine queued([&] { started = true; });
        spinner.join();
        queued.join();
    });

    EXPECT(started, ==, true);
    EXPECT(rounds < 100, ==, true);
}

TEST(runtime, interleaves_coroutines_that_yield_on_one_worker) {
    std::string trace;
    on_one_worker([&] {
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine first(here, [&] {
            for (int i = 0; i < 3; ++i) {
                trace += "甲";
                wavvix::yield();
            }
        });
        wavvix::coroutine second(here, [&] {
            for (int i = 0; i < 3; ++i) {
                trace += "乙";
                wavvix::yield();
            }
        });
        first.join();
        second.join();
    });

    EXPECT(trace, ==, "甲乙甲乙甲乙");
}

TEST(runtime, keeps_each_coroutines_own_stack) {
    std::vector<int> results(3, 0);
    on_one_worker([&] {
        std::vector<wavvix::coroutine> workers;
        for (int i = 0; i < 3; ++i) {
            workers.push_back(wavvix::coroutine(wavvix::scheduler::current(), [&results, i] {
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
            }));
        }
        for (std::size_t i = 0; i < workers.size(); ++i) {
            workers[i].join();
        }
    });

    EXPECT(results[0], ==, 0 + 1 + 2 + 3 + 4);
    EXPECT(results[1], ==, 1 + 2 + 3 + 4 + 5);
    EXPECT(results[2], ==, 2 + 3 + 4 + 5 + 6);
}

// ---- 睡眠与让出 -------------------------------------------------------------------

TEST(runtime, sleeping_coroutine_leaves_the_worker_to_others) {
    // 睡的是协程不是线程：睡着的那根不占着工作线程，另一根在这段时间里能跑很多轮。
    int spins = 0;
    bool slept = false;
    std::chrono::nanoseconds elapsed(0);
    on_one_worker([&] {
        const wavvix::scheduler here = wavvix::scheduler::current();
        const wavvix::stopwatch watch;
        wavvix::coroutine sleeper(here, [&] {
            wavvix::sleep_for(ms(30));
            slept = true;
        });
        wavvix::coroutine spinner(here, [&] {
            while (!slept) {
                ++spins;
                wavvix::yield();
            }
        });
        sleeper.join();
        spinner.join();
        elapsed = watch.elapsed();
    });

    EXPECT(slept, ==, true);
    EXPECT(spins > 10, ==, true);
    // 至少睡够了；不设上限，CI 机器可能很慢
    EXPECT(elapsed >= ms(25), ==, true);
}

TEST(runtime, wakes_sleepers_in_time_order) {
    std::string order;
    on_one_worker([&] {
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine c(here, [&] {
            wavvix::sleep_for(ms(30));
            order += "C";
        });
        wavvix::coroutine a(here, [&] {
            wavvix::sleep_for(ms(10));
            order += "A";
        });
        wavvix::coroutine b(here, [&] {
            wavvix::sleep_for(ms(20));
            order += "B";
        });
        c.join();
        a.join();
        b.join();
    });

    EXPECT(order, ==, "ABC");
}

TEST(runtime, wakes_sleepers_in_time_order_when_they_expire_in_one_round) {
    // 定时器粒度粗的平台上，一次睡眠会 overshoot 到好几个一起到期；它们要按时限排回就绪队列，
    // 不能按挂起的次序。这里让另一根协程不让出地占住工作线程，直到三个时限都过去，
    // 所以在定时器精细的 Linux 上也走得到这条路。
    std::string order;
    on_one_worker([&] {
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine c(here, [&] {
            wavvix::sleep_for(ms(30));
            order += "C";
        });
        wavvix::coroutine a(here, [&] {
            wavvix::sleep_for(ms(10));
            order += "A";
        });
        wavvix::coroutine b(here, [&] {
            wavvix::sleep_for(ms(20));
            order += "B";
        });
        wavvix::coroutine hog(here, [] {
            const wavvix::stopwatch watch;
            while (watch.elapsed() < ms(45)) {
                // 故意不让出：这一根跑完之前工作线程不会去看定时器
            }
        });
        c.join();
        a.join();
        b.join();
        hog.join();
    });

    EXPECT(order, ==, "ABC");
}

TEST(runtime, nonpositive_sleep_returns_without_suspending) {
    std::string trace;
    on_one_worker([&] {
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine sleeper(here, [&] {
            trace += "a";
            wavvix::sleep_for(ms(-1));
            trace += "a";
            wavvix::sleep_for(ms(0));
            trace += "a";
        });
        wavvix::coroutine other(here, [&] { trace += "b"; });
        sleeper.join();
        other.join();
    });

    EXPECT(trace, ==, "aaab");   // 挂起了的话 b 会插到中间
}

TEST(runtime, sleep_and_yield_outside_a_coroutine_act_on_the_thread) {
    const wavvix::stopwatch watch;
    wavvix::yield();
    wavvix::sleep_for(ms(-1));
    wavvix::sleep_for(ms(15));
    EXPECT(watch.elapsed() >= ms(12), ==, true);
}

// ---- coroutine 句柄 ----------------------------------------------------------------

TEST(coroutine, join_waits_until_the_callable_is_destroyed) {
    std::atomic<bool> destroyed(false);
    wavvix::runtime runtime(1);
    wavvix::coroutine worker{observable_task(destroyed)};
    worker.join();

    EXPECT(destroyed.load(), ==, true);
    EXPECT(worker.joinable(), ==, false);
}

TEST(coroutine, accepts_a_move_only_callable) {
    wavvix::runtime runtime(1);
    int value = 0;
    wavvix::coroutine worker{move_only_task(value)};
    worker.join();

    EXPECT(value, ==, 7);
}

TEST(coroutine, moves_arguments_and_takes_references_through_std_ref) {
    wavvix::runtime runtime(1);
    int value = 0;
    wavvix::coroutine worker([](std::unique_ptr<int> input, int& output) { output = *input; },
                             std::unique_ptr<int>(new int(11)), std::ref(value));
    worker.join();

    EXPECT(value, ==, 11);
}

TEST(coroutine, accepts_a_member_function) {
    struct counter {
        int value;
        void add(int amount) { value += amount; }
    } target = {2};

    wavvix::runtime runtime(1);
    wavvix::coroutine worker(&counter::add, &target, 5);
    worker.join();

    EXPECT(target.value, ==, 7);
}

TEST(coroutine, with_stack_keeps_the_size_apart_from_the_arguments) {
    wavvix::runtime runtime(1);
    std::size_t value = 0;
    wavvix::coroutine worker = wavvix::coroutine::with_stack(
        32 * 1024, [&](std::size_t input) { value = input; }, std::size_t(42));
    worker.join();

    EXPECT(value, ==, std::size_t(42));
}

TEST(coroutine, move_transfers_the_join_obligation) {
    wavvix::runtime runtime(1);
    int ran = 0;
    wavvix::coroutine original([&] { ++ran; });
    wavvix::coroutine moved(std::move(original));
    EXPECT(original.joinable(), ==, false);
    EXPECT(moved.joinable(), ==, true);

    moved.join();
    EXPECT(ran, ==, 1);
}

TEST(coroutine, detach_gives_up_the_handle) {
    std::atomic<int> ran(0);
    {
        wavvix::runtime runtime(1);
        wavvix::coroutine worker([&] { ran.fetch_add(1); });
        worker.detach();
        EXPECT(worker.joinable(), ==, false);
    }
    EXPECT(ran.load(), ==, 1);
}

TEST(coroutine, join_inside_a_coroutine_suspends_only_that_coroutine) {
    bool finished = false;
    bool seen_after_join = false;
    int spins = 0;
    on_one_worker([&] {
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine target(here, [&] {
            wavvix::sleep_for(ms(20));
            finished = true;
        });
        wavvix::coroutine spinner(here, [&] {
            while (!finished) {
                ++spins;
                wavvix::yield();
            }
        });
        target.join();
        seen_after_join = finished;
        spinner.join();
    });

    EXPECT(seen_after_join, ==, true);
    EXPECT(spins > 10, ==, true);   // join 期间工作线程照常跑别的协程
}

TEST(coroutine, joins_a_coroutine_on_another_worker) {
    wavvix::runtime runtime(2);
    bool finished = false;
    bool seen_after_join = false;
    std::thread::id joiner_thread;
    std::thread::id target_thread;

    wavvix::coroutine joiner(runtime.worker(0), [&] {
        joiner_thread = std::this_thread::get_id();
        wavvix::coroutine target(runtime.worker(1), [&] {
            target_thread = std::this_thread::get_id();
            wavvix::sleep_for(ms(10));
            finished = true;
        });
        target.join();
        seen_after_join = finished;
    });
    joiner.join();

    EXPECT(seen_after_join, ==, true);
    EXPECT(joiner_thread != target_thread, ==, true);
}

TEST(coroutine, joins_many_children_that_may_run_on_other_workers) {
    wavvix::runtime runtime(2);
    std::atomic<int> completed(0);
    wavvix::coroutine parent([&] {
        for (int i = 0; i < 64; ++i) {
            wavvix::coroutine child([&] { completed.fetch_add(1); });
            child.join();
        }
    });
    parent.join();

    EXPECT(completed.load(), ==, 64);
}
