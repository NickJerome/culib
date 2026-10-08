#include <culib/net.h>

#include "net_address_detail.h"

#include <cstring>
#include <vector>

namespace wavvix {
namespace net {
namespace {

const std::size_t offer_header_size = 4 + 16 + 1;

void append_u16(std::vector<unsigned char>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>(value >> 8));
    bytes.push_back(static_cast<unsigned char>(value));
}

void append_u32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>(value >> 24));
    bytes.push_back(static_cast<unsigned char>(value >> 16));
    bytes.push_back(static_cast<unsigned char>(value >> 8));
    bytes.push_back(static_cast<unsigned char>(value));
}

std::uint16_t read_u16(const unsigned char* bytes) noexcept {
    return static_cast<std::uint16_t>((static_cast<unsigned>(bytes[0]) << 8) | bytes[1]);
}

std::uint32_t read_u32(const unsigned char* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}

}  // namespace

result<std::vector<unsigned char>> encode_punch_offer(const punch_offer& offer) {
    if (offer.candidates.empty() ||
        offer.candidates.size() > max_punch_offer_candidates)
        return make_error(error_kind::invalid_argument,
                          "punch offer needs 1 to 32 candidates");
    bool has_session = false;
    for (std::size_t i = 0; i < offer.session.size(); ++i)
        has_session = has_session || offer.session[i] != 0;
    if (!has_session)
        return make_error(error_kind::invalid_argument,
                          "punch offer needs a nonzero session ID");
    std::vector<unsigned char> bytes;
    bytes.reserve(max_punch_offer_size);
    const unsigned char magic[] = {'C', 'U', 'O', '1'};
    bytes.insert(bytes.end(), magic, magic + sizeof magic);
    bytes.insert(bytes.end(), offer.session.begin(), offer.session.end());
    bytes.push_back(static_cast<unsigned char>(offer.candidates.size()));
    for (std::size_t i = 0; i < offer.candidates.size(); ++i) {
        const endpoint& candidate = offer.candidates[i];
        if (!detail::valid_unicast_candidate(candidate))
            return make_error(error_kind::invalid_argument,
                              "punch offer has an invalid candidate");
        const bool ipv4 = candidate.family() == address_family::ipv4;
        bytes.push_back(ipv4 ? 4 : 6);
        append_u16(bytes, candidate.port());
        append_u32(bytes, ipv4 ? 0 : candidate.scope_id());
        const unsigned char* const address = detail::endpoint_access::bytes(candidate);
        bytes.insert(bytes.end(), address, address + (ipv4 ? 4 : 16));
    }
    return bytes;
}

result<punch_offer> decode_punch_offer(const void* data, std::size_t size) {
    if (!data || size < offer_header_size || size > max_punch_offer_size)
        return make_error(error_kind::invalid_argument, "invalid punch offer size");
    const unsigned char* const bytes = static_cast<const unsigned char*>(data);
    const unsigned char magic[] = {'C', 'U', 'O', '1'};
    if (std::memcmp(bytes, magic, sizeof magic) != 0)
        return make_error(error_kind::invalid_argument, "invalid punch offer magic");
    const unsigned count = bytes[20];
    if (count == 0 || count > max_punch_offer_candidates)
        return make_error(error_kind::invalid_argument, "invalid punch offer count");
    punch_offer offer = {};
    std::memcpy(offer.session.data(), bytes + 4, offer.session.size());
    bool has_session = false;
    for (std::size_t i = 0; i < offer.session.size(); ++i)
        has_session = has_session || offer.session[i] != 0;
    if (!has_session)
        return make_error(error_kind::invalid_argument,
                          "punch offer has a zero session ID");
    offer.candidates.reserve(count);
    std::size_t position = offer_header_size;
    for (unsigned i = 0; i < count; ++i) {
        if (size - position < 7)
            return make_error(error_kind::invalid_argument, "truncated punch offer candidate");
        const unsigned family = bytes[position];
        if (family != 4 && family != 6)
            return make_error(error_kind::invalid_argument, "invalid punch offer family");
        const std::size_t address_size = family == 4 ? 4 : 16;
        if (size - position < 7 + address_size)
            return make_error(error_kind::invalid_argument, "truncated punch offer address");
        const std::uint16_t port = read_u16(bytes + position + 1);
        const std::uint32_t scope = read_u32(bytes + position + 3);
        if (port == 0 || (family == 4 && scope != 0))
            return make_error(error_kind::invalid_argument, "invalid punch offer endpoint");
        endpoint candidate = detail::endpoint_access::make(
            family == 4 ? address_family::ipv4 : address_family::ipv6,
            bytes + position + 7, port, scope);
        if (!detail::valid_unicast_candidate(candidate))
            return make_error(error_kind::invalid_argument,
                              "punch offer has an invalid candidate");
        offer.candidates.push_back(candidate);
        position += 7 + address_size;
    }
    if (position != size)
        return make_error(error_kind::invalid_argument, "trailing punch offer bytes");
    return offer;
}

}  // namespace net
}  // namespace wavvix
