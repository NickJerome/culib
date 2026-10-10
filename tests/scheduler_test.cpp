#include <culib/coroutine.h>
#include <culib/scheduler.h>
#include <culib/stopwatch.h>
#include <culib/test.h>

#include "runtime/park.h"
#include "runtime/remote_wake.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using wavvix::run;
using wavvix::scheduled;
using wavvix::sleep_for;
using wavvix::spawn;
using wavvix::yield;

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
    explicit observable_task(std::atomic<bool>& destroyed)
        : destroyed_(&destroyed), marker_(new int(1)) {}
    observable_task(observable_task&&) = default;
    observable_task(const observable_task&) = delete;
    ~observable_task() {
        if (marker_) destroyed_->store(true);
    }
    void operator()() {}
    std::atomic<bool>* destroyed_;
    std::unique_ptr<int> marker_;
};

#if defined(_MSC_VER)
struct late_scheduler_probe {
    late_scheduler_probe() : observed(0) {}
    ~late_scheduler_probe() {
        if (observed && scheduled() == 0) observed->fetch_add(1);
    }
    std::atomic<int>* observed;
};

thread_local late_scheduler_probe late_probe;
#endif

}  // namespace

TEST(scheduler, runs_nothing_when_nothing_was_spawned) {
    EXPECT(scheduled(), ==, 0u);
    EXPECT(run(), ==, 0u);
}

TEST(scheduler, reaps_vcpus_during_concurrent_thread_exit) {
    std::atomic<int> completed(0);
#if defined(_MSC_VER)
    std::atomic<int> late_observed(0);
#endif
    std::atomic<int> failed(0);
    for (int round = 0; round < 4; ++round) {
        std::vector<std::thread> workers;
        for (int i = 0; i < 16; ++i) workers.push_back(std::thread([&] {
#if defined(_MSC_VER)
            // MSVC 的线程局部析构晚于 FLS 回调。
            late_probe.observed = &late_observed;
#endif
            spawn([&] { completed.fetch_add(1); }).value();
            run();
            if (!wavvix::detail::scheduler_remote_prepare()) failed.fetch_add(1);
        }));
        for (std::size_t i = 0; i < workers.size(); ++i) workers[i].join();
    }
    EXPECT(completed.load(), ==, 64);
#if defined(_MSC_VER)
    EXPECT(late_observed.load(), ==, 64);
#endif
    EXPECT(failed.load(), ==, 0);
}

TEST(scheduler, scope_runs_submitted_coroutines_on_exit) {
    int finished = 0;
    {
        wavvix::scheduler_scope scope;
        spawn([&] { ++finished; }).value();
        EXPECT(finished, ==, 0);
        EXPECT(scheduled(), ==, 1u);
    }
    EXPECT(finished, ==, 1);
    EXPECT(scheduled(), ==, 0u);
}

TEST(scheduler, nested_scopes_finish_at_the_outer_boundary) {
    int finished = 0;
    {
        wavvix::scheduler_scope outer;
        {
            wavvix::scheduler_scope inner;
            spawn([&] { ++finished; }).value();
        }
        EXPECT(finished, ==, 0);
        EXPECT(scheduled(), ==, 1u);
    }
    EXPECT(finished, ==, 1);
    EXPECT(scheduled(), ==, 0u);
}

TEST(scheduler, scheduled_counts_tasks_waiting_in_the_current_round) {
    std::size_t before_spawn = 0;
    std::size_t after_spawn = 0;
    std::size_t second_task = 0;
    int finished = 0;
    spawn([&] {
        before_spawn = scheduled();
        spawn([&] { ++finished; }).value();
        after_spawn = scheduled();
        ++finished;
    }).value();
    spawn([&] { second_task = scheduled(); ++finished; }).value();
    spawn([&] { ++finished; }).value();
    EXPECT(scheduled(), ==, 3u);
    EXPECT(run(), ==, 4u);
    EXPECT(before_spawn, ==, 2u);
    EXPECT(after_spawn, ==, 3u);
    EXPECT(second_task, ==, 2u);
    EXPECT(finished, ==, 4);
    EXPECT(scheduled(), ==, 0u);
}

