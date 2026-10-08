#pragma once

#include <cstddef>
#include <cstring>

namespace wavvix {
namespace net {
namespace detail {

const std::size_t punch_session_size = 16;
const std::size_t punch_transaction_size = 12;
const std::size_t punch_packet_size = 4 + 1 + punch_session_size + punch_transaction_size;

enum punch_message_type { punch_probe = 1, punch_ack = 2 };

struct punch_message {
    punch_message_type type;
    unsigned char session[punch_session_size];
    unsigned char transaction[punch_transaction_size];
};

inline void punch_encode(const punch_message& message,
                         unsigned char output[punch_packet_size]) noexcept {
    const unsigned char magic[4] = {'C', 'U', 'P', '1'};
    std::memcpy(output, magic, sizeof magic);
    output[4] = static_cast<unsigned char>(message.type);
    std::memcpy(output + 5, message.session, punch_session_size);
    std::memcpy(output + 5 + punch_session_size, message.transaction,
                punch_transaction_size);
}

inline bool punch_decode(const void* data, std::size_t size,
                         punch_message& message) noexcept {
    if (!data || size != punch_packet_size) return false;
    const unsigned char* const input = static_cast<const unsigned char*>(data);
    const unsigned char magic[4] = {'C', 'U', 'P', '1'};
    if (std::memcmp(input, magic, sizeof magic) != 0 ||
        (input[4] != punch_probe && input[4] != punch_ack)) return false;
    message.type = static_cast<punch_message_type>(input[4]);
    std::memcpy(message.session, input + 5, punch_session_size);
    std::memcpy(message.transaction, input + 5 + punch_session_size,
                punch_transaction_size);
    return true;
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
