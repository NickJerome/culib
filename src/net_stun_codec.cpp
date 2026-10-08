#include "net_stun_codec.h"
#include "net_address_detail.h"

#include <cstring>

namespace wavvix {
namespace net {
namespace detail {
namespace {

const unsigned char magic_cookie[4] = {0x21, 0x12, 0xa4, 0x42};

unsigned read_u16(const unsigned char* bytes) noexcept {
    return static_cast<unsigned>(bytes[0]) << 8 | bytes[1];
}

}  // namespace

void stun_encode_binding_request(const unsigned char transaction_id[12],
                                 unsigned char out[20]) noexcept {
    out[0] = 0x00;
    out[1] = 0x01;
    out[2] = 0x00;
    out[3] = 0x00;
    std::memcpy(out + 4, magic_cookie, 4);
    std::memcpy(out + 8, transaction_id, 12);
}

result<endpoint> stun_decode_binding_response(const unsigned char* packet,
                                              std::size_t size,
                                              const unsigned char transaction_id[12]) {
    if (!packet || !transaction_id || size < 20)
        return make_error(error_kind::invalid_argument, "STUN 响应头无效");
    if (read_u16(packet) != 0x0101 ||
        std::memcmp(packet + 4, magic_cookie, 4) != 0 ||
        std::memcmp(packet + 8, transaction_id, 12) != 0)
        return make_error(error_kind::invalid_argument, "STUN 响应类型、cookie 或事务 ID 不匹配");
    const std::size_t body_size = read_u16(packet + 2);
    if (body_size % 4 != 0 || body_size != size - 20)
        return make_error(error_kind::invalid_argument, "STUN 响应长度无效");

    const unsigned char* mapped = 0;
    std::size_t mapped_size = 0;
    std::size_t offset = 20;
    while (offset < size) {
        if (size - offset < 4)
            return make_error(error_kind::invalid_argument, "STUN 属性头不完整");
        const unsigned type = read_u16(packet + offset);
        const std::size_t length = read_u16(packet + offset + 2);
        const std::size_t padded = (length + 3) & ~std::size_t(3);
        offset += 4;
        if (padded > size - offset)
            return make_error(error_kind::invalid_argument, "STUN 属性越过报文结尾");
        if (type == 0x0020) {
            if (mapped)
                return make_error(error_kind::invalid_argument, "STUN 映射地址重复");
            mapped = packet + offset;
            mapped_size = length;
        }
        offset += padded;
    }
    if (!mapped)
        return make_error(error_kind::not_found, "STUN 响应缺少 XOR-MAPPED-ADDRESS");
    if (mapped_size < 4 || mapped[0] != 0)
        return make_error(error_kind::invalid_argument, "STUN 映射地址无效");
    if (mapped[1] != 1 && mapped[1] != 2)
        return make_error(error_kind::invalid_argument, "STUN 映射地址族无效");
    const address_family family = mapped[1] == 1 ? address_family::ipv4
                                                    : address_family::ipv6;
    const std::size_t address_size = family == address_family::ipv4 ? 4 : 16;
    if (mapped_size != 4 + address_size)
        return make_error(error_kind::invalid_argument, "STUN 映射地址长度无效");
    const std::uint16_t port = static_cast<std::uint16_t>(read_u16(mapped + 2) ^ 0x2112);
    unsigned char address[16] = {};
    for (std::size_t i = 0; i < address_size; ++i)
        address[i] = mapped[4 + i] ^ (i < 4 ? magic_cookie[i]
                                              : transaction_id[i - 4]);
    return endpoint_access::make(family, address, port, 0);
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
