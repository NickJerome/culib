// Linux io_uring 后端。选择和 epoll 退回在 io_engine_linux.cpp。

#include "io_engine_uring_linux.h"
#include "io_engine_posix.h"

#include <cerrno>
#include <climits>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__has_include)
#  if __has_include(<linux/io_uring.h>)
#    include <linux/io_uring.h>
#    define WAVVIX_DETAIL_HAVE_URING_HEADER 1
#  endif
#endif

#if defined(WAVVIX_DETAIL_HAVE_URING_HEADER) && defined(__NR_io_uring_setup) && \
    defined(__NR_io_uring_enter)
#  define WAVVIX_DETAIL_HAVE_URING 1
#endif

namespace wavvix {
namespace detail {

#if defined(WAVVIX_DETAIL_HAVE_URING)

class uring_engine {
public:
    uring_engine() noexcept
        : fd(-1), sq_map(0), cq_map(0), sqes_map(0), sq_size(0), cq_size(0),
          sqes_size(0), sq_head(0), sq_tail(0), sq_mask(0), sq_array(0), sq_flags(0),
          cq_head(0), cq_tail(0), cq_mask(0), cqes(0), sqes(0), pending(0) {}

    int fd;
    void* sq_map;
    void* cq_map;
    void* sqes_map;
    std::size_t sq_size;
    std::size_t cq_size;
    std::size_t sqes_size;
    unsigned* sq_head;
    unsigned* sq_tail;
    unsigned* sq_mask;
    unsigned* sq_array;
    unsigned* sq_flags;
    unsigned* cq_head;
    unsigned* cq_tail;
    unsigned* cq_mask;
    io_uring_cqe* cqes;
    io_uring_sqe* sqes;
    std::vector<io_fd_slot> slots;
    std::size_t pending;
};

namespace {

const unsigned char cancel_opcode = 14;  // UAPI: IORING_OP_ASYNC_CANCEL
const unsigned char accept_opcode = 13;  // UAPI: IORING_OP_ACCEPT
const unsigned char connect_opcode = 16; // UAPI: IORING_OP_CONNECT
const unsigned char send_opcode = 26;    // UAPI: IORING_OP_SEND
const unsigned char recv_opcode = 27;    // UAPI: IORING_OP_RECV
const unsigned no_drop_feature = 1u << 1;
const unsigned cq_overflow_flag = 1u << 1;

long enter(uring_engine& engine, unsigned submit, unsigned wait, unsigned flags) {
    return ::syscall(__NR_io_uring_enter, engine.fd, submit, wait, flags, 0, 0);
}

template <typename T>
T* ring_field(void* base, unsigned offset) {
    return reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

bool map_rings(uring_engine& engine, const io_uring_params& params) {
    const std::size_t sq_size = params.sq_off.array +
                                params.sq_entries * sizeof(unsigned);
    const std::size_t cq_size = params.cq_off.cqes +
                                params.cq_entries * sizeof(io_uring_cqe);
    const bool shared = (params.features & IORING_FEAT_SINGLE_MMAP) != 0;
    engine.sq_size = shared && cq_size > sq_size ? cq_size : sq_size;
    engine.sq_map = ::mmap(0, engine.sq_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                           engine.fd, IORING_OFF_SQ_RING);
    if (engine.sq_map == MAP_FAILED) {
        engine.sq_map = 0;
        return false;
    }

    if (shared) {
        engine.cq_map = engine.sq_map;
    } else {
        engine.cq_size = cq_size;
        engine.cq_map = ::mmap(0, engine.cq_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                               engine.fd, IORING_OFF_CQ_RING);
        if (engine.cq_map == MAP_FAILED) {
            engine.cq_map = 0;
            return false;
        }
    }

    engine.sqes_size = params.sq_entries * sizeof(io_uring_sqe);
    engine.sqes_map = ::mmap(0, engine.sqes_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                             engine.fd, IORING_OFF_SQES);
    if (engine.sqes_map == MAP_FAILED) {
        engine.sqes_map = 0;
        return false;
    }

    engine.sq_head = ring_field<unsigned>(engine.sq_map, params.sq_off.head);
    engine.sq_tail = ring_field<unsigned>(engine.sq_map, params.sq_off.tail);
    engine.sq_mask = ring_field<unsigned>(engine.sq_map, params.sq_off.ring_mask);
    engine.sq_array = ring_field<unsigned>(engine.sq_map, params.sq_off.array);
    engine.sq_flags = ring_field<unsigned>(engine.sq_map, params.sq_off.flags);
    engine.cq_head = ring_field<unsigned>(engine.cq_map, params.cq_off.head);
    engine.cq_tail = ring_field<unsigned>(engine.cq_map, params.cq_off.tail);
    engine.cq_mask = ring_field<unsigned>(engine.cq_map, params.cq_off.ring_mask);
    engine.cqes = ring_field<io_uring_cqe>(engine.cq_map, params.cq_off.cqes);
    engine.sqes = static_cast<io_uring_sqe*>(engine.sqes_map);
    return true;
}

// 一个 vCPU 独占一个环；不启用 SQPOLL，所以失败时未消费的 SQE 可以撤回。
int submit(uring_engine& engine, const io_uring_sqe& operation) {
    const unsigned tail = __atomic_load_n(engine.sq_tail, __ATOMIC_RELAXED);
    const unsigned head = __atomic_load_n(engine.sq_head, __ATOMIC_ACQUIRE);
    const unsigned entries = *engine.sq_mask + 1;
    if (tail - head >= entries) return EBUSY;

    const unsigned index = tail & *engine.sq_mask;
    engine.sqes[index] = operation;
    engine.sq_array[index] = index;
    __atomic_store_n(engine.sq_tail, tail + 1, __ATOMIC_RELEASE);
    for (;;) {
        const long sent = enter(engine, 1, 0, 0);
        if (sent == 1 || __atomic_load_n(engine.sq_head, __ATOMIC_ACQUIRE) != head)
            return 0;
        if (sent < 0 && errno == EINTR) continue;
        const int failed = sent < 0 ? errno : EIO;
        __atomic_store_n(engine.sq_tail, tail, __ATOMIC_RELEASE);
        return failed;
    }
}

int submit_request(uring_engine& engine, io_request& request) {
    io_uring_sqe operation;
    std::memset(&operation, 0, sizeof operation);
    operation.fd = request.handle;
    if (request.operation == io_accept) {
        operation.opcode = accept_opcode;
        operation.accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
    } else if (request.operation == io_connect) {
        operation.opcode = connect_opcode;
        operation.addr = static_cast<__u64>(reinterpret_cast<std::uintptr_t>(request.address));
        operation.off = static_cast<__u64>(request.address_length);
    } else if (request.operation == io_socket_transfer) {
        operation.opcode = request.direction == io_read ? recv_opcode : send_opcode;
        operation.addr = static_cast<__u64>(reinterpret_cast<std::uintptr_t>(request.buffer));
        const std::size_t ceiling = static_cast<std::size_t>(UINT_MAX);
        operation.len = static_cast<__u32>(request.length < ceiling ? request.length : ceiling);
        operation.rw_flags = request.direction == io_write ? MSG_NOSIGNAL : 0;
    } else {
        request.iov.iov_base = request.buffer;
        request.iov.iov_len = request.length;
        operation.opcode = request.direction == io_read ? IORING_OP_READV : IORING_OP_WRITEV;
        operation.off = static_cast<__u64>(-1);
        operation.addr = static_cast<__u64>(reinterpret_cast<std::uintptr_t>(&request.iov));
        operation.len = 1;
    }
    operation.user_data = static_cast<__u64>(reinterpret_cast<std::uintptr_t>(&request));
    return submit(engine, operation);
}

int submit_cancel(uring_engine& engine, io_request& request) {
    io_uring_sqe operation;
    std::memset(&operation, 0, sizeof operation);
    operation.opcode = cancel_opcode;
    operation.fd = -1;
    operation.addr = static_cast<__u64>(reinterpret_cast<std::uintptr_t>(&request));
    operation.user_data = operation.addr | 1u;  // 取消命令的 CQE 不对应等待者
    return submit(engine, operation);
}

bool probe_cancel(uring_engine& engine) {
    io_uring_sqe operation;
    std::memset(&operation, 0, sizeof operation);
    operation.opcode = cancel_opcode;
    operation.fd = -1;
    operation.addr = 0;        // 不会找到任何请求
    operation.user_data = 1;   // 只用于识别这个探测完成项
    if (submit(engine, operation) != 0) return false;
    for (;;) {
        const unsigned head = __atomic_load_n(engine.cq_head, __ATOMIC_RELAXED);
        const unsigned tail = __atomic_load_n(engine.cq_tail, __ATOMIC_ACQUIRE);
        if (head != tail) {
            const int result = engine.cqes[head & *engine.cq_mask].res;
            __atomic_store_n(engine.cq_head, head + 1, __ATOMIC_RELEASE);
            if (result == -ENOENT) return true;
            errno = ENOSYS;
            return false;
        }
        if (enter(engine, 0, 1, IORING_ENTER_GETEVENTS) < 0 && errno != EINTR)
            return false;
    }
}

bool probe_socket_operation(uring_engine& engine, unsigned char opcode) {
    io_uring_sqe operation;
    std::memset(&operation, 0, sizeof operation);
    operation.opcode = opcode;
    operation.fd = -1;
    operation.user_data = 1;
    if (submit(engine, operation) != 0) return false;
    for (;;) {
        const unsigned head = __atomic_load_n(engine.cq_head, __ATOMIC_RELAXED);
        const unsigned tail = __atomic_load_n(engine.cq_tail, __ATOMIC_ACQUIRE);
        if (head != tail) {
            const int result = engine.cqes[head & *engine.cq_mask].res;
            __atomic_store_n(engine.cq_head, head + 1, __ATOMIC_RELEASE);
            if (result == -EBADF) return true;
            errno = ENOSYS;
            return false;
        }
        if (enter(engine, 0, 1, IORING_ENTER_GETEVENTS) < 0 && errno != EINTR)
            return false;
    }
}

// 套接字的立即可用路径只做一次系统调用；非套接字交给 io_uring。
bool try_socket_now(io_request& request) {
    ssize_t moved;
    for (;;) {
        moved = request.direction == io_read
                    ? ::recv(request.handle, request.buffer, request.length, MSG_DONTWAIT)
                    : ::send(request.handle, request.buffer, request.length,
                             MSG_DONTWAIT | MSG_NOSIGNAL);
        if (moved >= 0 || errno != EINTR) break;
    }
    if (moved < 0 && (errno == ENOTSOCK || io_would_block(errno))) return false;
    request.transferred = moved < 0 ? -1 : moved;
    request.failure = moved < 0 ? errno : 0;
    return true;
}

std::size_t drain(uring_engine& engine, std::vector<park_token*>& completed) {
    std::size_t fired = 0;
    bool flushed_overflow = false;
    for (;;) {
        const unsigned head = __atomic_load_n(engine.cq_head, __ATOMIC_RELAXED);
        const unsigned tail = __atomic_load_n(engine.cq_tail, __ATOMIC_ACQUIRE);
        if (head == tail) {
            if (!flushed_overflow && (*engine.sq_flags & cq_overflow_flag)) {
                flushed_overflow = true;
                enter(engine, 0, 0, IORING_ENTER_GETEVENTS);
                continue;
            }
            return fired;
        }
        const io_uring_cqe item = engine.cqes[head & *engine.cq_mask];
        __atomic_store_n(engine.cq_head, head + 1, __ATOMIC_RELEASE);
        if (item.user_data & 1u) continue;  // ASYNC_CANCEL 的完成项

        io_request* const request = reinterpret_cast<io_request*>(
            static_cast<std::uintptr_t>(item.user_data));
        if (!request) continue;
        const std::size_t index = static_cast<std::size_t>(request->handle);
        if (index >= engine.slots.size()) continue;
        io_fd_slot& slot = engine.slots[index];
        io_request*& queue = request->direction == io_read ? slot.readers : slot.writers;
        const unsigned bit = request->direction == io_read ? io_armed_read : io_armed_write;
        if (queue != request) continue;

        if (request->operation == io_accept && item.res >= 0) {
            request->accepted = item.res;
            request->transferred = 0;
        } else {
            request->transferred = item.res < 0 ? -1 : item.res;
        }
        request->failure = item.res < 0 ? -item.res : 0;
        io_hand_back(queue, completed, engine.pending);
        ++fired;
        slot.armed &= ~bit;
        if (queue) {
            const int failed = submit_request(engine, *queue);
            if (failed == 0) {
                slot.armed |= bit;
            } else {
                io_fail_chain(queue, failed, completed, engine.pending);
            }
        }
    }
}

}  // namespace

uring_engine* uring_engine_create() noexcept {
    io_uring_params params;
    std::memset(&params, 0, sizeof params);
    const long fd = ::syscall(__NR_io_uring_setup, 64u, &params);
    if (fd < 0) return 0;
    uring_engine* const engine = new uring_engine();
    engine->fd = static_cast<int>(fd);
    if (!(params.features & no_drop_feature) || !map_rings(*engine, params) ||
        !probe_cancel(*engine) || !probe_socket_operation(*engine, send_opcode) ||
        !probe_socket_operation(*engine, recv_opcode) ||
        !probe_socket_operation(*engine, accept_opcode) ||
        !probe_socket_operation(*engine, connect_opcode)) {
        const int failed = errno ? errno : ENOSYS;
        uring_engine_destroy(engine);
        errno = failed;
        return 0;
    }
    return engine;
}

void uring_engine_destroy(uring_engine* engine) noexcept {
    if (!engine) return;
    if (engine->sqes_map) ::munmap(engine->sqes_map, engine->sqes_size);
    if (engine->cq_map && engine->cq_map != engine->sq_map)
        ::munmap(engine->cq_map, engine->cq_size);
    if (engine->sq_map) ::munmap(engine->sq_map, engine->sq_size);
    if (engine->fd >= 0) ::close(engine->fd);
    delete engine;
}

bool uring_engine_adopt(uring_engine* engine, io_handle handle) noexcept {
    return engine != 0 && handle >= 0;
}

bool uring_engine_arm(uring_engine* engine, io_request& request) noexcept {
    const int fd = request.handle;
    if (!engine || fd < 0) {
        request.transferred = -1;
        request.failure = EBADF;
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(fd);
    if (index >= engine->slots.size())
        engine->slots.resize(index * 2 + 2, io_empty_slot());
    io_fd_slot& slot = engine->slots[index];
    io_request*& queue = request.direction == io_read ? slot.readers : slot.writers;
    if (!queue && request.operation != io_accept && request.operation != io_connect &&
        try_socket_now(request)) return false;

    request.next = 0;
    io_request** tail = &queue;
    while (*tail) tail = &(*tail)->next;
    *tail = &request;
    if (queue == &request) {
        const int failed = submit_request(*engine, request);
        if (failed != 0) {
            queue = 0;
            request.transferred = -1;
            request.failure = failed;
            return false;
        }
        slot.armed |= request.direction == io_read ? io_armed_read : io_armed_write;
    }
    ++engine->pending;
    return true;
}

std::size_t uring_engine_wait(uring_engine* engine, std::chrono::nanoseconds timeout,
                              std::vector<park_token*>& completed) noexcept {
    if (!engine) return 0;
    typedef std::chrono::steady_clock clock;
    const bool timed = timeout.count() >= 0;
    const clock::time_point deadline = timed ? clock::now() + timeout : clock::time_point();
    for (;;) {
        const std::size_t fired = drain(*engine, completed);
        if (fired != 0) return fired;
        struct timespec limit;
        struct timespec* wait_for = 0;
        if (timed) {
            const clock::time_point now = clock::now();
            if (now >= deadline) return 0;
            const std::chrono::nanoseconds left =
                std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
            limit.tv_sec = static_cast<time_t>(left.count() / 1000000000);
            limit.tv_nsec = static_cast<long>(left.count() % 1000000000);
            wait_for = &limit;
        }
        struct pollfd descriptor = {engine->fd, POLLIN, 0};
        const int ready = ::ppoll(&descriptor, 1, wait_for, 0);
        if (ready == 0) return 0;
        if (ready < 0 && errno != EINTR) return 0;
    }
}

bool uring_engine_cancel(uring_engine* engine, io_request& request) noexcept {
    if (!engine || request.handle < 0 ||
        static_cast<std::size_t>(request.handle) >= engine->slots.size()) return true;
    io_fd_slot& slot = engine->slots[static_cast<std::size_t>(request.handle)];
    io_request*& queue = request.direction == io_read ? slot.readers : slot.writers;
    if (queue == &request) {
        // 原请求仍在内核手里；等它的 CQE 后才能释放 request 所在的协程栈。
        if (submit_cancel(*engine, request) != 0) std::abort();
        return false;
    }
    if (io_unlink(queue, request) && engine->pending) --engine->pending;
    return true;
}

std::size_t uring_engine_pending(const uring_engine* engine) noexcept {
    return engine ? engine->pending : 0;
}

bool uring_engine_cq_overflowed(const uring_engine* engine) noexcept {
    return engine && (__atomic_load_n(engine->sq_flags, __ATOMIC_ACQUIRE) & cq_overflow_flag);
}

#else

class uring_engine {};

uring_engine* uring_engine_create() noexcept { errno = ENOSYS; return 0; }
void uring_engine_destroy(uring_engine*) noexcept {}
bool uring_engine_adopt(uring_engine*, io_handle) noexcept { return false; }
bool uring_engine_arm(uring_engine*, io_request&) noexcept { return false; }
std::size_t uring_engine_wait(uring_engine*, std::chrono::nanoseconds,
                              std::vector<park_token*>&) noexcept { return 0; }
bool uring_engine_cancel(uring_engine*, io_request&) noexcept { return true; }
std::size_t uring_engine_pending(const uring_engine*) noexcept { return 0; }
bool uring_engine_cq_overflowed(const uring_engine*) noexcept { return false; }

#endif

}  // namespace detail
}  // namespace wavvix
