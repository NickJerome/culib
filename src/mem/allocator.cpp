// 分配器。用法见 include/cu/mem.h。
//
// 8 KiB 以内的块放在 64 KiB 的页里，一页一种大小，页头在页的开头：块的地址掩掉低 16 位就是页头。更大的块占若干
// 连续的页（见 region.h），头也在第一页开头；一段地址装不下的单独向系统要。
//
// 每根线程一个堆，每种大小一串有空位的页（available）和一串分完的页（full），分配只看第一页的空闲链。每页三条链：
// free 给主人分配，local_free 是主人自己还的，thread_free 是别的线程还的（原子：压上去用一次 CAS，主人取空时一次
// 摘走整串）。别的线程还块只碰那一页的 thread_free，压上去以后不再碰那一页：主人随时可能把它收空、还掉。
//
// 分完的页挪进 full，每次慢路径顺带看最多 4 页有没有别的线程还回来的块。页空了就交回页的那一层，但每种大小留着
// 最后那一页，免得分配和释放交替时反复换页。线程退出时，空页交回，还有块的页放进按大小分的共享池，别的线程缺页时
// 接手。
//
// 释放时查重复释放：空着的块第二个字是一个进程里随机的值，见 block。大块还掉时把页头的 kind 清成 0。
//
// 交回来的页先留着内存。第一次有页空出来时定一个 1 秒后的时刻，到点以后谁先看到谁把所有空页的内存一起还给
// 系统，地址留着：小块的慢路径每 64 次看一眼，页空了交回时看一眼，要大块每 64 次看一眼；看一眼要读一次时钟，
// 所以还大块时不看。系统线程也看（mem_purge），它一次最多还 1 ms，免得耽误定时器和 I/O。

#include <cu/mem.h>

#include <cu/detail/spin_lock.h>

#include "allocator.h"
#include "base/bits.h"
#include "base/fatal.h"
#include "platform/memory.h"
#include "platform/thread_exit.h"
#include "region.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

namespace cu {
namespace detail {

namespace {

const std::size_t small_max = 8192;
const unsigned class_count = 32;

// 64 字节以内每 16 字节一档，之后每翻一倍分四档。块大小都是 16 的倍数。
const std::uint32_t class_sizes[class_count] = {
    16,   32,   48,   64,   80,   96,   112,  128,  160,  192,  224,  256,  320,  384,  448,  512,
    640,  768,  896,  1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096, 5120, 6144, 7168, 8192,
};

const std::size_t header_size = 128;                         // 页头；页里的块从这里开始
const std::size_t small_align_max = header_size;             // 页里的块最多按这么多对齐
const std::size_t large_align_max = mem_page_size / 2;       // 块离页头要不到 64 KiB，掩掉低 16 位才找得回页头
const std::int64_t purge_delay = 1000000000;                 // 1 s
const std::int64_t purge_budget = 1000000;                   // 1 ms
const std::int64_t purge_retry = 10000000;                   // 10 ms
const unsigned kept_page_tick = 1024;                        // 每这么多次慢路径，交回各档留着的那一页
const unsigned purge_tick = 64;                              // 每这么多次慢路径或者要大块，看一眼该不该还内存

struct heap;

// 空着的块。释放时第二个字写成 free_key、分配时清零：再释放时它还是 free_key，就是重复释放（glibc 的 tcache
// 也这么查）。块至少 16 字节，第二个字总在块里。
struct block {
    block* next;
    std::uintptr_t freed;
};

// 第一次建堆时定下，之后只读。只要和使用方的数据碰巧一样的机会小到可以不管，不需要密码学强度。
std::atomic<std::uintptr_t> free_key(0);

void choose_free_key() noexcept {
    const int local = 0;
    std::uintptr_t x = reinterpret_cast<std::uintptr_t>(&local) ^
                       static_cast<std::uintptr_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;

    std::uintptr_t unset = 0;
    free_key.compare_exchange_strong(unset, x | 1, std::memory_order_relaxed);
}

inline void mark_freed(block* b) noexcept {
    const std::uintptr_t key = free_key.load(std::memory_order_relaxed);
    if (b->freed == key) {
        fatal("cu::mem: double free of %p", static_cast<void*>(b));
    }
    b->freed = key;
}

// kind 为 0：大块已经还掉了。
enum : std::uint8_t { small_page = 1, large_run, huge_block };

// 页头。小块的页、大块的第一页、单独要的大块开头都是它。
struct page {
    page(std::uint8_t page_kind, region* page_home, std::size_t page_span) noexcept
        : free(0), local_free(0), thread_free(0), owner(0), next(0), prev(0), home(page_home), span(page_span),
          block_size(0), used(0), capacity(0), reserved(0), size_class(0), kind(page_kind), in_full(false) {}

