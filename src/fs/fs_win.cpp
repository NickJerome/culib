// fs 的 Windows 实现。
// 一律走 W 系列 API：路径是 UTF-8，先转 UTF-16 再调。
// '\0' 检查不等系统报错：W API 拿 NUL 当串尾，转完先查宽字符里有没有 0，
// 否则含 NUL 的路径会被截成前缀，悄悄命中另一个文件（磁盘被碰到就晚了）。
// 超过 MAX_PATH - 12 的路径统一经 GetFullPathNameW 换成 \\?\ 扩展形式再进内核，
// 长路径测试（tests/fs_test.cpp）钉死了这条行为。
// 平台私有契约见 directory.h / file.h；对外的 exists / create_directory / remove /
// directory_iterator 等组合操作由可移植的 fs.cpp 在这里的原语上拼出来。
// 和 fs_posix.cpp 一样都直接做系统调用。

#include <culib/fs.h>
#include <culib/defer.h>
#include <culib/lexical_cast.h>   // to_string
#include <culib/strings.h>

#include "directory.h"
#include "file.h"

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
        case ERROR_BAD_NETPATH:
        case ERROR_DIRECTORY:      // 路径段里拿文件当目录
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
        case ERROR_BAD_PATHNAME:   // \\?\ 扩展形式拼错了
            return error_kind::invalid_argument;
        case ERROR_NOT_SUPPORTED:
            return error_kind::unsupported;
        default:
            return error_kind::io;
    }
}

// 「找不到」这一族的错误码：调用方要把它翻译成答案（missing / false），不是错误。
bool is_missing_code(DWORD code) {
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
           code == ERROR_INVALID_DRIVE || code == ERROR_BAD_NETPATH ||
           code == ERROR_DIRECTORY;
}

// FormatMessageW 给的是 UTF-16，转回 UTF-8 才能放进 error 的消息。
std::string describe(DWORD code) {
    LPWSTR text = 0;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        0, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&text), 0, 0);
    if (length == 0 || !text) {
        return "Windows 错误 " + to_string(static_cast<unsigned long long>(code));
    }
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
    return make_error(kind_of(code), what.to_string() + " " + path.to_string() +
                                         " 失败：" + describe(code));
}

// UTF-8 → UTF-16，顺手做两件事：
// 1. 拒绝内嵌 NUL。utf8_to_wide 按长度转，NUL 会变成串里的 0 单元，
//    带着它调用 W API 会被截断成前缀——必须在任何系统调用之前拦下。
// 2. 长路径升级。够短（建目录还要给 8.3 预留 12 个字符）直接原样交给 Win32，
//    它自己能处理相对路径、'/' 和 "."；带 \\?\ / \\.\ 前缀的本来就是扩展形式，
//    原样直通（扩展形式要求调用方自己保证已规范、全反斜杠）。
//    其余一律解析成绝对路径再挂 \\?\ 前缀，绕开 MAX_PATH。
result<std::wstring> wide_path(string_view path) {
    result<std::wstring> converted = utf8_to_wide(path);
    if (converted.is_err()) {
        return std::move(converted).error();
    }
    std::wstring wide = std::move(converted).value();
    if (wide.find(L'\0') != std::wstring::npos) {
        return make_error(error_kind::invalid_argument, "路径包含 NUL 字节");
    }
    if (wide.size() < MAX_PATH - 12 ||
        (wide.size() >= 4 && wide[0] == L'\\' && wide[1] == L'\\' &&
         (wide[2] == L'?' || wide[2] == L'.') && wide[3] == L'\\')) {
        return wide;
    }

    DWORD capacity = ::GetFullPathNameW(wide.c_str(), 0, 0, 0);
    if (capacity == 0) {
        return from_last_error("解析路径", path, ::GetLastError());
    }
    std::wstring absolute;
    bool resolved = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        absolute.resize(static_cast<std::size_t>(capacity));
        const DWORD length =
            ::GetFullPathNameW(wide.c_str(), capacity, &absolute[0], 0);
        if (length == 0) {
            return from_last_error("解析路径", path, ::GetLastError());
        }
        if (length < capacity) {
            absolute.resize(static_cast<std::size_t>(length));
            resolved = true;
            break;
        }
        capacity = length + 1;
    }
    if (!resolved) {
        return make_error(error_kind::out_of_range, "路径在解析时发生变化");
    }
    for (std::size_t i = 0; i < absolute.size(); ++i) {
        if (absolute[i] == L'/') {
            absolute[i] = L'\\';
        }
    }
    if (absolute.size() >= 2 && absolute[0] == L'\\' && absolute[1] == L'\\') {
        return std::wstring(L"\\\\?\\UNC\\") + absolute.substr(2);
    }
    return std::wstring(L"\\\\?\\") + absolute;
}

