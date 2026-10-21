// errno 换成种类。调用方据此决定行为（文件不存在时用默认值、端口被占时换一个），所以这张表不能含糊；
// 表里没有的算 io。

#include "system_error.h"

#include <cerrno>
#include <cstring>

namespace cu {
namespace detail {

error_kind system_error_kind(int code) noexcept {
    switch (code) {
        case ENOENT:
        case ENOTDIR:
        case EADDRNOTAVAIL:
        case ECONNREFUSED:
            return error_kind::not_found;
        case EACCES:
        case EPERM:
        case EROFS:
            return error_kind::permission_denied;
        case EEXIST:
        case EADDRINUSE:
            return error_kind::already_exists;
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM:
        case ENOSPC:
            return error_kind::resource_exhausted;
        case ETIMEDOUT:
            return error_kind::timeout;
        case EINVAL:
        case ENAMETOOLONG:
            return error_kind::invalid_argument;
        case EISDIR:
        case EAFNOSUPPORT:
            return error_kind::unsupported;
        default:
            return error_kind::io;
    }
}

std::string system_error_text(int code) {
    return std::strerror(code);
}

}  // namespace detail
}  // namespace cu