    block* free;                       // 主人从这里分配
    block* local_free;                 // 主人还到这里
    std::atomic<block*> thread_free;   // 别的线程还到这里
    std::atomic<heap*> owner;          // 空：大块，或者主人已经退出
    page* next;
    page* prev;
    region* home;                      // 单独要的大块为空
    std::size_t span;                  // 大块占几页；单独要的大块是多少字节
    std::uint32_t block_size;
    std::uint16_t used;                // 分出去还没收回的块，含 thread_free 里的
    std::uint16_t capacity;            // 一页能放几块
    std::uint16_t reserved;            // 已经切出来的块；后面的内存从没写过
    std::uint8_t size_class;
    std::uint8_t kind;
    bool in_full;
};
static_assert(sizeof(page) <= header_size, "the page header fits before the first block");

struct page_queue {
    page* first;
    page* last;
};

struct heap {
    page_queue available[class_count];   // 第一页给快路径分配
    page_queue full[class_count];        // 分完的页，等别的线程还块
    unsigned slow_calls;
    heap* next_free;
};

// 都是常量初始化或者在静态区清零，进程收尾时一直能用。
spin_lock global_lock;                       // 只在慢路径上拿
std::atomic<page*> abandoned[class_count];   // 主人退出时还有块的页；锁外只看空不空
heap* free_heaps = 0;                        // 退出的线程留下的堆，给新线程
std::atomic<std::int64_t> purge_at(0);       // 0：没有等着还内存的页
heap empty_heap;                             // 全空：还没有堆的线程一律走慢路径
thread_local heap* this_heap = &empty_heap;
thread_local unsigned large_calls = 0;

std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline unsigned class_of(std::size_t size) noexcept {
    if (size <= 64) {
        return size == 0 ? 0 : static_cast<unsigned>((size - 1) >> 4);
    }

    const std::size_t n = size - 1;
    const unsigned top = highest_bit(n);
    return 4 + ((top - 6) << 2) + static_cast<unsigned>((n >> (top - 2)) & 3);
}

inline page* page_of(const void* address) noexcept {
    return reinterpret_cast<page*>(reinterpret_cast<std::uintptr_t>(address) & ~(mem_page_size - 1));
}

inline char* blocks_of(page* p) noexcept {
    return reinterpret_cast<char*>(p) + header_size;
}

// ---- 还内存 ----------------------------------------------------------------------

// 有页交回来了：还没定时刻就定在 1 秒后。调用方拿着锁。
void note_free_pages_locked() noexcept {
    if (purge_at.load(std::memory_order_relaxed) == 0) {
        purge_at.store(now_ns() + purge_delay, std::memory_order_relaxed);
    }
}

// 页 p 上别的线程还回来的块接到 local_free 前面；free 用完时接上 local_free。
void gather(page* p) noexcept {
    block* const remote = p->thread_free.exchange(0, std::memory_order_acquire);
    if (remote) {
        block* tail = remote;
        unsigned count = 1;
        while (tail->next) {
            tail = tail->next;
            ++count;
        }
        tail->next = p->local_free;
        p->local_free = remote;
        p->used = static_cast<std::uint16_t>(p->used - count);
    }

    if (!p->free) {
        p->free = p->local_free;
        p->local_free = 0;
    }
}

// 共享池里块都还回来了的页交回去。调用方拿着锁。
void collect_abandoned_locked() noexcept {
    for (unsigned c = 0; c < class_count; ++c) {
        page* kept = 0;
        page* p = abandoned[c].load(std::memory_order_relaxed);
        while (p) {
            page* const next = p->next;
            gather(p);
            if (p->used == 0) {
                give_pages_locked(p->home, reinterpret_cast<char*>(p), 1);
                note_free_pages_locked();
            } else {
                p->next = kept;
                kept = p;
            }
            p = next;
        }
        abandoned[c].store(kept, std::memory_order_relaxed);
    }
}

// 到点了就还：先收共享池里的空页，再把空页的内存一起还给系统，到 until 还没还完就停下，过 10 ms 再来。
// 调用方拿着锁。
void purge_locked(std::int64_t now, std::int64_t until) noexcept {
    const std::int64_t at = purge_at.load(std::memory_order_relaxed);
    if (at == 0 || now < at) {
        return;
    }

    collect_abandoned_locked();
    const bool done = purge_pages_locked(until);
    purge_at.store(done ? 0 : now + purge_retry, std::memory_order_relaxed);
}

// 到点了就一次还完。没有等着还的不读时钟。
void purge_if_due() noexcept {
    const std::int64_t at = purge_at.load(std::memory_order_relaxed);
    if (at == 0) {
        return;
    }
    const std::int64_t now = now_ns();
    if (now < at) {
        return;
    }

    std::lock_guard<spin_lock> held(global_lock);
    purge_locked(now, std::numeric_limits<std::int64_t>::max());
}

// ---- 页 ------------------------------------------------------------------------

void push_front(page_queue& q, page* p) noexcept {
    p->prev = 0;
    p->next = q.first;
    if (q.first) {
        q.first->prev = p;
    } else {
        q.last = p;
    }
    q.first = p;
}

void push_back(page_queue& q, page* p) noexcept {
    p->next = 0;
    p->prev = q.last;
    if (q.last) {
        q.last->next = p;
    } else {
        q.first = p;
    }
    q.last = p;
}

void detach(page_queue& q, page* p) noexcept {
    if (p->prev) {
        p->prev->next = p->next;
    } else {
        q.first = p->next;
    }
    if (p->next) {
        p->next->prev = p->prev;
    } else {
        q.last = p->prev;
    }
}

// 每次最多切出 4 KiB 的新块；没切到的部分不写，也就不占物理内存。
void extend(page* p) noexcept {
    const std::size_t size = p->block_size;
    std::size_t count = size >= 4096 ? 1 : 4096 / size;
    const std::size_t left = static_cast<std::size_t>(p->capacity - p->reserved);
    if (count > left) {
        count = left;
    }

    char* const first = blocks_of(p) + static_cast<std::size_t>(p->reserved) * size;
    for (std::size_t i = 0; i + 1 < count; ++i) {
        reinterpret_cast<block*>(first + i * size)->next = reinterpret_cast<block*>(first + (i + 1) * size);
    }
    reinterpret_cast<block*>(first + (count - 1) * size)->next = p->free;
    p->free = reinterpret_cast<block*>(first);
    p->reserved = static_cast<std::uint16_t>(p->reserved + count);
}

inline void* pop(page* p) noexcept {
    block* const b = p->free;
    p->free = b->next;
    b->freed = 0;
    ++p->used;
    return b;
}

// 块都还回来了的页交回页的那一层。
void retire(page* p) noexcept {
    p->owner.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<spin_lock> held(global_lock);
        give_pages_locked(p->home, reinterpret_cast<char*>(p), 1);
        note_free_pages_locked();
    }
    purge_if_due();
}

page* take_fresh(heap* h, unsigned c) noexcept {
    page_run run = {0, 0};
    {
        std::lock_guard<spin_lock> held(global_lock);
        run = take_pages_locked(1);
    }
    if (!run.first) {
        return 0;
    }

    page* const p = new (run.first) page(small_page, run.home, 1);
    p->owner.store(h, std::memory_order_relaxed);
    p->block_size = class_sizes[c];
    p->capacity = static_cast<std::uint16_t>((mem_page_size - header_size) / class_sizes[c]);
    p->size_class = static_cast<std::uint8_t>(c);
    extend(p);
    push_front(h->available[c], p);
    return p;
}

// ---- 线程的堆 ------------------------------------------------------------------

void abandon_queue(page_queue& q, unsigned c) noexcept {
    while (page* const p = q.first) {
        detach(q, p);
        gather(p);
        if (p->used == 0) {
            retire(p);
            continue;
        }

        p->owner.store(0, std::memory_order_release);
        std::lock_guard<spin_lock> held(global_lock);
        p->next = abandoned[c].load(std::memory_order_relaxed);
        abandoned[c].store(p, std::memory_order_relaxed);
    }
}

// 线程退出（或者临时的堆用完了）：空页交回，还有块的页放进共享池，堆留给下一根新线程。
void heap_abandon(heap* h) noexcept {
    if (this_heap == h) {
        this_heap = &empty_heap;
    }

    for (unsigned c = 0; c < class_count; ++c) {
        abandon_queue(h->available[c], c);
        abandon_queue(h->full[c], c);
    }

    std::lock_guard<spin_lock> held(global_lock);
    h->next_free = free_heaps;
    free_heaps = h;
}

void heap_thread_exited(void* value) {
    heap_abandon(static_cast<heap*>(value));
}

heap* new_heap() noexcept {
    {
        std::lock_guard<spin_lock> held(global_lock);
        heap* const reused = free_heaps;
        if (reused) {
            free_heaps = reused->next_free;
            std::memset(static_cast<void*>(reused), 0, sizeof(heap));
            return reused;
        }
    }

    // 刚提交的页全是零
    const std::size_t size = (sizeof(heap) + page_size() - 1) / page_size() * page_size();
    void* const memory = reserve_pages(size);
    if (!memory) {
        return 0;
    }
    if (!commit_pages(memory, size)) {
        release_pages(memory, size);
        return 0;
    }
    return static_cast<heap*>(memory);
}

// 建本线程的堆。先装进 this_heap 再登记退出通知：登记时嵌套的分配直接用它。登记不了（这根线程正在退出）时
// 返回的堆不留在 this_heap 里，调用方用一次就交出去。
heap* heap_create() noexcept {
    if (free_key.load(std::memory_order_relaxed) == 0) {
        choose_free_key();
    }
    heap* const h = new_heap();
    if (!h) {
        return 0;
    }

    this_heap = h;
    if (!notify_at_thread_exit(&heap_thread_exited, h)) {
        this_heap = &empty_heap;
    }
    return h;
}

// ---- 慢路径 --------------------------------------------------------------------

// 分完的页里可能有别的线程刚还的块；每次最多看 4 页。
page* take_full(heap* h, unsigned c) noexcept {
    page_queue& q = h->full[c];
    for (int i = 0; i < 4 && q.first; ++i) {
        page* const p = q.first;
        detach(q, p);
        gather(p);
        if (!p->free && p->reserved < p->capacity) {
            extend(p);
        }
        if (p->free) {
            p->in_full = false;
            push_front(h->available[c], p);
            return p;
        }

        push_back(q, p);
    }
    return 0;
}

page* pop_abandoned(unsigned c) noexcept {
    std::lock_guard<spin_lock> held(global_lock);
    page* const p = abandoned[c].load(std::memory_order_relaxed);
    if (p) {
        abandoned[c].store(p->next, std::memory_order_relaxed);
    }
    return p;
}

// 接手退出的线程留下的页；共享池是空的就不拿锁。
page* take_abandoned(heap* h, unsigned c) noexcept {
    while (abandoned[c].load(std::memory_order_relaxed)) {
        page* const p = pop_abandoned(c);
        if (!p) {
            return 0;
        }

        p->owner.store(h, std::memory_order_release);
        gather(p);
        if (!p->free && p->reserved < p->capacity) {
            extend(p);
        }
        if (p->free) {
            p->in_full = false;
            push_front(h->available[c], p);
            return p;
        }

        p->in_full = true;
        push_back(h->full[c], p);
    }
    return 0;
}

// 各档留着的那一页，每 kept_page_tick 次慢路径交回一次。
void release_kept_pages(heap* h, unsigned skip) noexcept {
    for (unsigned c = 0; c < class_count; ++c) {
        page* const p = h->available[c].first;
        if (c == skip || !p || p->used != 0 || p != h->available[c].last) {
            continue;
        }

        detach(h->available[c], p);
        retire(p);
    }
}

void* alloc_generic(heap* h, unsigned c) noexcept {
    ++h->slow_calls;
    if (h->slow_calls % purge_tick == 0) {
        purge_if_due();
    }
    if (h->slow_calls == kept_page_tick) {
        h->slow_calls = 0;
        release_kept_pages(h, c);
    }

    page_queue& q = h->available[c];
    while (page* const p = q.first) {
        gather(p);
        if (!p->free && p->reserved < p->capacity) {
            extend(p);
        }
        if (p->free) {
            return pop(p);
        }

        // 分完了：挪到 full，等别的线程还块
        detach(q, p);
        p->in_full = true;
        push_back(h->full[c], p);
    }

    page* p = take_full(h, c);
    if (!p) {
        p = take_abandoned(h, c);
    }
    if (!p) {
        p = take_fresh(h, c);
    }
    if (!p) {
        return 0;
    }
    return pop(p);
}

CU_DETAIL_NOINLINE void* alloc_small_slow(unsigned c) noexcept {
    heap* const h = this_heap != &empty_heap ? this_heap : heap_create();
    if (!h) {
        return 0;
    }
    if (h == this_heap) {
        return alloc_generic(h, c);
    }

    // 这根线程的退出通知已经发过，之后的析构里又在分配：用一次就交出去的临时堆
    void* const result = alloc_generic(h, c);
    heap_abandon(h);
    return result;
}

inline void* alloc_class(unsigned c) noexcept {
    page* const p = this_heap->available[c].first;
    if (!p || !p->free) {
        return alloc_small_slow(c);
    }
    return pop(p);
}

// ---- 大块 ----------------------------------------------------------------------

// 公共接口里不放取地址的局部变量：开着栈保护的编译器会让每次调用都设、查一遍金丝雀，所以大块单独成函数。
// 只要大块、不要小块的程序不走小块的慢路径，在这里隔一阵看一眼该不该还内存。
CU_DETAIL_NOINLINE void* alloc_large(std::size_t size, std::size_t alignment) noexcept {
    if (alignment > large_align_max) {
        return 0;
    }
    if (++large_calls == purge_tick) {
        large_calls = 0;
        purge_if_due();
    }
    const std::size_t offset = alignment > header_size ? alignment : header_size;
    if (size > std::numeric_limits<std::size_t>::max() / 2) {
        return 0;
    }

    const std::size_t count = (offset + size + mem_page_size - 1) >> mem_page_shift;
    if (count <= max_run_pages) {
        page_run run = {0, 0};
        {
            std::lock_guard<spin_lock> held(global_lock);
            run = take_pages_locked(count);
        }
        if (!run.first) {
            return 0;
        }
        new (run.first) page(large_run, run.home, count);
        return run.first + offset;
    }

    // 一段地址装不下：单独要，还的时候整段还掉
    const std::size_t bytes = count << mem_page_shift;
    char* const base = static_cast<char*>(reserve_aligned_pages(bytes, mem_page_size));
    if (!base) {
        return 0;
    }
    if (!commit_pages(base, bytes)) {
        release_pages(base, bytes);
        return 0;
    }
    new (base) page(huge_block, 0, bytes);
    return base + offset;
}

// ---- 释放 ----------------------------------------------------------------------

// 主人自己还的：在 full 里就挪回 available；页空了而且这一档不只这一页，就交回去。
CU_DETAIL_NOINLINE void free_local_slow(heap* h, page* p) noexcept {
    const unsigned c = p->size_class;
    if (p->in_full) {
        detach(h->full[c], p);
        p->in_full = false;
        push_back(h->available[c], p);
    }
    if (p->used != 0) {
        return;
    }

    page_queue& q = h->available[c];
    if (q.first == p && q.last == p) {
        return;
    }
    detach(q, p);
    retire(p);
}

// 别的线程的页，或者大块。大块还掉以前把 kind 清成 0：页还没被别人拿走时再还一次，就落到最后那个诊断上。
CU_DETAIL_NOINLINE void free_other(page* p, void* address) noexcept {
    if (p->kind == small_page) {
        block* const b = static_cast<block*>(address);
        mark_freed(b);
        block* head = p->thread_free.load(std::memory_order_relaxed);
        do {
            b->next = head;
        } while (!p->thread_free.compare_exchange_weak(head, b, std::memory_order_release, std::memory_order_relaxed));
        return;
    }

    if (p->kind == large_run) {
        p->kind = 0;
        std::lock_guard<spin_lock> held(global_lock);
        give_pages_locked(p->home, reinterpret_cast<char*>(p), p->span);
        note_free_pages_locked();
        return;
    }

    if (p->kind == huge_block) {
        release_pages(p, p->span);
        return;
    }

    fatal("cu::mem: double free, or free of a pointer cu::mem did not hand out: %p", address);
}

}  // namespace

std::int64_t mem_purge_deadline() noexcept {
    const std::int64_t at = purge_at.load(std::memory_order_relaxed);
    return at != 0 ? at : std::numeric_limits<std::int64_t>::max();
}

void mem_purge(std::int64_t now) noexcept {
    const std::int64_t at = purge_at.load(std::memory_order_relaxed);
    if (at == 0 || now < at) {
        return;
    }

    std::lock_guard<spin_lock> held(global_lock);
    purge_locked(now, now + purge_budget);
}

mem_stats read_mem_stats() noexcept {
    std::lock_guard<spin_lock> held(global_lock);
    const region_stats regions = read_region_stats_locked();
    mem_stats stats = {regions.regions, regions.used_pages, regions.dirty_pages, 0};
    for (unsigned c = 0; c < class_count; ++c) {
        for (page* p = abandoned[c].load(std::memory_order_relaxed); p; p = p->next) {
            ++stats.abandoned_pages;
        }
    }
    return stats;
}

}  // namespace detail

