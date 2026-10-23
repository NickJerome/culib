// cu::flag：选项和命令的登记，命令行的解析，帮助和报错。
//
// 选项和命令在 main 之前（全局对象）或 main 的线程上登记，表放在一个永不析构的对象里。写在命令里的选项构造时先当
// 全局选项登记，命令接手时把值挪进命令、改成归这个命令。init 先认命令名和全局选项，认到不再往下分的命令时接着解析
// 剩下的参数，最后才调它的函数：函数跑起来之前，命令行已经解析完。

#include <cu/flag.h>

#include "base/fatal.h"
#include "platform/command_line.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace cu {
namespace detail {

// 写在命令里的一个选项。值放在这里，不在选项对象里：选项对象是个临时对象，命令建好时就没了。
struct flag_member {
    flag_kind kind;
    void* value;   // new 出来的 bool、std::int64_t 或 std::string
    bool ready;
};

// 命令树上的一个命令。外层命令拥有它的子命令；顶层命令归全局的 cu::flag::command 对象。
struct flag_node {
    flag_node(const char* name_text, const char* help_text)
        : name(name_text), help(help_text), function(0), call(0), destroy(0) {}
    ~flag_node();

    flag_node(const flag_node&) = delete;
    flag_node& operator=(const flag_node&) = delete;

    const char* name;
    const char* help;
    std::vector<flag_node*> children;     // 按大括号里的顺序；不再往下分的命令没有
    void* function;                       // 不再往下分的命令要调的函数，见 cu::flag::command 的构造
    int (*call)(void*, void* const*);     // 用选项的值调 function
    void (*destroy)(void*);
    std::vector<flag_member*> members;    // 写在命令里的选项，按写的顺序，也是传给函数的顺序
};

namespace {

struct flag_option {
    flag_kind kind;
    void* value;
    bool* ready;
    const char* help;
    std::vector<const char*> names;   // 第一个是主名字
    std::string fallback;             // 帮助里写的默认值，零值时为空
    const flag_node* owner;           // 声明它的命令；全局选项为空
};

struct flag_state {
    flag_state() : current(0), initialized(false), parsed(false) {}

