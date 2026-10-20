// fs 的 POSIX 实现（Linux + macOS）：公共 API 里由本平台负责的那几个入口（file_size /
// last_write_time / copy_file / rename / current_path），和 src/fs/directory.h、file.h 的
// 私有原语。都直接做系统调用。

#include <culib/defer.h>
#include <culib/fs.h>

#include "directory.h"
#include "file.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
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

// 两个路径的操作（rename、copy）：消息里两边都要带上，报错时才知道动的是谁。
error from_errno_pair(string_view what, string_view from, string_view to, int code) {
    return make_error(kind_of(code), what.to_string() + " " + from.to_string() + " -> " +
                                         to.to_string() + " 失败：" + std::strerror(code));
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
        if (!valid_) {
            return;
        }

        if (path.size() < sizeof stack_) {
            if (!path.empty()) {
                std::memcpy(stack_, path.data(), path.size());
            }
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

}  // namespace

// --- 原生文件原语（src/fs/file.h）--------------------------------------------------
//
// 裸系统调用，NUL 在动手之前拦下，错误消息带路径。EINTR 一律重试到出结果，
// 唯独 close 不重试：重试可能关到别的线程刚复用的描述符。

namespace detail {

error file_error(string_view operation, string_view path, unsigned long code) {
    return from_errno(operation, path, static_cast<int>(code));
}

result<std::intptr_t> open_file(string_view path, file_mode mode) {
    const c_path native(path);
    if (!native.valid()) {
        return invalid_path();
    }

    int flags = mode == file_mode::read ? O_RDONLY : O_WRONLY | O_CREAT;
    if (mode == file_mode::truncate) {
        flags |= O_TRUNC;
    }
    if (mode == file_mode::append) {
        flags |= O_APPEND;
    }
    flags |= O_CLOEXEC;

    int fd;
    do {
        fd = ::open(native.get(), flags, 0666);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return from_errno("打开", path, errno);
    }
    return static_cast<std::intptr_t>(fd);
}

unsigned long close_file(std::intptr_t& handle) noexcept {
    if (handle == -1) {
        return 0;
    }

    const int fd = static_cast<int>(handle);
    handle = -1;
    // Retrying close after EINTR can close a descriptor reused by another thread.
    return ::close(fd) == 0 ? 0 : static_cast<unsigned long>(errno);
}

std::uint64_t file_size_hint(std::intptr_t handle) noexcept {
    struct stat info;
    if (::fstat(static_cast<int>(handle), &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_size <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(info.st_size);
}

result<std::size_t> read_some(std::intptr_t handle, char* data, std::size_t size,
                             string_view path) {
    const std::size_t chunk = size > 0x40000000u ? 0x40000000u : size;
    ssize_t got;
    do {
        got = ::read(static_cast<int>(handle), data, chunk);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        return from_errno("读取", path, errno);
    }
    return static_cast<std::size_t>(got);
}

result<std::size_t> write_some(std::intptr_t handle, const char* data, std::size_t size,
                              string_view path) {
    const std::size_t chunk = size > 0x40000000u ? 0x40000000u : size;
    ssize_t moved;
    do {
        moved = ::write(static_cast<int>(handle), data, chunk);
    } while (moved < 0 && errno == EINTR);
    if (moved < 0) {
        return from_errno("写入", path, errno);
    }
    return static_cast<std::size_t>(moved);
}

}  // namespace detail

// --- 私有后端契约（src/fs/directory.h）---------------------------------------------

namespace detail {

result<entry_kind> query(string_view path, follow_links follow) {
    const c_path native(path);
    if (!native.valid()) {
        return invalid_path();
    }

    struct stat info;
    // follow_links::no 用 lstat：链接报 link 本身，绝不跟进；
    // follow_links::yes 用 stat：跟进到目标，悬空的链接在 stat 这里就是 ENOENT，归 missing。
    const int code = follow == follow_links::yes ? ::stat(native.get(), &info)
                                                 : ::lstat(native.get(), &info);
    if (code != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return entry_kind::missing;
        }
        return from_errno("查询", path, errno);
    }

    if (S_ISREG(info.st_mode)) {
        return entry_kind::regular_file;
    }
    if (S_ISDIR(info.st_mode)) {
        return entry_kind::directory;
    }
    if (S_ISLNK(info.st_mode)) {
        return entry_kind::link;
    }
    return entry_kind::other;
}

result<void> make_directory(string_view path) {
    const c_path native(path);
    if (!native.valid()) {
        return invalid_path();
    }

    if (::mkdir(native.get(), 0777) == 0) {
        return result<void>();
    }
    return from_errno("创建目录", path, errno);   // EEXIST 会映射成 already_exists
}

result<void> remove_empty_directory(string_view path) {
    const c_path native(path);
    if (!native.valid()) {
        return invalid_path();
    }

    if (::rmdir(native.get()) != 0) {
        return from_errno("删除目录", path, errno);
    }
    // ENOTEMPTY 经 kind_of 映射成 already_exists：调用方拿它当「非空」的信号。
    return result<void>();
}

result<void> remove_link_or_file(string_view path) {
    const c_path native(path);
    if (!native.valid()) {
        return invalid_path();
    }

    // 先删，删掉了就是一次系统调用。unlink 删的是链接本身，目标原样不动。
    if (::unlink(native.get()) == 0) {
        return result<void>();
    }
    // 对目录 unlink，Linux 报 EISDIR，macOS 和某些权限下报 EPERM：这两种再 lstat 一次分清楚，
    // 真实目录报 already_exists。
    const int failure = errno;
    struct stat info;
    if ((failure == EISDIR || failure == EPERM) && ::lstat(native.get(), &info) == 0 && S_ISDIR(info.st_mode)) {
        return make_error(error_kind::already_exists,
                          path.to_string() + " 是目录，空目录请走 remove_empty_directory");
    }
    return from_errno("删除", path, failure);
}

// --- 原生目录句柄 ------------------------------------------------------------------

directory_handle::directory_handle(void* handle) noexcept : handle_(handle) {}

directory_handle::~directory_handle() {
    if (handle_) {
        ::closedir(static_cast<DIR*>(handle_));
    }
}

directory_handle::directory_handle(directory_handle&& other) noexcept
    : handle_(other.handle_) {
    other.handle_ = 0;
}

directory_handle& directory_handle::operator=(directory_handle&& other) noexcept {
    if (this != &other) {
        if (handle_) {
            ::closedir(static_cast<DIR*>(handle_));
        }
        handle_ = other.handle_;
        other.handle_ = 0;
    }
    return *this;
}

result<directory_handle> directory_handle::open(string_view path) {
    const c_path native(path);
    if (!native.valid()) {
        return invalid_path();
    }

    DIR* dir = ::opendir(native.get());
    if (!dir) {
        return from_errno("打开目录", path, errno);
    }
    return directory_handle(dir);
}

namespace {

// 类型只用 readdir 顺带给的 d_type，不补 stat：给不了就是 unknown，调用方需要时自己查。
#if defined(DT_DIR)
entry_kind hint_of(const struct dirent& item) {
    switch (item.d_type) {
        case DT_REG:
            return entry_kind::regular_file;
        case DT_DIR:
            return entry_kind::directory;
        case DT_LNK:
            return entry_kind::link;
        case DT_UNKNOWN:
            return entry_kind::unknown;
        default:
            return entry_kind::other;
    }
}
#else
entry_kind hint_of(const struct dirent&) {
    return entry_kind::unknown;
}
#endif

}  // namespace

result<bool> directory_handle::next(string_view& name, entry_kind& hint) {
    DIR* dir = static_cast<DIR*>(handle_);
    if (!dir) {
        return false;
    }

    for (;;) {
        errno = 0;
        const struct dirent* item = ::readdir(dir);
        if (!item) {
            if (errno != 0) {
                return from_errno("读取目录", "", errno);
            }
            return false;
        }
        if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0) {
            continue;
        }
        name = string_view(item->d_name);
        hint = hint_of(*item);
        return true;
    }
}

}  // namespace detail

// --- 公共 API 的平台部分 -----------------------------------------------------------
//
// culib/fs.h 里点名由平台实现的操作。

namespace {

result<std::uint64_t> file_size_native(string_view target) {
    const c_path native(target);
    if (!native.valid()) {
        return invalid_path();
    }

    struct stat info;
    if (::stat(native.get(), &info) != 0) {
        return from_errno("取大小", target, errno);
    }
    if (S_ISDIR(info.st_mode)) {
        return make_error(error_kind::unsupported, target.to_string() + " 是目录，没有文件大小");
    }
    return static_cast<std::uint64_t>(info.st_size);
}

result<wavvix::timestamp> last_write_time_native(string_view target) {
    const c_path native(target);
    if (!native.valid()) {
        return invalid_path();
    }

    struct stat info;
    if (::stat(native.get(), &info) != 0) {
        return from_errno("取修改时间", target, errno);
    }
    // POSIX 2008 的名字是 st_mtim，macOS 叫 st_mtimespec。
#if defined(__APPLE__)
    const struct timespec& modified = info.st_mtimespec;
#else
    const struct timespec& modified = info.st_mtim;
#endif
    // 时刻按毫秒存：秒数再大就放不下。这是文件系统里的数据，不该让进程终止。
    const std::int64_t seconds = static_cast<std::int64_t>(modified.tv_sec);
    const std::int64_t limit = (std::numeric_limits<std::int64_t>::max)() / 1000 - 1;
    if (seconds > limit || seconds < -limit) {
        return make_error(error_kind::out_of_range, "修改时间超出范围：" + target.to_string());
    }
    return timestamp::from_unix_millis(seconds * 1000 + static_cast<std::int64_t>(modified.tv_nsec / 1000000));
}

int open_retrying(const char* path, int flags) {
    int fd;
    do {
        fd = ::open(path, flags, 0666);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

// 只复制普通文件，和 std::filesystem::copy_file 一致。目录用 O_RDONLY 也打得开，读的时候才报
// EISDIR，所以要在建目标之前查，免得失败的复制在磁盘上留下空的（覆盖时是被截空的）目标；
// fifo、设备这类文件读起来要么阻塞要么语义不对。
result<void> check_source(int fd, string_view from, struct stat& info) {
    if (::fstat(fd, &info) != 0) {
        return from_errno("检查复制源", from, errno);
    }
    if (!S_ISREG(info.st_mode)) {
        return make_error(error_kind::unsupported, from.to_string() + " 不是普通文件，copy_file 只复制普通文件");
    }
    return result<void>();
}

// 覆盖之前：源和目标是同一个文件（同一路径、硬链接、经由链接绕回来都算）就报错，目标也得是
// 普通文件。这两样都要在截断之前查，否则内容先被清空了。
result<void> prepare_target(int fd, string_view to, const struct stat& source) {
    struct stat info;
    if (::fstat(fd, &info) != 0) {
        return from_errno("检查复制文件", to, errno);
    }
    if (info.st_dev == source.st_dev && info.st_ino == source.st_ino) {
        return make_error(error_kind::invalid_argument, "复制源和目标是同一文件");
    }
    if (!S_ISREG(info.st_mode)) {
        return make_error(error_kind::unsupported, to.to_string() + " 不是普通文件，copy_file 只覆盖普通文件");
    }
    if (::ftruncate(fd, 0) != 0) {
        return from_errno("截断", to, errno);
    }
    return result<void>();
}

result<void> copy_contents(std::intptr_t source, string_view from, std::intptr_t target, string_view to) {
    char chunk[65536];
    for (;;) {
        result<std::size_t> got = detail::read_some(source, chunk, sizeof chunk, from);
        if (got.is_err()) {
            return std::move(got).error();
        }
        if (got.value() == 0) {
            return result<void>();
        }
        const result<void> written = detail::write_all(target, chunk, got.value(), to);
        if (written.is_err()) {
            return written;
        }
    }
}

// 不覆盖时带 O_EXCL：目标是悬空链接也报 already_exists，不会透过链接把文件建到它指的地方。
// 覆盖时不带 O_TRUNC，截断要等查过是不是同一个文件。O_NONBLOCK 免得已有的 fifo 没人读时卡住。
int open_target(const char* path, bool overwrite) {
    return open_retrying(path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NONBLOCK | (overwrite ? 0 : O_EXCL));
}

// 权限跟着源走，不管目标是新建的还是被覆盖的。关闭也会失败（比如网络盘写回出错），要报出来。
result<void> finish_target(std::intptr_t& target, string_view to, mode_t mode) {
    if (::fchmod(static_cast<int>(target), mode & 07777) != 0) {
        return from_errno("设置权限", to, errno);
    }
    const unsigned long code = detail::close_file(target);
    if (code != 0) {
        return detail::file_error("关闭", to, code);
    }
    return result<void>();
}

result<void> copy_file_native(string_view from, string_view to, bool overwrite) {
    const c_path native_from(from);
    const c_path native_to(to);
    if (!native_from.valid() || !native_to.valid()) {
        return invalid_path();
    }

    std::intptr_t source = open_retrying(native_from.get(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (source < 0) {
        return from_errno("打开", from, errno);
    }
    WAVVIX_DEFER { (void)detail::close_file(source); };
    struct stat source_info;
    const result<void> checked = check_source(static_cast<int>(source), from, source_info);
    if (checked.is_err()) {
        return checked;
    }

    std::intptr_t target = open_target(native_to.get(), overwrite);
    if (target < 0) {
        return from_errno("创建", to, errno);   // EEXIST 映射成 already_exists
    }
    WAVVIX_DEFER { (void)detail::close_file(target); };
    if (overwrite) {
        const result<void> prepared = prepare_target(static_cast<int>(target), to, source_info);
        if (prepared.is_err()) {
            return prepared;
        }
    }
    const result<void> copied = copy_contents(source, from, target, to);
    if (copied.is_err()) {
        return copied;
    }
    return finish_target(target, to, source_info.st_mode);
}

result<void> rename_native(string_view from, string_view to) {
    const c_path native_from(from);
    const c_path native_to(to);
    if (!native_from.valid() || !native_to.valid()) {
        return invalid_path();
    }

    if (::rename(native_from.get(), native_to.get()) != 0) {
        return from_errno_pair("重命名", from, to, errno);
    }
    return result<void>();
}

result<path> current_directory_native() {
    // PATH_MAX 不可靠（有些文件系统能超过），所以按需翻倍重试。
    std::vector<char> buffer(512);
    for (;;) {
        if (::getcwd(&buffer[0], buffer.size())) {
            return path(std::string(&buffer[0]));
        }
        if (errno != ERANGE) {
            return from_errno("取当前目录", "", errno);
        }
        if (buffer.size() > (1u << 20)) {
            return from_errno("取当前目录", "", ENAMETOOLONG);
        }
        buffer.resize(buffer.size() * 2);
    }
}

result<void> set_current_directory_native(string_view directory) {
    const c_path native(directory);
    if (!native.valid()) {
        return invalid_path();
    }

    if (::chdir(native.get()) != 0) {
        return from_errno("切换到", directory, errno);
    }
    return result<void>();
}

}  // namespace

namespace detail {

result<std::string> make_absolute(string_view target) {
    result<path> base = current_directory_native();
    if (base.is_err()) {
        return std::move(base).error();
    }
    return (base.value() / path(target.to_string())).string();
}

}  // namespace detail

result<std::uint64_t> file_size(const path& target) {
    return file_size_native(target.string());
}

result<wavvix::timestamp> last_write_time(const path& target) {
    return last_write_time_native(target.string());
}

result<void> copy_file(const path& from, const path& to, copy_options options) {
    const bool overwrite = (static_cast<unsigned>(options) &
                            static_cast<unsigned>(copy_options::overwrite_existing)) != 0;
    return copy_file_native(from.string(), to.string(), overwrite);
}

result<void> rename(const path& from, const path& to) {
    return rename_native(from.string(), to.string());
}

result<path> current_path() {
    return current_directory_native();
}

result<void> current_path(const path& directory) {
    return set_current_directory_native(directory.string());
}

// 只读环境变量，不碰磁盘。
result<path> temp_directory_path() {
    static const char* const names[] = {"TMPDIR", "TMP", "TEMP"};
    for (std::size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        const char* value = std::getenv(names[i]);
        if (value && value[0] != '\0') {
            return path(std::string(value));
        }
    }
    return path(std::string("/tmp"));
}

}  // namespace fs
}  // namespace wavvix
