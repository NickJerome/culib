#include "net_socket_detail.h"
#include "runtime/scheduler_io.h"

#include <algorithm>
#include <climits>
#include <utility>

namespace wavvix {
namespace net {
namespace {

error timed_out() {
    return make_error(error_kind::timeout, "socket operation timed out");
}

result<void> connect_addresses(socket& client, const std::vector<endpoint>& addresses,
                               std::chrono::nanoseconds timeout, const endpoint* local) {
    detail::io_budget budget(timeout);
    error failed = make_error(error_kind::invalid_argument, "no address matches the bound socket family");
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        const endpoint& peer = addresses[i];
        if (local && peer.family() != local->family()) {
            continue;
        }
        if (budget.expired()) {
            return timed_out();
        }
        if (local && !client.is_open()) {
            result<void> bound = client.bind(*local);
            if (bound.is_err()) {
                return bound.error();
            }
        }

        result<void> connected = client.connect(peer, budget.remaining());
        if (connected.is_ok()) {
            return result<void>();
        }
        failed = std::move(connected).error();
    }
    return failed;
}

}  // namespace

socket::socket(protocol type) noexcept
    : native_(invalid_handle()), owner_(nullptr), type_(type),
      family_(address_family::ipv4), phase_(phase::closed) {}

socket::~socket() {
    close();
}

socket::socket(socket&& other) noexcept
    : native_(other.native_), owner_(other.owner_), type_(other.type_),
      family_(other.family_), phase_(other.phase_) {
    other.native_ = invalid_handle();
    other.owner_ = nullptr;
    other.phase_ = phase::closed;
}

socket& socket::operator=(socket&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    close();
    native_ = other.native_;
    owner_ = other.owner_;
    type_ = other.type_;
    family_ = other.family_;
    phase_ = other.phase_;
    other.native_ = invalid_handle();
    other.owner_ = nullptr;
    other.phase_ = phase::closed;
    return *this;
}

void socket::close() noexcept {
    if (is_open()) {
        detail::close_socket(native_);
    }
    native_ = invalid_handle();
    owner_ = nullptr;
    phase_ = phase::closed;
}

result<void> socket::bind(const endpoint& local) {
    if (is_open()) {
        return make_error(error_kind::invalid_argument, "socket is already open");
    }
    if (type_ != protocol::tcp && type_ != protocol::udp) {
        return make_error(error_kind::invalid_argument, "invalid socket protocol");
    }

    result<std::uintptr_t> opened = detail::open_socket(type_, local.family());
    if (opened.is_err()) {
        return opened.error();
    }
    native_ = opened.value();
    result<void> bound = detail::bind_socket(native_, local);
    if (bound.is_err()) {
        close();
        return bound.error();
    }
    family_ = local.family();
    phase_ = phase::bound;
    return result<void>();
}

result<void> socket::bind(string_view numeric_ip, std::uint16_t port) {
    result<endpoint> local = endpoint::parse(numeric_ip, port);
    if (local.is_err()) {
        return local.error();
    }
    return bind(local.value());
}

result<void> socket::listen(int backlog) {
    if (type_ != protocol::tcp) {
        return make_error(error_kind::unsupported, "listen requires TCP");
    }
    if (phase_ != phase::bound && phase_ != phase::listening) {
        return make_error(error_kind::invalid_argument, "listen requires a bound socket");
    }
    if (backlog <= 0) {
        return make_error(error_kind::invalid_argument, "backlog must be positive");
    }

    result<void> listening = detail::listen_socket(native_, backlog);
    if (listening.is_err()) {
        return listening.error();
    }
    phase_ = phase::listening;
    return result<void>();
}

result<void> socket::connect(const endpoint& peer, std::chrono::nanoseconds timeout) {
    if (type_ != protocol::tcp) {
        return make_error(error_kind::unsupported, "connect requires TCP");
    }
    if (phase_ != phase::closed && phase_ != phase::bound) {
        return make_error(error_kind::invalid_argument, "socket is already listening or connecting/connected");
    }
    const bool bound = phase_ == phase::bound;
    if (bound && family_ != peer.family()) {
        return make_error(error_kind::invalid_argument, "peer and bound socket address families differ");
    }

    detail::io_budget budget(timeout);
    if (!bound) {
        result<std::uintptr_t> opened = detail::open_socket(type_, peer.family());
        if (opened.is_err()) {
            return opened.error();
        }
        native_ = opened.value();
        family_ = peer.family();
    }
    if (!wavvix::detail::scheduler_bind(detail::io_handle_of(native_), owner_)) {
        close();
        return make_error(error_kind::io, "cannot adopt TCP socket");
    }

    phase_ = phase::connecting;
    result<void> connected = detail::connect_socket(native_, peer, bound, budget.remaining());
    if (connected.is_err()) {
        close();
        return connected.error();
    }
    phase_ = phase::connected;
    return result<void>();
}

result<void> socket::connect(string_view host, std::uint16_t port,
                             std::chrono::nanoseconds timeout) {
    if (type_ != protocol::tcp) {
        return make_error(error_kind::unsupported, "connect requires TCP");
    }
    if (phase_ != phase::closed && phase_ != phase::bound) {
        return make_error(error_kind::invalid_argument, "socket is already listening or connecting/connected");
    }

    result<std::vector<endpoint>> addresses = resolve(host, port);
    if (addresses.is_err()) {
        return addresses.error();
    }
    if (phase_ == phase::bound) {
        result<endpoint> local = local_endpoint();
        if (local.is_err()) {
            return local.error();
        }
        return connect_addresses(*this, addresses.value(), timeout, &local.value());
    }
    return connect_addresses(*this, addresses.value(), timeout, nullptr);
}

result<socket> socket::accept(std::chrono::nanoseconds timeout) {
    if (type_ != protocol::tcp) {
        return make_error(error_kind::unsupported, "accept requires TCP");
    }
    if (phase_ != phase::listening) {
        return make_error(error_kind::invalid_argument, "accept requires a listening socket");
    }
    if (!wavvix::detail::scheduler_bind(detail::io_handle_of(native_), owner_)) {
        return make_error(error_kind::io, "cannot adopt TCP listener");
    }

    result<std::uintptr_t> accepted = detail::accept_socket(native_, family_, timeout);
    if (accepted.is_err()) {
        return accepted.error();
    }
    socket client;
    client.native_ = accepted.value();
    client.family_ = family_;
    client.phase_ = phase::connected;
    return result<socket>(std::move(client));
}

result<endpoint> socket::local_endpoint() const {
    if (!is_open()) {
        return make_error(error_kind::invalid_argument, "socket is closed");
    }
    return detail::socket_endpoint(native_, false);
}

result<endpoint> socket::peer_endpoint() const {
    if (phase_ != phase::connected) {
        return make_error(error_kind::invalid_argument, "socket is not connected");
    }
    return detail::socket_endpoint(native_, true);
}

result<std::size_t> socket::transfer(void* buffer, std::size_t size, bool reading,
                                    std::chrono::nanoseconds timeout) {
    if (type_ != protocol::tcp) {
        return make_error(error_kind::unsupported, "stream transfer requires TCP");
    }
    if (phase_ != phase::connected) {
        return make_error(error_kind::invalid_argument, "socket is not connected");
    }
    if (size == 0) {
        return std::size_t(0);
    }
    if (!buffer) {
        return make_error(error_kind::invalid_argument, "transfer buffer is null");
    }
    if (!wavvix::detail::scheduler_bind(detail::io_handle_of(native_), owner_)) {
        return make_error(error_kind::io, "cannot adopt TCP socket");
    }

    wavvix::detail::io_request request;
    request.direction = reading ? wavvix::detail::io_read : wavvix::detail::io_write;
    request.operation = wavvix::detail::io_socket_transfer;
    request.handle = detail::io_handle_of(native_);
    request.buffer = buffer;
    request.length = (std::min)(size, static_cast<std::size_t>(INT_MAX));
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) {
        return detail::socket_error(reading ? "read" : "write", request.failure);
    }
    return static_cast<std::size_t>(request.transferred);
}

