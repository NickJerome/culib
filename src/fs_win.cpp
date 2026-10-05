// fs 的 Windows 实现。
//
// **本文件从未被任何机器编译过**（本地只有 Linux / GCC，CI 还没真正运行过）。
// 这是继 src/test_win.cpp、strings 的 utf8_to_wide 之后第三处这样的代码。
// 一律走 W 系列 API：路径是 UTF-8，先转 UTF-16 再调。

#include <culib/fs.h>
#include <culib/lexical_cast.h>   // to_string
#include <culib/strings.h>

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX        // 否则 windows.h 的 min / max 宏会污染使用方
#endif
#include <windows.h>

namespace wavvix {
namespace fs {
namespace {

error_kind kind_of(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
            return error_kind::not_found;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_WRITE_PROTECT:
            return error_kind::permission_denied;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
        case ERROR_DIR_NOT_EMPTY:
            return error_kind::already_exists;
        case ERROR_FILENAME_EXCED_RANGE:
        case ERROR_INVALID_NAME:
            return error_kind::invalid_argument;
        case ERROR_NOT_SUPPORTED:
            return error_kind::unsupported;
        default:
            return error_kind::io;
    }
}

// FormatMessageW 给的是 UTF-16，转回 UTF-8 才能放进 error 的消息。
std::string describe(DWORD code) {
    LPWSTR text = 0;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        0, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&text), 0, 0);
    if (length == 0 || !text) return "Windows 错误 " + to_string(static_cast<unsigned long long>(code));
    std::string message;
    const result<std::string> converted = wide_to_utf8(text, length);
    if (converted.is_ok()) {
        message = converted.value();
    } else {
        message = "Windows 错误 " + to_string(static_cast<unsigned long long>(code));
    }
    ::LocalFree(text);
    // FormatMessage 的消息自带换行，去掉
    while (!message.empty() && (message[message.size() - 1] == '\n' ||
                                message[message.size() - 1] == '\r')) {
        message.erase(message.size() - 1);
    }
    return message;
}

error from_last_error(string_view what, string_view path, DWORD code) {
    return make_error(kind_of(code),
                      what.to_string() + " " + path.to_string() + " 失败：" + describe(code));
}

// 路径转 UTF-16。转换失败说明传进来的不是合法 UTF-8，那本身就是错误。
result<std::wstring> wide_path(string_view path) { return utf8_to_wide(path); }

result<DWORD> attributes_of(string_view path) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) return std::move(wide).error();
    const DWORD attributes = ::GetFileAttributesW(wide.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return from_last_error("查询", path, ::GetLastError());
    }
    return attributes;
}

// FILETIME 是 1601-01-01 起的 100 纳秒刻度，转成 Unix 秒。
std::int64_t to_unix_seconds(const FILETIME& time) {
    ULARGE_INTEGER ticks;
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    const std::uint64_t kTicksTo1970 = 116444736000000000ull;
    if (ticks.QuadPart < kTicksTo1970) return 0;
    return static_cast<std::int64_t>((ticks.QuadPart - kTicksTo1970) / 10000000ull);
}

result<void> write_with_disposition(string_view path, string_view content, bool append) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) return std::move(wide).error();
    // 同上：共享模式给 0 的话，写的过程中别人连读都不行。POSIX 没有这种限制。
    const HANDLE file = ::CreateFileW(wide.value().c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                                      append ? OPEN_ALWAYS : CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return from_last_error("打开", path, ::GetLastError());
    if (append) ::SetFilePointer(file, 0, 0, FILE_END);
    std::size_t offset = 0;
    while (offset < content.size()) {
        // WriteFile 一次最多 DWORD 能表示的字节数，大文件要分几次
        const DWORD chunk = static_cast<DWORD>(
            (content.size() - offset) > 0x40000000u ? 0x40000000u : (content.size() - offset));
        DWORD written = 0;
        if (!::WriteFile(file, content.data() + offset, chunk, &written, 0)) {
            const DWORD code = ::GetLastError();
            ::CloseHandle(file);
            return from_last_error("写入", path, code);
        }
        offset += written;
    }
    ::CloseHandle(file);
    return result<void>();
}

}  // namespace