    std::vector<flag_option> options;      // 按登记的顺序
    std::vector<flag_node*> top;           // 顶层命令
    std::vector<std::string> arguments;    // 命令行，UTF-8
    std::vector<string_view> positional;   // 指向 arguments
    std::string path;                      // 程序名加上认出来的命令名，如 "cli admin register"
    const flag_node* current;              // 认到的最后一个命令；还在顶层时为空
    bool initialized;                      // init 调过了
    bool parsed;                           // 整个命令行解析完了
};

// 永不析构：进程退出时可能还有协程在读选项，全局的命令和选项析构时也要来这里注销。
flag_state& state() {
    static flag_state* const instance = new flag_state();
    return *instance;
}

// ---- 名字 ------------------------------------------------------------------------------------------

// 名字在命令行上的写法：一个字母的是 -v，长的是 --verbose。
std::string spelling(const char* name) {
    return std::string(name[0] != '\0' && name[1] == '\0' ? "-" : "--") + name;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// 选项名和命令名都不能是空的、以 '-' 开头、含 '=' 或空白，否则命令行上拆不出来。what 是 "option" 或 "command"。
void check_name(const char* what, const char* name) {
    if (!name || name[0] == '\0') {
        fatal("cu::flag: empty %s name", what);
    }
    if (name[0] == '-') {
        fatal("cu::flag: %s name \"%s\" must not start with '-'", what, name);
    }
    for (const char* c = name; *c != '\0'; ++c) {
        if (*c == '=' || is_space(*c)) {
            fatal("cu::flag: %s name \"%s\" must not contain '=' or spaces", what, name);
        }
    }
}

// 现在能用的选项里叫 name 的：全局选项，加上认到的命令自己的。
flag_option* find_visible(flag_state& self, string_view name) {
    for (std::size_t i = 0; i < self.options.size(); ++i) {
        flag_option& candidate = self.options[i];
        if (candidate.owner && candidate.owner != self.current) {
            continue;
        }
        for (std::size_t j = 0; j < candidate.names.size(); ++j) {
            if (name == string_view(candidate.names[j])) {
                return &candidate;
            }
        }
    }
    return 0;
}

const flag_option* find_by_value(const flag_state& self, const void* value) {
    for (std::size_t i = 0; i < self.options.size(); ++i) {
        if (self.options[i].value == value) {
            return &self.options[i];
        }
    }
    return 0;
}

const flag_node* find_command(const std::vector<flag_node*>& commands, string_view name) {
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (name == string_view(commands[i]->name)) {
            return commands[i];
        }
    }
    return 0;
}

// ---- 值 --------------------------------------------------------------------------------------------

std::string decimal(std::int64_t number) {
    // 按绝对值从个位往前写；最小的负数取反会溢出，所以在无符号数上算
    std::uint64_t magnitude =
        number < 0 ? 0 - static_cast<std::uint64_t>(number) : static_cast<std::uint64_t>(number);
    char digits[24];
    std::size_t at = sizeof digits;
    do {
        digits[--at] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (number < 0) {
        digits[--at] = '-';
    }
    return std::string(digits + at, sizeof digits - at);
}

// 帮助里括号中的默认值。零值（false、0、空串）不写，和 Go 一样。
std::string fallback_text(flag_kind kind, const void* value) {
    switch (kind) {
        case flag_kind::boolean:
            return *static_cast<const bool*>(value) ? std::string("true") : std::string();
        case flag_kind::int64: {
            const std::int64_t number = *static_cast<const std::int64_t*>(value);
            return number != 0 ? decimal(number) : std::string();
        }
        case flag_kind::string: {
            const std::string& text = *static_cast<const std::string*>(value);
            return text.empty() ? std::string() : '"' + text + '"';
        }
    }
    return std::string();
}

// 十进制整数，可以带正负号；不认空白、0x 和小数点。成功返回空指针，失败返回原因。
const char* parse_int64(string_view text, std::int64_t& number) {
    std::size_t at = 0;
    bool negative = false;
    if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
        negative = text[at] == '-';
        ++at;
    }
    if (at == text.size()) {
        return "not an integer";
    }
    // 按绝对值累加；负数的绝对值能比正数的大 1
    const std::uint64_t largest = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const std::uint64_t limit = negative ? largest + 1 : largest;
    std::uint64_t magnitude = 0;
    bool overflow = false;
    for (; at < text.size(); ++at) {
        const char c = text[at];
        if (c < '0' || c > '9') {
            return "not an integer";
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (overflow || magnitude > (limit - digit) / 10) {
            overflow = true;
            continue;
        }
        magnitude = magnitude * 10 + digit;
    }
    if (overflow) {
        return "out of range";
    }
    if (!negative) {
        number = static_cast<std::int64_t>(magnitude);
    } else if (magnitude == 0) {
        number = 0;
    } else {
        number = -static_cast<std::int64_t>(magnitude - 1) - 1;
    }
    return 0;
}

// ---- 输出 ------------------------------------------------------------------------------------------

void print_to(std::FILE* stream, const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stream);
}

struct help_line {
    std::string head;
    std::string text;
};

help_line make_line(const std::string& head, const std::string& text) {
    help_line line;
    line.head = head;
    line.text = text;
    return line;
}

// 两列：左边补空格到最宽的那一个，再空两格。
void append_lines(std::string& out, const std::vector<help_line>& lines) {
    std::size_t widest = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        widest = std::max(widest, lines[i].head.size());
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += "  ";
        out += lines[i].head;
        out.append(widest - lines[i].head.size() + 2, ' ');
        out += lines[i].text;
        out += '\n';
    }
}

// 帮助里一个选项的左半边：先短名字后长名字，值不是布尔时加上类型，如 "-p, --port <int>"。没有短名字的空出
// "-x, " 的位置，长名字和别的行对齐。
std::string option_head(const flag_option& option) {
    std::string shorts;
    std::string longs;
    for (std::size_t i = 0; i < option.names.size(); ++i) {
        const char* const name = option.names[i];
        std::string& into = name[1] == '\0' ? shorts : longs;
        if (!into.empty()) {
            into += ", ";
        }
        into += spelling(name);
    }
    std::string head = shorts.empty() ? std::string("    ") : shorts;
    if (!shorts.empty() && !longs.empty()) {
        head += ", ";
    }
    head += longs;
    if (option.kind == flag_kind::int64) {
        head += " <int>";
    } else if (option.kind == flag_kind::string) {
        head += " <string>";
    }
    return head;
}

bool command_before(const flag_node* a, const flag_node* b) {
    return std::strcmp(a->name, b->name) < 0;
}

bool option_before(const flag_option* a, const flag_option* b) {
    return std::strcmp(a->names[0], b->names[0]) < 0;
}

// 认到的命令（还在顶层时是程序本身）的帮助。命令和选项都按名字排。
std::string help_text(const flag_state& self) {
    const flag_node* const node = self.current;
    const std::vector<flag_node*>& commands = node ? node->children : self.top;

    std::string out = "Usage: " + self.path + " [options]";
    if (!commands.empty()) {
        out += " <command>";
    }
    out += "\n\n";
    if (node && node->help[0] != '\0') {
        out += node->help;
        out += "\n\n";
    }

    if (!commands.empty()) {
        std::vector<const flag_node*> sorted(commands.begin(), commands.end());
        std::sort(sorted.begin(), sorted.end(), command_before);
        std::vector<help_line> lines;
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            lines.push_back(make_line(sorted[i]->name, sorted[i]->help));
        }
        out += "Commands:\n";
        append_lines(out, lines);
        out += '\n';
    }