namespace mem {

void* alloc(std::size_t size) noexcept {
    if (size <= detail::small_max) {
        return detail::alloc_class(detail::class_of(size));
    }
    return detail::alloc_large(size, 0);
}

void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return 0;
    }
    if (alignment <= 16) {
        return alloc(size);
    }
    if (alignment > detail::small_align_max || size > detail::small_max) {
        return detail::alloc_large(size, alignment);
    }

    // 块大小是 alignment 的倍数的那一档：页里的块从 128 字节处开始，每一块都对齐
    unsigned c = detail::class_of(size < alignment ? alignment : size);
    while (detail::class_sizes[c] % alignment != 0) {
        ++c;
    }
    return detail::alloc_class(c);
}

void free(void* p) noexcept {
    if (!p) {
        return;
    }

    detail::page* const owner_page = detail::page_of(p);
    detail::heap* const h = detail::this_heap;
    if (owner_page->owner.load(std::memory_order_relaxed) != h) {
        detail::free_other(owner_page, p);
        return;
    }

    // 本线程的页：挂到 local_free；页空了或者原来在 full 里，走慢路径
    detail::block* const b = static_cast<detail::block*>(p);
    detail::mark_freed(b);
    b->next = owner_page->local_free;
    owner_page->local_free = b;
    if (--owner_page->used == 0 || owner_page->in_full) {
        detail::free_local_slow(h, owner_page);
    }
}

}  // namespace mem
}  // namespace cu
