#include <mesh/crypto.h>

#include <culib/fs.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

struct remove_on_exit {
    explicit remove_on_exit(const std::string& name) : path(name) {}
    ~remove_on_exit() { (void)wavvix::fs::remove_file(path); }
    std::string path;
};

}  // namespace

int main() {
    wavvix::result<std::string> directory = wavvix::fs::temp_directory();
    wavvix::result<wavvix::net::punch_session_id> random =
        wavvix::net::random_punch_session_id();
    if (directory.is_err() || random.is_err()) return fail("temporary path unavailable");
    static const char digits[] = "0123456789abcdef";
    std::string suffix;
    for (std::size_t i = 0; i < random.value().size(); ++i) {
        const unsigned char byte = random.value()[i];
        suffix += digits[byte >> 4];
        suffix += digits[byte & 15];
    }
    const std::string path = wavvix::fs::join(
        directory.value(), std::string("mesh-身份-") + suffix + ".key");
    remove_on_exit cleanup(path);

    wavvix::result<mesh::identity> created =
        mesh::create_identity_file(path, "correct horse battery staple");
    if (created.is_err()) {
        std::fprintf(stderr, "create: %s\n", created.error().to_string().c_str());
        return 1;
    }
    const mesh::public_key first_public = created.value().public_part();
    wavvix::result<mesh::identity> duplicate =
        mesh::create_identity_file(path, "another passphrase");
    if (duplicate.is_ok() || duplicate.error().kind() != wavvix::error_kind::already_exists)
        return fail("existing identity was overwritten");

    wavvix::result<mesh::identity> loaded =
        mesh::load_identity_file(path, "correct horse battery staple");
    if (loaded.is_err() || loaded.value().public_part() != first_public)
        return fail("identity changed across save and load");
#ifndef _WIN32
    struct stat info;
    if (::stat(path.c_str(), &info) != 0 || (info.st_mode & 077) != 0)
        return fail("identity file has group or world permissions");
    const std::string link_path = path + ".link";
    remove_on_exit link_cleanup(link_path);
    if (::symlink(path.c_str(), link_path.c_str()) != 0)
        return fail("identity symlink setup failed");
    if (mesh::load_identity_file(link_path, "correct horse battery staple").is_ok())
        return fail("identity symlink was followed");
#endif
    if (mesh::load_identity_file(path, "wrong passphrase").is_ok())
        return fail("wrong passphrase unlocked identity");
    wavvix::result<std::string> bytes = wavvix::fs::read_file(path);
    if (bytes.is_err() || bytes.value().size() != 124 ||
        std::memcmp(bytes.value().data(), "CMI1", 4) != 0)
        return fail("identity file has the wrong format or length");
    bytes.value().back() ^= 1;
    if (wavvix::fs::write_file(path, bytes.value()).is_err())
        return fail("identity tamper setup failed");
    if (mesh::load_identity_file(path, "correct horse battery staple").is_ok())
        return fail("tampered identity file was accepted");

    std::printf("mesh encrypted identity storage passed\n");
    return 0;
}
