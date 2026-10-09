/* ----------------------------------------------------------------------------
 * bitmap.h — 位图与带 rover 提示的连续空闲段查找
 * ------------------------------------------------------------------------- */
#ifndef WAVVIX_MEM_BITMAP_H
#define WAVVIX_MEM_BITMAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct mem_bitmap_t {
  uint64_t* words;     /* 存储（由调用方从 meta 池分配） */
  size_t    nbits;
};

/* 从上次成功的位置继续查找；位扫描按字进行。 */
struct mem_bbitmap_t {
  mem_bitmap_t map;
  size_t      hint;    /* 上次分配成功的字下标（rover），下次分配从这里起步 */
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
bool mem_bitmap_any(const mem_bitmap_t* bm);

/* 在"1=空闲"的位图中找 n 个连续 1，返回起始下标；失败返回 (size_t)-1 */
size_t mem_bitmap_find_free_run(const mem_bitmap_t* bm, size_t n);

/* binned bitmap 封装（接口与语义同位图） */
void   mem_bbitmap_init(mem_bbitmap_t* bm, uint64_t* storage, size_t nbits);
/* 找连续 n 位并占用，返回起点；失败 (size_t)-1。
 * align/base_phase 以位为单位：起点 idx 满足 (base_phase + idx) % align == 0。
 * base_phase 存在的原因：位图下标是 arena 相对的，而调用方要的是**绝对地址**
 * 对齐——arena 基址本身只保证 64KiB 对齐，两者之间的相位差必须显式传进来，
 * 否则 align > 1 时算出的起点在地址空间里并不对齐。align==1 时二者均被忽略。 */
size_t mem_bbitmap_alloc(mem_bbitmap_t* bm, size_t n, size_t align, size_t base_phase);
void   mem_bbitmap_free(mem_bbitmap_t* bm, size_t i, size_t n);

#endif /* WAVVIX_MEM_BITMAP_H */
