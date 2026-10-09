/* ----------------------------------------------------------------------------
 * arena.cpp — 地址窗口、平铺描述符数组、arena 切分、到期 purge、abandoned 桶
 *             （SPEC §4.3/§4.7/§4.8）
 *
 * 一次性保留一整片地址窗口（默认 1 TiB，失败逐级减半，下限 4 GiB）与一片
 * 与之等长的描述符数组（每 64KiB slice 一个 128B 描述符）。arena 只是窗口里
 * 顺序切下的一段连续 slice，因此：
 *   - 指针 → 描述符 是纯算术（见 internal.h 的 ed_ptr_page），没有查表层；
 *   - 描述符不再需要池、索引与回收，slice i 的描述符恒为 ed_desc_base[i]；
 *   - 描述符按 arena 粒度提交（ED_ARENA_MIN=2MiB → 32 slice → 4KiB 描述符），
 *     提交后内容保证为零，magic==0 即"该 slice 上没有活页"。
 *
 * slice 两态回收：dirty（物理页仍持有，可复用）→ decommitted（已归还内核）。
 * purge 到期检查摊销在分配慢路径，无后台线程。
 * ------------------------------------------------------------------------- */
#include "internal.h"
#include "bitmap.h"

#include <new>

/* 诊断计数器（ed_stats_print 展示；跨 TU 引用，不放进匿名 namespace） */
std::atomic<uint64_t> g_dbg_slice_alloc(0);
std::atomic<uint64_t> g_dbg_slice_free(0);

/* 地址窗口与平铺描述符数组（internal.h 声明；ed_ptr_page 内联读取） */
uint8_t*            ed_window_base = NULL;
size_t              ed_window_size = 0;
ed_page_t*          ed_desc_base   = NULL;
std::atomic<size_t> ed_window_used_slices(0);

namespace {

std::mutex            g_subproc_lock;                   /* arena 创建/全局 */
ed_arena_t*           g_arenas[ED_ARENA_COUNT_MAX];
std::atomic<int>      g_arena_count(0);
std::atomic<uint32_t> g_arena_round_robin(0);
size_t                g_arena_next_size = 0;            /* 下一个 arena 的保留大小 */

size_t g_window_slices  = 0;    /* 窗口总 slice 数 */
size_t g_desc_reserve   = 0;    /* 描述符数组保留字节数 */
size_t g_desc_committed = 0;    /* 描述符数组已提交字节数（单调递增） */

inline size_t ed_round_up(size_t v, size_t granule) {
  return (v + granule - 1) / granule * granule;
}

/* 描述符按 arena 提交：窗口 slice 是顺序切分的，故只需一个单调水位线。
 * 提交后内容保证全零——ed_arena_create 依赖它（magic 为 0 = 无活页）。
 * 调用方持 g_subproc_lock。 */
bool ed_desc_commit_upto(size_t end_slice) {
  size_t need = ed_round_up(end_slice * sizeof(ed_page_t), ed_os_page_size());
  /* 越界只可能来自调用方算错 end_slice：ed_arena_create 已用
   * start + n > g_window_slices 挡在前面，所以这里不可达。静默截断会让
   * 描述符区间悄悄少提交一段，之后写进去就是越界——宁可当场死。 */
  if (ED_UNLIKELY(need > g_desc_reserve))
    ed_os_fatal("descriptor commit beyond reserved range (bad end_slice)");
  if (need <= g_desc_committed) return true;
  if (!ed_os_commit((uint8_t*)ed_desc_base + g_desc_committed,
                    need - g_desc_committed))
    return false;
  g_desc_committed = need;
  return true;
}

/* 从窗口里顺序切下一段 arena。调用方持 g_subproc_lock；size 已是 2MiB 倍数。 */
ed_arena_t* ed_arena_create(size_t size) {
  int count = g_arena_count.load(std::memory_order_relaxed);
  if (count >= ED_ARENA_COUNT_MAX) return NULL;
  if (ed_window_base == NULL) return NULL;

  size_t n = size >> ED_SLICE_SHIFT;
  size_t start = ed_window_used_slices.load(std::memory_order_relaxed);
  if (n == 0 || start + n > g_window_slices) return NULL;
  if (!ed_desc_commit_upto(start + n)) return NULL;

  size_t words = (n + 63) / 64;
  ed_arena_t*   arena        = (ed_arena_t*)ed_meta_alloc(sizeof(ed_arena_t));
  ed_bbitmap_t* bm_free      = (ed_bbitmap_t*)ed_meta_alloc(sizeof(ed_bbitmap_t));
  ed_bitmap_t*  bm_committed = (ed_bitmap_t*)ed_meta_alloc(sizeof(ed_bitmap_t));
  ed_bitmap_t*  bm_dirty     = (ed_bitmap_t*)ed_meta_alloc(sizeof(ed_bitmap_t));
  ed_bitmap_t*  bm_purge     = (ed_bitmap_t*)ed_meta_alloc(sizeof(ed_bitmap_t));
  uint64_t* st_free      = (uint64_t*)ed_meta_alloc(words * 8);
  uint64_t* st_committed = (uint64_t*)ed_meta_alloc(words * 8);
  uint64_t* st_dirty     = (uint64_t*)ed_meta_alloc(words * 8);
  uint64_t* st_purge     = (uint64_t*)ed_meta_alloc(words * 8);
  if (arena == NULL || bm_free == NULL || bm_committed == NULL ||
      bm_dirty == NULL || bm_purge == NULL || st_free == NULL ||
      st_committed == NULL || st_dirty == NULL || st_purge == NULL)
    return NULL;

  arena->base        = ed_window_base + (start << ED_SLICE_SHIFT);
  arena->slice_count = n;
  arena->first_slice = start;
  arena->index       = (uint16_t)count;
  arena->slices_free      = bm_free;
  arena->slices_committed = bm_committed;
  arena->slices_dirty     = bm_dirty;
  arena->slices_purge     = bm_purge;
  ed_bbitmap_init(bm_free, st_free, n);
  ed_bitmap_init(bm_committed, st_committed, n, false);
  ed_bitmap_init(bm_dirty, st_dirty, n, false);
  ed_bitmap_init(bm_purge, st_purge, n, false);
  arena->purge_expire.store(0, std::memory_order_relaxed);
  /* std::mutex 是 meta 池里的 POD 初始化——placement new 显式构造 */
  new (&arena->mutex) std::mutex();
  for (int i = 0; i < ED_BIN_COUNT; i++) arena->abandoned[i] = NULL;

  if (ed_options.eager_commit) {
    if (!ed_os_commit(arena->base, size)) return NULL;
    ed_bitmap_set_range(bm_committed, 0, n);
  }

  g_arenas[count] = arena;
  g_arena_count.store(count + 1, std::memory_order_release);
  /* 最后推进水位线：ed_ptr_page 以它为准入门槛，描述符必须先就绪 */
  ed_window_used_slices.store(start + n, std::memory_order_release);
  return arena;
}

} /* anonymous namespace */

