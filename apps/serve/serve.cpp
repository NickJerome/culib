// serve 的第一步：每个请求都回同一个文件，回完就关连接。
//   serve <address> <file>，例如 serve :8080 index.html

#include <cu/coroutine.h>
#include <cu/fs.h>
#include <cu/net.h>
#include <cu/timeout.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <utility>

namespace {

// 收完请求头最多等这么久（hyper 的默认值也是 30 秒），写完一个响应最多等这么久：慢客户端和闲着的连接到点就断开。
// 响应现在是一个小文件；文件大了以后写的时限要改成每次写之间的空闲时间。
const std::chrono::seconds head_timeout(30);
const std::chrono::seconds send_timeout(30);

// 读到请求头结束（一个空行）。请求头超过 64 KiB、对端先关了，都算失败。
cu::result<std::string> read_head(cu::net::connection& connection) {
    std::string head;
    char buffer[4096];
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (head.size() > 64 * 1024) {
            return cu::make_error(cu::error_kind::invalid_argument, "request head is longer than 64 KiB");
        }
        cu::result<std::size_t> got = connection.read(buffer, sizeof buffer);
        if (got.is_err()) {
            return std::move(got).error();
        }
        if (got.value() == 0) {
            return cu::make_error(cu::error_kind::io, "peer closed the connection before the request head ended");
        }
        head.append(buffer, got.value());
    }
    return head;
}

// 收到完整的请求头就回文件。收不全（到点、太长、对端先关）直接关连接；写失败打一行。
void handle(cu::net::connection connection, const std::string& response) {
    cu::result<std::string> head = cu::timeout(head_timeout, [&connection] {
        return read_head(connection);
    });
    if (head.is_err()) {
        return;
    }
    cu::result<void> sent = cu::timeout(send_timeout, [&connection, &response] {
        return connection.write(response.data(), response.size());
    });
    if (sent.is_err()) {
        std::fprintf(stderr, "serve: %s\n", sent.error().message().c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: serve <address> <file>\n");
        return 2;
    }
    cu::result<std::string> body = cu::fs::read_file(argv[2]);
    if (body.is_err()) {
        std::fprintf(stderr, "serve: %s\n", body.error().message().c_str());
        return 1;
    }
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                                 std::to_string(body.value().size()) + "\r\nConnection: close\r\n\r\n" +
                                 body.value();

    cu::result<cu::net::listener> listening = cu::net::listen(argv[1]);
    if (listening.is_err()) {
        std::fprintf(stderr, "serve: %s\n", listening.error().message().c_str());
        return 1;
    }
    std::printf("serve: listening on %s\n", listening.value().local_address().to_string().c_str());
    std::fflush(stdout);

    // main 是普通线程：accept 阻塞这根线程，每个连接交给一根协程
    std::chrono::milliseconds backoff(0);
    for (;;) {
        cu::result<cu::net::connection> accepted = listening.value().accept();
        if (accepted.is_ok()) {
            backoff = std::chrono::milliseconds(0);
            cu::coroutine(handle, std::move(accepted).value(), std::cref(response)).detach();
            continue;
        }
        if (accepted.error().kind() != cu::error_kind::resource_exhausted) {
            std::fprintf(stderr, "serve: %s\n", accepted.error().message().c_str());
            return 1;
        }
        // 句柄用完：等一会儿再试，从 5 ms 起翻倍，最多 1 s，和 Go 的 net/http 一样
        backoff = std::min(std::max(backoff * 2, std::chrono::milliseconds(5)), std::chrono::milliseconds(1000));
        std::fprintf(stderr, "serve: %s; retrying in %lld ms\n", accepted.error().message().c_str(),
                     static_cast<long long>(backoff.count()));
        std::this_thread::sleep_for(backoff);
    }
}
