// call_blocking：阻塞调用放到后台线程上，等待期间只挂起调用它的协程。

#include <culib/coroutine.h>
#include <culib/test.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

typedef std::chrono::milliseconds ms;

}  // namespace

TEST(call_blocking, runs_the_function_on_another_thread) {
    std::thread::id worker_thread;
    std::thread::id call_thread;
    wavvix::wait_group group;
    group.go([&] {
        worker_thread = std::this_thread::get_id();
        wavvix::call_blocking([&] { call_thread = std::this_thread::get_id(); });
    });
    group.wait();

    EXPECT(call_thread != worker_thread, ==, true);
    EXPECT(call_thread != std::this_thread::get_id(), ==, true);
}

TEST(call_blocking, passes_arguments_like_a_direct_call) {
    // 参数和直接调用一样传：左值按引用，右值照样移动，不拷贝。
    int doubled = 0;
    std::string text = "原样";
    std::unique_ptr<int> returned;

    wavvix::wait_group group;
    group.go([&] {
        wavvix::call_blocking([](std::unique_ptr<int> input, int& output) { output = *input * 2; },
                              std::unique_ptr<int>(new int(21)), doubled);
        wavvix::call_blocking([](std::string& target) { target += "被改了"; }, text);
        returned = wavvix::call_blocking([] { return std::unique_ptr<int>(new int(7)); });
    });
    group.wait();

    EXPECT(doubled, ==, 42);
    EXPECT(text, ==, "原样被改了");
    EXPECT(returned && *returned == 7, ==, true);
}

#if WAVVIX_DETAIL_HAS_EXCEPTIONS
TEST(call_blocking, rethrows_what_the_function_throws) {
    std::string caught;
    int result = 0;
    wavvix::wait_group group;
    group.go([&] {
        try {
            result = wavvix::call_blocking([](int input) -> int {
                if (input < 0) {
                    throw std::runtime_error("后台线程上抛的");
                }
                return input;
            }, -1);
        } catch (const std::runtime_error& error) {
            caught = error.what();
        }
    });
    group.wait();

    EXPECT(caught, ==, "后台线程上抛的");
    EXPECT(result, ==, 0);
}
#endif

TEST(call_blocking, calls_directly_outside_a_coroutine) {
    std::thread::id call_thread;
    const int value = wavvix::call_blocking([&] {
        call_thread = std::this_thread::get_id();
        return 5;
    });

    EXPECT(value, ==, 5);
    EXPECT(call_thread == std::this_thread::get_id(), ==, true);
}

TEST(call_blocking, runs_calls_from_several_coroutines_side_by_side) {
    // 两个调用都占着后台线程等对方进来：只有同时在两根后台线程上，双方才都等得到。
    std::mutex gate;
    std::condition_variable changed;
    int entered = 0;
    bool met[2] = {false, false};
    const auto meet = [&](int index) {
        wavvix::call_blocking([&] {
            std::unique_lock<std::mutex> lock(gate);
            ++entered;
            changed.notify_all();
            met[index] = changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered == 2; });
        });
    };

    wavvix::wait_group group;
    group.go(meet, 0);
    group.go(meet, 1);
    group.wait();

    EXPECT(met[0] && met[1], ==, true);
}

TEST(call_blocking, queues_calls_beyond_the_thread_limit) {
    // 后台线程最多 64 根；更多的调用排队，一个不丢。
    std::atomic<int> running(0);
    std::atomic<int> peak(0);
    std::atomic<int> finished(0);
    wavvix::wait_group group;
    for (int i = 0; i < 100; ++i) {
        group.go([&] {
            wavvix::call_blocking([&] {
                const int now = running.fetch_add(1) + 1;
                int seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {
                }
                std::this_thread::sleep_for(ms(5));
                running.fetch_sub(1);
            });
            finished.fetch_add(1);
        });
    }
    group.wait();

    EXPECT(finished.load(), ==, 100);
    EXPECT(peak.load() <= 64, ==, true);
}

TEST(call_blocking, wakes_each_caller_on_its_own_worker) {
    std::atomic<int> completed(0);
    std::atomic<int> moved(0);
    wavvix::wait_group group;
    for (unsigned i = 0; i < 4; ++i) {
        group.go([&] {
            const std::thread::id home = std::this_thread::get_id();
            for (int n = 0; n < 50; ++n) {
                wavvix::call_blocking([] {});
                if (std::this_thread::get_id() != home) {
                    moved.fetch_add(1);
                }
                completed.fetch_add(1);
            }
        });
    }
    group.wait();

    EXPECT(completed.load(), ==, 200);
    EXPECT(moved.load(), ==, 0);
}
