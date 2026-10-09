#pragma once

#include <culib/result.h>
#include <culib/string_view.h>

#include <cstdint>

namespace wavvix {
namespace fs {
namespace detail {

struct snapshot_metadata {
    std::uint64_t device;
    std::uint64_t identity;
    std::uint64_t size;
    std::uint64_t modified;
    unsigned type;
    bool directory;
    bool link;
};

result<snapshot_metadata> read_snapshot_metadata(string_view path);

}  // namespace detail
}  // namespace fs
}  // namespace wavvix
