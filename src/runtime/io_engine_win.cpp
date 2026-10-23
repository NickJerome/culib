// 事件引擎的 Windows 部分，见 io_engine.h。一个完成端口，套接字都关联在上面；操作交给系统以后，等待者等它的完成包，
// 系统线程取完成包、叫醒等着的协程或普通线程。
//
// 没开 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS：当场完成的操作也有完成包，等待者一律等到完成包回来，OVERLAPPED 不会在
// 系统还要写它的时候被下一次操作复用。到点时取消操作，照样等完成包，Go 也这么做。
//
// 系统线程按时醒：GetQueuedCompletionStatusEx 的超时按系统节拍到期，默认约 15.6 ms，监控要每 1 ms 看一次。所以另设
// 一个高精度定时器，用等待完成包把它接到完成端口上，到点时端口里多一个包，Go 也这么做。等待完成包的函数在 ntdll 里，
// 没有头文件和导入库，用 GetProcAddress 取；取不到（较老的 Wine）就只按毫秒超时醒。2026-10-21 本机实测
// （Windows 10 19045）等 1 ms：直接设超时等了 15.5 ms，用定时器加等待完成包 1.5 ms；高精度定时器配 APC 的写法
// APC 不触发。

#include "io_engine.h"
#include "io_socket.h"

#include <cu/timeout.h>

#include "base/error_context.h"
#include "base/fatal.h"
#include "platform/system_error.h"
#include "platform/win32.h"
#include "system_thread.h"
#include "timer.h"
#include "wait.h"

#include <atomic>
#include <cstring>
#include <utility>

namespace cu {
namespace detail {

typedef LONG(NTAPI* create_packet_function)(HANDLE* packet, ACCESS_MASK access, void* attributes);
typedef LONG(NTAPI* associate_packet_function)(HANDLE packet, HANDLE port, HANDLE target, void* key, void* context,
                                               LONG status, ULONG_PTR information, BOOLEAN* already_signaled);

struct io_engine::platform {
    HANDLE port;
    HANDLE timer;    // 高精度定时器；为空表示建不起来，只按毫秒超时醒
    HANDLE packet;   // 等待完成包：定时器到点时往端口里投一个包
    associate_packet_function associate;
    // 定时器设上了、它的包还没取回来，以及设到了哪一刻。只有系统线程用
    bool armed;
    timer_time armed_until;
};

namespace {

const ULONG_PTR timer_key = 1;   // 定时器投的包带这个键，wake 投的是 0
const std::uintptr_t operation_done = 1;

// 一次读写最多这么多，和 Go 一样取 1 GiB（WSABUF 的长度是 32 位的）。
const ULONG max_transfer = 1UL << 30;

// AcceptEx 往缓冲区里写两个地址，每个要比最长的地址多留 16 字节。
const DWORD accept_room = sizeof(sockaddr_in6) + 16;

// ------------------------------------------------------------------ 系统线程按时醒

template <typename Function>
Function ntdll_function(const char* name) noexcept {
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return 0;
    }
    // 经整数转一道：函数指针之间直接转，GCC 报 -Wcast-function-type
    return reinterpret_cast<Function>(reinterpret_cast<std::uintptr_t>(::GetProcAddress(ntdll, name)));
}

// 建定时器和等待完成包。建不起来就都留空。
void open_timer(io_engine::platform& engine) noexcept {
    const create_packet_function create = ntdll_function<create_packet_function>("NtCreateWaitCompletionPacket");
    engine.associate = ntdll_function<associate_packet_function>("NtAssociateWaitCompletionPacket");
    if (!create || !engine.associate) {
        return;
    }
    const HANDLE timer = ::CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        return;
    }
    HANDLE packet = 0;
    if (create(&packet, GENERIC_ALL, 0) < 0) {   // NTSTATUS，负数是失败
        ::CloseHandle(timer);
        return;
    }
    engine.timer = timer;
    engine.packet = packet;
}

// 让端口在 timeout 纳秒以后（until 那一刻）多一个包。已经设了、而且不比 until 晚，就不动它：I/O 多的时候每取一批
// 完成包都要来这里，设得早了顶多多醒一次。
void arm_timer(io_engine::platform& engine, std::int64_t timeout, timer_time until) noexcept {
    if (!engine.timer || (engine.armed && engine.armed_until <= until)) {
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -(timeout / 100 + (timeout % 100 != 0 ? 1 : 0));   // 负数是相对时间，单位 100 纳秒
    if (!::SetWaitableTimer(engine.timer, &due, 0, 0, 0, FALSE)) {
        return;
    }
    // 包还关联着时，重设定时器就够了，到点时照样投包。一个包同一时刻只能关联一次：上一次的包取回来以后才重新关联，
    // 所以不用先撤
    if (!engine.armed) {
        void* const key = reinterpret_cast<void*>(timer_key);
        if (engine.associate(engine.packet, engine.port, engine.timer, key, 0, 0, 0, 0) < 0) {
            return;
        }
        engine.armed = true;
    }
    engine.armed_until = until;
}

// GetQueuedCompletionStatusEx 的超时以毫秒计，不足一毫秒进成一毫秒；-1 是一直等。
DWORD timeout_milliseconds(std::int64_t timeout) noexcept {
    if (timeout < 0) {
        return INFINITE;
    }
    const std::int64_t milliseconds = timeout / 1000000 + (timeout % 1000000 != 0 ? 1 : 0);
    return milliseconds < INFINITE ? static_cast<DWORD>(milliseconds) : INFINITE - 1;
}

// ------------------------------------------------------------------ Winsock

struct extensions {
    LPFN_ACCEPTEX accept;
    LPFN_CONNECTEX connect;
};

template <typename Function>
Function load_extension(SOCKET s, GUID id) noexcept {
    Function function = 0;
    DWORD bytes = 0;
    if (::WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof id, &function, sizeof function, &bytes, 0, 0) != 0) {
        return 0;
    }
    return function;
}

// 启动 Winsock，取 AcceptEx 和 ConnectEx。两个都是 TCP 提供程序的函数，IPv4 和 IPv6 用同一个，取一次就够。
// 起不来、取不到时打印诊断后终止：支持的系统上不会发生。
extensions start_winsock() noexcept {
    WSADATA data;
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        fatal("cu: cannot start Winsock");
    }
    const SOCKET probe =
        ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (probe == INVALID_SOCKET) {
        fatal("cu: cannot create a TCP socket");
    }
    const GUID accept_id = WSAID_ACCEPTEX;
    const GUID connect_id = WSAID_CONNECTEX;
    extensions loaded;
    loaded.accept = load_extension<LPFN_ACCEPTEX>(probe, accept_id);
    loaded.connect = load_extension<LPFN_CONNECTEX>(probe, connect_id);
    ::closesocket(probe);
    if (!loaded.accept || !loaded.connect) {
        fatal("cu: cannot load AcceptEx and ConnectEx");
    }
    return loaded;
}

