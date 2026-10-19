#pragma once

// 文件的原生原语，fs_posix.cpp / fs_win.cpp 各一份；write_all 建在 write_some 上，在 file.cpp。

#include <culib/result.h>
#include <culib/string_view.h>

#include <cstdint>

namespace wavvix { namespace fs { namespace detail {

enum class file_mode { read, truncate, append };

result<std::intptr_t> open_file(string_view path, file_mode mode);

// 不管成功与否都消费句柄（置成 -1），所以再关一次什么也不做。返回原生错误码，0 是成功。
unsigned long close_file(std::intptr_t& handle) noexcept;
error file_error(string_view operation, string_view path, unsigned long code);

// 0 表示空或者不知道。读的过程中大小可能变。
std::uint64_t file_size_hint(std::intptr_t handle) noexcept;
result<std::size_t> read_some(std::intptr_t handle, char* data, std::size_t size,
                             string_view path);
result<std::size_t> write_some(std::intptr_t handle, const char* data, std::size_t size,
                              string_view path);

// 写满为止：短写就接着写；一个字节都没写进去算 io 错误，不在原地打转。
result<void> write_all(std::intptr_t handle, const char* data, std::size_t size, string_view path);

} } }
