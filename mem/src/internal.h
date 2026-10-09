/* ----------------------------------------------------------------------------
 * internal.h — EduMalloc 内部公共头：常量、结构体、内联快路径
 *
 * 常量表与 SPEC.md §1 严格一致；结构体字段与 SPEC.md §3 / 设计骨架 §3 一致。
 * 本头文件禁止出现平台宏（平台差异全部收敛在 os.h/os.cpp）。
 * ------------------------------------------------------------------------- */
#ifndef EDUMALLOC_INTERNAL_H
#define EDUMALLOC_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <atomic>
#include <mutex>

#include "compiler.h"
#include "os.h"
#include "random.h"
#include "culib/detail/allocator_c.h"

/* ============================== 常量表（SPEC §1） ======================== */
#define ED_PAGE_SIZE        ((size_t)64 * 1024)        /* page = slice        */
#define ED_SLICE_SIZE       ((size_t)64 * 1024)
#define ED_SLICE_SHIFT      16
#define ED_SMALL_MAX        ((size_t)8 * 1024)         /* small/large 分界    */
#define ED_HUGE_THRESHOLD   ((size_t)8 * 1024 * 1024)  /* 超过则 free 即归还 OS */
#define ED_BIN_COUNT        37                          /* 36 small bin + FULL */
#define ED_BIN_FULL         36                          /* 满页队列索引        */
#define ED_DIRECT_MAX       25                          /* direct 直达 bin 1..24 */

/* 地址窗口：所有 arena 都从这一片连续保留区里切分，描述符是与之平铺的数组。
 * 窗口保留失败时逐级减半，下限 4 GiB（低于此无法支撑教学级工作负载）。 */
#define ED_WINDOW_DEFAULT   ((size_t)1 << 40)          /* 1 TiB               */
#define ED_WINDOW_MIN       ((size_t)4 << 30)          /* 4 GiB               */
#define ED_ARENA_MIN        ((size_t)2 << 20)          /* 2 MiB：描述符提交粒度 4 KiB 的最小单位 */
#define ED_ARENA_FIRST      ((size_t)64 << 20)
#define ED_ARENA_MAX        ((size_t)1 << 30)
#define ED_ARENA_COUNT_MAX  1024

#define ED_META_CHUNK       ((size_t)2 * 1024 * 1024)
#define ED_PAGE_MAX_CANDIDATES 4
#define ED_RETIRE_EXPIRE    4
#define ED_PURGE_DELAY_MS   1000
#define ED_PAGE_MAGIC       0xED0CAFE0u

/* 页种类 */
#define ED_PAGE_SMALL 0
#define ED_PAGE_LARGE 1
#define ED_PAGE_CONT  2                                /* 多 slice 页的续页项 */

/* ed_page_t.flags 位 */
#define ED_PAGE_FLAG_IN_FULL 0x01   /* 当前挂在 pages[ED_BIN_FULL] 队列 */
#define ED_PAGE_FLAG_HUGE    0x02   /* free 即 decommit 归还物理页       */
#define ED_PAGE_FLAG_GUARDED 0x04   /* L3 守护页分配                     */
#define ED_PAGE_FLAG_HAS_ALIGNED 0x08 /* 页内曾切出过对齐块：free 需按块网格找回块首 */
#define ED_PAGE_FLAG_PRIVATE 0x10   /* 私有堆页：不进 tcache（保 heap_destroy 语义） */

/* xthread_id 低位标志 */
#define ED_PAGE_XTHREAD_ABANDONED ((uintptr_t)0x1)

/* L1 哨兵：已分配块的首 8 字节被写入 encode(ED_BLOCK_ALLOCATED)。
 * 该值非 0、不可能是页内地址（页地址均 64KiB 对齐且远大于它）。 */
#define ED_BLOCK_ALLOCATED ((uintptr_t)0x1ED0C0DE1u)

/* ============================== 尺寸分级（SPEC §2） ===================== */

/* size -> bin（2..36）。wsize-- 不可省略：2 的幂必须精确落档。
 * 线性区取偶：块大小恒为 16 的倍数（x86-64/aarch64 ABI 要求 malloc 结果
 * 16 字节对齐，与 mimalloc 的 MI_ALIGN2W 一致）；bin 1/3/5/7 闲置。 */
static inline int ed_bin(size_t size) {
  size_t wsize = (size + 7) >> 3;
  if (wsize <= 8) return (int)((wsize + 1) & ~(size_t)1);
  wsize--;
  int b = 63 - ed_clz64(wsize);
  return (b << 2) + (int)((wsize >> (b - 2)) & 3) - 3;
}

/* bin -> 块大小 */
static inline size_t ed_bin_size(int bin) {
  if (bin <= 8) return (size_t)bin * 8;
  int b = (bin + 3) >> 2;
  int q = (bin + 3) & 3;
  return ((size_t)(5 + q)) << (b + 1);
}

/* ============================== 结构体（SPEC §3） ======================= */

struct ed_page_s;
struct ed_theap_s;
struct ed_arena_s;
typedef struct ed_page_s  ed_page_t;
typedef struct ed_theap_s ed_theap_t;
typedef struct ed_arena_s ed_arena_t;

/* --- diag.cpp：无分配的致命错误报告（内联快路径需要，故提前声明） --- */
ED_NORETURN void ed_fatal_ptr(const char* msg, const void* p);

