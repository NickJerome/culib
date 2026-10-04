#include <culib/optional.h>

#include <cstdio>
#include <cstdlib>

namespace wavvix {
namespace detail {

void optional_bad_value() noexcept {
    // 和 result 的误用诊断一致：直接写 stderr，不分配内存、不依赖流。
    std::fprintf(stderr, "wavvix::optional::value() called when there is no value\n");
    std::fflush(stderr);
    std::abort();
}

}  // namespace detail
}  // namespace wavvix
