#include <cu/fs.h>
#include <cu/test.h>

#include <string>

#include "support.h"

namespace {

std::string make_content(std::size_t size) {
    std::string content(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        content[i] = static_cast<char>(i * 7 + i / 251);
    }
    return content;
}

}  // namespace

TEST(fs, reads_whole_files_of_any_size) {
    const std::size_t sizes[] = {0, 1, 511, 512, 513, 4096, 100000};
    const std::string path = scratch_name("cu_fs_sizes");
    for (std::size_t size : sizes) {
        const std::string content = make_content(size);
        EXPECT(write_scratch_file(path, content), ==, true);
        cu::result<std::string> read = cu::fs::read_file(path);
        EXPECT(error_text(read), ==, std::string());
        if (read.is_ok()) {
            EXPECT(read.value().size(), ==, size);
            EXPECT(read.value() == content, ==, true);
        }
    }
    remove_scratch_file(path);
}

TEST(fs, reads_a_file_with_a_utf8_name) {
    const std::string path = scratch_name("cu_fs_\xe6\x96\x87\xe4\xbb\xb6");   // 「文件」
    EXPECT(write_scratch_file(path, "hello"), ==, true);
    cu::result<std::string> read = cu::fs::read_file(path);
    EXPECT(error_text(read), ==, std::string());
    EXPECT(read.value_or(std::string()), ==, std::string("hello"));
    remove_scratch_file(path);
}

TEST(fs, reports_a_missing_file) {
    const std::string path = scratch_name("cu_fs_missing");
    cu::result<std::string> read = cu::fs::read_file(path);
    EXPECT(read.is_err(), ==, true);
    if (read.is_err()) {
        const std::string prefix = "open " + path + ": ";
        EXPECT(read.error().kind(), ==, cu::error_kind::not_found);
        EXPECT(read.error().message().substr(0, prefix.size()), ==, prefix);
    }
}

TEST(fs, reports_a_directory) {
    cu::result<std::string> read = cu::fs::read_file(".");
    EXPECT(error_text(read), ==, std::string("unsupported: open .: is a directory"));
}

TEST(fs, rejects_a_path_with_a_nul_byte) {
    cu::result<std::string> read = cu::fs::read_file(cu::string_view("a\0b", 3));
    EXPECT(read.is_err(), ==, true);
    if (read.is_err()) {
        EXPECT(read.error().kind(), ==, cu::error_kind::invalid_argument);
    }
}
