// cu::mutex 和 cu::condition_variable：协程之间、协程和普通线程之间互相等，拿着锁跨过挂起点，限时等的到点和
// 通知先后到达。

#include <cu/coroutine.h>
#include <cu/sync.h>
#include <cu/test.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {

typedef std::chrono::steady_clock clock_type;

}  // namespace

TEST(mutex, coroutines_and_threads_take_turns) {
    static const int coroutines = 16;
    static const int threads = 2;
    static const int each = 2000;
    cu::mutex lock;
    long total = 0;
    std::vector<cu::coroutine> group;
    for (int i = 0; i < coroutines; ++i) {
        group.emplace_back([&lock, &total] {
            for (int k = 0; k < each; ++k) {
                std::lock_guard<cu::mutex> held(lock);
                ++total;
                if (k % 100 == 0) {
                    cu::this_coroutine::yield();   // 拿着锁让出
                }
            }
        });
    }
    std::vector<std::thread> plain;
    for (int i = 0; i < threads; ++i) {
        plain.emplace_back([&lock, &total] {
            for (int k = 0; k < each; ++k) {
                std::lock_guard<cu::mutex> held(lock);
                ++total;
            }
        });
    }
    for (std::size_t i = 0; i < group.size(); ++i) {
        group[i].join();
    }
    for (std::size_t i = 0; i < plain.size(); ++i) {
        plain[i].join();
    }
    EXPECT(total, ==, static_cast<long>(coroutines + threads) * each);
}

TEST(mutex, is_held_across_a_suspension) {
    cu::mutex lock;
    std::atomic<bool> holding(false);
    std::atomic<bool> released(false);
    bool got_it_early = false;
    cu::coroutine holder([&] {
        std::lock_guard<cu::mutex> held(lock);
        holding.store(true);
        cu::this_coroutine::sleep_for(std::chrono::milliseconds(30));   // 醒来可能在另一根线程上放锁
        released.store(true);
    });
    cu::coroutine taker([&] {
        while (!holding.load()) {
            cu::this_coroutine::yield();
        }
        std::lock_guard<cu::mutex> held(lock);
        got_it_early = !released.load();
    });
    holder.join();
    taker.join();
    EXPECT(got_it_early, ==, false);
}

TEST(mutex, try_lock_fails_while_held) {
    cu::mutex lock;
    EXPECT(lock.try_lock(), ==, true);
    bool second = true;
    cu::coroutine other([&lock, &second] { second = lock.try_lock(); });
    other.join();
    EXPECT(second, ==, false);
    lock.unlock();
    EXPECT(lock.try_lock(), ==, true);
    lock.unlock();
}

TEST(mutex, a_waiter_gets_the_lock_from_a_coroutine_that_keeps_taking_it) {
    cu::mutex lock;
    std::atomic<bool> stop(false);
    std::atomic<bool> started(false);
    // 每次拿着锁算 5 ms，放开马上又拿，从不挂起：等的那根只有放开到再拿之间那一下能抢到，
    // 不直接交给它就几乎拿不到
    cu::coroutine hog([&] {
        started.store(true);
        const clock_type::time_point give_up = clock_type::now() + std::chrono::seconds(10);
        while (!stop.load() && clock_type::now() < give_up) {
            lock.lock();
            const clock_type::time_point busy_until = clock_type::now() + std::chrono::milliseconds(5);
            while (clock_type::now() < busy_until) {
            }
            lock.unlock();
        }
    });
    while (!started.load()) {
        std::this_thread::yield();
    }
    const clock_type::time_point start = clock_type::now();
    cu::coroutine waiter([&] {
        std::lock_guard<cu::mutex> held(lock);
        stop.store(true);
    });
    waiter.join();
    hog.join();
    EXPECT(clock_type::now() - start < std::chrono::seconds(5), ==, true);
}

TEST(condition_variable, hands_items_from_producers_to_consumers) {
    static const int producers = 4;
    static const int consumers = 4;
    static const int each = 1000;
    cu::mutex lock;
    cu::condition_variable ready;
    std::deque<int> items;
    int produced_done = 0;
    long consumed = 0;
    long sum = 0;
    std::vector<cu::coroutine> group;
    for (int p = 0; p < producers; ++p) {
        group.emplace_back([&] {
            for (int k = 1; k <= each; ++k) {
                std::lock_guard<cu::mutex> held(lock);
                items.push_back(k);
                ready.notify_one();
            }
            std::lock_guard<cu::mutex> held(lock);
            ++produced_done;
            ready.notify_all();
        });
    }
    std::thread plain_consumer([&] {
        std::unique_lock<cu::mutex> held(lock);
        for (;;) {
            ready.wait(held, [&] { return !items.empty() || produced_done == producers; });
            if (items.empty()) {
                return;
            }
            sum += items.front();
            items.pop_front();
            ++consumed;
        }
    });
    for (int c = 0; c < consumers - 1; ++c) {
        group.emplace_back([&] {
            std::unique_lock<cu::mutex> held(lock);
            for (;;) {
                ready.wait(held, [&] { return !items.empty() || produced_done == producers; });
                if (items.empty()) {
                    return;
                }
                sum += items.front();
                items.pop_front();
                ++consumed;
            }
        });
    }
    for (std::size_t i = 0; i < group.size(); ++i) {
        group[i].join();
    }
    plain_consumer.join();
    EXPECT(consumed, ==, static_cast<long>(producers) * each);
    EXPECT(sum, ==, static_cast<long>(producers) * each * (each + 1) / 2);
}

