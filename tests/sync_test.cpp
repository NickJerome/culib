// 协程感知的同步原语：mutex、condition_variable、channel、wait_group。
//
// 这里是跨线程的用法和普通线程（main、std::thread）上的用法。断言次序的测试在只开一根
// 工作线程的 runtime_order_test 里（tests/order/sync_order_test.cpp）。

#include <culib/coroutine.h>
#include <culib/detail/macros.h>
#include <culib/stopwatch.h>
#include <culib/sync.h>
#include <culib/test.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using wavvix::channel;
using wavvix::condition_variable;
using wavvix::mutex;

typedef std::chrono::milliseconds ms;

}  // namespace

// ---- mutex ------------------------------------------------------------------------

TEST(mutex, try_lock_never_waits) {
    mutex lock;
    EXPECT(lock.try_lock(), ==, true);
    EXPECT(lock.try_lock(), ==, false);   // 已经被持有：拿不到，但不等
    lock.unlock();
    EXPECT(lock.try_lock(), ==, true);
    lock.unlock();
}

TEST(mutex, protects_state_shared_by_several_workers) {
    mutex lock;
    long total = 0;
    wavvix::wait_group group;
    for (int i = 0; i < 8; ++i) {
        group.go([&] {
            for (int n = 0; n < 2000; ++n) {
                std::lock_guard<mutex> held(lock);
                const long seen = total;
                if (n % 64 == 0) {
                    wavvix::yield();   // 临界区中间让出，别的协程照样进不来
                }
                total = seen + 1;
            }
        });
    }
    group.wait();

    EXPECT(total, ==, 16000L);
}

TEST(mutex, timed_waiters_on_several_workers_never_lose_the_lock) {
    // 放锁的线程交棒、等待者在自己的线程上到点，两件事可能同时发生，只能有一方生效：
    // 要么等待者拿到锁，要么超时，锁不能交给一个已经走掉的等待者。
    mutex lock;
    long entered = 0;
    std::atomic<long> timeouts(0);
    wavvix::wait_group group;
    for (int i = 0; i < 8; ++i) {
        group.go([&] {
            for (int n = 0; n < 2000; ++n) {
                if (!lock.try_lock_for(std::chrono::microseconds(20))) {
                    timeouts.fetch_add(1);
                    continue;
                }
                const long seen = entered;
                wavvix::yield();   // 持锁让出，别的协程在等锁时到点
                entered = seen + 1;
                lock.unlock();
            }
        });
    }
    group.wait();

    EXPECT(entered + timeouts.load(), ==, 16000L);
    EXPECT(lock.try_lock(), ==, true);   // 锁没有丢在一个走掉的等待者手里
    lock.unlock();
}

TEST(mutex, is_shared_by_plain_threads_and_coroutines) {
    // 一边是普通线程，一边是协程：等锁的普通线程阻塞，等锁的协程挂起，谁都不丢锁。
    mutex lock;
    long total = 0;
    wavvix::wait_group group;
    for (int i = 0; i < 4; ++i) {
        group.go([&] {
            for (int n = 0; n < 1000; ++n) {
                std::lock_guard<mutex> held(lock);
                const long seen = total;
                if (n % 64 == 0) {
                    wavvix::yield();
                }
                total = seen + 1;
            }
        });
    }
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; ++i) {
        threads.push_back(std::thread([&] {
            for (int n = 0; n < 1000; ++n) {
                std::lock_guard<mutex> held(lock);
                ++total;
            }
        }));
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }
    group.wait();

    EXPECT(total, ==, 6000L);
}

TEST(mutex, a_plain_thread_waits_until_a_coroutine_unlocks) {
    mutex lock;
    std::atomic<bool> held_by_coroutine(false);
    std::atomic<bool> released(false);
    wavvix::wait_group group;
    group.go([&] {
        lock.lock();
        held_by_coroutine.store(true);
        wavvix::sleep_for(ms(20));
        released.store(true);
        lock.unlock();
    });
    while (!held_by_coroutine.load()) {
        std::this_thread::yield();
    }

    lock.lock();   // main 线程在这里阻塞，直到协程放锁
    const bool after_release = released.load();
    lock.unlock();
    group.wait();

    EXPECT(after_release, ==, true);
}

