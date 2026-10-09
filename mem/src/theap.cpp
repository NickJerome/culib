/* ----------------------------------------------------------------------------
 * theap.cpp — 线程本地堆、分配/释放路径、并发模型（SPEC §4.5/§4.6/§4.7）
 *
 * 核心不变式：
 *   - 每 page 恰一 owner 线程；owner 操作 free/local_free/used 零锁零原子；
 *   - 跨线程 free 恰好一次 CAS 压 xthread_free；
 *   - 线程退出 = abandon（摘链 + 置标志 + 登记 arena 桶）；
 *   - reclaim 分配驱动兜底（ed_page_fresh 先查 abandoned 桶）。
 * ------------------------------------------------------------------------- */
#include "internal.h"

#include <cstring>

/* 快路径直接读取的 TLS 指针（internal.h 中 inline） */
thread_local ed_theap_t* ed_t_theap = NULL;

std::atomic<uint64_t> g_dbg_to_full(0), g_dbg_abandon(0), g_dbg_reclaim(0), g_dbg_unfull(0), g_dbg_retire(0), g_dbg_fresh_slice(0);

static void ed_tcache_flush_all(ed_theap_t* t);

namespace {

/* 线程退出钩子。由 OS 层的线程退出回调驱动（Windows FlsAlloc / POSIX
 * pthread_key_create），theap 指针作为**参数**传进来。
 *
 * 为什么不能用 thread_local 对象的析构函数——这一条是拿崩溃换来的：
 * MinGW-w64 上 __cxa_thread_atexit 登记的析构函数执行时，该线程的 TLS 存储
 * 已被释放，`this` 是悬垂指针。第一版（析构里读 ed_t_theap）恒读到 NULL，
 * 钩子静默空转，test_stress 跑完有 376 MiB 卡在已死线程的页队列里。
 * 第二版（把指针存进 guard 对象自身、改读 this->t）看似修好了，实则更糟：
 * 那块存储同样已被回收，读到的可能是别的线程那份。实测证据——
 *   PROBE: dtor on tid=5408  but guard was filled by tid=0     (theap 非 NULL)
 *   PROBE: dtor on tid=40924 but guard was filled by tid=1568  (theap 0x...801 非对齐)
 *   PROBE: 同一个 theap 被两个不同 tid 的析构各处理一次 -> 双重推入回收链
 * 后果是并发线程 churn 下 MinGW release 8/10 崩溃（SIGSEGV / "double free
 * detected" / "tcache corruption"），而基线 0/10。
 *
 * 结论：退出钩子里**不能从 TLS 读任何东西**。回调把值当参数传进来就没有这个
 * 问题。ed_t_theap 不在这里清——线程正在退出，没有后续访问，而写它同样要碰
 * 那块已失效的 TLS。 */
static void ed_thread_exit_cb(void* value) {
  ed_theap_t* t = (ed_theap_t*)value;
  if (t == NULL) return;
  /* 必须清 ed_t_theap，而且这不是收尾卫生、是正确性：下面 ed_theap_abandon
   * 会把 t 压进全局回收链，那一刻起别的线程随时可能取走它并 memset 重置。
   * 而本线程在钩子之后还可能分配——更晚的 pthread key 析构、thread_local
   * 对象的析构都排在后面——那时 ed_theap_fast() 看见 ed_t_theap 非空就直接
   * 返回 t，于是两个线程共用同一份 tcache 与页队列。实测漏清的代价：
   *   MSVC release 19/20 崩、ED_DEBUG 20/20 崩；Linux release 3/20；
   *   形态是 "tcache chain corrupted" / "double free detected" / SIGSEGV。
   * 这与本函数存在的理由（Task 9 那个 bug）完全同源：都是同一个 theap 被两个
   * 线程同时持有，只是那一版从失效 TLS 里*读*出了别人的 theap，这一版是*留*了
   * 自己的没清。
   *
   * MinGW 恰好不崩（0/20），但那是因为它身上带着原 bug 的另一半——FLS 回调
   * 执行时它的 ed_t_theap 已读回 NULL。**不要据此认为 Windows 不需要这次清零**：
   * MSVC 上这块 TLS 完全有效、读回的就是 t，是最需要它的平台。
   * （两种存储不是一回事：ed_t_theap 是裸 thread_local 指针，走原生 TLS 槽；
   *   被本函数取代的那个 EdThreadGuard 是带析构的 thread_local 对象，走
   *   __cxa_thread_atexit/emutls。生命周期按工具链各不相同。）
   *
   * 写成"读-比较-再写"而非无条件写：t 只会出现在它自己线程的槽里，所以
   * "读回的值 == t" 蕴含"这个槽就是本线程的、且 TLS 有效"。TLS 失效时读回的
   * 要么是 NULL 要么是别人的 theap，都 != t，于是不写——无条件写会把 NULL
   * 打到无辜线程的槽上。 */
  if (ed_t_theap == t) ed_t_theap = NULL;
  ed_tcache_flush_all(t);
  ed_theap_abandon(t);
}

}  /* namespace */

/* 由 ed_init_impl 调用一次。失败（FLS 槽位或 pthread key 耗尽）不致命，但线程
 * 退出将不再归还页——那正是改造前 376 MiB 卡死的形态，所以值得在这里留一笔。 */
