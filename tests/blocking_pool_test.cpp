#include <culib/blocking_pool.h>
#include <culib/coroutine.h>
#include <culib/scheduler.h>
#include <culib/test.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace {
typedef std::chrono::milliseconds ms;
}

TEST(blocking_pool, leaves_the_vcpu_free_while_work_blocks) {
    wavvix::result<wavvix::blocking_pool> created = wavvix::blocking_pool::create();
    EXPECT(created.is_ok(), ==, true);
    if (created.is_err()) return;
    wavvix::blocking_pool pool = std::move(created).value();
    bool other_ran = false;
    bool worker_finished = false;
    bool resumed_after_both = false;
    {
        wavvix::scheduler_scope runtime;
        wavvix::spawn([&] {
            wavvix::result<void> done = pool.run([&] {
                std::this_thread::sleep_for(ms(30));
                worker_finished = true;
            });
            resumed_after_both = done.is_ok() && worker_finished && other_ran;
        }).value();
        wavvix::spawn([&] { other_ran = true; }).value();
    }
    EXPECT(pool.size(), ==, 1u);
    EXPECT(resumed_after_both, ==, true);
}

TEST(blocking_pool, runs_two_blocking_calls_on_two_system_threads) {
    wavvix::result<wavvix::blocking_pool> created = wavvix::blocking_pool::create(2);
    EXPECT(created.is_ok(), ==, true);
    if (created.is_err()) return;
    wavvix::blocking_pool pool = std::move(created).value();
    std::mutex gate;
    std::condition_variable changed;
    int entered = 0;
    bool both_reached[2] = {false, false};
    bool returned[2] = {false, false};
    {
        wavvix::scheduler_scope runtime;
        for (int i = 0; i < 2; ++i) {
            wavvix::spawn([&, i] {
                wavvix::result<void> done = pool.run([&, i] {
                    std::unique_lock<std::mutex> lock(gate);
                    ++entered;
                    changed.notify_all();
                    both_reached[i] = changed.wait_for(lock, std::chrono::seconds(2),
                                                       [&] { return entered == 2; });
                });
                returned[i] = done.is_ok();
            }).value();
        }
    }
    EXPECT(pool.size(), ==, 2u);
    EXPECT(both_reached[0] && both_reached[1], ==, true);
    EXPECT(returned[0] && returned[1], ==, true);
}

TEST(blocking_pool, forwards_move_only_arguments_and_rejects_main_thread_wait) {
    wavvix::result<wavvix::blocking_pool> created = wavvix::blocking_pool::create();
    EXPECT(created.is_ok(), ==, true);
    if (created.is_err()) return;
    wavvix::blocking_pool pool = std::move(created).value();
    EXPECT(pool.run([] {}).is_err(), ==, true);
    int value = 0;
    bool returned = false;
    {
        wavvix::scheduler_scope runtime;
        wavvix::spawn([&] {
            wavvix::result<void> done = pool.run(
                [](std::unique_ptr<int> input, int& output) { output = *input; },
                std::unique_ptr<int>(new int(17)), std::ref(value));
            returned = done.is_ok();
        }).value();
    }
    EXPECT(returned, ==, true);
    EXPECT(value, ==, 17);
}

TEST(blocking_pool, wakes_each_call_on_its_own_vcpu) {
    wavvix::result<wavvix::blocking_pool> created = wavvix::blocking_pool::create(2);
    EXPECT(created.is_ok(), ==, true);
    if (created.is_err()) return;
    wavvix::blocking_pool pool = std::move(created).value();
    std::atomic<int> completed(0);
    const auto run_on_vcpu = [&] {
        wavvix::scheduler_scope runtime;
        wavvix::coroutine task([&] {
            if (pool.run([] {}).is_ok()) completed.fetch_add(1);
        });
        task.join();
    };
    std::thread first(run_on_vcpu);
    std::thread second(run_on_vcpu);
    first.join();
    second.join();
    EXPECT(completed.load(), ==, 2);
}

TEST(blocking_pool, resumes_a_coroutine_dispatched_to_a_worker_vcpu) {
    wavvix::result<wavvix::blocking_pool> created = wavvix::blocking_pool::create();
    EXPECT(created.is_ok(), ==, true);
    if (created.is_err()) return;
    wavvix::blocking_pool pool = std::move(created).value();
    wavvix::result<wavvix::scheduler_scope> opened =
        wavvix::scheduler_scope::with_workers(1);
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) return;
    wavvix::scheduler_scope runtime = std::move(opened).value();
    int answer = 0;
    bool returned = false;
    wavvix::coroutine task([&] {
        returned = pool.run([&] { answer = 42; }).is_ok();
    });
    task.join();
    EXPECT(returned, ==, true);
    EXPECT(answer, ==, 42);
}