// 给一坨文件属性归类（不跟进 reparse point：调用方保证或已决定不跟）。
// Windows 的属性区分不出普通文件和管道设备，DEVICE 是老掉牙的遗留位，
// 除此之外一律按普通文件算——这正好是可移植层要的分类粒度。
detail::entry_kind kind_from_attributes(DWORD attributes) {
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return detail::entry_kind::link;   // 符号链接 / junction，绝不跟进
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        return detail::entry_kind::directory;
    }
    if (attributes & FILE_ATTRIBUTE_DEVICE) {
        return detail::entry_kind::other;
    }
    return detail::entry_kind::regular_file;
}

// 只读地打开路径拿信息句柄：FILE_READ_ATTRIBUTES 够问时间、大小和类型，
// FILE_READ_DATA 让 GetFileSizeEx 在链接目标上也有依据；
// BACKUP_SEMANTICS 不要求特权，只放开「句柄可以是目录」和跟进链接后的目标形态。
// 共享位开到最大：这是查询用的临时句柄，不该碍着别人。
result<HANDLE> open_for_query(const std::wstring& wide, string_view path) {
    const HANDLE handle = ::CreateFileW(
        wide.c_str(), FILE_READ_ATTRIBUTES | FILE_READ_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        return from_last_error("查询", path, ::GetLastError());
    }
    return handle;
}

// 句柄指向的东西是不是目录。跟进链接之后属性要从句柄问，不能拿路径问。
result<bool> handle_is_directory(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info;
    if (!::GetFileInformationByHandle(handle, &info)) {
        return make_error(error_kind::io,
                          "读取句柄信息失败：Windows 错误 " +
                              to_string(static_cast<unsigned long long>(::GetLastError())));
    }
    return (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// follow::yes 撞上 reparse point：打开句柄让内核跟完整条链接链，从句柄问形态。
// 悬空链接（目标没了）在打开这一步报「找不到」，归 missing。
result<detail::entry_kind> follow_target(const std::wstring& wide, string_view path) {
    const result<HANDLE> opened = open_for_query(wide, path);
    if (opened.is_err()) {
        const wavvix::error& failure = opened.error();
        if (failure.kind() == error_kind::not_found) {
            return detail::entry_kind::missing;
        }
        return failure;
    }
    const HANDLE handle = opened.value();
    WAVVIX_DEFER { ::CloseHandle(handle); };
    BY_HANDLE_FILE_INFORMATION info;
    if (!::GetFileInformationByHandle(handle, &info)) {
        return from_last_error("查询", path, ::GetLastError());
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return detail::entry_kind::directory;
    }
    return detail::entry_kind::regular_file;
}

// FILETIME 是 1601-01-01 起的 100 纳秒刻度。转 Unix 毫秒，向下取整，
// 1970 以前同样是负数（纪元前测试钉死了 -1 秒这个例子），不许钳成 0。
timestamp timestamp_of(const FILETIME& time) {
    ULARGE_INTEGER ticks;
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    const std::uint64_t kTicksTo1970 = 116444736000000000ull;
    std::int64_t delta;
    if (ticks.QuadPart >= kTicksTo1970) {
        delta = static_cast<std::int64_t>(ticks.QuadPart - kTicksTo1970);
    } else {
        delta = -static_cast<std::int64_t>(kTicksTo1970 - ticks.QuadPart);
    }
    const std::int64_t kTicksPerMilli = 10000;
    std::int64_t millis = delta / kTicksPerMilli;   // C++ 除法向零取整
    if (delta < 0 && millis * kTicksPerMilli != delta) {
        --millis;                                   // 改成向负无穷取整
    }
    return timestamp::from_unix_millis(millis);
}

}  // namespace

// --- detail 契约：查询与目录项分类 ---------------------------------------------

namespace detail {

result<entry_kind> query(string_view path, follow_links follow) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) {
        return std::move(wide).error();
    }
    // GetFileAttributesW 不跟 reparse point：junction 和符号链接报的是链接自己，
    // 这正是 follow::no 要的（POSIX 的 lstat）。
    DWORD attributes = ::GetFileAttributesW(wide.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = ::GetLastError();
        if (is_missing_code(code)) {
            return entry_kind::missing;   // 「那里没有东西」是答案
        }
        return from_last_error("查询", path, code);
    }
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return kind_from_attributes(attributes);
    }
    if (follow == follow_links::no) {
        return entry_kind::link;
    }
    return follow_target(wide.value(), path);
}

