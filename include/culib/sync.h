#pragma once

// 协程感知的同步原语：等不到的时候**挂起这根协程，而不是卡住整个 vCPU**。
//
//     wavvix::mutex lock;
//     wavvix::spawn([&] {
//         wavvix::lock_guard<wavvix::mutex> held(lock);
//         update(shared);                    // 这中间可以放心让出
//     });
//
// 名字和 std:: 对齐（`lock` / `try_lock` / `try_lock_for` / `unlock`、
// `acquire` / `release`、`wait` / `notify_one` / `notify_all`），所以看名字就会用。
//
// ---- 先问一句：你真的需要锁吗 ----------------------------------------------------
//
// **同一个 vCPU 上的协程是协作式调度的，不会被抢占。** 所以一段**不让出**的临界区
// 本来就是原子的——中间没有任何别的协程能插进来，加锁只是多几条指令。
//
//     ++counter;                             // 不需要锁
//     map[key] = value;                      // 不需要锁
//
// 需要锁的是**临界区中间会让出**的情形，那时别的协程真的会跑起来并看到中间状态：
//
//     auto text = read_file(path);           // 这里会让出
//     cache[path] = text;                    // 回来时 cache 可能已经被别人动过
//
// 判据就一句：**这段代码从读到写之间，有没有可能让出。** 有就要锁，没有就别加。
//
// ---- 跨线程不要用这些 -----------------------------------------------------------
//
// 这里的每个类型都**只在自己那个 vCPU 内部有效**：它们一个原子操作都没有，因为
// 协程不跨 vCPU 迁移，同一个 vCPU 上不存在并发。要在**线程**之间同步，用
// `std::mutex` 那一套——但注意在协程里调 `std::mutex::lock()` 阻塞住的是整根 vCPU，
// 连带卡死它上面所有协程。
//
// ---- 这里没有 rwlock ------------------------------------------------------------
//
// 读写锁的实现成本和 API 设计自由度都比上面三个高（写者优先？能不能升级？），
// 而现在**没有调用方**。按开发约定「先有消费者，再有模块」先不做。
// **什么条件下重开**：出现真实的「读远多于写、而且读临界区里会让出」的场合。

#include <culib/coroutine.h>

#include <chrono>
#include <cstddef>

namespace wavvix {

namespace detail {
struct park_token;
}

// 协程感知的互斥量。拿不到就挂起，**不占着 vCPU**。
//
// 公平：等待者按先来后到排队，`unlock()` 交给队首那个。所以不会饿死。
class mutex {
public:
    mutex() : held_(false), head_(0), tail_(0) {}
    ~mutex();

    // 拿锁。被别人持有就挂起，直到轮到自己。只能在协程里调。
    void lock();

    // 拿不到就立刻回 false，**不挂起**。可以在协程外调。
    bool try_lock();

    // 最多等这么久。拿到回 true，到点还没拿到回 false。只能在协程里调。
    bool try_lock_for(std::chrono::nanoseconds timeout);

    void unlock();

private:
    mutex(const mutex&);
    mutex& operator=(const mutex&);

    bool held_;
    detail::park_token* head_;    // 等待队列，先来先服务
    detail::park_token* tail_;
};

// 计数信号量。常见用处是**给并发量封顶**：起 1000 根协程但只让 16 根同时去连数据库。
class semaphore {
public:
    explicit semaphore(std::ptrdiff_t initial)
        : count_(initial), head_(0), tail_(0) {}
    ~semaphore();

    void acquire();                                        // 没有就挂起
    bool try_acquire();                                    // 没有就立刻回 false
    bool try_acquire_for(std::chrono::nanoseconds timeout);
    void release(std::ptrdiff_t amount = 1);

    std::ptrdiff_t count() const { return count_; }

private:
    semaphore(const semaphore&);
    semaphore& operator=(const semaphore&);

    std::ptrdiff_t count_;
    detail::park_token* head_;
    detail::park_token* tail_;
};

// 条件变量。和 std:: 一样：**必须持有那把锁才能 wait**，wait 期间锁是放开的。
//
//     wavvix::lock_guard<wavvix::mutex> held(lock);
//     while (queue.empty()) ready.wait(lock);      // 照例用 while，不是 if
//     take(queue);
class condition_variable {
public:
    condition_variable() : head_(0), tail_(0) {}
    ~condition_variable();

    // 放开 held、挂起，被通知后重新拿回 held 才返回。只能在协程里调。
    void wait(mutex& held);

    // 同上，但最多等这么久。**返回 false 表示到点了**（不管哪种情况，返回时锁都
    // 重新拿在手里）。
    bool wait_for(mutex& held, std::chrono::nanoseconds timeout);

    void notify_one();
    void notify_all();

private:
    condition_variable(const condition_variable&);
    condition_variable& operator=(const condition_variable&);

    detail::park_token* head_;
    detail::park_token* tail_;
};

// 作用域锁，和 std::lock_guard 一样。
//
// 用 `defer { lock.unlock(); };` 也行（那是本库自己的办法），但这个更难写错：
// 加锁和解锁是同一条语句的两端，不存在「顺序写反」。
template <typename Mutex>
class lock_guard {
public:
    explicit lock_guard(Mutex& target) : target_(target) { target_.lock(); }
    ~lock_guard() { target_.unlock(); }

private:
    lock_guard(const lock_guard&);
    lock_guard& operator=(const lock_guard&);

    Mutex& target_;
};

}  // namespace wavvix
