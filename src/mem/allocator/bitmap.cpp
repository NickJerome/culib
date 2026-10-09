/* ----------------------------------------------------------------------------
 * bitmap.cpp — 位图 / binned bitmap 实现
 * ------------------------------------------------------------------------- */
#include "bitmap.h"
#include "os.h"   /* mem_ctz64 */

#include <cstring>

void mem_bitmap_init(mem_bitmap_t* bm, uint64_t* storage, size_t nbits, bool value) {
  bm->words = storage;
  bm->nbits = nbits;
  std::memset(storage, value ? 0xFF : 0x00, mem_bitmap_words(nbits) * sizeof(uint64_t));
  if (value && (nbits & 63) != 0) {
    /* 清掉尾字中超出 nbits 的位，避免"越界空闲"被分配 */
    storage[nbits >> 6] &= (1ull << (nbits & 63)) - 1;
  }
}

void mem_bitmap_set_range(mem_bitmap_t* bm, size_t i, size_t n) {
  for (size_t k = 0; k < n; k++) mem_bitmap_set(bm, i + k);
}

void mem_bitmap_clear_range(mem_bitmap_t* bm, size_t i, size_t n) {
  for (size_t k = 0; k < n; k++) mem_bitmap_clear(bm, i + k);
}

bool mem_bitmap_any(const mem_bitmap_t* bm) {
  size_t nw = mem_bitmap_words(bm->nbits);
  for (size_t w = 0; w < nw; w++)
    if (bm->words[w] != 0) return true;
  return false;
}

size_t mem_bitmap_find_free_run(const mem_bitmap_t* bm, size_t n) {
  if (n == 0 || n > bm->nbits) return (size_t)-1;
  size_t nw = mem_bitmap_words(bm->nbits);
  size_t run_start = 0, run_len = 0;
  for (size_t w = 0; w < nw; w++) {
    uint64_t word = bm->words[w];
    if (word == 0) { run_len = 0; continue; }
    for (size_t b = 0; b < 64; b++) {
      size_t idx = w * 64 + b;
      if (idx >= bm->nbits) return (size_t)-1;
      if ((word >> b) & 1ull) {
        if (run_len == 0) run_start = idx;
        run_len++;
        if (run_len == n) return run_start;
      } else {
        run_len = 0;
      }
    }
  }
  return (size_t)-1;
}

void mem_bbitmap_init(mem_bbitmap_t* bm, uint64_t* storage, size_t nbits) {
  mem_bitmap_init(&bm->map, storage, nbits, true);
  bm->hint = 0;
}

/* 字级空 run 扫描：ctz 逐 run 跳转（每 run ~2 次 ctz），rover 游标起步 +
 * 环绕重扫，稳态摊还近 O(1)。调用方须持有所属 arena 锁。
 * 对齐：在当前 run 内取第一个满足 (base_phase + idx) % align == 0 的起点，
 * 该起点之后仍需容得下 n 位才算命中；否则继续延长 run 再试。 */
size_t mem_bbitmap_alloc(mem_bbitmap_t* bm, size_t n, size_t align, size_t base_phase) {
  const mem_bitmap_t* m = &bm->map;
  if (n == 0 || n > m->nbits) return (size_t)-1;
  if (align == 0) align = 1;
  size_t nw = mem_bitmap_words(m->nbits);
  size_t start_w = (bm->hint < nw) ? bm->hint : 0;

  for (int pass = 0; pass < 2; pass++) {
    size_t w0 = (pass == 0) ? start_w : 0;
    size_t w1 = (pass == 0) ? nw : start_w;
    size_t run_start = 0, run_len = 0;
    for (size_t w = w0; w < w1; w++) {
      uint64_t word = m->words[w];
      if (word == 0) { run_len = 0; continue; }
      if ((word & 1ull) == 0) run_len = 0;   /* bit0 被占：上一字的 run 在此中断 */
      uint64_t x = word;
      while (x != 0) {
        int s = mem_ctz64(x);                    /* 本字下一个空闲位 */
        uint64_t run = x >> s;
        int ones = (run == ~0ull) ? (64 - s) : mem_ctz64(~run);
        if (run_len == 0) run_start = w * 64 + (size_t)s;
        run_len += (size_t)ones;
        /* 先用 run_len >= n 把绝大多数迭代挡在外面：对齐修正后的起点不可能
         * 早于 run_start，所以 run_len < n 时必然放不下，无需再算。
         * 本循环是 large 分配路径上被反复执行的内层，多出来的两次加法、
         * 一次取模和一个分支不该摊到每次迭代上。 */
        if (run_len >= n) {
          size_t idx = run_start;               /* 对齐后的候选起点 */
          if (align > 1) {
            size_t r = (base_phase + run_start) % align;
            if (r != 0) idx = run_start + (align - r);
          }
          if (run_start + run_len >= idx + n && idx + n <= m->nbits) {  /* 命中 */
            mem_bitmap_clear_range(&bm->map, idx, n);
            bm->hint = idx >> 6;
            return idx;
          }
        }
        if (s + ones >= 64) break;              /* run 顶到字尾，延续到下一字 */
        run_len = 0;
        x &= ~((((1ull << ones) - 1) << s));    /* 清掉已考察的空闲 run */
        x &= ~(1ull << (s + ones));             /* 跳过其后那个占用位 */
      }
      /* x==0 且 run_len!=0：run 顶到字尾被保留，跨字延续 */
    }
  }
  return (size_t)-1;
}

void mem_bbitmap_free(mem_bbitmap_t* bm, size_t i, size_t n) {
  mem_bitmap_set_range(&bm->map, i, n);
  if ((i >> 6) < bm->hint) bm->hint = i >> 6;   /* 回收早于游标：回退游标 */
}
