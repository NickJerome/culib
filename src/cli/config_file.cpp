#include <culib/config.h>
#include <culib/fs.h>

#include <cstddef>
#include <string>
#include <utility>

namespace wavvix {

namespace {

// 整块整块地读进来。open / read / close 的错误直接往上传：fs::file 的失败消息
// 自带打开时的路径，「哪个文件读不了」在错误里就有了。
result<void> read_entire_file(const fs::path& target, std::string& out) {
    result<fs::file> opened = fs::open(target, fs::open_mode::read);
    if (!opened.is_ok()) {
        return result<void>(std::move(opened).error());
    }

    fs::file handle = std::move(opened).value();
    char chunk[4096];
    for (;;) {
        const result<std::size_t> got = handle.read(chunk, sizeof chunk);
        if (!got.is_ok()) {
            return result<void>(std::move(got).error());
        }
        if (got.value() == 0) {
            break;
        }
        out.append(chunk, got.value());
    }
    return handle.close();
}

}  // namespace

result<config> config::load(string_view path) {
    std::string text;
    const result<void> read = read_entire_file(fs::path(path), text);
    if (!read.is_ok()) {
        return result<config>(std::move(read).error());
    }

    result<config> parsed = parse(text);
    if (!parsed.is_ok()) {
        // 只有文件层知道路径，解析器只报告行号。
        return result<config>(make_error(parsed.error().kind(),
                                         path.to_string() + " " + parsed.error().message()));
    }
    return parsed;
}

} // namespace wavvix
