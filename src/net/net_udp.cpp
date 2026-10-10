#include "net_udp_detail.h"
#include "runtime/scheduler_io.h"

#include <climits>
#include <utility>

namespace wavvix {
namespace net {

udp_socket::~udp_socket() { close(); }

udp_socket::udp_socket(udp_socket&& other) noexcept
    : native_(other.native_), adopted_(other.adopted_) {
    other.native_ = invalid_handle();
    other.adopted_ = false;
}

udp_socket& udp_socket::operator=(udp_socket&& other) noexcept {
    if (this != &other) {
        close();
        native_ = other.native_;
        adopted_ = other.adopted_;
        other.native_ = invalid_handle();
        other.adopted_ = false;
    }
    return *this;
}

result<udp_socket> udp_socket::bind(const endpoint& local) {
    result<std::uintptr_t> opened = detail::udp_bind_socket(local);
    if (opened.is_err()) return opened.error();
    return udp_socket(opened.value());
}

result<endpoint> udp_socket::local_endpoint() const {
    if (!is_open()) return make_error(error_kind::invalid_argument, "UDP socket is closed");
    return detail::udp_local_endpoint(native_);
}

void udp_socket::close() noexcept {
    if (is_open()) detail::udp_close_socket(native_);
    native_ = invalid_handle();
    adopted_ = false;
}

result<std::size_t> udp_socket::send_to(const void* buffer, std::size_t size,
                                        const endpoint& peer,
                                        std::chrono::nanoseconds timeout) {
    if (!is_open()) return make_error(error_kind::invalid_argument, "UDP socket is closed");
    if (size != 0 && !buffer)
        return make_error(error_kind::invalid_argument, "send buffer is null");
    if (size > static_cast<std::size_t>(UINT_MAX))
        return make_error(error_kind::out_of_range, "UDP datagram is too large");
    if (!adopted_) {
        if (!wavvix::detail::scheduler_adopt(detail::udp_io_handle(native_)))
            return make_error(error_kind::io, "cannot adopt UDP socket");
        adopted_ = true;
    }
    detail::socket_address address;
    detail::encode_address(peer, address);
    char empty = 0;
    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_datagram_transfer;
    request.direction = wavvix::detail::io_write;
    request.handle = detail::udp_io_handle(native_);
    request.buffer = const_cast<void*>(buffer ? buffer : &empty);
    request.length = size;
    request.address = &address.storage;
    request.address_length = address.length;
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) return detail::udp_error("send_to", request.failure);
    if (static_cast<std::size_t>(request.transferred) != size)
        return make_error(error_kind::io, "partial UDP datagram send");
    return size;
}

result<udp_datagram> udp_socket::recv_from(void* buffer, std::size_t size,
                                           std::chrono::nanoseconds timeout) {
    if (!is_open()) return make_error(error_kind::invalid_argument, "UDP socket is closed");
    if (size != 0 && !buffer)
        return make_error(error_kind::invalid_argument, "receive buffer is null");
    if (size > static_cast<std::size_t>(UINT_MAX))
        return make_error(error_kind::out_of_range, "receive buffer is too large");
    if (!adopted_) {
        if (!wavvix::detail::scheduler_adopt(detail::udp_io_handle(native_)))
            return make_error(error_kind::io, "cannot adopt UDP socket");
        adopted_ = true;
    }
    detail::socket_address address;
    address.length = static_cast<int>(sizeof address.storage);
    char empty = 0;
    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_datagram_transfer;
    request.direction = wavvix::detail::io_read;
    request.handle = detail::udp_io_handle(native_);
    request.buffer = buffer ? buffer : &empty;
    request.length = size == 0 ? 1 : size;
    request.address = &address.storage;
    request.address_length = address.length;
    wavvix::detail::scheduler_io(request, timeout);
    if (request.truncated || detail::udp_is_truncated(request.failure) ||
        (size == 0 && request.transferred > 0))
        return make_error(error_kind::out_of_range, "UDP datagram was truncated");
    if (request.transferred < 0) return detail::udp_error("recv_from", request.failure);
    address.length = request.address_length;
    result<endpoint> from = detail::decode_address(
        reinterpret_cast<const sockaddr*>(&address.storage), address.length);
    if (from.is_err()) return from.error();
    return udp_datagram(static_cast<std::size_t>(request.transferred), from.value());
}

}  // namespace net
}  // namespace wavvix
