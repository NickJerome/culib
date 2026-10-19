#include <culib/optional.h>

#include "fatal.h"

namespace wavvix {
namespace detail {

void optional_bad_value() noexcept {
    fatal("wavvix::optional::value() called when there is no value");
}

}  // namespace detail
}  // namespace wavvix
