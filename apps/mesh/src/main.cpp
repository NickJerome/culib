#include <mesh/crypto.h>
#include <mesh/node.h>
#include <mesh/signaling.h>

#include <sodium.h>

#include <culib/coroutine.h>
#include <culib/os.h>
#include <culib/scheduler.h>

#include <cstdio>
#include <cstddef>
#include <cwchar>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <culib/strings.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace {

typedef std::chrono::seconds seconds;

bool parse_port(const std::string& text, std::uint16_t& port) {
    if (text.empty()) return false;
    unsigned value = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const unsigned digit = static_cast<unsigned>(text[i] - '0');
        if (value > 6553 || (value == 6553 && digit > 5)) return false;
        value = value * 10 + digit;
    }
    port = static_cast<std::uint16_t>(value);
    return port != 0;
}

int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parse_public_key(const std::string& text, mesh::public_key& key) {
    if (text.size() != key.size() * 2) return false;
    for (std::size_t i = 0; i < key.size(); ++i) {
        const int high = hex_digit(text[i * 2]);
        const int low = hex_digit(text[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        key[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

void print_error(const wavvix::error& error) {
    std::fprintf(stderr, "%s\n", error.to_string().c_str());
}

wavvix::result<void> read_password(std::string& password);

void clear_password(std::string& password) noexcept {
    if (!password.empty()) sodium_memzero(&password[0], password.size());
    password.clear();
}

int run_relay(const std::vector<std::string>& args, bool persistent) {
    std::uint16_t port = 0;
    if (args.size() != 3 || !parse_port(args[2], port)) return 2;
    wavvix::result<wavvix::net::endpoint> address =
        wavvix::net::endpoint::parse("0.0.0.0", port);
    if (address.is_err()) { print_error(address.error()); return 1; }
    wavvix::result<wavvix::net::tcp_listener> opened =
        wavvix::net::tcp_listener::listen(address.value());
    if (opened.is_err()) { print_error(opened.error()); return 1; }
    wavvix::net::tcp_listener listener = std::move(opened).value();
    if (persistent) {
        wavvix::result<void> handler = wavvix::os::install_stop_handler();
        if (handler.is_err()) { print_error(handler.error()); return 1; }
    }
    bool succeeded = false;
    {
        wavvix::scheduler_scope runtime;
        wavvix::coroutine server([&] {
            do {
                wavvix::result<void> relayed = mesh::relay_offer_pair(
                    listener, persistent ? seconds(5) : seconds(60));
                if (relayed.is_err()) {
                    if (!persistent || relayed.error().kind() != wavvix::error_kind::timeout)
                        print_error(relayed.error());
                } else succeeded = true;
            } while (persistent && !wavvix::os::stop_requested());
        });
        server.join();
    }
    return (succeeded || (persistent && wavvix::os::stop_requested())) ? 0 : 1;
}

int run_peer(const std::vector<std::string>& args, bool tunnel) {
    if (args.size() != 6 && args.size() != 8 && args.size() != 9) return 2;
    mesh::public_key peer_key = {};
    std::uint16_t relay_port = 0;
    std::uint16_t stun_port = 0;
    std::uint16_t local_port = 0;
    if (!parse_public_key(args[3], peer_key) || !parse_port(args[5], relay_port) ||
        (args.size() >= 8 && !parse_port(args[7], stun_port)) ||
        (args.size() == 9 && !parse_port(args[8], local_port))) return 2;
    wavvix::result<wavvix::net::endpoint> relay =
        wavvix::net::endpoint::parse(args[4], relay_port);
    if (relay.is_err()) { print_error(relay.error()); return 1; }
    std::string password;
    wavvix::result<void> read = read_password(password);
    if (read.is_err()) {
        clear_password(password);
        print_error(read.error());
        return 1;
    }
    wavvix::result<mesh::identity> loaded =
        mesh::load_identity_file(args[2], password);
    clear_password(password);
    if (loaded.is_err()) { print_error(loaded.error()); return 1; }
    mesh::identity local = std::move(loaded).value();

    wavvix::net::virtual_interface device;
    if (tunnel) {
        wavvix::result<void> handler = wavvix::os::install_stop_handler();
        if (handler.is_err()) { print_error(handler.error()); return 1; }
        wavvix::result<wavvix::net::virtual_interface> opened_device =
            wavvix::net::virtual_interface::open();
        if (opened_device.is_err()) { print_error(opened_device.error()); return 1; }
        device = std::move(opened_device).value();
        const wavvix::string_view name = device.name();
        std::printf("virtual interface: %.*s\n",
                    static_cast<int>(name.size()), name.data());
        std::fflush(stdout);
    }

    wavvix::result<wavvix::net::endpoint> bind_address =
        wavvix::net::endpoint::parse("0.0.0.0", local_port);
    if (bind_address.is_err()) { print_error(bind_address.error()); return 1; }
    wavvix::result<wavvix::net::udp_socket> opened =
        wavvix::net::udp_socket::bind(bind_address.value());
    if (opened.is_err()) { print_error(opened.error()); return 1; }
    wavvix::net::udp_socket socket = std::move(opened).value();
    bool succeeded = false;
    {
        wavvix::scheduler_scope runtime;
        wavvix::coroutine worker([&] {
            do {
                wavvix::result<std::vector<wavvix::net::endpoint>> listed =
                    wavvix::net::local_candidates(socket);
                if (listed.is_err()) {
                    print_error(listed.error());
                    if (!tunnel) return;
                    wavvix::sleep_for(seconds(1));
                    continue;
                }
                std::vector<wavvix::net::endpoint> candidates =
                    std::move(listed).value();
                if (candidates.size() > wavvix::net::max_punch_offer_candidates)
                    candidates.erase(candidates.begin() +
                        static_cast<std::ptrdiff_t>(
                            wavvix::net::max_punch_offer_candidates),
                        candidates.end());
                if (args.size() >= 8) {
                    wavvix::result<wavvix::net::endpoint> stun =
                        wavvix::net::endpoint::parse(args[6], stun_port);
                    if (stun.is_err()) { print_error(stun.error()); return; }
                    wavvix::result<wavvix::net::endpoint> reflected =
                        wavvix::net::stun_public_endpoint(socket, stun.value(), seconds(3));
                    if (reflected.is_err()) {
                        print_error(reflected.error());
                    } else {
                        std::fprintf(stderr, "STUN public endpoint: %s\n",
                                     reflected.value().to_string().c_str());
                        if (candidates.size() == wavvix::net::max_punch_offer_candidates)
                            candidates.pop_back();
                        candidates.push_back(reflected.value());
                    }
                }
                wavvix::result<mesh::peer_connection> connection =
                    mesh::connect_peer(local, peer_key, socket, candidates,
                                       relay.value(), seconds(10));
                if (connection.is_err()) {
                    print_error(connection.error());
                    if (!tunnel) return;
                    wavvix::sleep_for(seconds(1));
                    continue;
                }
                std::printf("authenticated UDP path: %s\n",
                            connection.value().peer.to_string().c_str());
                std::fflush(stdout);
                if (!tunnel) {
                    wavvix::result<void> probed = mesh::probe_peer(
                        connection.value(), socket, seconds(10));
                    if (probed.is_err()) { print_error(probed.error()); return; }
                    std::puts("encrypted UDP probe: ok");
                    succeeded = true;
                    return;
                }
                succeeded = true;
                wavvix::result<void> forwarded =
                    mesh::run_tunnel(connection.value(), socket, device);
                if (forwarded.is_err()) print_error(forwarded.error());
                if (!wavvix::os::stop_requested()) wavvix::sleep_for(seconds(1));
            } while (tunnel && !wavvix::os::stop_requested());
        });
        worker.join();
    }
    return succeeded ? 0 : 1;
}

wavvix::result<void> read_password(std::string& password) {
#ifdef _WIN32
    HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && ::GetConsoleMode(input, &mode)) {
        std::fputs("Passphrase: ", stderr);
        std::fflush(stderr);
        if (!::SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT))
            return wavvix::make_error(wavvix::error_kind::io,
                                      "cannot disable console echo");
        wchar_t characters[4096] = {};
        DWORD count = 0;
        const BOOL read = ::ReadConsoleW(input, characters, 4095, &count, 0);
        const BOOL restored = ::SetConsoleMode(input, mode);
        std::fputc('\n', stderr);
        if (!read || !restored) {
            ::SecureZeroMemory(characters, sizeof(characters));
            return wavvix::make_error(wavvix::error_kind::io,
                                      "cannot read console passphrase");
        }
        if (count == 4095 && characters[count - 1] != L'\n') {
            ::SecureZeroMemory(characters, sizeof(characters));
            return wavvix::make_error(wavvix::error_kind::out_of_range,
                                      "console passphrase is too long");
        }
        while (count != 0 && (characters[count - 1] == L'\r' ||
                              characters[count - 1] == L'\n')) --count;
        if (count != 0) {
            const int length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                characters, static_cast<int>(count), 0, 0, 0, 0);
            if (length > 0) {
                password.resize(static_cast<std::size_t>(length));
                if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                    characters, static_cast<int>(count), &password[0], length, 0, 0) != length) {
                    clear_password(password);
                }
            }
        }
        ::SecureZeroMemory(characters, sizeof(characters));
        if (count != 0 && password.empty())
            return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                      "invalid Unicode passphrase");
        return wavvix::result<void>();
    }
