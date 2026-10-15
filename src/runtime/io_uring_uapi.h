#pragma once

// Linux io_uring 的固定 UAPI 布局。旧用户态内核头没有这些声明；不依赖其安装版本。

#include <cstddef>
#include <cstdint>

#if !defined(__linux__) || (!defined(__x86_64__) && !defined(__aarch64__))
#error "io_uring requires Linux x86_64 or arm64"
#endif

namespace wavvix {
namespace detail {
namespace uring_abi {

const long setup_call = 425;
const long enter_call = 426;
const std::uint64_t sq_offset = 0;
const std::uint64_t cq_offset = 0x8000000;
const std::uint64_t sqe_offset = 0x10000000;
const unsigned feature_nodrop = 1u << 1;
const unsigned feature_current_position = 1u << 3;
const unsigned enter_getevents = 1u << 0;
const unsigned flag_cq_overflow = 1u << 1;
const unsigned char read_operation = 22;
const unsigned char write_operation = 23;

struct submission_offsets {
    std::uint32_t head;
    std::uint32_t tail;
    std::uint32_t mask;
    std::uint32_t entries;
    std::uint32_t flags;
    std::uint32_t dropped;
    std::uint32_t array;
    std::uint32_t reserved;
    std::uint64_t address;
};

struct completion_offsets {
    std::uint32_t head;
    std::uint32_t tail;
    std::uint32_t mask;
    std::uint32_t entries;
    std::uint32_t overflow;
    std::uint32_t completions;
    std::uint32_t flags;
    std::uint32_t reserved;
    std::uint64_t address;
};

struct parameters {
    std::uint32_t sq_entries;
    std::uint32_t cq_entries;
    std::uint32_t flags;
    std::uint32_t polling_cpu;
    std::uint32_t polling_idle;
    std::uint32_t features;
    std::uint32_t worker_fd;
    std::uint32_t reserved[3];
    submission_offsets sq;
    completion_offsets cq;
};

struct submission {
    std::uint8_t operation;
    std::uint8_t flags;
    std::uint16_t priority;
    std::int32_t fd;
    std::uint64_t offset;
    std::uint64_t address;
    std::uint32_t length;
    std::uint32_t operation_flags;
    std::uint64_t user_data;
    std::uint16_t buffer_index;
    std::uint16_t personality;
    std::int32_t input_fd;
    std::uint64_t reserved[2];
};

struct completion {
    std::uint64_t user_data;
    std::int32_t result;
    std::uint32_t flags;
};

static_assert(sizeof(parameters) == 120, "io_uring_params ABI");
static_assert(offsetof(parameters, sq) == 40, "io_uring SQ offsets ABI");
static_assert(offsetof(parameters, cq) == 80, "io_uring CQ offsets ABI");
static_assert(sizeof(submission) == 64, "io_uring_sqe ABI");
static_assert(offsetof(submission, user_data) == 32, "io_uring user_data ABI");
static_assert(sizeof(completion) == 16, "io_uring_cqe ABI");

}  // namespace uring_abi
}  // namespace detail
}  // namespace wavvix
