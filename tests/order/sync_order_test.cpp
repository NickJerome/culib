// 只有一根工作线程时同步原语的次序：临界区不交错、交棒按先来后到、超时与交棒同时发生。
//
// 这些测试按真实用法写：临界区里会让出，否则用不着协程锁。协程都由一根协程起出来，
// 要等它挂起才开始跑，所以起步次序是确定的（见 runtime_order_test.cpp 开头）。

#include <culib/coroutine.h>
#include <culib/sync.h>
#include <culib/test.h>

#include <chrono>
#include <string>

namespace {

using wavvix::channel;
using wavvix::condition_variable;
using wavvix::lock_guard;
using wavvix::mutex;

typedef std::chrono::milliseconds ms;

template <typename F>
void in_coroutine(F body) {
    wavvix::wait_group group;
    group.go(body);
    group.wait();
}

}  // namespace

// ---- mutex ------------------------------------------------------------------------

TEST(mutex_order, serializes_critical_sections_that_yield) {
    // 锁真正要解决的那件事：临界区中间让出，别人跑起来看到了中间状态。
    mutex lock;
    std::string trace;
    in_coroutine([&] {
        wavvix::wait_group children;
        for (int i = 0; i < 2; ++i) {
            const char name = static_cast<char>('a' + i);
            children.go([&trace, &lock, name] {
                lock_guard<mutex> held(lock);
                trace += name;
                wavvix::sleep_for(ms(5));   // 临界区中间让出
                trace += name;
            });
        }
        children.wait();
    });

    EXPECT(trace, ==, "aabb");   // 不是 "abab"
}

TEST(mutex_order, hands_the_lock_over_in_arrival_order) {
    // 公平性：unlock 把锁交给队首，而不是放开让大家抢，所以排队的不会被插队的饿死。
    mutex lock;
    std::string order;
    in_coroutine([&] {
        wavvix::wait_group children;
        children.go([&] {
            lock.lock();
            wavvix::sleep_for(ms(20));   // 占着，让后面三根都排上队
            lock.unlock();
        });
        const char* names = "ABC";
        for (int i = 0; i < 3; ++i) {
            const char name = names[i];
            children.go([&order, &lock, name] {
                lock.lock();
                order += name;
                lock.unlock();
            });
        }
        children.wait();
    });

    EXPECT(order, ==, "ABC");
}

TEST(mutex_order, try_lock_for_gives_up_when_the_time_is_up) {
    mutex lock;
    bool got = true;
    in_coroutine([&] {
        wavvix::wait_group children;
        children.go([&] {
            lock.lock();
            wavvix::sleep_for(ms(40));
            lock.unlock();
        });
        children.go([&] { got = lock.try_lock_for(ms(10)); });
        children.wait();
    });

    EXPECT(got, ==, false);
}

TEST(mutex_order, does_not_lose_the_lock_when_a_waiter_times_out) {
    // 交棒给一个刚好超时走掉的等待者就是死锁：锁交出去了，那根协程却不会再来拿。
    // 这一条故意把「甲的时限到」和「持有者放锁」排在同一个时刻；不管甲最后是超时还是拿到，
    // 乙都必须拿得到。
    mutex lock;
    bool second_got_it = false;
    in_coroutine([&] {
        wavvix::wait_group children;
        children.go([&] {
            lock.lock();
            wavvix::sleep_for(ms(20));
            lock.unlock();
        });
        children.go([&] {
            if (lock.try_lock_for(ms(20))) {
                lock.unlock();
            }
        });
        children.go([&] {
            lock.lock();
            second_got_it = true;
            lock.unlock();
        });
        children.wait();
    });

    EXPECT(second_got_it, ==, true);
}

// ---- condition_variable -----------------------------------------------------------

TEST(condition_variable_order, notify_all_wakes_everyone_in_arrival_order) {
    mutex lock;
    condition_variable ready;
    bool go = false;
    std::string order;
    in_coroutine([&] {
        wavvix::wait_group children;
        const char* names = "ABC";
        for (int i = 0; i < 3; ++i) {
            const char name = names[i];
            children.go([&order, &lock, &ready, &go, name] {
                lock_guard<mutex> held(lock);
                while (!go) {
                    ready.wait(lock);
                }
                order += name;
            });
        }
        children.go([&] {
            wavvix::sleep_for(ms(10));
            {
                lock_guard<mutex> held(lock);
                go = true;
            }
            ready.notify_all();
        });
        children.wait();
    });

    EXPECT(order, ==, "ABC");
}

TEST(sync_order, nonpositive_timeouts_do_not_wait) {
    mutex lock;
    condition_variable never;
    std::string trace;
    bool negative = true;
    bool zero = true;
    bool waited_negative = true;
    bool waited_zero = true;
    in_coroutine([&] {
        lock.lock();
        wavvix::wait_group children;
        children.go([&] {
            trace += "t";
            negative = lock.try_lock_for(ms(-1));
            zero = lock.try_lock_for(ms(0));
            trace += "t";
        });
        children.go([&] { trace += "o"; });
        children.wait();

        waited_negative = never.wait_for(lock, ms(-1));
        waited_zero = never.wait_for(lock, ms(0));
        lock.unlock();
    });

    EXPECT(trace, ==, "tto");   // 挂起了的话 o 会插到中间
    EXPECT(negative, ==, false);
    EXPECT(zero, ==, false);
    EXPECT(waited_negative, ==, false);
    EXPECT(waited_zero, ==, false);
}

// ---- channel ----------------------------------------------------------------------

TEST(channel_order, send_waits_while_the_channel_is_full) {
    channel<int> values(1);
    std::string trace;
    in_coroutine([&] {
        wavvix::wait_group children;
        children.go([&] {
            for (int i = 0; i < 3; ++i) {
                values.send(i);
                trace += "S";
            }
        });
        children.go([&] {
            wavvix::sleep_for(ms(10));
            for (int i = 0; i < 3; ++i) {
                int value = -1;
                if (values.receive(value) && value == i) {
                    trace += "R";
                }
            }
        });
        children.wait();
    });

    EXPECT(trace, ==, "SRSRSR");   // 容量是 1：发送方最多领先一个
}