TEST(mutex, try_lock_for_on_a_plain_thread_gives_up_in_time) {
    mutex lock;
    std::atomic<bool> held(false);
    std::atomic<bool> done(false);
    wavvix::wait_group group;
    group.go([&] {
        lock.lock();
        held.store(true);
        while (!done.load()) {
            wavvix::sleep_for(ms(1));
        }
        lock.unlock();
    });
    while (!held.load()) {
        std::this_thread::yield();
    }

    const wavvix::stopwatch watch;
    const bool got = lock.try_lock_for(ms(20));
    const std::chrono::nanoseconds waited = watch.elapsed();
    done.store(true);
    group.wait();

    EXPECT(got, ==, false);
    EXPECT(waited >= ms(18), ==, true);
    EXPECT(lock.try_lock(), ==, true);   // 超时走掉的普通线程没把锁带走
    lock.unlock();
}

TEST(mutex, works_with_std_unique_lock) {
    // 满足标准库的 Lockable，所以 std::unique_lock 能延后加锁、带时限加锁、提前解锁。
    mutex lock;
    std::unique_lock<mutex> mine(lock);   // main 线程拿着
    bool got_while_held = true;
    bool got_after_release = false;
    wavvix::wait_group group;
    group.go([&] {
        std::unique_lock<mutex> theirs(lock, std::defer_lock);
        got_while_held = theirs.try_lock_for(ms(1));
    });
    group.wait();

    mine.unlock();
    group.go([&] {
        std::unique_lock<mutex> theirs(lock, std::defer_lock);
        got_after_release = theirs.try_lock_for(ms(1000));
    });
    group.wait();

    EXPECT(got_while_held, ==, false);
    EXPECT(got_after_release, ==, true);
    EXPECT(lock.try_lock(), ==, true);   // unique_lock 析构时放开了
    lock.unlock();
}

// ---- condition_variable -----------------------------------------------------------

TEST(condition_variable, wakes_a_waiter_on_another_worker) {
    mutex lock;
    condition_variable ready;
    std::vector<int> queue;
    int taken = -1;

    wavvix::wait_group group;
    group.go([&] {
        std::lock_guard<mutex> held(lock);
        while (queue.empty()) {
            ready.wait(lock);
        }
        taken = queue.back();
    });
    group.go([&] {
        wavvix::sleep_for(ms(10));
        {
            std::lock_guard<mutex> held(lock);
            queue.push_back(42);
        }
        ready.notify_one();
    });
    group.wait();

    EXPECT(taken, ==, 42);
}

TEST(condition_variable, wait_for_times_out_and_comes_back_holding_the_lock) {
    // 超时返回和被通知返回一样，锁都重新拿在手里——不然调用方那句 unlock 就错了。
    mutex lock;
    condition_variable never;
    bool notified = true;
    bool lock_is_held_again = false;
    wavvix::wait_group group;
    group.go([&] {
        lock.lock();
        notified = never.wait_for(lock, ms(20));
        lock_is_held_again = !lock.try_lock();   // 已经持有，所以拿不到
        lock.unlock();
    });
    group.wait();

    EXPECT(notified, ==, false);
    EXPECT(lock_is_held_again, ==, true);
}

TEST(condition_variable, a_coroutine_waiter_is_notified_from_a_plain_thread) {
    mutex lock;
    condition_variable ready;
    bool go = false;
    bool woke = false;
    wavvix::wait_group group;
    group.go([&] {
        std::lock_guard<mutex> held(lock);
        while (!go) {
            ready.wait(lock);
        }
        woke = true;
    });

    wavvix::sleep_for(ms(10));
    {
        std::lock_guard<mutex> held(lock);   // main 线程上直接拿锁
        go = true;
    }
    ready.notify_one();
    group.wait();

    EXPECT(woke, ==, true);
}

TEST(condition_variable, a_plain_thread_waits_until_a_coroutine_notifies) {
    mutex lock;
    condition_variable ready;
    int value = 0;
    wavvix::wait_group group;
    group.go([&] {
        wavvix::sleep_for(ms(10));
        {
            std::lock_guard<mutex> held(lock);
            value = 7;
        }
        ready.notify_all();
    });

    {
        std::lock_guard<mutex> held(lock);
        while (value == 0) {
            ready.wait(lock);   // main 线程在这里阻塞
        }
    }
    group.wait();

    EXPECT(value, ==, 7);
}