// --- 查询 ---------------------------------------------------------------------

bool exists(string_view path) noexcept {
    const result<DWORD> attributes = attributes_of(path);
    return attributes.is_ok();
}

bool is_file(string_view path) noexcept {
    const result<DWORD> attributes = attributes_of(path);
    return attributes.is_ok() && (attributes.value() & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool is_directory(string_view path) noexcept {
    const result<DWORD> attributes = attributes_of(path);
    return attributes.is_ok() && (attributes.value() & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

result<std::uint64_t> file_size(string_view path) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) return std::move(wide).error();
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!::GetFileAttributesExW(wide.value().c_str(), GetFileExInfoStandard, &data)) {
        return from_last_error("取大小", path, ::GetLastError());
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return make_error(error_kind::unsupported, path.to_string() + " 是目录，没有文件大小");
    }
    ULARGE_INTEGER size;
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    return static_cast<std::uint64_t>(size.QuadPart);
}

result<timestamp> last_write_time(string_view path) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) return std::move(wide).error();
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!::GetFileAttributesExW(wide.value().c_str(), GetFileExInfoStandard, &data)) {
        return from_last_error("取修改时间", path, ::GetLastError());
    }
    return timestamp::from_unix_seconds(to_unix_seconds(data.ftLastWriteTime));
}

// --- 整文件读写 ---------------------------------------------------------------

result<void> read_file(string_view path, std::string& out) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) return std::move(wide).error();
    // **共享模式要带 FILE_SHARE_WRITE**，否则读不了「别人正开着写」的文件——
    // 日志就是那种文件。POSIX 上读一个正在被写的文件永远可以，而 README 承诺
    // 「同一套 API 在三个平台上行为一致」，所以这里要跟上。
    //
    // 这是 log 那一期撞出来的：日志 sink 握着写句柄时 read_file 返回
    // ERROR_SHARING_VIOLATION，四条测试在 wine 下红了，而 Linux 上全绿。
    const HANDLE file = ::CreateFileW(wide.value().c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return from_last_error("打开", path, ::GetLastError());

    out.clear();
    LARGE_INTEGER size;
    if (::GetFileSizeEx(file, &size) && size.QuadPart > 0) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < out.size()) {
            const DWORD chunk = static_cast<DWORD>(
                (out.size() - offset) > 0x40000000u ? 0x40000000u : (out.size() - offset));
            DWORD got = 0;
            if (!::ReadFile(file, &out[offset], chunk, &got, 0)) {
                const DWORD code = ::GetLastError();
                ::CloseHandle(file);
                return from_last_error("读取", path, code);
            }
            if (got == 0) break;           // 读的过程中文件变短了
            offset += got;
        }
        out.resize(offset);
    } else {
        char chunk[8192];
        for (;;) {
            DWORD got = 0;
            if (!::ReadFile(file, chunk, sizeof chunk, &got, 0)) {
                const DWORD code = ::GetLastError();
                ::CloseHandle(file);
                return from_last_error("读取", path, code);
            }
            if (got == 0) break;
            out.append(chunk, got);
        }
    }
    ::CloseHandle(file);
    return result<void>();
}

result<void> write_file(string_view path, string_view content) {
    return write_with_disposition(path, content, false);
}

result<void> append_file(string_view path, string_view content) {
    return write_with_disposition(path, content, true);
}

// --- 一直开着的追加写句柄 -----------------------------------------------------
//
// FILE_APPEND_DATA 让每次 WriteFile 都落到文件末尾，不需要自己 seek——多个进程
// 写同一个日志文件时也不会互相盖。不开用户态缓冲，理由见 fs_posix.cpp 那边。

output_file::output_file() noexcept : handle_(-1), written_(0) {}

output_file::~output_file() { close(); }

bool output_file::is_open() const noexcept {
    return reinterpret_cast<HANDLE>(handle_) != INVALID_HANDLE_VALUE;
}

void output_file::close() noexcept {
    const HANDLE file = reinterpret_cast<HANDLE>(handle_);
    if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);
    handle_ = -1;
    written_ = 0;
}

