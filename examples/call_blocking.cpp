// call_blocking：没法协作式让出的调用放到后台线程上，等待期间只挂起调用它的协程。
//
//     xmake run example_call_blocking

#include <culib/coroutine.h>
#include <culib/runtime.h>

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
    wavvix::runtime runtime(1);   // 只有一根工作线程，看得出它没被占住

    int answer = 0;
    bool returned = false;
    long spins = 0;
    wavvix::coroutine caller([&] {
        answer = wavvix::call_blocking(slow_double, 21);   // 参数和直接调用一样传
        returned = true;
    });
    wavvix::coroutine other([&] {
        while (!returned) {
            ++spins;
            wavvix::yield();
        }
    });
    caller.join();
    other.join();

    std::printf("answer: %d\n", answer);
    std::printf("等它的 50 ms 里，同一根工作线程上的另一根协程转了 %ld 轮\n", spins);
    return answer == 42 ? 0 : 1;
}
