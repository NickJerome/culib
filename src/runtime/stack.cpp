// 空闲表和把栈还给系统。
//
// 空闲的栈在自己顶上记一小块（free_stack），串成一条按还回来先后排的链表：拿的时候拿最后还的那根，它还在缓存里；
// 还给系统时从最早还的那根开始。栈顶那一页协程的第一帧用过，一定可读写。
//
// 空闲表比 kept_free 多时，最早还的那些闲了 idle_limit 就还给系统，由系统线程上的一个定时器来做。还一根是一次
// 系统调用，一般几微秒，偶尔一毫秒以上；所以分批还，一批最多花 1 ms、批与批之间隔 10 ms，系统线程一次不卡太久，
// 不耽误别的定时器和 I/O。
//
// 每根栈占两个内存映射（保护区和可用的部分），Linux 默认的 vm.max_map_count（65530）下同时最多约 3.2 万根协程。

#include "stack.h"

#include <cu/detail/macros.h>

#include "platform/memory.h"
#include "platform/thread_stack.h"
#include "timer.h"

#include <mutex>

#if CU_DETAIL_ASAN
extern "C" void __asan_unpoison_memory_region(void const volatile* address, std::size_t size);
#endif

namespace cu {
namespace detail {

namespace {

const std::size_t kept_free = 64;              // 空闲表里一直留着的根数
const timer_time idle_limit = 1000000000;      // 1 s
const timer_time batch_budget = 1000000;       // 1 ms
const timer_time batch_interval = 10000000;    // 10 ms

struct free_stack {
    stack_memory memory;
    timer_time returned;   // 什么时候还回来的
    free_stack* older;     // 最早还的那根上没有意义
    free_stack* newer;     // 最后还的那根上没有意义
};

void check_fired(timer_node* node) noexcept;

struct stack_pool {
    stack_pool() noexcept : oldest(0), newest(0), count(0), armed(false) {
        check.fire = &check_fired;
    }

    std::mutex lock;
    free_stack* oldest;   // count 为 0 时 oldest、newest 没有意义
    free_stack* newest;
    std::size_t count;
    timer_node check;
    bool armed;           // check 在定时器堆里，或者正在跑
};

// 不析构：进程退出时工作线程可能还在还栈。
stack_pool& pool() noexcept {
    static stack_pool* const instance = new stack_pool();
    return *instance;
}

// 下面三个调用方都拿着锁。

void push_newest(stack_pool& p, const stack_memory& stack, timer_time now) noexcept {
    free_stack* const node = reinterpret_cast<free_stack*>(stack.base + stack_total) - 1;
    node->memory = stack;
    node->returned = now;
    node->older = p.newest;
    if (p.count == 0) {
        p.oldest = node;
    } else {
        p.newest->newer = node;
    }
    p.newest = node;
    ++p.count;
}

stack_memory pop_newest(stack_pool& p) noexcept {
    free_stack* const node = p.newest;
    --p.count;
    if (p.count != 0) {
        p.newest = node->older;
    }
    return node->memory;
}

stack_memory pop_oldest(stack_pool& p) noexcept {
    free_stack* const node = p.oldest;
    --p.count;
    if (p.count != 0) {
        p.oldest = node->newer;
    }
    return node->memory;
}

// 向系统要一根：保留整段地址，提交可用的部分，保护区不提交。
stack_memory new_stack() noexcept {
    stack_memory stack = {0, 0};
    char* const base = static_cast<char*>(reserve_pages(stack_total));
    if (!base) {
        return stack;
    }

    char* const committed = commit_stack(base + stack_guard, base + stack_total);
    if (!committed) {
        release_pages(base, stack_total);
        return stack;
    }
    stack.base = base;
    stack.committed = committed;
    return stack;
}

void arm_check(stack_pool& p, timer_time when) noexcept {
    p.check.when = when;
    timer_add(p.check);
}

// 系统线程上：闲够了的一根根还掉，这一批的时间用完了就定下一批什么时候来。
void check_fired(timer_node*) noexcept {
    stack_pool& p = pool();
    const timer_time start = timer_now();
    timer_time now = start;
    for (;;) {
        char* released = 0;
        timer_time next = timer_never;
        {
            std::lock_guard<std::mutex> held(p.lock);
            if (p.count <= kept_free) {
                p.armed = false;
                return;
            }

            if (now - p.oldest->returned < idle_limit) {
                next = p.oldest->returned + idle_limit;
            } else if (now - start >= batch_budget) {
                next = now + batch_interval;
            } else {
                released = pop_oldest(p).base;
            }
        }
        if (!released) {
            arm_check(p, next);
            return;
        }

        // 放了锁再还：要进内核，可能还要等别的核刷 TLB
        release_pages(released, stack_total);
        now = timer_now();
    }
}

// 协程跑完以后、栈再给别的协程用之前：它最后那几帧没有返回，在 ASan 里还标着，清掉。
void clean_stack(const stack_memory& stack) noexcept {
#if CU_DETAIL_ASAN
    __asan_unpoison_memory_region(stack.base + stack_guard, stack_usable);
#else
    (void)stack;
#endif
}

}  // namespace

// 空闲表空了也在锁里向系统要：几根线程同时要，在内核里抢地址空间的锁，比在这里排队还慢。
stack_memory take_stack() noexcept {
    stack_pool& p = pool();
    std::lock_guard<std::mutex> held(p.lock);
    if (p.count > 0) {
        return pop_newest(p);
    }
    return new_stack();
}

void return_stack(const stack_memory& stack) noexcept {
    stack_pool& p = pool();
    const timer_time now = timer_now();
    timer_time arm_at = timer_never;
    {
        std::lock_guard<std::mutex> held(p.lock);
        push_newest(p, stack, now);
        if (!p.armed && p.count > kept_free) {
            p.armed = true;
            arm_at = p.oldest->returned + idle_limit;
        }
    }
    if (arm_at != timer_never) {
        arm_check(p, arm_at);
    }
}

std::size_t free_stack_count() noexcept {
    stack_pool& p = pool();
    std::lock_guard<std::mutex> held(p.lock);
    return p.count;
}

void note_stack_growth(stack_memory& stack) noexcept {
    char* const bound = committed_bound();
    if (bound) {
        stack.committed = bound;
    }
}

stack_memory stack_spares::take() noexcept {
    if (count_ > 0) {
        return stacks_[--count_];
    }
    return take_stack();
}

void stack_spares::give(const stack_memory& stack) noexcept {
    clean_stack(stack);
    if (count_ < most) {
        stacks_[count_++] = stack;
        return;
    }
    return_stack(stack);
}

void stack_spares::release_all() noexcept {
    while (count_ > 0) {
        return_stack(stacks_[--count_]);
    }
}

}  // namespace detail
}  // namespace cu
