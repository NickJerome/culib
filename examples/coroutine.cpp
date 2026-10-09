#include <culib/coroutine.h>

#include <chrono>
#include <cstdio>
#include <functional>

int main() {
    wavvix::scheduler_scope runtime;
    int completed = 0;
    wavvix::coroutine first([&] {
        wavvix::sleep_for(std::chrono::milliseconds(10));
        ++completed;
    });
    wavvix::coroutine second([](int& count) { ++count; }, std::ref(completed));

    first.join();
    second.join();
    std::printf("completed: %d\n", completed);
    return completed == 2 ? 0 : 1;
}