result<void> make_directory(string_view path) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) {
        return std::move(wide).error();
    }
    if (::CreateDirectoryW(wide.value().c_str(), 0)) {
        return result<void>();
    }
    // ERROR_ALREADY_EXISTS 会映射成 already_exists，由调用方判断类型再决定
    return from_last_error("创建目录", path, ::GetLastError());
}

result<void> remove_empty_directory(string_view path) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) {
        return std::move(wide).error();
    }
    if (!::RemoveDirectoryW(wide.value().c_str())) {
        // ERROR_DIR_NOT_EMPTY → already_exists，调用方拿它当「非空」的信号
        return from_last_error("删除目录", path, ::GetLastError());
    }
    return result<void>();
}

result<void> remove_link_or_file(string_view path) {
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) {
        return std::move(wide).error();
    }
    const DWORD attributes = ::GetFileAttributesW(wide.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return from_last_error("删除", path, ::GetLastError());
    }
    // 属性里同时挂着 DIRECTORY 的目录型 reparse point（junction、目录符号链接）
    // 只能走 RemoveDirectoryW：删的是链接自己，目标原样不动。
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!::RemoveDirectoryW(wide.value().c_str())) {
                return from_last_error("删除", path, ::GetLastError());
            }
            return result<void>();
        }
        if (!::DeleteFileW(wide.value().c_str())) {
            return from_last_error("删除", path, ::GetLastError());
        }
        return result<void>();
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        return make_error(error_kind::already_exists,
                          path.to_string() + " 是目录，请走删空目录的接口");
    }
    if (!::DeleteFileW(wide.value().c_str())) {
        return from_last_error("删除", path, ::GetLastError());
    }
    return result<void>();
}

}  // namespace detail

// --- detail 契约：打开的文件 ---------------------------------------------------

namespace detail {

error file_error(string_view operation, string_view path, unsigned long code) {
    return from_last_error(operation, path, static_cast<DWORD>(code));
}

result<std::intptr_t> open_file(string_view path, file_mode mode) {
    result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) {
        return std::move(wide).error();
    }
    // append 用 FILE_APPEND_DATA 而不是 GENERIC_WRITE：
    // 写在内核里落当时文件末尾，多个 append 句柄同时写不会互相踩（测试钉死了这条）。
    const DWORD access = mode == file_mode::read
                             ? static_cast<DWORD>(GENERIC_READ)
                             : mode == file_mode::append
                                   ? FILE_APPEND_DATA
                                   : static_cast<DWORD>(GENERIC_WRITE);
    const DWORD disposition = mode == file_mode::read
                                  ? static_cast<DWORD>(OPEN_EXISTING)
                                  : mode == file_mode::append
                                        ? static_cast<DWORD>(OPEN_ALWAYS)
                                        : static_cast<DWORD>(CREATE_ALWAYS);
    const HANDLE file = ::CreateFileW(wide.value().c_str(), access,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, 0, disposition,
                                      FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        return from_last_error("打开", path, ::GetLastError());
    }
    return reinterpret_cast<std::intptr_t>(file);
}