void ed_theap_exit_hook_init(void) {
  ed_os_thread_exit_init(ed_thread_exit_cb);
}

namespace {

/* theap 结构回收链（线程 churn 不泄漏元数据） */
std::mutex   g_theap_recycle_lock;
ed_theap_t*  g_theap_recycle = NULL;

/* ------------------------- 页退休（SPEC §5.3/§6.3） --------------------- */

void ed_theap_retire_page(ed_theap_t* t, ed_page_t* page) {
  g_dbg_retire++;
  int bin = page->bin;
  if ((page->flags & ED_PAGE_FLAG_IN_FULL) != 0) {
    ed_page_queue_remove(&t->pages[ED_BIN_FULL], page);
    t->full_count[bin]--;
    page->flags &= (uint8_t)~ED_PAGE_FLAG_IN_FULL;
  } else {
    ed_page_queue_remove(&t->pages[bin - 1], page);
  }
  if (bin < ED_DIRECT_MAX && t->pages_free_direct[bin] == page)
    t->pages_free_direct[bin] = NULL;

  ed_page_release_slices(page);   /* 清 magic + 归还 slice */
}

/* 心跳驱动的退休宽限扫描：每 16 次本地 free 扫一次本 bin */
void ed_theap_retire_scan(ed_theap_t* t, int bin) {
  ed_page_queue_t* q = &t->pages[bin - 1];
  ed_page_t* page = q->first;
  while (page != NULL) {
    ed_page_t* next = page->next;
    if (page->used == 0 && page->retire_expire > 0) {
      page->retire_expire--;
      /* 到期且本 bin 不止这一页 → 真正退休（每 bin 至少留 1 页） */
      if (page->retire_expire == 0 &&
          q->count + t->full_count[bin] > 1) {
        ed_theap_retire_page(t, page);
      }
    }
    page = next;
  }
}

/* used==0 的页：立即退休（bin 内还有其它页）或进入宽限 */
void ed_theap_page_empty(ed_theap_t* t, ed_page_t* page) {
  int bin = page->bin;
  bool in_full = (page->flags & ED_PAGE_FLAG_IN_FULL) != 0;
  size_t total = t->pages[bin - 1].count + t->full_count[bin];
  if (total > 1 || in_full) {
    ed_theap_retire_page(t, page);
  } else {
    page->retire_expire = ED_RETIRE_EXPIRE;
  }
}

void ed_theap_page_to_full(ed_theap_t* t, ed_page_t* page) {
  g_dbg_to_full++;
  int bin = page->bin;
  ed_page_queue_remove(&t->pages[bin - 1], page);
  if (bin < ED_DIRECT_MAX && t->pages_free_direct[bin] == page)
    t->pages_free_direct[bin] = NULL;
  /* 满页隔离即终点，不做"超出 retain 即 abandon"：
   * 实测（larson 类负载）满页往往仍在高速轮换，abandon 会把其后每次本地
   * free 变成跨线程 CAS + arena 锁认领，造成 abandon/reclaim 风暴
   * （5s 内 250 万次往返，吞吐掉到 glibc 的 1/3）。mimalloc 同名选项
   * （page_full_retain）的语义实为"扫描期容忍的满页数"而非满页队列上限——
   * 设计文档 §4.2 此处与 mimalloc 实际行为不符，按性能验收数字修正机制，
   * 本实现因此不提供该选项。
   * 满页腾出空间经 unfull 自然回流，真正空闲的页由 retire 宽限回收。 */
  page->flags |= ED_PAGE_FLAG_IN_FULL;
  t->full_count[bin]++;
  ed_page_queue_push(&t->pages[ED_BIN_FULL], page);
}

/* 满页腾空 → 移回本 bin 普通队列 */
void ed_theap_unfull(ed_theap_t* t, ed_page_t* page) {
  g_dbg_unfull++;
  int bin = page->bin;
  ed_page_queue_remove(&t->pages[ED_BIN_FULL], page);
  t->full_count[bin]--;
  page->flags &= (uint8_t)~ED_PAGE_FLAG_IN_FULL;
  ed_page_queue_push(&t->pages[bin - 1], page);
}

/* FULL 队列里本 bin 的页可能被跨线程释放腾出了空间：collect 试试 */
ed_page_t* ed_theap_unfull_scan(ed_theap_t* t, int bin) {
  ed_page_queue_t* fq = &t->pages[ED_BIN_FULL];
  int scanned = 0;
  ed_page_t* page = fq->first;
  while (page != NULL && scanned < ED_PAGE_MAX_CANDIDATES) {
    ed_page_t* next = page->next;
    if (page->bin == bin) {
      scanned++;
      ed_page_free_collect(page);
      if (page->used == 0) {
        ed_theap_retire_page(t, page);   /* 全空：直接退休 */
      } else if (page->free != NULL) {
        ed_theap_unfull(t, page);
        return page;
      }
    }
    page = next;
  }
  return NULL;
}

/* ------------------------- ed_page_fresh（SPEC §5.2.3） ----------------- */

ed_page_t* ed_page_fresh(ed_theap_t* t, int bin) {
  /* 1) reclaim 优先：本 arena 的 abandoned 桶 */
  ed_arena_t* arena = ed_arena_get(t->arena_idx);
  if (arena != NULL) {
    for (;;) {
      ed_page_t* page = ed_arena_reclaim_page(arena, bin);
      if (page == NULL) break;
      /* 接管所有权 */
      g_dbg_reclaim++;
      /* 只清 IN_FULL：HUGE/GUARDED/HAS_ALIGNED 是页本身的属性，PRIVATE 由
       * ed_page_set_owner 按新 owner 重算，一律清零会丢失这些语义。 */
      page->flags &= (uint8_t)~ED_PAGE_FLAG_IN_FULL;
      ed_page_set_owner(page, t);
      page->retire_expire = 0;
      ed_page_free_collect(page);   /* 收进积压的跨线程释放 */
      if (page->free != NULL) {
        ed_page_queue_push(&t->pages[bin - 1], page);
        return page;
      }
      if (page->used == 0) {
        /* 全空的被弃页：直接退休归还 slice，继续找 */
        ed_page_release_slices(page);
        continue;
      }
      /* 仍全满：挂进 FULL 队列（可能再度触发 abandon） */
      page->flags |= ED_PAGE_FLAG_IN_FULL;
      t->full_count[bin]++;
      ed_page_queue_push(&t->pages[ED_BIN_FULL], page);
      return page;   /* 让调用方走"再找一页"逻辑 */
    }
  }
  return NULL;
}

} /* anonymous namespace */