/* ------------------------------ 窗口建立 ------------------------------- */

/* 保留窗口 + 描述符数组；窗口保留失败逐级减半，下限 ED_WINDOW_MIN。
 * 描述符数组大小 = 窗口 slice 数 × 128B（1 TiB 窗口 → 2 GiB 保留，仅地址空间）。 */
bool ed_window_init(size_t want) {
  size_t size = want < ED_WINDOW_MIN ? ED_WINDOW_MIN
                                     : (want & ~(ED_ARENA_MIN - 1));
  for (;;) {
    void* base = NULL;
    if (ed_os_reserve(size, &base)) {
      size_t slices = size >> ED_SLICE_SHIFT;
      size_t dres = ed_round_up(slices * sizeof(ed_page_t), ED_OS_RESERVE_ALIGN);
      void* d = NULL;
      if (ed_os_reserve(dres, &d)) {
        ed_window_base  = (uint8_t*)base;
        ed_window_size  = size;
        g_window_slices = slices;
        ed_desc_base    = (ed_page_t*)d;
        g_desc_reserve  = dres;
        g_desc_committed = 0;
        return true;
      }
      ed_os_release(base, size);
    }
    if (size <= ED_WINDOW_MIN) return false;
    size >>= 1;
  }
}

/* ------------------------------ arena 查询 ----------------------------- */

int ed_arena_count(void) {
  return g_arena_count.load(std::memory_order_acquire);
}

ed_arena_t* ed_arena_get(int idx) {
  if (idx < 0 || idx >= ed_arena_count()) return NULL;
  return g_arenas[idx];
}

/* 为线程挑选 arena：至少保证存在一个；轮询分摊。 */
int ed_arena_pick_for_thread(ed_theap_t* t) {
  int count = ed_arena_count();
  if (count == 0) {
    std::lock_guard<std::mutex> lock(g_subproc_lock);
    if (ed_arena_count() == 0) {
      g_arena_next_size = ed_options.arena_reserve;
      if (g_arena_next_size == 0) g_arena_next_size = ED_ARENA_FIRST;
      if (ed_arena_create(g_arena_next_size) == NULL) return -1;
    }
    count = ed_arena_count();
  }
  int idx = (int)(g_arena_round_robin.fetch_add(1, std::memory_order_relaxed) %
                  (uint32_t)count);
  t->arena_idx = idx;
  return idx;
}

