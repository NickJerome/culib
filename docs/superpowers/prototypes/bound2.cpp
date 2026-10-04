// 两根线程**同时活着**（真实 M:N 的形状）时，检查拦不拦得住？
#include <culib/coroutine.h>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
namespace {
std::mutex lock; std::condition_variable gate; int turn = -1;
void wait_turn(int m) { std::unique_lock<std::mutex> h(lock); gate.wait(h, [&]{ return turn==m||turn==9; }); }
void pass_turn(int n) { std::unique_lock<std::mutex> h(lock); turn=n; gate.notify_all(); }
}
int main() {
    using wavvix::coroutine;
    coroutine worker([] { coroutine::yield(); std::printf("   不该走到这里\n"); });
    std::thread a([&]{ wait_turn(0); std::printf("   [线程A] 第一次 resume\n"); worker.resume();
                       pass_turn(1); wait_turn(9); });
    std::thread b([&]{ wait_turn(1); std::printf("   [线程B] 再 resume（A 还活着）\n");
                       worker.resume(); std::printf("   **没拦住**\n"); pass_turn(9); });
    pass_turn(0); b.join(); a.join();
    return 0;
}
