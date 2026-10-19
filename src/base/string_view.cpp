#include <culib/string_view.h>

#include "fatal.h"

#include <ostream>

namespace wavvix {

const string_view::size_type string_view::npos = static_cast<string_view::size_type>(-1);

namespace detail {

void string_view_out_of_range(const char* what, std::size_t index, std::size_t size) noexcept {
    fatal("wavvix::string_view::%s out of range: index %zu, size %zu", what, index, size);
}

}  // namespace detail

std::ostream& operator<<(std::ostream& os, string_view value) {
    return os.write(value.data(), static_cast<std::streamsize>(value.size()));
}

}  // namespace wavvix
