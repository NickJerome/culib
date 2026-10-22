#pragma once

// culib 的分配器。要让 new / delete（连同标准容器、std::string、协程的控制块）都改用它，依赖 culib_new_override
// 这个目标就行，不用调下面的函数。下面是 C 风格的接口，给要直接管内存的代码用。
//
// 任何线程分配的块都可以在任何线程释放。分配失败返回空指针；返回的地址至少按 16 字节对齐。块只能交给 cu::mem::free；
// 同一块还两次，打印诊断后终止（还之间这块又被分配出去了的查不出来）。

#include <cu/detail/macros.h>

#include <cstddef>

namespace cu {
namespace mem {

CU_NODISCARD void* alloc(std::size_t size) noexcept;

// alignment 是 2 的幂，最大 32 KiB，否则返回空指针。
CU_NODISCARD void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept;

// 空指针什么也不做。
void free(void* p) noexcept;

}  // namespace mem
}  // namespace cu