    std::vector<const flag_option*> visible;
    for (std::size_t i = 0; i < self.options.size(); ++i) {
        if (!self.options[i].owner || self.options[i].owner == node) {
            visible.push_back(&self.options[i]);
        }
    }
    std::sort(visible.begin(), visible.end(), option_before);
    std::vector<help_line> lines;
    for (std::size_t i = 0; i < visible.size(); ++i) {
        std::string text = visible[i]->help;
        if (!visible[i]->fallback.empty()) {
            text += " (default " + visible[i]->fallback + ")";
        }
        lines.push_back(make_line(option_head(*visible[i]), text));
    }
    lines.push_back(make_line("-h, --help", "show this help"));
    out += "Options:\n";
    append_lines(out, lines);
    return out;
}

// 参数不对：一行错误、一行提示，退出码 2。
[[noreturn]] void usage_error(const flag_state& self, const std::string& message) {
    std::fflush(stdout);   // 命令已经打印的先送出去，不和错误交错
    print_to(stderr, self.path + ": " + message + "\nRun '" + self.path + " --help' for usage.\n");
    std::fflush(stderr);
    std::exit(2);
}

[[noreturn]] void show_help(const flag_state& self) {
    print_to(stdout, help_text(self));
    std::fflush(stdout);
    std::exit(0);
}

// ---- 解析 ------------------------------------------------------------------------------------------

bool is_option(string_view token) {
    // 单独一个 "-" 是位置参数，惯例表示标准输入
    return token.size() >= 2 && token[0] == '-';
}

// 一个选项参数拆成几段：--name、--name=value、-n、-n=value。
struct option_token {
    string_view spelled;   // 去掉 "=value" 的部分，报错时照写
    string_view name;      // 再去掉前面的 '-'
    string_view value;
    bool has_value;
};

option_token split_option(string_view token) {
    option_token parts;
    const std::size_t equals = token.find('=');
    parts.has_value = equals != string_view::npos;
    parts.spelled = parts.has_value ? token.substr(0, equals) : token;
    parts.value = parts.has_value ? token.substr(equals + 1) : string_view();
    parts.name = parts.spelled;
    parts.name.remove_prefix((parts.spelled.size() >= 2 && parts.spelled[1] == '-') ? 2 : 1);
    return parts;
}

[[noreturn]] void invalid_value(const flag_state& self, const option_token& parts, string_view text,
                                const char* why) {
    usage_error(self, "invalid value \"" + text.to_string() + "\" for option " + parts.spelled.to_string() + ": " +
                          why);
}

