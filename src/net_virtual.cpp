#include <culib/net_virtual.h>

#include "net_virtual_backend.h"

namespace wavvix {
namespace net {

constexpr std::size_t virtual_interface::max_packet_size;
static_assert(detail::virtual_max_packet_size == virtual_interface::max_packet_size,
              "public virtual packet limit must match the backends");

virtual_interface::~virtual_interface() { close(); }

virtual_interface::virtual_interface(virtual_interface&& other) noexcept
    : state_(other.state_) {
    other.state_ = 0;
}

virtual_interface& virtual_interface::operator=(virtual_interface&& other) noexcept {
    if (this != &other) {
        close();
        state_ = other.state_;
        other.state_ = 0;
    }
    return *this;
}

result<virtual_interface> virtual_interface::open() {
    result<detail::virtual_interface_state*> opened = detail::virtual_interface_open();
    if (opened.is_err()) return opened.error();
    return virtual_interface(opened.value());
}

string_view virtual_interface::name() const noexcept {
    return detail::virtual_interface_name(state_);
}

void virtual_interface::close() noexcept {
    if (state_) detail::virtual_interface_destroy(state_);
    state_ = 0;
}

result<std::size_t> virtual_interface::read(void* buffer, std::size_t capacity,
                                            std::chrono::nanoseconds timeout) {
    if (!state_)
        return make_error(error_kind::invalid_argument, "虚拟网卡已关闭");
    return detail::virtual_interface_read(state_, buffer, capacity, timeout);
}

result<std::size_t> virtual_interface::write(const void* packet, std::size_t size,
                                             std::chrono::nanoseconds timeout) {
    if (!state_)
        return make_error(error_kind::invalid_argument, "虚拟网卡已关闭");
    return detail::virtual_interface_write(state_, packet, size, timeout);
}

}  // namespace net
}  // namespace wavvix
