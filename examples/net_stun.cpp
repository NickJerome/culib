#include <culib/coroutine.h>
#include <culib/lexical_cast.h>
#include <culib/net.h>
#include <culib/scheduler.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::printf("usage: net_stun <numeric-stun-server-ip> [port]\n");
        return 0;
    }
    unsigned short port = 3478;
    if (argc >= 3) {
        const wavvix::result<unsigned short> parsed =
            wavvix::lexical_cast<unsigned short>(argv[2]);
        if (parsed.is_err() || parsed.value() == 0) return 1;
        port = parsed.value();
    }
    const wavvix::result<wavvix::net::endpoint> server =
        wavvix::net::endpoint::parse(argv[1], port);
    if (server.is_err()) return 1;
    const char* local_ip = server.value().family() == wavvix::net::address_family::ipv4
        ? "0.0.0.0" : "::";
    const wavvix::result<wavvix::net::endpoint> local =
        wavvix::net::endpoint::parse(local_ip, 0);
    if (local.is_err()) return 1;
    wavvix::result<wavvix::net::udp_socket> opened =
        wavvix::net::udp_socket::bind(local.value());
    if (opened.is_err()) return 1;
    wavvix::net::udp_socket socket = std::move(opened).value();

    wavvix::scheduler_scope runtime;
    bool ok = false;
    wavvix::coroutine query([&] {
        const wavvix::result<wavvix::net::endpoint> observed =
            wavvix::net::stun_public_endpoint(socket, server.value(),
                                              std::chrono::seconds(3));
        if (observed.is_err()) {
            std::fprintf(stderr, "%s\n", observed.error().to_string().c_str());
            return;
        }
        std::printf("public endpoint: %s\n", observed.value().to_string().c_str());
        ok = true;
    });
    query.join();
    return ok ? 0 : 1;
}
