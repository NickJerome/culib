#include "internal.h"
#include "meta.h"

#include <cstring>

namespace wavvix { namespace mem { namespace detail {

/* 快路径直接读取的 TLS 指针（internal.h 中 inline） */
thread_local mem_theap_t* mem_t_theap = NULL;
static thread_local bool thread_exiting = false;

static void mem_tcache_flush_all(mem_theap_t* t);

namespace {

/* OS 回调直接传入 theap；MinGW 的 TLS 对象析构阶段不能安全读取 TLS。
 * 历史问题与实测记录见 docs/archive/edumalloc/docs/decisions.md。 */
static void mem_thread_exit_cb(void* value) {
  mem_theap_t* t = (mem_theap_t*)value;
  if (t == NULL) return;
  if (mem_t_theap == t) thread_exiting = true;
  mem_theap_release(t);
}

}  /* namespace */

/* 注册失败时，分配入口使用单次调用的 heap_scope。 */
void mem_theap_exit_hook_init(void) {
  mem_os_thread_exit_init(mem_thread_exit_cb);
}

namespace {

/* theap 结构回收链（线程 churn 不泄漏元数据） */
allocator_mutex   g_theap_recycle_lock;
mem_theap_t*  g_theap_recycle = NULL;

/* 页退休 */

void mem_theap_retire_page(mem_theap_t* t, mem_page_t* page) {
  int bin = page->bin;
  if ((page->flags & WAVVIX_MEM_PAGE_FLAG_IN_FULL) != 0) {
    mem_page_queue_remove(&t->pages[WAVVIX_MEM_BIN_FULL], page);
    t->full_count[bin]--;
    page->flags &= (uint8_t)~WAVVIX_MEM_PAGE_FLAG_IN_FULL;
  } else {
    mem_page_queue_remove(&t->pages[bin - 1], page);
  }
  if (bin < WAVVIX_MEM_DIRECT_MAX && t->pages_free_direct[bin] == page)
    t->pages_free_direct[bin] = NULL;

  mem_page_release_slices(page);   /* 清 magic + 归还 slice */
}

/* 心跳驱动的退休宽限扫描：每 16 次本地 free 扫一次本 bin */
void mem_theap_retire_scan(mem_theap_t* t, int bin) {
  mem_page_queue_t* q = &t->pages[bin - 1];
  mem_page_t* page = q->first;
  while (page != NULL) {
    mem_page_t* next = page->next;
    if (page->used == 0 && page->retire_expire > 0) {
      page->retire_expire--;
      /* 到期且本 bin 不止这一页 → 真正退休（每 bin 至少留 1 页） */
      if (page->retire_expire == 0 &&
          q->count + t->full_count[bin] > 1) {
        mem_theap_retire_page(t, page);
      }
    }
    page = next;
  }
}

/* used==0 的页：立即退休（bin 内还有其它页）或进入宽限 */
void mem_theap_page_empty(mem_theap_t* t, mem_page_t* page) {
  int bin = page->bin;
  bool in_full = (page->flags & WAVVIX_MEM_PAGE_FLAG_IN_FULL) != 0;
  size_t total = t->pages[bin - 1].count + t->full_count[bin];
  if (total > 1 || in_full) {
    mem_theap_retire_page(t, page);
  } else {
    page->retire_expire = WAVVIX_MEM_RETIRE_EXPIRE;
  }
}

void mem_theap_page_to_full(mem_theap_t* t, mem_page_t* page) {
  int bin = page->bin;
  mem_page_queue_remove(&t->pages[bin - 1], page);
  if (bin < WAVVIX_MEM_DIRECT_MAX && t->pages_free_direct[bin] == page)
    t->pages_free_direct[bin] = NULL;
  /* 满页留在本地；腾出空间后由 unfull 回流，空页由 retire 回收。
   * abandon/reclaim 风暴的实测见 docs/archive/edumalloc/docs/design.md §5.2.2。 */
  page->flags |= WAVVIX_MEM_PAGE_FLAG_IN_FULL;
  t->full_count[bin]++;
  mem_page_queue_push(&t->pages[WAVVIX_MEM_BIN_FULL], page);
}

/* 满页腾空 → 移回本 bin 普通队列 */
void mem_theap_unfull(mem_theap_t* t, mem_page_t* page) {
  int bin = page->bin;
  mem_page_queue_remove(&t->pages[WAVVIX_MEM_BIN_FULL], page);
  t->full_count[bin]--;
  page->flags &= (uint8_t)~WAVVIX_MEM_PAGE_FLAG_IN_FULL;
  mem_page_queue_push(&t->pages[bin - 1], page);
}

/* FULL 队列里本 bin 的页可能被跨线程释放腾出了空间：collect 试试 */
mem_page_t* mem_theap_unfull_scan(mem_theap_t* t, int bin) {
  mem_page_queue_t* fq = &t->pages[WAVVIX_MEM_BIN_FULL];
  int scanned = 0;
  mem_page_t* page = fq->first;
  while (page != NULL && scanned < WAVVIX_MEM_PAGE_MAX_CANDIDATES) {
    mem_page_t* next = page->next;
    if (page->bin == bin) {
      scanned++;
      mem_page_free_collect(page);
      if (page->used == 0) {
        mem_theap_retire_page(t, page);   /* 全空：直接退休 */
      } else if (page->free != NULL) {
        mem_theap_unfull(t, page);
        return page;
      }
    }
    page = next;
  }
  return NULL;
}

/* abandoned 页接管 */

mem_page_t* mem_page_fresh(mem_theap_t* t, int bin) {
  /* 1) reclaim 优先：本 arena 的 abandoned 桶 */
  mem_arena_t* arena = mem_arena_get(t->arena_idx);
  if (arena != NULL) {
    for (;;) {
      mem_page_t* page = mem_arena_reclaim_page(arena, bin);
      if (page == NULL) break;
      /* 接管所有权 */
      /* 只清 IN_FULL；其余标志属于页本身。 */
      page->flags &= (uint8_t)~WAVVIX_MEM_PAGE_FLAG_IN_FULL;
      mem_page_set_owner(page, t);
      page->retire_expire = 0;
      mem_page_free_collect(page);   /* 收进积压的跨线程释放 */
      if (page->free != NULL) {
        mem_page_queue_push(&t->pages[bin - 1], page);
        return page;
      }
      if (page->used == 0) {
        /* 全空的被弃页：直接退休归还 slice，继续找 */
        mem_page_release_slices(page);
        continue;
      }
      /* 仍全满：挂进 FULL 队列（可能再度触发 abandon） */
      page->flags |= WAVVIX_MEM_PAGE_FLAG_IN_FULL;
      t->full_count[bin]++;
      mem_page_queue_push(&t->pages[WAVVIX_MEM_BIN_FULL], page);
      return page;   /* 让调用方走"再找一页"逻辑 */
    }
  }
  return NULL;
}

} /* anonymous namespace */

/* ------------------------- theap 生命周期 ------------------------------ */

mem_theap_t* mem_theap_create(void) {
  mem_init_once();

  mem_theap_t* t = NULL;
  {
    std::lock_guard<allocator_mutex> lock(g_theap_recycle_lock);
    if (g_theap_recycle != NULL) {
      t = g_theap_recycle;
      g_theap_recycle = t->next_free;
    }
  }
  if (t == NULL) t = (mem_theap_t*)mem_meta_alloc(sizeof(mem_theap_t));
  if (t == NULL) return NULL;
  std::memset(t, 0, sizeof(*t));
  for (int bin = 1; bin <= WAVVIX_MEM_BIN_FULL; bin++)
    t->pages[bin - 1].block_size = mem_bin_size(bin);
  t->pages[WAVVIX_MEM_BIN_FULL].block_size = 0;
  t->thread_id = mem_next_thread_id();
  t->random_state = mem_random_seed(mem_global_seed() ^ t->thread_id ^ (uintptr_t)t);
  t->tcache_key   = mem_random_next(&t->random_state);
  if (mem_arena_pick_for_thread(t) < 0) return NULL;   /* 确保至少一个 arena */
  mem_t_theap = t;
  /* 线程退出钩子登记。放在最后一步：上面任何一个失败返回都不该留下一个
   * "登记了却没建成"的 theap。 */
  t->temporary = thread_exiting || !mem_os_thread_exit_set(t);
  return t;
}

void mem_theap_release(mem_theap_t* t) {
  if (mem_t_theap == t) mem_t_theap = NULL;
  mem_tcache_flush_all(t);
  mem_theap_abandon(t);
}

/* 线程退出时摘出所持小页并登记 abandoned 桶。 */
void mem_theap_abandon(mem_theap_t* t) {
  for (int bin = 1; bin < WAVVIX_MEM_DIRECT_MAX; bin++)
    t->pages_free_direct[bin] = NULL;
  for (int qi = 0; qi < WAVVIX_MEM_BIN_COUNT; qi++) {
    mem_page_queue_t* q = &t->pages[qi];
    while (q->first != NULL) {
      mem_page_t* page = q->first;
      mem_page_queue_remove(q, page);
      int bin = page->bin;
      if (qi == WAVVIX_MEM_BIN_FULL && t->full_count[bin] > 0) t->full_count[bin]--;
      page->flags &= (uint8_t)~WAVVIX_MEM_PAGE_FLAG_IN_FULL;
      /* 先收一次：已全空的页直接归还 slice，不必进 abandoned 桶再等 reclaim */
      mem_page_free_collect(page);
      if (page->used == 0) {
        mem_page_release_slices(page);
        continue;
      }
      page->theap = NULL;
      page->xthread_id.fetch_or(WAVVIX_MEM_PAGE_XTHREAD_ABANDONED,
                                std::memory_order_acq_rel);
      mem_arena_t* arena = mem_arena_get(page->arena_idx);
      if (arena != NULL) mem_arena_abandon_page(arena, page);
    }
  }
  /* large 页不在队列中，free 路径直接处理。 */
  /* 回收 theap 结构本身（线程 churn 不泄漏元数据） */
  std::lock_guard<allocator_mutex> lock(g_theap_recycle_lock);
  t->next_free = g_theap_recycle;
  g_theap_recycle = t;
}

/* ------------------------- 强制收集（wavvix::mem::collect 用） ------------------- */

/* collect 所有页并退休全部空页（无视"每 bin 留一页"，调用方明确要求收缩） */
void mem_theap_collect(mem_theap_t* t) {
  mem_tcache_flush_all(t);   /* 先把 tcache 滞留块归还到页，collect 才能看到真空页 */
  for (int qi = 0; qi < WAVVIX_MEM_BIN_COUNT; qi++) {
    mem_page_queue_t* q = &t->pages[qi];
    mem_page_t* page = q->first;
    while (page != NULL) {
      mem_page_t* next = page->next;
      mem_page_free_collect(page);
#if defined(WAVVIX_MEM_DEBUG)
      /* collect 刚把两条链并完，此刻页内空闲块全在 free/local_free 上：
       * 这是 used/capacity 记账唯一能被廉价全量复核的时刻。必须在下面的
       * retire 之前——retire 会归还 slice，描述符随即失效。 */
      mem_page_check_invariants(page);
#endif
      if (page->used == 0) {
        mem_theap_retire_page(t, page);
      } else if ((page->flags & WAVVIX_MEM_PAGE_FLAG_IN_FULL) != 0 &&
                 page->free != NULL) {
        mem_theap_unfull(t, page);
      }
      page = next;
    }
  }
}

/* fork 钩子：prepare 时按固定顺序上锁，child 里重建（持锁线程已不存在） */
void mem_theap_recycle_lock(void)   { g_theap_recycle_lock.lock(); }
void mem_theap_recycle_unlock(void) { g_theap_recycle_lock.unlock(); }

void mem_theap_reinit_locks_child(void) {
  new (&g_theap_recycle_lock) allocator_mutex();
}

/* 分配慢路径 */

void* mem_malloc_generic(mem_theap_t* t, int bin) {
  t->heartbeat++;

  mem_arena_t* arena = mem_arena_get(t->arena_idx);
  /* purge 到期检查只在申请新 slice 时做，避免每次慢路径都读时钟。 */

  /* 0) 分配驱动兜底：本 bin 的 abandoned 桶非空 → 优先认领一页。
   *    否则满页压力降级产生的被弃页只进不出，内存会随时间膨胀。 */
  if (arena != NULL &&
      (arena->abandoned_nonempty.load(std::memory_order_relaxed) &
          (uint64_t(1) << (bin - 1))) != 0) {
    mem_page_t* rp = mem_page_fresh(t, bin);
    if (rp != NULL && rp->free != NULL) {
      if (bin < WAVVIX_MEM_DIRECT_MAX) t->pages_free_direct[bin] = rp;
      return mem_page_malloc(rp);   /* 记账在调用方（见 internal.h 的统计记账一节） */
    }
    /* rp 为 NULL（桶空）或仍全满（已挂 FULL 队列）：继续正常路径 */
  }

  /* next-fit：仅在 free 为空时 collect，首个可用页即返回；满页隔离，扫描有上限。 */
  mem_page_queue_t* q = &t->pages[bin - 1];
  mem_page_t* best = NULL;
  long scanned = 0;
  mem_page_t* page = q->first;
  while (page != NULL && scanned < WAVVIX_MEM_PAGE_MAX_CANDIDATES) {
    mem_page_t* next = page->next;
    if (page->free == NULL) mem_page_free_collect(page);
    scanned++;
    if (page->free != NULL) { best = page; break; }   /* 首个可用页即返回 */
    if (page->used == 0) {
      mem_theap_page_empty(t, page);                   /* 全空：退休判定 */
    } else {
      mem_theap_page_to_full(t, page);                 /* 仍满：隔离 */
    }
    page = next;
  }

  /* 2) FULL 队列中本 bin 的页可能已被跨线程释放腾出空间 */
  if (best == NULL) best = mem_theap_unfull_scan(t, bin);

  /* 3) reclaim abandoned / arena 新页 */
  mem_page_t* fresh = NULL;
  if (best == NULL) {
    fresh = mem_page_fresh(t, bin);
    if (fresh != NULL && fresh->free == NULL) {
      /* reclaim 到的是全满页：已挂 FULL，重新走 fresh 拿新页 */
      fresh = NULL;
    }
    if (fresh == NULL) {
      /* arena 分配 1 slice（唯一可能持锁/系统调用的环节）。
       * 摊销：既已到此慢路径，顺带做到期 purge 检查 */
      if (arena != NULL) mem_arena_purge_check(arena);
      bool zeroed = false;
      mem_arena_t* a = NULL;
      size_t sidx = mem_arena_alloc_slices_any(t, 1, 1, &a, &zeroed);
      if (sidx == (size_t)-1) return NULL;
      /* 描述符与 slice 平铺：slice 下标即描述符下标，无需分配与登记 */
      fresh = mem_desc_at(a->first_slice + sidx);
      uint8_t* start = a->base + sidx * WAVVIX_MEM_SLICE_SIZE;
      fresh->arena_idx = a->index;   /* 必须先于 mem_page_init 的 magic 发布 */
      mem_page_init(fresh, t, bin, mem_bin_size(bin), WAVVIX_MEM_PAGE_SMALL,
                   start, 1, zeroed);
      mem_page_publish(fresh);
      mem_page_queue_push(&t->pages[bin - 1], fresh);
    }
    best = fresh;
  }
  if (best == NULL || best->free == NULL) return NULL;

  if (bin < WAVVIX_MEM_DIRECT_MAX) t->pages_free_direct[bin] = best;
  return mem_page_malloc(best);
}

/* large / huge */

/* huge 在 free 时 decommit。align_slices=1 表示无额外对齐约束。 */
static void* mem_malloc_large_impl(mem_theap_t* t, size_t size,
                                   size_t align_slices, bool* out_zeroed) {
  if (size == 0) size = 1;
  const size_t data = (size + WAVVIX_MEM_SLICE_SIZE - 1) / WAVVIX_MEM_SLICE_SIZE * WAVVIX_MEM_SLICE_SIZE;
  const size_t n = data >> WAVVIX_MEM_SLICE_SHIFT;

  if (align_slices != 1) {
    /* 新建 arena 时需要为对齐预留空间，先定界避免 slice 数回绕。 */
    const size_t window_slices = mem_window_size >> WAVVIX_MEM_SLICE_SHIFT;
    if (align_slices == 0 || align_slices > window_slices ||
        n == 0 || n > window_slices - align_slices)
      return NULL;
  }

  bool zeroed = false;
  mem_arena_t* a = NULL;
  size_t sidx = mem_arena_alloc_slices_any(t, n, align_slices, &a, &zeroed);
  if (sidx == (size_t)-1) return NULL;

  mem_page_t* page = mem_desc_at(a->first_slice + sidx);
  uint8_t* start = a->base + sidx * WAVVIX_MEM_SLICE_SIZE;
  page->arena_idx = a->index;   /* slice 的固有属性，mem_page_init 不碰 */
  mem_page_init(page, t, 0, 0, WAVVIX_MEM_PAGE_LARGE, start, (uint32_t)n, zeroed);
  page->large_size = n * WAVVIX_MEM_SLICE_SIZE;
  if (size > WAVVIX_MEM_HUGE_THRESHOLD) page->flags |= WAVVIX_MEM_PAGE_FLAG_HUGE;
  mem_desc_mark_cont(mem_page_slice_index(page), n);
  page->used = 1;
  mem_page_publish(page);   /* 续页项先于首项发布：野指针只会解析到 magic==0 */

  if (out_zeroed != NULL) *out_zeroed = zeroed;
  return start;
}

void* mem_malloc_large(mem_theap_t* t, size_t size, bool* out_zeroed) {
  return mem_malloc_large_impl(t, size, 1, out_zeroed);
}

/* alignment > 64KiB 的对齐分配：位图按对齐起点选段（arena 相对下标 + 基址
 * 相位，见 mem_arena_alloc_slices）。alignment ≤ 64KiB 由 slice 天然对齐覆盖，
 * 走 mem_malloc_large 即可。 */
void* mem_malloc_large_aligned(mem_theap_t* t, size_t alignment, size_t size) {
  return mem_malloc_large_impl(t, size, alignment >> WAVVIX_MEM_SLICE_SHIFT, NULL);
}

/* large/huge 只有一个合法块首，无需 owner。 */
static void mem_page_free_large(mem_page_t* page, void* p) {
  const uint8_t* expect = page->slice_start;
  if (WAVVIX_MEM_UNLIKELY((const uint8_t*)p != expect))
    mem_fatal_page("free of interior pointer (large block)", p, page);

  mem_page_release_slices(page);
}

/* 释放路径 */

/* 页归还路径（tcache 未命中/未启用时的完整语义路径）。 */
static void mem_free_to_page(mem_theap_t* t, mem_page_t* page, void* p) {
  /* 先取出后面要用的字段：本地分支里 mem_theap_page_empty 可能退休本页（描述符
   * 失效、slice 归还），跨线程分支里 CAS 一成功块就对 owner 可见、owner 可能
   * 立刻退休本页——两种情况下再解引用 page 都是读已释放的描述符。 */
  const int    bin = page->bin;
#if defined(WAVVIX_MEM_DEBUG)
  const size_t bs  = page->block_size;   /* 只有 L2 的 0xDF 填充还要用它 */
#endif
  uintptr_t owner = page->xthread_id.load(std::memory_order_relaxed);
  if (t != NULL && WAVVIX_MEM_LIKELY(owner == t->thread_id)) {
    /* 本地释放（≤6 条指令目标）：压 local_free + 计数 */
    mem_tcache_check_guard(p);        /* 块仍驻留某个 tcache → double free */
    mem_secure_check_free(page, p);   /* L1 double-free 编码哨兵 */
    if (WAVVIX_MEM_UNLIKELY(page->used == 0))
      mem_fatal_page("double free (page used underflow)", p, page);
    mem_block_set_next(page, (mem_block_t*)p, page->local_free);
    page->local_free = (mem_block_t*)p;
    page->used--;
    page->free_is_zero = false;
#if defined(WAVVIX_MEM_DEBUG)
    /* L2：释放填 0xDF（保留首 8B 编码域与尾部 canary），UAF 写探测。
     * 必须在压链之后：哨兵校验依赖压链写入的编码态。 */
    if (bs > 16)
      std::memset((uint8_t*)p + WAVVIX_MEM_L2_LINK, 0xDF, bs - WAVVIX_MEM_L2_LINK - WAVVIX_MEM_L2_TAIL);
#endif
    if (WAVVIX_MEM_UNLIKELY(page->used == 0)) {
      mem_theap_page_empty(t, page);            /* 退休判定 */
    } else if ((page->flags & WAVVIX_MEM_PAGE_FLAG_IN_FULL) != 0) {
      mem_theap_unfull(t, page);                /* 满页腾空 → 移回本 bin */
    }
    t->heartbeat++;
    if ((t->heartbeat & 0xF) == 0) mem_theap_retire_scan(t, bin);
  } else {
    /* 跨线程释放（含 abandoned 页）：恰好一次成功 CAS 压 xthread_free。
     * ABA 安全：本链只允许压入；整链摘走由 owner/reclaimer 用 exchange(0)。 */
    mem_tcache_check_guard(p);        /* 块仍驻留某个 tcache → double free */
    mem_secure_check_free(page, p);
#if defined(WAVVIX_MEM_DEBUG)
    /* L2：先填 0xDF 再 CAS——CAS 成功瞬间块即对 owner 可见，
     * 填充必须在此之前完成（但绝不可碰首 8B 编码域）。 */
    if (bs > 16)
      std::memset((uint8_t*)p + WAVVIX_MEM_L2_LINK, 0xDF, bs - WAVVIX_MEM_L2_LINK - WAVVIX_MEM_L2_TAIL);
#endif
    mem_block_t* b = (mem_block_t*)p;
    uintptr_t old = page->xthread_free.load(std::memory_order_relaxed);
    do {
      mem_block_set_next(page, b, (mem_block_t*)old);
    } while (!page->xthread_free.compare_exchange_weak(
                 old, (uintptr_t)b, std::memory_order_release,
                 std::memory_order_relaxed));
  }
}

/* tcache 全量冲刷：线程退出/collect 时把缓存块逐个走完整页路径归还 */
static void mem_tcache_flush_all(mem_theap_t* t) {
  for (int bin = 1; bin <= WAVVIX_MEM_BIN_FULL; bin++) {
    mem_tcache_bin_t* tc = &t->tcache[bin - 1];
    while (tc->head != NULL) {
      uintptr_t* w = (uintptr_t*)tc->head;
      mem_page_t* page = mem_ptr_page(w);
      if (WAVVIX_MEM_UNLIKELY(page == NULL))
        mem_fatal_ptr("tcache flush: block has no live page", w);
      if (WAVVIX_MEM_UNLIKELY(page->kind != WAVVIX_MEM_PAGE_SMALL || page->bin != bin))
        mem_fatal_page("tcache flush: block belongs to another bin or page kind", w, page);
      if (WAVVIX_MEM_UNLIKELY(w[1] != (mem_tcache_guard ^ (uintptr_t)w)))
        mem_fatal_ptr("use-after-free write or tcache corruption", w);
      uintptr_t next = w[0] ^ t->tcache_key;
      if (WAVVIX_MEM_UNLIKELY((next & 0xFull) != 0))
        mem_fatal_ptr("tcache chain corrupted", w);
#if defined(WAVVIX_MEM_DEBUG)
      mem_debug_verify_tcache_block(w, page->block_size);
#endif
      w[1] = 0;
      tc->head = (void*)next;
      mem_free_to_page(t, page, w);
    }
    tc->count = 0;
  }
}

} } } // namespace wavvix::mem::detail

