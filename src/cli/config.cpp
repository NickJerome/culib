// 配置文件：ini 子集的解析。

#include <culib/config.h>

#include <culib/lexical_cast.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

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

typedef std::vector<std::pair<std::string, std::string> > entry_list;

// 从 at 起切出一行，at 移到下一行的开头。没有行了返回 false。最后一行可以没有换行符。
bool next_line(string_view text, std::size_t& at, string_view& line) {
    if (at > text.size()) {
        return false;
    }
    std::size_t end = at;
    while (end < text.size() && text[end] != '\n') {
        ++end;
    }
    line = text.substr(at, end - at);
    at = end + 1;
    return true;
}

// [段名]，段名去掉两边空白以后不能是空的。
result<void> read_section(string_view line, std::size_t row, std::string& section) {
    if (line[line.size() - 1] != ']') {
        return malformed(row, "段名少了右方括号");
    }
    const string_view name = trimmed(line.substr(1, line.size() - 2));
    if (name.empty()) {
        return malformed(row, "段名是空的");
    }
    section = name.to_string();
    return result<void>();
}

// 段里的键加上段名前缀，变成扁平的 section.key。
std::string full_key(const std::string& section, string_view name) {
    std::string key;
    if (!section.empty()) {
        key = section;
        key += '.';
    }
    key.append(name.data(), name.size());
    return key;
}

// 重复的键覆盖旧值，位置留在第一次出现的地方。
void store(entry_list& entries, const std::string& key, string_view value) {
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].first == key) {
            entries[i].second.assign(value.data(), value.size());
            return;
        }
    }
    entries.push_back(std::make_pair(key, value.to_string()));
}

// 键 = 值
result<void> read_entry(string_view line, std::size_t row, const std::string& section, entry_list& entries) {
    const std::size_t equals = line.find('=');
    if (equals == string_view::npos) {
        return malformed(row, "既不是注释也不是段，又没有等号");
    }
    const string_view name = trimmed(line.substr(0, equals));
    if (name.empty()) {
        return malformed(row, "等号左边是空的");
    }
    bool bad_value = false;
    const string_view value = value_of(line.substr(equals + 1), bad_value);
    if (bad_value) {
        return malformed(row, "引号没闭上，或者闭引号后面还有东西");
    }
    store(entries, full_key(section, name), value);
    return result<void>();
}

}  // namespace

result<config> config::parse(string_view text) {
    config built;
    std::string section;
    std::size_t row = 0;
    std::size_t at = 0;
    string_view raw;
    while (next_line(text, at, raw)) {
        ++row;
        const string_view line = trimmed(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        const result<void> read = line[0] == '[' ? read_section(line, row, section)
                                                 : read_entry(line, row, section, built.entries_);
        if (read.is_err()) {
            return read.error();
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