/* 空闲块首 8 字节存编码后的 next 指针 */
struct ed_block_t { uintptr_t encoded_next; };
typedef struct ed_block_t ed_block_t;

/* 页队列：双向链表 */
struct ed_page_queue_t {
  ed_page_t* first;
  ed_page_t* last;
  size_t     count;
  size_t     block_size;   /* 本队列对应 bin 的块大小（0 = FULL 队列） */
};
typedef struct ed_page_queue_t ed_page_queue_t;

/* 页描述符：恰两条 cache line，与地址窗口的 slice 一一平铺
 * （slice i 的描述符恒为 ed_desc_base[i]，不再有描述符池与索引表）。
 *
 * 字段分组偏离设计稿一处：magic 放在 cache line 0（设计稿放在 line 1），
 * large_size 换到 line 1。理由与实测：
 *   - CONT 续页项只写 kind / cont_first / magic 三个域。三者同在 line 0 时，
 *     每个续页项只脏一条 cache line；分处两条 line 时脏两条。large 路径每次
 *     alloc+free 要走 2×slice_count 个续页项，这一项是纯放大的内存写流量。
 *   - 7 轮配对实测（bench_large，log-uniform 64KiB..4MiB，平均 ~15 slice/块）：
 *     中位数 +5.3%；bench_single/larson/xthread 的中位数都落在各自噪声带内
 *     （该机跨轮漂移 ±8%~±12%），即"large 受益、small 不受损"。
 *   - large_size 只在 usable_size 与 large free 上被读，挪到 line 1 无代价。 */
struct ed_page_s {
  /* ---- cache line 0：分配/释放热路径 + CONT 续页项 ---- */
  std::atomic<uintptr_t> xthread_id;  /* owner 线程 id(左移3位) | 低3位标志 */
  ed_block_t*            free;        /* 立即可分配空闲链（仅 owner） */
  ed_block_t*            local_free;  /* 本线程延迟释放链（仅 owner） */
  uint32_t               used;        /* 已分配块数（含 xthread_free 中的块） */
  uint16_t               capacity;    /* 总块数 */
  uint16_t               block_size;  /* 块字节数（small） */
  uint8_t                kind;        /* SMALL / LARGE / CONT */
  uint8_t                retire_expire; /* 退休宽限计数（心跳） */
  bool                   free_is_zero;  /* 空闲块内容已清零 */
  uint8_t                flags;
  uint8_t                bin;         /* small bin（1..36）；large 为 0 */
  uint8_t                _pad0;
  uint16_t               arena_idx;   /* 所属 arena 下标（slice 的固有属性） */
  uint8_t*               slice_start; /* 页首地址（slice 起始，64KiB 对齐） */
  uint32_t               slice_count; /* 连续 slice 数（small=1） */
  uint32_t               cont_first;  /* CONT 项：首 slice 的窗口下标 */
  std::atomic<uint32_t>  magic;       /* ED_PAGE_MAGIC（L0 校验 + 发布屏障） */
  uint8_t                _pad1[4];

  /* ---- cache line 1：跨线程、队列、冷字段 ---- */
  std::atomic<uintptr_t> xthread_free; /* 他线程释放链头（裸指针，0=空） */
  ed_theap_t*            theap;        /* owner theap（abandoned 页为 NULL） */
  ed_page_t*             next;
  ed_page_t*             prev;
  uintptr_t              keys[2];      /* L1：空闲链 XOR 密钥 */
  size_t                 large_size;   /* large/huge/guarded：可用字节数 */
  uint8_t                _pad2[8];
};
static_assert(sizeof(ed_page_t) == 128, "ed_page_t must be exactly two cache lines");
/* CONT 续页项的三个域必须同处 line 0——这是上面那条布局决策的机器可检查形式 */
static_assert(offsetof(ed_page_t, kind) < 64 && offsetof(ed_page_t, cont_first) < 64 &&
                  offsetof(ed_page_t, magic) < 64,
              "kind/cont_first/magic must share cache line 0");

/* --- diag.cpp：带页上下文的致命错误报告 --- */
ED_NORETURN void ed_fatal_page(const char* msg, const void* p, const ed_page_t* page);

/* 线程本地堆（thread_local 持有） */
/* tcache：线程本地小块缓存（glibc 式；release 与 ED_DEBUG 均启用）。
 * 块在 tcache 中时页视角仍为"已分配"（used 不变），延迟批量归还。
 * L1 一致性：链指针按线程密钥 XOR 编码；guard 域 = 进程级密钥 ^ 块地址。
 * guard 必须是进程级而非线程级：块驻留 A 线程 tcache 时，B 线程再次
 * free 同一块（跨线程 double-free）必须能算出同样的标记才检得出来。 */
struct ed_tcache_bin_t {
  void*    head;
  uint32_t count;
  uint32_t _pad;
};

