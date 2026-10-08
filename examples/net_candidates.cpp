#include <culib/net.h>

#include <cstdio>
#include <utility>
#include <vector>

int main() {
    namespace net = wavvix::net;
    const wavvix::result<net::endpoint> wildcard =
        net::endpoint::parse("0.0.0.0", 0);
    if (wildcard.is_err()) return 1;
    wavvix::result<net::udp_socket> opened = net::udp_socket::bind(wildcard.value());
    if (opened.is_err()) return 1;
    net::udp_socket socket = std::move(opened).value();
    const wavvix::result<std::vector<net::endpoint>> candidates =
        net::local_candidates(socket);
    if (candidates.is_err()) {
        std::fprintf(stderr, "%s\n", candidates.error().to_string().c_str());
        return 1;
    }
    for (std::size_t i = 0; i < candidates.value().size(); ++i)
        std::printf("%s\n", candidates.value()[i].to_string().c_str());
    return 0;
}
