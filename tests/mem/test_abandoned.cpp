#include "test_util.h"
#include <mem_core.h>

#include <cstring>
#include <thread>

#if !defined(_WIN32)
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

using wavvix::mem::detail::mem_stats_get;
using wavvix::mem::detail::mem_stats_t;

int main() {
    void* pin = wavvix::mem::alloc(1024);
    void* large = wavvix::mem::alloc(64u << 20);
    CHECK(pin != NULL && large != NULL);
    mem_stats_t before;
    mem_stats_get(&before);
    CHECK(before.arena_count == 2);

    // 轮询分配区域：先用一次第二个 arena，让生产者和接管者偏好不同区域。
    std::thread visit([] {
        void* p = wavvix::mem::alloc(60000);
        CHECK(p != NULL);
        wavvix::mem::free(p);
    });
    visit.join();
    wavvix::mem::collect();

    void* kept = NULL;
    std::thread producer([&] {
        kept = wavvix::mem::alloc(1024);
        CHECK(kept != NULL);
        std::memset(kept, 0x3B, 1024);
    });
    producer.join();
    mem_stats_get(&before);
    CHECK(before.abandoned_pages == 1);

#if !defined(_WIN32)
    // fork 时池中已有页；子进程能回收，父进程随后仍能接管。
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        alarm(5);
        wavvix::mem::free(kept);
        wavvix::mem::free(pin);
        wavvix::mem::free(large);
        wavvix::mem::collect();
        mem_stats_t collected;
        mem_stats_get(&collected);
        _exit(collected.committed_bytes == 0 && collected.abandoned_pages == 0 ? 0 : 1);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    CHECK(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif

    void* reused = NULL;
    std::thread consumer([&] {
        reused = wavvix::mem::alloc(1024);
        CHECK(reused != NULL);
        std::memset(reused, 0x7C, 1024);
    });
    consumer.join();
    mem_stats_t after;
    mem_stats_get(&after);
    std::printf("committed before / after cross-arena reuse: %lu / %lu KiB\n",
                static_cast<unsigned long>(before.committed_bytes >> 10),
                static_cast<unsigned long>(after.committed_bytes >> 10));
    CHECK_MSG(after.committed_bytes == before.committed_bytes,
              "an abandoned page in another arena was ignored");
    CHECK(after.abandoned_pages == 1);
    for (int i = 0; i < 1024; ++i) {
        CHECK(static_cast<unsigned char*>(kept)[i] == 0x3B);
        CHECK(static_cast<unsigned char*>(reused)[i] == 0x7C);
    }
    wavvix::mem::free(kept);
    wavvix::mem::free(reused);
    wavvix::mem::free(pin);
    wavvix::mem::free(large);
    wavvix::mem::collect();
    mem_stats_get(&after);
    CHECK(after.committed_bytes == 0);
    CHECK(after.abandoned_pages == 0);
    return memtest::summary("test_abandoned");
}
