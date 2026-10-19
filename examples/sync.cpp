// 协程感知的同步原语：在协程里等不到时只挂起这根协程，工作线程照常跑别的协程；
// 在普通线程（包括 main）里就阻塞那根线程。等待者和唤醒者可以在不同的线程上。
//
//     xmake run example_sync

#include <culib/coroutine.h>
#include <culib/stopwatch.h>
#include <culib/sync.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

namespace {

typedef std::chrono::milliseconds ms;

using wavvix::channel;
using wavvix::condition_variable;
using wavvix::mutex;

}  // namespace

int main() {
    std::printf("1. 协程在多根工作线程上并行：共享的数据要加锁\n");
    {
        mutex lock;
        long counter = 0;
        wavvix::wait_group group;
        for (int i = 0; i < 8; ++i) {
            group.go([&] {
                for (int n = 0; n < 10000; ++n) {
                    std::lock_guard<mutex> held(lock);
                    ++counter;
                }
            });
        }
        group.wait();
        std::printf("   8 根协程各加 10000 次，结果 %ld\n", counter);
    }

    std::printf("\n2. 临界区中间会挂起也照样排队：锁住的是那一段，不是那根线程\n");
    {
        std::string trace;
        mutex lock;
        wavvix::wait_group group;
        for (int i = 0; i < 2; ++i) {
            const char name = static_cast<char>('a' + i);
            group.go([&trace, &lock, name] {
                std::lock_guard<mutex> held(lock);
                trace += name;
                wavvix::sleep_for(ms(5));   // 中间挂起，另一根进不来
                trace += name;
            });
        }
        group.wait();
        std::printf("   两根协程各写两次、中间睡 5 ms：%s\n", trace.c_str());
    }

    std::printf("\n3. channel：生产者和消费者是两根协程\n");
    {
        channel<int> numbers(4);
        long sum = 0;
        wavvix::wait_group group;
        group.go([&] {
            for (int i = 1; i <= 100; ++i) {
                numbers.send(i);   // 满了就挂起，等消费者取走
            }
            numbers.close();
        });
        group.go([&] {
            int value = 0;
            while (numbers.receive(value)) {   // 关闭并且取完以后返回 false
                sum += value;
            }
        });
        group.wait();
        std::printf("   发 1..100，收到的和是 %ld\n", sum);
    }

    std::printf("\n4. main 线程直接收协程的结果：在普通线程里等就是阻塞这根线程\n");
    {
        channel<int> squares(2);
        wavvix::wait_group workers;
        for (int i = 1; i <= 5; ++i) {
            workers.go([&squares, i] { squares.send(i * i); });
        }
        long sum = 0;
        for (int i = 0; i < 5; ++i) {
            int value = 0;
            squares.receive(value);   // main 线程在这里等
            sum += value;
        }
        workers.wait();
        std::printf("   五根协程各算一个平方，main 收到的和是 %ld\n", sum);
    }

    std::printf("\n5. 限制同时在跑的协程数：容量为 N 的 channel，send 占一个名额，receive 还一个\n");
    {
        channel<char> permits(3);
        mutex lock;
        int inside = 0;
        int peak = 0;
        const wavvix::stopwatch watch;
        wavvix::wait_group group;
        for (int i = 0; i < 12; ++i) {
            group.go([&] {
                permits.send(0);   // 已经有 3 根在里面就挂起
                {
                    std::lock_guard<mutex> held(lock);
                    ++inside;
                    if (inside > peak) {
                        peak = inside;
                    }
                }
                wavvix::sleep_for(ms(10));   // 假装在干一件慢活
                {
                    std::lock_guard<mutex> held(lock);
                    --inside;
                }
                char permit = 0;
                permits.receive(permit);
            });
        }
        group.wait();
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
        wavvix::wait_group group;
        group.go([&] {
            lock.lock();
            wavvix::sleep_for(ms(50));   // 占着不放
            lock.unlock();
        });
        wavvix::sleep_for(ms(5));        // 让它先拿到锁；main 线程上睡的就是 main 线程
        got = lock.try_lock_for(ms(10));
        group.wait();

        lock.lock();
        notified = never.wait_for(lock, ms(10));
        still_held = !lock.try_lock();   // 超时返回时锁照样拿在手里
        lock.unlock();
        std::printf("   锁被占 50 ms，只等 10 ms：拿到了吗 %s\n", got ? "是" : "否");
        std::printf("   没人通知的条件变量等 10 ms：被通知了吗 %s；返回时锁还在手里吗 %s\n",
                    notified ? "是" : "否", still_held ? "是" : "否");
    }
    return 0;
}
