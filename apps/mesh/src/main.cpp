#include <mesh/crypto.h>

#include <sodium.h>

#include <cstdio>
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

void clear_password(std::string& password) noexcept {
    if (!password.empty()) sodium_memzero(&password[0], password.size());
    password.clear();
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
    if (args.size() != 4 || args[1] != "identity" ||
        (args[2] != "create" && args[2] != "show")) {
        std::fputs("usage: mesh identity create|show PATH\n"
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