/* ------------------------------ slice 分配 ----------------------------- */

/* 找空闲段 → 取消待 purge → 按需 commit。返回 arena 内 slice 下标；
 * 失败 (size_t)-1。调用方不持锁。 */
size_t ed_arena_alloc_slices(ed_arena_t* arena, size_t n, size_t align_slices,
                             bool* out_zeroed) {
  std::lock_guard<std::mutex> lock(arena->mutex);
  /* 位图下标是 arena 相对的；调用方要的是绝对地址对齐，故传入基址相位 */
  size_t phase = (align_slices > 1)
                     ? (((uintptr_t)arena->base >> ED_SLICE_SHIFT) % align_slices)
                     : 0;
  size_t idx = ed_bbitmap_alloc(arena->slices_free, n, align_slices, phase);
  if (idx == (size_t)-1) return (size_t)-1;

  /* *out_zeroed 是整段的**合取**：只要有一个 slice 脏，整段就不算全零。
   * 逐 slice 先算 dirty、再让"本次新提交"把它压回 false（新提交的物理页由
   * OS 保证全零），最后才并进 zeroed——顺序颠倒过就会漏：原实现把
   * `zeroed = true` 直接写在提交分支里，于是"前一个 slice 脏 + 后一个 slice
   * 未提交"的混合段会被报成全零。单 slice 的小页碰不到（dirty 蕴含
   * committed，二者在同一个 i 上互斥），多 slice 的 large 段则完全可达——
   * ed_calloc 的免清零快路径正是按这个出参决定要不要 memset 的。 */
  bool zeroed = true;
  for (size_t i = idx; i < idx + n; i++) {
    if (ed_bitmap_test(arena->slices_purge, i)) {
      /* 取消待 purge：物理页仍持有 */
      ed_bitmap_clear(arena->slices_purge, i);
    }
    bool dirty = ed_bitmap_test(arena->slices_dirty, i);
    if (dirty) ed_bitmap_clear(arena->slices_dirty, i);
    if (!ed_bitmap_test(arena->slices_committed, i)) {
      /* 按需提交（v1 简化：整 slice 提交，不再细分更小粒度） */
      if (!ed_os_commit(arena->base + i * ED_SLICE_SIZE, ED_SLICE_SIZE)) {
        /* 回滚：本次循环已经把 [idx, i) 的 dirty 位清掉了，直接返回会让那些
         * 仍然脏的 slice 被标成干净，下一次分配就报 zeroed=true，calloc 的
         * 免清零快路径于是把上一任的数据交给调用方。保守地把整段重新标脏：
         * 代价是多几次 memset，换取"绝不把脏内存当零交出去"。 */
        ed_bitmap_set_range(arena->slices_dirty, idx, n);
        /* OOM：回滚 */
        ed_bbitmap_free(arena->slices_free, idx, n);
        return (size_t)-1;
      }
      ed_bitmap_set(arena->slices_committed, i);
      dirty = false;   /* 新提交的物理页保证全零 */
    }
    if (dirty) zeroed = false;
  }
  if (!ed_bitmap_any(arena->slices_purge))
    arena->purge_expire.store(0, std::memory_order_relaxed);
  g_dbg_slice_alloc.fetch_add(n, std::memory_order_relaxed);
  *out_zeroed = zeroed;
  return idx;
}

/* 在任意 arena 中分配 n 个连续 slice；必要时创建新 arena（×4 增长）。
 * 成功返回 arena 内 slice 下标并设置 *out_arena / *out_zeroed；失败 (size_t)-1。 */
size_t ed_arena_alloc_slices_any(ed_theap_t* t, size_t n, size_t align_slices,
                                 ed_arena_t** out_arena, bool* out_zeroed) {
  int count = ed_arena_count();
  int start = t->arena_idx;
  if (start < 0 || start >= count) start = 0;
  for (int k = 0; k < count; k++) {
    int idx = (start + k) % count;
    ed_arena_t* a = g_arenas[idx];
    size_t s = ed_arena_alloc_slices(a, n, align_slices, out_zeroed);
    if (s != (size_t)-1) {
      t->arena_idx = idx;
      *out_arena = a;
      return s;
    }
  }
  /* 全部满了：开新 arena（×4 增长，≤1GiB；需求更大时按需求走） */
  std::lock_guard<std::mutex> lock(g_subproc_lock);
  size_t need_slices = n + (align_slices > 1 ? align_slices : 0);
  size_t need = ed_round_up(need_slices << ED_SLICE_SHIFT, ED_ARENA_MIN);
  size_t size = g_arena_next_size * 4;
  if (size > ED_ARENA_MAX) size = ED_ARENA_MAX;
  if (size < ED_ARENA_FIRST) size = ED_ARENA_FIRST;
  if (size < need) size = need;
  size = ed_round_up(size, ED_ARENA_MIN);
  ed_arena_t* a = ed_arena_create(size);
  if (a == NULL) return (size_t)-1;
  g_arena_next_size = size;
  size_t s = ed_arena_alloc_slices(a, n, align_slices, out_zeroed);
  if (s == (size_t)-1) return (size_t)-1;
  t->arena_idx = a->index;
  *out_arena = a;
  return s;
}

