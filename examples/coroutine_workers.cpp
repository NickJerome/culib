#include <culib/coroutine.h>
#include <culib/scheduler.h>

#include <cstdio>
#include <utility>

int main() {
    wavvix::result<wavvix::scheduler_scope> opened =
        wavvix::scheduler_scope::with_workers(2);
    if (opened.is_err()) {
        std::fprintf(stderr, "%s\n", opened.error().to_string().c_str());
        return 1;
    }
    wavvix::scheduler_scope runtime = std::move(opened).value();

    int first = 0;
    int second = 0;
    wavvix::coroutine a([&] { first = 1; });
    wavvix::coroutine b([&] { second = 2; });
    a.join();
    b.join();
    std::printf("sum: %d\n", first + second);
    return first + second == 3 ? 0 : 1;
}
