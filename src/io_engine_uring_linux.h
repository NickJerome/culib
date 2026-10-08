#pragma once

// Linux io_uring 后端，仅由 io_engine_linux.cpp 调用。

#include "io_engine.h"

namespace wavvix {
namespace detail {

class uring_engine;

uring_engine* uring_engine_create() noexcept;
void uring_engine_destroy(uring_engine* engine) noexcept;
bool uring_engine_adopt(uring_engine* engine, io_handle handle) noexcept;
bool uring_engine_arm(uring_engine* engine, io_request& request) noexcept;
std::size_t uring_engine_wait(uring_engine* engine, std::chrono::nanoseconds timeout,
                              std::vector<park_token*>& completed) noexcept;
bool uring_engine_cancel(uring_engine* engine, io_request& request) noexcept;
std::size_t uring_engine_pending(const uring_engine* engine) noexcept;
bool uring_engine_cq_overflowed(const uring_engine* engine) noexcept;

}  // namespace detail
}  // namespace wavvix