// 一个选项参数。值不在 "=" 后面时取下一个参数，next 跟着往后走。
void take_option(flag_state& self, string_view token, std::size_t& next) {
    const option_token parts = split_option(token);
    if (parts.name == "help" || parts.name == "h") {
        if (parts.has_value) {
            usage_error(self, "option " + parts.spelled.to_string() + " does not take a value");
        }
        show_help(self);
    }
    flag_option* const found = find_visible(self, parts.name);
    if (!found) {
        usage_error(self, "unknown option " + parts.spelled.to_string());
    }

    if (found->kind == flag_kind::boolean) {
        // 布尔选项不吃下一个参数，要给值只能写在 "=" 后面
        bool& switched = *static_cast<bool*>(found->value);
        if (!parts.has_value || parts.value == "true") {
            switched = true;
        } else if (parts.value == "false") {
            switched = false;
        } else {
            invalid_value(self, parts, parts.value, "not true or false");
        }
        return;
    }

    string_view text = parts.value;
    if (!parts.has_value) {
        if (next >= self.arguments.size()) {
            usage_error(self, "option " + parts.spelled.to_string() + " needs a value");
        }
        text = self.arguments[next++];
    }
    if (found->kind == flag_kind::string) {
        static_cast<std::string*>(found->value)->assign(text.data(), text.size());
        return;
    }
    std::int64_t number = 0;
    const char* const problem = parse_int64(text, number);
    if (problem) {
        invalid_value(self, parts, text, problem);
    }
    *static_cast<std::int64_t*>(found->value) = number;
}

// 从 arguments[start] 解析到底：选项写进去，别的进位置参数。完了以后，现在登记着的选项都算解析好了。
void parse_rest(flag_state& self, std::size_t start) {
    std::size_t next = start;
    while (next < self.arguments.size()) {
        const string_view token = self.arguments[next++];
        if (token == "--") {
            for (; next < self.arguments.size(); ++next) {
                self.positional.push_back(self.arguments[next]);
            }
            break;
        }
        if (is_option(token)) {
            take_option(self, token, next);
            continue;
        }
        self.positional.push_back(token);
    }
    self.parsed = true;
    for (std::size_t i = 0; i < self.options.size(); ++i) {
        *self.options[i].ready = true;
    }
}

// argv[0] 去掉目录，再去掉结尾的 ".exe"：Windows 上的程序名都带，帮助里写不写一个样。
std::string program_name(const std::vector<std::string>& arguments) {
    std::string name = arguments.empty() ? std::string() : arguments[0];
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        name.erase(0, slash + 1);
    }
    if (name.size() > 4) {
        const char* const tail = name.c_str() + name.size() - 4;
        if (tail[0] == '.' && (tail[1] | 0x20) == 'e' && (tail[2] | 0x20) == 'x' && (tail[3] | 0x20) == 'e') {
            name.erase(name.size() - 4);
        }
    }
    return name.empty() ? std::string("program") : name;
}

// ---- 命令 ------------------------------------------------------------------------------------------

void* copy_value(flag_kind kind, const void* value) {
    switch (kind) {
        case flag_kind::boolean:
            return new bool(*static_cast<const bool*>(value));
        case flag_kind::int64:
            return new std::int64_t(*static_cast<const std::int64_t*>(value));
        case flag_kind::string:
            return new std::string(*static_cast<const std::string*>(value));
    }
    return 0;
}

void delete_value(flag_kind kind, void* value) noexcept {
    switch (kind) {
        case flag_kind::boolean:
            delete static_cast<bool*>(value);
            return;
        case flag_kind::int64:
            delete static_cast<std::int64_t*>(value);
            return;
        case flag_kind::string:
            delete static_cast<std::string*>(value);
            return;
    }
}

