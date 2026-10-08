// Windows 平台的操作系统接口。

#include <culib/lexical_cast.h>   // to_string
#include <culib/os.h>
#include <culib/strings.h>

#include <csignal>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace wavvix {
namespace os {

namespace detail {
extern volatile std::sig_atomic_t stop_flag;
}  // namespace detail

namespace {

error from_last_error(string_view what, DWORD code) {
    error_kind kind = error_kind::io;
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_ENVVAR_NOT_FOUND: kind = error_kind::not_found; break;
        case ERROR_ACCESS_DENIED: kind = error_kind::permission_denied; break;
        case ERROR_INVALID_PARAMETER:
        case ERROR_FILENAME_EXCED_RANGE: kind = error_kind::invalid_argument; break;
        default: break;
    }
    return make_error(kind, what.to_string() + " 失败：Windows 错误 " +
                                to_string(static_cast<unsigned long long>(code)));
}

// 关机 / 关窗口 / 注销 / Ctrl-C / Ctrl-Break 五种都当「该停了」。
// 返回 TRUE 表示「这个事件我处理了」，系统于是不走默认的立即终止。
BOOL WINAPI console_handler(DWORD event) {
    switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            detail::stop_flag = 1;
            return TRUE;
        default:
            return FALSE;
    }
}

result<void> check_name(string_view name) {
    if (name.empty()) {
        return make_error(error_kind::invalid_argument, "环境变量名不能为空");
    }
    if (name.find('=') != string_view::npos) {
        return make_error(error_kind::invalid_argument,
                          "环境变量名不能含 '='：" + name.to_string());
    }
    return result<void>();
}

}  // namespace

// --- 环境变量 -----------------------------------------------------------------

optional<std::string> getenv(string_view name) {
    const result<std::wstring> wide_name = utf8_to_wide(name);
    if (wide_name.is_err()) return nullopt;

    // 两次调用的返回值含义不同，混起来会把「值为空」当成「没设置」：
    //   - 不给缓冲区时返回**需要的大小，含结尾 '\0'**，返回 0 才是没设置；
    //   - 给了缓冲区时返回**存进去的字符数，不含 '\0'**，值为空时就是 0。
    const DWORD needed = ::GetEnvironmentVariableW(wide_name.value().c_str(), 0, 0);
    if (needed == 0) return nullopt;              // 不存在
    if (needed == 1) return std::string();        // 存在，值是空字符串

    std::vector<wchar_t> buffer(needed);
    const DWORD length = ::GetEnvironmentVariableW(wide_name.value().c_str(), &buffer[0], needed);
    // 走到这里 needed >= 2，所以 length 应该是 needed-1；不是就说明两次调用之间被改了
    if (length == 0 || length >= needed) return nullopt;

    const result<std::string> text = wide_to_utf8(&buffer[0], length);
    if (text.is_err()) return nullopt;
    return text.value();
}

result<void> setenv(string_view name, string_view value) {
    const result<void> valid = check_name(name);
    if (valid.is_err()) return valid;
    const result<std::wstring> wide_name = utf8_to_wide(name);
    if (wide_name.is_err()) return std::move(wide_name).error();
    const result<std::wstring> wide_value = utf8_to_wide(value);
    if (wide_value.is_err()) return std::move(wide_value).error();
    if (!::SetEnvironmentVariableW(wide_name.value().c_str(), wide_value.value().c_str())) {
        return from_last_error("设置环境变量 " + name.to_string(), ::GetLastError());
    }
    return result<void>();
}

result<void> unsetenv(string_view name) {
    const result<void> valid = check_name(name);
    if (valid.is_err()) return valid;
    const result<std::wstring> wide_name = utf8_to_wide(name);
    if (wide_name.is_err()) return std::move(wide_name).error();
    // 传 0 作为值就是删除。本来没设置时也返回成功，所以这是幂等的。
    if (!::SetEnvironmentVariableW(wide_name.value().c_str(), 0)) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_ENVVAR_NOT_FOUND) return result<void>();
        return from_last_error("清除环境变量 " + name.to_string(), code);
    }
    return result<void>();
}

// --- 进程与机器信息 -----------------------------------------------------------

result<std::string> executable_path() {
    // GetModuleFileNameW 在缓冲区不够时**也返回请求的大小**并设 ERROR_INSUFFICIENT_BUFFER，
    // 而且 Windows XP 以前不保证补 '\0'。所以按需翻倍重试，自己留一个字节。
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(0, &buffer[0],
                                                  static_cast<DWORD>(buffer.size()));
        if (length == 0) return from_last_error("取可执行文件路径", ::GetLastError());
        if (length < buffer.size()) return wide_to_utf8(&buffer[0], length);
        if (buffer.size() > (1u << 16)) {
            return make_error(error_kind::io, "可执行文件路径过长");
        }
        buffer.resize(buffer.size() * 2);
    }
}

result<std::string> hostname() {
    // ComputerNameDnsHostname 对应 POSIX 的 gethostname；MAX_COMPUTERNAME_LENGTH
    // 只够 NetBIOS 名，DNS 名可以更长，所以同样按需重试。
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    std::vector<wchar_t> buffer(size);
    for (;;) {
        if (::GetComputerNameExW(ComputerNameDnsHostname, &buffer[0], &size)) {
            return wide_to_utf8(&buffer[0], size);
        }
        const DWORD code = ::GetLastError();
        if (code != ERROR_MORE_DATA || size > 4096) return from_last_error("取主机名", code);
        buffer.resize(size);          // 失败时 size 已被改成需要的大小
    }
}

long process_id() noexcept { return static_cast<long>(::GetCurrentProcessId()); }

// --- 优雅退出 -----------------------------------------------------------------

result<void> install_stop_handler() {
    // 重复注册同一个函数会注册两次（两次都调），所以先摘掉再装，保证幂等。
    ::SetConsoleCtrlHandler(&console_handler, FALSE);
    if (!::SetConsoleCtrlHandler(&console_handler, TRUE)) {
        return from_last_error("注册控制台事件处理", ::GetLastError());
    }
    return result<void>();
}

}  // namespace os
}  // namespace wavvix
