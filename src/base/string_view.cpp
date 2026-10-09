#include <culib/string_view.h>

#include <cstdio>
#include <cstdlib>
#include <ostream>

namespace wavvix {

const string_view::size_type string_view::npos = static_cast<string_view::size_type>(-1);

namespace detail {

void string_view_out_of_range(const char* what, std::size_t index, std::size_t size) noexcept {
    // 不分配内存、不依赖流，和 result 的误用诊断保持一致。
    std::fprintf(stderr, "wavvix::string_view::%s out of range: index %zu, size %zu\n", what, index,
                 size);
    std::fflush(stderr);
    std::abort();
}

}  // namespace detail

std::ostream& operator<<(std::ostream& os, string_view value) {
    return os.write(value.data(), static_cast<std::streamsize>(value.size()));
}

}  // namespace wavvix
