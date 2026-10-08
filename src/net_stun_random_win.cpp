#include "net_stun_random.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <climits>
#include <string>

namespace wavvix {
namespace net {
namespace detail {

result<void> stun_random_bytes(unsigned char* out, std::size_t size) {
    if (size > ULONG_MAX)
        return make_error(error_kind::out_of_range, "随机字节请求过大");
    const NTSTATUS status = ::BCryptGenRandom(0, out, static_cast<ULONG>(size),
                                              BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status))
        return make_error(error_kind::io, "BCryptGenRandom 失败：" +
                                              std::to_string(static_cast<long>(status)));
    return result<void>();
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
