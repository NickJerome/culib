// 协程感知的同步原语：等不到的时候挂起这根协程，工作线程照常跑别的协程。
// 等待者和唤醒者可以在不同的工作线程上。
//
//     xmake run example_sync

#include <culib/coroutine.h>
#include <culib/runtime.h>
#include <culib/stopwatch.h>
#include <culib/sync.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

typedef std::chrono::milliseconds ms;

using wavvix::channel;
using wavvix::condition_variable;
using wavvix::lock_guard;
using wavvix::mutex;

void join_all(std::vector<wavvix::coroutine>& tasks) {
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        tasks[i].join();
    }
}

}  // namespace

int main() {
    wavvix::runtime runtime(4);

    std::printf("1. 不同工作线程上的协程是并行的：共享的数据要加锁\n");
    {
        mutex lock;
        long counter = 0;
        std::vector<wavvix::coroutine> tasks;
        for (int i = 0; i < 8; ++i) {
            tasks.push_back(wavvix::coroutine([&] {
                for (int n = 0; n < 10000; ++n) {
                    lock_guard<mutex> held(lock);
                    ++counter;
                }
            }));
        }
        join_all(tasks);
        std::printf("   8 根协程各加 10000 次，结果 %ld\n", counter);
    }

    std::printf("\n2. 派到同一根线程上的协程不会并行：不让出的临界区不用锁\n");
    {
        int counter = 0;
        wavvix::coroutine parent(runtime.worker(0), [&] {
            std::vector<wavvix::coroutine> tasks;
            for (int i = 0; i < 100; ++i) {
                tasks.push_back(wavvix::coroutine(wavvix::scheduler::current(), [&counter] { ++counter; }));
            }
            join_all(tasks);
        });
        parent.join();
        std::printf("   100 根协程都在第 0 根线程上，各加一次，没有锁，结果 %d\n", counter);
        std::printf("   判据：这段代码从读到写之间会不会让出、会不会有别的线程来改。都不会就不用锁。\n");
    }

    std::printf("\n3. 会让出的临界区要锁，哪怕都在同一根线程上\n");
    {
        std::string without;
        std::string with;
        mutex lock;
        wavvix::coroutine parent(runtime.worker(1), [&] {
            const wavvix::scheduler here = wavvix::scheduler::current();
            std::vector<wavvix::coroutine> tasks;
            for (int i = 0; i < 2; ++i) {
                const char name = static_cast<char>('a' + i);
                tasks.push_back(wavvix::coroutine(here, [&without, name] {
                    without += name;
                    wavvix::sleep_for(ms(5));   // 中间让出，别人插进来了
                    without += name;
                }));
                tasks.push_back(wavvix::coroutine(here, [&with, &lock, name] {
                    lock_guard<mutex> held(lock);
                    with += name;
                    wavvix::sleep_for(ms(5));   // 同样让出，但这次是排队进来的
                    with += name;
                }));
            }
            join_all(tasks);
        });
        parent.join();
        std::printf("   不加锁 %s   加锁 %s\n", without.c_str(), with.c_str());
    }

    std::printf("\n4. channel：生产者和消费者在不同的工作线程上\n");
    {
        channel<int> numbers(4);
        long sum = 0;
        wavvix::coroutine producer(runtime.worker(2), [&] {
            for (int i = 1; i <= 100; ++i) {
                numbers.send(i);   // 满了就挂起，等消费者取走
            }
            numbers.close();
        });
        wavvix::coroutine consumer(runtime.worker(3), [&] {
            int value = 0;
            while (numbers.receive(value)) {   // 关闭并且取完以后返回 false
                sum += value;
            }
        });
        producer.join();
        consumer.join();
        std::printf("   发 1..100，收到的和是 %ld\n", sum);
    }

    std::printf("\n5. 限制同时在跑的协程数：容量为 N 的 channel，send 占一个名额，receive 还一个\n");
    {
        channel<char> permits(3);
        mutex lock;
        int inside = 0;
        int peak = 0;
        const wavvix::stopwatch watch;
        std::vector<wavvix::coroutine> tasks;
        for (int i = 0; i < 12; ++i) {
            tasks.push_back(wavvix::coroutine([&] {
                permits.send(0);   // 已经有 3 根在里面就挂起
                {
                    lock_guard<mutex> held(lock);
                    ++inside;
                    if (inside > peak) {
                        peak = inside;
                    }
                }
                wavvix::sleep_for(ms(10));   // 假装在干一件慢活
                {
                    lock_guard<mutex> held(lock);
                    --inside;
                }
                char permit = 0;
                permits.receive(permit);
            }));
        }
        join_all(tasks);
        std::printf("   12 根活、每根 10 ms、名额 3 个：同时最多 %d 根，总共 %s\n", peak,
                    wavvix::format_duration(watch.elapsed()).c_str());
    }

    std::printf("\n6. 等不起就别等：try_lock_for 和 wait_for 都带时限\n");
    {
        mutex lock;
        condition_variable never;
        bool got = true;
        bool notified = true;
        bool still_held = false;
        wavvix::coroutine holder(runtime.worker(0), [&] {
            lock.lock();
            wavvix::sleep_for(ms(50));   // 占着不放
            lock.unlock();
        });
        wavvix::coroutine waiter(runtime.worker(0), [&] { got = lock.try_lock_for(ms(10)); });
        holder.join();
        waiter.join();

        wavvix::coroutine condition_waiter([&] {
            lock_guard<mutex> held(lock);
            notified = never.wait_for(lock, ms(10));
            still_held = !lock.try_lock();   // 超时返回时锁照样拿在手里
        });
        condition_waiter.join();
        std::printf("   锁被占 50 ms，只等 10 ms：拿到了吗 %s\n", got ? "是" : "否");
        std::printf("   没人通知的条件变量等 10 ms：被通知了吗 %s；返回时锁还在手里吗 %s\n",
                    notified ? "是" : "否", still_held ? "是" : "否");
    }
    return 0;
}
