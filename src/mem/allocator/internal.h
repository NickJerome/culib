#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <atomic>
#include <mutex>
#include "mutex.h"

#include "compiler.h"
#include "os.h"
#include "random.h"
#include "mem_core.h"

/* ============================== 常量表 ================================ */
#define WAVVIX_MEM_SLICE_SIZE       ((size_t)64 * 1024)
#define WAVVIX_MEM_SLICE_SHIFT      16
#define WAVVIX_MEM_SMALL_MAX        ((size_t)8 * 1024)         /* small/large 分界    */
#define WAVVIX_MEM_HUGE_THRESHOLD   ((size_t)8 * 1024 * 1024)  /* 超过则 free 即归还 OS */
#define WAVVIX_MEM_BIN_COUNT        37                          /* 36 small bin + FULL */
#define WAVVIX_MEM_BIN_FULL         36                          /* 满页队列索引        */

/* 窗口保留失败时逐级减半。 */
#define WAVVIX_MEM_WINDOW_DEFAULT   ((size_t)1 << 40)          /* 1 TiB               */
#define WAVVIX_MEM_WINDOW_MIN       ((size_t)4 << 30)          /* 4 GiB               */
#define WAVVIX_MEM_ARENA_MIN        ((size_t)2 << 20)          /* 2 MiB：描述符提交粒度 4 KiB 的最小单位 */
#define WAVVIX_MEM_ARENA_FIRST      ((size_t)64 << 20)
#define WAVVIX_MEM_ARENA_MAX        ((size_t)1 << 30)
#define WAVVIX_MEM_ARENA_COUNT_MAX  1024

#define WAVVIX_MEM_PAGE_MAX_CANDIDATES 4
#define WAVVIX_MEM_PURGE_DELAY_MS   1000
#define WAVVIX_MEM_PAGE_MAGIC       0xED0CAFE0u

/* 页种类 */
#define WAVVIX_MEM_PAGE_SMALL 0
#define WAVVIX_MEM_PAGE_LARGE 1
#define WAVVIX_MEM_PAGE_CONT  2                                /* 多 slice 页的续页项 */

/* mem_page_t.flags 位 */
#define WAVVIX_MEM_PAGE_FLAG_IN_FULL 0x01   /* 当前挂在 pages[WAVVIX_MEM_BIN_FULL] 队列 */
#define WAVVIX_MEM_PAGE_FLAG_HUGE    0x02   /* free 即 decommit 归还物理页       */

/* L1 哨兵：已分配块的首 8 字节被写入 encode(WAVVIX_MEM_BLOCK_ALLOCATED)。
 * 该值非 0、不可能是页内地址（页地址均 64KiB 对齐且远大于它）。 */
#define WAVVIX_MEM_BLOCK_ALLOCATED ((uintptr_t)0x1ED0C0DE1u)

