// 私有 UAPI 与现代真实内核头的编译期对照：size、offsetof 与常量。
//
// 生产代码只用 src/runtime/io_uring_uapi.h 里这份固定布局，不依赖系统头是否
// 安装、是什么版本；这个文件把同一份布局和当前系统的 <linux/io_uring.h> 逐字段
// 比较，断言全部在编译期。只在装有现代内核头的 Linux 上编译（xmake 目标
// file_uring_uapi_check，CI 的 Linux 任务显式构建）：旧 GCC 容器没有这个头，
// 也不需要——它编译生产代码时只见到私有头，不含 <linux/io_uring.h>。

#if defined(__linux__)

#include "runtime/io_uring_uapi.h"

#include <linux/io_uring.h>
#include <sys/syscall.h>

#include <cstddef>

namespace abi = wavvix::detail::uring_abi;

#define FIELD(own, native, a, b) \
    static_assert(offsetof(own, a) == offsetof(native, b), "ABI offset mismatch")

static_assert(sizeof(abi::parameters) == sizeof(io_uring_params), "params size");
static_assert(sizeof(abi::submission_offsets) == sizeof(io_sqring_offsets), "SQ offsets size");
static_assert(sizeof(abi::completion_offsets) == sizeof(io_cqring_offsets), "CQ offsets size");
static_assert(sizeof(abi::submission) == sizeof(io_uring_sqe), "SQE size");
static_assert(sizeof(abi::completion) == sizeof(io_uring_cqe), "CQE size");

FIELD(abi::parameters, io_uring_params, sq, sq_off);
FIELD(abi::parameters, io_uring_params, cq, cq_off);
FIELD(abi::parameters, io_uring_params, features, features);
FIELD(abi::parameters, io_uring_params, worker_fd, wq_fd);

FIELD(abi::submission_offsets, io_sqring_offsets, head, head);
FIELD(abi::submission_offsets, io_sqring_offsets, tail, tail);
FIELD(abi::submission_offsets, io_sqring_offsets, mask, ring_mask);
FIELD(abi::submission_offsets, io_sqring_offsets, entries, ring_entries);
FIELD(abi::submission_offsets, io_sqring_offsets, flags, flags);
FIELD(abi::submission_offsets, io_sqring_offsets, dropped, dropped);
FIELD(abi::submission_offsets, io_sqring_offsets, array, array);

FIELD(abi::completion_offsets, io_cqring_offsets, head, head);
FIELD(abi::completion_offsets, io_cqring_offsets, tail, tail);
FIELD(abi::completion_offsets, io_cqring_offsets, mask, ring_mask);
FIELD(abi::completion_offsets, io_cqring_offsets, entries, ring_entries);
FIELD(abi::completion_offsets, io_cqring_offsets, overflow, overflow);
FIELD(abi::completion_offsets, io_cqring_offsets, completions, cqes);
FIELD(abi::completion_offsets, io_cqring_offsets, flags, flags);

FIELD(abi::submission, io_uring_sqe, operation, opcode);
FIELD(abi::submission, io_uring_sqe, flags, flags);
FIELD(abi::submission, io_uring_sqe, priority, ioprio);
FIELD(abi::submission, io_uring_sqe, fd, fd);
FIELD(abi::submission, io_uring_sqe, offset, off);
FIELD(abi::submission, io_uring_sqe, address, addr);
FIELD(abi::submission, io_uring_sqe, length, len);
FIELD(abi::submission, io_uring_sqe, operation_flags, rw_flags);
FIELD(abi::submission, io_uring_sqe, user_data, user_data);
FIELD(abi::submission, io_uring_sqe, buffer_index, buf_index);
FIELD(abi::submission, io_uring_sqe, personality, personality);
FIELD(abi::submission, io_uring_sqe, input_fd, splice_fd_in);

FIELD(abi::completion, io_uring_cqe, user_data, user_data);
FIELD(abi::completion, io_uring_cqe, result, res);
FIELD(abi::completion, io_uring_cqe, flags, flags);

#undef FIELD

static_assert(abi::setup_call == __NR_io_uring_setup, "setup syscall number");
static_assert(abi::enter_call == __NR_io_uring_enter, "enter syscall number");

static_assert(abi::sq_offset == IORING_OFF_SQ_RING, "SQ ring mmap offset");
static_assert(abi::cq_offset == IORING_OFF_CQ_RING, "CQ ring mmap offset");
static_assert(abi::sqe_offset == IORING_OFF_SQES, "SQE array mmap offset");

static_assert(abi::read_operation == IORING_OP_READ, "read opcode");
static_assert(abi::write_operation == IORING_OP_WRITE, "write opcode");

static_assert(abi::feature_nodrop == IORING_FEAT_NODROP, "NODROP feature flag");
static_assert(abi::feature_current_position == IORING_FEAT_RW_CUR_POS,
              "current-position feature flag");
static_assert(abi::enter_getevents == IORING_ENTER_GETEVENTS, "GETEVENTS enter flag");
// 收完成时读 SQ flags 字冲内核溢出表的是 SQ 环标志（5.13 起进 UAPI；6.12 另有
// 同值的 enter 侧 IORING_ENTER_CQ_OVERFLOW，这里只对照生产用到的那个）。
static_assert(abi::flag_cq_overflow == IORING_SQ_CQ_OVERFLOW, "CQ overflow flag");

int main() {
    return 0;
}

#endif  // defined(__linux__)
