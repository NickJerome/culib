#pragma once

// 系统错误码换成 cu::error：种类按一张固定的表，消息是「操作 对象: 系统原话」。
// 实现在 system_error_posix.cpp / system_error_win.cpp。

#include <cu/result.h>

#include <string>

namespace cu {
namespace detail {

// POSIX 上 code 是 errno；Windows 上是 GetLastError() 或 WSAGetLastError() 的值。
error_kind system_error_kind(int code) noexcept;
// 系统对这个错误码的说明，去掉结尾的换行。
std::string system_error_text(int code);

inline error system_error(const std::string& what, int code) {
    return make_error(system_error_kind(code), what + ": " + system_error_text(code));
}

}  // namespace detail
}  // namespace cu