TEST(scheduler, wakes_a_parked_coroutine_from_another_thread) {
    bool resumed = false;
    bool notified = false;
    spawn([&] {
        wavvix::detail::remote_waker* const waker =
            wavvix::detail::scheduler_remote_prepare();
        EXPECT(waker != 0, ==, true);
        if (!waker) return;
        wavvix::detail::park_token token;
        wavvix::detail::scheduler_external_wait_begin(waker);
        std::thread worker([&] {
            std::this_thread::sleep_for(ms(20));
            notified = wavvix::detail::scheduler_external_wake(waker, token);
        });
        resumed = wavvix::detail::scheduler_park(
            token, std::chrono::nanoseconds(-1));
        worker.join();
    }).value();
    EXPECT(run(), ==, 1u);
    EXPECT(notified, ==, true);
    EXPECT(resumed, ==, true);
}

TEST(scheduler, spawn_moves_arguments_and_accepts_explicit_references) {
    int value = 0;
    spawn([](std::unique_ptr<int> input, int& output) {
        output = *input;
    }, std::unique_ptr<int>(new int(11)), std::ref(value)).value();
    EXPECT(value, ==, 0);
    EXPECT(run(), ==, 1u);
    EXPECT(value, ==, 11);
}

TEST(scheduler, spawn_with_stack_separates_stack_size_from_function_argument) {
    std::size_t value = 0;
    wavvix::spawn_with_stack(32 * 1024,
        [](std::size_t input, std::size_t* output) { *output = input; },
        std::size_t(42), &value).value();
    EXPECT(run(), ==, 1u);
    EXPECT(value, ==, std::size_t(42));
}

TEST(scheduler, invalid_spawn_stack_returns_error_without_scheduling) {
    bool ran = false;
    const std::size_t before = scheduled();
    wavvix::result<void> too_small = wavvix::spawn_with_stack(0, [&] { ran = true; });
    EXPECT(too_small.is_err(), ==, true);
    EXPECT(too_small.error().kind(), ==, wavvix::error_kind::out_of_range);
    wavvix::result<void> too_large = wavvix::spawn_with_stack(
        (std::numeric_limits<std::size_t>::max)(), [&] { ran = true; });
    EXPECT(too_large.is_err(), ==, true);
    EXPECT(too_large.error().kind(), ==, wavvix::error_kind::out_of_range);
    EXPECT(scheduled(), ==, before);
    EXPECT(ran, ==, false);
}

TEST(scheduler, coroutine_object_joins_like_a_thread) {
    int ran = 0;
    wavvix::coroutine worker([&] { ++ran; });
    EXPECT(worker.joinable(), ==, true);
    worker.join();
    EXPECT(ran, ==, 1);
    EXPECT(worker.joinable(), ==, false);
    EXPECT(scheduled(), ==, 0u);
}

TEST(scheduler, coroutine_object_accepts_move_only_body) {
    int value = 0;
    wavvix::coroutine worker{move_only_task(value)};
    worker.join();
    EXPECT(value, ==, 7);
}

TEST(scheduler, coroutine_object_moves_arguments_into_function) {
    int value = 0;
    wavvix::coroutine worker([](std::unique_ptr<int> input, int& output) {
        output = *input;
    }, std::unique_ptr<int>(new int(11)), std::ref(value));
    worker.join();
    EXPECT(value, ==, 11);
}

TEST(scheduler, coroutine_object_accepts_member_function) {
    struct counter {
        int value;
        void add(int amount) { value += amount; }
    } target = {2};
    wavvix::coroutine worker(&counter::add, &target, 5);
    worker.join();
    EXPECT(target.value, ==, 7);
}

TEST(scheduler, worker_scope_rejects_zero_workers) {
    int ran = 0;
    wavvix::spawn([&] { ++ran; }).value();
    wavvix::result<wavvix::scheduler_scope> opened =
        wavvix::scheduler_scope::with_workers(0);
    EXPECT(opened.is_err(), ==, true);
    EXPECT(ran, ==, 0);
    EXPECT(wavvix::scheduled(), ==, 1u);
    EXPECT(wavvix::run(), ==, 1u);
    EXPECT(ran, ==, 1);
}

TEST(scheduler, nested_worker_scope_stops_its_pool_on_exit) {
    wavvix::scheduler_scope outer;
    for (int i = 0; i < 2; ++i) {
        wavvix::result<wavvix::scheduler_scope> opened =
            wavvix::scheduler_scope::with_workers(1);
        EXPECT(opened.is_ok(), ==, true);
        if (opened.is_err()) return;
        wavvix::scheduler_scope runtime = std::move(opened).value();
        wavvix::coroutine task([] {});
        task.join();
    }
}

