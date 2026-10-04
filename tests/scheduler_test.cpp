#include <culib/scheduler.h>
#include <culib/stopwatch.h>
#include <culib/test.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace {

using wavvix::run;
using wavvix::scheduled;
using wavvix::sleep_for;
using wavvix::spawn;
using wavvix::yield;

typedef std::chrono::milliseconds ms;

}  // namespace

TEST(scheduler, runs_nothing_when_nothing_was_spawned) {
    EXPECT(scheduled(), ==, 0u);
    EXPECT(run(), ==, 0u);
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