TEST(condition_variable, wait_for_times_out_and_holds_the_lock_again) {
    cu::mutex lock;
    cu::condition_variable never;
    std::cv_status in_coroutine = std::cv_status::no_timeout;
    bool held_after = false;
    clock_type::duration waited(0);
    cu::coroutine waiter([&] {
        std::unique_lock<cu::mutex> held(lock);
        const clock_type::time_point start = clock_type::now();
        in_coroutine = never.wait_for(held, std::chrono::milliseconds(20));
        waited = clock_type::now() - start;
        held_after = held.owns_lock() && !lock.try_lock();
    });
    waiter.join();
    EXPECT(in_coroutine == std::cv_status::timeout, ==, true);
    EXPECT(waited >= std::chrono::milliseconds(20), ==, true);
    EXPECT(held_after, ==, true);

    std::unique_lock<cu::mutex> held(lock);
    const clock_type::time_point start = clock_type::now();
    EXPECT(never.wait_for(held, std::chrono::milliseconds(20)) == std::cv_status::timeout, ==, true);
    EXPECT(clock_type::now() - start >= std::chrono::milliseconds(20), ==, true);
    EXPECT(never.wait_for(held, std::chrono::milliseconds(0)) == std::cv_status::timeout, ==, true);
}

TEST(condition_variable, a_notified_wait_cancels_its_timer) {
    cu::mutex lock;
    cu::condition_variable ready;
    bool flag = false;
    bool notified = false;
    bool slept_whole = false;
    cu::coroutine waiter([&] {
        std::unique_lock<cu::mutex> held(lock);
        notified = ready.wait_for(held, std::chrono::milliseconds(50), [&] { return flag; });
        held.unlock();
        // 定时器要是没拿掉，50 ms 到点时会把这次睡眠提前叫醒
        const clock_type::time_point start = clock_type::now();
        cu::this_coroutine::sleep_for(std::chrono::milliseconds(100));
        slept_whole = clock_type::now() - start >= std::chrono::milliseconds(100);
    });
    cu::coroutine notifier([&] {
        cu::this_coroutine::sleep_for(std::chrono::milliseconds(5));
        std::lock_guard<cu::mutex> held(lock);
        flag = true;
        ready.notify_one();
    });
    notifier.join();
    waiter.join();
    EXPECT(notified, ==, true);
    EXPECT(slept_whole, ==, true);
}

TEST(condition_variable, wait_until_follows_its_own_clock) {
    cu::mutex lock;
    cu::condition_variable never;
    std::unique_lock<cu::mutex> held(lock);
    const std::chrono::system_clock::time_point until = std::chrono::system_clock::now() + std::chrono::milliseconds(10);
    EXPECT(never.wait_until(held, until) == std::cv_status::timeout, ==, true);
    EXPECT(std::chrono::system_clock::now() >= until, ==, true);
    EXPECT(never.wait_until(held, until, [] { return false; }), ==, false);
}

TEST(condition_variable, timeouts_and_notifications_racing_wake_everyone_once) {
    static const int waiters = 400;
    static const int rounds = 20;
    cu::mutex lock;
    cu::condition_variable ready;
    std::atomic<int> finished(0);
    std::atomic<bool> stop(false);
    std::vector<cu::coroutine> group;
    for (int i = 0; i < waiters; ++i) {
        group.emplace_back([&, i] {
            for (int round = 0; round < rounds; ++round) {
                std::unique_lock<cu::mutex> held(lock);
                ready.wait_for(held, std::chrono::microseconds(100 + (i * 37 + round * 101) % 900));
            }
            finished.fetch_add(1);
        });
    }
    std::thread notifier([&] {
        while (!stop.load()) {
            ready.notify_all();
            std::this_thread::sleep_for(std::chrono::microseconds(300));
            ready.notify_one();
        }
    });
    for (std::size_t i = 0; i < group.size(); ++i) {
        group[i].join();
    }
    stop.store(true);
    notifier.join();
    EXPECT(finished.load(), ==, waiters);
}
