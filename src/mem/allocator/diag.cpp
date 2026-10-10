#include "internal.h"

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

} } } // namespace wavvix::mem::detail
