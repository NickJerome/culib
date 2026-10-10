#include <culib/flag.h>

#include <culib/fs.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace wavvix {
namespace flag {

const required_t required = required_t();

namespace {

struct entry {
    detail::kind which;
    void* storage;
    const char* name;
    const char* help;
    std::vector<const char*> aliases;
    bool is_required;
    std::string default_text;
    bool on_command_line;
};

struct registry {
    std::vector<entry> entries;
    std::vector<string_view> positional;
    std::string program;
    std::string version;
    bool parsed;
    bool help_requested;
    bool version_requested;

    registry() : parsed(false), help_requested(false), version_requested(false) {}
};

registry& table() {
    static registry instance;
    return instance;
}

struct parse_attempt {
    explicit parse_attempt(registry& value) : state(value), complete(false) {
        state.parsed = false;
        state.positional.clear();
        state.help_requested = false;
        state.version_requested = false;
    }

    ~parse_attempt() {
        if (complete) return;
        state.positional.clear();
        state.help_requested = false;
        state.version_requested = false;
        for (std::size_t i = 0; i < state.entries.size(); ++i)
            state.entries[i].on_command_line = false;
    }

    registry& state;
    bool complete;
};

entry* find_by_name(registry& self, string_view name) {
    for (std::size_t i = 0; i < self.entries.size(); ++i) {
        if (name == self.entries[i].name) return &self.entries[i];
        for (std::size_t j = 0; j < self.entries[i].aliases.size(); ++j) {
            if (name == self.entries[i].aliases[j]) return &self.entries[i];
        }
    }
    return 0;
}

void append_spelling(std::string& out, const char* name) {
    out += name[1] == '\0' ? "-" : "--";
    out += name;
}

std::string spelling(const char* name) {
    std::string out;
    append_spelling(out, name);
    return out;
}

error prefixed(const entry& which, const error& cause) {
    return make_error(cause.kind(), spelling(which.name) + "：" + cause.message());
}

template <typename T>
result<void> assign_as(entry& which, string_view text) {
    result<T> value = lexical_cast<T>(text);
    if (value.is_err()) return prefixed(which, value.error());
    *static_cast<T*>(which.storage) = value.value();
    return result<void>();
}

result<void> assign(entry& which, string_view text) {
    switch (which.which) {
        case detail::kind::boolean:          return assign_as<bool>(which, text);
        case detail::kind::signed_integer:   return assign_as<long long>(which, text);
        case detail::kind::unsigned_integer: return assign_as<unsigned long long>(which, text);
        case detail::kind::number:           return assign_as<double>(which, text);
        case detail::kind::text:
            // 字符串不需要转换，直接写进去
            static_cast<std::string*>(which.storage)->assign(text.data(), text.size());
            return result<void>();
    }
    return make_error(error_kind::unknown, spelling(which.name) + " 的类型标签非法");
}

const char* type_label(detail::kind which) {
    switch (which) {
        case detail::kind::boolean: return "";          // 布尔选项不带值，不显示类型
        case detail::kind::signed_integer: return "<int>";
        case detail::kind::unsigned_integer: return "<uint>";
        case detail::kind::number: return "<num>";
        case detail::kind::text: return "<str>";
    }
    return "";
}

result<void> apply_defaults(registry& self) {
    for (std::size_t i = 0; i < self.entries.size(); ++i) {
        self.entries[i].on_command_line = false;
        if (self.entries[i].is_required) continue;
        const result<void> applied = assign(self.entries[i], self.entries[i].default_text);
        if (applied.is_err()) return applied;   // 默认值自己都转换不了，是编程错误
    }
    return result<void>();
}

result<void> check_required(const registry& self) {
    if (self.help_requested || self.version_requested) return result<void>();
    for (std::size_t i = 0; i < self.entries.size(); ++i) {
        if (self.entries[i].is_required && !self.entries[i].on_command_line) {
            return make_error(error_kind::invalid_argument,
                              "缺少必填选项 " + spelling(self.entries[i].name));
        }
    }
    return result<void>();
}

bool is_option(string_view token) {
    // 单独一个 "-" 是位置参数（惯例表示标准输入）
    return token.size() >= 2 && token[0] == '-';
}

}  // namespace

namespace detail {

[[noreturn]] void reject(const std::string& why) {
    std::fprintf(stderr, "wavvix::flag: %s\n", why.c_str());
    std::fflush(stderr);
    std::abort();
}

void reject_bad_name(registry& self, const char* name, const char* owner) {
    // name == owner 时它就是主名字，否则是 owner 的别名
    const std::string whose =
        (name == owner) ? std::string() : "（" + spelling(owner) + " 的别名）";
    if (!name || name[0] == '\0') {
        reject("有一个选项的名字或别名是空的" + whose);
    }
    if (name[0] == '-') {
        reject(std::string("名字 \"") + name + "\"" + whose + " 不要带前导 '-'");
    }
    const string_view candidate(name);
    if (candidate == "help" || candidate == "h" || candidate == "version") {
        reject(std::string("\"") + name + "\"" + whose +
               " 是保留名，会被内置的 --help / --version 遮住");
    }
    if (find_by_name(self, candidate)) {
        reject(std::string("\"") + name + "\"" + whose + " 和已登记的选项重名");
    }
}

void register_flag(kind which, void* storage, const char* help,
                   std::initializer_list<const char*> names, bool is_required,
                   std::string default_text) {
    registry& self = table();
    if (names.size() == 0) reject("选项至少需要一个名字");
    const char* const name = *names.begin();
    reject_bad_name(self, name, name);
    for (std::initializer_list<const char*>::const_iterator it = names.begin() + 1;
         it != names.end(); ++it) {
        reject_bad_name(self, *it, name);
        for (std::initializer_list<const char*>::const_iterator previous = names.begin();
             previous != it; ++previous) {
            if (string_view(*previous) == *it)
                reject(std::string("\"") + *it + "\" 在同一个选项中重复");
        }
    }
    entry added;
    added.which = which;
    added.storage = storage;
    added.name = name;
    added.help = help;
    added.aliases.assign(names.begin() + 1, names.end());
    added.is_required = is_required;
    added.default_text = std::move(default_text);
    added.on_command_line = false;
    self.entries.push_back(std::move(added));
    // A new option was not part of the previous parse.
    self.parsed = false;
    self.positional.clear();
    self.help_requested = false;
    self.version_requested = false;
    for (std::size_t i = 0; i < self.entries.size(); ++i)
        self.entries[i].on_command_line = false;
}

void unregister_flag(const void* storage) noexcept {
    registry& self = table();
    for (std::size_t i = 0; i < self.entries.size(); ++i) {
        if (self.entries[i].storage == storage) {
            self.entries.erase(self.entries.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void read_before_parse(const void* storage) noexcept {
    const char* name = "?";
    registry& self = table();
    for (std::size_t i = 0; i < self.entries.size(); ++i) {
        if (self.entries[i].storage == storage) {
            name = self.entries[i].name;
            break;
        }
    }
    std::fprintf(stderr,
                 "wavvix::flag: 在 init() / parse() 之前读取了 %s%s；"
                 "选项的值要等命令行解析完才有意义\n",
                 name[1] == '\0' ? "-" : "--", name);
    std::fflush(stderr);
    std::abort();
}

bool was_given(const void* storage) noexcept {
    registry& self = table();
    for (std::size_t i = 0; i < self.entries.size(); ++i) {
        if (self.entries[i].storage == storage) return self.entries[i].on_command_line;
    }
    return false;
}

bool parsed() noexcept { return table().parsed; }

}  // namespace detail

result<void> parse(int argc, char* argv[], string_view version) {
    registry& self = table();
    parse_attempt attempt(self);
    self.version = version.to_string();
    self.program = (argc > 0 && argv[0]) ? fs::filename(argv[0]).to_string()
                                        : std::string("program");

    const result<void> defaults = apply_defaults(self);
    if (defaults.is_err()) return defaults;

    bool only_positional = false;
    for (int i = 1; i < argc; ++i) {
        string_view token = argv[i] ? string_view(argv[i]) : string_view();

        if (only_positional) {
            self.positional.push_back(token);
            continue;
        }
        if (token == "--") {
            only_positional = true;          // 规则 3
            continue;
        }
        if (!is_option(token)) {
            self.positional.push_back(token);   // 规则 1（继续往后解析）和规则 4
            continue;
        }

        // 去掉前导 '-'：长选项两个，短选项一个
        string_view body = token;
        body.remove_prefix(body.size() >= 2 && body[1] == '-' ? 2 : 1);

        // --name=value 里的 value 自带，和下一个 token 无关
        string_view inline_value;
        bool has_inline = false;
        const string_view::size_type equals = body.find('=');
        if (equals != string_view::npos) {
            inline_value = body.substr(equals + 1);
            body = body.substr(0, equals);
            has_inline = true;
        }

        if (body == "help" || body == "h" ||
            (body == "version" && !self.version.empty())) {
            if (has_inline)
                return make_error(error_kind::invalid_argument,
                                  token.to_string() + " 不接受值");
            if (body == "version") self.version_requested = true;
            else self.help_requested = true;
            continue;
        }

        entry* which = find_by_name(self, body);
        if (!which) {
            // 未知选项报错，不收进位置参数：静默接受拼错的 --widht 是最常见的 CLI 坑
            return make_error(error_kind::invalid_argument,
                              "未知选项 " + token.to_string());
        }

        if (which->which == detail::kind::boolean && !has_inline) {
            // 规则 2：布尔选项不吃下一个 token
            *static_cast<bool*>(which->storage) = true;
            which->on_command_line = true;
            continue;
        }

        string_view text = inline_value;
        if (!has_inline) {
            if (i + 1 >= argc || !argv[i + 1]) {
                return make_error(error_kind::invalid_argument,
                                  token.to_string() + " 后面缺少值");
            }
            text = string_view(argv[++i]);
        }
        const result<void> assigned = assign(*which, text);
        if (assigned.is_err()) return assigned;
        which->on_command_line = true;
    }

    const result<void> missing = check_required(self);
    if (missing.is_err()) return missing;
    self.parsed = true;
    attempt.complete = true;
    return result<void>();
}

void init(int argc, char* argv[], string_view version) {
    const result<void> status = parse(argc, argv, version);
    if (status.is_err()) {
        std::fprintf(stderr, "%s\n\n%s", status.error().message().c_str(), usage().c_str());
        std::fflush(stderr);
        std::exit(2);        // 和测试框架的「参数错误」退出码一致
    }
    if (help_requested()) {
        std::fprintf(stdout, "%s", usage().c_str());
        std::exit(0);
    }
    if (version_requested()) {
        std::fprintf(stdout, "%s\n", table().version.c_str());
        std::exit(0);
    }
}

bool help_requested() noexcept { return table().help_requested; }
bool version_requested() noexcept { return table().version_requested; }

const std::vector<string_view>& args() noexcept { return table().positional; }

std::string usage() {
    registry& self = table();
    std::string out = "用法：" + (self.program.empty() ? std::string("program") : self.program) +
                      " [选项]... [参数]...\n\n选项：\n";

    // 先把每一行的左半边生成出来，算出最宽的，再对齐右半边
    std::vector<std::string> heads;
    std::size_t widest = 0;
    heads.reserve(self.entries.size());
    for (std::size_t i = 0; i < self.entries.size(); ++i) {
        const entry& which = self.entries[i];
        std::string head = "  ";
        for (std::size_t j = 0; j < which.aliases.size(); ++j) {
            append_spelling(head, which.aliases[j]);
            head += ", ";
        }
        append_spelling(head, which.name);
        const char* label = type_label(which.which);
        if (label[0] != '\0') {
            head += " ";
            head += label;
        }
        if (head.size() > widest) widest = head.size();
        heads.push_back(head);
    }
    const std::string help_head = "  -h, --help";
    const std::string version_head = "  --version";
    if (help_head.size() > widest) widest = help_head.size();
    if (!self.version.empty() && version_head.size() > widest) widest = version_head.size();

    for (std::size_t i = 0; i < self.entries.size(); ++i) {
        const entry& which = self.entries[i];
        out += heads[i];
        out.append(widest + 2 - heads[i].size(), ' ');
        out += which.help ? which.help : "";
        if (which.is_required) {
            out += "（必填）";
        } else if (!which.default_text.empty()) {
            out += "（默认 " + which.default_text + "）";
        }
        out += "\n";
    }
    out += help_head;
    out.append(widest + 2 - help_head.size(), ' ');
    out += "显示这段用法\n";
    if (!self.version.empty()) {
        out += version_head;
        out.append(widest + 2 - version_head.size(), ' ');
        out += "显示版本号\n";
    }
    return out;
}

bool was_given(string_view name) noexcept {
    registry& self = table();
    const entry* const which = find_by_name(self, name);
    return which != 0 && which->on_command_line;
}

result<void> set(string_view name, string_view text) {
    registry& self = table();
    if (!self.parsed) {
        return make_error(error_kind::invalid_argument,
                          "flag::set() 需要先成功调用 flag::parse() 或 flag::init()");
    }
    entry* which = find_by_name(self, name);
    if (!which) {
        return make_error(error_kind::not_found, "没有这个选项：" + name.to_string());
    }
    const result<void> assigned = assign(*which, text);
    if (assigned.is_err()) return assigned;
    return result<void>();
}

}  // namespace flag
}  // namespace wavvix
