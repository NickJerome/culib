// wavvix::mem 的分配器：按 64 MiB 对齐的 chunk 切成 64 KiB 的 slice，小块按尺寸分档放进页，
// 每个线程一个堆。结构、并发约定、取舍和测量见 docs/mem-internals.md。
#include "mem_stats.h"
#include "native.h"
#include "../pages.h"

#include <culib/mem.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(_MSC_VER)
#include <intrin.h>
#define WAVVIX_DETAIL_MEM_NOINLINE __declspec(noinline)
#else
#define WAVVIX_DETAIL_MEM_NOINLINE __attribute__((noinline))
#endif

static_assert(sizeof(void*) == 8, "wavvix::mem supports 64-bit targets only");

namespace wavvix { namespace mem { namespace detail {

namespace {

// ---- 尺寸 ----------------------------------------------------------------------

const std::size_t slice_shift = 16;
const std::size_t slice_size = std::size_t(1) << slice_shift;   // 64 KiB
const std::size_t chunk_shift = 26;
const std::size_t chunk_size = std::size_t(1) << chunk_shift;   // 64 MiB，按自身大小对齐
const std::size_t chunk_slices = chunk_size / slice_size;
const std::size_t bitmap_words = chunk_slices / 64;

// 8 KiB 以内一页一个 slice；8–64 KiB 一页 8 个 slice（512 KiB），每页至少 8 块
const std::size_t small_max = 8192;
const std::size_t medium_max = 65536;
const std::size_t medium_slices = 8;
const int bin_count = 49;

const std::uint64_t purge_delay_ms = 1000;   // 空出来的 slice 至少等这么久才还给系统
const unsigned kept_page_tick = 1024;         // 每这么多次慢路径，还掉各档留着的那个空页
const std::size_t npos = ~std::size_t(0);

inline int clz64(std::uint64_t value) {
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanReverse64(&index, value);
    return 63 - static_cast<int>(index);
#else
    return __builtin_clzll(value);
#endif
}

inline int ctz64(std::uint64_t value) {
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanForward64(&index, value);
    return static_cast<int>(index);
#else
    return __builtin_ctzll(value);
#endif
}

inline std::uint64_t bit(std::size_t index) {
    return std::uint64_t(1) << (index % 64);
}

inline std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

inline std::uint64_t mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// 64 字节以内 16 字节一档，之后每翻一倍分四档；块大小都是 16 的倍数
inline int bin_of(std::size_t size) {
    std::size_t words = (size + 7) >> 3;
    if (words <= 2) {
        return 2;
    }
    if (words <= 8) {
        return static_cast<int>((words + 1) & ~std::size_t(1));
    }

    --words;
    const int top = 63 - clz64(words);
    return (top << 2) + static_cast<int>((words >> (top - 2)) & 3) - 3;
}

inline std::size_t bin_size(int bin) {
    if (bin <= 8) {
        return static_cast<std::size_t>(bin) * 8;
    }

    const int top = (bin + 3) >> 2;
    return static_cast<std::size_t>(5 + ((bin + 3) & 3)) << (top + 1);
}

// ---- 数据结构 ------------------------------------------------------------------

struct heap;

// 空闲块：next 串成链表。块被释放时 key 写成 g.free_key、分配时清零，再次释放时还是它就是重复释放
struct block {
    block* next;
    std::uint64_t key;
};

// 一页的描述符，放在 chunk 头部，按 slice 下标排列
struct page {
    block* free;                       // 拥有者从这里分配
    block* local_free;                 // 拥有者释放到这里
    std::atomic<block*> thread_free;   // 其他线程释放到这里
    std::atomic<heap*> owner;          // 空：大块，或拥有者线程已退出
    page* next;
    page* prev;
    std::uint32_t block_size;          // 0：大块
    std::uint16_t used;                // 没收回的块，含 thread_free 里的
    std::uint16_t capacity;
    std::uint16_t reserved;            // 已切出的块；后面的内存从没写过
    std::uint16_t slices;              // 0：这页已经还掉了
    std::uint8_t bin;
    bool in_full;
    std::uint8_t back;                 // 中等页第 i 个 slice 的描述符记 i，据此找回页头；其余为 0
};
static_assert(sizeof(page) <= 64, "a page descriptor fits in one cache line");

// 描述符间隔 128 字节，相邻两页的描述符不共用一对缓存行（实测比 64 字节快）
const std::size_t desc_stride = 128;

struct queue {
    page* first;
    page* last;
};

struct heap {
    queue available[bin_count];   // 队首页供快路径分配
    queue full[bin_count];        // 分完的页，等其他线程释放
    unsigned slow_calls;
    heap* next_free;
};

// chunk 开头 64 KiB 放位图，接着是描述符表（每个 slice 一项，共 128 KiB），之后才是可分配的 slice。
// 描述符表按 64 KiB 一段、用到才提交；按 64 KiB 提交也照顾到 16 KiB、64 KiB 的系统页。
const std::size_t header_bytes = slice_size;
const std::size_t table_unit = slice_size;
const std::size_t header_slices = (header_bytes + chunk_slices * desc_stride) / slice_size;

struct chunk {
    std::uint64_t free_bits[bitmap_words];        // 1：空闲
    std::uint64_t committed_bits[bitmap_words];   // 1：提交过，可能还占着物理内存
    std::uint64_t table_bits;                     // 描述符表的哪几段已经提交
    std::size_t huge_size;                        // 非零：整段只装一个超大块，这是保留的总字节数
    std::size_t huge_offset;                      // 超大块相对起点的偏移
};
static_assert(sizeof(chunk) <= header_bytes, "the chunk header fits in its first 64 KiB");
static_assert(chunk_slices * desc_stride / table_unit <= 64, "descriptor table units fit in table_bits");

// ---- 全局状态：都在静态区清零，不需要构造 --------------------------------------

struct global_state {
    chunk* chunks[16384];                      // 只增不减，最多 1 TiB；超大块不在这里
    std::size_t chunk_count;
    std::atomic<std::uint64_t> purge_at;       // 0：没有等着归还的 slice；锁外只读
    std::atomic<page*> abandoned[bin_count];   // 拥有者线程退出时还有活块的页；锁外只看是否为空
    heap* free_heaps;
    std::uint64_t free_key;                    // 第一次建堆时生成，之后只读
};

global_state g;
heap empty_heap;   // 队列全空：还没有堆的线程一律走慢路径
thread_local heap* t_heap = &empty_heap;

// 每 64 MiB 地址各一位，标出 chunk 和超大块的起点：free 先查它，挡住不是本分配器给出的指针。
// 用户态地址不超过 2^48（x86-64 和 arm64 上的 Linux、macOS、Windows 都是），每张图 512 KiB，
// 在静态区里，用到的那几个字才占物理内存。
const std::size_t map_words = (std::size_t(1) << (48 - chunk_shift)) / 64;
std::atomic<std::uint64_t> chunk_map[map_words];
std::atomic<std::uint64_t> huge_map[map_words];

struct locked {
    locked() {
        native::lock();
    }