TEST(condition_variable, notify_all_wakes_coroutines_and_plain_threads_in_one_go) {
    // notify_all 把叫醒的协程攒成一批排进就绪队列，普通线程逐个叫醒，等到点放弃了的跳过。
    // 一半协程用很短的 wait_for 等，常常在通知之前或者正赶上通知时到点。
    mutex lock;
    condition_variable changed;
    int generation = 0;
    int arrived = 0;
    const int coroutines = 64;
    const int threads = 2;
    const int rounds = 50;

    auto waiter = [&](bool timed) {
        for (int round = 0; round < rounds; ++round) {
            std::lock_guard<mutex> held(lock);
            ++arrived;
            while (generation == round) {
                if (timed) {
                    changed.wait_for(lock, std::chrono::microseconds(50));
                } else {
                    changed.wait(lock);
                }
            }
        }
    };
    wavvix::wait_group group;
    for (int i = 0; i < coroutines; ++i) {
        group.go(waiter, i % 2 == 0);
    }
    std::vector<std::thread> plain;
    for (int i = 0; i < threads; ++i) {
        plain.emplace_back(waiter, false);
    }

    for (int round = 0; round < rounds; ++round) {
        for (;;) {
            {
                std::lock_guard<mutex> held(lock);
                if (arrived == (round + 1) * (coroutines + threads)) {
                    generation = round + 1;
                    changed.notify_all();
                    break;
                }
            }
            std::this_thread::yield();
        }
    }
    group.wait();
    for (std::size_t i = 0; i < plain.size(); ++i) {
        plain[i].join();
    }

    EXPECT(arrived, ==, rounds * (coroutines + threads));
}

TEST(condition_variable, wait_for_on_a_plain_thread_times_out_holding_the_lock) {
    mutex lock;
    condition_variable never;
    lock.lock();
    const wavvix::stopwatch watch;
    const bool notified = never.wait_for(lock, ms(20));
    const std::chrono::nanoseconds waited = watch.elapsed();
    const bool held_again = !lock.try_lock();
    lock.unlock();

    EXPECT(notified, ==, false);
    EXPECT(waited >= ms(18), ==, true);
    EXPECT(held_again, ==, true);
}

// ---- channel ----------------------------------------------------------------------

TEST(channel, passes_values_in_order_between_workers) {
    channel<int> values(16);
    std::vector<int> received;

    wavvix::wait_group group;
    group.go([&] {
        for (int i = 0; i < 1000; ++i) {
            values.send(i);
        }
        values.close();
    });
    group.go([&] {
        int value = 0;
        while (values.receive(value)) {
            received.push_back(value);
        }
    });
    group.wait();

    bool in_order = received.size() == 1000;
    for (std::size_t i = 0; in_order && i < received.size(); ++i) {
        in_order = received[i] == static_cast<int>(i);
    }
    EXPECT(received.size(), ==, std::size_t(1000));
    EXPECT(in_order, ==, true);
}

TEST(channel, receive_drains_what_is_left_after_close) {
    std::vector<int> received;
    channel<int> values(4);
    values.send(1);
    values.send(2);
    values.send(3);
    values.close();
    int value = 0;
    while (values.receive(value)) {
        received.push_back(value);
    }
    const bool after_drain = values.receive(value);

    EXPECT(received.size(), ==, std::size_t(3));
    EXPECT(received.size() == 3 && received[0] == 1 && received[2] == 3, ==, true);
    EXPECT(after_drain, ==, false);
}

TEST(channel, close_wakes_every_waiting_receiver) {
    channel<int> values(1);
    std::atomic<int> finished(0);
    wavvix::wait_group group;
    for (int i = 0; i < 3; ++i) {
        group.go([&] {
            int value = 0;
            if (!values.receive(value)) {
                finished.fetch_add(1);
            }
        });
    }
    wavvix::sleep_for(ms(10));
    values.close();
    group.wait();

    EXPECT(finished.load(), ==, 3);
}