/* ------------------------- theap 生命周期 ------------------------------ */

ed_theap_t* ed_theap_create(void) {
  ed_init_once();

  ed_theap_t* t = NULL;
  {
    std::lock_guard<std::mutex> lock(g_theap_recycle_lock);
    if (g_theap_recycle != NULL) {
      t = g_theap_recycle;
      g_theap_recycle = t->next_free;
    }
  }
  if (t == NULL) t = (ed_theap_t*)ed_meta_alloc(sizeof(ed_theap_t));
  if (t == NULL) return NULL;
  std::memset(t, 0, sizeof(*t));
  for (int bin = 1; bin <= ED_BIN_FULL; bin++)
    t->pages[bin - 1].block_size = ed_bin_size(bin);
  t->pages[ED_BIN_FULL].block_size = 0;
  t->thread_id = ed_next_thread_id();
  t->heap = ed_default_heap();
  t->random_state = ed_random_seed(ed_global_seed() ^ t->thread_id ^ (uintptr_t)t);
  t->tcache_key   = ed_random_next(&t->random_state);
  t->guarded_countdown = (uint64_t)ed_options.guarded_sample_rate;
  if (ed_arena_pick_for_thread(t) < 0) return NULL;   /* 确保至少一个 arena */
  ed_t_theap = t;
  /* 线程退出钩子登记。放在最后一步：上面任何一个失败返回都不该留下一个
   * "登记了却没建成"的 theap。 */
  ed_os_thread_exit_set(t);
  return t;
}

ed_theap_t* ed_theap_current(void) {
  ed_theap_t* t = ed_t_theap;
  if (ED_UNLIKELY(t == NULL)) t = ed_theap_create();
  return t;
}

/* 线程退出 = abandon（SPEC §7.2）：O(页数) 摘链、置标志、登记桶，无锁 */
void ed_theap_abandon(ed_theap_t* t) {
  for (int bin = 1; bin < ED_DIRECT_MAX; bin++)
    t->pages_free_direct[bin] = NULL;
  for (int qi = 0; qi < ED_BIN_COUNT; qi++) {
    ed_page_queue_t* q = &t->pages[qi];
    while (q->first != NULL) {
      ed_page_t* page = q->first;
      ed_page_queue_remove(q, page);
      int bin = page->bin;
      if (qi == ED_BIN_FULL && t->full_count[bin] > 0) t->full_count[bin]--;
      page->flags &= (uint8_t)~ED_PAGE_FLAG_IN_FULL;
      /* 先收一次：已全空的页直接归还 slice，不必进 abandoned 桶再等 reclaim */
      ed_page_free_collect(page);
      if (page->used == 0) {
        ed_page_release_slices(page);
        continue;
      }
      page->theap = NULL;
      page->xthread_id.fetch_or(ED_PAGE_XTHREAD_ABANDONED,
                                std::memory_order_acq_rel);
      ed_arena_t* arena = ed_arena_get(page->arena_idx);
      if (arena != NULL) ed_arena_abandon_page(arena, page);
      g_dbg_abandon.fetch_add(1, std::memory_order_relaxed);
    }
  }
  /* large 页不在队列中：free 路径按 kind 直接处理，无需 abandon（SPEC §4.6） */
  /* 回收 theap 结构本身（线程 churn 不泄漏元数据） */
  std::lock_guard<std::mutex> lock(g_theap_recycle_lock);
  t->next_free = g_theap_recycle;
  g_theap_recycle = t;
}

/* ------------------------- 强制收集（ed_collect 用） ------------------- */

