#include "net_virtual_backend.h"
#include "scheduler_detail.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/if_utun.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
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
        return make_error(error_kind::io, "无法将 utun 交给事件引擎");
    state.adopted = true;
    return result<void>();
}

}  // namespace

result<virtual_interface_state*> virtual_interface_open() {
    const int fd = ::socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0) return virtual_error("打开 utun 控制套接字", errno);
    ctl_info info;
    std::memset(&info, 0, sizeof info);
    std::strncpy(info.ctl_name, UTUN_CONTROL_NAME, sizeof info.ctl_name - 1);
    if (::ioctl(fd, CTLIOCGINFO, &info) < 0) {
        const int code = errno;
        ::close(fd);
        return virtual_error("查询 utun 控制器", code);
    }
    sockaddr_ctl address;
    std::memset(&address, 0, sizeof address);
    address.sc_len = sizeof address;
    address.sc_family = AF_SYSTEM;
    address.ss_sysaddr = AF_SYS_CONTROL;
    address.sc_id = info.ctl_id;
    address.sc_unit = 0;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) < 0) {
        const int code = errno;
        ::close(fd);
        return virtual_error("创建 utun 接口", code);
    }
    char name[IFNAMSIZ] = {};
    socklen_t name_size = sizeof name;
    if (::getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, name, &name_size) < 0) {
        const int code = errno;
        ::close(fd);
        return virtual_error("读取 utun 接口名", code);
    }
    name[sizeof name - 1] = '\0';
    const int flags = ::fcntl(fd, F_GETFL);
    const int descriptor_flags = ::fcntl(fd, F_GETFD);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        descriptor_flags < 0 || ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        const int code = errno;
        ::close(fd);
        return virtual_error("设置 utun 非阻塞模式", code);
    }
    return new virtual_interface_state(fd, name);
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
        return make_error(error_kind::invalid_argument, "utun 接收参数无效");
    if (capacity < 20)
        return make_error(error_kind::out_of_range, "utun 接收缓冲区不足");
    result<void> adopted = adopt(*state);
    if (adopted.is_err()) return adopted.error();
    std::uint32_t family = 0;
    char overflow = 0;
    const std::size_t readable = capacity < virtual_max_packet_size
        ? capacity : virtual_max_packet_size;
    struct iovec vectors[3];
    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_vectored_transfer;
    request.direction = wavvix::detail::io_read;
    request.handle = state->fd;
    vectors[0].iov_base = &family;
    vectors[0].iov_len = sizeof family;
    vectors[1].iov_base = buffer;
    vectors[1].iov_len = readable;
    vectors[2].iov_base = &overflow;
    vectors[2].iov_len = 1;
    request.vectors = vectors;
    request.vector_count = 3;
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) return virtual_error("读取 utun", request.failure);
    if (request.transferred < static_cast<std::ptrdiff_t>(sizeof family))
        return make_error(error_kind::io, "utun 报文缺少地址族前缀");
    const std::size_t received = static_cast<std::size_t>(request.transferred) - sizeof family;
    if (received > readable)
        return make_error(error_kind::out_of_range, "utun 报文超出接收缓冲区");
    const std::uint32_t protocol = ntohl(family);
    const unsigned version = received ? static_cast<unsigned>(
        static_cast<unsigned char*>(buffer)[0] >> 4) : 0;
    if ((protocol != AF_INET || version != 4 || received < 20) &&
        (protocol != AF_INET6 || version != 6 || received < 40))
        return make_error(error_kind::io, "utun 地址族与 IP 报文不匹配");
    return received;
}

result<std::size_t> virtual_interface_write(virtual_interface_state* state,
                                            const void* packet, std::size_t size,
                                            std::chrono::nanoseconds timeout) {
    if (!state || !packet || size == 0)
        return make_error(error_kind::invalid_argument, "utun 发送参数无效");
    if (size > virtual_max_packet_size)
        return make_error(error_kind::out_of_range, "utun 报文超出支持的长度");
    const unsigned version = static_cast<const unsigned char*>(packet)[0] >> 4;
    if ((version == 4 && size < 20) || (version == 6 && size < 40) ||
        (version != 4 && version != 6))
        return make_error(error_kind::invalid_argument, "utun 只接受完整的 IPv4/IPv6 头");
    result<void> adopted = adopt(*state);
    if (adopted.is_err()) return adopted.error();
    std::uint32_t family = htonl(version == 4 ? AF_INET : AF_INET6);
    struct iovec vectors[2];
    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_vectored_transfer;
    request.direction = wavvix::detail::io_write;
    request.handle = state->fd;
    vectors[0].iov_base = &family;
    vectors[0].iov_len = sizeof family;
    vectors[1].iov_base = const_cast<void*>(packet);
    vectors[1].iov_len = size;
    request.vectors = vectors;
    request.vector_count = 2;
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) return virtual_error("写入 utun", request.failure);
    if (static_cast<std::size_t>(request.transferred) != sizeof family + size)
        return make_error(error_kind::io, "utun 报文只写入了一部分");
    return size;
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