TEST(scheduler, coroutine_objects_run_in_parallel_vcpus) {
    wavvix::result<wavvix::scheduler_scope> opened =
        wavvix::scheduler_scope::with_workers(2);
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) return;
    wavvix::scheduler_scope runtime = std::move(opened).value();
    const std::thread::id main_thread = std::this_thread::get_id();
    std::thread::id threads[2];
    std::mutex gate;
    std::condition_variable changed;
    int entered = 0;
    bool parallel[2] = {false, false};
    const auto work = [&](int index) {
        threads[index] = std::this_thread::get_id();
        std::unique_lock<std::mutex> lock(gate);
        ++entered;
        changed.notify_all();
        parallel[index] = changed.wait_for(lock, std::chrono::seconds(2),
                                           [&] { return entered == 2; });
    };
    wavvix::coroutine first(work, 0);
    wavvix::coroutine second(work, 1);
    first.join();
    second.join();
    EXPECT(parallel[0] && parallel[1], ==, true);
    EXPECT(threads[0] != main_thread && threads[1] != main_thread, ==, true);
    EXPECT(threads[0] != threads[1], ==, true);
}

TEST(scheduler, spawn_stays_local_when_coroutines_use_workers) {
    wavvix::result<wavvix::scheduler_scope> opened =
        wavvix::scheduler_scope::with_workers(1);
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) return;
    wavvix::scheduler_scope runtime = std::move(opened).value();
    const std::thread::id main_thread = std::this_thread::get_id();
    std::thread::id spawned_thread;
    spawn([&] { spawned_thread = std::this_thread::get_id(); }).value();
    EXPECT(run(), ==, 1u);
    EXPECT(spawned_thread == main_thread, ==, true);
}

TEST(scheduler, coroutine_created_on_worker_stays_on_that_worker) {
    wavvix::result<wavvix::scheduler_scope> opened =
        wavvix::scheduler_scope::with_workers(1);
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) return;
    wavvix::scheduler_scope runtime = std::move(opened).value();
    std::thread::id parent_thread;
    std::thread::id child_thread;
    wavvix::coroutine parent([&] {
        parent_thread = std::this_thread::get_id();
        wavvix::coroutine child([&] { child_thread = std::this_thread::get_id(); });
        child.join();
    });
    parent.join();
    EXPECT(parent_thread != std::this_thread::get_id(), ==, true);
    EXPECT(child_thread == parent_thread, ==, true);
}

TEST(scheduler, remote_join_waits_for_callable_destruction) {
    std::atomic<bool> destroyed(false);
    {
        wavvix::result<wavvix::scheduler_scope> opened =
            wavvix::scheduler_scope::with_workers(1);
        EXPECT(opened.is_ok(), ==, true);
        if (opened.is_err()) return;
        wavvix::scheduler_scope runtime = std::move(opened).value();
        wavvix::coroutine worker{observable_task(destroyed)};
        worker.join();
        EXPECT(destroyed.load(), ==, true);
    }
}

TEST(scheduler, local_join_waits_for_callable_destruction) {
    std::atomic<bool> destroyed(false);
    wavvix::coroutine worker{observable_task(destroyed)};
    worker.join();
    EXPECT(destroyed.load(), ==, true);
}

TEST(scheduler, coroutine_join_inside_parent_vcpu_wakes_after_remote_completion) {
    wavvix::result<wavvix::scheduler_scope> opened =
        wavvix::scheduler_scope::with_workers(2);
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) return;
    wavvix::scheduler_scope runtime = std::move(opened).value();
    int completed = 0;
    spawn([&] {
        for (int i = 0; i < 64; ++i) {
            wavvix::coroutine worker([&] { ++completed; });
            worker.join();
        }
    }).value();
    EXPECT(run(), ==, 1u);
    EXPECT(completed, ==, 64);
}