/* collect 所有页并退休全部空页（无视"每 bin 留一页"，调用方明确要求收缩） */
void ed_theap_collect(ed_theap_t* t) {
  ed_tcache_flush_all(t);   /* 先把 tcache 滞留块归还到页，collect 才能看到真空页 */
  for (int qi = 0; qi < ED_BIN_COUNT; qi++) {
    ed_page_queue_t* q = &t->pages[qi];
    ed_page_t* page = q->first;
    while (page != NULL) {
      ed_page_t* next = page->next;
      ed_page_free_collect(page);
#if defined(ED_DEBUG)
      /* collect 刚把两条链并完，此刻页内空闲块全在 free/local_free 上：
       * 这是 used/capacity 记账唯一能被廉价全量复核的时刻。必须在下面的
       * retire 之前——retire 会归还 slice，描述符随即失效。 */
      ed_page_check_invariants(page);
#endif
      if (page->used == 0) {
        ed_theap_retire_page(t, page);
      } else if ((page->flags & ED_PAGE_FLAG_IN_FULL) != 0 &&
                 page->free != NULL) {
        ed_theap_unfull(t, page);
      }
      page = next;
    }
  }
}

/* 私有堆销毁：归还全部页（含 large_list）。用户契约：堆内块不再使用。 */
void ed_theap_destroy_all_pages(ed_theap_t* t) {
  for (int qi = 0; qi < ED_BIN_COUNT; qi++) {
    ed_page_queue_t* q = &t->pages[qi];
    while (q->first != NULL) {
      ed_page_t* page = q->first;
      ed_theap_retire_page(t, page);   /* 摘链 + 归还 slice + 回收描述符 */
    }
  }
  for (int bin = 1; bin < ED_DIRECT_MAX; bin++) t->pages_free_direct[bin] = NULL;
  while (t->large_list != NULL) {
    ed_page_t* page = t->large_list;
    t->large_list = page->next;
    page->next = page->prev = NULL;
    ed_page_release_slices(page);
  }
}

/* fork 钩子：prepare 时按固定顺序上锁，child 里重建（持锁线程已不存在） */
void ed_theap_recycle_lock(void)   { g_theap_recycle_lock.lock(); }
void ed_theap_recycle_unlock(void) { g_theap_recycle_lock.unlock(); }

void ed_theap_reinit_locks_child(void) {
  new (&g_theap_recycle_lock) std::mutex();
}

/* ------------------------- 分配慢路径（SPEC §5.2） --------------------- */

void* ed_malloc_generic(ed_theap_t* t, int bin) {
  t->heartbeat++;

  ed_arena_t* arena = ed_arena_get(t->arena_idx);
  /* 注：purge 到期检查摊销在下方"到达 arena 分配新 slice"的分支里，
   * 不在每次 generic 调用做——purge_expire 几乎总被 retire 重新武装，
   * 每次调用都 clock_gettime 实测白付 ~11ns（gprof 占 9%）。 */

  /* 0) 分配驱动兜底：本 bin 的 abandoned 桶非空 → 优先认领一页。
   *    否则满页压力降级产生的被弃页只进不出，内存会随时间膨胀。 */
  if (arena != NULL && arena->abandoned[bin - 1] != NULL) {
    ed_page_t* rp = ed_page_fresh(t, bin);
    if (rp != NULL && rp->free != NULL) {
      if (bin < ED_DIRECT_MAX) t->pages_free_direct[bin] = rp;
      return ed_page_malloc(rp);   /* 记账在调用方（见 internal.h 的统计记账一节） */
    }
    /* rp 为 NULL（桶空）或仍全满（已挂 FULL 队列）：继续正常路径 */
  }

  /* 1) next-fit 扫描本 bin 队列（对齐 mimalloc find_free_ex 的实际行为）：
   *    - 仅在 free==NULL 时才 collect（有空闲块的页零额外代价）；
   *    - 首个可用页即返回，不再对所有扫描页做全量 collect——
   *      实测"每页必 collect"会让 larson 类负载慢 3 倍（gprof：collect 42%）。
   *    - collect 后仍满的页隔离进 FULL 队列；扫描上限防长尾。 */
  ed_page_queue_t* q = &t->pages[bin - 1];
  ed_page_t* best = NULL;
  long scanned = 0;
  ed_page_t* page = q->first;
  while (page != NULL && scanned < ED_PAGE_MAX_CANDIDATES) {
    ed_page_t* next = page->next;
    if (page->free == NULL) ed_page_free_collect(page);
    scanned++;
    if (page->free != NULL) { best = page; break; }   /* 首个可用页即返回 */
    if (page->used == 0) {
      ed_theap_page_empty(t, page);                   /* 全空：退休判定 */
    } else {
      ed_theap_page_to_full(t, page);                 /* 仍满：隔离 */
    }
    page = next;
  }

  /* 2) FULL 队列中本 bin 的页可能已被跨线程释放腾出空间 */
  if (best == NULL) best = ed_theap_unfull_scan(t, bin);

  /* 3) reclaim abandoned / arena 新页 */
  ed_page_t* fresh = NULL;
  if (best == NULL) {
    fresh = ed_page_fresh(t, bin);
    if (fresh != NULL && fresh->free == NULL) {
      /* reclaim 到的是全满页：已挂 FULL，重新走 fresh 拿新页 */
      fresh = NULL;
    }
    if (fresh == NULL) {
      /* arena 分配 1 slice（唯一可能持锁/系统调用的环节）。
       * 摊销：既已到此慢路径，顺带做到期 purge 检查 */
      if (arena != NULL) ed_arena_purge_check(arena);
      bool zeroed = false;
      ed_arena_t* a = NULL;
      g_dbg_fresh_slice++;
      size_t sidx = ed_arena_alloc_slices_any(t, 1, 1, &a, &zeroed);
      if (sidx == (size_t)-1) return NULL;
      /* 描述符与 slice 平铺：slice 下标即描述符下标，无需分配与登记 */
      fresh = ed_desc_base + a->first_slice + sidx;
      uint8_t* start = a->base + sidx * ED_SLICE_SIZE;
      fresh->arena_idx = a->index;   /* 必须先于 ed_page_init 的 magic 发布 */
      ed_page_init(fresh, t, bin, ed_bin_size(bin), ED_PAGE_SMALL,
                   start, 1, zeroed);
      ed_page_publish(fresh);
      ed_page_queue_push(&t->pages[bin - 1], fresh);
    }
    best = fresh;
  }
  if (best == NULL || best->free == NULL) return NULL;

  if (bin < ED_DIRECT_MAX) t->pages_free_direct[bin] = best;
  /* 不在这里记账：本函数是小块的慢路径，与三条快路径同属一次 malloc，
   * 统计统一由汇合点 ed_malloc_internal_with / ed_heap_malloc_internal 记。 */
  return ed_page_malloc(best);
}

