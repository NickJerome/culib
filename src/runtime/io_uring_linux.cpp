// 文件 io_uring 环：裸 syscall + mmap，零第三方依赖。每个工作线程私有，socket 路径
// 完全不经过这里。open 负责一次无副作用的能力验证，失败返回 false 并保留 errno，
// 由引擎归档成初始化选择；之后 arm/reap/cancel 只管真实请求，运行期错误一律经
// request/result 交还调用方，不终止进程。

#include "io_engine_posix.h"
#include "io_uring_linux.h"

#include <cerrno>
#include <cstring>

#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>

// 老的用户态头可能没有这个 hint；没有就当 0，只影响缺页时机不影响正确性。
#if !defined(MAP_POPULATE)
#define MAP_POPULATE 0
#endif

namespace wavvix {
namespace detail {

namespace {

// 固定 256 项；每个操作恰好一个 SQE 和一次 enter。在途上限取 min(SQ, CQ)，
// 所以未收的完成项永远装得下 CQ 环。
const unsigned ring_capacity = 256;

// io_uring_enter(fd, 次数, min_complete, flags, sigset)。
int ring_enter(int fd, unsigned to_submit, unsigned flags) noexcept {
    return static_cast<int>(::syscall(uring_abi::enter_call, fd, to_submit, 0, flags, 0));
}

}  // namespace

file_ring::file_ring() noexcept
    : fd_(-1), sq_map_(0), cq_map_(0), entries_map_(0), sq_bytes_(0), cq_bytes_(0),
      entries_bytes_(0), sq_head_(0), sq_tail_(0), sq_flags_(0), sq_array_(0), cq_head_(0),
      cq_tail_(0), submissions_(0), completions_(0), sq_mask_(0), cq_mask_(0), capacity_(0),
      active_(0), pending_(0), head_(0), tail_(0) {}

file_ring::~file_ring() noexcept {
    reset();
}

// 建环并做能力验证。任何一步失败都清干净再返回 false；errno 是与引擎的契约，
// 而 reset 会碰 close/munmap，所以先存后还原。
bool file_ring::open(int epoll_fd) noexcept {
    uring_abi::parameters params;
    std::memset(&params, 0, sizeof params);

    int fd;
    for (;;) {
        fd = static_cast<int>(::syscall(uring_abi::setup_call, ring_capacity, &params));
        if (fd >= 0 || errno != EINTR) {
            break;
        }
    }
    if (fd < 0) {
        return false;
    }
    fd_ = fd;

    // 缺 NODROP 的内核在 CQ 满时会丢完成项；缺 RW_CUR_POS 的不支持 offset=-1。
    // 两个都是能力问题，归档成阻塞池由引擎负责。
    const unsigned required = uring_abi::feature_nodrop | uring_abi::feature_current_position;
    if ((params.features & required) != required) {
        errno = EOPNOTSUPP;
        return fail();
    }

    // 三块共享内存。params.sq/cq 里的字段全部是环内存的偏移；条目数在顶层的
    // sq_entries / cq_entries，映射长度必须用它们。
    sq_bytes_ = static_cast<std::size_t>(params.sq.array) +
                static_cast<std::size_t>(params.sq_entries) * sizeof(unsigned);
    cq_bytes_ = static_cast<std::size_t>(params.cq.completions) +
                static_cast<std::size_t>(params.cq_entries) * sizeof(uring_abi::completion);
    entries_bytes_ =
        static_cast<std::size_t>(params.sq_entries) * sizeof(uring_abi::submission);

    sq_map_ = ::mmap(0, sq_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd_,
                     static_cast<off_t>(uring_abi::sq_offset));
    if (sq_map_ == MAP_FAILED) {
        sq_map_ = 0;
        return fail();
    }

    cq_map_ = ::mmap(0, cq_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd_,
                     static_cast<off_t>(uring_abi::cq_offset));
    if (cq_map_ == MAP_FAILED) {
        cq_map_ = 0;
        return fail();
    }

    entries_map_ = ::mmap(0, entries_bytes_, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE, fd_,
                          static_cast<off_t>(uring_abi::sqe_offset));
    if (entries_map_ == MAP_FAILED) {
        entries_map_ = 0;
        return fail();
    }

    unsigned char* const sq_base = static_cast<unsigned char*>(sq_map_);
    unsigned char* const cq_base = static_cast<unsigned char*>(cq_map_);
    // mask 在环内存自己的偏移上，读出副本，收发时只做算术。
    std::memcpy(&sq_mask_, sq_base + params.sq.mask, sizeof sq_mask_);
    std::memcpy(&cq_mask_, cq_base + params.cq.mask, sizeof cq_mask_);
    sq_head_ = reinterpret_cast<unsigned*>(sq_base + params.sq.head);
    sq_tail_ = reinterpret_cast<unsigned*>(sq_base + params.sq.tail);
    sq_flags_ = reinterpret_cast<unsigned*>(sq_base + params.sq.flags);
    sq_array_ = reinterpret_cast<unsigned*>(sq_base + params.sq.array);
    cq_head_ = reinterpret_cast<unsigned*>(cq_base + params.cq.head);
    cq_tail_ = reinterpret_cast<unsigned*>(cq_base + params.cq.tail);
    submissions_ = static_cast<uring_abi::submission*>(entries_map_);
    completions_ = reinterpret_cast<uring_abi::completion*>(cq_base + params.cq.completions);
    // 在途上限：min(SQ, CQ)，spec 的不变量，不依赖内核默认 cq = 2*sq。
    capacity_ = params.sq_entries < params.cq_entries ? params.sq_entries : params.cq_entries;
    active_ = 0;
    pending_ = 0;
    head_ = 0;
    tail_ = 0;

    // SQ 间接表恒等映射：内核按 array[head..tail) 找 SQE 槽位，不初始化就会
    // 提交到错的槽位上。首次 enter 之前必须就位。
    for (unsigned i = 0; i < capacity_; ++i) {
        sq_array_[i] = i;
    }

    // ring fd 水平触发登记进现有 epoll：CQ 非空就一直报，阻塞的工作线程能被
    // 完成项叫醒。失败保留 errno（老内核 ring fd 不可 poll 时是 EPERM）。
    epoll_event event;
    std::memset(&event, 0, sizeof event);
    event.events = static_cast<std::uint32_t>(EPOLLIN);
    event.data.fd = fd_;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd_, &event) != 0) {
        return fail();
    }

