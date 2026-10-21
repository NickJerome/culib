// serve 的第一步：每个请求都回同一个文件，回完就关连接。
//   serve <address> <file>，例如 serve :8080 index.html

#include <cu/coroutine.h>
#include <cu/fs.h>
#include <cu/net.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <utility>

namespace {

// 读到请求头结束（一个空行）就回文件。请求头超过 64 KiB、读失败或者对端先关了，直接关连接。
void handle(cu::net::connection connection, const std::string& response) {
    std::string head;
    char buffer[4096];
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (head.size() > 64 * 1024) {
            return;
        }
        cu::result<std::size_t> got = connection.read(buffer, sizeof buffer);
        if (got.is_err() || got.value() == 0) {
            return;
        }
        head.append(buffer, got.value());
    }
    cu::result<void> sent = connection.write(response.data(), response.size());
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