// 不调 WSACleanup：进程退出时别的线程可能还在用套接字。
const extensions& winsock() noexcept {
    static const extensions loaded = start_winsock();
    return loaded;
}

// overlapped、子进程不继承。失败返回 INVALID_SOCKET，原因在 WSAGetLastError()。
SOCKET open_socket(int family) noexcept {
    return ::WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

void set_no_delay(SOCKET s) noexcept {
    const BOOL on = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

// ------------------------------------------------------------------ 一次操作

// 一次 overlapped 操作，放在等待者的栈上。从交给系统到完成包回来，OVERLAPPED 和缓冲区归系统用；finish 等到完成包
// 回来才返回，所以它析构时系统一定不会再碰它。OVERLAPPED 放在最前面：完成包交回的就是它的地址。
class overlapped_operation {
public:
    explicit overlapped_operation(SOCKET s) noexcept
        : socket_(s), state_(0), transferred_(0), code_(0), timed_out_(false) {
        std::memset(&overlapped_, 0, sizeof overlapped_);
    }
    overlapped_operation(const overlapped_operation&) = delete;
    overlapped_operation& operator=(const overlapped_operation&) = delete;

    OVERLAPPED* get() noexcept {
        return &overlapped_;
    }

    // 提交以后调。submitted：提交函数直接返回了成功；没有的话，错误码是 WSA_IO_PENDING 才算交给了系统。交给了系统的
    // 等完成包，最多等到当前的截止时刻，到点就取消、照样等包；当场完成的不用取消。成功返回 true。
    bool finish(bool submitted) noexcept {
        if (!submitted) {
            code_ = ::WSAGetLastError();
            if (code_ != WSA_IO_PENDING) {
                return false;
            }
        }
        {
            waiter self;
            std::uintptr_t expected = 0;
            if (state_.compare_exchange_strong(expected, reinterpret_cast<std::uintptr_t>(&self))) {
                deadline_timer timer(submitted ? timer_never : current_deadline(), &overlapped_operation::expire,
                                     this);
                self.sleep();   // 叫醒的永远是完成包；到点只是让它早点回来
            }
        }
        DWORD flags = 0;
        if (::WSAGetOverlappedResult(socket_, &overlapped_, &transferred_, FALSE, &flags)) {
            code_ = 0;
            return true;
        }
        code_ = ::WSAGetLastError();
        return false;
    }

    // finish 成功以后：传了多少字节。
    DWORD transferred() const noexcept {
        return transferred_;
    }

    // finish 失败以后：系统的错误码。
    int code() const noexcept {
        return code_;
    }

    // finish 失败以后：换成 cu::error。被自己的定时器取消的是超时；等的时候不能关句柄，所以没有别人会取消它。
    error failure() const {
        if (code_ == WSA_OPERATION_ABORTED && timed_out_) {
            return timeout_error();
        }
        return system_error(code_);
    }

    // 系统线程取到完成包时调。
    void complete() noexcept {
        const std::uintptr_t previous = state_.exchange(operation_done, std::memory_order_acq_rel);
        if (previous > operation_done) {
            reinterpret_cast<waiter*>(previous)->wake();
        }
    }

private:
    // 系统线程上，到点了：记下是自己取消的，再取消这次操作。已经完成的取消不到（ERROR_NOT_FOUND），不碍事。
    static void expire(void* context) noexcept {
        overlapped_operation* const self = static_cast<overlapped_operation*>(context);
        self->timed_out_ = true;
        ::CancelIoEx(reinterpret_cast<HANDLE>(self->socket_), &self->overlapped_);
    }

    OVERLAPPED overlapped_;
    SOCKET socket_;
    std::atomic<std::uintptr_t> state_;   // 0 完成包还没回来，1 回来了，其余是等着的 waiter
    DWORD transferred_;
    int code_;
    bool timed_out_;   // 只在定时器的回调里写；定时器收掉以后才读
};

}  // namespace

// ------------------------------------------------------------------ 引擎

// 端口的最后一个参数是允许几根线程同时处理完成包：取过完成包的线程在叫醒协程，设上限的话会被扣着，所以不设。
io_engine::platform* io_engine::create() {
    platform* const created = new platform();
    created->port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, MAXDWORD);
    if (!created->port) {
        fatal("cu: cannot create the I/O completion port");
    }
    open_timer(*created);
    system_thread_start();
    return created;
}

