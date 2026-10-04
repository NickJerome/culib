// 绑定检查到底拦不拦得住：同一个迁移动作，现在应该当场 abort。
#include <culib/coroutine.h>
#include <cstdio>
#include <thread>
int main(int argc, char**) {
    using wavvix::coroutine;
    if (argc > 1) {
        std::printf("=== 允许的：主线程造、工作线程第一次 resume（绑在工作线程上）===\n");
        coroutine worker([] { std::printf("   [协程] 跑在工作线程上\n"); coroutine::yield();
                              std::printf("   [协程] 第二段，还是同一根工作线程\n"); });
        std::thread w([&] { while (worker.resume()) {} });
        w.join();
        std::printf("   done=%s，没 abort\n", worker.done() ? "true" : "false");
        return 0;
    }
    std::printf("=== 被拦的：A 线程 resume 过，再从 B 线程 resume ===\n");
    coroutine worker([] { coroutine::yield(); std::printf("   不该走到这里\n"); });
    std::thread a([&] { std::printf("   [线程A] 第一次 resume，绑定\n"); worker.resume(); });
    a.join();
    std::thread b([&] { std::printf("   [线程B] 再 resume —— 应该当场 abort\n"); worker.resume(); });
    b.join();
    std::printf("   **没拦住** —— 检查失效了\n");
    return 0;
}
