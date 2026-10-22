// 定时器的最小堆：4 叉，数组里每格放定时器的时刻和指针。比较只读数组，不去碰散在各协程栈上的定时器；
// 每个定时器记着自己在堆里的位置。
//
// 堆分成 16 份，每根线程固定往其中一份里放，各有一把锁：工作线程之间不抢同一把锁，只和系统线程抢。
// 系统线程到点时挨个看这 16 份。

#include "timer.h"

#include "base/fatal.h"
#include "base/spin_lock.h"
#include "system_thread.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace cu {
namespace detail {

namespace {

struct heap_entry {
    timer_time when;
    timer_node* node;
};

struct timer_heap {
    spin_lock lock;
    std::vector<heap_entry> entries;
};

const unsigned heap_count = 16;

// 不析构：进程退出时系统线程可能还在用它。
timer_heap* heaps() noexcept {
    static timer_heap* const instances = new timer_heap[heap_count];
    return instances;
}

std::atomic<unsigned> next_heap(0);
thread_local unsigned this_heap = 0;   // 这根线程用的那一份加 1；还没分到时为 0

// 这根线程放定时器的那一份：第一次用时轮流分一份，之后不变。
timer_heap& heap_of_this_thread() noexcept {
    if (this_heap == 0) {
        this_heap = next_heap.fetch_add(1, std::memory_order_relaxed) % heap_count + 1;
    }
    return heaps()[this_heap - 1];
}

void place(std::vector<heap_entry>& entries, std::size_t at, const heap_entry& entry) noexcept {
    entries[at] = entry;
    entry.node->index = at + 1;
}

void sift_up(std::vector<heap_entry>& entries, std::size_t at) noexcept {
    const heap_entry entry = entries[at];
    while (at > 0) {
        const std::size_t parent = (at - 1) / 4;
        if (entries[parent].when <= entry.when) {
            break;
        }
        place(entries, at, entries[parent]);
        at = parent;
    }
    place(entries, at, entry);
}

void sift_down(std::vector<heap_entry>& entries, std::size_t at) noexcept {
    const heap_entry entry = entries[at];
    const std::size_t count = entries.size();
    for (;;) {
        const std::size_t first = at * 4 + 1;
        if (first >= count) {
            break;
        }
        std::size_t earliest = first;
        const std::size_t end = first + 4 < count ? first + 4 : count;
        for (std::size_t child = first + 1; child < end; ++child) {
            if (entries[child].when < entries[earliest].when) {
                earliest = child;
            }
        }
        if (entry.when <= entries[earliest].when) {
            break;
        }
        place(entries, at, entries[earliest]);
        at = earliest;
    }
    place(entries, at, entry);
}

// 取下堆顶。调用方拿着锁，堆不空。
timer_node* pop_locked(std::vector<heap_entry>& entries) noexcept {
    timer_node* const top = entries[0].node;
    top->index = 0;
    const heap_entry last = entries.back();
    entries.pop_back();
    if (!entries.empty()) {
        entries[0] = last;
        sift_down(entries, 0);
    }
    return top;
}

// 叫醒一份里到点的定时器，返回这一份里下一个的时刻。一次取下一批，只拿一次锁；放了锁再调 fire，fire 要去
// 叫醒协程。
timer_time run_heap(timer_heap& timers, timer_time now) noexcept {
    for (;;) {
        timer_node* due[64];
        std::size_t count = 0;
        timer_time next = timer_never;
        {
            std::lock_guard<spin_lock> held(timers.lock);
            while (count < 64 && !timers.entries.empty() && timers.entries[0].when <= now) {
                due[count++] = pop_locked(timers.entries);
            }
            if (!timers.entries.empty()) {
                next = timers.entries[0].when;
            }
        }
        for (std::size_t i = 0; i < count; ++i) {
            due[i]->fire(due[i]);
        }
        if (count < 64) {
            return next;
        }
    }
}

}  // namespace

timer_time timer_now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

timer_time timer_after(timer_time now, std::int64_t duration) noexcept {
    if (duration >= timer_never - now) {
        return timer_never;
    }
    return now + duration;
}

void timer_add(timer_node& node) noexcept {
    timer_heap& timers = heap_of_this_thread();
    const heap_entry entry = {node.when, &node};
    bool earliest = false;
    {
        std::lock_guard<spin_lock> held(timers.lock);
#if CU_DETAIL_HAS_EXCEPTIONS
        try {
            timers.entries.push_back(entry);
        } catch (...) {
            fatal("cu: out of memory for a timer");
        }
#else
        timers.entries.push_back(entry);
#endif
        sift_up(timers.entries, timers.entries.size() - 1);
        earliest = timers.entries[0].node == &node;
    }
    // 成了这一份里最早的，就可能比系统线程打算醒的时刻早：叫醒它重新算。不是最早的，这一份里有更早的，
    // 系统线程至少会在那时醒
    if (earliest) {
        system_thread_wake();
    }
}

timer_time timers_run(timer_time now) noexcept {
    timer_heap* const all = heaps();
    timer_time next = timer_never;
    for (unsigned i = 0; i < heap_count; ++i) {
        const timer_time earliest = run_heap(all[i], now);
        if (earliest < next) {
            next = earliest;
        }
    }
    return next;
}

}  // namespace detail
}  // namespace cu
