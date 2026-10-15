#pragma once

// Linux 协程文件读写的原生接入。只被 src/fs/file.cpp 在 Linux 上包含；
// src/fs/file_linux.cpp 只在 Linux 编译（xmake 对 *_linux.cpp 的平台规则）。

#include <culib/result.h>
#include <culib/string_view.h>

#include <cstdint>

namespace wavvix { namespace fs { namespace detail {

// 当前协程所在工作线程的文件路径准备：1 = 原生 io_uring 可用，0 = 这个工作线程的
// 初始化判定是阻塞池（协程外也返回 0），负数 = 初始化错误（负 errno）。判定结果
// 缓存在引擎里，重复调用零成本。
int prepare_file() noexcept;

// 一次（读）或写满为止的多次（写）原生提交，调用前必须先确认 prepare_file() == 1。
// 语义与同步 read_some / write_some（含写满循环）逐条一致：短读短写是答案不是错误、
// 0 是 EOF、错误经 file_error 保持同一份文案与 errno 映射。
result<std::size_t> native_read(std::intptr_t handle, char* data, std::size_t size,
                                string_view path);
result<void> native_write(std::intptr_t handle, const void* data, std::size_t size,
                          string_view path);

} } }
