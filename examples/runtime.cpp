// 协程运行时：全局等待队列、派到指定的工作线程、让出与睡眠、detach。最小用法见 coroutine.cpp。
//
//     xmake run example_runtime

#include <culib/coroutine.h>
#include <culib/runtime.h>
#include <culib/stopwatch.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

typedef std::chrono::milliseconds ms;

void join_all(std::vector<wavvix::coroutine>& tasks) {
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        tasks[i].join();
    }
}

}  // namespace

int main() {
    std::atomic<int> detached_finished(0);
    {
        // 一个进程同时只有一个 runtime：main 建它、起协程、等协程。
        wavvix::runtime runtime(4);

        std::printf("1. 协程排进全局队列，空闲的工作线程取走\n");
        {
            std::mutex guard;
            std::set<std::thread::id> threads;
            std::vector<wavvix::coroutine> tasks;
            for (int i = 0; i < 8; ++i) {
                tasks.push_back(wavvix::coroutine([&] {
                    const wavvix::stopwatch busy;   // 占着工作线程 5 ms，别的协程只能去别的线程
                    while (busy.elapsed() < ms(5)) {
                    }
                    std::lock_guard<std::mutex> lock(guard);
                    threads.insert(std::this_thread::get_id());
                }));
            }
            join_all(tasks);
            std::printf("   8 根各占 5 ms 的协程，分到了 %zu 根工作线程上\n", threads.size());
        }

        std::printf("\n2. 睡的是协程，不是线程\n");
        {
            // 两根都派到第 0 根工作线程：一根睡 30 ms，另一根在这段时间里一直让出。
            bool slept = false;
            long spins = 0;
            wavvix::coroutine sleeper(runtime.worker(0), [&] {
                wavvix::sleep_for(ms(30));
                slept = true;
            });
            wavvix::coroutine spinner(runtime.worker(0), [&] {
                while (!slept) {
                    ++spins;
                    wavvix::yield();
                }
            });
            sleeper.join();
            spinner.join();
            std::printf("   同一根线程上，一根睡了 30 ms，另一根在这段时间里转了 %ld 轮\n", spins);
        }

        std::printf("\n3. 派到同一根线程上的协程轮流跑\n");
        {
            // scheduler::current() 是自己所在的工作线程。同一根线程上的协程不会并行、不会被抢占，
            // 让出才轮到别人。
            std::string trace;
            wavvix::coroutine parent(runtime.worker(1), [&] {
                const wavvix::scheduler here = wavvix::scheduler::current();
                wavvix::coroutine first(here, [&] {
                    for (int i = 0; i < 3; ++i) {
                        trace += "甲";
                        wavvix::yield();
                    }
                });
                wavvix::coroutine second(here, [&] {
                    for (int i = 0; i < 3; ++i) {
                        trace += "乙";
                        wavvix::yield();
                    }
                });
                first.join();
                second.join();
            });
            parent.join();
            std::printf("   两根各写三次、每次让出：%s\n", trace.c_str());
        }

        std::printf("\n4. 定时器按时限唤醒\n");
        {
            std::string order;
            wavvix::coroutine parent(runtime.worker(2), [&] {
                const wavvix::scheduler here = wavvix::scheduler::current();
                wavvix::coroutine c(here, [&] {
                    wavvix::sleep_for(ms(30));
                    order += "C";
                });
                wavvix::coroutine a(here, [&] {
                    wavvix::sleep_for(ms(10));
                    order += "A";
                });
                wavvix::coroutine b(here, [&] {
                    wavvix::sleep_for(ms(20));
                    order += "B";
                });
                c.join();
                a.join();
                b.join();
            });
            parent.join();
            std::printf("   起的顺序是 C A B，醒来的顺序是 %s\n", order.c_str());
        }

        std::printf("\n5. detach 的协程由 runtime 收尾\n");
        for (int i = 0; i < 100; ++i) {
            wavvix::coroutine([&] {
                wavvix::sleep_for(ms(5));
                detached_finished.fetch_add(1);
            }).detach();
        }
        std::printf("   起了 100 根就 detach，此刻跑完了 %d 根\n", detached_finished.load());
    }   // runtime 析构：先等所有协程结束（detach 的也算），再停掉工作线程
    std::printf("   runtime 结束以后：%d 根\n", detached_finished.load());
    return 0;
}
