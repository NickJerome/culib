// 协程栈的池。地址按 64 格一次保留，一格是 pooled_stack_size 加上保护区；空闲的格子挂在链表上，
// 下一根协程直接接着用，不碰系统。
//
// 栈不向 mem 要：Windows 上栈要只保留、按需提交，保护区要跟着这一格走，都不是普通内存的用法。
// 和 mem 一样只建在 pages 上；和操作系统打交道的三步在 stack_posix.cpp / stack_win.cpp。
// 原因和数据见 docs/implementation-notes.md 的协程栈一节。

#include <culib/detail/macros.h>

#include "pages.h"
#include "stack.h"
#include "stack_detail.h"

#include <limits>
#include <mutex>
#include <new>

#if WAVVIX_DETAIL_ASAN
extern "C" void __asan_unpoison_memory_region(void const volatile* address, std::size_t size);
#endif

namespace wavvix { namespace mem { namespace detail {

struct stack_slot {
    char* base;
    char* committed;   // 准备好以后已经提交的下界；为空表示要重新准备
    stack_slot* next;
};

namespace {

const std::size_t region_slots = 64;    // 一次保留这么多格
const std::size_t keep_limit = 1024;    // 准备好的空闲格常留这么多，再多的由 trim_stacks 把内存还给系统

struct stack_pool {
    std::mutex mutex;
    stack_slot* ready;         // 准备好的空闲格，后进先出，最多 keep_limit 个
    std::size_t ready_count;
    stack_slot* surplus;       // 超出 keep_limit 的准备好的空闲格，等着把内存还给系统
    std::size_t surplus_count;
    stack_slot* idle;          // 还没准备过、或者内存已经还给系统的格
};

void push(stack_slot*& list, stack_slot* slot) {
    slot->next = list;
    list = slot;
}

stack_slot* pop(stack_slot*& list) {
    stack_slot* const slot = list;
    if (slot) {
        list = slot->next;
    }
    return slot;
}

std::size_t round_up(std::size_t value, std::size_t unit) {
    return (value + unit - 1) / unit * unit;
}

// 保护区至少 16 KiB，也至少一页。
std::size_t guard_size() {
    const std::size_t page = pages::page_size();
    return page > 16 * 1024 ? page : 16 * 1024;
}

std::size_t slot_size() {
    return round_up(pooled_stack_size + guard_size(), pages::page_size());
}

// 永不析构：main 返回时可能还有协程挂着。建不起来时为空，之后的分配都失败。
stack_pool* pool() {
    static stack_pool* const state = new (std::nothrow) stack_pool();
    return state;
}

// 再保留一个区域，把它的格子都挂到 idle 上。区域和格子的记录都不还，进程退出时随进程释放。
// 调用方拿着锁。
bool add_region_locked(stack_pool& p) {
    const std::size_t slot = slot_size();
    const std::size_t size = region_slots * slot;
    char* const base = static_cast<char*>(pages::reserve(size, 64 * 1024));
    if (!base) {
        return false;
    }

    stack_slot* const slots = new (std::nothrow) stack_slot[region_slots];
    if (!slots) {
        pages::release(base, size);
        return false;
    }

    pages::no_huge_pages(base, size);   // 栈只碰顶上几页，开了透明大页时一碰就是 2 MiB
    for (std::size_t i = 0; i < region_slots; ++i) {
        slots[i].base = base + i * slot;
        slots[i].committed = 0;
        push(p.idle, &slots[i]);
    }
    return true;
}

// 先取准备好的，再取多出来的（也准备好了），再取没准备的，都没有就再保留一个区域。
stack_slot* take_slot(stack_pool& p) {
    const std::lock_guard<std::mutex> held(p.mutex);
    if (p.ready) {
        --p.ready_count;
        return pop(p.ready);
    }
    if (p.surplus) {
        --p.surplus_count;
        return pop(p.surplus);
    }
    if (!p.idle && !add_region_locked(p)) {
        return 0;
    }
    return pop(p.idle);
}

// 准备好的格子挂回去。常留的已经够了就挂到多出来的那一串上，内存由 trim_stacks 还，
// 协程结束时不做系统调用。
void put_ready(stack_pool& p, stack_slot* slot) {
    const std::lock_guard<std::mutex> held(p.mutex);
    if (p.ready_count < keep_limit) {
        push(p.ready, slot);
        ++p.ready_count;
        return;
    }
    push(p.surplus, slot);
    ++p.surplus_count;
}

void put_idle(stack_pool& p, stack_slot* slot) {
    const std::lock_guard<std::mutex> held(p.mutex);
    push(p.idle, slot);
}

// 比一格大的栈单独保留，用完直接还。只有自己指定了栈大小的 fiber 会走到这里。
stack_memory allocate_alone(std::size_t usable) {
    stack_memory out = {0, 0, 0, 0, 0};
    const std::size_t page = pages::page_size();
    const std::size_t guard = guard_size();
    if (usable > (std::numeric_limits<std::size_t>::max)() - guard - page) {
        return out;
    }

    const std::size_t total = round_up(usable, page) + guard;
    char* const base = static_cast<char*>(pages::reserve(total, 64 * 1024));
    if (!base) {
        return out;
    }

    char* const committed = prepare_stack(base, total, guard);
    if (!committed) {
        pages::release(base, total);
        return out;
    }

    out.base = base;
    out.total = total;
    out.guard = guard;
    out.committed = committed;
    return out;
}

}  // namespace

stack_memory allocate_stack(std::size_t usable) noexcept {
    if (usable > pooled_stack_size) {
        return allocate_alone(usable);
    }

    stack_memory out = {0, 0, 0, 0, 0};
    stack_pool* const p = pool();
    stack_slot* const slot = p ? take_slot(*p) : 0;
    if (!slot) {
        return out;
    }

    const std::size_t total = slot_size();
    const std::size_t guard = guard_size();
    if (!slot->committed) {
        slot->committed = prepare_stack(slot->base, total, guard);   // 系统调用不拿着锁做
    }
    if (!slot->committed) {
        put_idle(*p, slot);
        return out;
    }

    out.base = slot->base;
    out.total = total;
    out.guard = guard;
    out.committed = slot->committed;
    out.slot = slot;
    return out;
}

void note_stack_growth(stack_memory& memory) noexcept {
    char* const bound = committed_bound();
    if (bound) {
        memory.committed = bound;
    }
}

void release_stack(const stack_memory& memory) noexcept {
#if WAVVIX_DETAIL_ASAN
    // 栈上留下的 ASan 红区要先清掉：下一根协程接着用这段内存，留着会误报。
    __asan_unpoison_memory_region(memory.base + memory.guard, memory.total - memory.guard);
#endif
    stack_slot* const slot = memory.slot;
    if (!slot) {
        pages::release(memory.base, memory.total);
        return;
    }

    stack_pool& p = *pool();
    slot->committed = memory.committed;
    if (slot->committed) {
        put_ready(p, slot);
        return;
    }

    clear_stack(slot->base, memory.total, memory.guard);
    put_idle(p, slot);
}

void trim_stacks(std::size_t at_most) noexcept {
    stack_pool* const p = pool();
    if (!p) {
        return;
    }

    stack_slot* taken = 0;
    {
        const std::lock_guard<std::mutex> held(p->mutex);
        for (std::size_t i = 0; i < at_most && p->surplus; ++i) {
            push(taken, pop(p->surplus));
            --p->surplus_count;
        }
    }

    const std::size_t total = slot_size();
    const std::size_t guard = guard_size();
    while (stack_slot* const slot = pop(taken)) {
        clear_stack(slot->base, total, guard);   // 系统调用不拿着锁做
        slot->committed = 0;
        put_idle(*p, slot);
    }
}

std::size_t surplus_stacks() noexcept {
    stack_pool* const p = pool();
    if (!p) {
        return 0;
    }
    const std::lock_guard<std::mutex> held(p->mutex);
    return p->surplus_count;
}

} } } // namespace wavvix::mem::detail
