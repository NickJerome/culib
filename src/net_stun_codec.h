#pragma once

#include <culib/net.h>

#include <cstddef>

namespace wavvix {
namespace net {
namespace detail {

const std::size_t stun_transaction_id_size = 12;
const std::size_t stun_binding_request_size = 20;

void stun_encode_binding_request(const unsigned char transaction_id[12],
                                 unsigned char out[20]) noexcept;

result<endpoint> stun_decode_binding_response(const unsigned char* packet,
                                              std::size_t size,
                                              const unsigned char transaction_id[12]);

}  // namespace detail
}  // namespace net
}  // namespace wavvix
