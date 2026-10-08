#pragma once

#include <culib/result.h>
#include <culib/string_view.h>

#include <chrono>
#include <cstddef>

namespace wavvix {
namespace net {
namespace detail {

const std::size_t virtual_max_packet_size = 65575;

struct virtual_interface_state;
result<virtual_interface_state*> virtual_interface_open();
void virtual_interface_destroy(virtual_interface_state* state) noexcept;
string_view virtual_interface_name(const virtual_interface_state* state) noexcept;
result<std::size_t> virtual_interface_read(virtual_interface_state* state, void* buffer,
                                           std::size_t capacity,
                                           std::chrono::nanoseconds timeout);
result<std::size_t> virtual_interface_write(virtual_interface_state* state,
                                            const void* packet, std::size_t size,
                                            std::chrono::nanoseconds timeout);

}  // namespace detail
}  // namespace net
}  // namespace wavvix
