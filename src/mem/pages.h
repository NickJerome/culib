#pragma once

#include <cstddef>

namespace wavvix { namespace mem { namespace detail { namespace pages {

std::size_t page_size() noexcept;

// 分配可读写、已清零的页，不经过分配器。
void* allocate(std::size_t size) noexcept;
bool release(void* address, std::size_t size) noexcept;

// 保留地址空间，起点按 alignment 对齐（2 的幂，至少 64 KiB）；size 是页大小的整数倍。
// 失败返回空。Linux 默认允许 overcommit，这时保留下来的空间已经可读写，第一次写才占物理内存。
void* reserve(std::size_t size, std::size_t alignment) noexcept;
// 让保留的页可读写。在 Linux 默认配置下什么都不做，所以不能用它撤销 protect。
bool commit(void* address, std::size_t size) noexcept;
// 丢弃内容、归还物理内存，之后读到全零。在 Linux 默认配置下页仍然可读写。
bool decommit(void* address, std::size_t size) noexcept;
// 保留内容但禁止访问，例如协程栈的保护页。
bool protect(void* address, std::size_t size) noexcept;
// 撤销 protect，恢复成可读写。
bool unprotect(void* address, std::size_t size) noexcept;
// 这段地址不用透明大页。只有 Linux 有透明大页，其他平台什么都不做。稀疏使用的大段地址（分配器的
// 地址块、协程栈）在 THP 设成 always 的系统上，碰一个字节就可能占满 2 MiB 物理内存。要对整段映射调用：
// 只标其中一部分会把映射拆开。内核不支持透明大页时调用会失败，那本来就没有大页，所以不报错。
void no_huge_pages(void* address, std::size_t size) noexcept;

} } } } // namespace wavvix::mem::detail::pages
