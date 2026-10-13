// 协程感知的同步原语：mutex、condition_variable、channel。
//
// 这些测试按真实用法写：临界区里会让出，否则用不着协程锁。要断言次序的只开一根工作线程，
// 由一根协程把其余的派到 scheduler::current()（见 runtime_test.cpp 开头）；跨线程的用法各有一条。

#include <culib/coroutine.h>
#include <culib/runtime.h>
#include <culib/sync.h>
#include <culib/test.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using wavvix::channel;
using wavvix::condition_variable;
using wavvix::lock_guard;
using wavvix::mutex;

typedef std::chrono::milliseconds ms;

template <typename F>
void on_one_worker(F body) {
    wavvix::runtime runtime(1);
    wavvix::coroutine first(body);
    first.join();
}

void join_all(std::vector<wavvix::coroutine>& workers) {
    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }
}

}  // namespace

// ---- mutex ------------------------------------------------------------------------

TEST(mutex, serializes_critical_sections_that_yield) {
    // 锁真正要解决的那件事：临界区中间让出，别人跑起来看到了中间状态。
    mutex lock;
    std::string trace;
    on_one_worker([&] {
        std::vector<wavvix::coroutine> workers;
        for (int i = 0; i < 2; ++i) {
            const char name = static_cast<char>('a' + i);
            workers.push_back(wavvix::coroutine(wavvix::scheduler::current(), [&trace, &lock, name] {
                lock_guard<mutex> held(lock);
                trace += name;
                wavvix::sleep_for(ms(5));   // 临界区中间让出
                trace += name;
            }));
        }
        join_all(workers);
    });

    EXPECT(trace, ==, "aabb");   // 不是 "abab"
}

TEST(mutex, hands_the_lock_over_in_arrival_order) {
    // 公平性：unlock 把锁交给队首，而不是放开让大家抢，所以排队的不会被插队的饿死。
    mutex lock;
    std::string order;
    on_one_worker([&] {
        const wavvix::scheduler here = wavvix::scheduler::current();
        std::vector<wavvix::coroutine> workers;
        workers.push_back(wavvix::coroutine(here, [&] {
            lock.lock();
            wavvix::sleep_for(ms(20));   // 占着，让后面三根都排上队
            lock.unlock();
        }));
        const char* names = "ABC";
        for (int i = 0; i < 3; ++i) {
            const char name = names[i];
            workers.push_back(wavvix::coroutine(here, [&order, &lock, name] {
                lock.lock();
                order += name;
                lock.unlock();
            }));
        }
        join_all(workers);
    });

    EXPECT(order, ==, "ABC");
}

TEST(mutex, try_lock_never_suspends_and_works_outside_a_coroutine) {
    mutex lock;
    EXPECT(lock.try_lock(), ==, true);
    EXPECT(lock.try_lock(), ==, false);   // 已经被持有：拿不到，但不挂起
    lock.unlock();
    EXPECT(lock.try_lock(), ==, true);
    lock.unlock();
}

TEST(mutex, try_lock_for_gives_up_when_the_time_is_up) {
    mutex lock;
    bool got = true;
    on_one_worker([&] {
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine holder(here, [&] {
            lock.lock();
            wavvix::sleep_for(ms(40));
            lock.unlock();
        });
        wavvix::coroutine waiter(here, [&] { got = lock.try_lock_for(ms(10)); });
        holder.join();
        waiter.join();
    });

    EXPECT(got, ==, false);
}

TEST(mutex, does_not_lose_the_lock_when_a_waiter_times_out) {
    // 交棒给一个刚好超时走掉的等待者就是死锁：锁交出去了，那根协程却不会再来拿。
    // 这一条故意把「甲的时限到」和「持有者放锁」排在同一个时刻；不管甲最后是超时还是拿到，
    // 乙都必须拿得到。
    mutex lock;
    bool second_got_it = false;
    on_one_worker([&] {
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine holder(here, [&] {
            lock.lock();
            wavvix::sleep_for(ms(20));
            lock.unlock();
        });
        wavvix::coroutine first(here, [&] {
            if (lock.try_lock_for(ms(20))) {
                lock.unlock();
            }
        });
        wavvix::coroutine second(here, [&] {
            lock.lock();
            second_got_it = true;
            lock.unlock();
        });
        holder.join();
        first.join();
        second.join();
    });

    EXPECT(second_got_it, ==, true);
}

TEST(mutex, protects_state_shared_by_several_workers) {
    wavvix::runtime runtime(4);
    mutex lock;
    long total = 0;
    std::vector<wavvix::coroutine> workers;
    for (int i = 0; i < 8; ++i) {
        workers.push_back(wavvix::coroutine([&] {
            for (int n = 0; n < 2000; ++n) {
                lock_guard<mutex> held(lock);
                const long seen = total;
                if (n % 64 == 0) {
                    wavvix::yield();   // 临界区中间让出，别的协程照样进不来
                }
                total = seen + 1;
            }
        }));
    }
    join_all(workers);

    EXPECT(total, ==, 16000L);
}