unsigned long close_file(std::intptr_t& handle) noexcept {
    if (handle == -1) {
        return 0;
    }
    const HANDLE file = reinterpret_cast<HANDLE>(handle);
    handle = -1;
    return ::CloseHandle(file) ? 0 : ::GetLastError();
}

std::uint64_t file_size_hint(std::intptr_t handle) noexcept {
    LARGE_INTEGER size;
    if (!::GetFileSizeEx(reinterpret_cast<HANDLE>(handle), &size) || size.QuadPart <= 0) {
        return 0;   // 管道之类问不出大小，0 = 「空或不知道」
    }
    return static_cast<std::uint64_t>(size.QuadPart);
}

result<std::size_t> read_some(std::intptr_t handle, char* data, std::size_t size,
                              string_view path) {
    const DWORD chunk = static_cast<DWORD>(size > 0x40000000u ? 0x40000000u : size);
    DWORD got = 0;
    if (::ReadFile(reinterpret_cast<HANDLE>(handle), data, chunk, &got, 0)) {
        return static_cast<std::size_t>(got);
    }
    const DWORD code = ::GetLastError();
    // 管道写端 close 之后 ReadFile 报断管，这就是 EOF
    if (code == ERROR_BROKEN_PIPE) {
        return std::size_t(0);
    }
    return from_last_error("读取", path, code);
}

result<std::size_t> write_some(std::intptr_t handle, const char* data, std::size_t size,
                               string_view path) {
    const DWORD chunk = static_cast<DWORD>(size > 0x40000000u ? 0x40000000u : size);
    DWORD moved = 0;
    if (!::WriteFile(reinterpret_cast<HANDLE>(handle), data, chunk, &moved, 0)) {
        return from_last_error("写入", path, ::GetLastError());
    }
    return static_cast<std::size_t>(moved);
}

}  // namespace detail

// --- 查询：大小与修改时间 -------------------------------------------------------

