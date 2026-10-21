// Win32 和 Winsock 的错误码换成种类，表里没有的算 io。说明取系统的英文版，和库自己的消息一个语言，
// Go 也这么做；系统没装英文时用系统语言。

#include "system_error.h"

#include "text_win.h"
#include "win32.h"

#include <winsock2.h>

namespace cu {
namespace detail {

error_kind system_error_kind(int code) noexcept {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_CONNECTION_REFUSED:
        case WSAEADDRNOTAVAIL:
        case WSAECONNREFUSED:
            return error_kind::not_found;
        case ERROR_ACCESS_DENIED:
        case ERROR_WRITE_PROTECT:
        case WSAEACCES:
            return error_kind::permission_denied;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
        case WSAEADDRINUSE:
            return error_kind::already_exists;
        case ERROR_TOO_MANY_OPEN_FILES:
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
        case WSAEMFILE:
        case WSAENOBUFS:
            return error_kind::resource_exhausted;
        case ERROR_TIMEOUT:
        case ERROR_SEM_TIMEOUT:
        case WSAETIMEDOUT:
            return error_kind::timeout;
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_FILENAME_EXCED_RANGE:
        case ERROR_INVALID_PARAMETER:
        case WSAEINVAL:
            return error_kind::invalid_argument;
        case ERROR_NOT_SUPPORTED:
        case WSAEAFNOSUPPORT:
        case WSAEOPNOTSUPP:
            return error_kind::unsupported;
        default:
            return error_kind::io;
    }
}

namespace {

// 系统对 code 的说明，language 是要的语言；没有这个语言的说明时返回空串。
std::string format_message(int code, WORD language) {
    LPWSTR text = 0;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 0,
        static_cast<DWORD>(code), language, reinterpret_cast<LPWSTR>(&text), 0, 0);
    std::string message = length != 0 && text ? to_utf8(text, length) : std::string();
    if (text) {
        ::LocalFree(text);
    }
    while (!message.empty() && (message[message.size() - 1] == '\n' || message[message.size() - 1] == '\r')) {
        message.erase(message.size() - 1);
    }
    return message;
}

}  // namespace

std::string system_error_text(int code) {
    std::string message = format_message(code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    if (message.empty()) {
        message = format_message(code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT));
    }
    if (message.empty()) {
        message = "Windows error " + std::to_string(code);
    }
    return message;
}

}  // namespace detail
}  // namespace cu