/* 归还 slice：置 free/dirty/purge 位图并刷新到期时间戳（SPEC §6.3.2）。
 * 此刻不做 madvise——物理页归还延迟到 purge 到期。 */
void ed_arena_free_slices(ed_arena_t* arena, size_t slice_idx, size_t n) {
  g_dbg_slice_free.fetch_add(n, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(arena->mutex);
  ed_bbitmap_free(arena->slices_free, slice_idx, n);
  ed_bitmap_set_range(arena->slices_dirty, slice_idx, n);
  ed_bitmap_set_range(arena->slices_purge, slice_idx, n);
  int64_t expire = ed_os_now_ms() + ed_options.purge_delay_ms;
  arena->purge_expire.store(expire, std::memory_order_release);
}

/* 归还已由调用方 decommit 的 slice（huge/guarded 页：free 即归还物理页）。 */
void ed_arena_free_slices_decommitted(ed_arena_t* arena, size_t slice_idx, size_t n) {
  g_dbg_slice_free.fetch_add(n, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(arena->mutex);
  ed_bbitmap_free(arena->slices_free, slice_idx, n);
  ed_bitmap_clear_range(arena->slices_committed, slice_idx, n);
  ed_bitmap_clear_range(arena->slices_dirty, slice_idx, n);
  ed_bitmap_clear_range(arena->slices_purge, slice_idx, n);
}

void ed_arena_purge_force(ed_arena_t* arena) {
  std::lock_guard<std::mutex> lock(arena->mutex);
  if (!ed_bitmap_any(arena->slices_purge)) {
    arena->purge_expire.store(0, std::memory_order_relaxed);
    return;
  }
  for (size_t i = 0; i < arena->slice_count; i++) {
    if (!ed_bitmap_test(arena->slices_purge, i)) continue;
    ed_os_decommit(arena->base + i * ED_SLICE_SIZE, ED_SLICE_SIZE);
    ed_bitmap_clear(arena->slices_purge, i);
    ed_bitmap_clear(arena->slices_committed, i);
    ed_bitmap_clear(arena->slices_dirty, i);   /* 重新提交后内容保证全零 */
  }
  arena->purge_expire.store(0, std::memory_order_release);
}

/* 到期检查：摊销在分配慢路径；purge_expire==0 时只是一次比较 */
void ed_arena_purge_check(ed_arena_t* arena) {
  int64_t expire = arena->purge_expire.load(std::memory_order_acquire);
  if (expire != 0 && ed_os_now_ms() >= expire) ed_arena_purge_force(arena);
}

/* --------------------------- abandoned 桶 ------------------------------ */
/* （bin b -> bucket[b-1]，仅 small 页） */

void ed_arena_abandon_page(ed_arena_t* arena, ed_page_t* page) {
  std::lock_guard<std::mutex> lock(arena->mutex);
  int b = page->bin;
  /* 与读侧 ed_arena_reclaim_page 同样两端校验，且不夹取：夹到 1 比中止更糟——
   * bin==0 的 large 页会被静默归进 bin 1 的桶，之后当 bin-1 的小页发出去。
   * 目前不可达（只有小页队列会走到这里），这是把契约写在使用点上。 */
  if (ED_UNLIKELY(b < 1 || b > ED_BIN_FULL))
    ed_fatal_page("abandon of a page with an out-of-range bin", NULL, page);
  page->next = arena->abandoned[b - 1];
  arena->abandoned[b - 1] = page;
}

ed_page_t* ed_arena_reclaim_page(ed_arena_t* arena, int bin) {
  if (bin < 1 || bin > ED_BIN_FULL) return NULL;
  std::lock_guard<std::mutex> lock(arena->mutex);
  ed_page_t* page = arena->abandoned[bin - 1];
  if (page != NULL) {
    arena->abandoned[bin - 1] = page->next;
    page->next = NULL;
  }
  return page;
}

/* ed_collect 用：遍历全部 37 个桶，collect 每页，把 used==0 的摘链并用 next
 * 串成一条返回链交给调用方归还 slice（归还必须在锁外做——ed_page_release_slices
 * 自身还要取同一把 arena 锁）。 */
ed_page_t* ed_arena_take_abandoned_empty(ed_arena_t* arena) {
  std::lock_guard<std::mutex> lock(arena->mutex);
  ed_page_t* head = NULL;
  for (int b = 0; b < ED_BIN_COUNT; b++) {
    ed_page_t** pp = &arena->abandoned[b];
    while (*pp != NULL) {
      ed_page_t* pg = *pp;
      ed_page_free_collect(pg);
      if (pg->used == 0) {
        *pp = pg->next;
        pg->next = head;
        head = pg;
      } else {
        pp = &pg->next;
      }
    }
  }
  return head;
}

/* ------------------------------ 统计 ----------------------------------- */

size_t ed_arena_committed_bytes(void) {
  size_t total = 0;
  int count = ed_arena_count();
  for (int i = 0; i < count; i++) {
    ed_arena_t* a = g_arenas[i];
    std::lock_guard<std::mutex> lock(a->mutex);
    size_t nw = ed_bitmap_words(a->slice_count);
    for (size_t w = 0; w < nw; w++)
      total += (size_t)ed_popcount64(a->slices_committed->words[w]);
  }
  return total * ED_SLICE_SIZE;
}

size_t ed_arena_abandoned_count(void) {
  size_t total = 0;
  int count = ed_arena_count();
  for (int i = 0; i < count; i++) {
    ed_arena_t* a = g_arenas[i];
    std::lock_guard<std::mutex> lock(a->mutex);
    for (int b = 0; b < ED_BIN_COUNT; b++)
      for (ed_page_t* pg = a->abandoned[b]; pg != NULL; pg = pg->next) total++;
  }
  return total;
}

/* ------------------------ 平铺描述符数组的维护 ------------------------- */

/* 多 slice 页的续页项：[first+1, first+n) 置 CONT 并指回首项。
 * ed_ptr_page 命中续页时经 cont_first 一跳落到首描述符。 */
void ed_desc_mark_cont(size_t first_idx, size_t n) {
  for (size_t i = 1; i < n; i++) {
    ed_page_t* d = ed_desc_base + first_idx + i;
    d->kind = (uint8_t)ED_PAGE_CONT;
    d->cont_first = (uint32_t)first_idx;
    d->magic.store(ED_PAGE_MAGIC, std::memory_order_release);
  }
}

/* 注销一个页：只清首项 magic，此后该页地址段上的任何 free 都被 L0 抓获。
 *
 * 续页项（CONT）有意不清，代价是 ed_ptr_page 必须替它兜底。那里的三条复核
 * （首项仍活、首项本身不是续页项、本 slice 落在首项的 slice_count 范围内）
 * 缺一不可——尤其第二条：ed_desc_mark_cont 不写 slice_count，所以一个"曾当过
 * 首项、现被征用成别的页的续页项"的描述符会带着上一辈子的 slice_count 亮着
 * magic，只查前一条和后一条会放行它。完整的不变式论证见 internal.h 的
 * ed_ptr_page。
 * 省掉的这一轮 n 次写是 large 路径上最大的一笔额外开销：描述符每项 128B，
 * 逐 slice 写会脏 n 条 cache line，而它取代的旧页映射表每项只有 2B。7 轮配对
 * 实测（bench_large，平均 ~15 slice/块）：逐项清零时相对改造前基线是
 * -10%，只清首项后回到 +3%。 */
void ed_desc_clear(size_t first_idx) {
  ed_desc_base[first_idx].magic.store(0, std::memory_order_release);
}

/* ------------------------ fork 钩子支撑（heap.cpp 注册） ---------------- */

void ed_arena_lock_all(void) {
  g_subproc_lock.lock();
  int count = ed_arena_count();
  for (int i = 0; i < count; i++) g_arenas[i]->mutex.lock();
}
void ed_arena_unlock_all(void) {
  int count = ed_arena_count();
  for (int i = 0; i < count; i++) g_arenas[i]->mutex.unlock();
  g_subproc_lock.unlock();
}
void ed_arena_reinit_locks_child(void) {
  int count = ed_arena_count();
  for (int i = 0; i < count; i++) {
    new (&g_arenas[i]->mutex) std::mutex();
  }
  new (&g_subproc_lock) std::mutex();
}