// 没有 OVERLAPPED 的包是定时器到点或者 wake 投进来的，只为把系统线程叫醒。
void io_engine::poll(std::int64_t timeout) noexcept {
    platform& engine = *state_;
    if (timeout > 0) {
        arm_timer(engine, timeout, timer_after(timer_now(), timeout));
    }

    // 毫秒超时只是兜底：定时器的包没投进来时，按系统节拍醒
    OVERLAPPED_ENTRY entries[64];
    ULONG count = 0;
    if (!::GetQueuedCompletionStatusEx(engine.port, entries, 64, &count, timeout_milliseconds(timeout), FALSE)) {
        return;
    }
    for (ULONG i = 0; i < count; ++i) {
        if (entries[i].lpOverlapped) {
            reinterpret_cast<overlapped_operation*>(entries[i].lpOverlapped)->complete();
        } else if (entries[i].lpCompletionKey == timer_key) {
            engine.armed = false;
        }
    }
}

void io_engine::wake() noexcept {
    ::PostQueuedCompletionStatus(state_->port, 0, 0, 0);
}

// ------------------------------------------------------------------ 套接字

void socket_handle::close() noexcept {
    if (native_ == no_native) {
        return;
    }
    ::closesocket(static_cast<SOCKET>(native_));
    native_ = no_native;
    record_ = nullptr;
}

result<void> socket_handle::attach() {
    const HANDLE handle = reinterpret_cast<HANDLE>(native_);
    if (!::CreateIoCompletionPort(handle, io_engine::instance().state().port, 0, 0)) {
        return system_error(static_cast<int>(::GetLastError()));
    }
    return result<void>();
}

result<socket_handle> socket_handle::listen(const endpoint& local) {
    winsock();
    endpoint wanted = local;
    SOCKET s = open_socket(wanted.at.base.sa_family);
    if (s == INVALID_SOCKET && wanted.any && ::WSAGetLastError() == WSAEAFNOSUPPORT) {   // 系统没开 IPv6：只收 IPv4
        use_ipv4(wanted.at, INADDR_ANY);
        s = open_socket(AF_INET);
    }
    if (s == INVALID_SOCKET) {
        const int code = ::WSAGetLastError();
        return in_context("socket", system_error(code));
    }
    socket_handle made(static_cast<std::uintptr_t>(s), nullptr);

    // Windows 上 IPv6 套接字默认只收 IPv6，双栈要明说。不设 SO_REUSEADDR：在 Windows 上它让别的套接字能绑到同一个
    // 端口上抢连接。
    if (wanted.at.base.sa_family == AF_INET6) {
        const DWORD only_v6 = wanted.any ? 0 : 1;
        ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&only_v6), sizeof only_v6);
    }
    if (::bind(s, &wanted.at.base, address_length(wanted.at)) != 0) {
        const int code = ::WSAGetLastError();
        return in_context("bind", system_error(code));
    }
    if (::listen(s, SOMAXCONN) != 0) {
        const int code = ::WSAGetLastError();
        return in_context("listen", system_error(code));
    }
    result<void> attached = made.attach();
    if (attached.is_err()) {
        return std::move(attached).error();
    }
    return result<socket_handle>(std::move(made));
}