/* ------------------------- large / huge（SPEC §5.3） ------------------- */

/* large 侧的统计汇合点。三件事值得说明：
 *   - 记在 ed_t_theap（调用线程）而非入参 t：私有堆的 t 是堆内嵌的伪 theap，
 *     而释放侧只认调用线程，记在 t 上两边永远对不上。
 *   - 口径用 page->large_size，不用请求的 size：非 guarded 的 large 会向上
 *     取整到整数个 slice，而 ed_free_internal 抵扣的正是 large_size。用 size
 *     记、用 large_size 减，每块就少记一截——这是 stat_current 下溢的第二个源。
 *   - t 为 NULL（线程尚无 theap，只可能经私有堆进来）时跳过：释放侧同样跳过，
 *     不记比记半边强。 */
static void ed_stat_large_alloc(const ed_page_t* page) {
  ed_theap_t* st = ed_t_theap;
  if (st != NULL) ed_stat_alloc(st, page->large_size);
}

/* large 总入口：8KiB < size，含 huge 与 L3 guarded。
 * 全部从 arena 切连续 slice——窗口建立后再无独立映射，因此 ed_ptr_page 对
 * huge/guarded 同样是纯算术。
 *   guarded：多要一个 slice 置 PROT_NONE，对象贴其前边界放置（越界写即 SIGSEGV）；
 *   huge（> ED_HUGE_THRESHOLD）与 guarded：打 HUGE 标志，free 即 decommit。 */
void* ed_malloc_large_ex(ed_theap_t* t, size_t size, bool guarded,
                         bool* out_zeroed) {
  if (size == 0) size = 1;
  const size_t data = (size + ED_SLICE_SIZE - 1) / ED_SLICE_SIZE * ED_SLICE_SIZE;
  const size_t n = data >> ED_SLICE_SHIFT;
  const size_t total_slices = n + (guarded ? 1 : 0);

  bool zeroed = false;
  ed_arena_t* a = NULL;
  size_t sidx = ed_arena_alloc_slices_any(t, total_slices, 1, &a, &zeroed);
  if (sidx == (size_t)-1) return NULL;

  ed_page_t* page = ed_desc_base + a->first_slice + sidx;
  uint8_t* start = a->base + sidx * ED_SLICE_SIZE;
  page->arena_idx = a->index;   /* slice 的固有属性，ed_page_init 不碰 */
  ed_page_init(page, t, 0, 0, ED_PAGE_LARGE, start, (uint32_t)total_slices, zeroed);
  /* guarded 时对象贴守护页右对齐放置，起点 = guard_start - size。这个起点必须
   * 向下取整到 16 的倍数，否则 size % 16 != 0 的请求返回的指针就不是 16 字节
   * 对齐的——"ed_malloc 返回值恒 16 字节对齐"是一条无条件保证，不能退化成
   * "取决于 L3 采样没开"。等价地说：按 round_up(size,16) 放置。
   *   - start + data 本身 64KiB 对齐 ⇒ 减去 16 的倍数仍是 16 的倍数；
   *   - data 是 64KiB 的倍数、size <= data ⇒ gsize <= data，不会退出本页。
   * large_size 必须同步取整值：usable_size 与 ed_page_free_large 的等值比较都
   * 读它，少记这 0..15 字节就会分别表现为少报可用空间、和把合法释放判成内部
   * 指针。代价是对象末尾与守护页之间多出 0..15 字节空隙，这一小段的越界写不
   * 再立即 SIGSEGV（ED_DEBUG 下尾 canary 仍能在 free 时抓到）。 */
  const size_t gsize = (size + 15) & ~(size_t)15;
  page->large_size = guarded ? gsize : n * ED_SLICE_SIZE;
  if (size > ED_HUGE_THRESHOLD || guarded) page->flags |= ED_PAGE_FLAG_HUGE;
  if (guarded) {
    page->flags |= ED_PAGE_FLAG_GUARDED;
    ed_os_protect(start + n * ED_SLICE_SIZE, ED_SLICE_SIZE, false);
  }
  ed_desc_mark_cont(ed_page_slice_index(page), total_slices);
  page->used = 1;
  ed_page_publish(page);   /* 续页项先于首项发布：野指针只会解析到 magic==0 */

  /* 私有堆的 large 页挂堆链表，供 heap_destroy 整体归还（持堆锁操作） */
  if ((page->flags & ED_PAGE_FLAG_PRIVATE) != 0) {
    page->next = t->large_list;
    page->prev = NULL;
    if (t->large_list != NULL) t->large_list->prev = page;
    t->large_list = page;
  }
  ed_stat_large_alloc(page);
  if (out_zeroed != NULL) *out_zeroed = zeroed;
  /* guarded：对象右对齐贴住守护页（按取整后的 gsize，见上） */
  return guarded ? start + data - gsize : start;
}

