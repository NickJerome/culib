/* ----------------------------------------------------------------------------
 * compiler.h — 编译器相关宏与内在函数（唯一允许出现编译器宏的头）
 *
 * 分支预测提示、noreturn 属性、位扫描内在函数全部收敛在本头；
 * 其余源码文件禁止出现 __GNUC__ / _MSC_VER 等编译器宏。
 * ------------------------------------------------------------------------- */
#ifndef EDUMALLOC_COMPILER_H
#define EDUMALLOC_COMPILER_H

#include <stdint.h>
#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
/* ------------------------------------------------------------- GCC/Clang */
#  define ED_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define ED_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define ED_NORETURN    __attribute__((noreturn))
static inline int ed_clz64(uint64_t v) { return __builtin_clzll(v); }
static inline int ed_ctz64(uint64_t v) { return __builtin_ctzll(v); }
static inline int ed_popcount64(uint64_t v) { return __builtin_popcountll(v); }
#elif defined(_MSC_VER)
/* ----------------------------------------------------------------- MSVC */
#  include <intrin.h>
/* MSVC 无分支预测内在函数；(!!(x)) 而非裸 (x)：与 GCC 分支保持
 * "结果恒为 0/1" 的同一语义，避免两个分支在非布尔上下文里行为分叉。 */
#  define ED_LIKELY(x)   (!!(x))
#  define ED_UNLIKELY(x) (!!(x))
#  define ED_NORETURN    __declspec(noreturn)
static inline int ed_clz64(uint64_t v) {
  unsigned long r;
  _BitScanReverse64(&r, v);
  return 63 - (int)r;
}
static inline int ed_ctz64(uint64_t v) {
  unsigned long r;
  _BitScanForward64(&r, v);
  return (int)r;
}
/* SWAR popcount：不依赖 POPCNT 指令，无需 /arch 开关。 */
static inline int ed_popcount64(uint64_t v) {
  v = v - ((v >> 1) & 0x5555555555555555ull);
  v = (v & 0x3333333333333333ull) + ((v >> 2) & 0x3333333333333333ull);
  v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0Full;
  return (int)((v * 0x0101010101010101ull) >> 56);
}
#else
#  error "edumalloc: unsupported compiler (need GCC, Clang or MSVC)"
#endif

static_assert(sizeof(void*) == 8, "edumalloc supports 64-bit targets only");

#endif /* EDUMALLOC_COMPILER_H */
