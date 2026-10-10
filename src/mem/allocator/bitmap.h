#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace wavvix { namespace mem { namespace detail {

struct mem_bitmap_t {
  uint64_t* words;     /* 存储（由调用方从 meta 池分配） */
  size_t    nbits;
};

static inline size_t mem_bitmap_words(size_t nbits) { return (nbits + 63) / 64; }

/* value=true：全部置 1（全空闲）；false：全部清 0 */
void mem_bitmap_init(mem_bitmap_t* bm, uint64_t* storage, size_t nbits, bool value);

static inline void mem_bitmap_set(mem_bitmap_t* bm, size_t i) {
  bm->words[i >> 6] |= (1ull << (i & 63));
}
static inline void mem_bitmap_clear(mem_bitmap_t* bm, size_t i) {
  bm->words[i >> 6] &= ~(1ull << (i & 63));
}
static inline bool mem_bitmap_test(const mem_bitmap_t* bm, size_t i) {
  return (bm->words[i >> 6] >> (i & 63)) & 1ull;
}
void mem_bitmap_set_range(mem_bitmap_t* bm, size_t i, size_t n);
void mem_bitmap_clear_range(mem_bitmap_t* bm, size_t i, size_t n);

/* 找连续 n 位并占用，返回起点；失败 (size_t)-1。
 * align/base_phase 以位为单位：起点 idx 满足 (base_phase + idx) % align == 0。
 * align==1 时忽略 base_phase；调用方负责同步。 */
size_t mem_bitmap_alloc(mem_bitmap_t* bm, size_t n, size_t align, size_t base_phase);

} } } // namespace wavvix::mem::detail
