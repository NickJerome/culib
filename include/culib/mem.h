#pragma once

#include <culib/detail/macros.h>

#include <cstddef>

#if !(((defined(__linux__) || defined(__APPLE__)) && (defined(__x86_64__) || defined(__aarch64__))) || defined(_WIN64))
#error "wavvix::mem currently supports only 64-bit Linux, macOS, and Windows"
#endif

namespace wavvix {
namespace mem {

// 想让 std::vector、std::string、new 等改用本分配器，不用调用下面这些函数：依赖
// culib_new_override 覆盖全局 new/delete 即可，Linux 还可以用 LD_PRELOAD 换掉 malloc，
// 见 docs/mem.md。下面是 C 语义的底层接口，给 C 风格的代码和这两个覆盖目标用。
//
// 全部线程安全；任何线程分配的块都可以在任何线程释放。分配失败返回空指针，
// 返回的地址至少按 16 字节对齐。块只能交给本命名空间的 free / realloc。
//
// 释放时检查误用：不是本分配器给出的指针、块中间的地址、重复释放，打印一行
// "culib mem: fatal: ..." 后 abort，release 构建同样检查。
//
// 返回新块的函数丢掉返回值时编译器会警告：alloc 的结果没人接就是泄漏，realloc 的
// 结果没人接则原指针可能已经释放。
WAVVIX_NODISCARD void* alloc(std::size_t size) noexcept;

// 空指针什么都不做。
void free(void* p) noexcept;

// count * size 溢出时返回空指针。
WAVVIX_NODISCARD void* calloc(std::size_t count, std::size_t size) noexcept;

// p 为空等同 alloc；size 为 0 时释放 p 并返回空指针。失败时 p 原样保留。
WAVVIX_NODISCARD void* realloc(void* p, std::size_t size) noexcept;

// alignment 必须是 2 的幂，最大 32 MiB，否则返回空指针；size 任意。
WAVVIX_NODISCARD void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept;

// 块实际可用的字节数。空指针、不是本分配器给出的地址、块中间的地址都返回 0。
std::size_t usable_size(const void* p) noexcept;

// 马上把本线程的空页、已退出线程留下的空页和空闲内存还给系统。
// 不调用也会还：空出来的内存至少过 1 秒，在之后的分配或释放里归还。
void collect() noexcept;

}  // namespace mem
}  // namespace wavvix
