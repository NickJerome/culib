#pragma once

#include <culib/result.h>
#include <culib/string_view.h>

#include <cstdint>

namespace wavvix { namespace fs { namespace detail {

enum class file_mode { read, truncate, append };

result<std::intptr_t> open_file(string_view path, file_mode mode);

// Consumes the handle, including on failure; returns a native error code or zero.
unsigned long close_file(std::intptr_t& handle) noexcept;
error file_error(string_view operation, string_view path, unsigned long code);

// Zero means empty or unknown. The size can change while reading.
std::uint64_t file_size_hint(std::intptr_t handle) noexcept;
result<std::size_t> read_some(std::intptr_t handle, char* data, std::size_t size,
                             string_view path);
result<std::size_t> write_some(std::intptr_t handle, const char* data, std::size_t size,
                              string_view path);

} } }