TEST(scheduler, detached_remote_tasks_and_worker_children_finish_on_scope_exit) {
    std::atomic<bool> detached_finished(false);
    std::atomic<bool> child_finished(false);
    {
        wavvix::result<wavvix::scheduler_scope> opened =
            wavvix::scheduler_scope::with_workers(1);
        EXPECT(opened.is_ok(), ==, true);
        if (opened.is_err()) return;
        wavvix::scheduler_scope runtime = std::move(opened).value();
        wavvix::coroutine detached([&] {
            wavvix::sleep_for(ms(10));
            detached_finished.store(true);
            spawn([&] { child_finished.store(true); }).value();
        });
        detached.detach();
    }
    EXPECT(detached_finished.load(), ==, true);
    EXPECT(child_finished.load(), ==, true);
}

TEST(scheduler, many_remote_tasks_finish_after_immediate_detach) {
    std::atomic<int> completed(0);
    {
        wavvix::result<wavvix::scheduler_scope> opened =
            wavvix::scheduler_scope::with_workers(2);
        EXPECT(opened.is_ok(), ==, true);
        if (opened.is_err()) return;
        wavvix::scheduler_scope runtime = std::move(opened).value();
        for (int i = 0; i < 256; ++i) {
            wavvix::coroutine task([&] { completed.fetch_add(1); });
            EXPECT(task.joinable(), ==, true);
            task.detach();
        }
    }
    EXPECT(completed.load(), ==, 256);
}

TEST(scheduler, coroutine_object_accepts_size_t_argument_and_explicit_stack) {
    std::size_t value = 0;
    wavvix::coroutine worker = wavvix::coroutine::with_stack(
        32 * 1024, [&](std::size_t input) { value = input; }, std::size_t(42));
    worker.join();
    EXPECT(value, ==, std::size_t(42));
}

#if WAVVIX_DETAIL_HAS_EXCEPTIONS
TEST(scheduler, coroutine_object_rejects_invalid_stack) {
    bool small_rejected = false;
    bool large_rejected = false;
    try {
        wavvix::coroutine::with_stack(0, [] {});
    } catch (const std::invalid_argument&) {
        small_rejected = true;
    }
    try {
        wavvix::coroutine::with_stack(
            (std::numeric_limits<std::size_t>::max)(), [] {});
    } catch (const std::invalid_argument&) {
        large_rejected = true;
    }
    EXPECT(small_rejected, ==, true);
    EXPECT(large_rejected, ==, true);
    EXPECT(scheduled(), ==, 0u);
}
#endif

TEST(scheduler, coroutine_object_transfers_join_ownership) {
    int ran = 0;
    wavvix::coroutine original([&] { ++ran; });
    wavvix::coroutine moved(std::move(original));
    EXPECT(original.joinable(), ==, false);
    EXPECT(moved.joinable(), ==, true);
    moved.join();
    EXPECT(ran, ==, 1);
}

TEST(scheduler, coroutine_object_joins_from_another_coroutine) {
    bool finished = false;
    bool joined = false;
    spawn([&] {
        wavvix::coroutine worker([&] {
            sleep_for(ms(10));
            finished = true;
        });
        worker.join();
        joined = finished;
    }).value();
    EXPECT(run(), ==, 2u);
    EXPECT(joined, ==, true);
}

TEST(scheduler, coroutine_object_detach_releases_ownership) {
    int ran = 0;
    {
        wavvix::coroutine worker([&] { ++ran; });
        worker.detach();
        EXPECT(worker.joinable(), ==, false);
    }
    EXPECT(run(), ==, 1u);
    EXPECT(ran, ==, 1);
}

TEST(scheduler, detached_coroutine_finishes_at_scope_exit) {
    int ran = 0;
    {
        wavvix::scheduler_scope runtime;
        wavvix::coroutine worker([&] { ++ran; });
        worker.detach();
        EXPECT(ran, ==, 0);
    }
    EXPECT(ran, ==, 1);
}

TEST(scheduler, main_thread_sleep_drives_initialized_vcpu) {
    wavvix::scheduler_scope runtime;
    int ran = 0;
    wavvix::coroutine worker([&] {
        sleep_for(ms(5));
        ++ran;
    });
    sleep_for(ms(20));
    EXPECT(ran, ==, 1);
    worker.join();
    EXPECT(scheduled(), ==, 0u);
}

TEST(scheduler, nonpositive_sleep_returns_without_suspending) {
    wavvix::scheduler_scope runtime;
    int steps = 0;
    spawn([&] {
        sleep_for(ms(-1));
        ++steps;
        sleep_for(ms(0));
        ++steps;
    }).value();
    sleep_for(ms(-1));
    EXPECT(steps, ==, 0);
    EXPECT(run(), ==, 1u);
    EXPECT(steps, ==, 2);
}

