#pragma once

// 线程池：工作线程有几根、哪些闲着睡了、该叫醒谁。
//
// 有几根：平时开进程能用的核数（start）。监控看到有线程卡在一根协程里 10 ms 以上时补一根（add）；卡住的恢复以后，
// 多出来的线程闲下来就退出（try_retire），但不少于平时的根数。
//
// 睡和叫醒照路线图 4.1。往队列里放了活的一方，只在有闲着的、又没人在找活时叫醒一根（wake_if_needed），被叫醒的
// 记成在找活；找到活的那根如果是最后一根在找的，再叫醒下一根（stop_searching）。要睡的一方先登记成闲着
// （enter_idle），再把所有队列看一遍，有活就撤销登记（leave_idle），没活才睡（wait_idle）。放活和要睡的两边各有
// 一道 seq_cst 屏障，至少有一边看得见另一边，叫醒不会丢。

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "worker.h"

namespace cu {
namespace detail {

struct runtime;

const unsigned most_threads = 256;   // 补线程以后的总数上限

class thread_pool {
public:
    explicit thread_pool(runtime& rt) noexcept;
    thread_pool(const thread_pool&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;

    // 有几根
    void start(unsigned count) noexcept;          // 起 count 根，起不来就终止
    bool add() noexcept;                          // 补一根：到了 256 根、内存不够或者起不来，返回 false
    void set_surplus(unsigned count) noexcept;    // 监控算出来多了几根
    bool try_retire() noexcept;                   // 闲下来的线程问一句：多了就记下这一根要退，返回 true
    void retired(unsigned self) noexcept;         // 线程退出前最后一步，之后这一格留给下一根补的线程

    // 睡和叫醒
    void wake_if_needed() noexcept;               // 往队列里放了活以后调
    void wake_one() noexcept;                     // 叫醒最后睡下的那根，记成在找活；有线程在找活时不叫
    void stop_searching() noexcept;               // 找到活了
    void give_up_searching() noexcept;            // 没找到、要去睡：只减计数，不叫人
    void enter_idle(unsigned self) noexcept;      // 登记成闲着
    bool leave_idle(unsigned self, bool was_searching) noexcept;   // 撤销登记，返回现在算不算在找活
    void wait_idle(unsigned self) noexcept;       // 睡到被叫醒；醒来以后算在找活

    // 给偷活的线程和监控看
    unsigned size() const noexcept {              // 线程表里建过几格，含已经退出的
        return created_.load(std::memory_order_acquire);
    }

    worker& at(unsigned i) noexcept {
        return entries_[i]->work;
    }

    bool alive(unsigned i) const noexcept {
        return entries_[i]->alive.load();
    }

    bool idle(unsigned i) const noexcept {
        return entries_[i]->idle.load();
    }

    unsigned base() const noexcept {
        return base_;
    }

    unsigned live() const noexcept {
        return live_.load();
    }

    int idle_count() const noexcept {
        return idle_count_.load();
    }

    int searching_count() const noexcept {
        return searching_.load();
    }

private:
    // 线程表里的一格：线程本身，加上线程池替它记的状态。建了就不释放。
    struct entry {
        entry(runtime& rt, unsigned index) noexcept
            : work(rt, index), alive(false), idle(false), woken(false), next_idle(0) {}

        worker work;
        std::atomic<bool> alive;          // 线程在跑
        std::atomic<bool> idle;           // 在闲着的名单上。在 lock_ 里改，监控不拿锁读
        bool woken;                       // 被 wake_one 叫醒了。在 lock_ 里读写
        entry* next_idle;
        std::condition_variable wakeup;
    };

    entry* free_entry() noexcept;
    void unlink_idle(entry& self) noexcept;

    runtime& rt_;
    entry* entries_[most_threads];        // 建过的格子
    std::atomic<unsigned> created_;       // entries_ 里建过几格
    unsigned base_;                       // 平时开几根
    std::atomic<unsigned> live_;          // 在跑的线程数
    std::atomic<unsigned> surplus_;       // 多出来几根，闲下来的线程看到就退出

    // 闲着的线程：lock_ 护着名单和每一格的 woken；两个计数也在锁里改，放活的一方不拿锁先看一眼
    std::mutex lock_;
    entry* idle_head_;                    // 最后睡下的在最前面
    std::atomic<int> idle_count_;
    std::atomic<int> searching_;          // 被叫醒来找活、还没找到的线程数
};

}  // namespace detail
}  // namespace cu
