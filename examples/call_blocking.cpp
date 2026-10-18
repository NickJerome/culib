// call_blocking：没法协作式让出的调用放到后台线程上，等待期间只挂起调用它的协程。
//
//     xmake run example_call_blocking

#include <culib/coroutine.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

// 假装是第三方库的同步接口：一调用就阻塞 50 ms。
int slow_double(int input) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return input * 2;
}

}  // namespace

int main() {
    int answer = 0;
    std::atomic<bool> returned(false);
    long spins = 0;

    wavvix::wait_group group;
    group.go([&] {
        // 协程里起的协程先排在本线程上：它能转起来，说明等 call_blocking 时这根线程没被占住。
        wavvix::wait_group other;
        other.go([&] {
            while (!returned.load()) {
                ++spins;
                wavvix::yield();
            }
        });
        answer = wavvix::call_blocking(slow_double, 21);   // 参数和直接调用一样传
        returned.store(true);
        other.wait();
    });
    group.wait();

    std::printf("answer: %d\n", answer);
    std::printf("等它的 50 ms 里，同一根工作线程上的另一根协程转了 %ld 轮\n", spins);
    return answer == 42 ? 0 : 1;
}
