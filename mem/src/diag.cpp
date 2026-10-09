/* ----------------------------------------------------------------------------
 * diag.cpp — 诊断与报错：fatal 报告、mem_stats_print、WAVVIX_MEM_DEBUG 泄漏报告
 *
 * fatal 路径必须无分配、无 stdio：错误发生时堆状态已不可信，而 override
 * 场景下 fprintf 自身还会再次调用 malloc。因此这里手写十六进制/十进制格式化
 * 到 256 字节栈缓冲，最终只经 mem_os_fatal 走一次裸 write。
 * ------------------------------------------------------------------------- */
#include "internal.h"
#include "bitmap.h"

#include <cstdio>
#include <cstring>

/* 诊断计数器（定义在 arena.cpp / theap.cpp）。必须在文件作用域声明：
 * 放在函数体内的块作用域 extern 会被 MSVC 按 C 链接解析，与定义处的
 * C++ 链接不匹配（LNK2019）。 */
extern std::atomic<uint64_t> g_dbg_slice_alloc, g_dbg_slice_free;
extern std::atomic<uint64_t> g_dbg_to_full, g_dbg_abandon, g_dbg_reclaim,
                             g_dbg_unfull, g_dbg_retire, g_dbg_fresh_slice;

namespace {

/* ------------------------- 无分配的行缓冲拼装 -------------------------- */

struct mem_sbuf_t {
  char   buf[256];
  size_t n;
};

void sb_init(mem_sbuf_t* s) { s->n = 0; s->buf[0] = '\0'; }

void sb_str(mem_sbuf_t* s, const char* t) {
  if (t == NULL) t = "(null)";
  while (*t != '\0' && s->n + 1 < sizeof(s->buf)) s->buf[s->n++] = *t++;
  s->buf[s->n] = '\0';
}

void sb_hex(mem_sbuf_t* s, uintptr_t v) {
  sb_str(s, "0x");
  for (int i = 15; i >= 0; i--) {
    int d = (int)((v >> (i * 4)) & 0xF);
    char c = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
    if (s->n + 1 < sizeof(s->buf)) s->buf[s->n++] = c;
  }
  s->buf[s->n] = '\0';
}

void sb_dec(mem_sbuf_t* s, uint64_t v) {
  char tmp[24];
  int k = 0;
  if (v == 0) tmp[k++] = '0';
  while (v != 0 && k < (int)sizeof(tmp)) { tmp[k++] = (char)('0' + (int)(v % 10)); v /= 10; }
  while (k > 0) {
    k--;
    if (s->n + 1 < sizeof(s->buf)) s->buf[s->n++] = tmp[k];
  }
  s->buf[s->n] = '\0';
}

} /* anonymous namespace */

/* ------------------------------ fatal 报告 ----------------------------- */

WAVVIX_MEM_NORETURN void mem_fatal_ptr(const char* msg, const void* p) {
  mem_sbuf_t s;
  sb_init(&s);
  sb_str(&s, msg);
  sb_str(&s, " ptr=");
  sb_hex(&s, (uintptr_t)p);
  mem_os_fatal(s.buf);
}

WAVVIX_MEM_NORETURN void mem_fatal_page(const char* msg, const void* p, const mem_page_t* page) {
  mem_sbuf_t s;
  sb_init(&s);
  sb_str(&s, msg);
  sb_str(&s, " ptr=");
  sb_hex(&s, (uintptr_t)p);
  if (page != NULL) {
    sb_str(&s, " page=");   sb_hex(&s, (uintptr_t)page->slice_start);
    sb_str(&s, " slices="); sb_dec(&s, page->slice_count);
    sb_str(&s, " kind=");   sb_dec(&s, page->kind);
    sb_str(&s, " bin=");    sb_dec(&s, page->bin);
    sb_str(&s, " used=");   sb_dec(&s, page->used);
    sb_str(&s, "/");        sb_dec(&s, page->capacity);
  }
  mem_os_fatal(s.buf);
}

/* ------------------------------ 统计快照 ------------------------------- */

/* 汇总一份 mem_stats_t。进程级字段逐个 arena 加锁读取，因此本函数**不可**在
 * fatal 路径或信号处理函数里调用（那里不能取锁）——头文件已写明这条契约。
 *
 * 线程级字段只读调用线程自己的 theap，无锁也无竞争。mem_t_theap 为 NULL
 * （本线程一次都没分配过）时这些字段留 0，而不是顺手建一个 theap：
 * 观测动作不该改变被观测对象的状态。 */