void wavvix::mem::free(void* p) noexcept {
  using namespace detail;
#if defined(WAVVIX_MEM_DEBUG)
  p = mem_l2_unwrap(p);   /* 校验首尾 canary，还原块首 */
#endif
  if (WAVVIX_MEM_UNLIKELY(p == NULL)) return;
  mem_page_t* page = mem_ptr_page(p);
  /* L0：窗口内且描述符 magic 匹配（两者均由 mem_ptr_page 完成）*/
  if (WAVVIX_MEM_UNLIKELY(page == NULL))
    mem_fatal_ptr("invalid free (address is not a live culib mem block)", p);
  if (WAVVIX_MEM_UNLIKELY(page->kind == WAVVIX_MEM_PAGE_LARGE)) {
    mem_page_free_large(page, p);
    return;
  }
  /* 后续 small 路径以 bin 在 1..36 为前提。 */
  if (WAVVIX_MEM_UNLIKELY(page->kind != WAVVIX_MEM_PAGE_SMALL))
    mem_fatal_page("invalid free (descriptor is not a live page)", p, page);

  const size_t offset = (size_t)((uint8_t*)p - page->slice_start);
  if (WAVVIX_MEM_UNLIKELY(offset >= (size_t)page->capacity * page->block_size))
    mem_fatal_page("free outside small block range", p, page);

  /* 曾有对齐块的页按网格找回块首；块内指针必须有定位标记。 */
  if (WAVVIX_MEM_UNLIKELY(page->has_aligned.load(std::memory_order_relaxed) != 0)) {
    uint8_t* const block = mem_small_block_start(page, p);
#if !defined(WAVVIX_MEM_DEBUG)
    if (p != block) {
      if (WAVVIX_MEM_UNLIKELY((size_t)((uint8_t*)p - block) < sizeof(uintptr_t)))
        mem_fatal_page("free of interior pointer (small block)", p, page);
      uintptr_t cookie = 0;
      std::memcpy(&cookie, (uint8_t*)p - sizeof(cookie), sizeof(cookie));
      if (WAVVIX_MEM_UNLIKELY(cookie != mem_aligned_cookie(p)))
        mem_fatal_page("free of interior pointer (small block)", p, page);
    }
#endif
    p = block;
  }
  else if (WAVVIX_MEM_UNLIKELY(offset % page->block_size != 0)) {
    mem_fatal_page("free of interior pointer (small block)", p, page);
  }

  mem_theap_t* t = mem_t_theap;
  /* tcache 快路径（release 与 WAVVIX_MEM_DEBUG 均启用）。
   * 块在 tcache 期间页视角仍为已分配（used 不变）。 */
  if (t != NULL) {
    int bin = page->bin;
    /* 最小块已是 16B（mem_bin 线性区取偶），tcache 元数据（编码链 + guard）
     * 恒能放下，无需再按 block_size 设门槛。 */
    if ((unsigned)(bin - 1) < (unsigned)WAVVIX_MEM_BIN_FULL) {
      mem_tcache_bin_t* tc = &t->tcache[bin - 1];
      if (tc->count < mem_tcache_cap(page->block_size)) {
        uintptr_t* w = (uintptr_t*)p;
        /* double-free（块已在某个 tcache 里）：guard 已置位 */
        mem_tcache_check_guard(p);
        /* double-free（块在页的空闲链里）：L1 编码哨兵可检出 */
        mem_secure_check_free(page, p);
        w[0] = t->tcache_key ^ (uintptr_t)tc->head;
        w[1] = mem_tcache_guard ^ (uintptr_t)w;
#if defined(WAVVIX_MEM_DEBUG)
        /* L2：元数据之后一路填到块尾，pop 时按同一区间校验（UAF 写探测）。
         * 必须在写完 w[0]/w[1] 之后——区间虽不相交，顺序写清楚更难写错。
         * 这块若之后被 mem_tcache_flush_all 推到页空闲链，mem_free_to_page 会
         * 用页侧区间 [WAVVIX_MEM_L2_LINK, bs - WAVVIX_MEM_L2_TAIL) 重填一遍，把 [8,16) 上
         * 残留的 guard 覆盖掉——页侧 pop 的校验因此不会误报。 */
        {
          const size_t bs = page->block_size;
          if (bs > WAVVIX_MEM_L2_TCACHE_META)
            std::memset(w + 2, 0xDF, bs - WAVVIX_MEM_L2_TCACHE_META);
        }
#endif
        tc->head = w;
        tc->count++;
        return;
      }
    }
  }
  mem_free_to_page(t, page, p);
}
