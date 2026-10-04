#include <culib/coroutine.h>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using wavvix::coroutine;
#if defined(_MSC_VER)
__declspec(thread) int slot = 0;
#else
__thread int slot = 0;
#endif
// 不让编译器把 *p 折叠回 TLS 访问
int* opaque(int* p) { volatile int* v = p; return const_cast<int*>(v); }
}  // namespace

int main() {
    std::printf("=== A. 缓存 TLS 地址，跨线程后比**地址**不比值 ===\n");
    {
        int* cached = 0;
        coroutine worker([&] {
            slot = 100;
            cached = opaque(&slot);
            std::printf("   线程A 上：&slot=%p  *cached=%d\n", (void*)cached, *cached);
            coroutine::yield();
            int* now = opaque(&slot);
            std::printf("   线程B 上：&slot=%p  *cached=%d  本线程 slot=%d\n",
                        (void*)now, *cached, slot);
            std::printf("   地址%s —— %s\n", cached == now ? "相同" : "**不同**",
                        cached == now ? "没有发生迁移效应" :
                        "**缓存下来的 thread_local 指针在脚下换了线程**");
        });
        std::thread a([&] { slot = 1; worker.resume(); });  a.join();
        std::thread b([&] { slot = 2; while (worker.resume()) {} }); b.join();
    }

    std::printf("\n=== B. 在 catch 块里跨线程，再碰 __cxa_eh_globals ===\n");
    {
        std::string outcome = "(没走到)";
        coroutine worker([&] {
            try {
                throw std::runtime_error("原始异常");
            } catch (const std::exception& error) {
                std::printf("   接住了「%s」，在 catch 块里让出\n", error.what());
                coroutine::yield();
                // 这几个都读 __cxa_eh_globals（按线程存的）
                std::exception_ptr captured = std::current_exception();
                outcome = captured ? "current_exception() 拿到了" : "**current_exception() 是空的**";
                std::printf("   换线程回来：%s\n", outcome.c_str());
                if (captured) {
                    try { std::rethrow_exception(captured); }
                    catch (const std::exception& again) {
                        std::printf("   重抛也接住了：「%s」\n", again.what());
                    }
                }
            }
        });
        std::thread a([&] { worker.resume(); }); a.join();
        std::thread b([&] { while (worker.resume()) {} }); b.join();
    }

    std::printf("\n=== C. 不迁移的对照：同一根线程上做 B 的同一件事 ===\n");
    {
        coroutine worker([&] {
            try {
                throw std::runtime_error("原始异常");
            } catch (const std::exception& error) {
                (void)error;
                coroutine::yield();
                std::exception_ptr captured = std::current_exception();
                std::printf("   同线程：%s\n",
                            captured ? "current_exception() 拿到了" : "**空的**");
            }
        });
        while (worker.resume()) {}
    }
    std::printf("\n跑完，没崩\n");
    return 0;
}
