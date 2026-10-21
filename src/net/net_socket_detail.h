#pragma once

#include "net_address_detail.h"
#include "runtime/io_engine.h"

namespace wavvix {
namespace net {
namespace detail {

inline wavvix::detail::io_handle io_handle_of(std::uintptr_t native) noexcept {
#if defined(_WIN32)
    return reinterpret_cast<wavvix::detail::io_handle>(native);
#else
    return static_cast<int>(native);
#endif
}

result<std::uintptr_t> open_socket(protocol type, address_family family);
void close_socket(std::uintptr_t native) noexcept;
// 关掉 Nagle。尽力而为：设不上只是小包写得慢一点。
void disable_nagle(std::uintptr_t native) noexcept;
result<void> bind_socket(std::uintptr_t native, protocol type, const endpoint& local);
result<void> listen_socket(std::uintptr_t native, int backlog);
result<endpoint> socket_endpoint(std::uintptr_t native, bool peer);
result<void> connect_socket(std::uintptr_t native, const endpoint& peer, bool bound,
                            std::chrono::nanoseconds timeout);
result<std::uintptr_t> accept_socket(std::uintptr_t native, address_family family,
                                      std::chrono::nanoseconds timeout);
error socket_error(const char* operation, int code);
bool datagram_truncated(int code) noexcept;

// 不把 timeout 加到当前时刻，避免最长 duration 溢出；每一段消耗同一个预算。
class io_budget {
public:
    explicit io_budget(std::chrono::nanoseconds timeout)
        : timeout_(timeout), started_(std::chrono::steady_clock::now()) {}

    std::chrono::nanoseconds remaining() const {
        if (timeout_.count() < 0) {
            return timeout_;
        }
        const std::chrono::nanoseconds elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started_);
        return elapsed >= timeout_ ? std::chrono::nanoseconds(0) : timeout_ - elapsed;
    }

    bool expired() const {
        // 零预算允许继续立即完成的分段，只是不等待；内核超时也不等于总预算耗尽。
        return timeout_.count() > 0 && remaining().count() == 0;
    }

private:
    std::chrono::nanoseconds timeout_;
    std::chrono::steady_clock::time_point started_;
};

}  // namespace detail
}  // namespace net
}  // namespace wavvix
