#pragma once

// 事件引擎上的一个套接字。放在公共目录只因为 cu::net::listener、cu::net::connection 要按值放一个（和 spin_lock.h
// 一样），使用方不应依赖。事件引擎的说明见 src/runtime/io_engine.h。

#include <cu/result.h>

#include <cstddef>
#include <cstdint>

namespace cu {
namespace detail {

union socket_address;   // 系统调用用的地址，见 src/platform/socket_address.h
struct endpoint;        // 拆好的「host:port」，同上

// 登记在事件引擎上的一个套接字。只能移动，析构时撤下登记、关掉句柄。三个平台同一份声明，实现在
// src/runtime/io_engine_posix.cpp（Linux、macOS）和 src/runtime/io_engine_win.cpp。
//
// 会等的操作（connect、accept、read、write）在协程里只挂起当前协程，在普通线程上阻塞这根线程；最多等到当前的截止
// 时刻（cu::timeout 定的），到点返回 error_kind::timeout，消息是 "i/o timeout"。失败的消息只有系统原话，或者
// 「步骤: 原话」（例如 "bind: Address already in use"），是哪个操作、哪个地址由调用方补。同一时刻最多一个读、
// 一个写；还有人在等的时候不能析构。
class socket_handle {
public:
    struct accepted;   // accept 的结果：新连接和对端地址，见 src/runtime/io_socket.h

    socket_handle() noexcept : native_(no_native), record_(nullptr) {}

    socket_handle(socket_handle&& other) noexcept : native_(other.native_), record_(other.record_) {
        other.native_ = no_native;
        other.record_ = nullptr;
    }

    // 先关掉自己原来那个，再接过来。
    socket_handle& operator=(socket_handle&& other) noexcept {
        if (this != &other) {
            close();
            native_ = other.native_;
            record_ = other.record_;
            other.native_ = no_native;
            other.record_ = nullptr;
        }
        return *this;
    }

    socket_handle(const socket_handle&) = delete;
    socket_handle& operator=(const socket_handle&) = delete;

    ~socket_handle() {
        close();
    }

    // 在 local 上监听：建套接字、设选项、bind、listen、登记。中途失败时什么也不留下。
    static result<socket_handle> listen(const endpoint& local);

    // 连到 peer：建套接字、发起连接、等连上、登记。会等。
    static result<socket_handle> connect(const endpoint& peer);

    // 等下一个连接。对端在 accept 之前就断开的，接着等下一个。
    result<accepted> accept();

    // 读到一些就返回，最多 capacity 字节；返回 0 表示对端关了。
    result<std::size_t> read(void* buffer, std::size_t capacity);

    // 写出一些就返回，返回写了多少；要全部写完由调用方循环。
    result<std::size_t> write(const void* data, std::size_t size);

    socket_address local_address() const;

private:
    static const std::uintptr_t no_native = ~std::uintptr_t(0);   // POSIX 的 -1、Windows 的 INVALID_SOCKET

    // 接管一个刚建好的句柄。record 为空表示还没登记。
    socket_handle(std::uintptr_t native, void* record) noexcept : native_(native), record_(record) {}

    // 登记到事件引擎：Linux / macOS 加进 epoll / kqueue，Windows 关联到完成端口。
    result<void> attach();

    void close() noexcept;

    std::uintptr_t native_;   // POSIX 的 fd，Windows 的 SOCKET
    void* record_;            // Linux / macOS：事件引擎里这个句柄的记录；Windows 不用
};

}  // namespace detail
}  // namespace cu