void mem_stats_get(mem_stats_t* out) {
  if (out == NULL) return;
  mem_init_once();
  std::memset(out, 0, sizeof(*out));
  out->reserved_bytes    = mem_window_size;
  out->committed_bytes   = mem_arena_committed_bytes();
  out->arena_count       = (size_t)mem_arena_count();
  out->abandoned_pages   = mem_arena_abandoned_count();
  out->process_rss_bytes = mem_os_process_rss();

  mem_theap_t* t = mem_t_theap;
  if (t == NULL) return;
  out->thread_alloc_count = (unsigned long long)t->stat_alloc_count;
  out->thread_free_count  = (unsigned long long)t->stat_free_count;
  /* 净值可能为负（跨线程释放：分配记在 A、释放记在 B），截断到 0。
   * 存的是两个单调递增量正是为了让这一步是显式的截断而不是静默的下溢。 */
  out->thread_current_bytes =
      t->stat_alloc_bytes > t->stat_free_bytes
          ? (size_t)(t->stat_alloc_bytes - t->stat_free_bytes)
          : 0;
  for (int bin = 1; bin <= WAVVIX_MEM_BIN_FULL; bin++)
    out->thread_tcache_bytes +=
        (size_t)t->tcache[bin - 1].count * mem_bin_size(bin);
}

/* ------------------------------ 统计打印 ------------------------------- */

void mem_stats_print(void) {
  mem_stats_t s;
  mem_stats_get(&s);          /* 内含 mem_init_once */

  std::fprintf(stderr, "=== culib mem stats ===\n");
  std::fprintf(stderr,
               "options: purge_delay=%ldms window=%zu arena_reserve=%zu "
               "secure=%ld guarded_sample_rate=%ld eager_commit=%ld\n",
               mem_options.purge_delay_ms, mem_options.window_size,
               mem_options.arena_reserve, mem_options.secure,
               mem_options.guarded_sample_rate, mem_options.eager_commit);
  std::fprintf(stderr, "window: base=%p size=%zu used_slices=%zu desc=%p\n",
               (void*)mem_window_base, mem_window_size,
               mem_window_used_slices.load(std::memory_order_relaxed),
               (void*)mem_desc_base);
  std::fprintf(stderr,
               "process: reserved=%zu committed=%zu arenas=%zu "
               "abandoned_pages=%zu rss=%zu\n",
               s.reserved_bytes, s.committed_bytes, s.arena_count,
               s.abandoned_pages, s.process_rss_bytes);

  mem_theap_t* t = mem_t_theap;
  if (t != NULL) {
    /* current 是截断后的净值；两个累计量一并打出来，截断掉的那部分
     * （跨线程释放 / heap_destroy 造成的偏差）才有迹可循。 */
    std::fprintf(stderr,
                 "thread heap: allocs=%llu frees=%llu current=%zu bytes "
                 "(alloc_bytes=%llu free_bytes=%llu) tcache=%zu bytes\n",
                 s.thread_alloc_count, s.thread_free_count,
                 s.thread_current_bytes,
                 (unsigned long long)t->stat_alloc_bytes,
                 (unsigned long long)t->stat_free_bytes,
                 s.thread_tcache_bytes);
    for (int bin = 1; bin <= WAVVIX_MEM_BIN_FULL; bin++) {
      mem_page_queue_t* q = &t->pages[bin - 1];
      if (q->count == 0 && t->full_count[bin] == 0 &&
          t->tcache[bin - 1].count == 0)
        continue;
      size_t used = 0, cap = 0;
      for (mem_page_t* p = q->first; p != NULL; p = p->next) {
        used += p->used;
        cap += p->capacity;
      }
      std::fprintf(stderr,
                   /* 注意 used 与上面 thread heap 行的 current 口径不同：
                    * tcache 滞留块在这里计入 used（页视角仍是已分配），
                    * 在 current 里却已算作已释放。同屏两个"在用"，标明避免误读。 */
                   "  bin %2d (bsize %5zu): pages=%zu full=%u used=%zu/%zu "
                   "(used 含 tcache 滞留块) tcache=%u\n",
                   bin, q->block_size, q->count, (unsigned)t->full_count[bin],
                   used, cap, (unsigned)t->tcache[bin - 1].count);
    }
  }

  int count = mem_arena_count();
  for (int i = 0; i < count; i++) {
    mem_arena_t* a = mem_arena_get(i);
    if (a == NULL) continue;
    size_t free_slices = 0;
    size_t nw = mem_bitmap_words(a->slice_count);
    for (size_t w = 0; w < nw; w++)
      free_slices += (size_t)mem_popcount64(a->slices_free->map.words[w]);
    int64_t expire = a->purge_expire.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "arena %d: base=%p first_slice=%zu slices=%zu free=%zu "
                 "purge_pending=%s\n",
                 i, (void*)a->base, a->first_slice, a->slice_count, free_slices,
                 expire != 0 ? "yes" : "no");
  }
  /* committed / abandoned 已在上面的 process 行报过，不再二次加锁遍历 */
  std::fprintf(stderr,
               "to_full=%llu abandon=%llu reclaim=%llu unfull=%llu retire=%llu "
               "fresh_slice=%llu\n",
               (unsigned long long)g_dbg_to_full.load(),
               (unsigned long long)g_dbg_abandon.load(),
               (unsigned long long)g_dbg_reclaim.load(),
               (unsigned long long)g_dbg_unfull.load(),
               (unsigned long long)g_dbg_retire.load(),
               (unsigned long long)g_dbg_fresh_slice.load());
  std::fprintf(stderr, "slices: alloc=%llu free=%llu live=%lld\n",
               (unsigned long long)g_dbg_slice_alloc.load(),
               (unsigned long long)g_dbg_slice_free.load(),
               (long long)(g_dbg_slice_alloc.load() - g_dbg_slice_free.load()));
  std::fflush(stderr);
}