    ~locked() {
        native::unlock();
    }

    locked(const locked&) = delete;
    locked& operator=(const locked&) = delete;
};

inline bool in_map(const std::atomic<std::uint64_t>* map, const void* address) {
    const std::uintptr_t index = reinterpret_cast<std::uintptr_t>(address) >> chunk_shift;
    if (index >= map_words * 64) {
        return false;
    }

    return (map[index / 64].load(std::memory_order_relaxed) & bit(index)) != 0;
}

// 地址超出图的范围时返回 false
bool mark(std::atomic<std::uint64_t>* map, const void* base, bool on) {
    const std::uintptr_t index = reinterpret_cast<std::uintptr_t>(base) >> chunk_shift;
    if (index >= map_words * 64) {
        return false;
    }

    if (on) {
        map[index / 64].fetch_or(bit(index), std::memory_order_relaxed);
    } else {
        map[index / 64].fetch_and(~bit(index), std::memory_order_relaxed);
    }
    return true;
}

inline chunk* chunk_of(const void* address) {
    return reinterpret_cast<chunk*>(reinterpret_cast<std::uintptr_t>(address) & ~(chunk_size - 1));
}

inline page* desc_at(chunk* c, std::size_t index) {
    return reinterpret_cast<page*>(reinterpret_cast<char*>(c) + header_bytes + index * desc_stride);
}

inline std::size_t index_of(const page* p) {
    const std::uintptr_t table = reinterpret_cast<std::uintptr_t>(chunk_of(p)) + header_bytes;
    return (reinterpret_cast<std::uintptr_t>(p) - table) / desc_stride;
}

inline char* start_of(const page* p) {
    return reinterpret_cast<char*>(chunk_of(p)) + (index_of(p) << slice_shift);
}

// 地址所在的页，以及地址相对页起点的偏移
struct located {
    page* p;
    std::size_t offset;
};

// 落在中等页后面的 slice 里时，描述符记着离页头隔了几个 slice（back），其余为 0
inline located locate(const void* address) {
    const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(address);
    chunk* const c = chunk_of(address);
    const std::size_t index = (at & (chunk_size - 1)) >> slice_shift;
    const std::size_t back = desc_at(c, index)->back;
    located result;
    result.p = desc_at(c, index - back);
    result.offset = (at & (slice_size - 1)) + (back << slice_shift);
    return result;
}

// ---- 误用 ----------------------------------------------------------------------

// 不是本分配器给出的指针、块中间的地址、重复释放：打印一行诊断后 abort，release 下同样检查
[[noreturn]] WAVVIX_DETAIL_MEM_NOINLINE void fail(const char* what, const void* ptr) {
    char line[128];
    std::size_t n = 0;
    for (const char* s = what; *s != '\0' && n < 96; ++s) {
        line[n++] = *s;
    }
    for (const char* s = " ptr=0x"; *s != '\0'; ++s) {
        line[n++] = *s;
    }

    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(ptr);
    for (int shift = 60; shift >= 0; shift -= 4) {
        line[n++] = "0123456789abcdef"[(value >> shift) & 15];
    }

    line[n] = '\0';
    native::die(line);
}

// 偏移 offset 处是不是活页 p 里一个块的起点
inline bool block_start(const page* p, std::size_t offset) {
    if (p->block_size == 0) {
        return offset == 0;
    }

    return offset < std::size_t(p->capacity) * p->block_size &&
           static_cast<std::uint32_t>(offset) % p->block_size == 0;
}

// ptr 所在的页；ptr 必须是活块的起点，否则报错退出
inline page* checked_page_of(void* ptr) {
    const located at = locate(ptr);
    if (at.p->slices == 0) {
        fail("free of a block whose page was already released", ptr);
    }
    if (!block_start(at.p, at.offset)) {
        fail("free of an interior pointer", ptr);
    }

    return at.p;
}

inline void mark_freed(block* b) {
    if (b->key == g.free_key) {
        fail("double free", b);
    }

    b->key = g.free_key;
}

// ---- slice：调用方持锁 ---------------------------------------------------------

void purge_locked() {
    for (std::size_t i = 0; i < g.chunk_count; ++i) {
        chunk* const c = g.chunks[i];
        for (std::size_t w = 0; w < bitmap_words; ++w) {
            std::uint64_t bits = c->free_bits[w] & c->committed_bits[w];
            while (bits != 0) {
                // 一次归还一段连续的 slice；归还失败的仍记作已提交，下次再试
                const int first = ctz64(bits);
                const std::uint64_t shifted = bits >> first;
                const int count = shifted == ~std::uint64_t(0) ? 64 : ctz64(~shifted);
                const std::uint64_t run = count == 64 ? ~std::uint64_t(0) : ((std::uint64_t(1) << count) - 1) << first;
                bits &= ~run;

                char* const at = reinterpret_cast<char*>(c) + ((w * 64 + static_cast<std::size_t>(first)) << slice_shift);
                if (pages::decommit(at, static_cast<std::size_t>(count) << slice_shift)) {
                    c->committed_bits[w] &= ~run;
                }
            }
        }
    }
    g.purge_at.store(0, std::memory_order_relaxed);
}

void maybe_purge_locked() {
    const std::uint64_t at = g.purge_at.load(std::memory_order_relaxed);
    if (at != 0 && native::now_ms() >= at) {
        purge_locked();
    }
}

chunk* chunk_create_locked() {
    if (g.chunk_count == sizeof(g.chunks) / sizeof(g.chunks[0])) {
        return 0;
    }

    void* const base = pages::reserve(chunk_size, chunk_size);
    if (!base) {
        return 0;
    }
    // 块稀疏地落在 chunk 里：开了透明大页的系统上，碰一个字节就可能占满 2 MiB
    pages::no_huge_pages(base, chunk_size);
    if (!pages::commit(base, header_bytes) || !mark(chunk_map, base, true)) {
        pages::release(base, chunk_size);
        return 0;
    }

    // 新映射全是零：committed_bits、table_bits 和超大块字段本来就是 0
    chunk* const c = static_cast<chunk*>(base);
    for (std::size_t w = 0; w < bitmap_words; ++w) {
        c->free_bits[w] = ~std::uint64_t(0);
    }
    for (std::size_t s = 0; s < header_slices; ++s) {
        c->free_bits[s / 64] &= ~bit(s);
    }

    g.chunks[g.chunk_count] = c;
    ++g.chunk_count;
    return c;
}

// from 起第一个空闲的 slice；没有就返回 chunk_slices
std::size_t next_free(const chunk* c, std::size_t from) {
    std::size_t s = from;
    while (s < chunk_slices) {
        const std::uint64_t idle = c->free_bits[s / 64] >> (s % 64);
        if (idle != 0) {
            return s + static_cast<std::size_t>(ctz64(idle));
        }
        s = (s / 64 + 1) * 64;
    }
    return chunk_slices;
}

// [from, to) 里第一个占用的 slice；全空返回 npos
std::size_t first_busy(const chunk* c, std::size_t from, std::size_t to) {
    std::size_t s = from;
    while (s < to) {
        const std::uint64_t busy = ~c->free_bits[s / 64] >> (s % 64);
        if (busy != 0) {
            const std::size_t at = s + static_cast<std::size_t>(ctz64(busy));
            return at < to ? at : npos;
        }
        s = (s / 64 + 1) * 64;
    }
    return npos;
}

// 找 n 个连续空闲的 slice，起点下标是 step 的倍数。chunk 按自身大小对齐，下标对齐就是地址对齐
std::size_t find_run(const chunk* c, std::size_t n, std::size_t step) {
    std::size_t start = align_up(next_free(c, header_slices), step);
    while (start + n <= chunk_slices) {
        const std::size_t busy = first_busy(c, start, start + n);
        if (busy == npos) {
            return start;
        }
        start = align_up(next_free(c, busy + 1), step);
    }
    return npos;
}

// 描述符表用到哪段提交哪段
bool commit_table_locked(chunk* c, std::size_t first, std::size_t count) {
    const std::size_t begin = first * desc_stride / table_unit;
    const std::size_t end = ((first + count) * desc_stride - 1) / table_unit;
    for (std::size_t unit = begin; unit <= end; ++unit) {
        if (c->table_bits & bit(unit)) {
            continue;
        }

        char* const at = reinterpret_cast<char*>(c) + header_bytes + unit * table_unit;
        if (!pages::commit(at, table_unit)) {
            return false;
        }
        c->table_bits |= bit(unit);
    }
    return true;
}

// 取 n 个连续 slice，起点按 step 个 slice 对齐；*zeroed 报告这段读出来是不是全零
page* slices_alloc_locked(std::size_t n, std::size_t step, bool* zeroed) {
    maybe_purge_locked();

    chunk* c = 0;
    std::size_t index = npos;
    for (std::size_t i = 0; i < g.chunk_count && index == npos; ++i) {
        c = g.chunks[i];
        index = find_run(c, n, step);
    }
    if (index == npos) {
        c = chunk_create_locked();
        index = c ? find_run(c, n, step) : npos;
    }
    if (index == npos) {
        return 0;
    }

    // 有一片没提交过就整段一起提交；一片都没提交过的读出来全零
    bool fresh = true;
    bool committed = true;
    for (std::size_t s = index; s < index + n; ++s) {
        if (c->committed_bits[s / 64] & bit(s)) {
            fresh = false;
        } else {
            committed = false;
        }
    }

    char* const start = reinterpret_cast<char*>(c) + (index << slice_shift);
    if (!commit_table_locked(c, index, n) || (!committed && !pages::commit(start, n << slice_shift))) {
        return 0;
    }

    for (std::size_t s = index; s < index + n; ++s) {
        c->free_bits[s / 64] &= ~bit(s);
        c->committed_bits[s / 64] |= bit(s);
    }

    page* const p = desc_at(c, index);
    p->slices = static_cast<std::uint16_t>(n);
    p->back = 0;   // 这个 slice 以前可能是别的中等页的后半段
    *zeroed = fresh;
    return p;
}

page* slices_alloc(std::size_t n, std::size_t step, bool* zeroed) {
    locked hold;
    return slices_alloc_locked(n, step, zeroed);
}

void slices_free_locked(page* p) {
    chunk* const c = chunk_of(p);
    const std::size_t index = index_of(p);
    const std::size_t n = p->slices;
    p->slices = 0;   // 这页还掉了：之后对它的 free 会被当成误用
    for (std::size_t s = index; s < index + n; ++s) {
        c->free_bits[s / 64] |= bit(s);
    }

    if (g.purge_at.load(std::memory_order_relaxed) == 0) {
        g.purge_at.store(native::now_ms() + purge_delay_ms, std::memory_order_relaxed);
    }
    maybe_purge_locked();
}

void slices_free(page* p) {
    locked hold;
    slices_free_locked(p);
}

// 稳态下既不取也不还 slice，靠慢路径定期看一眼，到期的照样还
void purge_if_due() {
    const std::uint64_t at = g.purge_at.load(std::memory_order_relaxed);
    if (at == 0 || native::now_ms() < at) {
        return;
    }

    locked hold;
    maybe_purge_locked();
}

// ---- 页 ------------------------------------------------------------------------

void push_front(queue& q, page* p) {
    p->prev = 0;
    p->next = q.first;
    if (q.first) {
        q.first->prev = p;
    } else {
        q.last = p;
    }
    q.first = p;
}

void push_back(queue& q, page* p) {
    p->next = 0;
    p->prev = q.last;
    if (q.last) {
        q.last->next = p;
    } else {
        q.first = p;
    }
    q.last = p;
}

void detach(queue& q, page* p) {
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

// 把其他线程释放的一串块接到 local_free 前面
void prepend_remote(page* p, block* remote) {
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

// 收回其他线程释放的块；free 用完时接上 local_free
void gather(page* p) {
    block* const remote = p->thread_free.exchange(0, std::memory_order_acquire);
    if (remote) {
        prepend_remote(p, remote);
    }

    if (!p->free) {
        p->free = p->local_free;
        p->local_free = 0;
    }
}

// 每次最多切出 4 KiB 的新块；没用到的部分不写，也就不占物理内存
void extend(page* p) {
    const std::size_t size = p->block_size;
    std::size_t count = size >= 4096 ? 1 : 4096 / size;
    const std::size_t left = static_cast<std::size_t>(p->capacity - p->reserved);
    if (count > left) {
        count = left;
    }

    char* const first = start_of(p) + static_cast<std::size_t>(p->reserved) * size;
    for (std::size_t i = 0; i + 1 < count; ++i) {
        reinterpret_cast<block*>(first + i * size)->next = reinterpret_cast<block*>(first + (i + 1) * size);
    }
    reinterpret_cast<block*>(first + (count - 1) * size)->next = p->free;
    p->free = reinterpret_cast<block*>(first);
    p->reserved = static_cast<std::uint16_t>(p->reserved + count);
}

void retire(page* p) {
    p->owner.store(0, std::memory_order_relaxed);
    slices_free(p);
}

inline void* pop(page* p) {
    block* const b = p->free;
    p->free = b->next;
    ++p->used;
    b->key = 0;
    return b;
}

// ---- 线程堆 --------------------------------------------------------------------

void abandon_queue(queue& q, int bin) {
    while (page* const p = q.first) {
        detach(q, p);
        gather(p);
        if (p->used == 0) {
            retire(p);
            continue;
        }

        p->owner.store(0, std::memory_order_release);
        locked hold;
        p->next = g.abandoned[bin].load(std::memory_order_relaxed);
        g.abandoned[bin].store(p, std::memory_order_relaxed);
    }
}

// 线程退出（或临时堆用完）：空页还掉，还有活块的页交给共享池，其他线程按尺寸接手
void heap_abandon(heap* h) {
    if (t_heap == h) {
        t_heap = &empty_heap;
    }

    for (int bin = 0; bin < bin_count; ++bin) {
        abandon_queue(h->available[bin], bin);
        abandon_queue(h->full[bin], bin);
    }

    locked hold;
    h->next_free = g.free_heaps;
    g.free_heaps = h;
}

heap* reuse_heap() {
    locked hold;
    if (g.free_key == 0) {
        // 重复释放检查用的随机值：不需要密码学强度，只要不会和用户数据撞上
        g.free_key = mix(native::random_seed()) | 1;
    }

    heap* const h = g.free_heaps;
    if (h) {
        g.free_heaps = h->next_free;
    }
    return h;
}

// 建本线程的堆。登记不了线程退出回调（本线程正在退出）时，返回一个不装进 t_heap 的临时堆
heap* heap_create() {
    native::watch_fork();
    if (t_heap != &empty_heap) {
        return t_heap;   // 登记 fork 钩子时嵌套的分配已经建好了
    }

    heap* h = reuse_heap();
    if (!h) {
        h = static_cast<heap*>(pages::allocate(sizeof(heap)));
    }
    if (!h) {
        return 0;
    }

    std::memset(h, 0, sizeof(heap));
    t_heap = h;
    if (!native::watch_thread_exit(h)) {
        t_heap = &empty_heap;
    }
    return h;
}

// 满页里可能有其他线程刚释放的块；每次最多看四页
page* take_full(heap* h, int bin) {
    queue& q = h->full[bin];
    for (int i = 0; i < 4 && q.first; ++i) {
        page* const p = q.first;
        detach(q, p);
        gather(p);
        if (!p->free && p->reserved < p->capacity) {
            extend(p);
        }
        if (p->free) {
            p->in_full = false;
            push_front(h->available[bin], p);
            return p;
        }

        push_back(q, p);
    }
    return 0;
}

page* pop_abandoned(int bin) {
    locked hold;
    page* const p = g.abandoned[bin].load(std::memory_order_relaxed);
    if (p) {
        g.abandoned[bin].store(p->next, std::memory_order_relaxed);
    }
    return p;
}

// 接手已退出线程留下的页；共享池是空的就不取锁
page* take_abandoned(heap* h, int bin) {
    while (g.abandoned[bin].load(std::memory_order_relaxed)) {
        page* const p = pop_abandoned(bin);
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
            push_front(h->available[bin], p);
            return p;
        }

        p->in_full = true;
        push_back(h->full[bin], p);
    }
    return 0;
}

page* take_fresh(heap* h, int bin) {
    const std::size_t size = bin_size(bin);
    const std::size_t n = size <= small_max ? 1 : medium_slices;
    bool zeroed = false;
    page* const p = slices_alloc(n, 1, &zeroed);
    if (!p) {
        return 0;
    }

    p->free = 0;
    p->local_free = 0;
    p->thread_free.store(0, std::memory_order_relaxed);
    p->owner.store(h, std::memory_order_relaxed);
    p->block_size = static_cast<std::uint32_t>(size);
    p->used = 0;
    p->capacity = static_cast<std::uint16_t>(n * slice_size / size);
    p->reserved = 0;
    p->bin = static_cast<std::uint8_t>(bin);
    p->in_full = false;

    // 中等页后面几个 slice 的描述符记下离页头多远
    chunk* const c = chunk_of(p);
    const std::size_t index = index_of(p);
    for (std::size_t i = 1; i < n; ++i) {
        desc_at(c, index + i)->back = static_cast<std::uint8_t>(i);
    }

    extend(p);
    push_front(h->available[bin], p);
    return p;
}

// 各档留着的那一个空页，每 kept_page_tick 次慢路径还掉一次
void release_kept_pages(heap* h, int skip) {
    for (int bin = 0; bin < bin_count; ++bin) {
        page* const p = h->available[bin].first;
        if (bin == skip || !p || p->used != 0 || p != h->available[bin].last) {
            continue;
        }

        detach(h->available[bin], p);
        retire(p);
    }
}

void* alloc_generic(heap* h, int bin) {
    ++h->slow_calls;
    if (h->slow_calls == kept_page_tick) {
        h->slow_calls = 0;
        release_kept_pages(h, bin);
        purge_if_due();
    }

    queue& q = h->available[bin];
    while (page* const p = q.first) {
        gather(p);
        if (!p->free && p->reserved < p->capacity) {
            extend(p);
        }
        if (p->free) {
            return pop(p);
        }

        // 分完了：挪到满页队列，等其他线程释放
        detach(q, p);
        p->in_full = true;
        push_back(h->full[bin], p);
    }

    page* p = take_full(h, bin);
    if (!p) {
        p = take_abandoned(h, bin);
    }
    if (!p) {
        p = take_fresh(h, bin);
    }
    if (!p) {
        return 0;
    }

    return pop(p);
}

WAVVIX_DETAIL_MEM_NOINLINE void* alloc_small_slow(int bin) {
    heap* const h = t_heap != &empty_heap ? t_heap : heap_create();
    if (!h) {
        return 0;
    }
    if (h == t_heap) {
        return alloc_generic(h, bin);
    }

    // 本线程的退出回调已经跑过，之后的析构里又在分配：用一次就交出去的临时堆
    void* const result = alloc_generic(h, bin);
    heap_abandon(h);
    return result;
}

inline void* alloc_bin(int bin) {
    page* const p = t_heap->available[bin].first;
    if (!p || !p->free) {
        return alloc_small_slow(bin);
    }

    return pop(p);
}

// ---- 大块 ----------------------------------------------------------------------

// 超大块的偏移要小于一个 chunk，chunk_of 才找得回它的头部，所以对齐最多 32 MiB
const std::size_t align_max = chunk_size / 2;

// 超过一个 chunk 能装的块单独保留一段地址：开头放 chunk 头记下大小，块从对齐的偏移开始
void* alloc_huge(std::size_t size, std::size_t alignment) {
    const std::size_t offset = alignment > header_bytes ? alignment : header_bytes;
    if (size > (std::numeric_limits<std::size_t>::max)() / 2 - offset) {
        return 0;
    }

    const std::size_t total = align_up(offset + size, slice_size);
    void* const base = pages::reserve(total, chunk_size);
    if (!base) {
        return 0;
    }
    if (!pages::commit(base, total)) {
        pages::release(base, total);
        return 0;
    }

    chunk* const c = static_cast<chunk*>(base);
    c->huge_size = total;
    c->huge_offset = offset;
    if (!mark(huge_map, base, true)) {
        pages::release(base, total);
        return 0;
    }
    return static_cast<char*>(base) + offset;
}

// 按 slice 分配，起点至少按 64 KiB 对齐；*zeroed 报告块读出来是不是全零
void* alloc_large(std::size_t size, std::size_t alignment, bool* zeroed) {
    if (alignment > align_max) {
        return 0;
    }

    const std::size_t step = alignment > slice_size ? alignment / slice_size : 1;
    if (size > chunk_size || align_up(header_slices, step) + (size + slice_size - 1) / slice_size > chunk_slices) {
        *zeroed = true;
        return alloc_huge(size, alignment);
    }

    page* const p = slices_alloc((size + slice_size - 1) / slice_size, step, zeroed);
    if (!p) {
        return 0;
    }

    p->free = 0;
    p->local_free = 0;
    p->thread_free.store(0, std::memory_order_relaxed);
    p->owner.store(0, std::memory_order_relaxed);
    p->block_size = 0;
    p->used = 0;
    p->capacity = 0;
    p->reserved = 0;
    p->bin = 0;
    p->in_full = false;
    return start_of(p);
}

// 公开接口里不能有取地址的局部变量：开了栈保护的编译器会让每次调用都设、查一遍金丝雀
// （Ubuntu 的 GCC 默认开，实测 alloc 每次多 12 条指令），所以大块路径单独成函数
WAVVIX_DETAIL_MEM_NOINLINE void* alloc_large_block(std::size_t size, std::size_t alignment) {
    bool zeroed = false;
    return alloc_large(size, alignment, &zeroed);
}

// 整段都是新提交的大块读出来本来就是零，不再写一遍，也就不会把整块拖进物理内存
WAVVIX_DETAIL_MEM_NOINLINE void* calloc_large(std::size_t total) {
    bool zeroed = false;
    void* const p = alloc_large(total, 0, &zeroed);
    if (p && !zeroed) {
        std::memset(p, 0, total);
    }
    return p;
}

std::size_t huge_usable_size(const void* ptr) {
    if (!in_map(huge_map, ptr)) {
        return 0;
    }

    const chunk* const c = chunk_of(ptr);
    if (ptr != reinterpret_cast<const char*>(c) + c->huge_offset) {
        return 0;
    }

    return c->huge_size - c->huge_offset;
}

// ---- 释放 ----------------------------------------------------------------------

// 不在任何 chunk 里：要么是超大块，要么是误用
WAVVIX_DETAIL_MEM_NOINLINE void free_huge(void* ptr) {
    if (!in_map(huge_map, ptr)) {
        fail("free of a pointer wavvix::mem did not allocate", ptr);
    }

    chunk* const c = chunk_of(ptr);
    if (ptr != reinterpret_cast<char*>(c) + c->huge_offset) {
        fail("free of an interior pointer", ptr);
    }

    const std::size_t size = c->huge_size;
    mark(huge_map, c, false);
    pages::release(c, size);
}

WAVVIX_DETAIL_MEM_NOINLINE void free_local_slow(heap* h, page* p) {
    const int bin = p->bin;
    if (p->in_full) {
        detach(h->full[bin], p);
        p->in_full = false;
        push_back(h->available[bin], p);
    }
    if (p->used != 0) {
        return;
    }

    // 这个尺寸只剩这一页就留着，免得分配和释放交替时反复换页
    queue& q = h->available[bin];
    if (q.first == p && q.last == p) {
        return;
    }

    detach(q, p);
    retire(p);
}

// 大块，或者别的线程的页
WAVVIX_DETAIL_MEM_NOINLINE void free_other(page* p, block* b) {
    if (p->block_size == 0) {
        slices_free(p);
        return;
    }

    mark_freed(b);
    block* head = p->thread_free.load(std::memory_order_relaxed);
    do {
        b->next = head;
    } while (!p->thread_free.compare_exchange_weak(head, b, std::memory_order_release, std::memory_order_relaxed));
}

// ---- 主动回收 ------------------------------------------------------------------

void collect_available(heap* h, int bin) {
    queue& q = h->available[bin];
    page* p = q.first;
    while (p) {
        page* const next = p->next;
        gather(p);
        if (p->used == 0) {
            detach(q, p);
            retire(p);
        }
        p = next;
    }
}

void collect_full(heap* h, int bin) {
    queue& q = h->full[bin];
    page* p = q.first;
    while (p) {
        page* const next = p->next;
        gather(p);
        if (p->used == 0) {
            detach(q, p);
            retire(p);
        } else if (p->free) {
            detach(q, p);
            p->in_full = false;
            push_back(h->available[bin], p);
        }
        p = next;
    }
}

// 本线程的页：收回别人释放的块，空页还掉，满页里有了空位的挪回可用队列
void collect_heap(heap* h) {
    for (int bin = 0; bin < bin_count; ++bin) {
        collect_available(h, bin);
        collect_full(h, bin);
    }
}

// 共享池里已经没有活块的页还掉；调用方持锁
void collect_abandoned_locked() {
    for (int bin = 0; bin < bin_count; ++bin) {
        page* kept = 0;
        page* p = g.abandoned[bin].load(std::memory_order_relaxed);
        while (p) {
            page* const next = p->next;
            gather(p);
            if (p->used == 0) {
                slices_free_locked(p);
            } else {
                p->next = kept;
                kept = p;
            }
            p = next;
        }
        g.abandoned[bin].store(kept, std::memory_order_relaxed);
    }
}

}  // namespace

namespace native {

void on_thread_exit(void* value) noexcept {
    heap_abandon(static_cast<heap*>(value));
}

}  // namespace native

stats get_stats() noexcept {
    locked hold;
    stats out = {0, 0};
    for (std::size_t i = 0; i < g.chunk_count; ++i) {
        for (std::size_t w = 0; w < bitmap_words; ++w) {
            // 逐位清掉数：std::bitset::count 在没有 POPCNT 指令时会调用 libgcc，覆盖库就得依赖 libgcc_s
            for (std::uint64_t bits = g.chunks[i]->committed_bits[w]; bits != 0; bits &= bits - 1) {
                out.committed_bytes += slice_size;
            }
        }
    }
    for (int bin = 0; bin < bin_count; ++bin) {
        for (page* p = g.abandoned[bin].load(std::memory_order_relaxed); p; p = p->next) {
            ++out.abandoned_pages;
        }
    }
    return out;
}

} // namespace detail

// ---- 公共接口 ------------------------------------------------------------------

void* alloc(std::size_t size) noexcept {
    if (size <= detail::medium_max) {
        return detail::alloc_bin(detail::bin_of(size));
    }

    return detail::alloc_large_block(size, 0);
}

void free(void* ptr) noexcept {
    if (!ptr) {
        return;
    }
    if (!detail::in_map(detail::chunk_map, ptr)) {
        detail::free_huge(ptr);
        return;
    }

    detail::page* const p = detail::checked_page_of(ptr);
    detail::block* const b = static_cast<detail::block*>(ptr);
    detail::heap* const h = detail::t_heap;
    if (p->owner.load(std::memory_order_relaxed) != h) {
        // 大块，或者别的线程的页
        detail::free_other(p, b);
        return;
    }

    // 本线程的页：挂到 local_free；页空了或者原来在满页队列里，就走慢路径
    detail::mark_freed(b);
    b->next = p->local_free;
    p->local_free = b;
    if (--p->used == 0 || p->in_full) {
        detail::free_local_slow(h, p);
    }
}

void* calloc(std::size_t count, std::size_t size) noexcept {
    if (count != 0 && size > (std::numeric_limits<std::size_t>::max)() / count) {
        return 0;
    }

    const std::size_t total = count * size;
    if (total > detail::medium_max) {
        return detail::calloc_large(total);
    }

    void* const p = alloc(total);
    if (p) {
        std::memset(p, 0, total);
    }
    return p;
}

void* realloc(void* ptr, std::size_t size) noexcept {
    if (!ptr) {
        return alloc(size);
    }
    if (size == 0) {
        free(ptr);
        return 0;
    }

    // 缩到一半以下才换块，免得小对象一直占着大块
    const std::size_t usable = usable_size(ptr);
    if (size <= usable && size >= usable / 2) {
        return ptr;
    }

    void* const moved = alloc(size);
    if (!moved) {
        return 0;
    }

    std::memcpy(moved, ptr, usable < size ? usable : size);
    free(ptr);
    return moved;
}

void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return 0;
    }
    if (alignment <= 16) {
        return alloc(size);
    }

    // 小块挑块大小是对齐倍数的那一档：页从 64 KiB 边界开始，档里每一块都对齐
    if (size <= detail::medium_max && alignment <= detail::medium_max) {
        int bin = detail::bin_of(size);
        while (detail::bin_size(bin) % alignment != 0) {
            bin += bin < 8 ? 2 : 1;
        }
        return detail::alloc_bin(bin);
    }

    return detail::alloc_large_block(size, alignment);
}

std::size_t usable_size(const void* ptr) noexcept {
    if (!ptr) {
        return 0;
    }
    if (!detail::in_map(detail::chunk_map, ptr)) {
        return detail::huge_usable_size(ptr);
    }

    const detail::located at = detail::locate(ptr);
    if (at.p->slices == 0 || !detail::block_start(at.p, at.offset)) {
        return 0;
    }
    if (at.p->block_size == 0) {
        return static_cast<std::size_t>(at.p->slices) << detail::slice_shift;
    }

    return at.p->block_size;
}

void collect() noexcept {
    detail::heap* const h = detail::t_heap;
    if (h != &detail::empty_heap) {
        detail::collect_heap(h);
    }

    detail::locked hold;
    detail::collect_abandoned_locked();
    detail::purge_locked();
}

} } // namespace wavvix::mem
