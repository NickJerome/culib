#include <culib/coroutine.h>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
void tls_set(int v); int tls_get(); void* tls_addr(); unsigned long which_thread();
namespace {
using wavvix::coroutine;
std::mutex lock; std::condition_variable gate; int turn = -1;
void wait_turn(int mine) { std::unique_lock<std::mutex> h(lock); gate.wait(h, [&]{ return turn==mine||turn==9; }); }
void pass_turn(int n) { std::unique_lock<std::mutex> h(lock); turn=n; gate.notify_all(); }
}
int main() {
    void* cached = 0; unsigned long tid_before = 0;
    coroutine worker([&] {
        tls_set(100);
        cached = tls_addr();
        tid_before = which_thread();
        std::printf("   [协程] 第一段：线程=%lx  &slot=%p  slot=%d\n",
                    tid_before, cached, tls_get());
        coroutine::yield();
        std::printf("   [协程] 第二段：线程=%lx  &slot=%p  slot=%d\n",
                    which_thread(), tls_addr(), tls_get());
        std::printf("   → 线程%s，TLS 地址%s\n",
                    which_thread()==tid_before ? "相同" : "**变了**",
                    tls_addr()==cached ? "相同" : "**也变了**");
        std::printf("   → 协程第一段写进 slot 的 100 **看不见了**，现在读到 %d（是线程B 的那一份）\n",
                    tls_get());
    });
    std::thread a([&]{ tls_set(1); wait_turn(0); std::printf("   [线程A] resume\n"); worker.resume(); pass_turn(1); wait_turn(9); });
    std::thread b([&]{ tls_set(2); wait_turn(1); std::printf("   [线程B] resume\n"); while (worker.resume()) {} pass_turn(9); });
    pass_turn(0); b.join(); a.join();
    return 0;
}