namespace {

// 链接上的 file_size / last_write_time：查询要跟随符号链接，属性查询跟不了，
// 开一个只读信息的句柄，让内核跟进链接链，从句柄问大小和时间。
// 句柄指向目录时报 unsupported（和直接对目录调 file_size 一致）。
result<std::uint64_t> size_through_link(string_view path, const std::wstring& wide) {
    const result<HANDLE> opened = open_for_query(wide, path);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    const HANDLE handle = opened.value();
    WAVVIX_DEFER { ::CloseHandle(handle); };
    const result<bool> directory = handle_is_directory(handle);
    if (directory.is_err()) {
        return std::move(directory).error();
    }
    if (directory.value()) {
        return make_error(error_kind::unsupported,
                          path.to_string() + " 是目录，没有文件大小");
    }
    LARGE_INTEGER size;
    if (!::GetFileSizeEx(handle, &size)) {
        return from_last_error("取大小", path, ::GetLastError());
    }
    return static_cast<std::uint64_t>(size.QuadPart);
}

result<wavvix::timestamp> time_through_link(string_view path, const std::wstring& wide) {
    const result<HANDLE> opened = open_for_query(wide, path);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    const HANDLE handle = opened.value();
    WAVVIX_DEFER { ::CloseHandle(handle); };
    FILETIME creation;
    FILETIME last_access;
    FILETIME last_write;
    if (!::GetFileTime(handle, &creation, &last_access, &last_write)) {
        return from_last_error("取修改时间", path, ::GetLastError());
    }
    return timestamp_of(last_write);
}

// 源和目标是不是同一个文件（含 junction / 硬链接绕过去的别名）。
// 任何一边「不存在」都算「不同」——让后面的真正操作去报权威的错。
//
// 身份只认内核给的卷号 + FileIndex：两边 FileIndex 都非 0（NTFS、SMB2+
// 之类）时，硬链接别名同值，直接比就完事。
// 任何一边 FileIndex 全 0 就是身份不可得——报错拒绝覆盖，不设退路：
// - FAT / exFAT 和部分网络重定向器不维护持久的 64 位索引，
//   GetFileInformationByHandle 报的是兼容层合成的值（chkdsk / 重新挂载后
//   不保同一）或干脆 0，「卷号相等 + 0 == 0」会把同卷上不相干的文件误判
//   成同一个。
// - 不拿最终路径兜底：零索引的重定向器同样可能支持硬链接，同一个文件的
//   两个别名可以各有自己的最终路径——「路径不同」证明不了「文件不同」，
//   没有任何可查询的东西能排除这种别名，问得出 GUID 路径也不构成身份。
// - CopyFileW 的文档不承诺拒绝同一文件（它甚至直接覆盖符号链接的目标），
//   没有可押注的行为；覆盖是「先清目标再复制」，身份不明时唯一安全的
//   做法是拒绝。
result<bool> same_file(const std::wstring& wide_from, string_view from,
                       const std::wstring& wide_to, string_view to) {
    const result<HANDLE> source = open_for_query(wide_from, from);
    if (source.is_err()) {
        if (source.error().kind() == error_kind::not_found) {
            return false;
        }
        return std::move(source).error();
    }
    const HANDLE source_handle = source.value();
    WAVVIX_DEFER { ::CloseHandle(source_handle); };
    const result<HANDLE> target = open_for_query(wide_to, to);
    if (target.is_err()) {
        if (target.error().kind() == error_kind::not_found) {
            return false;
        }
        return std::move(target).error();
    }
    const HANDLE target_handle = target.value();
    WAVVIX_DEFER { ::CloseHandle(target_handle); };

    BY_HANDLE_FILE_INFORMATION source_info;
    if (!::GetFileInformationByHandle(source_handle, &source_info)) {
        return from_last_error("查询", from, ::GetLastError());
    }
    BY_HANDLE_FILE_INFORMATION target_info;
    if (!::GetFileInformationByHandle(target_handle, &target_info)) {
        return from_last_error("查询", to, ::GetLastError());
    }
    const bool source_indexed =
        source_info.nFileIndexHigh != 0 || source_info.nFileIndexLow != 0;
    const bool target_indexed =
        target_info.nFileIndexHigh != 0 || target_info.nFileIndexLow != 0;
    if (!source_indexed || !target_indexed) {
        return make_error(error_kind::unsupported,
                          "无法确认 " + from.to_string() + " 与 " + to.to_string() +
                              " 是否同一文件：文件系统未给出可用的 FileIndex，拒绝覆盖");
    }
    return source_info.dwVolumeSerialNumber == target_info.dwVolumeSerialNumber &&
           source_info.nFileIndexHigh == target_info.nFileIndexHigh &&
           source_info.nFileIndexLow == target_info.nFileIndexLow;
}

result<std::uint64_t> file_size_native(string_view target) {
    const result<std::wstring> wide = wide_path(target);
    if (wide.is_err()) {
        return std::move(wide).error();
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!::GetFileAttributesExW(wide.value().c_str(), GetFileExInfoStandard, &data)) {
        return from_last_error("取大小", target, ::GetLastError());
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return size_through_link(target, wide.value());
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return make_error(error_kind::unsupported,
                          target.to_string() + " 是目录，没有文件大小");
    }
    ULARGE_INTEGER size;
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    return static_cast<std::uint64_t>(size.QuadPart);
}

result<wavvix::timestamp> last_write_time_native(string_view target) {
    const result<std::wstring> wide = wide_path(target);
    if (wide.is_err()) {
        return std::move(wide).error();
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!::GetFileAttributesExW(wide.value().c_str(), GetFileExInfoStandard, &data)) {
        return from_last_error("取修改时间", target, ::GetLastError());
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return time_through_link(target, wide.value());
    }
    return timestamp_of(data.ftLastWriteTime);
}

result<void> copy_file_native(string_view from, string_view to, bool overwrite) {
    const result<std::wstring> wide_from = wide_path(from);
    if (wide_from.is_err()) {
        return std::move(wide_from).error();
    }
    const result<std::wstring> wide_to = wide_path(to);
    if (wide_to.is_err()) {
        return std::move(wide_to).error();
    }
    if (overwrite) {
        // 覆盖是「先清空目标再复制」：同一文件（字符串相等，或经 junction / 硬链接
        // 绕到同一个 inode）必须在任何写盘动作之前拦下，否则内容直接被清光。
        if (from == to) {
            return make_error(error_kind::invalid_argument, "复制源和目标是同一文件");
        }
        const result<bool> identical = same_file(wide_from.value(), from, wide_to.value(), to);
        if (identical.is_err()) {
            return std::move(identical).error();
        }
        if (identical.value()) {
            return make_error(error_kind::invalid_argument, "复制源和目标是同一文件");
        }
    }
    // 不覆盖时目标已存在报 FILE_EXISTS → already_exists；源不存在报找不到。
    // 注意默认语义就是「难误用」：skip 不是这里的选项。
    if (!::CopyFileW(wide_from.value().c_str(), wide_to.value().c_str(),
                     overwrite ? FALSE : TRUE)) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
            return make_error(error_kind::already_exists, to.to_string() + " 已存在");
        }
        return from_last_error("复制到", to, code);
    }
    return result<void>();
}

