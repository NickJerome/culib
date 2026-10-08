#include <culib/coroutine.h>
#include <culib/defer.h>

#include <chrono>
#include <cstdio>

int main() {
    wavvix::coroutine::init();
    defer { wavvix::coroutine::shutdown(); };
    int completed = 0;
    wavvix::coroutine first([&] {
        wavvix::sleep_for(std::chrono::milliseconds(10));
        ++completed;
    });
    wavvix::coroutine second([&] {
        ++completed;
    });

    first.join();
    second.join();
    std::printf("completed: %d\n", completed);
    return completed == 2 ? 0 : 1;
}
