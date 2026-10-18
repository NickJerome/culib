// TCP 回显：服务端和客户端各是一根协程。
//     xmake run example_net_echo

#include <culib/coroutine.h>
#include <culib/net.h>

#include <chrono>
#include <cstdio>
#include <string>

int main() {
    namespace net = wavvix::net;
    typedef std::chrono::seconds seconds;

    net::socket listener;
    if (listener.bind("127.0.0.1", 0).is_err() || listener.listen().is_err()) {
        return 1;
    }
    const wavvix::result<net::endpoint> local = listener.local_endpoint();
    if (local.is_err()) {
        return 1;
    }

    std::string answer;
    bool echoed = false;
    wavvix::wait_group group;
    group.go([&] {
        wavvix::result<net::socket> accepted = listener.accept(seconds(10));
        if (accepted.is_err()) {
            return;
        }
        char buffer[5];
        if (accepted.value().read_exactly(buffer, sizeof buffer, seconds(10)).is_err()) {
            return;
        }
        echoed = accepted.value().write(buffer, sizeof buffer, seconds(10)).is_ok();
    });
    group.go([&] {
        net::socket stream;
        if (stream.connect("localhost", local.value().port(), seconds(10)).is_err()) {
            return;
        }
        if (stream.write("hello", 5, seconds(10)).is_err()) {
            return;
        }
        char buffer[5];
        if (stream.read_exactly(buffer, sizeof buffer, seconds(10)).is_err()) {
            return;
        }
        answer.assign(buffer, sizeof buffer);
    });
    group.wait();

    std::printf("echo: %s\n", answer.c_str());
    return echoed && answer == "hello" ? 0 : 1;
}