void forget_top(const flag_node* node) noexcept {
    std::vector<flag_node*>& top = state().top;
    for (std::size_t i = 0; i < top.size(); ++i) {
        if (top[i] == node) {
            top.erase(top.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

// 顶层命令到 init 时才齐：同名的子命令写在大括号里时，也会先当一会儿顶层命令。
void check_top_names(const flag_state& self) {
    for (std::size_t i = 0; i < self.top.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (std::strcmp(self.top[i]->name, self.top[j]->name) == 0) {
                fatal("cu::flag: two commands are named \"%s\"", self.top[i]->name);
            }
        }
    }
}

// 写在命令里的选项和全局选项重名。登记时已经查过一遍，但那时写在后面、或者别的文件里的全局选项还没登记。
void check_member_names(const flag_state& self) {
    for (std::size_t i = 0; i < self.options.size(); ++i) {
        const flag_option& member = self.options[i];
        if (!member.owner) {
            continue;
        }
        for (std::size_t j = 0; j < self.options.size(); ++j) {
            const flag_option& global = self.options[j];
            if (global.owner) {
                continue;
            }
            for (std::size_t a = 0; a < member.names.size(); ++a) {
                for (std::size_t b = 0; b < global.names.size(); ++b) {
                    if (std::strcmp(member.names[a], global.names[b]) == 0) {
                        fatal("cu::flag: two options are named \"%s\"", member.names[a]);
                    }
                }
            }
        }
    }
}

}  // namespace

flag_node::~flag_node() {
    for (std::size_t i = 0; i < children.size(); ++i) {
        delete children[i];
    }
    if (function) {
        destroy(function);
    }
    for (std::size_t i = 0; i < members.size(); ++i) {
        flag_unregister(members[i]->value);
        delete_value(members[i]->kind, members[i]->value);
        delete members[i];
    }
}

// ---- 给头文件的 ------------------------------------------------------------------------------------

void flag_register(flag_kind kind, void* value, bool* ready, const char* help,
                   std::initializer_list<const char*> names) {
    if (names.size() == 0) {
        fatal("cu::flag: an option needs at least one name");
    }
    for (const char* const* name = names.begin(); name != names.end(); ++name) {
        check_name("option", *name);
    }
    const char* const first = *names.begin();
    flag_state& self = state();
    if (self.parsed) {
        fatal("cu::flag: %s declared after the command line was parsed; declare options at namespace scope or "
              "as members of a command class", spelling(first).c_str());
    }
    for (const char* const* name = names.begin(); name != names.end(); ++name) {
        const string_view text(*name);
        if (text == "help" || text == "h") {
            fatal("cu::flag: option name \"%s\" is taken by %s", *name, spelling(*name).c_str());
        }
        for (const char* const* earlier = names.begin(); earlier != name; ++earlier) {
            if (text == string_view(*earlier)) {
                fatal("cu::flag: option %s lists the name \"%s\" twice", spelling(first).c_str(), *name);
            }
        }
        if (find_visible(self, text)) {
            fatal("cu::flag: two options are named \"%s\"", *name);
        }
    }

    flag_option added;
    added.kind = kind;
    added.value = value;
    added.ready = ready;
    added.help = help ? help : "";
    added.names.assign(names.begin(), names.end());
    added.fallback = fallback_text(kind, value);
    added.owner = 0;   // 写在命令里的，命令接手时（flag_adopt）再改
    self.options.push_back(added);
}

void flag_unregister(const void* value) noexcept {
    flag_state& self = state();
    for (std::size_t i = 0; i < self.options.size(); ++i) {
        if (self.options[i].value == value) {
            self.options.erase(self.options.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void flag_read_too_early(const void* value) noexcept {
    const flag_state& self = state();
    const flag_option* const found = find_by_value(self, value);
    const std::string name = found ? spelling(found->names[0]) : std::string("an option");
    if (!self.initialized) {
        fatal("cu::flag: %s read before cu::flag::init()", name.c_str());
    }
    fatal("cu::flag: %s read before the command line was parsed", name.c_str());
}

// 新命令先当顶层命令登记；它写在外层命令的大括号里时，外层接手，从顶层拿掉。
flag_node* flag_new_command(const char* name, const char* help) {
    check_name("command", name);
    flag_state& self = state();
    if (self.initialized) {
        fatal("cu::flag: command \"%s\" declared after cu::flag::init()", name);
    }
    flag_node* const node = new flag_node(name, help ? help : "");
    self.top.push_back(node);
    return node;
}

void flag_adopt(flag_node* node, void* value) {
    flag_state& self = state();
    for (std::size_t i = 0; i < self.options.size(); ++i) {
        flag_option& entry = self.options[i];
        if (entry.value != value) {
            continue;
        }
        flag_member* const member = new flag_member;
        member->kind = entry.kind;
        member->value = copy_value(entry.kind, entry.value);
        member->ready = false;
        entry.value = member->value;   // 临时的选项对象析构时按原来的地址注销，找不到，什么也不做
        entry.ready = &member->ready;
        entry.owner = node;
        node->members.push_back(member);
        return;
    }
    fatal("cu::flag: command \"%s\" got an option that is not registered", node->name);
}

void flag_set_function(flag_node* node, void* function, int (*call)(void*, void* const*),
                       void (*destroy)(void*)) {
    node->function = function;
    node->call = call;
    node->destroy = destroy;
}

flag_subcommand::flag_subcommand(flag::command&& child) noexcept : node_(child.node_) {
    child.node_ = 0;
    forget_top(node_);
}

}  // namespace detail

namespace flag {

command::command(const char* name, const char* help, std::initializer_list<detail::flag_subcommand> subcommands)
    : node_(detail::flag_new_command(name, help)) {
    if (subcommands.size() == 0) {
        detail::fatal("cu::flag: command \"%s\" has no subcommands", name);
    }
    for (const detail::flag_subcommand* it = subcommands.begin(); it != subcommands.end(); ++it) {
        detail::flag_node* const child = it->node();
        if (!child) {
            detail::fatal("cu::flag: command \"%s\" got a subcommand that was already used", name);
        }
        for (std::size_t i = 0; i < node_->children.size(); ++i) {
            if (std::strcmp(node_->children[i]->name, child->name) == 0) {
                detail::fatal("cu::flag: command \"%s\" has two subcommands named \"%s\"", name, child->name);
            }
        }
        node_->children.push_back(child);
    }
}

command::~command() {
    if (node_) {
        detail::forget_top(node_);
        delete node_;
    }
}

void init(int argc, char* argv[]) {
    detail::flag_state& self = detail::state();
    if (self.initialized) {
        detail::fatal("cu::flag: init() called twice");
    }
    self.initialized = true;
    detail::check_top_names(self);
    detail::check_member_names(self);
    self.arguments = detail::command_line_arguments(argc, argv);
    self.path = detail::program_name(self.arguments);

    if (self.top.empty()) {
        detail::parse_rest(self, 1);
        return;
    }

    // 有命令：位置参数一个个认成命令名，直到不再往下分的那个；中间的选项只能是全局选项
    std::size_t next = 1;
    const detail::flag_node* chosen = 0;
    for (;;) {
        if (next >= self.arguments.size()) {
            detail::usage_error(self, "missing command");
        }
        const string_view token = self.arguments[next++];
        if (token == "--") {
            detail::usage_error(self, "missing command");
        }
        if (detail::is_option(token)) {
            detail::take_option(self, token, next);
            continue;
        }
        chosen = detail::find_command(self.current ? self.current->children : self.top, token);
        if (!chosen) {
            detail::usage_error(self, "unknown command " + token.to_string());
        }
        self.path += ' ';
        self.path.append(token.data(), token.size());
        self.current = chosen;
        if (chosen->children.empty()) {
            break;
        }
    }

    // 先解析完剩下的参数，再把命令自己的选项的值按顺序交给它的函数
    detail::parse_rest(self, next);
    std::vector<void*> values;
    for (std::size_t i = 0; i < chosen->members.size(); ++i) {
        values.push_back(chosen->members[i]->value);
    }
    const int code = chosen->call(chosen->function, values.empty() ? 0 : &values[0]);
    std::exit(code);
}

const std::vector<string_view>& args() {
    const detail::flag_state& self = detail::state();
    if (!self.initialized) {
        detail::fatal("cu::flag: args() called before cu::flag::init()");
    }
    if (!self.parsed) {
        detail::fatal("cu::flag: args() called before the command line was parsed");
    }
    return self.positional;
}

}  // namespace flag
}  // namespace cu
