#include <culib/config.h>
#include <culib/fs.h>

namespace wavvix {

result<config> config::load(string_view path) {
    result<std::string> text = fs::read_file(path);
    if (!text.is_ok()) return result<config>(std::move(text).error());
    result<config> parsed = parse(text.value());
    if (!parsed.is_ok()) {
        // 只有文件层知道路径，解析器只报告行号。
        return result<config>(make_error(parsed.error().kind(),
                                         path.to_string() + " " + parsed.error().message()));
    }
    return parsed;
}

} // namespace wavvix
