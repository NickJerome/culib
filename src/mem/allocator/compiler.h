#pragma once

#include <stdint.h>
#include <stddef.h>

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
/* ------------------------------------------------------------- GCC/Clang */
#  define WAVVIX_MEM_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define WAVVIX_MEM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define WAVVIX_MEM_NORETURN    __attribute__((noreturn))
namespace wavvix { namespace mem { namespace detail {
static inline int mem_clz64(uint64_t v) { return __builtin_clzll(v); }
static inline int mem_ctz64(uint64_t v) { return __builtin_ctzll(v); }
static inline int mem_popcount64(uint64_t v) { return __builtin_popcountll(v); }
} } } // namespace wavvix::mem::detail
#elif defined(_MSC_VER)
/* ----------------------------------------------------------------- MSVC */
/* MSVC 无分支预测内在函数；(!!(x)) 而非裸 (x)：与 GCC 分支保持
 * "结果恒为 0/1" 的同一语义，避免两个分支在非布尔上下文里行为分叉。 */
#  define WAVVIX_MEM_LIKELY(x)   (!!(x))
#  define WAVVIX_MEM_UNLIKELY(x) (!!(x))
#  define WAVVIX_MEM_NORETURN    __declspec(noreturn)
namespace wavvix { namespace mem { namespace detail {
static inline int mem_clz64(uint64_t v) {
  unsigned long r;
  _BitScanReverse64(&r, v);
  return 63 - (int)r;
}
static inline int mem_ctz64(uint64_t v) {
  unsigned long r;
  _BitScanForward64(&r, v);
  return (int)r;
}
/* SWAR popcount：不依赖 POPCNT 指令，无需 /arch 开关。 */
static inline int mem_popcount64(uint64_t v) {
  v = v - ((v >> 1) & 0x5555555555555555ull);
  v = (v & 0x3333333333333333ull) + ((v >> 2) & 0x3333333333333333ull);
  v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0Full;
  return (int)((v * 0x0101010101010101ull) >> 56);
}
} } } // namespace wavvix::mem::detail
#else
#  error "culib mem: unsupported compiler (need GCC, Clang or MSVC)"
#endif

static_assert(sizeof(void*) == 8, "culib mem supports 64-bit targets only");