result<std::size_t> socket::read(void* buffer, std::size_t capacity,
                                std::chrono::nanoseconds timeout) {
    return transfer(buffer, capacity, true, timeout);
}

result<void> socket::transfer_all(void* buffer, std::size_t size, bool reading,
                                  std::chrono::nanoseconds timeout) {
    detail::io_budget budget(timeout);
    std::size_t done = 0;
    do {
        if (done != 0 && budget.expired()) {
            return timed_out();
        }
        void* next = buffer ? static_cast<char*>(buffer) + done : nullptr;
        result<std::size_t> part = transfer(next, size - done, reading, budget.remaining());
        if (part.is_err()) {
            return part.error();
        }
        if (part.value() == 0 && done != size) {
            return make_error(error_kind::io, reading ? "EOF before read_exactly completed"
                                                      : "write made no progress");
        }
        done += part.value();
    } while (done != size);
    return result<void>();
}

result<void> socket::read_exactly(void* buffer, std::size_t size,
                                  std::chrono::nanoseconds timeout) {
    return transfer_all(buffer, size, true, timeout);
}

result<void> socket::write(const void* buffer, std::size_t size,
                           std::chrono::nanoseconds timeout) {
    return transfer_all(const_cast<void*>(buffer), size, false, timeout);
}

result<void> socket::send_to(const void* buffer, std::size_t size, const endpoint& peer,
                             std::chrono::nanoseconds timeout) {
    if (type_ != protocol::udp) {
        return make_error(error_kind::unsupported, "send_to requires UDP");
    }
    if (phase_ != phase::bound) {
        return make_error(error_kind::invalid_argument, "UDP socket must be bound");
    }
    if (peer.family() != family_) {
        return make_error(error_kind::invalid_argument, "peer and bound socket address families differ");
    }
    if (size != 0 && !buffer) {
        return make_error(error_kind::invalid_argument, "send buffer is null");
    }
    if (size > static_cast<std::size_t>(UINT_MAX)) {
        return make_error(error_kind::out_of_range, "UDP datagram is too large");
    }
    if (!wavvix::detail::scheduler_bind(detail::io_handle_of(native_), owner_)) {
        return make_error(error_kind::io, "cannot adopt UDP socket");
    }

    detail::socket_address address;
    detail::encode_address(peer, address);
    char empty = 0;
    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_datagram_transfer;
    request.direction = wavvix::detail::io_write;
    request.handle = detail::io_handle_of(native_);
    request.buffer = const_cast<void*>(buffer ? buffer : &empty);
    request.length = size;
    request.address = &address.storage;
    request.address_length = address.length;
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) {
        return detail::socket_error("send_to", request.failure);
    }
    if (static_cast<std::size_t>(request.transferred) != size) {
        return make_error(error_kind::io, "partial UDP datagram send");
    }
    return result<void>();
}

