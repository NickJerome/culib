#include <culib/version.h>

#define WAVVIX_DETAIL_STRINGIFY_IMPL(x) #x
#define WAVVIX_DETAIL_STRINGIFY(x) WAVVIX_DETAIL_STRINGIFY_IMPL(x)

namespace wavvix {

const char* version() noexcept {
    return WAVVIX_DETAIL_STRINGIFY(WAVVIX_VERSION_MAJOR) "." WAVVIX_DETAIL_STRINGIFY(
        WAVVIX_VERSION_MINOR) "." WAVVIX_DETAIL_STRINGIFY(WAVVIX_VERSION_PATCH);
}

}  // namespace wavvix
