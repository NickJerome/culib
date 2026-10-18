// 只有一根工作线程时的 call_blocking：等后台线程的这段时间，同一根线程照常跑别的协程。

#include <culib/coroutine.h>
#include <culib/test.h>

#include <chrono>
#include <thread>

TEST(call_blocking_order, returns_the_result_while_the_worker_runs_other_coroutines) {
    int value = 0;
    bool other_ran = false;
    bool other_ran_before_return = false;

    wavvix::wait_group group;
    group.go([&] {
        value = wavvix::call_blocking([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            return 42;
        });
        other_ran_before_return = other_ran;
    });
    group.go([&] { other_ran = true; });
    group.wait();

    EXPECT(value, ==, 42);
    EXPECT(other_ran_before_return, ==, true);
}
