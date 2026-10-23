#pragma once

// 跨层传播错误时补上下文：种类不变，消息前面加上「what: 」。底层只知道 "Connection reset by peer"，中间那层才知道
// 这是 "read 127.0.0.1:51234"。

#include <cu/result.h>

#include <string>

namespace cu {
namespace detail {

inline error in_context(const std::string& what, const error& failure) {
    return make_error(failure.kind(), what + ": " + failure.message());
}

}  // namespace detail
}  // namespace cu