    // READ/WRITE 各探测一次；fd=-1 在提交里内联失败，CQE 随 enter 就到。
    if (!probe(uring_abi::read_operation)) {
        return fail();
    }
    if (!probe(uring_abi::write_operation)) {
        return fail();
    }
    return true;
}

// 挂上一个文件请求。返回 true 表示进了本地记账（在途或排队），调用方应当挂起；
// 返回 false 表示提交失败，结果已写进 request，不挂起。
bool file_ring::arm(io_request& request) noexcept {
    if (request.handle < 0) {
        request.transferred = -1;
        request.failure = EBADF;
        return false;
    }

    // 在途还有名额：直接提交。
    if (active_ < capacity_) {
        if (!submit(request)) {
            return false;
        }
        ++pending_;
        return true;
    }

    // 名额用满：串到本地 FIFO 队尾。pending 含排队与在途；每收一个完成放一个进来。
    request.next = 0;
    if (tail_) {
        tail_->next = &request;
    } else {
        head_ = &request;
    }
    tail_ = &request;
    ++pending_;
    return true;
}

// 把已经到达的完成项全部收走：结果写进各自的请求，交回完成队列，并按空出的名额
// 从 FIFO 放进等着的请求。失败项也当场交回，不卡后继。
void file_ring::reap(io_completion_batch& completed) noexcept {
    if (fd_ < 0) {
        return;
    }

    unsigned head = __atomic_load_n(cq_head_, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);

    // NODROP 之下 CQ 满会把完成项攒在内核内部并立起溢出标志。在途上限让它不该
    // 发生；真发生了就进一次内核冲出来。
    if (head == tail &&
        (__atomic_load_n(sq_flags_, __ATOMIC_ACQUIRE) & uring_abi::flag_cq_overflow) != 0) {
        flush_events();
        head = __atomic_load_n(cq_head_, __ATOMIC_ACQUIRE);
        tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
    }

    while (head != tail) {
        // 先复制再发布 head：发布之后这个槽位就还给内核了。
        const uring_abi::completion done = completions_[head & cq_mask_];
        ++head;
        __atomic_store_n(cq_head_, head, __ATOMIC_RELEASE);

        io_request* const request =
            reinterpret_cast<io_request*>(static_cast<std::uintptr_t>(done.user_data));
        if (request) {
            request->transferred = done.result < 0 ? -1 : done.result;
            request->failure = done.result < 0 ? -done.result : 0;
            if (active_) {
                --active_;
            }
            if (pending_) {
                --pending_;
            }
            completed.push(request);

            // 空出的名额放一个排队的进来。提交成功是排队转在途，pending 不变；
            // 失败当场把错误交回完成队列。
            io_request* const waiting = head_;
            if (waiting) {
                head_ = waiting->next;
                if (!head_) {
                    tail_ = 0;
                }
                waiting->next = 0;
                if (!submit(*waiting)) {
                    if (pending_) {
                        --pending_;
                    }
                    completed.push(waiting);
                }
            }
        }

        // 刚才的空档内核可能又补了完成项进来，重新看一眼队尾。
        tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
    }
}