#else
    if (::isatty(STDIN_FILENO)) {
        struct termios original;
        if (::tcgetattr(STDIN_FILENO, &original) != 0)
            return wavvix::make_error(wavvix::error_kind::io,
                                      "cannot read terminal mode");
        struct termios hidden = original;
        hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
        std::fputs("Passphrase: ", stderr);
        std::fflush(stderr);
        if (::tcsetattr(STDIN_FILENO, TCSANOW, &hidden) != 0)
            return wavvix::make_error(wavvix::error_kind::io,
                                      "cannot disable terminal echo");
        const bool read = static_cast<bool>(std::getline(std::cin, password));
        const int restored = ::tcsetattr(STDIN_FILENO, TCSANOW, &original);
        std::fputc('\n', stderr);
        if (!read || restored != 0) {
            clear_password(password);
            return wavvix::make_error(wavvix::error_kind::io,
                                      "cannot read terminal passphrase");
        }
        return wavvix::result<void>();
    }
#endif
    if (!std::getline(std::cin, password))
        return wavvix::make_error(wavvix::error_kind::io,
                                  "cannot read passphrase from stdin");
    return wavvix::result<void>();
}

int run(const std::vector<std::string>& args) {
    if (args.size() >= 2 && (args[1] == "relay" || args[1] == "serve"))
        return run_relay(args, args[1] == "serve");
    if (args.size() >= 2 && (args[1] == "peer" || args[1] == "tunnel"))
        return run_peer(args, args[1] == "tunnel");
    if (args.size() != 4 || args[1] != "identity" ||
        (args[2] != "create" && args[2] != "show")) {
        std::fputs("usage: mesh identity create|show PATH\n"
                   "       mesh relay|serve PORT\n"
                   "       mesh peer|tunnel IDENTITY_PATH PEER_PUBLIC_KEY RELAY_IP RELAY_PORT "
                   "[STUN_IP STUN_PORT [LOCAL_UDP_PORT]]\n"
                   "passphrase is read from the terminal or stdin\n", stderr);
        return 2;
    }
    std::string password;
    wavvix::result<void> read = read_password(password);
    if (read.is_err()) {
        clear_password(password);
        std::fprintf(stderr, "%s\n", read.error().to_string().c_str());
        return 1;
    }
    wavvix::result<mesh::identity> identity =
        args[2] == "create"
            ? mesh::create_identity_file(args[3], password)
            : mesh::load_identity_file(args[3], password);
    clear_password(password);
    if (identity.is_err()) {
        std::fprintf(stderr, "%s\n", identity.error().to_string().c_str());
        return 1;
    }
    static const char hex[] = "0123456789abcdef";
    const mesh::public_key& key = identity.value().public_part();
    for (std::size_t i = 0; i < key.size(); ++i) {
        std::putchar(hex[key[i] >> 4]);
        std::putchar(hex[key[i] & 15]);
    }
    std::putchar('\n');
    return 0;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        wavvix::result<std::string> converted =
            wavvix::wide_to_utf8(argv[i], std::wcslen(argv[i]));
        if (converted.is_err()) {
            std::fprintf(stderr, "%s\n", converted.error().to_string().c_str());
            return 2;
        }
        args.push_back(std::move(converted).value());
    }
    return run(args);
}
#else
int main(int argc, char* argv[]) {
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
