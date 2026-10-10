#pragma once

#include "internal.h"

namespace wavvix { namespace mem { namespace detail {

/* 线程本地堆（thread_local 持有） */
/* tcache 中的块仍计入 page->used；跨线程检查需要进程级驻留标记。 */
struct mem_tcache_bin_t {
  void*    head;
  uint32_t count;
  uint32_t _pad;
};

struct mem_theap_t {
  mem_page_queue_t pages[WAVVIX_MEM_BIN_COUNT];              /* 0..35 每 bin + [WAVVIX_MEM_BIN_FULL] */
  uint64_t        random_state;                     /* splitmix64 */
  uintptr_t       thread_id;                        /* 唯一的非零线程编号 */
  int             arena_idx;                        /* 当前偏好 arena */
  bool            temporary;                        /* 本次调用后归还，不能依赖退出钩子 */
  /* tcache（见上方注释）；tcache_key 在 theap 创建时随机化 */
  mem_tcache_bin_t tcache[WAVVIX_MEM_BIN_FULL];                /* 索引 bin-1，small bin 1..36 */
  uintptr_t       tcache_key;
  mem_theap_t* next_free;                            /* theap 回收链 */
};


/* ============================ tcache 快路径 ============================ */
/* 按字节预算限制每个 bin 的滞留量，再夹到最小/最大条数。
 * 容量与占用数据见 docs/archive/edumalloc/docs/decisions.md D1。 */
#define WAVVIX_MEM_TCACHE_BIN_BUDGET ((size_t)16 * 1024)
#define WAVVIX_MEM_TCACHE_MIN 4
#define WAVVIX_MEM_TCACHE_MAX 64
static inline uint32_t mem_tcache_cap(size_t block_size) {
  size_t n = WAVVIX_MEM_TCACHE_BIN_BUDGET / block_size;
  if (n < WAVVIX_MEM_TCACHE_MIN) n = WAVVIX_MEM_TCACHE_MIN;
  if (n > WAVVIX_MEM_TCACHE_MAX) n = WAVVIX_MEM_TCACHE_MAX;
  return (uint32_t)n;
}


/* tcache 驻留标记：w[1] == guard ^ 块地址。任何线程都能校验，
 * 因此跨线程 double-free 与 bin 满时走页路径的 double-free 都检得出。 */
static inline void mem_tcache_check_guard(const void* p) {
  const uintptr_t* w = (const uintptr_t*)p;
  if (WAVVIX_MEM_UNLIKELY(w[1] == (mem_tcache_guard ^ (uintptr_t)w)))
    mem_fatal_ptr("double free detected (block is resident in a thread cache)", p);
}

/* tcache 弹出：无锁、无页查找、无统计。
 * 校验 guard（检出 UAF 写/二次入链）与链指针对齐（检出链污染）。
 * 注意：弹出的块 encoded_next 域保持 tcache 编码残值——页密钥解码为随机值，
 * mem_secure_check_free 视为用户数据放行，不会误判 double-free。 */
static inline void* mem_tcache_pop(mem_theap_t* t, int bin) {
  mem_tcache_bin_t* tc = &t->tcache[bin - 1];
  uintptr_t* w = (uintptr_t*)tc->head;
  if (WAVVIX_MEM_UNLIKELY(w == NULL)) return NULL;
  if (WAVVIX_MEM_UNLIKELY(w[1] != (mem_tcache_guard ^ (uintptr_t)w)))
    mem_fatal_ptr("use-after-free write or tcache corruption", w);
  uintptr_t next = w[0] ^ t->tcache_key;
  /* 所有块大小都是 16 的倍数、页 64KiB 对齐 → 块地址恒 16 字节对齐 */
  if (WAVVIX_MEM_UNLIKELY((next & 0xFull) != 0))
    mem_fatal_ptr("tcache chain corrupted", w);
#if defined(WAVVIX_MEM_DEBUG)
  mem_debug_verify_tcache_block(w, mem_bin_size(bin));   /* L2: UAF 写校验 */
#endif
  w[1] = 0;
  tc->head = (void*)next;
  tc->count--;
  return w;
}


/* --- theap.cpp --- */
void mem_theap_exit_hook_init(void);   /* 注册线程退出回调（init 调一次） */
mem_theap_t* mem_theap_create(void);
void        mem_theap_release(mem_theap_t* t);
void*       mem_malloc_generic(mem_theap_t* t, int bin);
void*       mem_malloc_large(mem_theap_t* t, size_t size, bool* out_zeroed = NULL);
void*       mem_malloc_large_aligned(mem_theap_t* t, size_t alignment, size_t size);
void        mem_theap_collect(mem_theap_t* t);               /* wavvix::mem::collect 用 */
void        mem_theap_recycle_lock(void);                   /* fork 钩子用 */
void        mem_theap_recycle_unlock(void);
void        mem_theap_reinit_locks_child(void);

/* 快路径直接读取的 TLS 指针（定义在 theap.cpp） */
extern thread_local mem_theap_t* mem_t_theap;
static inline mem_theap_t* mem_theap_fast(void) {
  mem_theap_t* t = mem_t_theap;
  if (WAVVIX_MEM_UNLIKELY(t == NULL)) t = mem_theap_create();
  return t;
}

// Normal threads retain their heap; teardown/failing exit registration gets a lease.
class heap_scope {
public:
  heap_scope() : heap_(mem_theap_fast()) {}
  ~heap_scope() {
    if (heap_ != NULL && WAVVIX_MEM_UNLIKELY(heap_->temporary)) mem_theap_release(heap_);
  }
  mem_theap_t* get() const { return heap_; }
  heap_scope(const heap_scope&) = delete;
  heap_scope& operator=(const heap_scope&) = delete;
private:
  mem_theap_t* heap_;
};


/* ============================ 分配总入口 ============================== */
/* api.cpp 的 wavvix::mem::alloc 经此内联展开快路径；t 属于调用线程。 */

/* 小块先取线程缓存，再取所属 bin 的队首页。 */
static inline void* mem_malloc_small(mem_theap_t* t, int bin) {
  /* tcache 命中即返回。WAVVIX_MEM_DEBUG 下同样启用：tcache 侧另有一套 L2 填充/校验
   * 区间（见 mem_debug_verify_tcache_block），覆盖不因此出缺口；而"调试构建
   * 走的是另一条分配路径"本身才是更大的风险——真正跑在生产里的那条快路径
   * 将永远得不到 canary 与 UAF 填充的复核。 */
  void* tcp = mem_tcache_pop(t, bin);
  if (WAVVIX_MEM_LIKELY(tcp != NULL)) return tcp;
  mem_page_t* page = t->pages[bin - 1].first;
  if (page != NULL) {
    void* p = mem_page_malloc(page);
    if (p != NULL) return p;
  }
  return mem_malloc_generic(t, bin);
}

static inline void* mem_malloc_internal_with(mem_theap_t* t, size_t size) {
  if (WAVVIX_MEM_LIKELY(size <= WAVVIX_MEM_SMALL_MAX)) {
    if (WAVVIX_MEM_UNLIKELY(size == 0)) size = 1;
    int bin = mem_bin(size);
    return mem_malloc_small(t, bin);
  }
  return mem_malloc_large(t, size);
}


} } } // namespace wavvix::mem::detail
