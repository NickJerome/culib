// 配置文件：ini 子集的解析。

#include <culib/config.h>

#include <culib/lexical_cast.h>

#include <cstddef>
#include <utility>

namespace wavvix {

namespace {

bool blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

string_view trimmed(string_view text) {
    std::size_t from = 0;
    std::size_t to = text.size();
    while (from < to && blank(text[from])) {
        ++from;
    }
    while (to > from && blank(text[to - 1])) {
        --to;
    }
    return text.substr(from, to - from);
}

// 提取值并去掉行尾注释；引号未闭合或其后有非注释内容时置 bad。
string_view value_of(string_view text, bool& bad) {
    const string_view body = trimmed(text);

    if (!body.empty() && body[0] == '"') {
        const std::size_t closing = body.find('"', 1);
        if (closing == string_view::npos) {
            bad = true;    // 引号没闭上
            return string_view();
        }
        // 闭引号后只允许空白或注释。
        const string_view rest = trimmed(body.substr(closing + 1));
        if (!rest.empty() && rest[0] != '#' && rest[0] != ';') {
            bad = true;
            return string_view();
        }
        return body.substr(1, closing - 1);
    }

    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '#' && body[i] != ';') {
            continue;
        }
        if (i == 0 || blank(body[i - 1])) {
            return trimmed(body.substr(0, i));
        }
    }
    return body;
}

error malformed(std::size_t row, string_view why) {
    return make_error(error_kind::invalid_argument,
                      "配置第 " + to_string(static_cast<long long>(row)) + " 行：" +
                          why.to_string());
}

}  // namespace

result<config> config::parse(string_view text) {
    config built;
    std::string section;
    std::size_t row = 0;
    std::size_t at = 0;

    while (at <= text.size()) {
        // 切出一行（最后一行可能没有换行符）
        std::size_t end = at;
        while (end < text.size() && text[end] != '\n') {
            ++end;
        }
        const string_view line = trimmed(text.substr(at, end - at));
        at = end + 1;
        ++row;

        if (line.empty() || line[0] == '#' || line[0] == ';') {
            if (end >= text.size()) {
                break;
            }
            continue;
        }

        if (line[0] == '[') {
            if (line[line.size() - 1] != ']') {
                return result<config>(malformed(row, "段名少了右方括号"));
            }
            const string_view name = trimmed(line.substr(1, line.size() - 2));
            if (name.empty()) {
                return result<config>(malformed(row, "段名是空的"));
            }
            section = name.to_string();
            if (end >= text.size()) {
                break;
            }
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == string_view::npos) {
            return result<config>(malformed(row, "既不是注释也不是段，又没有等号"));
        }
        const string_view name = trimmed(line.substr(0, equals));
        if (name.empty()) {
            return result<config>(malformed(row, "等号左边是空的"));
        }

        bool bad_value = false;
        const string_view value = value_of(line.substr(equals + 1), bad_value);
        if (bad_value) {
            return result<config>(malformed(row, "引号没闭上，或者闭引号后面还有东西"));
        }

        // 段里的键加前缀，变成扁平的 section.key
        std::string key;
        if (!section.empty()) {
            key = section;
            key += '.';
        }
        key.append(name.data(), name.size());

        // 重复键覆盖旧值，保持首次出现的位置。
        bool replaced = false;
        for (std::size_t i = 0; i < built.entries_.size(); ++i) {
            if (built.entries_[i].first == key) {
                built.entries_[i].second.assign(value.data(), value.size());
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            built.entries_.push_back(
                std::make_pair(key, std::string(value.data(), value.size())));
        }

        if (end >= text.size()) {
            break;
        }
    }

    return result<config>(std::move(built));
}

const std::string* config::find(string_view key) const {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].first.size() == key.size() &&
            string_view(entries_[i].first.data(), entries_[i].first.size()) == key) {
            return &entries_[i].second;
        }
    }
    return 0;
}

optional<string_view> config::get(string_view key) const {
    const std::string* const found = find(key);
    if (!found) {
        return optional<string_view>();
    }
    return optional<string_view>(string_view(found->data(), found->size()));
}

std::vector<string_view> config::keys() const {
    std::vector<string_view> all;
    all.reserve(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        all.push_back(string_view(entries_[i].first.data(), entries_[i].first.size()));
    }
    return all;
}

result<std::string> config::get_string(string_view key, string_view fallback) const {
    const std::string* const found = find(key);
    if (!found) {
        return result<std::string>(fallback.to_string());
    }
    return result<std::string>(*found);
}

namespace {

// 缺失时用默认值；转换失败时补上键名与原始值。
template <typename T>
result<T> typed(const std::string* found, string_view key, T fallback) {
    if (!found) {
        return result<T>(fallback);
    }
    const string_view text(found->data(), found->size());
    const result<T> parsed = lexical_cast<T>(text);
    if (parsed.is_ok()) {
        return parsed;
    }
    return result<T>(make_error(error_kind::invalid_argument,
                                "配置项 " + key.to_string() + " 的值 \"" + text.to_string() +
                                    "\" 解析不了：" + parsed.error().message()));
}

}  // namespace

result<long long> config::get_int64(string_view key, long long fallback) const {
    return typed<long long>(find(key), key, fallback);
}

result<double> config::get_double(string_view key, double fallback) const {
    return typed<double>(find(key), key, fallback);
}

result<bool> config::get_bool(string_view key, bool fallback) const {
    return typed<bool>(find(key), key, fallback);
}


}  // namespace wavvix