void* ed_malloc_large(ed_theap_t* t, size_t size) {
  return ed_malloc_large_ex(t, size, false, NULL);
}

/* alignment > 64KiB 的对齐分配：位图按对齐起点选段（arena 相对下标 + 基址
 * 相位，见 ed_arena_alloc_slices）。alignment ≤ 64KiB 由 slice 天然对齐覆盖，
 * 走 ed_malloc_large_ex 即可。 */
void* ed_malloc_large_aligned(ed_theap_t* t, size_t alignment, size_t size) {
  if (size == 0) size = 1;
  const size_t data = (size + ED_SLICE_SIZE - 1) / ED_SLICE_SIZE * ED_SLICE_SIZE;
  const size_t n = data >> ED_SLICE_SHIFT;
  const size_t align_slices = alignment >> ED_SLICE_SHIFT;
  /* 夹住荒谬的 alignment：n + align_slices 是新建 arena 时要预留的 slice 数，
   * 不设上界的话它会在 << ED_SLICE_SHIFT 时回绕。 */
  const size_t window_slices = ed_window_size >> ED_SLICE_SHIFT;
  if (align_slices == 0 || align_slices > window_slices ||
      n == 0 || n > window_slices - align_slices)
    return NULL;

  bool zeroed = false;
  ed_arena_t* a = NULL;
  size_t sidx = ed_arena_alloc_slices_any(t, n, align_slices, &a, &zeroed);
  if (sidx == (size_t)-1) return NULL;

  ed_page_t* page = ed_desc_base + a->first_slice + sidx;
  uint8_t* start = a->base + sidx * ED_SLICE_SIZE;
  page->arena_idx = a->index;
  ed_page_init(page, t, 0, 0, ED_PAGE_LARGE, start, (uint32_t)n, zeroed);
  page->large_size = n * ED_SLICE_SIZE;
  if (size > ED_HUGE_THRESHOLD) page->flags |= ED_PAGE_FLAG_HUGE;
  ed_desc_mark_cont(ed_page_slice_index(page), n);
  page->used = 1;
  ed_page_publish(page);
  if ((page->flags & ED_PAGE_FLAG_PRIVATE) != 0) {
    page->next = t->large_list;
    page->prev = NULL;
    if (t->large_list != NULL) t->large_list->prev = page;
    t->large_list = page;
  }
  ed_stat_large_alloc(page);
  return start;
}

/* large/huge/guarded 的释放：整块归还，无需 owner（capacity=1） */
static void ed_page_free_large(ed_page_t* page, void* p) {
  /* 内部指针拒收，用等值比较而非"落在本页内就放行"：GUARDED 的对象贴守护页
   * 右对齐放置、不等于 slice_start，但它的地址是可精确算出来的（见
   * ed_large_block_start）。整页放行的话，guarded——安全采样模式，本该最严——
   * 反倒成了全页任意地址都收的口子。
   * 对齐族不需要例外：release 的 large 对齐分配返回的就是 slice_start，
   * ED_DEBUG 下 ed_l2_unwrap 已经把用户指针还原成块首才走到这里。 */
  const uint8_t* expect = ed_large_block_start(page);
  if (ED_UNLIKELY((const uint8_t*)p != expect))
    ed_fatal_page("free of interior pointer (large block)", p, page);

  /* 私有堆 large 页：从堆链表摘除（free 方持堆锁） */
  if ((page->flags & ED_PAGE_FLAG_PRIVATE) != 0) {
    ed_theap_t* owner = page->theap;
    if (owner != NULL && owner->heap != NULL) {
      ed_heap_t* h = owner->heap;
      h->mutex.lock();
      if (page->prev != NULL) page->prev->next = page->next;
      else if (owner->large_list == page) owner->large_list = page->next;
      if (page->next != NULL) page->next->prev = page->prev;
      page->next = page->prev = NULL;
      h->mutex.unlock();
    }
  }
  ed_page_release_slices(page);
  /* 记账在 ed_free_internal（large_size 在那里于归还之前已取出） */
}

/* ------------------------- 释放路径（SPEC §4.6） ----------------------- */

/* 页归还路径（tcache 未命中/未启用时的完整语义路径）。
 * 不记统计：本函数还会被 ed_tcache_flush_all 调用，而那些块在进 tcache 时
 * 就已经作为一次 ed_free 记过账了。统计只记在 ed_free_internal 那一处。 */