result<datagram> socket::recv_from(void* buffer, std::size_t capacity,
                                   std::chrono::nanoseconds timeout) {
    if (type_ != protocol::udp) {
        return make_error(error_kind::unsupported, "recv_from requires UDP");
    }
    if (phase_ != phase::bound) {
        return make_error(error_kind::invalid_argument, "UDP socket must be bound");
    }
    if (capacity != 0 && !buffer) {
        return make_error(error_kind::invalid_argument, "receive buffer is null");
    }
    if (capacity > static_cast<std::size_t>(UINT_MAX)) {
        return make_error(error_kind::out_of_range, "receive buffer is too large");
    }
    if (!wavvix::detail::scheduler_bind(detail::io_handle_of(native_), owner_)) {
        return make_error(error_kind::io, "cannot adopt UDP socket");
    }

    detail::socket_address address;
    char empty = 0;
    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_datagram_transfer;
    request.direction = wavvix::detail::io_read;
    request.handle = detail::io_handle_of(native_);
    request.buffer = capacity == 0 ? &empty : buffer;
    request.length = capacity == 0 ? 1 : capacity;
    request.address = &address.storage;
    request.address_length = static_cast<int>(sizeof address.storage);
    wavvix::detail::scheduler_io(request, timeout);
    if (request.truncated || detail::datagram_truncated(request.failure) ||
        (capacity == 0 && request.transferred > 0)) {
        return make_error(error_kind::out_of_range, "UDP datagram was truncated");
    }
    if (request.transferred < 0) {
        return detail::socket_error("recv_from", request.failure);
    }

    result<endpoint> from = detail::decode_address(
        reinterpret_cast<const sockaddr*>(&address.storage), request.address_length);
    if (from.is_err()) {
        return from.error();
    }
    return datagram(static_cast<std::size_t>(request.transferred), from.value());
}

}  // namespace net
}  // namespace wavvix
