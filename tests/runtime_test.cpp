// 协程运行时：go() 起协程、wait_group 等协程、工作线程、让出与睡眠。
// 断言执行次序的测试在只开一根工作线程的 runtime_order_test 里（tests/order/）。

#include <culib/coroutine.h>
#include <culib/stopwatch.h>
#include <culib/sync.h>
#include <culib/test.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
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


// 现在在哪根线程上。协程在挂起点会换线程，而 glibc 把 pthread_self 声明成 const：
// std::this_thread::get_id() 内联以后，编译器在一个函数里只取一次，挂起前后拿到同一个值。
// 经 volatile 函数指针调，每次都真的去取。
std::thread::id read_thread_id() {
    return std::this_thread::get_id();
}

std::thread::id current_thread() {
    std::thread::id (*volatile read)() = &read_thread_id;
    return read();
}

}  // namespace

// ---- go 与 wait_group --------------------------------------------------------------

TEST(runtime, runs_coroutines_on_worker_threads) {
    std::thread::id ran_on;
    wavvix::wait_group group;
    group.go([&] { ran_on = std::this_thread::get_id(); });
    group.wait();

    EXPECT(ran_on != std::thread::id(), ==, true);
    EXPECT(ran_on != std::this_thread::get_id(), ==, true);
}

TEST(runtime, uses_the_worker_count_it_was_given) {
    // culib_test 在 main 里把 WAVVIX_MAXPROCS 设成 4：同时跑协程的最多 4 根线程。每根协程只占线程
    // 200 微秒，不到监控线程把它当成卡住的时间，不会多出线程来。
    std::mutex guard;
    std::set<std::thread::id> threads;
    wavvix::wait_group group;
    for (int i = 0; i < 32; ++i) {
        group.go([&] {
            const wavvix::stopwatch busy;   // 占着工作线程，别的协程只能去别的线程
            while (busy.elapsed() < std::chrono::microseconds(200)) {
            }
            std::lock_guard<std::mutex> held(guard);
            threads.insert(std::this_thread::get_id());
        });
    }
    group.wait();

    EXPECT(threads.size() >= std::size_t(2), ==, true);
    EXPECT(threads.size() <= std::size_t(4), ==, true);
}

TEST(runtime, wait_returns_after_every_coroutine_in_the_group_ended) {
    std::atomic<int> finished(0);
    wavvix::wait_group group;
    for (int i = 0; i < 256; ++i) {
        group.go([&] {
            wavvix::yield();
            finished.fetch_add(1);
        });
    }
    group.wait();

    EXPECT(finished.load(), ==, 256);
}

TEST(runtime, wait_covers_coroutines_added_from_inside_the_group) {
    std::atomic<bool> parent_finished(false);
    std::atomic<bool> child_finished(false);
    wavvix::wait_group group;
    group.go([&] {
        wavvix::sleep_for(ms(10));
        group.go([&] {
            wavvix::sleep_for(ms(10));
            child_finished.store(true);
        });
        parent_finished.store(true);
    });
    group.wait();

    EXPECT(parent_finished.load(), ==, true);
    EXPECT(child_finished.load(), ==, true);
}

TEST(runtime, wait_with_nothing_running_returns_at_once) {
    wavvix::wait_group group;
    const wavvix::stopwatch watch;
    group.wait();
    EXPECT(watch.elapsed() < ms(100), ==, true);
}

TEST(runtime, a_group_can_be_used_again_after_wait) {
    wavvix::wait_group group;
    int rounds = 0;
    for (int round = 0; round < 3; ++round) {
        int ran = 0;
        group.go([&] { ++ran; });
        group.wait();
        rounds += ran;
    }

    EXPECT(rounds, ==, 3);
}

TEST(runtime, an_idle_worker_takes_the_next_queued_coroutine) {
    // 两根协程都占着工作线程等对方进来：只有分到两根线程上，双方才都等得到。
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

    wavvix::wait_group group;
    group.go(meet, 0);
    group.go(meet, 1);
    group.wait();

    EXPECT(met[0] && met[1], ==, true);
    EXPECT(threads[0] != threads[1], ==, true);
}

TEST(runtime, accepts_coroutines_from_several_threads_at_once) {
    std::atomic<int> finished(0);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.push_back(std::thread([&] {
            for (int i = 0; i < 32; ++i) {
                wavvix::wait_group group;
                group.go([&] { finished.fetch_add(1); });
                group.wait();
            }
        }));
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    EXPECT(finished.load(), ==, 256);
}

TEST(runtime, go_without_a_group_still_runs) {
    std::mutex guard;
    std::condition_variable changed;
    bool ran = false;
    wavvix::go([&] {
        std::lock_guard<std::mutex> held(guard);
        ran = true;
        changed.notify_all();
    });

    std::unique_lock<std::mutex> lock(guard);
    const bool seen = changed.wait_for(lock, std::chrono::seconds(5), [&] { return ran; });
    EXPECT(seen, ==, true);
}