result<void> output_file::open(string_view path) {
    close();
    const result<std::wstring> wide = wide_path(path);
    if (!wide.is_ok()) return result<void>(wide.error());

    // FILE_SHARE_READ | FILE_SHARE_WRITE：让别人能同时 tail 这个日志。
    const HANDLE file = ::CreateFileW(wide.value().c_str(), FILE_APPEND_DATA,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return from_last_error("打开", path, ::GetLastError());

    LARGE_INTEGER had;
    had.QuadPart = 0;
    handle_ = reinterpret_cast<std::intptr_t>(file);
    written_ = ::GetFileSizeEx(file, &had) ? static_cast<std::uint64_t>(had.QuadPart) : 0;
    return result<void>();
}

result<void> output_file::write(string_view content) {
    const HANDLE file = reinterpret_cast<HANDLE>(handle_);
    if (file == INVALID_HANDLE_VALUE) {
        return make_error(error_kind::invalid_argument, "写入前要先 open");
    }
    const char* at = content.data();
    std::size_t left = content.size();
    while (left) {
        const DWORD chunk = (left > 0x40000000u) ? 0x40000000u : static_cast<DWORD>(left);
        DWORD moved = 0;
        if (!::WriteFile(file, at, chunk, &moved, 0)) {
            return from_last_error("写入", "", ::GetLastError());
        }
        at += moved;
        left -= moved;
        written_ += moved;
    }
    return result<void>();
}

// --- 目录与文件操作 -----------------------------------------------------------

namespace detail {

result<void> make_directory(string_view path) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) return std::move(wide).error();
    if (::CreateDirectoryW(wide.value().c_str(), 0)) return result<void>();
    // ERROR_ALREADY_EXISTS 会映射成 already_exists
    return from_last_error("创建目录", path, ::GetLastError());
}

bool is_directory_not_link(string_view path) noexcept {
    const result<DWORD> attributes = attributes_of(path);
    if (attributes.is_err()) return false;
    // REPARSE_POINT 覆盖符号链接和 junction，两者都不跟进去
    return (attributes.value() & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes.value() & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

result<void> remove_empty_directory(string_view path) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) return std::move(wide).error();
    if (!::RemoveDirectoryW(wide.value().c_str())) {
        return from_last_error("删除目录", path, ::GetLastError());
    }
    return result<void>();
}

}  // namespace detail

result<void> remove_file(string_view path) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) return std::move(wide).error();
    if (!::DeleteFileW(wide.value().c_str())) {
        return from_last_error("删除", path, ::GetLastError());
    }
    return result<void>();
}

result<void> copy_file(string_view from, string_view to, overwrite mode) {
    const result<std::wstring> wide_from = wide_path(from);
    if (wide_from.is_err()) return std::move(wide_from).error();
    const result<std::wstring> wide_to = wide_path(to);
    if (wide_to.is_err()) return std::move(wide_to).error();
    const BOOL fail_if_exists = mode == overwrite::no ? TRUE : FALSE;
    if (!::CopyFileW(wide_from.value().c_str(), wide_to.value().c_str(), fail_if_exists)) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_EXISTS) {
            return make_error(error_kind::already_exists, to.to_string() + " 已存在");
        }
        return from_last_error("复制到", to, code);
    }
    return result<void>();
}

