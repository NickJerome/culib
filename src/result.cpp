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

std::ostream& operator<<(std::ostream& os, const error& value) { return os << value.to_string(); }

namespace detail {
namespace {
// 误用诊断直接写 stderr，不依赖流、动态分配或异常。
[[noreturn]] void die(const char* text) noexcept {
    std::fputs(text, stderr);
    std::abort();
}
}  // namespace

[[noreturn]] void result_bad_error() noexcept {
    die("wavvix::result::error() called on a success value\n");
}
[[noreturn]] void result_bad_value() noexcept {
    die("wavvix::result::value() called on an error\n");
}
[[noreturn]] void result_bad_value(const wavvix::error& value) noexcept {
    // 一次 fprintf 写完；说明为空时连中间的 ": " 一起省掉，和 to_string() 的格式一致。
    std::fprintf(stderr, "wavvix::result::value() called on an error: %s%s%.*s\n",
                 error_kind_name(value.kind()), value.message().empty() ? "" : ": ",
                 static_cast<int>(value.message().size()), value.message().data());
    std::abort();
}
}  // namespace detail
}  // namespace wavvix
