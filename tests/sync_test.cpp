// 协程感知的同步原语。
//
// 这些测试都按**真实用法**写：临界区里会让出，否则根本不需要锁（见 culib/sync.h
// 开头那段）。只断言不让出的临界区，等于在测一个没人需要的性质。

#include <culib/scheduler.h>
#include <culib/sync.h>
#include <culib/test.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using wavvix::condition_variable;
using wavvix::lock_guard;
using wavvix::mutex;
using wavvix::run;
using wavvix::semaphore;
using wavvix::sleep_for;
using wavvix::spawn;

typedef std::chrono::milliseconds ms;

}  // namespace

TEST(mutex, serializes_critical_sections_that_yield) {
    // 锁真正要解决的那件事：临界区中间让出，别人跑起来看到了中间状态。
    // 不加锁时下面这段会拿到 "ab" 交错；加了锁就是两段各自完整。
    mutex lock;
    std::string trace;

    for (int i = 0; i < 2; ++i) {
        const char name = static_cast<char>('a' + i);
        spawn([&trace, &lock, name] {
            lock_guard<mutex> held(lock);
            trace += name;
            sleep_for(ms(5));      // 临界区中间让出
            trace += name;
        }).value();
    }

    EXPECT(run(), ==, 2u);
    EXPECT(trace, ==, "aabb");     // 不是 "abab"
}

TEST(mutex, hands_the_lock_over_in_arrival_order) {
    // 公平性：unlock 把锁**交给队首**而不是放开让大家抢。放开让抢的话，一个刚好在跑的
    // 协程可以插队，队首那个可能一直排不上。
    mutex lock;
    std::string order;

    spawn([&] {
        lock.lock();
        sleep_for(ms(20));         // 占着，让后面三根都排上队
        lock.unlock();
    }).value();
    const char* names = "ABC";
    for (int i = 0; i < 3; ++i) {
        const char name = names[i];
        spawn([&order, &lock, name] {
            lock.lock();
            order += name;
            lock.unlock();
        }).value();
    }

    EXPECT(run(), ==, 4u);
    EXPECT(order, ==, "ABC");
}

TEST(mutex, try_lock_never_suspends) {
    mutex lock;
    bool first = false;
    bool second = true;

    spawn([&] {
        first = lock.try_lock();       // 空着，拿得到
        second = lock.try_lock();      // 自己已经持有，拿不到，但**不挂起**
        lock.unlock();
    }).value();

    EXPECT(run(), ==, 1u);
    EXPECT(first, ==, true);
    EXPECT(second, ==, false);
}

TEST(mutex, try_lock_for_gives_up_when_the_time_is_up) {
    mutex lock;
    bool got = true;

    spawn([&] {
        lock.lock();
        sleep_for(ms(40));
        lock.unlock();
    }).value();
    spawn([&] { got = lock.try_lock_for(ms(10)); }).value();

    EXPECT(run(), ==, 2u);
    EXPECT(got, ==, false);
}

TEST(mutex, does_not_lose_the_lock_when_a_waiter_times_out) {
    // **交棒给一个刚好超时走掉的等待者就是死锁**：锁交出去了，但那根协程已经被排回
    // ready、不会再来拿。这一条故意把「甲的时限到」和「持有者放锁」排在同一个时刻。
    // 不管甲最后是超时还是拿到，乙都必须拿得到——拿不到就说明锁丢了。
    mutex lock;
    bool second_got_it = false;

    spawn([&] {
        lock.lock();
        sleep_for(ms(20));
        lock.unlock();
    }).value();
    spawn([&] {
        if (lock.try_lock_for(ms(20))) lock.unlock();
    }).value();
    spawn([&] {
        lock.lock();
        second_got_it = true;
        lock.unlock();
    }).value();

    EXPECT(run(), ==, 3u);
    EXPECT(second_got_it, ==, true);
}