/* --------------------------- WAVVIX_MEM_DEBUG 泄漏报告 -------------------------- */
#if defined(WAVVIX_MEM_DEBUG)
/* 进程退出时遍历每个 arena 的描述符区间，报告仍存活的块。
 * 描述符与 slice 平铺，因此"遍历区间"就是一次线性扫描，无需页映射表去重。 */
void mem_debug_leak_report(void) {
  /* 先把本线程 tcache 里的块还给页：驻留 tcache 的块页视角仍算 used，不冲刷
   * 就会被逐页报成泄漏（WAVVIX_MEM_DEBUG 从本任务起也启用 tcache）。正常情况下线程
   * 退出钩子已经冲过——atexit 的登记早于 g_thread_guard 的构造，析构逆序时
   * guard 先跑——这里是兜底，兼顾"主线程钩子被平台跳过"的情形。 */
  if (mem_t_theap != NULL) mem_theap_collect(mem_t_theap);

  size_t leaked_pages = 0, leaked_blocks = 0;
  int count = mem_arena_count();
  const size_t constructed = mem_desc_constructed_slices.load(std::memory_order_acquire);
  for (int ai = 0; ai < count; ai++) {
    mem_arena_t* a = mem_arena_get(ai);
    if (a == NULL) continue;
    for (size_t i = 0; i < a->slice_count; i++) {
      const size_t idx = a->first_slice + i;
      if (idx >= constructed) break;
      mem_page_t* page = mem_desc_at(idx);
      if (page->magic.load(std::memory_order_relaxed) != WAVVIX_MEM_PAGE_MAGIC) continue;
      if (page->kind == WAVVIX_MEM_PAGE_CONT) continue;   /* 续页项：首项已计过 */
      /* used 含未 collect 的 xthread_free 块：先 collect 再读，排除误报 */
      mem_page_free_collect(page);
      if (page->used == 0) continue;
      leaked_pages++;
      leaked_blocks += page->used;
      std::fprintf(stderr,
                   "culib mem: leak: page %p bin=%u kind=%u used=%u/%u\n",
                   (void*)page->slice_start, (unsigned)page->bin,
                   (unsigned)page->kind, (unsigned)page->used,
                   (unsigned)page->capacity);
    }
  }
  if (leaked_blocks != 0)
    std::fprintf(stderr, "culib mem: leak report: %zu live blocks in %zu pages\n",
                 leaked_blocks, leaked_pages);
}
#endif /* WAVVIX_MEM_DEBUG */
