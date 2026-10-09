/* ----------------------------------------------------------------------------
 * random.h — splitmix64 每线程随机源（L1 密钥生成 / 页内洗牌）
 * ------------------------------------------------------------------------- */
#ifndef WAVVIX_MEM_RANDOM_H
#define WAVVIX_MEM_RANDOM_H

#include <stdint.h>

namespace wavvix { namespace mem { namespace detail {

/* splitmix64：推进状态并返回 64 位伪随机值 */
static inline uint64_t mem_random_next(uint64_t* state) {
  uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

/* 播种：混合外部熵源 */
static inline uint64_t mem_random_seed(uint64_t x) {
  uint64_t s = x;
  return mem_random_next(&s);
}

} } } // namespace wavvix::mem::detail

#endif /* WAVVIX_MEM_RANDOM_H */