static void ed_free_to_page(ed_theap_t* t, ed_page_t* page, void* p) {
  /* 先取出后面要用的字段：本地分支里 ed_theap_page_empty 可能退休本页（描述符
   * 失效、slice 归还），跨线程分支里 CAS 一成功块就对 owner 可见、owner 可能
   * 立刻退休本页——两种情况下再解引用 page 都是读已释放的描述符。 */
  const int    bin = page->bin;
#if defined(ED_DEBUG)
  const size_t bs  = page->block_size;   /* 只有 L2 的 0xDF 填充还要用它 */
#endif
  uintptr_t owner = page->xthread_id.load(std::memory_order_relaxed);
  if (ED_LIKELY(owner == t->thread_id)) {
    /* 本地释放（≤6 条指令目标）：压 local_free + 计数 */
    ed_tcache_check_guard(p);        /* 块仍驻留某个 tcache → double free */
    ed_secure_check_free(page, p);   /* L1 double-free 编码哨兵 */
    if (ED_UNLIKELY(page->used == 0))
      ed_fatal_page("double free (page used underflow)", p, page);
    ed_block_set_next(page, (ed_block_t*)p, page->local_free);
    page->local_free = (ed_block_t*)p;
    page->used--;
    page->free_is_zero = false;
#if defined(ED_DEBUG)
    /* L2：释放填 0xDF（保留首 8B 编码域与尾部 canary），UAF 写探测。
     * 必须在压链之后：哨兵校验依赖压链写入的编码态。 */
    if (bs > 16)
      std::memset((uint8_t*)p + ED_L2_LINK, 0xDF, bs - ED_L2_LINK - ED_L2_TAIL);
#endif
    if (ED_UNLIKELY(page->used == 0)) {
      ed_theap_page_empty(t, page);            /* 退休判定 */
    } else if ((page->flags & ED_PAGE_FLAG_IN_FULL) != 0) {
      ed_theap_unfull(t, page);                /* 满页腾空 → 移回本 bin */
    }
    t->heartbeat++;
    if ((t->heartbeat & 0xF) == 0) ed_theap_retire_scan(t, bin);
  } else {
    /* 跨线程释放（含 abandoned 页）：恰好一次成功 CAS 压 xthread_free。
     * ABA 安全：本链只允许压入；整链摘走由 owner/reclaimer 用 exchange(0)。 */
    ed_tcache_check_guard(p);        /* 块仍驻留某个 tcache → double free */
    ed_secure_check_free(page, p);
#if defined(ED_DEBUG)
    /* L2：先填 0xDF 再 CAS——CAS 成功瞬间块即对 owner 可见，
     * 填充必须在此之前完成（但绝不可碰首 8B 编码域）。 */
    if (bs > 16)
      std::memset((uint8_t*)p + ED_L2_LINK, 0xDF, bs - ED_L2_LINK - ED_L2_TAIL);
#endif
    ed_block_t* b = (ed_block_t*)p;
    uintptr_t old = page->xthread_free.load(std::memory_order_relaxed);
    do {
      ed_block_set_next(page, b, (ed_block_t*)old);
    } while (!page->xthread_free.compare_exchange_weak(
                 old, (uintptr_t)b, std::memory_order_release,
                 std::memory_order_relaxed));
  }
}

/* tcache 全量冲刷：线程退出/collect 时把缓存块逐个走完整页路径归还 */
static void ed_tcache_flush_all(ed_theap_t* t) {
  for (int bin = 1; bin <= ED_BIN_FULL; bin++) {
    ed_tcache_bin_t* tc = &t->tcache[bin - 1];
    while (tc->head != NULL) {
      uintptr_t* w = (uintptr_t*)tc->head;
      uintptr_t next = w[0] ^ t->tcache_key;
      w[1] = 0;
      tc->head = (void*)next;
      ed_page_t* page = ed_ptr_page(w);
      if (page == NULL) continue;
      /* 与 ed_free_internal 同一条护栏：ed_free_to_page 里的 t->pages[bin-1]
       * 对非 small 页会越界读写 theap 自身。两个调用方必须保持一致。 */
      if (ED_UNLIKELY(page->kind != ED_PAGE_SMALL))
        ed_fatal_page("tcache flush: descriptor is not a live small page", w, page);
      ed_free_to_page(t, page, w);
    }
    tc->count = 0;
  }
}

