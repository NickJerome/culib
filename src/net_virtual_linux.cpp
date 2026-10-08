#include "net_virtual_backend.h"
#include "scheduler_detail.h"

#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace wavvix {
namespace net {
namespace detail {

struct virtual_interface_state {
    int fd;
    std::string name;
    bool adopted;
    virtual_interface_state(int descriptor, std::string assigned)
        : fd(descriptor), name(assigned), adopted(false) {}
};

namespace {

error virtual_error(const char* action, int code) {
    const error_kind kind = code == EACCES || code == EPERM ? error_kind::permission_denied
        : code == ENOENT ? error_kind::not_found
        : code == ETIMEDOUT ? error_kind::timeout : error_kind::io;
    return make_error(kind, std::string(action) + " 失败：" + std::to_string(code));
}

result<void> adopt(virtual_interface_state& state) {
    if (state.adopted) return result<void>();
    if (!wavvix::detail::scheduler_adopt(state.fd))
        return make_error(error_kind::io, "无法将 TUN 交给事件引擎");
    state.adopted = true;
    return result<void>();
}

}  // namespace

result<virtual_interface_state*> virtual_interface_open() {
    const int fd = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return virtual_error("打开 /dev/net/tun", errno);
    ifreq request;
    std::memset(&request, 0, sizeof request);
    request.ifr_flags = IFF_TUN | IFF_NO_PI;
    if (::ioctl(fd, TUNSETIFF, &request) < 0) {
        const int code = errno;
        ::close(fd);
        return virtual_error("创建 TUN 接口", code);
    }
    return new virtual_interface_state(fd, request.ifr_name);
}

void virtual_interface_destroy(virtual_interface_state* state) noexcept {
    if (!state) return;
    ::close(state->fd);
    delete state;
}

string_view virtual_interface_name(const virtual_interface_state* state) noexcept {
    return state ? string_view(state->name) : string_view();
}

result<std::size_t> virtual_interface_read(virtual_interface_state* state, void* buffer,
                                           std::size_t capacity,
                                           std::chrono::nanoseconds timeout) {
    if (!state || !buffer)
        return make_error(error_kind::invalid_argument, "TUN 接收参数无效");
    if (capacity < 20)
        return make_error(error_kind::out_of_range, "TUN 接收缓冲区不足");
    result<void> adopted = adopt(*state);
    if (adopted.is_err()) return adopted.error();
    wavvix::detail::io_request request;
    char overflow = 0;
    const std::size_t readable = capacity < virtual_max_packet_size
        ? capacity : virtual_max_packet_size;
    struct iovec vectors[2];
    request.operation = wavvix::detail::io_vectored_transfer;
    request.direction = wavvix::detail::io_read;
    request.handle = state->fd;
    vectors[0].iov_base = buffer;
    vectors[0].iov_len = readable;
    vectors[1].iov_base = &overflow;
    vectors[1].iov_len = 1;
    request.vectors = vectors;
    request.vector_count = 2;
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) return virtual_error("读取 TUN", request.failure);
    if (request.transferred == 0)
        return make_error(error_kind::io, "TUN 已关闭");
    const std::size_t received = static_cast<std::size_t>(request.transferred);
    if (received > readable)
        return make_error(error_kind::out_of_range, "TUN 报文超出接收缓冲区");
    return received;
}

result<std::size_t> virtual_interface_write(virtual_interface_state* state,
                                            const void* packet, std::size_t size,
                                            std::chrono::nanoseconds timeout) {
    if (!state || !packet || size == 0)
        return make_error(error_kind::invalid_argument, "TUN 发送参数无效");
    if (size > virtual_max_packet_size)
        return make_error(error_kind::out_of_range, "TUN 报文超出支持的长度");
    result<void> adopted = adopt(*state);
    if (adopted.is_err()) return adopted.error();
    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_transfer;
    request.direction = wavvix::detail::io_write;
    request.handle = state->fd;
    request.buffer = const_cast<void*>(packet);
    request.length = size;
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) return virtual_error("写入 TUN", request.failure);
    if (static_cast<std::size_t>(request.transferred) != size)
        return make_error(error_kind::io, "TUN 报文只写入了一部分");
    return size;
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