TEST(semaphore, caps_how_many_run_at_once) {
    // 最常见的用处：起一堆协程，但只让 N 根同时进去。
    semaphore permits(2);
    int inside = 0;
    int peak = 0;

    for (int i = 0; i < 6; ++i) {
        spawn([&] {
            permits.acquire();
            ++inside;
            if (inside > peak) peak = inside;
            sleep_for(ms(5));      // 占着名额让出
            --inside;
            permits.release();
        }).value();
    }

    EXPECT(run(), ==, 6u);
    EXPECT(peak, ==, 2);
    EXPECT(inside, ==, 0);
}

TEST(semaphore, try_acquire_for_gives_up_when_the_time_is_up) {
    semaphore permits(0);
    bool got = true;

    spawn([&] { got = permits.try_acquire_for(ms(10)); }).value();

    EXPECT(run(), ==, 1u);
    EXPECT(got, ==, false);
    EXPECT(permits.count(), ==, 0);
}

TEST(semaphore, maximum_timeout_still_waits_for_a_release) {
    semaphore permits(0);
    bool acquired = false;
    spawn([&] {
        acquired = permits.try_acquire_for((std::chrono::nanoseconds::max)());
    }).value();
    spawn([&] {
        wavvix::yield();
        permits.release();
    }).value();

    EXPECT(run(), ==, 2u);
    EXPECT(acquired, ==, true);
}

TEST(semaphore, early_wakes_remove_timers_without_losing_later_timeouts) {
    semaphore permits(0);
    int acquired = 0;
    int expired = 0;
    for (int i = 0; i < 32; ++i) {
        const int timeout = 80 + (i * 7 % 4) * 10;
        spawn([&, timeout] {
            if (permits.try_acquire_for(ms(timeout))) ++acquired;
            else ++expired;
        }).value();
    }
    spawn([&] { permits.release(16); }).value();

    EXPECT(run(), ==, 33u);
    EXPECT(acquired, ==, 16);
    EXPECT(expired, ==, 16);
    EXPECT(permits.count(), ==, 0);
}

TEST(semaphore, a_middle_timeout_keeps_the_remaining_waiters_in_order) {
    semaphore permits(0);
    std::string order;
    bool middle_acquired = true;
    spawn([&] { if (permits.try_acquire_for(ms(1000))) order += 'A'; }).value();
    spawn([&] { middle_acquired = permits.try_acquire_for(ms(0)); }).value();
    spawn([&] { if (permits.try_acquire_for(ms(1000))) order += 'C'; }).value();
    spawn([&] {
        sleep_for(ms(10));
        permits.release(2);
    }).value();

    EXPECT(run(), ==, 4u);
    EXPECT(middle_acquired, ==, false);
    EXPECT(order, ==, "AC");
    EXPECT(permits.count(), ==, 0);
}

TEST(semaphore, release_without_waiters_puts_the_permit_back) {
    semaphore permits(0);
    permits.release();
    permits.release(2);
    EXPECT(permits.count(), ==, 3);
    EXPECT(permits.try_acquire(), ==, true);
    EXPECT(permits.count(), ==, 2);
}

TEST(semaphore, release_skips_an_expired_waiter_before_it_resumes) {
    semaphore permits(0);
    bool expired_acquired = true;
    bool next_acquired = false;
    spawn([&] {
        wavvix::yield();
        permits.release();
    }).value();
    spawn([&] { expired_acquired = permits.try_acquire_for(ms(1)); }).value();
    spawn([&] { permits.acquire(); next_acquired = true; }).value();
    // Hold the vCPU until the first wait expires. Release runs before it resumes.
    spawn([] { std::this_thread::sleep_for(ms(5)); }).value();

    EXPECT(run(), ==, 4u);
    EXPECT(expired_acquired, ==, false);
    EXPECT(next_acquired, ==, true);
    EXPECT(permits.try_acquire(), ==, false);
}

TEST(semaphore, batch_release_wakes_waiters_and_keeps_the_rest) {
    wavvix::scheduler_scope runtime;
    semaphore permits(0);
    int awakened = 0;
    for (int i = 0; i < 3; ++i)
        spawn([&] { permits.acquire(); ++awakened; }).value();
    wavvix::yield();
    permits.release(5);
    EXPECT(permits.count(), ==, 2);
    EXPECT(run(), ==, 3u);
    EXPECT(awakened, ==, 3);
}