void ed_free_internal(void* p) {
  if (ED_UNLIKELY(p == NULL)) return;
  ed_page_t* page = ed_ptr_page(p);
  /* L0：窗口内且描述符 magic 匹配（两者均由 ed_ptr_page 完成）*/
  if (ED_UNLIKELY(page == NULL))
    ed_fatal_ptr("invalid free (address is not a live edumalloc block)", p);
  if (ED_UNLIKELY(page->kind == ED_PAGE_LARGE)) {
    /* large_size 必须在归还之前取：ed_page_free_large 会清 magic 并把 slice
     * 还给 arena，之后这个描述符可能已被别的页征用。 */
    const size_t usable = page->large_size;
    ed_page_free_large(page, p);
    ed_theap_t* lt = ed_t_theap;
    if (lt != NULL) ed_stat_free(lt, usable);   /* 口径见 ed_stat_large_alloc */
    return;
  }
  /* L0 兜底：下面整条 small 路径都假定 kind == SMALL——尤其 bin 落在 1..36，
   * 否则 ed_free_to_page 的 t->pages[bin - 1] 会越界读写 theap 结构本身
   * （large 页 bin == 0）。ed_ptr_page 已经保证不会返回续页项，这里把那条
   * 跨模块的隐式契约变成使用点上一次可预测的比较：kind 与上面那次判断读的是
   * 同一个字节、同一条 cache line，代价在噪声以下。 */
  if (ED_UNLIKELY(page->kind != ED_PAGE_SMALL))
    ed_fatal_page("invalid free (descriptor is not a live page)", p, page);

  /* 对齐族：ed_aligned_alloc 交出去的指针落在块内部而不是块首，按块网格找回。
   * 只对"曾切出过对齐块"的页做（HAS_ALIGNED 由 ed_aligned_alloc 打上，页归还
   * 重建时随 ed_page_init 清零）。代价要说清楚：这些页上真正的内部指针 free 在
   * release 构建里会被静默纠正而不是报错，L0 的检出强度在此类页上确有下降；
   * 换成无条件找回则是把全部 small 页都降到这个强度，那才是不能接受的。
   * ED_DEBUG 不受影响：头 canary 紧贴用户指针，内部指针在 ed_l2_unwrap 就已被
   * 抓住，走到这里的 p 必定已是块首——下面那条整除校验就是这句话的兜底。
   *
   * 那条整除校验只在 ED_DEBUG 存在，这是**有意的取舍，不是漏加**：
   *   - bin 尺寸是 16/32/48/64/80/96/112/128/160/192... 并不全是 2 的幂，
   *     `off % block_size` 里的 block_size 又不是编译期常量，所以它在 release
   *     的 free 快路径（目标 ≤6 条指令）上就是一次实打实的硬件除法，没有便宜的
   *     等价写法；
   *   - 非 HAS_ALIGNED 页上，内部指针 free 仍有 L1 编码哨兵兜底
   *     （ed_secure_check_free 解码出页内地址即报 double free）；
   *   - HAS_ALIGNED 页上则确实没有：release 下内部指针会被上面那次向下取整
   *     **静默纠正**成合法块首，调用方的 bug 不会当场暴露。这是为了不让小对齐
   *     分配吃掉整个 slice 所付的代价，范围被限制在"曾切出过对齐块"的页上。
   * 想改成 release 也检查，请连带评估 free 快路径的除法开销，而不是直接把
   * #if 去掉。 */
  if (ED_UNLIKELY((page->flags & ED_PAGE_FLAG_HAS_ALIGNED) != 0)) {
    p = ed_small_block_start(page, p);
  }
#if defined(ED_DEBUG)
  else if ((size_t)((uint8_t*)p - page->slice_start) % page->block_size != 0) {
    ed_fatal_page("free of interior pointer (small block)", p, page);
  }
#endif

  ed_theap_t* t = ed_theap_current();
  /* 小块释放的统计汇合点：无论块进 tcache 还是直接还页，都只记这一次。
   * block_size 在页可能被退休之前读出（本行之后 ed_free_to_page 可能归还它）。 */
  ed_stat_free(t, page->block_size);
  /* tcache 快路径（release 与 ED_DEBUG 均启用）：L0 魔数校验已过；
   * 私有堆页绕过——heap_destroy 无条件归还页，滞留块会成 UAF 源。
   * 块在 tcache 期间页视角仍为已分配（used 不变），退休/统计语义不变。 */
  {
    int bin = page->bin;
    /* 最小块已是 16B（ed_bin 线性区取偶），tcache 元数据（编码链 + guard）
     * 恒能放下，无需再按 block_size 设门槛。 */
    if ((unsigned)(bin - 1) < (unsigned)ED_BIN_FULL &&
        (page->flags & ED_PAGE_FLAG_PRIVATE) == 0) {
      ed_tcache_bin_t* tc = &t->tcache[bin - 1];
      if (tc->count < ed_tcache_cap(page->block_size)) {
        uintptr_t* w = (uintptr_t*)p;
        /* double-free（块已在某个 tcache 里）：guard 已置位 */
        ed_tcache_check_guard(p);
        /* double-free（块在页的空闲链里）：L1 编码哨兵可检出 */
        ed_secure_check_free(page, p);
        w[0] = t->tcache_key ^ (uintptr_t)tc->head;
        w[1] = ed_tcache_guard ^ (uintptr_t)w;
#if defined(ED_DEBUG)
        /* L2：元数据之后一路填到块尾，pop 时按同一区间校验（UAF 写探测）。
         * 必须在写完 w[0]/w[1] 之后——区间虽不相交，顺序写清楚更难写错。
         * 这块若之后被 ed_tcache_flush_all 推到页空闲链，ed_free_to_page 会
         * 用页侧区间 [ED_L2_LINK, bs - ED_L2_TAIL) 重填一遍，把 [8,16) 上
         * 残留的 guard 覆盖掉——页侧 pop 的校验因此不会误报。 */
        {
          const size_t bs = page->block_size;
          if (bs > ED_L2_TCACHE_META)
            std::memset(w + 2, 0xDF, bs - ED_L2_TCACHE_META);
        }
#endif
        tc->head = w;
        tc->count++;
        return;
      }
    }
  }
  ed_free_to_page(t, page, p);
}
