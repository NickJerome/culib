// 地址解析是同步操作；服务器可在启动协程调度器之前做。
//     xmake run example_net_address

#include <culib/net.h>

#include <cstdio>
#include <vector>

int main() {
    const wavvix::result<wavvix::net::endpoint> loopback =
        wavvix::net::endpoint::parse("127.0.0.1", 8080);
    if (loopback.is_err()) {
        std::fprintf(stderr, "%s\n", loopback.error().to_string().c_str());
        return 1;
    }
    std::printf("数字地址：%s\n", loopback.value().to_string().c_str());

    const wavvix::result<std::vector<wavvix::net::endpoint>> found =
        wavvix::net::resolve("localhost", 8080);
    if (found.is_err()) {
        std::fprintf(stderr, "%s\n", found.error().to_string().c_str());
        return 1;
    }
    for (std::size_t i = 0; i < found.value().size(); ++i)
        std::printf("候选地址：%s\n", found.value()[i].to_string().c_str());
    return 0;
}
