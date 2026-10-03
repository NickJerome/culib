#include <culib/result.h>

#include <cstdio>
#include <cstdlib>
#include <ostream>

namespace wavvix {

error::error(error_kind kind, std::string message) : kind_(kind), message_(std::move(message)) {}

const char* error_kind_name(error_kind kind) noexcept {
    switch (kind) {
    case error_kind::unknown: return "unknown";
    case error_kind::invalid_argument: return "invalid_argument";
    case error_kind::out_of_range: return "out_of_range";
    case error_kind::not_found: return "not_found";
    case error_kind::already_exists: return "already_exists";
    case error_kind::permission_denied: return "permission_denied";
    case error_kind::timeout: return "timeout";
    case error_kind::unsupported: return "unsupported";
    case error_kind::io: return "io";
    }
    return "unknown";
}

std::string error::to_string() const {
    const char* name = error_kind_name(kind_);
    return message_.empty() ? std::string(name) : std::string(name) + ": " + message_;
}

error make_error(error_kind kind, std::string message) { return error(kind, std::move(message)); }

std::ostream& operator<<(std::ostream& os, const error& value) {
    os << error_kind_name(value.kind());
    if (!value.message().empty()) os << ": " << value.message();
    return os;
}

namespace detail {
// 误用诊断直接写 stderr，不依赖流、动态分配或异常。
[[noreturn]] void result_bad_error() noexcept {
    std::fputs("wavvix::result::error() called on a success value\n", stderr);
    std::abort();
}
[[noreturn]] void result_bad_value() noexcept {
    std::fputs("wavvix::result::value() called on an error\n", stderr);
    std::abort();
}
[[noreturn]] void result_bad_value(const wavvix::error& value) noexcept {
    std::fprintf(stderr, "wavvix::result::value() called on an error: %s", error_kind_name(value.kind()));
    if (!value.message().empty()) {
        std::fputs(": ", stderr);
        std::fwrite(value.message().data(), 1, value.message().size(), stderr);
    }
    std::fputc('\n', stderr);
    std::abort();
}
[[noreturn]] void result_bad_signed_value(long long value) noexcept {
    std::fprintf(stderr, "wavvix::result::value() called on an error: %lld\n", value);
    std::abort();
}
[[noreturn]] void result_bad_unsigned_value(unsigned long long value) noexcept {
    std::fprintf(stderr, "wavvix::result::value() called on an error: %llu\n", value);
    std::abort();
}
}  // namespace detail
}  // namespace wavvix