namespace wavvix { namespace mem { namespace detail {

/* ============================== 尺寸分级 ============================== */

/* size -> bin（2..36）。wsize-- 不可省略：2 的幂必须精确落档。
 * 线性区取偶：块大小恒为 16 的倍数（x86-64/aarch64 ABI 要求 malloc 结果
 * 16 字节对齐，与 mimalloc 的 MI_ALIGN2W 一致）；bin 1/3/5/7 闲置。 */
static inline int mem_bin(size_t size) {
  size_t wsize = (size + 7) >> 3;
  if (wsize <= 8) return (int)((wsize + 1) & ~(size_t)1);
  wsize--;
  int b = 63 - mem_clz64(wsize);
  return (b << 2) + (int)((wsize >> (b - 2)) & 3) - 3;
}

/* bin -> 块大小 */
static inline size_t mem_bin_size(int bin) {
  if (bin <= 8) return (size_t)bin * 8;
  int b = (bin + 3) >> 2;
  int q = (bin + 3) & 3;
  return ((size_t)(5 + q)) << (b + 1);
}

/* ============================== 结构体 ================================ */

struct mem_page_t;
struct mem_arena_t;

/* --- diag.cpp：无分配的致命错误报告（内联快路径需要，故提前声明） --- */
WAVVIX_MEM_NORETURN void mem_fatal_ptr(const char* msg, const void* p);

/* 空闲块首 8 字节存编码后的 next 指针 */
struct mem_block_t { uintptr_t encoded_next; };

/* 页队列：双向链表 */
struct mem_page_queue_t {
  mem_page_t* first;
  mem_page_t* last;
};

/* 每个 64KiB slice 对应一个两条 cache line 的描述符。
 * CONT 发布所写的 kind/cont_first/magic 集中在第一条 cache line。 */
struct mem_page_t {
  /* ---- cache line 0：分配/释放热路径 + CONT 续页项 ---- */
  std::atomic<uintptr_t> owner_id;    /* 所属线程编号；0 表示无主 */
  mem_block_t*            free;        /* 立即可分配空闲链（仅 owner） */
  mem_block_t*            local_free;  /* 本线程延迟释放链（仅 owner） */
  uint32_t               used;        /* 已分配块数（含 xthread_free 中的块） */
  uint16_t               capacity;    /* 总块数 */
  uint16_t               block_size;  /* 块字节数（small） */
  uint8_t                kind;        /* SMALL / LARGE / CONT */
  uint8_t                flags;
  uint8_t                bin;         /* small bin（1..36）；large 为 0 */
  std::atomic<uint8_t>   has_aligned; /* 一旦置位，本页复用前不再清零 */
  uint16_t               arena_idx;   /* 所属 arena 下标（slice 的固有属性） */
  uint8_t*               slice_start; /* 页首地址（slice 起始，64KiB 对齐） */
  uint32_t               slice_count; /* 连续 slice 数（small=1） */
  uint32_t               cont_first;  /* CONT 项：首 slice 的窗口下标 */
  std::atomic<uint32_t>  magic;       /* WAVVIX_MEM_PAGE_MAGIC（L0 校验 + 发布屏障） */