result<socket_handle> socket_handle::connect(const endpoint& peer) {
    const extensions& loaded = winsock();
    const int family = peer.at.base.sa_family;
    const SOCKET s = open_socket(family);
    if (s == INVALID_SOCKET) {
        const int code = ::WSAGetLastError();
        return in_context("socket", system_error(code));
    }
    socket_handle made(static_cast<std::uintptr_t>(s), nullptr);
    result<void> attached = made.attach();
    if (attached.is_err()) {
        return std::move(attached).error();
    }

    // ConnectEx 要求先绑定：绑到任意地址，端口由系统挑。
    socket_address local;
    std::memset(&local, 0, sizeof local);
    local.base.sa_family = static_cast<ADDRESS_FAMILY>(family);
    if (::bind(s, &local.base, address_length(local)) != 0) {
        const int code = ::WSAGetLastError();
        return in_context("bind", system_error(code));
    }
    overlapped_operation operation(s);
    const bool submitted =
        loaded.connect(s, &peer.at.base, address_length(peer.at), 0, 0, 0, operation.get()) != FALSE;
    if (!operation.finish(submitted)) {
        return operation.failure();
    }

    // 让套接字认得自己连上了（getpeername、shutdown 要用）
    ::setsockopt(s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, 0, 0);
    set_no_delay(s);
    return result<socket_handle>(std::move(made));
}

result<socket_handle::accepted> socket_handle::accept() {
    const SOCKET listening = static_cast<SOCKET>(native_);
    const int family = local_address().base.sa_family;
    for (;;) {
        const SOCKET s = open_socket(family);
        if (s == INVALID_SOCKET) {
            return system_error(::WSAGetLastError());
        }
        socket_handle made(static_cast<std::uintptr_t>(s), nullptr);
        result<void> attached = made.attach();
        if (attached.is_err()) {
            return std::move(attached).error();
        }

        char addresses[2 * accept_room];
        overlapped_operation operation(listening);
        DWORD received = 0;
        const bool submitted = winsock().accept(listening, s, addresses, 0, accept_room, accept_room, &received,
                                                operation.get()) != FALSE;
        if (!operation.finish(submitted)) {
            const int code = operation.code();
            if (code == WSAECONNRESET || code == ERROR_NETNAME_DELETED) {   // 对端在 accept 之前就断开了
                continue;
            }
            return operation.failure();
        }

        // 让新套接字认得自己的地址（getpeername、shutdown 要用）
        ::setsockopt(s, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<const char*>(&listening),
                     sizeof listening);
        set_no_delay(s);
        socket_address remote;
        std::memset(&remote, 0, sizeof remote);
        int length = sizeof remote;
        ::getpeername(s, &remote.base, &length);
        accepted taken = {std::move(made), remote};
        return result<accepted>(std::move(taken));
    }
}

result<std::size_t> socket_handle::read(void* buffer, std::size_t capacity) {
    const SOCKET s = static_cast<SOCKET>(native_);
    WSABUF slice;
    slice.buf = static_cast<char*>(buffer);
    slice.len = capacity < max_transfer ? static_cast<ULONG>(capacity) : max_transfer;
    overlapped_operation operation(s);
    DWORD flags = 0;
    const bool submitted = ::WSARecv(s, &slice, 1, 0, &flags, operation.get(), 0) == 0;
    if (!operation.finish(submitted)) {
        return operation.failure();
    }
    return std::size_t(operation.transferred());
}

result<std::size_t> socket_handle::write(const void* data, std::size_t size) {
    const SOCKET s = static_cast<SOCKET>(native_);
    WSABUF slice;
    slice.buf = static_cast<char*>(const_cast<void*>(data));
    slice.len = size < max_transfer ? static_cast<ULONG>(size) : max_transfer;
    overlapped_operation operation(s);
    const bool submitted = ::WSASend(s, &slice, 1, 0, 0, operation.get(), 0) == 0;
    if (!operation.finish(submitted)) {
        return operation.failure();
    }
    return std::size_t(operation.transferred());
}

socket_address socket_handle::local_address() const {
    socket_address at;
    std::memset(&at, 0, sizeof at);
    int length = sizeof at;
    ::getsockname(static_cast<SOCKET>(native_), &at.base, &length);
    return at;
}

}  // namespace detail
}  // namespace cu
