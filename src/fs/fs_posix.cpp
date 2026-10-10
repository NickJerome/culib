// fs 的 POSIX 实现（Linux + macOS）。

#include <culib/fs.h>
#include "file.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wavvix {
namespace fs {
namespace {

// errno 到 error_kind 的映射。调用方要据此决定行为（文件不存在时用默认值、
// 权限不足时报错退出），所以这个映射不能含糊。
error_kind kind_of(int code) {
    switch (code) {
        case ENOENT:
        case ENOTDIR:
            return error_kind::not_found;
        case EACCES:
        case EPERM:
        case EROFS:
            return error_kind::permission_denied;
        case EEXIST:
        case ENOTEMPTY:
            return error_kind::already_exists;
        case ENAMETOOLONG:
        case EINVAL:
            return error_kind::invalid_argument;
        case EISDIR:
            return error_kind::unsupported;
        default:
            return error_kind::io;
    }
}

error from_errno(string_view what, string_view path, int code) {
    return make_error(kind_of(code), what.to_string() + " " + path.to_string() + " 失败：" +
                                         std::strerror(code));
}

error invalid_path() {
    return make_error(error_kind::invalid_argument, "路径包含 NUL 字节");
}

bool contains_nul(string_view path) noexcept {
    return !path.empty() && std::memchr(path.data(), 0, path.size()) != 0;
}

// string_view 不保证 '\0' 结尾，而所有系统调用都要 C 字符串。
// 短路径走栈，长路径才分配——路径在 HDD / 网络盘上本来就慢，这点优化是为了循环遍历。
class c_path {
public:
    explicit c_path(string_view path)
        : text_(0), valid_(!contains_nul(path)) {
        if (!valid_) return;
        if (path.size() < sizeof stack_) {
            if (!path.empty()) std::memcpy(stack_, path.data(), path.size());
            stack_[path.size()] = '\0';
            text_ = stack_;
        } else {
            heap_.assign(path.data(), path.size());
            text_ = heap_.c_str();
        }
    }
    const char* get() const { return text_; }
    bool valid() const noexcept { return valid_; }

