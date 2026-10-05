// 协程感知的同步原语：等不到的时候挂起这根协程，而不是卡住整个 vCPU。
//
//     xmake run example_sync

#include <culib/scheduler.h>
#include <culib/stopwatch.h>
#include <culib/sync.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

typedef std::chrono::milliseconds ms;

using wavvix::condition_variable;
using wavvix::lock_guard;
using wavvix::mutex;
using wavvix::run;
using wavvix::semaphore;
using wavvix::sleep_for;
using wavvix::spawn;

}  // namespace

int main() {
    std::printf("1. 先问一句：很多时候你并不需要锁\n");
    {
        // 同一个 vCPU 上的协程是协作式调度的，**不会被抢占**。所以一段不让出的
        // 临界区本来就是原子的——中间没有任何别的协程能插进来。
        int counter = 0;
        for (int i = 0; i < 100; ++i) {
            spawn([&counter] { ++counter; });     // 没有锁，而且不需要
        }
        run();
        std::printf("   100 根协程各加一次，没有锁，结果是 %d\n", counter);
        std::printf("   判据：**这段代码从读到写之间会不会让出**。不会就别加锁。\n");
    }

    std::printf("\n2. 会让出的临界区才需要锁\n");
    {
        std::string without;
        for (int i = 0; i < 2; ++i) {
            const char name = static_cast<char>('a' + i);
            spawn([&without, name] {
                without += name;
                sleep_for(ms(5));                 // 中间让出，别人插进来了
                without += name;
            });
        }
        run();

        mutex lock;
        std::string with;
        for (int i = 0; i < 2; ++i) {
            const char name = static_cast<char>('a' + i);
            spawn([&with, &lock, name] {
                lock_guard<mutex> held(lock);
                with += name;
                sleep_for(ms(5));                 // 同样让出，但这次是排队进来的
                with += name;
            });
        }
        run();

        std::printf("   不加锁 %s   加锁 %s\n", without.c_str(), with.c_str());
    }

    std::printf("\n3. 信号量：起一堆协程，但只让 N 根同时进去\n");
    {
        semaphore permits(3);
        int inside = 0;
        int peak = 0;
        const wavvix::stopwatch watch;

        for (int i = 0; i < 12; ++i) {
            spawn([&permits, &inside, &peak] {
                permits.acquire();
                ++inside;
                if (inside > peak) peak = inside;
                sleep_for(ms(10));                // 假装在干一件慢活
                --inside;
                permits.release();
            });
        }
        run();
        const double elapsed = static_cast<double>(watch.elapsed().count()) / 1e6;
        std::printf("   12 根活、每根 10 ms、名额 3 个：同时最多 %d 根，总共 %.1f ms\n",
                    peak, elapsed);
        std::printf("   （12 ÷ 3 = 4 批，所以是四十毫秒上下，不是一百二）\n");
    }

    std::printf("\n4. 条件变量：生产者 / 消费者\n");
    {
        mutex lock;
        condition_variable ready;
        std::vector<int> queue;
        int sum = 0;
        bool done = false;

        spawn([&] {                               // 消费者
            for (;;) {
                lock_guard<mutex> held(lock);
                while (queue.empty() && !done) ready.wait(lock);
                if (queue.empty() && done) return;
                sum += queue.back();
                queue.pop_back();
            }
        });
        spawn([&] {                               // 生产者
            for (int i = 1; i <= 5; ++i) {
                sleep_for(ms(2));
                {
                    lock_guard<mutex> held(lock);
                    queue.push_back(i);
                }
                ready.notify_one();
            }
            {
                lock_guard<mutex> held(lock);
                done = true;
            }
            ready.notify_all();
        });
        run();
        std::printf("   生产 1..5，消费者累加得到 %d\n", sum);
    }

    std::printf("\n5. 等不起就别等：每个会等的接口都有带时限的版本\n");
    {
        mutex lock;
        bool got = true;
        const wavvix::stopwatch watch;

        spawn([&] {
            lock.lock();
            sleep_for(ms(50));                    // 占着不放
            lock.unlock();
        });
        spawn([&] { got = lock.try_lock_for(ms(10)); });
        run();

        const double elapsed = static_cast<double>(watch.elapsed().count()) / 1e6;
        std::printf("   锁被占 50 ms，只等 10 ms：拿到了吗 %s（整段 %.1f ms）\n",
                    got ? "是" : "否", elapsed);
        std::printf("   semaphore::try_acquire_for 和 condition_variable::wait_for 同理，\n");
        std::printf("   **超时返回时条件变量照样把锁还给你**，所以后面那句 unlock 不会错。\n");
    }

    std::printf("\n6. 这些只在一个 vCPU 内部有效\n");
    std::printf("   它们一个原子操作都没有——协程不跨 vCPU 迁移，同一个 vCPU 上不存在并发。\n");
    std::printf("   要在**线程**之间同步就用 std::mutex；但注意在协程里调它阻塞住的是\n");
    std::printf("   整根 vCPU，连带卡死它上面所有协程。\n");
    return 0;
}