  /* ---- cache line 1：跨线程、队列、冷字段 ---- */
  std::atomic<uintptr_t> xthread_free; /* 他线程释放链头（裸指针，0=空） */
  mem_page_t*             next;
  mem_page_t*             prev;
  uintptr_t              key;         /* L1：空闲链 XOR 密钥 */
  uint8_t                reserved[32]; // 描述符数组保持 128 字节步长。
};
static_assert(sizeof(mem_page_t) == 128, "mem_page_t must be exactly two cache lines");
static_assert(ATOMIC_CHAR_LOCK_FREE == 2, "page alignment marker must be lock-free");
/* CONT 续页项的三个域必须同处 line 0——这是上面那条布局决策的机器可检查形式 */
static_assert(offsetof(mem_page_t, kind) < 64 && offsetof(mem_page_t, cont_first) < 64 &&
                  offsetof(mem_page_t, magic) < 64,
              "kind/cont_first/magic must share cache line 0");

/* --- diag.cpp：带页上下文的致命错误报告 --- */
WAVVIX_MEM_NORETURN void mem_fatal_page(const char* msg, const void* p, const mem_page_t* page);

/* arena：窗口内的一段连续 slice，用位图管理。 */
struct mem_arena_t {
  uint8_t*    base;                 /* = mem_window_base + first_slice*64KiB */
  size_t      slice_count;
  size_t      first_slice;          /* 在窗口/描述符数组中的起始下标 */
  struct mem_bitmap_t*  slices_free;      /* 空闲 slice（bitmap.h 定义） */
  struct mem_bitmap_t*  slices_committed;
  std::atomic<int64_t> purge_expire;     /* ms 时间戳，0=无待 purge */
  uint16_t    index;                /* 自身在 g_arenas[] 中的下标 */
  allocator_mutex  mutex;                /* arena 唯一锁 */
};

/* 地址窗口与平铺描述符数组由 arena.cpp 定义。 */
extern uint8_t*            mem_window_base;
extern size_t              mem_window_size;
extern mem_page_t*          mem_desc_base;
extern std::atomic<size_t> mem_window_used_slices;   /* 已切分给 arena 的 slice 数 */
extern std::atomic<size_t> mem_desc_constructed_slices; /* 已构造描述符的连续前缀 */

static inline mem_page_t* mem_desc_at(size_t idx) {
  return (mem_page_t*)((uint8_t*)mem_desc_base + idx * sizeof(mem_page_t));
}

/* CONT 项可能过时；跳转后须复核首页仍活、不是 CONT 且覆盖原 slice。
 * 顺序与原因见 docs/mem-internals.md。 */
static inline mem_page_t* mem_ptr_page(const void* p) {
  uintptr_t off = (uintptr_t)p - (uintptr_t)mem_window_base;   /* p < base 回绕为巨大值 */
  size_t idx = off >> WAVVIX_MEM_SLICE_SHIFT;
  if (WAVVIX_MEM_UNLIKELY(idx >= mem_desc_constructed_slices.load(std::memory_order_acquire)))
    return NULL;
  mem_page_t* d = mem_desc_at(idx);
  if (WAVVIX_MEM_UNLIKELY(d->magic.load(std::memory_order_acquire) != WAVVIX_MEM_PAGE_MAGIC)) return NULL;
  if (WAVVIX_MEM_UNLIKELY(d->kind == WAVVIX_MEM_PAGE_CONT)) {
    /* 陈旧 CONT 不能直接信任 cont_first。 */
    size_t first = d->cont_first;
    /* 合法续页项的首页必须在它之前；先定界再解引用。 */
    if (WAVVIX_MEM_UNLIKELY(first >= idx)) return NULL;
    d = mem_desc_at(first);
    if (WAVVIX_MEM_UNLIKELY(d->magic.load(std::memory_order_acquire) != WAVVIX_MEM_PAGE_MAGIC)) return NULL;
    /* 首页若也变成 CONT，旧 slice_count 可能仍在。 */
    if (WAVVIX_MEM_UNLIKELY(d->kind == WAVVIX_MEM_PAGE_CONT)) return NULL;
    if (WAVVIX_MEM_UNLIKELY(idx - first >= d->slice_count)) return NULL;
  }
  return d;
}

/* 描述符指针 -> 窗口 slice 下标（纯算术） */
static inline size_t mem_page_slice_index(const mem_page_t* page) {
  return ((uintptr_t)page - (uintptr_t)mem_desc_base) / sizeof(mem_page_t);
}

/* ======================== 空闲链 XOR 编码（L1） ======================== */
static inline uintptr_t mem_encode(const mem_page_t* page, const void* p) {
  return (uintptr_t)p ^ page->key;
}
static inline mem_block_t* mem_decode(const mem_page_t* page, uintptr_t v) {
  return (mem_block_t*)(v ^ page->key);
}
static inline mem_block_t* mem_block_next(const mem_page_t* page, const mem_block_t* b) {
  return mem_decode(page, b->encoded_next);
}
static inline void mem_block_set_next(const mem_page_t* page, mem_block_t* b,
                                     const mem_block_t* next) {
  b->encoded_next = mem_encode(page, next);
}

/* L1 double-free 编码哨兵：块被释放后其 next 域为编码态；
 * 同一块再次释放时解码值必为 0 或页内地址 → 非法。
 * 已分配块由 pop 时写入的 encode(WAVVIX_MEM_BLOCK_ALLOCATED) 标记（解码为常量，通过）。
 * 用户数据解码为随机 64 位值，误判概率 ≈ 页大小/2^64。 */
static inline void mem_secure_check_free(const mem_page_t* page, const void* p) {
  uintptr_t v = ((const mem_block_t*)p)->encoded_next ^ page->key;
  if (v == WAVVIX_MEM_BLOCK_ALLOCATED) return;       /* 正常已分配块 */
  if (v == 0 ||
      v - (uintptr_t)page->slice_start <
          (uintptr_t)page->slice_count * WAVVIX_MEM_SLICE_SIZE) {
    mem_fatal_ptr("double free detected (encoded sentinel)", p);
  }
  /* 其余情况视为用户数据，放行 */
}

#if defined(WAVVIX_MEM_DEBUG)
/* L2 在 tcache 侧的填充起点：块首两个机器字是 tcache 自己的元数据
 * （w[0] = 编码后的链指针，w[1] = 驻留 guard），只能从其后开始填与校。
 * 与页空闲链那套（从 WAVVIX_MEM_L2_LINK 起、保留尾部 WAVVIX_MEM_L2_TAIL）区间不同，
 * 理由见 secure.cpp 的 mem_debug_verify_tcache_block。
 * 校验由 theap.h 的缓存读取路径调用。 */
#define WAVVIX_MEM_L2_TCACHE_META (2 * sizeof(uintptr_t))
void mem_debug_verify_tcache_block(const void* block, size_t block_size);
#endif

/* 进程级驻留标记供跨线程重复释放检查；权衡见 docs/mem-internals.md。 */
extern uintptr_t mem_tcache_guard;

/* release 对齐块在用户指针前保存定位标记；调试构建使用头 canary。 */
static inline uintptr_t mem_aligned_cookie(const void* user) {
  return mem_tcache_guard ^ (uintptr_t)user;
}

/* 对齐分配可能返回块内指针；找回块首统一走 mem_user_block_start。 */

/* 只对 HAS_ALIGNED 页使用：它会把任何块内地址归整为块首。 */
static inline uint8_t* mem_small_block_start(const mem_page_t* page, const void* p) {
  size_t off = (size_t)((const uint8_t*)p - page->slice_start);
  return page->slice_start + (off / page->block_size) * page->block_size;
}

/* ======================== 页内快路径（≤10 指令目标） =================== */
#if defined(WAVVIX_MEM_DEBUG)
/* L2 普通块：编码域、头 canary、用户区、尾 canary；布局见 docs/mem-internals.md。 */
#define WAVVIX_MEM_L2_LINK      8                            /* 空闲链编码域，fill/verify 跳过 */
#define WAVVIX_MEM_L2_CANARY    8                            /* 单个 canary 的字节数 */
#define WAVVIX_MEM_L2_HEAD     (WAVVIX_MEM_L2_LINK + WAVVIX_MEM_L2_CANARY)   /* = 16：用户指针相对块首的偏移 */
#define WAVVIX_MEM_L2_TAIL      WAVVIX_MEM_L2_CANARY
#define WAVVIX_MEM_L2_OVERHEAD (WAVVIX_MEM_L2_HEAD + WAVVIX_MEM_L2_TAIL)
/* 对齐块的头 canary 始终紧贴 user；普通块是固定偏移的特例。 */
void*  mem_l2_wrap(void* block);
void*  mem_l2_wrap_at(void* block, void* user);
void*  mem_l2_unwrap(void* user);
void   mem_debug_verify_free_block(const mem_page_t* page, const void* block);
#endif

/* 用户指针 → 块首。wavvix::mem::free / usable_size / mem_l2_unwrap
 * 三处共用的唯一口径（上面两个分量在这里汇合）。 */
static inline uint8_t* mem_user_block_start(const mem_page_t* page, const void* p) {
  if (page->kind == WAVVIX_MEM_PAGE_LARGE) return page->slice_start;
  if (WAVVIX_MEM_UNLIKELY(page->has_aligned.load(std::memory_order_relaxed) != 0))
    return mem_small_block_start(page, p);
#if defined(WAVVIX_MEM_DEBUG)
  return (uint8_t*)((uintptr_t)p - WAVVIX_MEM_L2_HEAD);   /* 普通块：用户指针在块首之后 */
#else
  return (uint8_t*)(uintptr_t)p;                  /* 普通块：用户指针就是块首 */
#endif
}

static inline void* mem_page_malloc(mem_page_t* page) {
  mem_block_t* b = page->free;
  if (WAVVIX_MEM_UNLIKELY(b == NULL)) return NULL;
  page->free = mem_block_next(page, b);              /* L1: 解 XOR */
#if defined(WAVVIX_MEM_DEBUG)
  mem_debug_verify_free_block(page, b);              /* L2: UAF 写校验 */
#endif
  b->encoded_next = mem_encode(page, (const void*)WAVVIX_MEM_BLOCK_ALLOCATED);
  page->used++;
  return b;
}

/* ============================ 模块接口 ================================ */

/* --- page.cpp --- */
/* 先 init 并填好全部字段，最后 publish 点亮 magic。 */
void mem_page_init(mem_page_t* page, uintptr_t owner, uint64_t& random_state, int bin,
                  uint8_t* slice_start, uint32_t slice_count);
static inline void mem_page_publish(mem_page_t* page) {
  page->magic.store(WAVVIX_MEM_PAGE_MAGIC, std::memory_order_release);
}
bool mem_page_free_collect(mem_page_t* page);         /* 返回是否腾出空间 */
void mem_page_release_slices(mem_page_t* page);       /* magic 清零 + 归还 slice */
void mem_page_queue_push(mem_page_queue_t* q, mem_page_t* page);
void mem_page_queue_remove(mem_page_queue_t* q, mem_page_t* page);
#if defined(WAVVIX_MEM_DEBUG)
/* 页级不变式自检（WAVVIX_MEM_DEBUG）：见 page.cpp。违反即 fatal，不返回错误码——
 * 到这一步页的记账已经坏了，继续跑只会把现场冲掉。 */
void mem_page_check_invariants(const mem_page_t* page);
#endif

/* --- arena.cpp --- */
bool        mem_window_init();                      /* 保留窗口 + 描述符数组 */
mem_arena_t* mem_arena_get(int idx);
int         mem_arena_count(void);
int         mem_arena_pick();
/* 分配 n 个连续 slice，返回 arena 内下标（失败 (size_t)-1）。
 * align_slices：返回地址须按 align_slices*64KiB 对齐（1 = 无额外约束）。
 * out_zeroed 可为空；否则报告 slice 内容是否保证全零（fresh 或 purge 后）。 */
size_t      mem_arena_alloc_slices(mem_arena_t* a, size_t n, size_t align_slices,
                                  bool* out_zeroed);
size_t      mem_arena_alloc_slices_any(int& preferred_arena, size_t n, size_t align_slices,
                                      mem_arena_t** out_arena, bool* out_zeroed);
// immediate 为 true 时立即尝试归还物理页，否则延迟 purge。
void        mem_arena_free_slices(mem_arena_t* a, size_t idx, size_t n, bool immediate);
void        mem_arena_purge_check(mem_arena_t* a);   /* 到期才 purge */
void        mem_arena_purge_force(mem_arena_t* a);   /* wavvix::mem::collect 用 */
size_t      mem_arena_committed_bytes(void);
/* 描述符平铺数组的维护：CONT 项登记 / 整段 magic 清零 */
void        mem_desc_mark_cont(size_t first_idx, size_t n);
void        mem_desc_clear(size_t first_idx);
/* fork 钩子支撑 */
void        mem_arena_lock_all(void);
void        mem_arena_unlock_all(void);
void        mem_arena_reinit_locks_child(void);

/* --- runtime.cpp --- */
uint64_t mem_global_seed(void);
void        mem_init_once(void);
uintptr_t   mem_next_thread_id(void);                /* 全局自增的非零线程编号 */

/* --- secure.cpp --- */
/* L2 声明（mem_l2_wrap / mem_l2_unwrap / mem_debug_verify_free_block）见上文
 * "页内快路径"一节——mem_page_malloc 内联需要先行声明。 */

/* 块的可用字节数；大块容量由 slice 数直接算出。 */
static inline size_t mem_page_usable_size(const mem_page_t* page) {
  if (page->kind == WAVVIX_MEM_PAGE_SMALL) return page->block_size;
  return (size_t)page->slice_count * WAVVIX_MEM_SLICE_SIZE;
}

} } } // namespace wavvix::mem::detail
