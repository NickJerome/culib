#include <culib/coroutine.h>
#include <culib/scheduler.h>
#include <culib/stopwatch.h>
#include <culib/test.h>

#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
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

}  // namespace

TEST(scheduler, runs_nothing_when_nothing_was_spawned) {
    EXPECT(scheduled(), ==, 0u);
    EXPECT(run(), ==, 0u);
}

TEST(scheduler, scope_runs_submitted_coroutines_on_exit) {
    int finished = 0;
    {
        wavvix::scheduler_scope scope;
        spawn([&] { ++finished; });
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
            spawn([&] { ++finished; });
        }
        EXPECT(finished, ==, 0);
        EXPECT(scheduled(), ==, 1u);
    }
    EXPECT(finished, ==, 1);
    EXPECT(scheduled(), ==, 0u);
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

TEST(scheduler, coroutine_object_reports_unavailable_stack) {
    wavvix::coroutine too_small([] {}, 0);
    wavvix::coroutine too_large([] {}, (std::numeric_limits<std::size_t>::max)());
    EXPECT(too_small.joinable(), ==, false);
    EXPECT(too_large.joinable(), ==, false);
    EXPECT(scheduled(), ==, 0u);
}

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
    });
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
    wavvix::coroutine::init();
    int ran = 0;
    wavvix::coroutine worker([&] {
        sleep_for(ms(5));
        ++ran;
    });
    sleep_for(ms(20));
    EXPECT(ran, ==, 1);
    worker.join();
    wavvix::coroutine::shutdown();
    EXPECT(scheduled(), ==, 0u);
}

TEST(scheduler, main_thread_sleep_runs_tasks_due_before_overshoot) {
    wavvix::coroutine::init();
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
    wavvix::coroutine::shutdown();
}

TEST(scheduler, main_thread_yield_runs_another_coroutine) {
    wavvix::coroutine::init();
    int ran = 0;
    wavvix::coroutine worker([&] { ++ran; });
    wavvix::yield();
    EXPECT(ran, ==, 1);
    worker.join();
    wavvix::coroutine::shutdown();
}

TEST(scheduler, joining_one_coroutine_leaves_unrelated_work_scheduled) {
    int other = 0;
    spawn([&] { sleep_for(ms(20)); ++other; });
    wavvix::coroutine worker([] {});
    worker.join();
    EXPECT(other, ==, 0);
    EXPECT(scheduled(), ==, 1u);
    EXPECT(run(), ==, 1u);
    EXPECT(other, ==, 1);
}

TEST(scheduler, runs_a_single_coroutine_to_completion) {
    int ran = 0;
    spawn([&] { ++ran; });
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
    });
    spawn([&] {
        for (int i = 0; i < 3; ++i) {
            trace += "乙";
            yield();
        }
    });
    EXPECT(run(), ==, 2u);
    EXPECT(trace, ==, "甲乙甲乙甲乙");
}

TEST(scheduler, lets_a_coroutine_spawn_more) {
    // 嵌套 spawn：协程里起的协程也要被跑到，而且不能让队列在脚下失效
    int leaves = 0;
    spawn([&] {
        for (int i = 0; i < 4; ++i) {
            spawn([&] { ++leaves; });
        }
    });
    const std::size_t finished = run();
    EXPECT(finished, ==, 5u);        // 1 根父 + 4 根子
    EXPECT(leaves, ==, 4);
}

TEST(scheduler, spawns_many_without_losing_any) {
    const int count = 200;
    int done = 0;
    for (int i = 0; i < count; ++i) {
        spawn([&] {
            yield();
            ++done;
            yield();
        }, 32 * 1024);
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
    });
    spawn([&] {
        // 等到对面睡醒为止，一直让出
        while (!slept) {
            ++spins;
            yield();
        }
    });

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
    spawn([&] { sleep_for(ms(30)); order += "C"; });
    spawn([&] { sleep_for(ms(10)); order += "A"; });
    spawn([&] { sleep_for(ms(20)); order += "B"; });
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
    spawn([&] { sleep_for(ms(30)); order += "C"; });
    spawn([&] { sleep_for(ms(10)); order += "A"; });
    spawn([&] { sleep_for(ms(20)); order += "B"; });
    spawn([&] {
        const wavvix::stopwatch hog;
        while (hog.elapsed() < ms(45)) {
            // 故意不 yield：这一根跑完之前调度器不会去看定时器
        }
    });

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
    spawn([] {});
    spawn([] {});
    EXPECT(scheduled(), ==, 2u);
    run();
    EXPECT(scheduled(), ==, 0u);
}

TEST(scheduler, keeps_each_coroutines_own_stack) {
    // 每根协程的局部变量互不干扰，哪怕交替跑
    std::vector<int> results(3, 0);
    for (int i = 0; i < 3; ++i) {
        const int mine = i;
        spawn([&results, mine] {
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
        }, 32 * 1024);
    }
    EXPECT(run(), ==, 3u);
    EXPECT(results[0], ==, 0 + 1 + 2 + 3 + 4);
    EXPECT(results[1], ==, 1 + 2 + 3 + 4 + 5);
    EXPECT(results[2], ==, 2 + 3 + 4 + 5 + 6);
}

TEST(scheduler, run_returns_zero_after_everything_finished) {
    spawn([] {});
    EXPECT(run(), ==, 1u);
    EXPECT(run(), ==, 0u);        // 再跑一次没活了
}