TEST(condition_variable, wakes_a_waiter_once_the_condition_holds) {
    mutex lock;
    condition_variable ready;
    std::vector<int> queue;
    int taken = -1;

    spawn([&] {
        lock_guard<mutex> held(lock);
        while (queue.empty()) ready.wait(lock);    // 照例用 while，不是 if
        taken = queue.back();
    }).value();
    spawn([&] {
        sleep_for(ms(10));
        {
            lock_guard<mutex> held(lock);
            queue.push_back(42);
        }
        ready.notify_one();
    }).value();

    EXPECT(run(), ==, 2u);
    EXPECT(taken, ==, 42);
}

TEST(condition_variable, notify_all_wakes_everyone_in_arrival_order) {
    mutex lock;
    condition_variable ready;
    bool go = false;
    std::string order;

    const char* names = "ABC";
    for (int i = 0; i < 3; ++i) {
        const char name = names[i];
        spawn([&order, &lock, &ready, &go, name] {
            lock_guard<mutex> held(lock);
            while (!go) ready.wait(lock);
            order += name;
        }).value();
    }
    spawn([&] {
        sleep_for(ms(10));
        {
            lock_guard<mutex> held(lock);
            go = true;
        }
        ready.notify_all();
    }).value();

    EXPECT(run(), ==, 4u);
    EXPECT(order, ==, "ABC");
}

TEST(condition_variable, wait_for_times_out_and_comes_back_holding_the_lock) {
    // 超时返回和被通知返回一样，**锁都重新拿在手里**——不然调用方那句 unlock 就错了。
    mutex lock;
    condition_variable never;
    bool notified = true;
    bool lock_is_held_again = false;

    spawn([&] {
        lock.lock();
        notified = never.wait_for(lock, ms(20));
        lock_is_held_again = !lock.try_lock();    // 已经持有，所以拿不到
        lock.unlock();
    }).value();

    EXPECT(run(), ==, 1u);
    EXPECT(notified, ==, false);
    EXPECT(lock_is_held_again, ==, true);
}

TEST(sync, nonpositive_timeouts_do_not_wait) {
    mutex lock;
    semaphore permits(1);
    mutex condition_lock;
    condition_variable condition;
    bool lock_done = false;
    bool permit_done = false;
    bool condition_done = false;
    bool lock_now = false, lock_negative = true, lock_zero = true;
    bool permit_now = false, permit_negative = true, permit_zero = true;
    bool condition_negative = true, condition_zero = true;

    spawn([&] {
        lock_now = lock.try_lock_for(ms(-1));
        lock_negative = lock.try_lock_for(ms(-1));
        lock_zero = lock.try_lock_for(ms(0));
        lock_done = true;
        if (lock_now) lock.unlock();
    }).value();
    spawn([&] { sleep_for(ms(5)); if (!lock_done) lock.unlock(); }).value();
    spawn([&] {
        permit_now = permits.try_acquire_for(ms(-1));
        permit_negative = permits.try_acquire_for(ms(-1));
        permit_zero = permits.try_acquire_for(ms(0));
        permit_done = true;
    }).value();
    spawn([&] { sleep_for(ms(5)); if (!permit_done) permits.release(); }).value();
    spawn([&] {
        condition_lock.lock();
        condition_negative = condition.wait_for(condition_lock, ms(-1));
        condition_zero = condition.wait_for(condition_lock, ms(0));
        condition_done = true;
        condition_lock.unlock();
    }).value();
    spawn([&] { sleep_for(ms(5)); if (!condition_done) condition.notify_one(); }).value();

    EXPECT(run(), ==, 6u);
    EXPECT(lock_now, ==, true);
    EXPECT(lock_negative, ==, false);
    EXPECT(lock_zero, ==, false);
    EXPECT(permit_now, ==, true);
    EXPECT(permit_negative, ==, false);
    EXPECT(permit_zero, ==, false);
    EXPECT(condition_negative, ==, false);
    EXPECT(condition_zero, ==, false);
}