// 撤销一个还没提交的排队项：摘干净，之后不会有任何通知。在途的撤不动，
// 返回 false，调用方必须等自然完成。
bool file_ring::cancel(io_request& request) noexcept {
    io_request** link = &head_;
    while (*link) {
        if (*link != &request) {
            link = &(*link)->next;
            continue;
        }
        *link = request.next;
        if (tail_ == &request) {
            io_request* last = head_;
            while (last && last->next) {
                last = last->next;
            }
            tail_ = last;
        }
        request.next = 0;
        if (pending_) {
            --pending_;
        }
        return true;
    }
    return false;
}

// 填一个 SQE、发布 tail、enter 一次。返回 true 表示已进内核（在途，结果只会从
// CQE 来）；返回 false 表示没进内核，失败已同步写进 request。
bool file_ring::submit(io_request& request) noexcept {
    const unsigned tail = __atomic_load_n(sq_tail_, __ATOMIC_RELAXED);

    uring_abi::submission& entry = submissions_[tail & sq_mask_];
    std::memset(&entry, 0, sizeof entry);
    entry.operation = request.direction == io_read ? uring_abi::read_operation
                                                   : uring_abi::write_operation;
    entry.fd = request.handle;
    entry.offset = UINT64_MAX;   // -1：用内核当前文件位置并推进它；O_APPEND 由内核执行
    entry.address =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(request.buffer));
    entry.length = static_cast<std::uint32_t>(request.length);
    entry.user_data =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&request));

    // release 发布 tail：SQE 内容先于队尾对内核可见。
    __atomic_store_n(sq_tail_, tail + 1, __ATOMIC_RELEASE);

    int entered;
    for (;;) {
        entered = ring_enter(fd_, 1, 0);
        if (entered >= 0) {
            break;
        }
        if (errno != EINTR) {
            break;
        }
        // 信号打断不盲重发：先看这个 SQE 是否已被内核消费。消费了就绝不能再
        // enter 同一个请求，结果只能等 CQE。
        if (__atomic_load_n(sq_head_, __ATOMIC_ACQUIRE) == tail + 1) {
            break;
        }
    }

    if (__atomic_load_n(sq_head_, __ATOMIC_ACQUIRE) == tail + 1) {
        // 已消费：不管 enter 报了什么（哪怕 EINTR），请求都在内核手里。
        ++active_;
        return true;
    }

    // 没消费：撤回这次的 tail。槽位内容留给下一次提交覆盖，内核不会再读它。
    __atomic_store_n(sq_tail_, tail, __ATOMIC_RELEASE);
    request.transferred = -1;
    request.failure = entered < 0 ? errno : EIO;
    return false;
}