TEST(runtime, wait_inside_a_coroutine_waits_for_children_on_other_workers) {
    std::atomic<int> completed(0);
    bool all_seen = false;
    wavvix::wait_group outer;
    outer.go([&] {
        wavvix::wait_group children;
        for (int i = 0; i < 64; ++i) {
            children.go([&] {
                wavvix::sleep_for(ms(1));
                completed.fetch_add(1);
            });
        }
        children.wait();
        all_seen = completed.load() == 64;
    });
    outer.wait();

    EXPECT(all_seen, ==, true);
}

// ---- 换线程 ------------------------------------------------------------------------

TEST(runtime, idle_workers_take_coroutines_queued_on_a_busy_one) {
    // 协程里起的协程先排在本线程上；这根线程忙着的时候，闲着的工作线程把它们拿走。
    std::mutex guard;
    std::set<std::thread::id> threads;
    wavvix::wait_group group;
    group.go([&] {
        wavvix::wait_group children;
        for (int i = 0; i < 8; ++i) {
            children.go([&] {
                const wavvix::stopwatch busy;
                while (busy.elapsed() < ms(5)) {
                }
                std::lock_guard<std::mutex> held(guard);
                threads.insert(std::this_thread::get_id());
            });
        }
        children.wait();
    });
    group.wait();

    EXPECT(threads.size() >= std::size_t(2), ==, true);
}

TEST(runtime, a_parked_coroutine_continues_on_another_worker) {
    // 协程挂起的时候，它原来那根工作线程被占着：叫醒以后只能由别的工作线程接着跑，栈上的东西都还在。
    // 占位协程从这根协程里起，排在本线程上，这根协程一挂起它就接着在这里跑；偶尔会被闲着的线程
    // 先拿走，那一次不算，重来。
    bool occupied = false;
    bool moved = false;
    int value = 0;
    for (int attempt = 0; attempt < 50 && !occupied; ++attempt) {
        wavvix::channel<int> wake(1);
        std::atomic<int> where(0);   // 1：占位协程在原来那根线程上；2：在别的线程上
        std::atomic<bool> release(false);
        std::thread::id before;
        std::thread::id after;
        wavvix::wait_group occupiers;
        wavvix::wait_group group;
        group.go([&] {
            const int local = 7;
            before = current_thread();
            const std::thread::id home = before;
            occupiers.go([&where, &release, home] {
                where.store(current_thread() == home ? 1 : 2);
                while (!release.load()) {
                }
            });
            int got = 0;
            wake.receive(got);
            after = current_thread();
            value = local + got;
        });
        while (where.load() == 0) {
            std::this_thread::yield();
        }
        occupied = where.load() == 1;
        wake.send(35);
        group.wait();
        release.store(true);
        occupiers.wait();
        moved = after != before;
    }

    EXPECT(occupied, ==, true);
    EXPECT(moved, ==, true);
    EXPECT(value, ==, 42);
}

// ---- 参数与可调用对象 --------------------------------------------------------------

TEST(runtime, wait_returns_after_the_callable_is_destroyed) {
    std::atomic<bool> destroyed(false);
    wavvix::wait_group group;
    group.go(observable_task(destroyed));
    group.wait();

    EXPECT(destroyed.load(), ==, true);
}

TEST(runtime, accepts_a_move_only_callable) {
    int value = 0;
    wavvix::wait_group group;
    group.go(move_only_task(value));
    group.wait();

    EXPECT(value, ==, 7);
}

TEST(runtime, moves_arguments_and_takes_references_through_std_ref) {
    int value = 0;
    wavvix::wait_group group;
    group.go([](std::unique_ptr<int> input, int& output) { output = *input; },
             std::unique_ptr<int>(new int(11)), std::ref(value));
    group.wait();

    EXPECT(value, ==, 11);
}

TEST(runtime, accepts_a_member_function) {
    struct counter {
        int value;
        void add(int amount) { value += amount; }
    } target = {2};

    wavvix::wait_group group;
    group.go(&counter::add, &target, 5);
    group.wait();

    EXPECT(target.value, ==, 7);
}

// ---- 睡眠与让出 -------------------------------------------------------------------

TEST(runtime, sleep_and_yield_outside_a_coroutine_act_on_the_thread) {
    const wavvix::stopwatch watch;
    wavvix::yield();
    wavvix::sleep_for(ms(-1));
    wavvix::sleep_for(ms(15));
    EXPECT(watch.elapsed() >= ms(12), ==, true);
}

TEST(runtime, a_sleeping_coroutine_wakes_up_after_its_time) {
    std::chrono::nanoseconds slept(0);
    wavvix::wait_group group;
    group.go([&] {
        const wavvix::stopwatch watch;
        wavvix::sleep_for(ms(20));
        slept = watch.elapsed();
    });
    group.wait();

    EXPECT(slept >= ms(18), ==, true);
}
