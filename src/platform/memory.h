#pragma once

// 系统的虚拟内存，按页操作：保留地址、提交、还内存、还地址。协程栈和分配器用它；它自己不经过 malloc。
// 地址和大小都要按页对齐。

#include <cstddef>

namespace cu {
namespace detail {

std::size_t page_size() noexcept;

// 保留 size 字节的地址，还不能访问，也不占内存。失败返回空。
void* reserve_pages(std::size_t size) noexcept;

// 同上，起点按 alignment 对齐。alignment 是 2 的幂，最大 64 KiB（Windows 保留的地址本来就按 64 KiB 对齐）。
void* reserve_aligned_pages(std::size_t size, std::size_t alignment) noexcept;

// 让保留的页可读写。用到哪页才占哪页物理内存。失败（内存不够、超过提交上限）返回 false。
bool commit_pages(void* address, std::size_t size) noexcept;

// 把物理内存还给系统，地址留着，内容不保留。之后要用先调 reuse_pages。
void decommit_pages(void* address, std::size_t size) noexcept;

// 让 decommit_pages 还过内存的页重新可读写。Linux 上那些页一直可读写，什么也不做。失败返回 false。
bool reuse_pages(void* address, std::size_t size) noexcept;

// 把 reserve_pages、reserve_aligned_pages 拿到的整段地址还掉。
void release_pages(void* address, std::size_t size) noexcept;

// 这段地址不用透明大页（只有 Linux 有事做）：块稀疏地落在一大段地址里时，开着透明大页的系统上碰一个字节
// 就可能占满 2 MiB。
void no_huge_pages(void* address, std::size_t size) noexcept;

}  // namespace detail
}  // namespace cu