struct ed_theap_s {
  ed_page_t*      pages_free_direct[ED_DIRECT_MAX]; /* 索引 1..24 */
  ed_page_queue_t pages[ED_BIN_COUNT];              /* 0..35 每 bin + [ED_BIN_FULL] */
  uint64_t        heartbeat;
  uint64_t        random_state;                     /* splitmix64 */
  uintptr_t       thread_id;                        /* 唯一 id（非 0，低3位为 0） */
  ed_heap_t*      heap;                             /* 所属一等堆 */
  int             arena_idx;                        /* 当前偏好 arena */
  uint32_t        full_count[ED_BIN_COUNT];         /* 每 bin 在 FULL 队列中的页数 */
  uint64_t        guarded_countdown;                /* L3：按字节倒数采样 */
  ed_page_t*      large_list;                       /* 私有堆的 large 页链表 */
  /* tcache（见上方注释）；tcache_key 在 theap 创建时随机化 */
  ed_tcache_bin_t tcache[ED_BIN_FULL];                /* 索引 bin-1，small bin 1..36 */
  uintptr_t       tcache_key;
  /* 统计（诊断族用，见 internal.h 的 ed_stat_alloc/ed_stat_free）。
   * 四个计数器全部单调递增，没有"current"这种可增可减的量——跨线程释放
   * 天然会让"本线程当前字节"变成负数，用无符号存就是下溢（历史缺陷：
   * ed_stats_print 曾打出 current=18446744073705882174）。存两个方向的
   * 累计量，把"是否可能为负"这件事留给 ed_stats_get 显式处理。
   * 按 alloc/free 成对相邻排列：一次 malloc 只脏前 16 字节、一次 free 只脏
   * 后 16 字节，四者同处一条 cache line。 */
  uint64_t        stat_alloc_count;
  uint64_t        stat_alloc_bytes;
  uint64_t        stat_free_count;
  uint64_t        stat_free_bytes;
  struct ed_theap_s* next_free;                     /* theap 回收链 */
};

/* 一等堆：内嵌一份"伪 theap"承载页队列；私有堆分配路径整体串行化（堆级锁），
 * 默认堆快路径走 thread_local theap、完全无锁（SPEC §3 允许的最小实现）。 */
struct ed_heap_s {
  ed_theap_t  theap;      /* 页队列 + 伪 owner id */
  std::mutex  mutex;      /* 私有堆分配串行化 */
  bool        is_default;
};

/* arena：窗口内的一段连续 slice，用位图管理（SPEC §3） */
struct ed_arena_s {
  uint8_t*    base;                 /* = ed_window_base + first_slice*64KiB */
  size_t      slice_count;
  size_t      first_slice;          /* 在窗口/描述符数组中的起始下标 */
  struct ed_bbitmap_t* slices_free;      /* 空闲 slice（bitmap.h 定义） */
  struct ed_bitmap_t*  slices_committed;
  struct ed_bitmap_t*  slices_dirty;     /* 内容脏（再分配需清 free_is_zero） */
  struct ed_bitmap_t*  slices_purge;     /* 待归还 OS */
  std::atomic<int64_t> purge_expire;     /* ms 时间戳，0=无待 purge */
  uint16_t    index;                /* 自身在 g_arenas[] 中的下标 */
  std::mutex  mutex;                /* arena 唯一锁 */
  ed_page_t*  abandoned[ED_BIN_COUNT];  /* abandoned 页桶：bin b -> [b-1] */
};

/* 运行时选项（heap.cpp，启动解析一次，之后只读） */
struct ed_options_t {
  long    purge_delay_ms;        /* EDUMALLOC_PURGE_DELAY        默认 1000 */
  size_t  window_size;           /* EDUMALLOC_WINDOW_SIZE        默认 1TiB */
  size_t  arena_reserve;         /* EDUMALLOC_ARENA_RESERVE      默认 64MiB */
  long    secure;                /* EDUMALLOC_SECURE             默认 1    */
  long    guarded_sample_rate;   /* EDUMALLOC_GUARDED_SAMPLE_RATE 默认 0   */
  long    eager_commit;          /* EDUMALLOC_EAGER_COMMIT       默认 0    */
};
extern ed_options_t ed_options;

/* ===================== 地址窗口 + 平铺描述符数组（arena.cpp 定义） =======
 * 窗口是一整片 PROT_NONE 保留区；描述符数组是与之等长（每 slice 一项）的
 * 第二片保留区，按 arena 粒度提交。两者都只增不减，故 ed_ptr_page 是
 * 「一次减法 + 一次移位 + 一次数组访问」，没有查表间接层。 */
extern uint8_t*            ed_window_base;
extern size_t              ed_window_size;
extern ed_page_t*          ed_desc_base;
extern std::atomic<size_t> ed_window_used_slices;   /* 已切分给 arena 的 slice 数 */

/* free 的第一步：窗口内定位描述符。无锁只读；窗口外/未登记返回 NULL。
 *
 * 依赖的不变式：对任一 slice 下标 j，至多有一个活页覆盖它。由两件事保证——
 *   (i)  arena 位图的排他性：一段 slice 同时只属于一个页；
 *   (ii) 登记顺序：ed_desc_mark_cont 写完全部续页项，才 ed_page_publish 点亮首项。
 *
 * 难点在于续页项在页归还时**不清**（见 arena.cpp 的 ed_desc_clear），所以
 * desc[idx] 可能是一个陈旧的 CONT 项。下面三条复核缺一不可，按"跳转目标
 * desc[first] 处于什么状态"枚举，每条各挡一格：
 *
 *   目标状态                       挡它的是
 *   ── 已释放（magic 灭）           magic 复核
 *   ── 被征用为别的页的续页项       kind != CONT 复核  ← 最易漏掉的一格
 *   ── 活页，但并不覆盖 idx         区间复核
 *   ── 活页，且确实覆盖 idx         全部通过，这正是要找的页
 *
 * 第二条为什么不能省：ed_desc_mark_cont 只写 kind/cont_first/magic，**不写
 * slice_count**。因此一个"曾当过首项、现被征用成别人续页项"的描述符，会带着
 * 上一辈子的 slice_count 亮着 magic 站在那里；只查第一、三条的话，区间复核读到
 * 的是陈旧值，会把这个 CONT 项当页返回——后果是悬垂 free 被静默接受、slice
 * 重复归还，最终两次分配返回重叠内存。
 *
 * 区间复核写成无符号减法：idx < first（陈旧项指向更高地址）会回绕成巨大值，
 * 同样被 >= slice_count 挡下，一条比较覆盖两个方向。 */