TEST(mutex, timed_waiters_on_several_workers_never_lose_the_lock) {
    // 放锁的线程交棒、等待者在自己的线程上到点，两件事可能同时发生，只能有一方生效：
    // 要么等待者拿到锁，要么超时，锁不能交给一个已经走掉的等待者。
    wavvix::runtime runtime(4);
    mutex lock;
    long entered = 0;
    std::atomic<long> timeouts(0);
    std::vector<wavvix::coroutine> workers;
    for (int i = 0; i < 8; ++i) {
        workers.push_back(wavvix::coroutine([&] {
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
        }));
    }
    join_all(workers);

    EXPECT(entered + timeouts.load(), ==, 16000L);
    EXPECT(lock.try_lock(), ==, true);   // 锁没有丢在一个走掉的等待者手里
    lock.unlock();
}

// ---- condition_variable -----------------------------------------------------------

TEST(condition_variable, wakes_a_waiter_on_another_worker) {
    wavvix::runtime runtime(2);
    mutex lock;
    condition_variable ready;
    std::vector<int> queue;
    int taken = -1;

    wavvix::coroutine consumer(runtime.worker(0), [&] {
        lock_guard<mutex> held(lock);
        while (queue.empty()) {
            ready.wait(lock);
        }
        taken = queue.back();
    });
    wavvix::coroutine producer(runtime.worker(1), [&] {
        wavvix::sleep_for(ms(10));
        {
            lock_guard<mutex> held(lock);
            queue.push_back(42);
        }
        ready.notify_one();
    });
    consumer.join();
    producer.join();

    EXPECT(taken, ==, 42);
}

TEST(condition_variable, notify_all_wakes_everyone_in_arrival_order) {
    mutex lock;
    condition_variable ready;
    bool go = false;
    std::string order;
    on_one_worker([&] {
        const wavvix::scheduler here = wavvix::scheduler::current();
        std::vector<wavvix::coroutine> workers;
        const char* names = "ABC";
        for (int i = 0; i < 3; ++i) {
            const char name = names[i];
            workers.push_back(wavvix::coroutine(here, [&order, &lock, &ready, &go, name] {
                lock_guard<mutex> held(lock);
                while (!go) {
                    ready.wait(lock);
                }
                order += name;
            }));
        }
        workers.push_back(wavvix::coroutine(here, [&] {
            wavvix::sleep_for(ms(10));
            {
                lock_guard<mutex> held(lock);
                go = true;
            }
            ready.notify_all();
        }));
        join_all(workers);
    });

    EXPECT(order, ==, "ABC");
}

TEST(condition_variable, wait_for_times_out_and_comes_back_holding_the_lock) {
    // 超时返回和被通知返回一样，锁都重新拿在手里——不然调用方那句 unlock 就错了。
    mutex lock;
    condition_variable never;
    bool notified = true;
    bool lock_is_held_again = false;
    on_one_worker([&] {
        lock.lock();
        notified = never.wait_for(lock, ms(20));
        lock_is_held_again = !lock.try_lock();   // 已经持有，所以拿不到
        lock.unlock();
    });

    EXPECT(notified, ==, false);
    EXPECT(lock_is_held_again, ==, true);
}

TEST(condition_variable, can_be_notified_from_outside_a_coroutine) {
    wavvix::runtime runtime(1);
    mutex lock;
    condition_variable ready;
    bool go = false;
    bool woke = false;
    wavvix::coroutine waiter([&] {
        lock_guard<mutex> held(lock);
        while (!go) {
            ready.wait(lock);
        }
        woke = true;
    });

    // 协程外拿不了 lock()，用 try_lock 等到锁空出来。
    while (!lock.try_lock()) {
        std::this_thread::yield();
    }
    go = true;
    lock.unlock();
    ready.notify_one();
    waiter.join();

    EXPECT(woke, ==, true);
}