result<void> rename_native(string_view from, string_view to) {
    const result<std::wstring> wide_from = wide_path(from);
    if (wide_from.is_err()) {
        return std::move(wide_from).error();
    }
    const result<std::wstring> wide_to = wide_path(to);
    if (wide_to.is_err()) {
        return std::move(wide_to).error();
    }
    // REPLACE_EXISTING 让语义向 POSIX 的 rename 看齐（目标存在则顶掉）。
    // COPY_ALLOWED：Windows 允许跨卷「改名」，退化成复制再删，比直接报错了事更贴近
    // 使用方的直觉；跨卷原子性本来就不存在。
    if (!::MoveFileExW(wide_from.value().c_str(), wide_to.value().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        return from_last_error("重命名", from, ::GetLastError());
    }
    return result<void>();
}

result<path> current_directory_native() {
    const DWORD needed = ::GetCurrentDirectoryW(0, 0);   // 含结尾 NUL
    if (needed == 0) {
        return from_last_error("取当前目录", "", ::GetLastError());
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(needed));
    const DWORD length = ::GetCurrentDirectoryW(needed, &buffer[0]);
    if (length == 0 || length >= needed) {
        // 两次调用之间目录被人换了
        return from_last_error("取当前目录", "", ::GetLastError());
    }
    const result<std::string> converted = wide_to_utf8(&buffer[0], length);
    if (converted.is_err()) {
        return std::move(converted).error();
    }
    return path(std::move(converted).value());
}

result<void> set_current_directory_native(string_view directory) {
    const result<std::wstring> wide = wide_path(directory);
    if (wide.is_err()) {
        return std::move(wide).error();
    }
    // 已知边界：SetCurrentDirectoryW 不接受 \\?\ 扩展前缀（进程工作目录是
    // DOS 命名空间的），超长的工作目录设不进去——这是 Windows 自身的限制。
    if (!::SetCurrentDirectoryW(wide.value().c_str())) {
        return from_last_error("切换到", directory, ::GetLastError());
    }
    return result<void>();
}

}  // namespace