TEST(channel, a_plain_thread_receives_what_coroutines_send) {
    channel<int> values(2);
    wavvix::wait_group group;
    for (int s = 0; s < 4; ++s) {
        group.go([&values, s] {
            for (int i = 0; i < 100; ++i) {
                values.send(s * 100 + i);
            }
        });
    }
    std::thread closer([&] {
        group.wait();   // 另一根普通线程等发送方全部结束再关
        values.close();
    });

    long sum = 0;
    int count = 0;
    int value = 0;
    while (values.receive(value)) {   // main 线程在这里阻塞等值
        sum += value;
        ++count;
    }
    closer.join();

    EXPECT(count, ==, 400);
    EXPECT(sum, ==, 400L * 399 / 2);
}

TEST(channel, a_plain_thread_send_waits_for_room) {
    channel<int> values(1);
    std::atomic<int> received(0);
    wavvix::wait_group group;
    group.go([&] {
        wavvix::sleep_for(ms(10));
        int value = 0;
        while (values.receive(value)) {
            received.fetch_add(1);
        }
    });

    for (int i = 0; i < 5; ++i) {
        values.send(i);   // 满了就阻塞 main 线程
    }
    values.close();
    group.wait();

    EXPECT(received.load(), ==, 5);
}

TEST(channel, carries_move_only_values) {
    int total = 0;
    channel<std::unique_ptr<int> > values(2);
    wavvix::wait_group group;
    group.go([&] {
        for (int i = 1; i <= 3; ++i) {
            values.send(std::unique_ptr<int>(new int(i)));
        }
        values.close();
    });
    std::unique_ptr<int> value;
    while (values.receive(value)) {
        total += *value;
    }
    group.wait();

    EXPECT(total, ==, 6);
}

TEST(channel, serves_many_senders_and_receivers_on_several_workers) {
    channel<int> values(8);
    std::atomic<long> sum(0);
    std::atomic<int> count(0);

    wavvix::wait_group senders;
    for (int s = 0; s < 4; ++s) {
        senders.go([&values, s] {
            for (int i = 0; i < 1000; ++i) {
                values.send(s * 1000 + i);
            }
        });
    }
    wavvix::wait_group receivers;
    for (int r = 0; r < 4; ++r) {
        receivers.go([&] {
            int value = 0;
            while (values.receive(value)) {
                sum.fetch_add(value);
                count.fetch_add(1);
            }
        });
    }
    senders.wait();
    values.close();
    receivers.wait();

    EXPECT(count.load(), ==, 4000);
    EXPECT(sum.load(), ==, 4000L * 3999 / 2);
}

// ---- wait_group -------------------------------------------------------------------

TEST(wait_group, several_waiters_all_return) {
    wavvix::wait_group group;
    std::atomic<bool> release(false);
    group.go([&] {
        while (!release.load()) {
            wavvix::sleep_for(ms(1));
        }
    });

    std::atomic<int> returned(0);
    wavvix::wait_group waiters;
    for (int i = 0; i < 3; ++i) {
        waiters.go([&] {
            group.wait();   // 协程里等
            returned.fetch_add(1);
        });
    }
    std::thread plain([&] {
        group.wait();   // 普通线程里等
        returned.fetch_add(1);
    });

    wavvix::sleep_for(ms(10));
    release.store(true);
    plain.join();
    waiters.wait();

    EXPECT(returned.load(), ==, 4);
}

#if WAVVIX_DETAIL_HAS_EXCEPTIONS
namespace {

// 拷贝时抛出异常的参数。抛不抛由运行时的值决定：编译器看得出必然抛的话，会把调用方后面的代码判成走不到。
struct refuses_copy {
    explicit refuses_copy(bool refuse) : refuse(refuse) {}
    refuses_copy(const refuses_copy& other) : refuse(other.refuse) {
        if (refuse) {
            throw 7;
        }
    }

    bool refuse;
};

}  // namespace

TEST(wait_group, a_coroutine_whose_arguments_fail_to_copy_is_not_counted) {
    // 参数拷进协程时抛出异常，这根协程就不存在，组里不能多算一根。多算了的话，组析构时还有一根
    // 「没结束」，打印诊断后终止。
    int thrown = 0;
    {
        wavvix::wait_group group;
        volatile bool refuse = true;
        const refuses_copy argument(refuse);
        try {
            group.go([](const refuses_copy&) {}, argument);
        } catch (int value) {
            thrown = value;
        }
    }

    EXPECT(thrown, ==, 7);
}
#endif