static inline ed_page_t* ed_ptr_page(const void* p) {
  uintptr_t off = (uintptr_t)p - (uintptr_t)ed_window_base;   /* p < base 回绕为巨大值 */
  size_t idx = off >> ED_SLICE_SHIFT;
  if (ED_UNLIKELY(idx >= ed_window_used_slices.load(std::memory_order_acquire)))
    return NULL;
  ed_page_t* d = ed_desc_base + idx;
  if (ED_UNLIKELY(d->magic.load(std::memory_order_acquire) != ED_PAGE_MAGIC)) return NULL;
  if (ED_UNLIKELY(d->kind == ED_PAGE_CONT)) {
    /* 续页项在页归还时不清（见 arena.cpp 的 ed_desc_clear），因此这里必须复核：
     * 首项仍活，且本 slice 确实落在它的 [first, first+slice_count) 里。 */
    size_t first = d->cont_first;
    /* 先给 first 定界再解引用：idx 有 used_slices 把关，first 却是从描述符里
     * 读出来的裸值。合法续页项恒满足 first < idx（ed_desc_mark_cont 只写
     * [first+1, first+n)），所以这条比较既拦住了损坏值，也让下面这次数组访问
     * 可证明在界内——原先的区间复核发生在解引用之后，救不了越界读本身。 */
    if (ED_UNLIKELY(first >= idx)) return NULL;
    d = ed_desc_base + first;
    if (ED_UNLIKELY(d->magic.load(std::memory_order_acquire) != ED_PAGE_MAGIC)) return NULL;
    /* 跳转目标必须是真页：ed_desc_mark_cont 不写 slice_count，所以一个"曾当过首项、
     * 现被征用成别人续页项"的描述符会带着上一辈子的 slice_count 亮着 magic。
     * 少了这一条，下面的区间复核就会读到陈旧值而放行，把 CONT 项当页返回。 */
    if (ED_UNLIKELY(d->kind == ED_PAGE_CONT)) return NULL;
    if (ED_UNLIKELY(idx - first >= d->slice_count)) return NULL;
  }
  return d;
}

/* 描述符指针 -> 窗口 slice 下标（纯算术） */
static inline size_t ed_page_slice_index(const ed_page_t* page) {
  return (size_t)(page - ed_desc_base);
}

/* ======================== 空闲链 XOR 编码（L1） ======================== */
/* secure 关闭时 keys[0]==0，编码退化为恒等——同一代码路径，零分支。 */
static inline uintptr_t ed_encode(const ed_page_t* page, const void* p) {
  return (uintptr_t)p ^ page->keys[0];
}
static inline ed_block_t* ed_decode(const ed_page_t* page, uintptr_t v) {
  return (ed_block_t*)(v ^ page->keys[0]);
}
static inline ed_block_t* ed_block_next(const ed_page_t* page, const ed_block_t* b) {
  return ed_decode(page, b->encoded_next);
}
static inline void ed_block_set_next(const ed_page_t* page, ed_block_t* b,
                                     const ed_block_t* next) {
  b->encoded_next = ed_encode(page, next);
}

/* L1 double-free 编码哨兵：块被释放后其 next 域为编码态；
 * 同一块再次释放时解码值必为 0 或页内地址 → 非法。
 * 已分配块由 pop 时写入的 encode(ED_BLOCK_ALLOCATED) 标记（解码为常量，通过）。
 * 用户数据解码为随机 64 位值，误判概率 ≈ 页大小/2^64。 */
static inline void ed_secure_check_free(const ed_page_t* page, const void* p) {
  if (page->keys[0] == 0) return;            /* secure 关闭：无哨兵 */
  uintptr_t v = ((const ed_block_t*)p)->encoded_next ^ page->keys[0];
  if (v == ED_BLOCK_ALLOCATED) return;       /* 正常已分配块 */
  if (v == 0 ||
      v - (uintptr_t)page->slice_start <
          (uintptr_t)page->slice_count * ED_SLICE_SIZE) {
    ed_fatal_ptr("double free detected (encoded sentinel)", p);
  }
  /* 其余情况视为用户数据，放行 */
}

