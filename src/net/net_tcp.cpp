#include "net_tcp_detail.h"
#include "runtime/scheduler_detail.h"

#include <utility>

namespace wavvix {
namespace net {

tcp_stream::~tcp_stream() { close(); }

tcp_stream::tcp_stream(tcp_stream&& other) noexcept
    : native_(other.native_), adopted_(other.adopted_) {
    other.native_ = invalid_handle();
    other.adopted_ = false;
}

tcp_stream& tcp_stream::operator=(tcp_stream&& other) noexcept {
    if (this != &other) {
        close();
        native_ = other.native_;
        adopted_ = other.adopted_;
        other.native_ = invalid_handle();
        other.adopted_ = false;
    }
    return *this;
}

tcp_listener::~tcp_listener() { close(); }

tcp_listener::tcp_listener(tcp_listener&& other) noexcept
    : native_(other.native_), adopted_(other.adopted_) {
    other.native_ = invalid_handle();
    other.adopted_ = false;
}

tcp_listener& tcp_listener::operator=(tcp_listener&& other) noexcept {
    if (this != &other) {
        close();
        native_ = other.native_;
        adopted_ = other.adopted_;
        other.native_ = invalid_handle();
        other.adopted_ = false;
    }
    return *this;
}

result<std::size_t> tcp_stream::read(void* buffer, std::size_t size,
                                      std::chrono::nanoseconds timeout) {
    if (!is_open()) return make_error(error_kind::invalid_argument, "stream is closed");
    if (size == 0) return std::size_t(0);
    if (!buffer) return make_error(error_kind::invalid_argument, "read buffer is null");
    if (!adopted_) {
        if (!wavvix::detail::scheduler_adopt(detail::io_handle_of(native_)))
            return make_error(error_kind::io, "cannot adopt TCP stream");
        adopted_ = true;
    }
    wavvix::detail::io_request request;
    request.direction = wavvix::detail::io_read;
    request.operation = wavvix::detail::io_socket_transfer;
    request.handle = detail::io_handle_of(native_);
    request.buffer = buffer;
    request.length = size;
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) return detail::tcp_error("read", request.failure);
    return static_cast<std::size_t>(request.transferred);
}

result<std::size_t> tcp_stream::write(const void* buffer, std::size_t size,
                                       std::chrono::nanoseconds timeout) {
    if (!is_open()) return make_error(error_kind::invalid_argument, "stream is closed");
    if (size == 0) return std::size_t(0);
    if (!buffer) return make_error(error_kind::invalid_argument, "write buffer is null");
    if (!adopted_) {
        if (!wavvix::detail::scheduler_adopt(detail::io_handle_of(native_)))
            return make_error(error_kind::io, "cannot adopt TCP stream");
        adopted_ = true;
    }
    wavvix::detail::io_request request;
    request.direction = wavvix::detail::io_write;
    request.operation = wavvix::detail::io_socket_transfer;
    request.handle = detail::io_handle_of(native_);
    request.buffer = const_cast<void*>(buffer);
    request.length = size;
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) return detail::tcp_error("write", request.failure);
    return static_cast<std::size_t>(request.transferred);
}

}  // namespace net
}  // namespace wavvix
