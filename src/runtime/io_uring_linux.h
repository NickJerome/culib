#pragma once

#include "io_engine.h"
#include "io_uring_uapi.h"

#include <cstddef>

namespace wavvix {
namespace detail {

class file_ring {
public:
    file_ring() noexcept;
    ~file_ring() noexcept;
    file_ring(const file_ring&) = delete;
    file_ring& operator=(const file_ring&) = delete;

    bool open(int epoll_fd) noexcept;
    int fd() const noexcept { return fd_; }
    bool arm(io_request& request) noexcept;
    void reap(io_completion_batch& completed) noexcept;
    bool cancel(io_request& request) noexcept;
    std::size_t pending() const noexcept { return pending_; }

private:
    bool submit(io_request& request) noexcept;
    bool probe(unsigned char operation) noexcept;
    bool flush_events() noexcept;
    bool fail() noexcept;
    void reset() noexcept;

    int fd_;
    void* sq_map_;
    void* cq_map_;
    void* entries_map_;
    std::size_t sq_bytes_;
    std::size_t cq_bytes_;
    std::size_t entries_bytes_;
    unsigned* sq_head_;
    unsigned* sq_tail_;
    unsigned* sq_flags_;    // 内核写：CQ 积压标志，收完成时读它冲内核内部的溢出表
    unsigned* sq_array_;    // SQ 间接表；open 时初始化成恒等映射
    unsigned* cq_head_;
    unsigned* cq_tail_;
    uring_abi::submission* submissions_;
    uring_abi::completion* completions_;
    unsigned sq_mask_;
    unsigned cq_mask_;
    unsigned capacity_;
    unsigned active_;
    std::size_t pending_;
    io_request* head_;
    io_request* tail_;
};

}  // namespace detail
}  // namespace wavvix