/* ============================ tcache 快路径 ============================ */
/* 每 bin 容量 = 固定字节预算 / 块大小，再夹到 [ED_TCACHE_MIN, ED_TCACHE_MAX]。
 * 按"条数"分档（旧实现：128/64/32 条）会让滞留字节随块大小线性放大：8KiB 的
 * bin 光自己就扣住 32 x 8KiB = 256KiB，36 个 bin 全满合计 1908KiB/线程，线程
 * 一多这就是纯粹的内存占用。按字节预算分档后，中段每个 bin 的滞留量钉在
 * ~16KiB，全满合计 448KiB/线程（单 bin 上界 32KiB，出现在被 MIN 托住的最大
 * 几个 bin 上），而小块（命中率最高的那批）仍有 64 条可用。两端的夹紧各有用
 * 处：MIN 保证最大的几个 bin 也还有缓存，MAX 让小块不至于囤积到几千条——那
 * 已经超出"线程本地热缓存"的意义。 */
#define ED_TCACHE_BIN_BUDGET ((size_t)16 * 1024)
#define ED_TCACHE_MIN 4
#define ED_TCACHE_MAX 64
static inline uint32_t ed_tcache_cap(size_t block_size) {
  size_t n = ED_TCACHE_BIN_BUDGET / block_size;
  if (n < ED_TCACHE_MIN) n = ED_TCACHE_MIN;
  if (n > ED_TCACHE_MAX) n = ED_TCACHE_MAX;
  return (uint32_t)n;
}

#if defined(ED_DEBUG)
/* L2 在 tcache 侧的填充起点：块首两个机器字是 tcache 自己的元数据
 * （w[0] = 编码后的链指针，w[1] = 驻留 guard），只能从其后开始填与校。
 * 与页空闲链那套（从 ED_L2_LINK 起、保留尾部 ED_L2_TAIL）区间不同，
 * 理由见 secure.cpp 的 ed_debug_verify_tcache_block。
 * 声明必须先于 ed_tcache_pop（下方内联快路径要用）。 */
#define ED_L2_TCACHE_META (2 * sizeof(uintptr_t))
void ed_debug_verify_tcache_block(const void* block, size_t block_size);
#endif

/* 进程级 tcache guard（heap.cpp 在 init 时用 OS 熵源初始化，恒非 0）。 */
/* 代价：guard 是进程级的，任一线程对自己释放过的块做一次 UAF 读即可还原它，
 * 之后可为任意线程的块伪造驻留标记，跨线程 tcache 溢出的检出强度因此下降。
 * 接受这个代价是因为：链劫持仍由 per-thread 的 tcache_key 把守；而若改用页密钥
 * 做驻留标记，就要在最热的 ed_tcache_pop 快路径上加一次 ed_ptr_page 查找。 */
extern uintptr_t ed_tcache_guard;

/* tcache 驻留标记：w[1] == guard ^ 块地址。任何线程都能校验，
 * 因此跨线程 double-free 与 bin 满时走页路径的 double-free 都检得出。 */
static inline void ed_tcache_check_guard(const void* p) {
  const uintptr_t* w = (const uintptr_t*)p;
  if (ED_UNLIKELY(w[1] == (ed_tcache_guard ^ (uintptr_t)w)))
    ed_fatal_ptr("double free detected (block is resident in a thread cache)", p);
}

/* tcache 弹出：无锁、无页查找、无统计。
 * 校验 guard（检出 UAF 写/二次入链）与链指针对齐（检出链污染）。
 * 注意：弹出的块 encoded_next 域保持 tcache 编码残值——页密钥解码为随机值，
 * ed_secure_check_free 视为用户数据放行，不会误判 double-free。 */
static inline void* ed_tcache_pop(ed_theap_t* t, int bin) {
  ed_tcache_bin_t* tc = &t->tcache[bin - 1];
  uintptr_t* w = (uintptr_t*)tc->head;
  if (ED_LIKELY(w != NULL)) {
    if (ED_UNLIKELY(w[1] != (ed_tcache_guard ^ (uintptr_t)w)))
      ed_fatal_ptr("use-after-free write or tcache corruption", w);
    uintptr_t next = w[0] ^ t->tcache_key;
    /* 所有块大小都是 16 的倍数、页 64KiB 对齐 → 块地址恒 16 字节对齐 */
    if (ED_UNLIKELY((next & 0xFull) != 0))
      ed_fatal_ptr("tcache chain corrupted", w);
#if defined(ED_DEBUG)
    ed_debug_verify_tcache_block(w, ed_bin_size(bin));   /* L2: UAF 写校验 */
#endif
    w[1] = 0;
    tc->head = (void*)next;
    tc->count--;
    return w;
  }
  return NULL;
}

/* ==================== 用户指针 ←→ 块首（SPEC §4.5 对齐族） ==============
 * ed_aligned_alloc 交给调用方的指针**不一定是块首**：小块路径先超额分配一个
 * 普通块，再在块内向上对齐。于是"从用户指针走回块首"成了释放侧的必修课，而且
 * ed_free / ed_malloc_usable_size / ed_l2_unwrap 三处必须走同一条口径——三处
 * 各推一遍，是"free 已经对了而 usable_size 还错着"这类缺陷的温床。 */

/* large/huge/guarded 页里那唯一一个块的起点。guarded 时对象贴守护页右对齐
 * 放置，因此不等于 slice_start；其余情况就是 slice_start。 */
static inline uint8_t* ed_large_block_start(const ed_page_t* page) {
  if ((page->flags & ED_PAGE_FLAG_GUARDED) == 0) return page->slice_start;
  return page->slice_start + (size_t)(page->slice_count - 1) * ED_SLICE_SIZE
         - page->large_size;
}

