#include <culib/result.h>
#include <cstdio>
#include <utility>

wavvix::result<int> checked_port(int port) {
    if (port < 1 || port > 65535) {
        return wavvix::make_error(wavvix::error_kind::out_of_range, "port must be between 1 and 65535");
    }
    return port;
}

wavvix::result<void> validate_name(const std::string& name) {
    if (name.empty()) {
        return wavvix::make_error(wavvix::error_kind::invalid_argument, "name is empty");
    }
    return {};
}

// 自己不处理错误、原样交给调用方时，就手写 if 把错误移出去。
// 每一处 return 都看得见，这是故意的——不包成宏。
wavvix::result<std::string> describe(const std::string& name, int port) {
    wavvix::result<void> named = validate_name(name);
    if (named.is_err()) {
        return std::move(named).error();
    }
    wavvix::result<int> checked = checked_port(port);
    if (checked.is_err()) {
        return std::move(checked).error();
    }
    return name + ":" + std::to_string(checked.value());
}

int main() {
    auto port = checked_port(8080);
    if (port.is_err()) {
        std::fprintf(stderr, "%s\n", port.error().to_string().c_str());
        return 1;
    }
    std::printf("port: %d\n", port.value());
    std::printf("fallback: %d\n", checked_port(-1).value_or(8080));
    auto name = validate_name("");
    if (name.is_err()) std::printf("%s\n", name.error().to_string().c_str());

    std::printf("%s\n", describe("server", 443).value().c_str());
    std::printf("%s\n", describe("server", 70000).error().to_string().c_str());
    std::printf("%s\n", describe("", 443).error().to_string().c_str());
    return 0;
}
