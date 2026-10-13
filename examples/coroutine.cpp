// 最小的协程程序：main 建 runtime、起协程、等它们结束。更多用法见 runtime.cpp。
//
//     xmake run example_coroutine

#include <culib/coroutine.h>

#include <chrono>
#include <cstdio>
#include <functional>

int main() {
    wavvix::runtime runtime(2);   // 两根工作线程，协程都在它们上面跑

    int first = 0;
    int second = 0;
    wavvix::coroutine a([&] {
        wavvix::sleep_for(std::chrono::milliseconds(10));   // 只挂起这根协程
        first = 1;
    });
    wavvix::coroutine b([](int& out) { out = 2; }, std::ref(second));   // 参数按值保存，引用用 std::ref

    a.join();
    b.join();
    std::printf("sum: %d\n", first + second);
    return first + second == 3 ? 0 : 1;
}