/* small 页：页内任意地址 → 所在块的起点（块在页内等间距平铺）。
 * 它对**块内任意偏移**都给出一个合法块首，所以只能用在已知会出现"用户指针
 * 落在块内部"的页上（ED_PAGE_FLAG_HAS_ALIGNED）。无条件用它，等于把每一次
 * 内部指针 free 都静默纠正成合法释放，L0 的野指针检出就整体失效了。 */
static inline uint8_t* ed_small_block_start(const ed_page_t* page, const void* p) {
  size_t off = (size_t)((const uint8_t*)p - page->slice_start);
  return page->slice_start + (off / page->block_size) * page->block_size;
}

/* ======================== 页内快路径（≤10 指令目标） =================== */
#if defined(ED_DEBUG)
/* L2 普通块布局（user = block + ED_L2_HEAD）：
 *   [0, 8)              空闲链编码域 ED_L2_LINK：空闲时存编码 next，
 *                       已分配时存 encode(ED_BLOCK_ALLOCATED)（L1 哨兵）
 *   [8, 16)             头 canary（紧贴用户指针之前）
 *   [16, usable-8)      用户数据
 *   [usable-8, usable)  尾 canary
 * 于是 ED_L2_HEAD = ED_L2_LINK + ED_L2_CANARY 是推导出来的，不是巧合：头 canary
 * 只能放在编码域之后（否则每次 pop/push 都会把它冲掉），又必须紧贴用户指针
 * （否则对齐块的头 canary 离用户指针有一个 alignment 那么远，解包时还没找回
 * 块首就没法先验它）。用户指针因此恒落在块首 +16，16 字节对齐保证同样成立。
 *
 * 这个布局顺带补上了一条旧缺陷：头 canary 曾放在 [0,8)，于是 (a) 它把 L1 的
 * 已分配哨兵冲掉、ED_DEBUG 下 L1 的 double-free 检测形同虚设，(b) [8,16) 这段
 * 对齐填充在"已分配未释放"期间无人校验，紧贴用户指针的下溢写漏检。现在两段
 * 各司其职，1 字节的下溢写就会撞上头 canary。
 *
 * 不变式：被包装的块 usable >= 32（请求恒为 size + ED_L2_OVERHEAD，size >= 1，
 * 经 ed_bin 上取整后最小 32），故 usable - ED_L2_OVERHEAD 不会下溢。
 * 对齐块的同一笔账由 ed_aligned_alloc 的 slack 负责，两者都在 ed_l2_wrap_at
 * 里有一次机器可检查的兜底。 */
#define ED_L2_LINK      8                            /* 空闲链编码域，fill/verify 跳过 */
#define ED_L2_CANARY    8                            /* 单个 canary 的字节数 */
#define ED_L2_HEAD     (ED_L2_LINK + ED_L2_CANARY)   /* = 16：用户指针相对块首的偏移 */
#define ED_L2_TAIL      ED_L2_CANARY
#define ED_L2_OVERHEAD (ED_L2_HEAD + ED_L2_TAIL)
/* L2（编译期 -DED_DEBUG，实现于 secure.cpp）：canary 包装/解包 + UAF 写校验。
 * ed_l2_wrap_at 是对齐族用的通式：user 可以落在块内任意位置，头 canary 始终
 * 紧贴 user 之前 8 字节。ed_l2_wrap 是它 user = block + ED_L2_HEAD 的特例。 */
void*  ed_l2_wrap(void* block);
void*  ed_l2_wrap_at(void* block, void* user);
void*  ed_l2_unwrap(void* user);
void   ed_debug_verify_free_block(const ed_page_t* page, const void* block);
#endif

/* 用户指针 → 块首。ed_free_internal / ed_malloc_usable_size / ed_l2_unwrap
 * 三处共用的唯一口径（上面两个分量在这里汇合）。 */
static inline uint8_t* ed_user_block_start(const ed_page_t* page, const void* p) {
  if (page->kind == ED_PAGE_LARGE) return ed_large_block_start(page);
  if (ED_UNLIKELY((page->flags & ED_PAGE_FLAG_HAS_ALIGNED) != 0))
    return ed_small_block_start(page, p);
#if defined(ED_DEBUG)
  return (uint8_t*)((uintptr_t)p - ED_L2_HEAD);   /* 普通块：用户指针在块首之后 */
#else
  return (uint8_t*)(uintptr_t)p;                  /* 普通块：用户指针就是块首 */
#endif
}

static inline void* ed_page_malloc(ed_page_t* page) {
  ed_block_t* b = page->free;
  if (ED_LIKELY(b != NULL)) {
    page->free = ed_block_next(page, b);              /* L1: 解 XOR */
#if defined(ED_DEBUG)
    ed_debug_verify_free_block(page, b);              /* L2: UAF 写校验 */
#endif
    b->encoded_next = ed_encode(page, (const void*)ED_BLOCK_ALLOCATED);
    page->used++;
    return b;
  }
  return NULL;
}

/* ============================ 模块接口 ================================ */

/* --- meta.cpp：元数据 bump 分配器（与用户数据物理分离） --- */
void*      ed_meta_alloc(size_t size);              /* 16 字节对齐，永不 free */
void       ed_meta_lock(void);
void       ed_meta_unlock(void);
void       ed_meta_reinit_locks_child(void);

