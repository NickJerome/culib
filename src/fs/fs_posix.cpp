// 读文件的 Linux / macOS 实现。

#include <cu/fs.h>

#include "platform/system_error.h"

#include <cerrno>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cu {
namespace fs {

namespace {

// 一次最多读这么多。macOS 上超过 INT_MAX 字节会报 EINVAL；和 Go 一样取 1 GiB。
const std::size_t max_read = std::size_t(1) << 30;

// 离开作用域时关掉 fd。
class file_guard {
public:
    explicit file_guard(int fd) noexcept : fd_(fd) {}
    file_guard(const file_guard&) = delete;
    file_guard& operator=(const file_guard&) = delete;

    ~file_guard() {
        ::close(fd_);
    }

private:
    int fd_;
};

}  // namespace

result<std::string> read_file(string_view path) {
    const std::string name = path.to_string();
    if (name.find('\0') != std::string::npos) {
        return make_error(error_kind::invalid_argument, "open " + name + ": path contains a NUL byte");
    }
    int fd = -1;
    do {
        fd = ::open(name.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        const int code = errno;
        return cu::detail::system_error("open " + name, code);
    }
    const file_guard guard(fd);

    struct stat info;
    std::size_t expected = 0;
    if (::fstat(fd, &info) == 0) {
        if (S_ISDIR(info.st_mode)) {
            return make_error(error_kind::unsupported, "open " + name + ": is a directory");
        }
        if (S_ISREG(info.st_mode)) {
            expected = static_cast<std::size_t>(info.st_size);
        }
    }

    // 比文件大小多一个字节：读满文件以后再读一次拿到 0，才知道到头了，这一次不用扩容。
    std::string content(expected + 1 > 512 ? expected + 1 : 512, '\0');
    std::size_t used = 0;
    for (;;) {
        if (used == content.size()) {
            content.resize(content.size() * 2);
        }
        const std::size_t room = content.size() - used;
        const ssize_t got = ::read(fd, &content[used], room < max_read ? room : max_read);
        if (got > 0) {
            used += static_cast<std::size_t>(got);
            continue;
        }
        if (got == 0) {
            break;
        }
        const int code = errno;
        if (code == EINTR) {
            continue;
        }
        return cu::detail::system_error("read " + name, code);
    }
    content.resize(used);
    return result<std::string>(std::move(content));
}

}  // namespace fs
}  // namespace cu