// --- 公共入口 ---------------------------------------------------------------------

result<std::uint64_t> file_size(const path& target) {
    return file_size_native(target.string());
}

result<wavvix::timestamp> last_write_time(const path& target) {
    return last_write_time_native(target.string());
}

// --- 目录与文件操作 -------------------------------------------------------------

result<void> copy_file(const path& from, const path& to, copy_options options) {
    const bool overwrite = options == copy_options::overwrite_existing;
    return copy_file_native(from.string(), to.string(), overwrite);
}

result<void> rename(const path& from, const path& to) {
    return rename_native(from.string(), to.string());
}

// --- 当前目录与临时目录 -----------------------------------------------------------

result<path> current_path() {
    return current_directory_native();
}

result<void> current_path(const path& directory) {
    return set_current_directory_native(directory.string());
}

// 只读环境变量 / 已知位置，不碰磁盘（和 fs_posix.cpp 一致）。
result<path> temp_directory_path() {
    std::size_t capacity = MAX_PATH + 1;
    std::vector<wchar_t> buffer(capacity);
    DWORD length = 0;
    for (;;) {
        length = ::GetTempPathW(static_cast<DWORD>(capacity), &buffer[0]);
        if (length == 0) {
            return from_last_error("取临时目录", "", ::GetLastError());
        }
        if (length < capacity) {
            break;
        }
        // 缓冲区不够长：返回值是需要的长度，扩容重试
        if (capacity >= (1u << 20)) {
            return make_error(error_kind::out_of_range, "临时目录路径过长");
        }
        capacity = static_cast<std::size_t>(length) + 1;
        buffer.resize(capacity);
    }
    const result<std::string> converted = wide_to_utf8(&buffer[0], length);
    if (converted.is_err()) {
        return std::move(converted).error();
    }
    std::string text = std::move(converted).value();
    // GetTempPath 带尾反斜杠，去掉，和其他返回路径的函数一致
    while (!text.empty() && (text[text.size() - 1] == '\\' || text[text.size() - 1] == '/')) {
        text.erase(text.size() - 1);
    }
    return path(std::move(text));
}

// --- 遍历：流式目录句柄 -----------------------------------------------------------

namespace detail {
namespace {

// FindFirstFile 的句柄 + 当前项 + 名字的 UTF-8 缓冲（next 给的视图指向这里）。
// 公共侧的 directory_handle 只持一个 void*，指向这个。
// empty_open：目录确实存在但里面没东西——FindFirstFile 一个都没抓到，
// 没有原生句柄可留，读完标志直接置位。
// pending：FindFirstFileW 已经取回了第一项，还没轮到交给调用方。
struct find_state {
    HANDLE handle;
    WIN32_FIND_DATAW data;
    std::string name;
    bool exhausted;
    bool pending;
};

entry_kind hint_from_attributes(DWORD attributes) {
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return entry_kind::link;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        return entry_kind::directory;
    }
    if (attributes & FILE_ATTRIBUTE_DEVICE) {
        return entry_kind::other;
    }
    return entry_kind::regular_file;
}

}  // namespace

