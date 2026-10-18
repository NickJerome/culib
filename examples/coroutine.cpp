// 最小的协程程序：go() 起协程，wait_group 等它们结束。更多用法见 runtime.cpp。
//
//     xmake run example_coroutine

#include <culib/coroutine.h>

#include <chrono>
#include <cstdio>
#include <functional>

int main() {
    int first = 0;
    int second = 0;

    wavvix::wait_group group;   // 不用建运行时：第一次 go() 时自己起来
    group.go([&] {
        wavvix::sleep_for(std::chrono::milliseconds(10));   // 只挂起这根协程
        first = 1;
    });
    group.go([](int& out) { out = 2; }, std::ref(second));   // 参数按值保存，引用用 std::ref

    group.wait();
    std::printf("sum: %d\n", first + second);
    return first + second == 3 ? 0 : 1;
}