TEST(scheduler, main_thread_sleep_runs_tasks_due_before_overshoot) {
    wavvix::scheduler_scope runtime;
    int ran = 0;
    wavvix::coroutine worker([&] {
        sleep_for(ms(1));
        ++ran;
    });
    wavvix::coroutine blocker([] { std::this_thread::sleep_for(ms(20)); });
    sleep_for(ms(5));
    EXPECT(ran, ==, 1);
    worker.join();
    blocker.join();
}

TEST(scheduler, main_thread_yield_runs_another_coroutine) {
    wavvix::scheduler_scope runtime;
    int ran = 0;
    wavvix::coroutine worker([&] { ++ran; });
    wavvix::yield();
    EXPECT(ran, ==, 1);
    worker.join();
}

TEST(scheduler, main_yield_keeps_new_and_yielded_tasks_in_the_next_round) {
    wavvix::scheduler_scope runtime;
    std::string order;
    spawn([&] {
        order += 'A';
        EXPECT(scheduled(), ==, 1u);
        spawn([&] {
            order += 'C';
            EXPECT(scheduled(), ==, 2u);
        }).value();
        EXPECT(scheduled(), ==, 2u);
        yield();
        order += 'a';
    }).value();
    spawn([&] {
        order += 'B';
        EXPECT(scheduled(), ==, 2u);
        yield();
        order += 'b';
    }).value();

    yield();
    EXPECT(order, ==, "AB");
    EXPECT(scheduled(), ==, 3u);
    yield();
    EXPECT(order, ==, "ABCab");
    EXPECT(scheduled(), ==, 0u);
}

TEST(scheduler, joining_one_coroutine_leaves_unrelated_work_scheduled) {
    int other = 0;
    spawn([&] { sleep_for(ms(20)); ++other; }).value();
    wavvix::coroutine worker([] {});
    worker.join();
    EXPECT(other, ==, 0);
    EXPECT(scheduled(), ==, 1u);
    EXPECT(run(), ==, 1u);
    EXPECT(other, ==, 1);
}

TEST(scheduler, runs_a_single_coroutine_to_completion) {
    int ran = 0;
    spawn([&] { ++ran; }).value();
    EXPECT(scheduled(), ==, 1u);

    EXPECT(run(), ==, 1u);
    EXPECT(ran, ==, 1);
    EXPECT(scheduled(), ==, 0u);
}

TEST(scheduler, interleaves_on_yield) {
    // 轮转：两根协程交替推进，而不是一根跑完再跑另一根
    std::string trace;
    spawn([&] {
        for (int i = 0; i < 3; ++i) {
            trace += "甲";
            yield();
        }
    }).value();
    spawn([&] {
        for (int i = 0; i < 3; ++i) {
            trace += "乙";
            yield();
        }
    }).value();
    EXPECT(run(), ==, 2u);
    EXPECT(trace, ==, "甲乙甲乙甲乙");
}

TEST(scheduler, lets_a_coroutine_spawn_more) {
    // 嵌套 spawn：协程里起的协程也要被跑到，而且不能让队列在脚下失效
    int leaves = 0;
    spawn([&] {
        for (int i = 0; i < 4; ++i) {
            spawn([&] { ++leaves; }).value();
        }
    }).value();
    const std::size_t finished = run();
    EXPECT(finished, ==, 5u);        // 1 根父 + 4 根子
    EXPECT(leaves, ==, 4);
}

TEST(scheduler, spawns_many_without_losing_any) {
    const int count = 200;
    int done = 0;
    for (int i = 0; i < count; ++i) {
        wavvix::spawn_with_stack(32 * 1024, [&] {
            yield();
            ++done;
            yield();
        }).value();
    }
    EXPECT(run(), ==, static_cast<std::size_t>(count));
    EXPECT(done, ==, count);
}

