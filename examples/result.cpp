#include <culib/result.h>
#include <cstdio>

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
    return 0;
}
