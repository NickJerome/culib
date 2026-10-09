#include "fs_watcher_metadata.h"

#include <cerrno>
#include <cstring>
#include <string>

#include <sys/stat.h>

namespace wavvix {
namespace fs {
namespace detail {

result<snapshot_metadata> read_snapshot_metadata(string_view path) {
    const std::string name = path.to_string();
    struct stat info;
    if (::lstat(name.c_str(), &info) != 0) {
        const int code = errno;
        const error_kind kind = code == ENOENT || code == ENOTDIR ? error_kind::not_found
            : code == EACCES || code == EPERM ? error_kind::permission_denied
            : error_kind::io;
        return make_error(kind, "查询 " + name + " 失败：" + std::strerror(code));
    }

    snapshot_metadata value;
    value.device = static_cast<std::uint64_t>(info.st_dev);
    value.identity = static_cast<std::uint64_t>(info.st_ino);
    value.size = static_cast<std::uint64_t>(info.st_size);
#if defined(__APPLE__)
    value.modified = static_cast<std::uint64_t>(info.st_mtimespec.tv_sec) * 1000000000ull +
                     static_cast<std::uint64_t>(info.st_mtimespec.tv_nsec);
#else
    value.modified = static_cast<std::uint64_t>(info.st_mtim.tv_sec) * 1000000000ull +
                     static_cast<std::uint64_t>(info.st_mtim.tv_nsec);
#endif
    value.type = static_cast<unsigned>(info.st_mode & S_IFMT);
    value.directory = S_ISDIR(info.st_mode);
    value.link = S_ISLNK(info.st_mode);
    return value;
}

}  // namespace detail
}  // namespace fs
}  // namespace wavvix
