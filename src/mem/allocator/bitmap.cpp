// 位图与连续空闲段分配。
#include "bitmap.h"
#include "compiler.h"   /* mem_ctz64 */

#include <cstring>

namespace wavvix { namespace mem { namespace detail {

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

// 从头按字查找连续空闲段；跨字保留 run，跳过已占用区间。
size_t mem_bitmap_alloc(mem_bitmap_t* bm, size_t n, size_t align, size_t base_phase) {
  if (n == 0 || n > bm->nbits) return (size_t)-1;
  const size_t nw = mem_bitmap_words(bm->nbits);
  size_t run_start = 0, run_len = 0;
  for (size_t w = 0; w < nw; w++) {
    uint64_t word = bm->words[w];
    if (word == 0) { run_len = 0; continue; }
    if ((word & 1ull) == 0) run_len = 0;
    while (word != 0) {
      const int start = mem_ctz64(word);
      const uint64_t run = word >> start;
      const int ones = run == ~0ull ? 64 : mem_ctz64(~run);
      if (run_len == 0) run_start = w * 64 + (size_t)start;
      run_len += (size_t)ones;
      if (run_len >= n) {
        size_t idx = run_start;
        if (align > 1) {
          const size_t remainder = (base_phase + idx) % align;
          if (remainder != 0) idx += align - remainder;
        }
        if (idx + n <= run_start + run_len && idx + n <= bm->nbits) {
          mem_bitmap_clear_range(bm, idx, n);
          return idx;
        }
      }
      if (start + ones == 64) break;
      run_len = 0;
      word &= ~(((1ull << ones) - 1) << start);
    }
  }
  return (size_t)-1;
}

} } } // namespace wavvix::mem::detail
