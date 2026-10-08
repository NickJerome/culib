#pragma once

#include <culib/result.h>
#include <culib/string_view.h>

#include <chrono>
#include <cstddef>

namespace wavvix {
namespace net {

namespace detail { struct virtual_interface_state; }

// 三层虚拟网卡。open() 创建临时接口，析构时关闭；地址和路由由调用方配置。
class virtual_interface {
public:
    static constexpr std::size_t max_packet_size = 65535;

    virtual_interface() noexcept : state_(0) {}
    ~virtual_interface();
    virtual_interface(virtual_interface&& other) noexcept;
    virtual_interface& operator=(virtual_interface&& other) noexcept;
    virtual_interface(const virtual_interface&) = delete;
    virtual_interface& operator=(const virtual_interface&) = delete;

    static result<virtual_interface> open();

    // 返回接口名的非拥有视图；关闭或移动本对象后视图失效。
    string_view name() const noexcept;
    bool is_open() const noexcept { return state_ != 0; }
    void close() noexcept;

    // 一次读取一个 IP 包；缓冲区不足时消费该包并返回 out_of_range。
    // 只能在调度器协程中调用；负超时表示一直等待。
    result<std::size_t> read(void* buffer, std::size_t capacity,
                             std::chrono::nanoseconds timeout);
    result<std::size_t> write(const void* packet, std::size_t size,
                              std::chrono::nanoseconds timeout);

private:
    explicit virtual_interface(detail::virtual_interface_state* state) noexcept : state_(state) {}
    detail::virtual_interface_state* state_;
};

}  // namespace net
}  // namespace wavvix
