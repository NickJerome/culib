#pragma once

#include <culib/result.h>
#include <culib/string_view.h>

#include <cstddef>

namespace mesh {
namespace detail {

wavvix::result<void> read_identity_bytes(
    wavvix::string_view path, unsigned char* output, std::size_t size);
wavvix::result<void> publish_identity_bytes(
    wavvix::string_view path, const unsigned char* input, std::size_t size);

}  // namespace detail
}  // namespace mesh
