// fs 的 POSIX 实现（Linux + macOS）。

#include <culib/fs.h>

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

result<void> write_with_mode(string_view path, string_view content, const char* mode) {
    const c_path native(path);
    if (!native.valid()) return invalid_path();
    std::FILE* file = std::fopen(native.get(), mode);
    if (!file) return from_errno("打开", path, errno);
    if (!content.empty()) {
        const std::size_t written = std::fwrite(content.data(), 1, content.size(), file);
        if (written != content.size()) {
            const int code = errno;
            std::fclose(file);
            return from_errno("写入", path, code ? code : EIO);
        }
    }
    if (std::fclose(file) != 0) return from_errno("关闭", path, errno);
    return result<void>();
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

// --- 整文件读写 ---------------------------------------------------------------

result<void> read_file(string_view path, std::string& out) {
    const c_path native(path);
    if (!native.valid()) return invalid_path();
    std::FILE* file = std::fopen(native.get(), "rb");
    if (!file) return from_errno("打开", path, errno);

    // 先问大小好一次读完。拿不到大小（管道、/proc 这类）就退回分块读。
    struct stat info;
    out.clear();
    if (::fstat(::fileno(file), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0) {
        out.resize(static_cast<std::size_t>(info.st_size));
        const std::size_t got = std::fread(&out[0], 1, out.size(), file);
        out.resize(got);          // 读的过程中文件变短了也不算错，只是短一些
        if (got != static_cast<std::size_t>(info.st_size) && std::ferror(file)) {
            const int code = errno;
            std::fclose(file);
            return from_errno("读取", path, code ? code : EIO);
        }
    } else {
        char chunk[8192];
        for (;;) {
            const std::size_t got = std::fread(chunk, 1, sizeof chunk, file);
            out.append(chunk, got);
            if (got < sizeof chunk) {
                if (std::ferror(file)) {
                    const int code = errno;
                    std::fclose(file);
                    return from_errno("读取", path, code ? code : EIO);
                }
                break;
            }
        }
    }
    std::fclose(file);
    return result<void>();
}

result<void> write_file(string_view path, string_view content) {
    return write_with_mode(path, content, "wb");
}

result<void> append_file(string_view path, string_view content) {
    return write_with_mode(path, content, "ab");
}

// --- 一直开着的追加写句柄 -----------------------------------------------------
//
// 用裸 open / write，避免 stdio 的用户态缓冲滞留已写出的日志。

output_file::output_file() noexcept : handle_(-1), written_(0) {}

output_file::~output_file() { close(); }

bool output_file::is_open() const noexcept { return handle_ >= 0; }

void output_file::close() noexcept {
    if (handle_ >= 0) ::close(static_cast<int>(handle_));
    handle_ = -1;
    written_ = 0;
}

result<void> output_file::open(string_view path) {
    close();
    const c_path native(path);
    if (!native.valid()) return invalid_path();
    int flags = O_WRONLY | O_CREAT | O_APPEND;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;   // 别让子进程继承一个它用不上的日志句柄
#endif
    const int fd = ::open(native.get(), flags, 0644);
    if (fd < 0) return from_errno("打开", path, errno);
#ifndef O_CLOEXEC
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        const int code = errno;
        ::close(fd);
        return from_errno("设置文件句柄", path, code);
    }
#endif

    // 记下已有的长度，这样 size() 不用每次去 stat。
    struct stat info;
    handle_ = fd;
    written_ = (::fstat(fd, &info) == 0 && info.st_size > 0)
                   ? static_cast<std::uint64_t>(info.st_size)
                   : 0;
    return result<void>();
}

result<void> output_file::write(string_view content) {
    if (handle_ < 0) return make_error(error_kind::invalid_argument, "写入前要先 open");
    const char* at = content.data();
    std::size_t left = content.size();
    while (left) {
        const ssize_t moved = ::write(static_cast<int>(handle_), at, left);
        if (moved < 0) {
            if (errno == EINTR) continue;   // 信号打断，接着写
            return from_errno("写入", "", errno);
        }
        at += moved;
        left -= static_cast<std::size_t>(moved);
        written_ += static_cast<std::uint64_t>(moved);
    }
    return result<void>();
}

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

    std::FILE* target = 0;
    if (mode == overwrite::yes) {
        target = std::fopen(native_to.get(), "wb");
    } else {
        const int fd = ::open(native_to.get(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) {
            target = ::fdopen(fd, "wb");
            if (!target) {
                const int code = errno;
                ::close(fd);
                errno = code;
            }
        }
    }
    if (!target) {
        const int code = errno;
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
    if (std::fclose(target) != 0) return from_errno("关闭", to, errno);
    if (read_failed) return from_errno("读取", from, EIO);

    // 权限一起带过去，否则复制可执行文件会丢掉执行位
    struct stat info;
    if (stat_path(from, info)) ::chmod(native_to.get(), info.st_mode & 07777);
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
