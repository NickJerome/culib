#include <culib/blocking_pool.h>
#include <culib/scheduler.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <utility>

int main() {
    wavvix::result<wavvix::blocking_pool> created =
        wavvix::blocking_pool::create(2);
    if (created.is_err()) {
        std::fprintf(stderr, "%s\n", created.error().to_string().c_str());
        return 1;
    }
    wavvix::blocking_pool pool = std::move(created).value();
    int answer = 0;
    bool completed = false;
    {
        wavvix::scheduler_scope runtime;
        wavvix::spawn([&] {
            wavvix::result<void> done = pool.run(
                [](int input, int& output) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    output = input * 2;
                }, 21, std::ref(answer));
            completed = done.is_ok();
        });
        wavvix::spawn([] { std::puts("another coroutine ran while work blocked"); });
    }
    if (!completed) return 1;
    std::printf("answer: %d\n", answer);
    return 0;
}
