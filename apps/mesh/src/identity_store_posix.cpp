#include "identity_store_platform.h"

#include <sodium.h>

#include <cerrno>
#include <cstdint>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mesh {
namespace detail {
namespace {

wavvix::error file_error(const char* operation, int code) {
    const wavvix::error_kind kind =
        code == EEXIST ? wavvix::error_kind::already_exists :
        code == ENOENT ? wavvix::error_kind::not_found :
        (code == EACCES || code == EPERM) ? wavvix::error_kind::permission_denied :
        wavvix::error_kind::io;
    return wavvix::make_error(kind, std::string(operation) + ": errno " +
                                   std::to_string(code));
}

std::string temporary_path(const std::string& path) {
    unsigned char random[8];
    randombytes_buf(random, sizeof(random));
    static const char hex[] = "0123456789abcdef";
    std::string result = path + ".tmp-";
    for (std::size_t i = 0; i < sizeof(random); ++i) {
        result += hex[random[i] >> 4];
        result += hex[random[i] & 15];
    }
    return result;
}

}  // namespace

wavvix::result<void> read_identity_bytes(
    wavvix::string_view path, unsigned char* output, std::size_t size) {
    const std::string native(path.data(), path.size());
    const int fd = ::open(native.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return file_error("open identity file", errno);
    struct stat info;
    if (::fstat(fd, &info) != 0) {
        const int code = errno;
        ::close(fd);
        return file_error("inspect identity file", code);
    }
    if (!S_ISREG(info.st_mode) || info.st_size != static_cast<off_t>(size)) {
        ::close(fd);
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "identity file must be a regular file of the expected size");
    }
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t got = ::read(fd, output + offset, size - offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            const int code = got < 0 ? errno : EIO;
            ::close(fd);
            return file_error("read identity file", code);
        }
        offset += static_cast<std::size_t>(got);
    }
    unsigned char trailing = 0;
    ssize_t extra = 0;
    do {
        extra = ::read(fd, &trailing, 1);
    } while (extra < 0 && errno == EINTR);
    const int extra_code = errno;
    ::close(fd);
    if (extra < 0) return file_error("finish reading identity file", extra_code);
    if (extra != 0)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "identity file changed while reading");
    return wavvix::result<void>();
}

wavvix::result<void> publish_identity_bytes(
    wavvix::string_view path, const unsigned char* input, std::size_t size) {
    const std::string final_path(path.data(), path.size());
    std::string temporary;
    int fd = -1;
    for (int attempt = 0; attempt < 4; ++attempt) {
        temporary = temporary_path(final_path);
        fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0600);
        if (fd >= 0) break;
        if (errno != EEXIST) return file_error("create identity temp file", errno);
    }
    if (fd < 0) return file_error("create identity temp file", EEXIST);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t wrote = ::write(fd, input + offset, size - offset);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) {
            const int code = wrote < 0 ? errno : EIO;
            ::close(fd);
            ::unlink(temporary.c_str());
            return file_error("write identity temp file", code);
        }
        offset += static_cast<std::size_t>(wrote);
    }
    if (::fsync(fd) != 0) {
        const int code = errno;
        ::close(fd);
        ::unlink(temporary.c_str());
        return file_error("sync identity temp file", code);
    }
    if (::close(fd) != 0) {
        const int code = errno;
        ::unlink(temporary.c_str());
        return file_error("close identity temp file", code);
    }
    if (::link(temporary.c_str(), final_path.c_str()) != 0) {
        const int code = errno;
        ::unlink(temporary.c_str());
        return file_error("publish identity file", code);
    }
    ::unlink(temporary.c_str());
    return wavvix::result<void>();
}

}  // namespace detail
}  // namespace mesh