    // text_ 可能指向 heap_ 内部，复制会悬垂
    c_path(const c_path&) = delete;
    c_path& operator=(const c_path&) = delete;

private:
    char stack_[256];
    std::string heap_;
    const char* text_;
    bool valid_;
};

bool stat_path(string_view path, struct stat& info) {
    const c_path native(path);
    if (!native.valid()) { errno = EINVAL; return false; }
    return ::stat(native.get(), &info) == 0;   // stat 跟随符号链接，见头文件约定
}

}  // namespace

// --- 查询 ---------------------------------------------------------------------

bool exists(string_view path) noexcept {
    struct stat info;
    return stat_path(path, info);
}

bool is_file(string_view path) noexcept {
    struct stat info;
    return stat_path(path, info) && S_ISREG(info.st_mode);
}

bool is_directory(string_view path) noexcept {
    struct stat info;
    return stat_path(path, info) && S_ISDIR(info.st_mode);
}

result<std::uint64_t> file_size(string_view path) {
    struct stat info;
    if (!stat_path(path, info))
        return contains_nul(path) ? invalid_path() : from_errno("取大小", path, errno);
    if (S_ISDIR(info.st_mode)) {
        return make_error(error_kind::unsupported, path.to_string() + " 是目录，没有文件大小");
    }
    return static_cast<std::uint64_t>(info.st_size);
}

result<timestamp> last_write_time(string_view path) {
    struct stat info;
    if (!stat_path(path, info))
        return contains_nul(path) ? invalid_path() : from_errno("取修改时间", path, errno);
    return timestamp::from_unix_seconds(static_cast<std::int64_t>(info.st_mtime));
}

namespace detail {

error file_error(string_view operation, string_view path, unsigned long code) {
    return from_errno(operation, path, static_cast<int>(code));
}

result<std::intptr_t> open_file(string_view path, file_mode mode) {
    const c_path native(path);
    if (!native.valid()) return invalid_path();
    int flags = mode == file_mode::read ? O_RDONLY : O_WRONLY | O_CREAT;
    if (mode == file_mode::truncate) flags |= O_TRUNC;
    if (mode == file_mode::append) flags |= O_APPEND;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd;
    do { fd = ::open(native.get(), flags, 0666); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return from_errno("打开", path, errno);
#ifndef O_CLOEXEC
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        const int code = errno;
        ::close(fd);
        return from_errno("设置文件句柄", path, code);
    }
#endif
    return static_cast<std::intptr_t>(fd);
}

unsigned long close_file(std::intptr_t& handle) noexcept {
    if (handle == -1) return 0;
    const int fd = static_cast<int>(handle);
    handle = -1;
    // Retrying close after EINTR can close a descriptor reused by another thread.
    return ::close(fd) == 0 ? 0 : static_cast<unsigned long>(errno);
}

std::uint64_t file_size_hint(std::intptr_t handle) noexcept {
    struct stat info;
    if (::fstat(static_cast<int>(handle), &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_size <= 0) return 0;
    return static_cast<std::uint64_t>(info.st_size);
}

result<std::size_t> read_some(std::intptr_t handle, char* data, std::size_t size,
                             string_view path) {
    const std::size_t chunk = size > 0x40000000u ? 0x40000000u : size;
    ssize_t got;
    do { got = ::read(static_cast<int>(handle), data, chunk); } while (got < 0 && errno == EINTR);
    if (got < 0) return from_errno("读取", path, errno);
    return static_cast<std::size_t>(got);
}

result<std::size_t> write_some(std::intptr_t handle, const char* data, std::size_t size,
                              string_view path) {
    const std::size_t chunk = size > 0x40000000u ? 0x40000000u : size;
    ssize_t moved;
    do { moved = ::write(static_cast<int>(handle), data, chunk); } while (moved < 0 && errno == EINTR);
    if (moved < 0) return from_errno("写入", path, errno);
    return static_cast<std::size_t>(moved);
}

}  // namespace detail

// --- 目录与文件操作 -----------------------------------------------------------

namespace detail {

result<void> make_directory(string_view path) {
    const c_path native(path);
    if (!native.valid()) return invalid_path();
    if (::mkdir(native.get(), 0777) == 0) return result<void>();
    return from_errno("创建目录", path, errno);   // EEXIST 会映射成 already_exists
}

bool is_directory_not_link(string_view path) noexcept {
    // lstat 不跟随符号链接，所以指向目录的链接在这里是 false，递归删除不会跟进去
    const c_path native(path);
    if (!native.valid()) return false;
    struct stat info;
    return ::lstat(native.get(), &info) == 0 && S_ISDIR(info.st_mode);
}

result<void> remove_empty_directory(string_view path) {
    const c_path native(path);
    if (!native.valid()) return invalid_path();
    if (::rmdir(native.get()) != 0) return from_errno("删除目录", path, errno);
    return result<void>();
}

}  // namespace detail

result<void> remove_file(string_view path) {
    const c_path native(path);
    if (!native.valid()) return invalid_path();
    if (::unlink(native.get()) != 0) return from_errno("删除", path, errno);
    return result<void>();
}

result<void> copy_file(string_view from, string_view to, overwrite mode) {
    const c_path native_from(from);
    const c_path native_to(to);
    if (!native_from.valid() || !native_to.valid()) return invalid_path();
    std::FILE* source = std::fopen(native_from.get(), "rb");
    if (!source) return from_errno("打开", from, errno);
    struct stat source_info;
    if (::fstat(::fileno(source), &source_info) != 0) {
        const int code = errno;
        std::fclose(source);
        return from_errno("检查复制源", from, code);
    }

    const int flags = O_WRONLY | O_CREAT | (mode == overwrite::no ? O_EXCL : 0);
    const int fd = ::open(native_to.get(), flags, 0666);
    if (fd < 0) {
        const int code = errno;
        std::fclose(source);
        return from_errno("创建", to, code);
    }
    if (mode == overwrite::yes) {
        struct stat target_info;
        if (::fstat(fd, &target_info) != 0) {
            const int code = errno;
            ::close(fd);
            std::fclose(source);
            return from_errno("检查复制文件", to, code);
        }
        if (source_info.st_dev == target_info.st_dev &&
            source_info.st_ino == target_info.st_ino) {
            ::close(fd);
            std::fclose(source);
            return make_error(error_kind::invalid_argument, "复制源和目标是同一文件");
        }
        if (::ftruncate(fd, 0) != 0) {
            const int code = errno;
            ::close(fd);
            std::fclose(source);
            return from_errno("截断", to, code);
        }
    }
    std::FILE* target = ::fdopen(fd, "wb");
    if (!target) {
        const int code = errno;
        ::close(fd);
        std::fclose(source);
        return from_errno("创建", to, code);
    }

    char chunk[65536];
    for (;;) {
        const std::size_t got = std::fread(chunk, 1, sizeof chunk, source);
        if (got > 0 && std::fwrite(chunk, 1, got, target) != got) {
            const int code = errno;
            std::fclose(source);
            std::fclose(target);
            return from_errno("写入", to, code ? code : EIO);
        }
        if (got < sizeof chunk) break;
    }
    const bool read_failed = std::ferror(source) != 0;
    std::fclose(source);
    if (read_failed) {
        std::fclose(target);
        return from_errno("读取", from, EIO);
    }
    if (std::fflush(target) != 0) {
        const int code = errno;
        std::fclose(target);
        return from_errno("写入", to, code ? code : EIO);
    }
    if (::fchmod(fd, source_info.st_mode & 07777) != 0) {
        const int code = errno;
        std::fclose(target);
        return from_errno("设置权限", to, code);
    }
    if (std::fclose(target) != 0) return from_errno("关闭", to, errno);
    return result<void>();
}

result<void> rename(string_view from, string_view to) {
    const c_path native_from(from);
    const c_path native_to(to);
    if (!native_from.valid() || !native_to.valid()) return invalid_path();
    if (::rename(native_from.get(), native_to.get()) != 0) {
        return from_errno("重命名", from, errno);
    }
    return result<void>();
}

result<std::string> temp_directory() {
    static const char* const names[] = {"TMPDIR", "TMP", "TEMP"};
    for (std::size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        const char* value = std::getenv(names[i]);
        if (value && value[0] != '\0') return std::string(value);
    }
    return std::string("/tmp");
}

result<std::string> current_directory() {
    // PATH_MAX 不可靠（有些文件系统能超过），所以按需翻倍重试。
    std::vector<char> buffer(512);
    for (;;) {
        if (::getcwd(&buffer[0], buffer.size())) return std::string(&buffer[0]);
        if (errno != ERANGE) return from_errno("取当前目录", "", errno);
        if (buffer.size() > (1u << 20)) return from_errno("取当前目录", "", ENAMETOOLONG);
        buffer.resize(buffer.size() * 2);
    }
}

result<void> set_current_directory(string_view path) {
    const c_path native(path);
    if (!native.valid()) return invalid_path();
    if (::chdir(native.get()) != 0) return from_errno("切换到", path, errno);
    return result<void>();
}

// --- 遍历 ---------------------------------------------------------------------

namespace detail {

result<directory_handle> directory_handle::open(string_view path) {
    const c_path native(path);
    if (!native.valid()) return invalid_path();
    DIR* dir = ::opendir(native.get());
    if (!dir) return from_errno("打开目录", path, errno);
    return directory_handle(dir);
}

directory_handle::~directory_handle() {
    if (handle_) ::closedir(static_cast<DIR*>(handle_));
}

directory_handle& directory_handle::operator=(directory_handle&& other) noexcept {
    if (this != &other) {
        if (handle_) ::closedir(static_cast<DIR*>(handle_));
        handle_ = other.handle_;
        other.handle_ = 0;
    }
    return *this;
}

result<bool> directory_handle::next(string_view& name, bool& is_directory) {
    DIR* dir = static_cast<DIR*>(handle_);
    if (!dir) return false;
    for (;;) {
        errno = 0;
        const struct dirent* item = ::readdir(dir);
        if (!item) {
            if (errno != 0) return from_errno("读取目录", "", errno);
            return false;
        }
        if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0) continue;
        name = string_view(item->d_name);
#if defined(DT_DIR)
        if (item->d_type != DT_UNKNOWN) {
            is_directory = item->d_type == DT_DIR;
            return true;
        }
#endif
        // d_type 不可用（部分文件系统返回 DT_UNKNOWN）时才退回 stat。这一次额外的系统
        // 调用在 HDD / 网络盘上很贵，所以只在非得如此时才做。
        // 用 fstatat 配 dirfd 而不是拼完整路径：handle 不存目录路径，而且这样不受
        // 遍历期间当前目录被改动的影响。
        struct stat info;
        if (::fstatat(::dirfd(dir), item->d_name, &info, 0) != 0)
            return from_errno("查询目录项", item->d_name, errno);
        is_directory = S_ISDIR(info.st_mode);
        return true;
    }
}

}  // namespace detail

}  // namespace fs
}  // namespace wavvix