TEST(sync, nonpositive_timeouts_do_not_wait) {
    mutex lock;
    condition_variable never;
    std::string trace;
    bool negative = true;
    bool zero = true;
    bool waited_negative = true;
    bool waited_zero = true;
    on_one_worker([&] {
        const wavvix::scheduler here = wavvix::scheduler::current();
        lock.lock();
        wavvix::coroutine trier(here, [&] {
            trace += "t";
            negative = lock.try_lock_for(ms(-1));
            zero = lock.try_lock_for(ms(0));
            trace += "t";
        });
        wavvix::coroutine other(here, [&] { trace += "o"; });
        trier.join();
        other.join();

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

TEST(channel, passes_values_in_order_between_workers) {
    wavvix::runtime runtime(2);
    channel<int> values(16);
    std::vector<int> received;

    wavvix::coroutine producer(runtime.worker(0), [&] {
        for (int i = 0; i < 1000; ++i) {
            values.send(i);
        }
        values.close();
    });
    wavvix::coroutine consumer(runtime.worker(1), [&] {
        int value = 0;
        while (values.receive(value)) {
            received.push_back(value);
        }
    });
    producer.join();
    consumer.join();

    bool in_order = received.size() == 1000;
    for (std::size_t i = 0; in_order && i < received.size(); ++i) {
        in_order = received[i] == static_cast<int>(i);
    }
    EXPECT(received.size(), ==, std::size_t(1000));
    EXPECT(in_order, ==, true);
}

TEST(channel, send_waits_while_the_channel_is_full) {
    channel<int> values(1);
    std::string trace;
    on_one_worker([&] {
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine producer(here, [&] {
            for (int i = 0; i < 3; ++i) {
                values.send(i);
                trace += "S";
            }
        });
        wavvix::coroutine consumer(here, [&] {
            wavvix::sleep_for(ms(10));
            for (int i = 0; i < 3; ++i) {
                int value = -1;
                if (values.receive(value) && value == i) {
                    trace += "R";
                }
            }
        });
        producer.join();
        consumer.join();
    });

    EXPECT(trace, ==, "SRSRSR");   // 容量是 1：发送方最多领先一个
}

TEST(channel, receive_drains_what_is_left_after_close) {
    std::vector<int> received;
    bool after_drain = true;
    on_one_worker([&] {
        channel<int> values(4);
        values.send(1);
        values.send(2);
        values.send(3);
        values.close();
        int value = 0;
        while (values.receive(value)) {
            received.push_back(value);
        }
        after_drain = values.receive(value);
    });

    EXPECT(received.size(), ==, std::size_t(3));
    EXPECT(received.size() == 3 && received[0] == 1 && received[2] == 3, ==, true);
    EXPECT(after_drain, ==, false);
}

TEST(channel, close_wakes_every_waiting_receiver) {
    channel<int> values(1);
    int finished = 0;
    on_one_worker([&] {
        const wavvix::scheduler here = wavvix::scheduler::current();
        std::vector<wavvix::coroutine> workers;
        for (int i = 0; i < 3; ++i) {
            workers.push_back(wavvix::coroutine(here, [&] {
                int value = 0;
                if (!values.receive(value)) {
                    ++finished;
                }
            }));
        }
        workers.push_back(wavvix::coroutine(here, [&] {
            wavvix::sleep_for(ms(10));
            values.close();
        }));
        join_all(workers);
    });

    EXPECT(finished, ==, 3);
}

TEST(channel, can_be_closed_from_outside_a_coroutine) {
    wavvix::runtime runtime(1);
    channel<int> values(1);
    bool ended = false;
    wavvix::coroutine receiver([&] {
        int value = 0;
        ended = !values.receive(value);
    });

    std::this_thread::sleep_for(ms(10));
    values.close();
    receiver.join();

    EXPECT(ended, ==, true);
}

TEST(channel, carries_move_only_values) {
    int total = 0;
    on_one_worker([&] {
        channel<std::unique_ptr<int> > values(2);
        wavvix::coroutine producer(wavvix::scheduler::current(), [&] {
            for (int i = 1; i <= 3; ++i) {
                values.send(std::unique_ptr<int>(new int(i)));
            }
            values.close();
        });
        std::unique_ptr<int> value;
        while (values.receive(value)) {
            total += *value;
        }
        producer.join();
    });

    EXPECT(total, ==, 6);
}

TEST(channel, serves_many_senders_and_receivers_on_several_workers) {
    wavvix::runtime runtime(4);
    channel<int> values(8);
    std::atomic<long> sum(0);
    std::atomic<int> count(0);

    std::vector<wavvix::coroutine> senders;
    for (int s = 0; s < 4; ++s) {
        senders.push_back(wavvix::coroutine([&values, s] {
            for (int i = 0; i < 1000; ++i) {
                values.send(s * 1000 + i);
            }
        }));
    }
    std::vector<wavvix::coroutine> receivers;
    for (int r = 0; r < 4; ++r) {
        receivers.push_back(wavvix::coroutine([&] {
            int value = 0;
            while (values.receive(value)) {
                sum.fetch_add(value);
                count.fetch_add(1);
            }
        }));
    }
    join_all(senders);
    values.close();
    join_all(receivers);

    EXPECT(count.load(), ==, 4000);
    EXPECT(sum.load(), ==, 4000L * 3999 / 2);
}