// 无副作用能力探测：一个 fd=-1、长度 0、不引用任何用户缓冲区的请求，user_data 用
// 0 哨兵。支持的 opcode 应报 EBADF；内核不认识 opcode 报 EINVAL，归一成 EOPNOTSUPP。
// 探测失败随整个环一起销毁，不牵扯任何栈上的 io_request。
bool file_ring::probe(unsigned char operation) noexcept {
    const unsigned tail = __atomic_load_n(sq_tail_, __ATOMIC_RELAXED);

    uring_abi::submission& entry = submissions_[tail & sq_mask_];
    std::memset(&entry, 0, sizeof entry);
    entry.operation = operation;
    entry.fd = -1;
    entry.length = 0;
    entry.user_data = 0;

    __atomic_store_n(sq_tail_, tail + 1, __ATOMIC_RELEASE);

    int entered;
    for (;;) {
        entered = ring_enter(fd_, 1, 0);
        if (entered >= 0) {
            break;
        }
        if (errno != EINTR) {
            break;
        }
        if (__atomic_load_n(sq_head_, __ATOMIC_ACQUIRE) == tail + 1) {
            break;
        }
    }

    if (__atomic_load_n(sq_head_, __ATOMIC_ACQUIRE) != tail + 1) {
        // 没消费就永远不会有它的完成项，不能空转等 CQE；失败随整个环清理。
        errno = entered < 0 ? errno : EIO;
        return false;
    }

    // 已消费：fd=-1 内联失败，CQE 应已可见；每轮刷一次内核的溢出表再收。
    for (;;) {
        const unsigned head = __atomic_load_n(cq_head_, __ATOMIC_ACQUIRE);
        const unsigned produced = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
        if (head == produced) {
            if (!flush_events()) {
                return false;
            }
            continue;
        }

        const uring_abi::completion done = completions_[head & cq_mask_];
        __atomic_store_n(cq_head_, head + 1, __ATOMIC_RELEASE);

        if (done.user_data != 0) {
            errno = EIO;   // 探测期间不该有任何别的完成项
            return false;
        }
        if (done.result == -EBADF) {
            return true;
        }
        if (done.result == -EINVAL) {
            errno = EOPNOTSUPP;   // 内核不认识这个 opcode
            return false;
        }
        if (done.result < 0) {
            errno = -done.result;
            return false;
        }
        errno = EIO;   // fd=-1 竟然成功，环的行为不对
        return false;
    }
}

// 把内核内部攒着的完成项冲进 CQ 环。EINTR 重试，其余错误如实交出去。
bool file_ring::flush_events() noexcept {
    for (;;) {
        if (ring_enter(fd_, 0, uring_abi::enter_getevents) >= 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

// 失败清理。open 失败时 errno 是契约的一部分，先存后还原再返回 false。
bool file_ring::fail() noexcept {
    const int saved = errno;
    reset();
    errno = saved;
    return false;
}

void file_ring::reset() noexcept {
    if (sq_map_) {
        ::munmap(sq_map_, sq_bytes_);
    }
    if (cq_map_) {
        ::munmap(cq_map_, cq_bytes_);
    }
    if (entries_map_) {
        ::munmap(entries_map_, entries_bytes_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }

    fd_ = -1;
    sq_map_ = 0;
    cq_map_ = 0;
    entries_map_ = 0;
    sq_bytes_ = 0;
    cq_bytes_ = 0;
    entries_bytes_ = 0;
    sq_head_ = 0;
    sq_tail_ = 0;
    sq_flags_ = 0;
    sq_array_ = 0;
    cq_head_ = 0;
    cq_tail_ = 0;
    submissions_ = 0;
    completions_ = 0;
    sq_mask_ = 0;
    cq_mask_ = 0;
    capacity_ = 0;
    active_ = 0;
    pending_ = 0;
    head_ = 0;
    tail_ = 0;
}

}  // namespace detail
}  // namespace wavvix
