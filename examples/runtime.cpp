// 协程运行时：go() 起协程、wait_group 等协程、让出与睡眠、定时器。最小用法见 coroutine.cpp。
//
//     xmake run example_runtime
//     WAVVIX_MAXPROCS=2 xmake run example_runtime   # 工作线程数默认取 CPU 数，环境变量可以改

#include <culib/coroutine.h>
#include <culib/os.h>
#include <culib/stopwatch.h>
#include <culib/sync.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace {

typedef std::chrono::milliseconds ms;

}  // namespace

int main() {
    std::printf("1. 不用建运行时：第一次 go() 时起来，工作线程数取 CPU 数（这台是 %u）\n",
                wavvix::os::cpu_count());
    {
        std::mutex guard;
        std::set<std::thread::id> threads;
        wavvix::wait_group group;
        for (int i = 0; i < 8; ++i) {
            group.go([&] {
                const wavvix::stopwatch busy;   // 占着工作线程 5 ms，别的协程只能去别的线程
                while (busy.elapsed() < ms(5)) {
                }
                std::lock_guard<std::mutex> lock(guard);
                threads.insert(std::this_thread::get_id());
            });
        }
        group.wait();
        std::printf("   8 根各占 5 ms 的协程，分到了 %zu 根工作线程上\n", threads.size());
    }

    std::printf("\n2. 睡的是协程，不是线程\n");
    {
        std::atomic<bool> slept(false);
        long spins = 0;
        wavvix::wait_group group;
        group.go([&] {
            wavvix::sleep_for(ms(30));
            slept.store(true);
        });
        group.go([&] {
            while (!slept.load()) {
                ++spins;
                wavvix::yield();
            }
        });
        group.wait();
        std::printf("   一根睡了 30 ms，另一根在这段时间里转了 %ld 轮\n", spins);
    }

    std::printf("\n3. 定时器按时限唤醒\n");
    {
        // 时限隔开 40 ms：几根协程在不同的工作线程上醒，谁先送出还要看那根线程什么时候跑到，
        // 隔开一些才看得出先后。Windows 10 1803 以前的系统按约 15.6 ms 的节拍醒，隔得太近还会落在同一个节拍里。
        wavvix::channel<char> woke(3);
        wavvix::wait_group group;
        group.go([&] {
            wavvix::sleep_for(ms(100));
            woke.send('C');
        });
        group.go([&] {
            wavvix::sleep_for(ms(20));
            woke.send('A');
        });
        group.go([&] {
            wavvix::sleep_for(ms(60));
            woke.send('B');
        });
        std::string order;
        for (int i = 0; i < 3; ++i) {
            char name = 0;
            woke.receive(name);   // main 线程在这里等
            order += name;
        }
        group.wait();
        std::printf("   起的顺序是 C A B，醒来的顺序是 %s\n", order.c_str());
    }

    std::printf("\n4. 组里的协程还能往组里加，wait() 连它们一起等\n");
    {
        std::atomic<int> finished(0);
        wavvix::wait_group group;
        for (int i = 0; i < 10; ++i) {
            group.go([&] {
                for (int j = 0; j < 10; ++j) {
                    group.go([&] {
                        wavvix::sleep_for(ms(5));
                        finished.fetch_add(1);
                    });
                }
            });
        }
        group.wait();
        std::printf("   10 根协程各起 10 根，wait() 返回时跑完了 %d 根\n", finished.load());
    }

    std::printf("\n5. 不等也行：go() 起了就不管，但 main 返回时进程直接退出\n");
    {
        std::atomic<int> started(0);
        for (int i = 0; i < 100; ++i) {
            wavvix::go([&] { started.fetch_add(1); });
        }
        while (started.load() < 100) {
            wavvix::sleep_for(ms(1));
        }
        std::printf("   起了 100 根不等它们，轮询到 %d 根都跑过了；要等就用 wait_group\n",
                    started.load());
    }
    return 0;
}
