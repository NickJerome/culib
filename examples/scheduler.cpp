// 调度器的 spawn()/run() 用法；持有句柄并 join() 的用法见 coroutine.cpp。

#include <culib/lexical_cast.h>
#include <culib/scheduler.h>
#include <culib/stopwatch.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

typedef std::chrono::milliseconds ms;
using wavvix::run;
using wavvix::sleep_for;
using wavvix::spawn;
using wavvix::yield;

}  // namespace

int main() {
    std::printf("1. spawn 完就不管，调度器负责推\n");
    {
        std::string trace;
        spawn([&] {
            for (int i = 0; i < 3; ++i) {
                trace += "甲";
                yield();
            }
        }).value();
        spawn([&] {
            for (int i = 0; i < 3; ++i) {
                trace += "乙";
                yield();
            }
        }).value();
        const std::size_t finished = run();
        std::printf("   执行顺序 %s   跑完 %zu 根（轮转，不是一根跑到底）\n", trace.c_str(),
                    finished);
    }

    std::printf("\n2. 睡的是协程，不是线程\n");
    {
        bool slept = false;
        long spins = 0;
        spawn([&] {
            sleep_for(ms(30));
            slept = true;
        }).value();
        spawn([&] {
            while (!slept) {
                ++spins;
                yield();
            }
        }).value();
        wavvix::stopwatch watch;
        run();
        std::printf("   一根睡了 30 ms，另一根在这段时间里转了 %ld 轮\n", spins);
        std::printf("   总共花了 %s —— 线程没被睡住，只有那一根协程在等\n",
                    wavvix::format_duration(watch.elapsed()).c_str());
    }

    std::printf("\n3. 定时器按时间排序唤醒\n");
    {
        std::string order;
        spawn([&] { sleep_for(ms(30)); order += "C"; }).value();
        spawn([&] { sleep_for(ms(10)); order += "A"; }).value();
        spawn([&] { sleep_for(ms(20)); order += "B"; }).value();
        run();
        std::printf("   spawn 的顺序是 C A B，醒来的顺序是 %s\n", order.c_str());
    }

    std::printf("\n4. 协程里还能再 spawn\n");
    {
        int leaves = 0;
        spawn([&] {
            for (int i = 0; i < 4; ++i) spawn([&] { ++leaves; }).value();
        }).value();
        const std::size_t finished = run();
        std::printf("   一根父协程起了 4 根子协程，总共跑完 %zu 根\n", finished);
    }

    std::printf("\n5. vCPU 模型：一根线程一个调度器，想用多核就多调几次 run()\n");
    {
        const int vcpus = 3;
        static const int per_vcpu = 50;
        std::vector<std::size_t> finished(static_cast<std::size_t>(vcpus), 0);
        std::vector<std::thread> carriers;

        wavvix::stopwatch watch;
        for (int v = 0; v < vcpus; ++v) {
            std::size_t* const slot = &finished[static_cast<std::size_t>(v)];
            carriers.push_back(std::thread([slot] {
                for (int i = 0; i < per_vcpu; ++i) {
                    wavvix::spawn_with_stack(32 * 1024, [] {
                        // 一点活，中间让出两次
                        yield();
                        yield();
                    }).value();
                }
                *slot = run();          // 把这根线程变成 vCPU
            }));
        }
        for (std::size_t i = 0; i < carriers.size(); ++i) carriers[i].join();

        std::size_t total = 0;
        for (std::size_t i = 0; i < finished.size(); ++i) total += finished[i];
        std::printf("   %d 个 vCPU 各跑 %d 根，共 %zu 根，花了 %s\n", vcpus, per_vcpu, total,
                    wavvix::format_duration(watch.elapsed()).c_str());
        std::printf("   每个 vCPU 有自己的运行队列，不加锁，协程也不会被偷到别的 vCPU\n");
    }

    std::printf("\n6. spawn 的开销：冷分配与热复用\n");
    {
        // 冷：先 spawn 一大批再跑。池是空的，每根都要真分配一次栈。
        // 默认栈在 Windows 上会整段 commit，控制冷启动样本的峰值占用。
        const int burst = 512;
        wavvix::stopwatch watch;
        for (int i = 0; i < burst; ++i) spawn([] {}).value();
        const double cold = static_cast<double>(watch.elapsed().count()) / burst;
        run();

        // 热：spawn 一根、跑完、再 spawn 一根……服务器「一个请求一根协程」就是这个形状。
        // 上一根结束时栈进了池子，下一根直接复用，一个系统调用都不用。
        const int steady = 20000;
        watch.restart();
        for (int i = 0; i < steady; ++i) {
            spawn([] {}).value();
            run();
        }
        const double warm = static_cast<double>(watch.elapsed().count()) / steady;

        std::printf("   冷（先攒 %d 根再跑）   %6.0f ns/根   池是空的，每根真分配一次栈\n",
                    burst, cold);
        std::printf("   热（spawn 一根跑一根）%6.0f ns/根   池命中，零系统调用\n", warm);
        std::printf("   服务器是第二种形状；池按每线程 %zu MB 封顶，超了才真还给系统\n",
                    static_cast<std::size_t>(sizeof(void*) >= 8 ? 32 : 8));
    }
    return 0;
}
