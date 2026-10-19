#include "fatal.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace wavvix {
namespace detail {

void fatal(const char* format, ...) noexcept {
    std::va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::abort();
}

}  // namespace detail
}  // namespace wavvix