result<void> rename(string_view from, string_view to) {
    const result<std::wstring> wide_from = wide_path(from);
    if (wide_from.is_err()) return std::move(wide_from).error();
    const result<std::wstring> wide_to = wide_path(to);
    if (wide_to.is_err()) return std::move(wide_to).error();
    // MOVEFILE_REPLACE_EXISTING 让语义和 POSIX 的 rename 一致（覆盖目标）
    if (!::MoveFileExW(wide_from.value().c_str(), wide_to.value().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        return from_last_error("重命名", from, ::GetLastError());
    }
    return result<void>();
}

result<std::string> temp_directory() {
    wchar_t buffer[MAX_PATH + 1];
    const DWORD length = ::GetTempPathW(MAX_PATH + 1, buffer);
    if (length == 0) return from_last_error("取临时目录", "", ::GetLastError());
    result<std::string> converted = wide_to_utf8(buffer, length);
    if (converted.is_err()) return converted;
    std::string path = std::move(converted).value();
    // GetTempPath 带尾部反斜杠，去掉以便和其他返回路径的函数一致
    while (!path.empty() && (path[path.size() - 1] == '\\' || path[path.size() - 1] == '/')) {
        path.erase(path.size() - 1);
    }
    return path;
}

result<std::string> current_directory() {
    const DWORD needed = ::GetCurrentDirectoryW(0, 0);
    if (needed == 0) return from_last_error("取当前目录", "", ::GetLastError());
    std::vector<wchar_t> buffer(needed);
    const DWORD length = ::GetCurrentDirectoryW(needed, &buffer[0]);
    if (length == 0 || length >= needed) {
        return from_last_error("取当前目录", "", ::GetLastError());
    }
    return wide_to_utf8(&buffer[0], length);
}

result<void> set_current_directory(string_view path) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) return std::move(wide).error();
    if (!::SetCurrentDirectoryW(wide.value().c_str())) {
        return from_last_error("切换到", path, ::GetLastError());
    }
    return result<void>();
}

// --- 遍历 ---------------------------------------------------------------------

namespace detail {
namespace {

// FindFirstFile 的句柄加上当前项，再加上名字的 UTF-8 缓冲区。
// 公共头文件里 directory_handle 只有一个 void*，指向这个。
struct find_state {
    HANDLE handle;
    WIN32_FIND_DATAW data;
    std::string name;      // next() 返回的视图指向这里
    bool exhausted;
    bool pending;          // FindFirstFile 已经取回了第一项，还没交给调用方
};

}  // namespace

result<directory_handle> directory_handle::open(string_view path) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) return std::move(wide).error();
    std::wstring pattern = wide.value();
    if (!pattern.empty() && pattern[pattern.size() - 1] != L'\\' &&
        pattern[pattern.size() - 1] != L'/') {
        pattern += L'\\';
    }
    pattern += L'*';

    find_state* state = new find_state;
    state->exhausted = false;
    state->pending = true;
    state->handle = ::FindFirstFileW(pattern.c_str(), &state->data);
    if (state->handle == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        delete state;
        return from_last_error("打开目录", path, code);
    }
    return directory_handle(state);
}

directory_handle::~directory_handle() {
    if (handle_) {
        find_state* state = static_cast<find_state*>(handle_);
        if (state->handle != INVALID_HANDLE_VALUE) ::FindClose(state->handle);
        delete state;
    }
}

directory_handle& directory_handle::operator=(directory_handle&& other) noexcept {
    if (this != &other) {
        if (handle_) {
            find_state* state = static_cast<find_state*>(handle_);
            if (state->handle != INVALID_HANDLE_VALUE) ::FindClose(state->handle);
            delete state;
        }
        handle_ = other.handle_;
        other.handle_ = 0;
    }
    return *this;
}

bool directory_handle::next(string_view& name, bool& is_directory) {
    find_state* state = static_cast<find_state*>(handle_);
    if (!state || state->exhausted) return false;
    for (;;) {
        if (!state->pending) {
            if (!::FindNextFileW(state->handle, &state->data)) {
                state->exhausted = true;
                return false;
            }
        }
        state->pending = false;

        const wchar_t* found = state->data.cFileName;
        if (found[0] == L'.' && (found[1] == L'\0' || (found[1] == L'.' && found[2] == L'\0'))) {
            continue;    // 跳过 "." 和 ".."
        }
        std::size_t length = 0;
        while (found[length] != L'\0') ++length;
        const result<std::string> converted = wide_to_utf8(found, length);
        if (converted.is_err()) continue;   // 文件名不是合法 UTF-16，跳过而不是让整次遍历失败
        state->name = converted.value();
        name = string_view(state->name);
        is_directory = (state->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return true;
    }
}

}  // namespace detail

}  // namespace fs
}  // namespace wavvix