result<directory_handle> directory_handle::open(string_view path) {
    // 空路径绝不退化成「当前目录 + *」：FindFirstFileW("*") 会按当前工作目录
    // 解析，列出别人脚下的目录。POSIX 的 opendir("") 报 ENOENT，这里对齐成 not_found。
    if (path.empty()) {
        return make_error(error_kind::not_found, "打开目录失败：路径为空");
    }
    const result<std::wstring> wide = wide_path(path);
    if (wide.is_err()) {
        return std::move(wide).error();   // 含 NUL 在这里就 invalid_argument，磁盘碰不到
    }
    std::wstring pattern = wide.value();
    if (!pattern.empty() && pattern[pattern.size() - 1] != L'\\' &&
        pattern[pattern.size() - 1] != L'/') {
        pattern += L'\\';
    }
    pattern += L'*';

    find_state* state = new find_state;
    state->handle = ::FindFirstFileW(pattern.c_str(), &state->data);
    if (state->handle != INVALID_HANDLE_VALUE) {
        state->exhausted = false;
        state->pending = true;
        return directory_handle(state);
    }
    const DWORD code = ::GetLastError();
    // FindFirstFile 报「找不到」有两种截然不同的原因：目录是空的（或这个文件系统
    // 对空目录报 FILE_NOT_FOUND），和那个路径根本不存在 / 不是目录。光看错误码分
    // 不开，查一次目录本身的属性来断案。
    if (code == ERROR_NO_MORE_FILES || is_missing_code(code)) {
        const DWORD attributes = ::GetFileAttributesW(wide.value().c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            // 链接目标没了的 junction 也算「不存在」：POSIX 的 opendir 同样报 ENOENT。
            // NO_MORE_FILES 才是「目录在、里面是空的」的明确信号。
            (code == ERROR_NO_MORE_FILES ||
             (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)) {
            state->handle = INVALID_HANDLE_VALUE;
            state->exhausted = true;
            state->pending = false;
            return directory_handle(state);   // 空目录：零个元素，零个错误
        }
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            delete state;
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                return make_error(error_kind::not_found,
                                  path.to_string() + " 的链接目标不存在");
            }
            return make_error(error_kind::not_found,
                              path.to_string() + " 不是目录");   // 对齐 POSIX 的 ENOTDIR
        }
        const DWORD second = ::GetLastError();
        delete state;
        return from_last_error("打开目录", path, second);   // 不存在 / 没权限，报真实的
    }
    delete state;
    return from_last_error("打开目录", path, code);
}

directory_handle::directory_handle(void* handle) noexcept : handle_(handle) {}

directory_handle::~directory_handle() {
    if (handle_) {
        find_state* state = static_cast<find_state*>(handle_);
        if (state->handle != INVALID_HANDLE_VALUE) {
            ::FindClose(state->handle);
        }
        delete state;
    }
}

directory_handle::directory_handle(directory_handle&& other) noexcept
    : handle_(other.handle_) {
    other.handle_ = 0;
}

directory_handle& directory_handle::operator=(directory_handle&& other) noexcept {
    if (this != &other) {
        if (handle_) {
            find_state* state = static_cast<find_state*>(handle_);
            if (state->handle != INVALID_HANDLE_VALUE) {
                ::FindClose(state->handle);
            }
            delete state;
        }
        handle_ = other.handle_;
        other.handle_ = 0;
    }
    return *this;
}

result<bool> directory_handle::next(string_view& name, entry_kind& hint) {
    find_state* state = static_cast<find_state*>(handle_);
    if (!state || state->exhausted) {
        return false;
    }
    for (;;) {
        if (!state->pending) {
            if (!::FindNextFileW(state->handle, &state->data)) {
                const DWORD code = ::GetLastError();
                state->exhausted = true;   // 报错后状态不保证，不再让 next 进来
                if (code != ERROR_NO_MORE_FILES) {
                    return from_last_error("读取目录", "", code);
                }
                return false;
            }
        }
        state->pending = false;

        const wchar_t* found = state->data.cFileName;
        if (found[0] == L'.' &&
            (found[1] == L'\0' || (found[1] == L'.' && found[2] == L'\0'))) {
            continue;   // 跳过 "." 和 ".."
        }
        std::size_t length = 0;
        while (found[length] != L'\0') {
            ++length;
        }
        const result<std::string> converted = wide_to_utf8(found, length);
        if (converted.is_err()) {
            state->exhausted = true;
            return std::move(converted).error();
        }
        state->name = std::move(converted).value();
        name = string_view(state->name);
        hint = hint_from_attributes(state->data.dwFileAttributes);
        return true;
    }
}

}  // namespace detail

}  // namespace fs
}  // namespace wavvix