/* --- page.cpp --- */
/* 调用序约定：
 *   page->arena_idx = ...        （slice 的固有属性，ed_page_init 不碰）
 *   ed_page_init(...)            （清 magic → 写全部字段，此时本页对外不可见）
 *   ...large 还要补 large_size / flags / used / ed_desc_mark_cont...
 *   ed_page_publish(page)        （release store magic，唯一的发布点）
 * 把发布点单独拿出来，是因为 magic 就是 ed_ptr_page 的准入门槛：任何在它
 * 之后才写的字段，都可能被并发的野指针 free 读到半成品。 */
void ed_page_init(ed_page_t* page, ed_theap_t* owner, int bin, size_t block_size,
                  int kind, uint8_t* slice_start, uint32_t slice_count,
                  bool slice_is_zero);
static inline void ed_page_publish(ed_page_t* page) {
  page->magic.store(ED_PAGE_MAGIC, std::memory_order_release);
}
bool ed_page_free_collect(ed_page_t* page);         /* 返回是否腾出空间 */
void ed_page_set_owner(ed_page_t* page, ed_theap_t* t);  /* 设 owner 并重算 PRIVATE */
void ed_page_release_slices(ed_page_t* page);       /* magic 清零 + 归还 slice */
void ed_page_queue_push(ed_page_queue_t* q, ed_page_t* page);
void ed_page_queue_remove(ed_page_queue_t* q, ed_page_t* page);
#if defined(ED_DEBUG)
/* 页级不变式自检（ED_DEBUG）：见 page.cpp。违反即 fatal，不返回错误码——
 * 到这一步页的记账已经坏了，继续跑只会把现场冲掉。 */
void ed_page_check_invariants(const ed_page_t* page);
#endif

/* --- theap.cpp --- */
void ed_theap_exit_hook_init(void);   /* 注册线程退出回调（init 调一次） */
ed_theap_t* ed_theap_current(void);
ed_theap_t* ed_theap_create(void);
void*       ed_malloc_generic(ed_theap_t* t, int bin);
void*       ed_malloc_large(ed_theap_t* t, size_t size);   /* 8KiB < size：含 huge */
/* large 总入口：guarded 时多要一个 slice 作 PROT_NONE 守护页、对象贴其前边界 */
void*       ed_malloc_large_ex(ed_theap_t* t, size_t size, bool guarded,
                               bool* out_zeroed);
void*       ed_malloc_large_aligned(ed_theap_t* t, size_t alignment, size_t size);
void        ed_free_internal(void* p);
void        ed_theap_abandon(ed_theap_t* t);
void        ed_theap_collect(ed_theap_t* t);               /* ed_collect 用 */
void        ed_theap_destroy_all_pages(ed_theap_t* t);     /* heap_destroy 用 */
void        ed_theap_recycle_lock(void);                   /* fork 钩子用 */
void        ed_theap_recycle_unlock(void);
void        ed_theap_reinit_locks_child(void);

/* 快路径直接读取的 TLS 指针（定义在 theap.cpp） */
extern thread_local ed_theap_t* ed_t_theap;
static inline ed_theap_t* ed_theap_fast(void) {
  ed_theap_t* t = ed_t_theap;
  if (ED_UNLIKELY(t == NULL)) t = ed_theap_create();
  return t;
}

/* --- arena.cpp --- */
bool        ed_window_init(size_t want_size);       /* 保留窗口 + 描述符数组 */
ed_arena_t* ed_arena_get(int idx);
int         ed_arena_count(void);
int         ed_arena_pick_for_thread(ed_theap_t* t);
/* 分配 n 个连续 slice，返回 arena 内下标（失败 (size_t)-1）。
 * align_slices：返回地址须按 align_slices*64KiB 对齐（1 = 无额外约束）。
 * *out_zeroed：slice 内容是否保证全零（fresh 或 purge 后）。 */
size_t      ed_arena_alloc_slices(ed_arena_t* a, size_t n, size_t align_slices,
                                  bool* out_zeroed);
size_t      ed_arena_alloc_slices_any(ed_theap_t* t, size_t n, size_t align_slices,
                                      ed_arena_t** out_arena, bool* out_zeroed);
void        ed_arena_free_slices(ed_arena_t* a, size_t idx, size_t n);  /* dirty → 到期 purge */
void        ed_arena_free_slices_decommitted(ed_arena_t* a, size_t idx, size_t n);
void        ed_arena_purge_check(ed_arena_t* a);   /* 到期才 purge */
void        ed_arena_purge_force(ed_arena_t* a);   /* ed_collect 用 */
void        ed_arena_abandon_page(ed_arena_t* a, ed_page_t* page);
ed_page_t*  ed_arena_reclaim_page(ed_arena_t* a, int bin);
/* collect 全桶，摘出 used==0 的页链（next 串接），调用方负责归还 slice */
ed_page_t*  ed_arena_take_abandoned_empty(ed_arena_t* a);
size_t      ed_arena_committed_bytes(void);
size_t      ed_arena_abandoned_count(void);
/* 描述符平铺数组的维护：CONT 项登记 / 整段 magic 清零 */
void        ed_desc_mark_cont(size_t first_idx, size_t n);
void        ed_desc_clear(size_t first_idx);
/* fork 钩子支撑 */
void        ed_arena_lock_all(void);
void        ed_arena_unlock_all(void);
void        ed_arena_reinit_locks_child(void);