TEST(scheduler, sleeping_a_coroutine_does_not_block_the_others) {
    // 这是「睡的是协程不是线程」的全部意义：睡着的那根不占着 vCPU，
    // 所以另一根能在它睡的这段时间里跑很多轮。
    int spins = 0;
    bool slept = false;

    spawn([&] {
        sleep_for(ms(30));
        slept = true;
    }).value();
    spawn([&] {
        // 等到对面睡醒为止，一直让出
        while (!slept) {
            ++spins;
            yield();
        }
    }).value();

    wavvix::stopwatch watch;
    EXPECT(run(), ==, 2u);
    const std::chrono::nanoseconds elapsed = watch.elapsed();

    EXPECT(slept, ==, true);
    EXPECT(spins > 10, ==, true);     // 对面睡着的时候我们转了很多轮
    // 至少睡够了（允许调度误差，不设上限：CI 机器可能很慢）
    EXPECT(elapsed >= ms(25), ==, true);
}

TEST(scheduler, wakes_sleepers_in_time_order) {
    std::string order;
    spawn([&] { sleep_for(ms(30)); order += "C"; }).value();
    spawn([&] { sleep_for(ms(10)); order += "A"; }).value();
    spawn([&] { sleep_for(ms(20)); order += "B"; }).value();
    EXPECT(run(), ==, 3u);
    EXPECT(order, ==, "ABC");
}

TEST(scheduler, wakes_sleepers_in_time_order_when_they_expire_in_one_round) {
    // 上面那条在 Linux 上查不出问题：空闲路径正好睡到最早那个 deadline，所以三个各自
    // 单独一轮醒，次序怎么排都对。**它在 macOS 和 Windows 上红过**（拿到 "CAB" 和
    // "ACB"），因为那两个平台定时器粒度更粗，一次睡眠 overshoot 到好几个一起到期，
    // 而当时的代码是按容器次序（也就是 spawn 次序）把它们搬回 ready 的。
    //
    // 这一条不靠平台的定时器粒度，而是**让三个 deadline 必定落在同一轮**：另一根协程
    // 不让出地占住 vCPU 直到所有 deadline 都过去。所以它在 Linux 上也能复现那个 bug。
    std::string order;
    spawn([&] { sleep_for(ms(30)); order += "C"; }).value();
    spawn([&] { sleep_for(ms(10)); order += "A"; }).value();
    spawn([&] { sleep_for(ms(20)); order += "B"; }).value();
    spawn([&] {
        const wavvix::stopwatch hog;
        while (hog.elapsed() < ms(45)) {
            // 故意不 yield：这一根跑完之前调度器不会去看定时器
        }
    }).value();

    EXPECT(run(), ==, 4u);
    EXPECT(order, ==, "ABC");
}

TEST(scheduler, sleeps_the_thread_when_called_outside_a_coroutine) {
    // 不在协程里就真睡线程，这样随手写个 sleep 在哪儿都对
    wavvix::stopwatch watch;
    sleep_for(ms(15));
    EXPECT(watch.elapsed() >= ms(12), ==, true);
}

TEST(scheduler, counts_what_is_still_scheduled) {
    EXPECT(scheduled(), ==, 0u);
    spawn([] {}).value();
    spawn([] {}).value();
    EXPECT(scheduled(), ==, 2u);
    run();
    EXPECT(scheduled(), ==, 0u);
}

TEST(scheduler, keeps_each_coroutines_own_stack) {
    // 每根协程的局部变量互不干扰，哪怕交替跑
    std::vector<int> results(3, 0);
    for (int i = 0; i < 3; ++i) {
        const int mine = i;
        wavvix::spawn_with_stack(32 * 1024, [&results, mine] {
            int accumulated = 0;
            char filler[2048];
            for (int round = 0; round < 5; ++round) {
                filler[round] = static_cast<char>(mine + round);
                accumulated += mine + round;
                yield();
            }
            // 让出五次之后，自己的栈上那些东西还在
            EXPECT(filler[4], ==, static_cast<char>(mine + 4));
            results[static_cast<std::size_t>(mine)] = accumulated;
        }).value();
    }
    EXPECT(run(), ==, 3u);
    EXPECT(results[0], ==, 0 + 1 + 2 + 3 + 4);
    EXPECT(results[1], ==, 1 + 2 + 3 + 4 + 5);
    EXPECT(results[2], ==, 2 + 3 + 4 + 5 + 6);
}

TEST(scheduler, run_returns_zero_after_everything_finished) {
    spawn([] {}).value();
    EXPECT(run(), ==, 1u);
    EXPECT(run(), ==, 0u);        // 再跑一次没活了
}
