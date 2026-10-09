/* ----------------------------------------------------------------------------
 * diag.cpp — 诊断与报错：fatal 报告、统计快照、WAVVIX_MEM_DEBUG 泄漏报告
 *
 * fatal 路径必须无分配、无 stdio：错误发生时堆状态已不可信，而 override
 * 场景下 fprintf 自身还会再次调用 malloc。因此这里手写十六进制/十进制格式化
 * 到 256 字节栈缓冲，最终只经 mem_os_fatal 走一次裸 write。
 * ------------------------------------------------------------------------- */
#include "internal.h"

#include <cstdio>
#include <cstring>

namespace wavvix { namespace mem { namespace detail {

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

/* 进程级字段逐个 arena 加锁读取；不可在 fatal 路径或信号处理函数里调用。 */
void mem_stats_get(mem_stats_t* out) {
  if (out == NULL) return;
  mem_init_once();
  std::memset(out, 0, sizeof(*out));
  out->reserved_bytes    = mem_window_size;
  out->committed_bytes   = mem_arena_committed_bytes();
  out->arena_count       = (size_t)mem_arena_count();
  out->abandoned_pages   = mem_arena_abandoned_count();
  out->process_rss_bytes = mem_os_process_rss();
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

} } } // namespace wavvix::mem::detail
