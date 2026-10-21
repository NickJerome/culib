// 读文件的 Windows 实现。

#include <cu/fs.h>

#include "platform/system_error.h"
#include "platform/text_win.h"
#include "platform/win32.h"

#include <string>
#include <utility>

namespace cu {
namespace fs {

namespace {

// 一次最多读这么多，和 Go 一样取 1 GiB（ReadFile 的长度是 32 位的）。
const DWORD max_read = DWORD(1) << 30;

// 离开作用域时关掉句柄。
class file_guard {
public:
    explicit file_guard(HANDLE file) noexcept : file_(file) {}
    file_guard(const file_guard&) = delete;
    file_guard& operator=(const file_guard&) = delete;

    ~file_guard() {
        ::CloseHandle(file_);
    }

private:
    HANDLE file_;
};

bool is_directory(const std::wstring& path) noexcept {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

}  // namespace

result<std::string> read_file(string_view path) {
    const std::string name = path.to_string();
    if (name.find('\0') != std::string::npos) {
        return make_error(error_kind::invalid_argument, "open " + name + ": path contains a NUL byte");
    }
    std::wstring wide;
    if (!cu::detail::to_wide(path, wide)) {
        return make_error(error_kind::invalid_argument, "open " + name + ": path is not valid UTF-8");
    }
    const HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        // 打开目录报的是拒绝访问；和 Linux、macOS 一样报成目录
        if (code == ERROR_ACCESS_DENIED && is_directory(wide)) {
            return make_error(error_kind::unsupported, "open " + name + ": is a directory");
        }
        return cu::detail::system_error("open " + name, static_cast<int>(code));
    }
    const file_guard guard(file);

    std::size_t expected = 0;
    LARGE_INTEGER size;
    if (::GetFileType(file) == FILE_TYPE_DISK && ::GetFileSizeEx(file, &size)) {
        expected = static_cast<std::size_t>(size.QuadPart);
    }

    // 比文件大小多一个字节：读满文件以后再读一次拿到 0，才知道到头了，这一次不用扩容。
    std::string content(expected + 1 > 512 ? expected + 1 : 512, '\0');
    std::size_t used = 0;
    for (;;) {
        if (used == content.size()) {
            content.resize(content.size() * 2);
        }
        const std::size_t room = content.size() - used;
        DWORD got = 0;
        if (!::ReadFile(file, &content[used], room < max_read ? static_cast<DWORD>(room) : max_read, &got, 0)) {
            const DWORD code = ::GetLastError();
            return cu::detail::system_error("read " + name, static_cast<int>(code));
        }
        if (got == 0) {
            break;
        }
        used += got;
    }
    content.resize(used);
    return result<std::string>(std::move(content));
}

}  // namespace fs
}  // namespace cu
