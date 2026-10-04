// fs 的 POSIX 实现（Linux + macOS）。

#include <culib/fs.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
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

// string_view 不保证 '\0' 结尾，而所有系统调用都要 C 字符串。
// 短路径走栈，长路径才分配——路径在 HDD / 网络盘上本来就慢，这点优化是为了循环遍历。
class c_path {
public:
    explicit c_path(string_view path) {
        if (path.size() < sizeof stack_) {
            std::memcpy(stack_, path.data(), path.size());
            stack_[path.size()] = '\0';
            text_ = stack_;
        } else {
            heap_.assign(path.data(), path.size());
            text_ = heap_.c_str();
        }
    }
    const char* get() const { return text_; }

    // text_ 可能指向 heap_ 内部，复制会悬垂
    c_path(const c_path&) = delete;
    c_path& operator=(const c_path&) = delete;

private:
    char stack_[256];
    std::string heap_;
    const char* text_;
};

bool stat_path(string_view path, struct stat& info) {
    const c_path native(path);
    return ::stat(native.get(), &info) == 0;   // stat 跟随符号链接，见头文件约定
}

result<void> write_with_mode(string_view path, string_view content, const char* mode) {
    const c_path native(path);
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
    if (!stat_path(path, info)) return from_errno("取大小", path, errno);
    if (S_ISDIR(info.st_mode)) {
        return make_error(error_kind::unsupported, path.to_string() + " 是目录，没有文件大小");
    }
    return static_cast<std::uint64_t>(info.st_size);
}

result<std::int64_t> last_write_time(string_view path) {
    struct stat info;
    if (!stat_path(path, info)) return from_errno("取修改时间", path, errno);
    return static_cast<std::int64_t>(info.st_mtime);
}

// --- 整文件读写 ---------------------------------------------------------------

result<void> read_file(string_view path, std::string& out) {
    const c_path native(path);
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

// --- 目录与文件操作 -----------------------------------------------------------

result<void> create_directory(string_view path, recursive mode) {
    if (path.empty()) return make_error(error_kind::invalid_argument, "路径是空的");

    if (mode == recursive::yes) {
        // 逐级建。中间某级已存在是正常情况，不是错误。
        std::string partial;
        partial.reserve(path.size());
        for (std::size_t i = 0; i <= path.size(); ++i) {
            if (i == path.size() || path.data()[i] == '/') {
                if (!partial.empty()) {
                    if (::mkdir(partial.c_str(), 0777) != 0 && errno != EEXIST) {
                        return from_errno("创建目录", partial, errno);
                    }
                }
                if (i < path.size()) partial.push_back('/');
            } else {
                partial.push_back(path.data()[i]);
            }
        }
        if (!is_directory(path)) {
            return make_error(error_kind::already_exists,
                              path.to_string() + " 已存在但不是目录");
        }
        return result<void>();
    }

    const c_path native(path);
    if (::mkdir(native.get(), 0777) == 0) return result<void>();
    if (errno == EEXIST) {
        // 幂等：已经是目录就算成功，是文件才报错
        if (is_directory(path)) return result<void>();
        return make_error(error_kind::already_exists, path.to_string() + " 已存在但不是目录");
    }
    return from_errno("创建目录", path, errno);
}

result<void> remove_file(string_view path) {
    const c_path native(path);
    if (::unlink(native.get()) != 0) return from_errno("删除", path, errno);
    return result<void>();
}

result<void> remove_directory(string_view path, recursive mode) {
    if (mode == recursive::yes) {
        // 先收集再删：一边遍历一边改目录在部分文件系统上行为未定义。
        std::vector<std::string> children;
        result<void> listed = list_directory(path, [&children](const entry& item) {
            children.push_back(item.name.to_string());
            return true;
        });
        if (listed.is_err()) return listed;
        for (std::size_t i = 0; i < children.size(); ++i) {
            const std::string child = join(path, children[i]);
            // lstat 不跟随符号链接：指向目录的符号链接要删链接本身，
            // 跟进去会删掉链接指向的别处内容，那个代价太大（见头文件约定）。
            struct stat info;
            const c_path native_child(child);
            if (::lstat(native_child.get(), &info) != 0) {
                return from_errno("检查", child, errno);
            }
            result<void> removed = S_ISDIR(info.st_mode)
                                       ? remove_directory(child, recursive::yes)
                                       : remove_file(child);
            if (removed.is_err()) return removed;
        }
    }
    const c_path native(path);
    if (::rmdir(native.get()) != 0) return from_errno("删除目录", path, errno);
    return result<void>();
}

result<void> copy_file(string_view from, string_view to, overwrite mode) {
    if (mode == overwrite::no && exists(to)) {
        return make_error(error_kind::already_exists, to.to_string() + " 已存在");
    }
    const c_path native_from(from);
    std::FILE* source = std::fopen(native_from.get(), "rb");
    if (!source) return from_errno("打开", from, errno);

    const c_path native_to(to);
    std::FILE* target = std::fopen(native_to.get(), "wb");
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
    if (::chdir(native.get()) != 0) return from_errno("切换到", path, errno);
    return result<void>();
}

// --- 遍历 ---------------------------------------------------------------------

namespace detail {

result<directory_handle> directory_handle::open(string_view path) {
    const c_path native(path);
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

bool directory_handle::next(string_view& name, bool& is_directory) {
    DIR* dir = static_cast<DIR*>(handle_);
    if (!dir) return false;
    for (;;) {
        const struct dirent* item = ::readdir(dir);
        if (!item) return false;
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
        is_directory = ::fstatat(::dirfd(dir), item->d_name, &info, 0) == 0 &&
                       S_ISDIR(info.st_mode);
        return true;
    }
}

}  // namespace detail

}  // namespace fs
}  // namespace wavvix
