// 线程带着活块退出后，它的页进共享池：fork 出的子进程能回收，之后别的线程接手同一页、不多提交内存。
#include "test_util.h"

#include <culib/mem.h>
#include <mem_stats.h>

#include <cerrno>
#include <cstring>
#include <thread>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

wavvix::mem::detail::stats stats() {
    return wavvix::mem::detail::get_stats();
}

#if !defined(_WIN32)
// 共享池里已经有页时 fork：子进程释放全部块并 collect，内存要能全部还掉
void check_fork_child_reclaims(void* kept, void* pin) {
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        alarm(5);
        wavvix::mem::free(kept);
        wavvix::mem::free(pin);
        wavvix::mem::collect();
        const wavvix::mem::detail::stats left = stats();
        _exit(left.committed_bytes == 0 && left.abandoned_pages == 0 ? 0 : 1);
    }

    int status = 0;
    pid_t waited = waitpid(child, &status, 0);
    while (waited < 0 && errno == EINTR) {
        waited = waitpid(child, &status, 0);
    }
    CHECK(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
#endif

}  // namespace

int main() {
    void* const pin = wavvix::mem::alloc(1024);   // 主线程自己的页
    CHECK(pin != NULL);

    void* kept = NULL;
    std::thread producer([&kept] {
        kept = wavvix::mem::alloc(1024);
        CHECK(kept != NULL);
        std::memset(kept, 0x3B, 1024);
    });
    producer.join();
    CHECK(stats().abandoned_pages == 1);

#if !defined(_WIN32)
    check_fork_child_reclaims(kept, pin);
#endif

    // 接手原来那一页：已提交的内存不变，块内容原样
    const wavvix::mem::detail::stats before = stats();
    void* reused = NULL;
    std::thread consumer([&reused] {
        reused = wavvix::mem::alloc(1024);
        CHECK(reused != NULL);
        std::memset(reused, 0x7C, 1024);
    });
    consumer.join();
    const wavvix::mem::detail::stats after = stats();
    CHECK_MSG(after.committed_bytes == before.committed_bytes, "an abandoned page was ignored");
    CHECK(after.abandoned_pages == 1);
    for (int i = 0; i < 1024; ++i) {
        CHECK(static_cast<unsigned char*>(kept)[i] == 0x3B);
        CHECK(static_cast<unsigned char*>(reused)[i] == 0x7C);
    }

    wavvix::mem::free(kept);
    wavvix::mem::free(reused);
    wavvix::mem::free(pin);
    wavvix::mem::collect();
    CHECK(stats().committed_bytes == 0);
    CHECK(stats().abandoned_pages == 0);
    return memtest::summary("test_abandoned");
}
