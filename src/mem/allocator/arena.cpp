/* ----------------------------------------------------------------------------
 * arena.cpp — 地址窗口、平铺描述符、slice 回收和 abandoned 页
 * ------------------------------------------------------------------------- */
#include "internal.h"
#include "bitmap.h"

#include <limits>
#include <new>

namespace wavvix { namespace mem { namespace detail {

/* 地址窗口与平铺描述符数组（internal.h 声明；mem_ptr_page 内联读取） */
uint8_t*            mem_window_base = NULL;
size_t              mem_window_size = 0;
mem_page_t*          mem_desc_base   = NULL;
std::atomic<size_t> mem_window_used_slices(0);
std::atomic<size_t> mem_desc_constructed_slices(0);

namespace {

std::mutex            g_subproc_lock;                   /* arena 创建/全局 */
mem_arena_t*           g_arenas[WAVVIX_MEM_ARENA_COUNT_MAX];
std::atomic<int>      g_arena_count(0);
std::atomic<uint32_t> g_arena_round_robin(0);
size_t                g_arena_next_size = 0;            /* 下一个 arena 的保留大小 */

size_t g_window_slices  = 0;    /* 窗口总 slice 数 */
size_t g_desc_reserve   = 0;    /* 描述符数组保留字节数 */
size_t g_desc_committed = 0;    /* 描述符数组已提交字节数（单调递增） */
const size_t desc_init_chunk = 64;

inline size_t mem_round_up(size_t v, size_t granule) {
  return (v + granule - 1) / granule * granule;
}

/* 描述符按 arena 提交虚拟页；对象到首次使用时才构造。
 * 调用方持 g_subproc_lock。 */
bool mem_desc_commit_upto(size_t end_slice) {
  size_t need = mem_round_up(end_slice * sizeof(mem_page_t), mem_os_page_size());
  /* 越界只可能来自调用方算错 end_slice：mem_arena_create 已用
   * start + n > g_window_slices 挡在前面，所以这里不可达。静默截断会让
   * 描述符区间悄悄少提交一段，之后写进去就是越界——宁可当场死。 */
  if (WAVVIX_MEM_UNLIKELY(need > g_desc_reserve))
    mem_os_fatal("descriptor commit beyond reserved range (bad end_slice)");
  if (need > g_desc_committed) {
    if (!mem_os_commit((uint8_t*)mem_desc_base + g_desc_committed,
                       need - g_desc_committed))
      return false;
    g_desc_committed = need;
  }
  return true;
}

/* 调用方持 g_subproc_lock；分批触碰描述符，减少小程序的冷启动占用。 */
void mem_desc_construct_upto(size_t end_slice, size_t arena_end) {
  size_t want = mem_round_up(end_slice, desc_init_chunk);
  if (want > arena_end) want = arena_end;
  size_t done = mem_desc_constructed_slices.load(std::memory_order_relaxed);
  if (want <= done) return;
  for (size_t i = done; i < want; ++i) {
    mem_page_t* d = new (mem_desc_at(i)) mem_page_t;
    d->magic.store(0, std::memory_order_relaxed);
  }
  mem_desc_constructed_slices.store(want, std::memory_order_release);
}

/* 从窗口里顺序切下一段 arena。调用方持 g_subproc_lock；size 已是 2MiB 倍数。 */
mem_arena_t* mem_arena_create(size_t size) {
  int count = g_arena_count.load(std::memory_order_relaxed);
  if (count >= WAVVIX_MEM_ARENA_COUNT_MAX) return NULL;
  if (mem_window_base == NULL) return NULL;

  size_t n = size >> WAVVIX_MEM_SLICE_SHIFT;
  size_t start = mem_window_used_slices.load(std::memory_order_relaxed);
  if (n == 0 || start + n > g_window_slices) return NULL;
  if (!mem_desc_commit_upto(start + n)) return NULL;

  size_t words = (n + 63) / 64;
  mem_arena_t*   arena        = (mem_arena_t*)mem_meta_alloc(sizeof(mem_arena_t));
  mem_bbitmap_t* bm_free      = (mem_bbitmap_t*)mem_meta_alloc(sizeof(mem_bbitmap_t));
  mem_bitmap_t*  bm_committed = (mem_bitmap_t*)mem_meta_alloc(sizeof(mem_bitmap_t));
  mem_bitmap_t*  bm_dirty     = (mem_bitmap_t*)mem_meta_alloc(sizeof(mem_bitmap_t));
  mem_bitmap_t*  bm_purge     = (mem_bitmap_t*)mem_meta_alloc(sizeof(mem_bitmap_t));
  uint64_t* st_free      = (uint64_t*)mem_meta_alloc(words * 8);
  uint64_t* st_committed = (uint64_t*)mem_meta_alloc(words * 8);
  uint64_t* st_dirty     = (uint64_t*)mem_meta_alloc(words * 8);
  uint64_t* st_purge     = (uint64_t*)mem_meta_alloc(words * 8);
  if (arena == NULL || bm_free == NULL || bm_committed == NULL ||
      bm_dirty == NULL || bm_purge == NULL || st_free == NULL ||
      st_committed == NULL || st_dirty == NULL || st_purge == NULL)
    return NULL;
  new (arena) mem_arena_t;

  arena->base        = mem_window_base + (start << WAVVIX_MEM_SLICE_SHIFT);
  arena->slice_count = n;
  arena->first_slice = start;
  arena->index       = (uint16_t)count;
  arena->slices_free      = bm_free;
  arena->slices_committed = bm_committed;
  arena->slices_dirty     = bm_dirty;
  arena->slices_purge     = bm_purge;
  mem_bbitmap_init(bm_free, st_free, n);
  mem_bitmap_init(bm_committed, st_committed, n, false);
  mem_bitmap_init(bm_dirty, st_dirty, n, false);
  mem_bitmap_init(bm_purge, st_purge, n, false);
  arena->purge_expire.store(0, std::memory_order_relaxed);
  for (int i = 0; i < WAVVIX_MEM_BIN_COUNT; i++) arena->abandoned[i] = NULL;
  arena->abandoned_nonempty.store(0, std::memory_order_relaxed);

  g_arenas[count] = arena;
  g_arena_count.store(count + 1, std::memory_order_release);
  /* arena 完整初始化后再发布；描述符由分配路径按需构造。 */
  mem_window_used_slices.store(start + n, std::memory_order_release);
  return arena;
}

} /* anonymous namespace */

/* ------------------------------ 窗口建立 ------------------------------- */

/* 保留窗口 + 描述符数组；窗口保留失败逐级减半，下限 WAVVIX_MEM_WINDOW_MIN。
 * 描述符数组大小 = 窗口 slice 数 × 128B（1 TiB 窗口 → 2 GiB 保留，仅地址空间）。 */
bool mem_window_init(size_t want) {
  size_t size = want < WAVVIX_MEM_WINDOW_MIN ? WAVVIX_MEM_WINDOW_MIN
                                     : (want & ~(WAVVIX_MEM_ARENA_MIN - 1));
  for (;;) {
    void* base = NULL;
    if (mem_os_reserve(size, &base)) {
      size_t slices = size >> WAVVIX_MEM_SLICE_SHIFT;
      size_t dres = mem_round_up(slices * sizeof(mem_page_t), WAVVIX_MEM_OS_RESERVE_ALIGN);
      void* d = NULL;
      if (mem_os_reserve(dres, &d)) {
        mem_window_base  = (uint8_t*)base;
        mem_window_size  = size;
        g_window_slices = slices;
        mem_desc_base    = (mem_page_t*)d;
        g_desc_reserve  = dres;
        g_desc_committed = 0;
        mem_desc_constructed_slices.store(0, std::memory_order_relaxed);
        return true;
      }
      mem_os_release(base, size);
    }
    if (size <= WAVVIX_MEM_WINDOW_MIN) return false;
    size >>= 1;
  }
}

/* ------------------------------ arena 查询 ----------------------------- */

int mem_arena_count(void) {
  return g_arena_count.load(std::memory_order_acquire);
}

mem_arena_t* mem_arena_get(int idx) {
  if (idx < 0 || idx >= mem_arena_count()) return NULL;
  return g_arenas[idx];
}

/* 为线程挑选 arena：至少保证存在一个；轮询分摊。 */
int mem_arena_pick_for_thread(mem_theap_t* t) {
  int count = mem_arena_count();
  if (count == 0) {
    std::lock_guard<std::mutex> lock(g_subproc_lock);
    if (mem_arena_count() == 0) {
      g_arena_next_size = mem_options.arena_reserve;
      if (g_arena_next_size == 0) g_arena_next_size = WAVVIX_MEM_ARENA_FIRST;
      if (mem_arena_create(g_arena_next_size) == NULL) return -1;
    }
    count = mem_arena_count();
  }
  int idx = (int)(g_arena_round_robin.fetch_add(1, std::memory_order_relaxed) %
                  (uint32_t)count);
  t->arena_idx = idx;
  return idx;
}

/* ------------------------------ slice 分配 ----------------------------- */

/* 找空闲段 → 取消待 purge → 按需 commit。返回 arena 内 slice 下标；
 * 失败 (size_t)-1。调用方不持锁。 */
size_t mem_arena_alloc_slices(mem_arena_t* arena, size_t n, size_t align_slices,
                             bool* out_zeroed) {
  std::lock_guard<std::mutex> lock(arena->mutex);
  /* 位图下标是 arena 相对的；调用方要的是绝对地址对齐，故传入基址相位 */
  size_t phase = (align_slices > 1)
                     ? (((uintptr_t)arena->base >> WAVVIX_MEM_SLICE_SHIFT) % align_slices)
                     : 0;
  size_t idx = mem_bbitmap_alloc(arena->slices_free, n, align_slices, phase);
  if (idx == (size_t)-1) return (size_t)-1;

  /* *out_zeroed 是整段的**合取**：只要有一个 slice 脏，整段就不算全零。
   * 逐 slice 先算 dirty、再让"本次新提交"把它压回 false（新提交的物理页由
   * OS 保证全零），最后才并进 zeroed——顺序颠倒过就会漏：原实现把
   * `zeroed = true` 直接写在提交分支里，于是"前一个 slice 脏 + 后一个 slice
   * 未提交"的混合段会被报成全零。单 slice 的小页碰不到（dirty 蕴含
   * committed，二者在同一个 i 上互斥），多 slice 的 large 段则完全可达——
   * wavvix::mem::calloc 的免清零快路径正是按这个出参决定要不要 memset 的。 */
  bool zeroed = true;
  for (size_t i = idx; i < idx + n; i++) {
    if (mem_bitmap_test(arena->slices_purge, i)) {
      /* 取消待 purge：物理页仍持有 */
      mem_bitmap_clear(arena->slices_purge, i);
    }
    bool dirty = mem_bitmap_test(arena->slices_dirty, i);
    if (dirty) mem_bitmap_clear(arena->slices_dirty, i);
    if (!mem_bitmap_test(arena->slices_committed, i)) {
      /* 按需提交（v1 简化：整 slice 提交，不再细分更小粒度） */
      if (!mem_os_commit(arena->base + i * WAVVIX_MEM_SLICE_SIZE, WAVVIX_MEM_SLICE_SIZE)) {
        /* 回滚：本次循环已经把 [idx, i) 的 dirty 位清掉了，直接返回会让那些
         * 仍然脏的 slice 被标成干净，下一次分配就报 zeroed=true，calloc 的
         * 免清零快路径于是把上一任的数据交给调用方。保守地把整段重新标脏：
         * 代价是多几次 memset，换取"绝不把脏内存当零交出去"。 */
        mem_bitmap_set_range(arena->slices_dirty, idx, n);
        /* OOM：回滚 */
        mem_bbitmap_free(arena->slices_free, idx, n);
        return (size_t)-1;
      }
      mem_bitmap_set(arena->slices_committed, i);
      dirty = false;   /* 新提交的物理页保证全零 */
    }
    if (dirty) zeroed = false;
  }
  if (!mem_bitmap_any(arena->slices_purge))
    arena->purge_expire.store(0, std::memory_order_relaxed);
  *out_zeroed = zeroed;
  return idx;
}

/* 在任意 arena 中分配 n 个连续 slice；必要时创建新 arena（×4 增长）。
 * 成功返回 arena 内 slice 下标并设置 *out_arena / *out_zeroed；失败 (size_t)-1。 */
size_t mem_arena_alloc_slices_any(mem_theap_t* t, size_t n, size_t align_slices,
                                 mem_arena_t** out_arena, bool* out_zeroed) {
  std::unique_lock<std::mutex> growth_lock(g_subproc_lock, std::defer_lock);
  for (int pass = 0; pass < 2; ++pass) {
    if (pass != 0) growth_lock.lock();
    int count = mem_arena_count();
    int start = t->arena_idx;
    if (start < 0 || start >= count) start = 0;
    for (int k = 0; k < count; k++) {
      int idx = (start + k) % count;
      mem_arena_t* a = g_arenas[idx];
      size_t s = mem_arena_alloc_slices(a, n, align_slices, out_zeroed);
      if (s != (size_t)-1) {
        const size_t end = a->first_slice + s + n;
        if (end > mem_desc_constructed_slices.load(std::memory_order_acquire)) {
          if (growth_lock.owns_lock()) {
            mem_desc_construct_upto(end, a->first_slice + a->slice_count);
          } else {
            std::lock_guard<std::mutex> lock(g_subproc_lock);
            mem_desc_construct_upto(end, a->first_slice + a->slice_count);
          }
        }
        t->arena_idx = idx;
        *out_arena = a;
        return s;
      }
    }
  }
  /* 持锁重查后仍无空位，才开新 arena。 */
  size_t need_slices = n + (align_slices > 1 ? align_slices : 0);
  size_t need = mem_round_up(need_slices << WAVVIX_MEM_SLICE_SHIFT, WAVVIX_MEM_ARENA_MIN);
  size_t size = g_arena_next_size * 4;
  if (size > WAVVIX_MEM_ARENA_MAX) size = WAVVIX_MEM_ARENA_MAX;
  if (size < WAVVIX_MEM_ARENA_FIRST) size = WAVVIX_MEM_ARENA_FIRST;
  if (size < need) size = need;
  size = mem_round_up(size, WAVVIX_MEM_ARENA_MIN);
  mem_arena_t* a = mem_arena_create(size);
  if (a == NULL) return (size_t)-1;
  g_arena_next_size = size;
  size_t s = mem_arena_alloc_slices(a, n, align_slices, out_zeroed);
  if (s == (size_t)-1) return (size_t)-1;
  mem_desc_construct_upto(a->first_slice + s + n, a->first_slice + a->slice_count);
  t->arena_idx = a->index;
  *out_arena = a;
  return s;
}

/* 归还 slice：置 free/dirty/purge 位图并刷新到期时间戳（SPEC §6.3.2）。
 * 此刻不做 madvise——物理页归还延迟到 purge 到期。 */
void mem_arena_free_slices(mem_arena_t* arena, size_t slice_idx, size_t n) {
  std::lock_guard<std::mutex> lock(arena->mutex);
  mem_bbitmap_free(arena->slices_free, slice_idx, n);
  mem_bitmap_set_range(arena->slices_dirty, slice_idx, n);
  mem_bitmap_set_range(arena->slices_purge, slice_idx, n);
  const int64_t now = mem_os_now_ms();
  const int64_t delay = static_cast<int64_t>(mem_options.purge_delay_ms);
  const int64_t max = (std::numeric_limits<int64_t>::max)();
  const int64_t expire = now > max - delay ? max : now + delay;
  arena->purge_expire.store(expire, std::memory_order_release);
}

/* 归还已由调用方 decommit 的 slice（huge 页：free 即归还物理页）。 */
void mem_arena_free_slices_decommitted(mem_arena_t* arena, size_t slice_idx, size_t n) {
  std::lock_guard<std::mutex> lock(arena->mutex);
  mem_bbitmap_free(arena->slices_free, slice_idx, n);
  mem_bitmap_clear_range(arena->slices_committed, slice_idx, n);
  mem_bitmap_clear_range(arena->slices_dirty, slice_idx, n);
  mem_bitmap_clear_range(arena->slices_purge, slice_idx, n);
}

void mem_arena_purge_force(mem_arena_t* arena) {
  std::lock_guard<std::mutex> lock(arena->mutex);
  if (!mem_bitmap_any(arena->slices_purge)) {
    arena->purge_expire.store(0, std::memory_order_relaxed);
    return;
  }
  for (size_t i = 0; i < arena->slice_count; i++) {
    if (!mem_bitmap_test(arena->slices_purge, i)) continue;
    mem_os_decommit(arena->base + i * WAVVIX_MEM_SLICE_SIZE, WAVVIX_MEM_SLICE_SIZE);
    mem_bitmap_clear(arena->slices_purge, i);
    mem_bitmap_clear(arena->slices_committed, i);
    mem_bitmap_clear(arena->slices_dirty, i);   /* 重新提交后内容保证全零 */
  }
  arena->purge_expire.store(0, std::memory_order_release);
}

/* 到期检查：摊销在分配慢路径；purge_expire==0 时只是一次比较 */
void mem_arena_purge_check(mem_arena_t* arena) {
  int64_t expire = arena->purge_expire.load(std::memory_order_acquire);
  if (expire != 0 && mem_os_now_ms() >= expire) mem_arena_purge_force(arena);
}

/* --------------------------- abandoned 桶 ------------------------------ */
/* （bin b -> bucket[b-1]，仅 small 页） */

void mem_arena_abandon_page(mem_arena_t* arena, mem_page_t* page) {
  std::lock_guard<std::mutex> lock(arena->mutex);
  int b = page->bin;
  /* 与读侧 mem_arena_reclaim_page 同样两端校验，且不夹取：夹到 1 比中止更糟——
   * bin==0 的 large 页会被静默归进 bin 1 的桶，之后当 bin-1 的小页发出去。
   * 目前不可达（只有小页队列会走到这里），这是把契约写在使用点上。 */
  if (WAVVIX_MEM_UNLIKELY(b < 1 || b > WAVVIX_MEM_BIN_FULL))
    mem_fatal_page("abandon of a page with an out-of-range bin", NULL, page);
  page->next = arena->abandoned[b - 1];
  arena->abandoned[b - 1] = page;
  arena->abandoned_nonempty.store(
      arena->abandoned_nonempty.load(std::memory_order_relaxed) | (uint64_t(1) << (b - 1)),
      std::memory_order_relaxed);
}

mem_page_t* mem_arena_reclaim_page(mem_arena_t* arena, int bin) {
  if (bin < 1 || bin > WAVVIX_MEM_BIN_FULL) return NULL;
  std::lock_guard<std::mutex> lock(arena->mutex);
  mem_page_t* page = arena->abandoned[bin - 1];
  if (page != NULL) {
    arena->abandoned[bin - 1] = page->next;
    page->next = NULL;
    if (arena->abandoned[bin - 1] == NULL)
      arena->abandoned_nonempty.store(
          arena->abandoned_nonempty.load(std::memory_order_relaxed) &
              ~(uint64_t(1) << (bin - 1)), std::memory_order_relaxed);
  }
  return page;
}

/* wavvix::mem::collect 用：遍历全部 37 个桶，collect 每页，把 used==0 的摘链并用 next
 * 串成一条返回链交给调用方归还 slice（归还必须在锁外做——mem_page_release_slices
 * 自身还要取同一把 arena 锁）。 */
mem_page_t* mem_arena_take_abandoned_empty(mem_arena_t* arena) {
  std::lock_guard<std::mutex> lock(arena->mutex);
  mem_page_t* head = NULL;
  uint64_t nonempty = 0;
  for (int b = 0; b < WAVVIX_MEM_BIN_COUNT; b++) {
    mem_page_t** pp = &arena->abandoned[b];
    while (*pp != NULL) {
      mem_page_t* pg = *pp;
      mem_page_free_collect(pg);
      if (pg->used == 0) {
        *pp = pg->next;
        pg->next = head;
        head = pg;
      } else {
        pp = &pg->next;
      }
    }
    if (arena->abandoned[b] != NULL) nonempty |= uint64_t(1) << b;
  }
  arena->abandoned_nonempty.store(nonempty, std::memory_order_relaxed);
  return head;
}

/* ------------------------------ 统计 ----------------------------------- */

size_t mem_arena_committed_bytes(void) {
  size_t total = 0;
  int count = mem_arena_count();
  for (int i = 0; i < count; i++) {
    mem_arena_t* a = g_arenas[i];
    std::lock_guard<std::mutex> lock(a->mutex);
    size_t nw = mem_bitmap_words(a->slice_count);
    for (size_t w = 0; w < nw; w++)
      total += (size_t)mem_popcount64(a->slices_committed->words[w]);
  }
  return total * WAVVIX_MEM_SLICE_SIZE;
}

size_t mem_arena_abandoned_count(void) {
  size_t total = 0;
  int count = mem_arena_count();
  for (int i = 0; i < count; i++) {
    mem_arena_t* a = g_arenas[i];
    std::lock_guard<std::mutex> lock(a->mutex);
    for (int b = 0; b < WAVVIX_MEM_BIN_COUNT; b++)
      for (mem_page_t* pg = a->abandoned[b]; pg != NULL; pg = pg->next) total++;
  }
  return total;
}

/* ------------------------ 平铺描述符数组的维护 ------------------------- */

/* 多 slice 页的续页项：[first+1, first+n) 置 CONT 并指回首项。
 * mem_ptr_page 命中续页时经 cont_first 一跳落到首描述符。 */
void mem_desc_mark_cont(size_t first_idx, size_t n) {
  for (size_t i = 1; i < n; i++) {
    mem_page_t* d = mem_desc_at(first_idx + i);
    d->kind = (uint8_t)WAVVIX_MEM_PAGE_CONT;
    d->cont_first = (uint32_t)first_idx;
    d->magic.store(WAVVIX_MEM_PAGE_MAGIC, std::memory_order_release);
  }
}

/* 只清首项；续页项可陈旧，mem_ptr_page 的三条复核必须保留。
 * 不变式见 internal.h，逐项清零的成本见 docs/archive/edumalloc/docs/decisions.md D3。 */
void mem_desc_clear(size_t first_idx) {
  mem_desc_at(first_idx)->magic.store(0, std::memory_order_release);
}

/* ------------------------ fork 钩子支撑（heap.cpp 注册） ---------------- */

void mem_arena_lock_all(void) {
  g_subproc_lock.lock();
  int count = mem_arena_count();
  for (int i = 0; i < count; i++) g_arenas[i]->mutex.lock();
}
void mem_arena_unlock_all(void) {
  int count = mem_arena_count();
  for (int i = 0; i < count; i++) g_arenas[i]->mutex.unlock();
  g_subproc_lock.unlock();
}
void mem_arena_reinit_locks_child(void) {
  int count = mem_arena_count();
  for (int i = 0; i < count; i++) {
    new (&g_arenas[i]->mutex) std::mutex();
  }
  new (&g_subproc_lock) std::mutex();
}

} } } // namespace wavvix::mem::detail
