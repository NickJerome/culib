// 栈池：一大群协程跑完以后，栈留在空闲表里给下一群用；多出 64 根的那些一阵子没人用，就还给系统。

#include <cu/coroutine.h>
#include <cu/test.h>

#include "runtime/stack.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

typedef std::chrono::steady_clock clock_type;

// 同时活着 count 根协程：都睡到同一时刻再结束。返回跑完的根数。
int run_crowd(int count) {
    const clock_type::time_point until = clock_type::now() + std::chrono::milliseconds(50);
    std::atomic<int> finished(0);
    std::vector<cu::coroutine> crowd;
    for (int i = 0; i < count; ++i) {
        crowd.emplace_back([until, &finished] {
            cu::this_coroutine::sleep_until(until);
            finished.fetch_add(1);
        });
    }
    for (std::size_t i = 0; i < crowd.size(); ++i) {
        crowd[i].join();
    }
    return finished.load();
}

// 等空闲表降到 64 根以内，最多等 limit。
bool wait_for_release(std::chrono::seconds limit) {
    const clock_type::time_point deadline = clock_type::now() + limit;
    while (cu::detail::free_stack_count() > 64) {
        if (clock_type::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

}  // namespace

TEST(stack, a_crowd_returns_its_extra_stacks_to_the_system) {
    // 比所有工作线程手边能留的（每根 16 根）多出 1000 根，多出来的一定进空闲表
    const int crowd = 1000 + 16 * static_cast<int>(std::thread::hardware_concurrency());
    EXPECT(run_crowd(crowd), ==, crowd);
    EXPECT(cu::detail::free_stack_count(), >, 64u);
    EXPECT(wait_for_release(std::chrono::seconds(5)), ==, true);

    // 还掉以后再来一群，用的是新要的栈
    EXPECT(run_crowd(crowd), ==, crowd);
    EXPECT(wait_for_release(std::chrono::seconds(5)), ==, true);
}