/* --- heap.cpp --- */
uint64_t ed_global_seed(void);   /* heap.cpp：init 时取自 OS 熵源 */
void        ed_init_once(void);                     /* 选项解析/窗口/默认堆/atfork */
ed_heap_t*  ed_default_heap(void);
uintptr_t   ed_next_thread_id(void);                /* 全局自增，左移3位唯一 */
void*       ed_heap_malloc_internal(ed_heap_t* heap, size_t size);

/* --- secure.cpp --- */
void   ed_secure_page_keys(ed_page_t* page, uint64_t* random_state);
void*  ed_guarded_alloc(ed_theap_t* t, size_t size);   /* L3 守护页分配 */
/* L2 声明（ed_l2_wrap / ed_l2_unwrap / ed_debug_verify_free_block）见上文
 * "页内快路径"一节——ed_page_malloc 内联需要先行声明。 */

/* --- diag.cpp --- */
#if defined(ED_DEBUG)
void   ed_debug_leak_report(void);
#endif

/* ============================== 统计记账 ============================== */
/* 每次 malloc 恰调用一次 ed_stat_alloc、每次 free 恰调用一次 ed_stat_free，
 * 记在**调用线程**的 theap 上。字节口径统一为"可用字节"（small = block_size，
 * large = large_size），两侧同口径，相减才有意义。
 *
 * 这条"恰好一次"是硬约束而非风格偏好：原实现把记账撒在 ed_malloc_generic /
 * ed_malloc_large / ed_free_to_page 三处，于是 tcache 命中与 pages_free_direct
 * 命中这两条快路径只减不加，stat_current 一路下溢。修法是把记账点上提到
 * 每条路径必经的汇合处（分配侧 = ed_malloc_internal_with / large 入口，
 * 释放侧 = ed_free_internal），页层与 tcache 冲刷一律不再记账。
 *
 * 代价：快路径多两条对同一条 cache line 的写（计数 + 字节）。这条线在
 * theap 结构里、进程生命期内常驻 L1，实测 bench_single 落在噪声带内。 */
static inline void ed_stat_alloc(ed_theap_t* t, size_t usable) {
  t->stat_alloc_count++;
  t->stat_alloc_bytes += usable;
}
static inline void ed_stat_free(ed_theap_t* t, size_t usable) {
  t->stat_free_count++;
  t->stat_free_bytes += usable;
}

/* ============================ 分配总入口 ============================== */
/* api.cpp 的 ed_malloc 经此内联展开快路径。
 * 约定：t 恒为**调用线程**的 theap（私有堆走 heap.cpp 的独立路径）。 */

/* 小块的三条快路径 + 慢路径兜底。单独拆出来，是为了让记账在调用方只出现
 * 一次——本函数的每一个 return 都是"一次成功分配"，谁也漏不掉。 */
static inline void* ed_malloc_small(ed_theap_t* t, int bin) {
  /* tcache 命中即返回。ED_DEBUG 下同样启用：tcache 侧另有一套 L2 填充/校验
   * 区间（见 ed_debug_verify_tcache_block），覆盖不因此出缺口；而"调试构建
   * 走的是另一条分配路径"本身才是更大的风险——真正跑在生产里的那条快路径
   * 将永远得不到 canary 与 UAF 填充的复核。 */
  void* tcp = ed_tcache_pop(t, bin);
  if (ED_LIKELY(tcp != NULL)) return tcp;
  ed_page_t* page;
  if (bin < ED_DIRECT_MAX) {
    page = t->pages_free_direct[bin];
    if (ED_LIKELY(page != NULL)) {
      void* p = ed_page_malloc(page);
      if (ED_LIKELY(p != NULL)) return p;
    }
  } else {
    page = t->pages[bin - 1].first;
    if (page != NULL) {
      void* p = ed_page_malloc(page);
      if (p != NULL) return p;
    }
  }
  return ed_malloc_generic(t, bin);
}

static inline void* ed_malloc_internal_with(ed_theap_t* t, size_t size) {
  if (ED_LIKELY(size <= ED_SMALL_MAX)) {
    if (ED_UNLIKELY(size == 0)) size = 1;
    int bin = ed_bin(size);
    void* p = ed_malloc_small(t, bin);
    /* ed_bin_size(bin) 恒等于所在页的 block_size（ed_malloc_generic 就是用它
     * 建的页），但它是纯 ALU、不碰描述符——tcache 命中路径因此仍然不需要
     * 任何一次 ed_ptr_page 查找。 */
    if (ED_LIKELY(p != NULL)) ed_stat_alloc(t, ed_bin_size(bin));
    return p;
  }
  return ed_malloc_large(t, size);   /* large 路径在 theap.cpp 内自行记账 */
}

static inline void* ed_malloc_internal(size_t size) {
  return ed_malloc_internal_with(ed_theap_fast(), size);
}

/* small 块的可用字节数 */
static inline size_t ed_page_usable_size(const ed_page_t* page) {
  if (page->kind == ED_PAGE_SMALL) return page->block_size;
  return page->large_size;
}

#endif /* EDUMALLOC_INTERNAL_H */
