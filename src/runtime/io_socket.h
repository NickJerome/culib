#pragma once

// socket_handle（include/cu/detail/socket_handle.h）的内部部分：accept 的结果和到点的错误。net 和事件引擎的实现用，
// 会带进系统的套接字头文件。

#include <cu/detail/socket_handle.h>

#include "platform/socket_address.h"

namespace cu {
namespace detail {

struct socket_handle::accepted {
    socket_handle socket;
    socket_address peer;
};

// 到点还没完成。是哪个操作、哪个地址由调用方补，例如 "read 127.0.0.1:51234: i/o timeout"，和 Go 一样。
inline error timeout_error() {
    return make_error(error_kind::timeout, "i/o timeout");
}

}  // namespace detail
}  // namespace cu
