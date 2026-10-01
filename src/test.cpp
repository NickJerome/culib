#include <culib/test.h>

#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <locale>

namespace wavvix {
namespace test {
namespace detail {

namespace {

test_context* current = nullptr;

const char hex_digits[] = "0123456789abcdef";

std::string hex_byte(unsigned char byte) {
    std::string text = "\\x";
    text += hex_digits[byte >> 4];
    text += hex_digits[byte & 0x0f];
    return text;
}

// 加上双引号，并转义换行、制表符、双引号、反斜杠和其他控制字符；UTF-8 字节原样保留。
std::string quote(const char* data, std::size_t size) {
    std::string text = "\"";
    for (std::size_t i = 0; i < size; ++i) {
        const char c = data[i];
        const unsigned char byte = static_cast<unsigned char>(c);
        if (c == '\n') {
            text += "\\n";
        } else if (c == '\t') {
            text += "\\t";
        } else if (c == '"') {
            text += "\\\"";
        } else if (c == '\\') {
            text += "\\\\";
        } else if (byte < 0x20 || byte == 0x7f) {
            text += hex_byte(byte);
        } else {
            text += c;
        }
    }
    text += '"';
    return text;
}

// 以百分之一为单位的整数，格式化成两位小数，例如 125 -> "1.25"。
std::string two_decimals(std::int64_t hundredths) {
    std::string fraction = display_signed(hundredths % 100);
    if (fraction.size() < 2) {
        fraction.insert(0, "0");
    }
    return display_signed(hundredths / 100) + "." + fraction;
}

// "1 test"、"2 tests"
std::string count_text(std::size_t count, const char* noun) {
    std::string text = display_unsigned(count) + " " + noun;
    if (count != 1) {
        text += "s";
    }
    return text;
}

std::int64_t steady_now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string full_name(const test_case& test) {
    return std::string(test.suite) + "." + test.name;
}

struct suite_group {
    const char* name;
    std::vector<const test_case*> tests;
};

// 按组名分组：组按第一次出现的先后排列，组内保持原有顺序。
std::vector<suite_group> group_by_suite(const std::vector<const test_case*>& tests) {
    std::vector<suite_group> groups;
    for (const test_case* test : tests) {
        suite_group* group = nullptr;
        for (suite_group& existing : groups) {
            if (std::strcmp(existing.name, test->suite) == 0) {
                group = &existing;
                break;
            }
        }
        if (group == nullptr) {
            groups.push_back(suite_group{test->suite, std::vector<const test_case*>()});
            group = &groups.back();
        }
        group->tests.push_back(test);
    }
    return groups;
}

// 运行一个测试，返回是否通过；elapsed 返回测试函数本身的耗时。
bool run_single_test(const test_case& test, std::ostream& out, clock_function clock, std::int64_t& elapsed) {
    test_context context = {&out, false};
    test_context* previous = exchange_context(&context);
    const std::int64_t start = clock();
    test.function();
    elapsed = clock() - start;
    exchange_context(previous);
    return !context.failed;
}

// 用 : 分隔的多个模式里，是否有一个匹配
bool matches_any(const std::string& patterns, const std::string& text) {
    std::size_t start = 0;
    while (true) {
        const std::size_t end = patterns.find(':', start);
        const std::string pattern =
            patterns.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (matches_pattern(pattern.c_str(), text.c_str())) {
            return true;
        }
        if (end == std::string::npos) {
            return false;
        }
        start = end + 1;
    }
}

void print_usage(std::ostream& out, const char* program) {
    out << "Usage: " << program << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --filter=PATTERNS  Only run tests whose full name (suite.name) matches.\n"
        << "                     '*' matches any string and '?' matches one character.\n"
        << "                     Separate patterns with ':'; patterns after '-' are excluded.\n"
        << "                     Example: --filter=defer.*-*loop*\n"
        << "  --list             List the tests instead of running them.\n"
        << "  --help             Show this message.\n";
}

}  // namespace

// ------------------------------------------------------------------ 注册

std::vector<test_case>& registry() {
    static std::vector<test_case> tests;
    return tests;
}

registrar::registrar(const char* suite, const char* name, test_function function, const char* file, int line) {
    const test_case entry = {suite, name, function, file, line};
    registry().push_back(entry);
}

// ------------------------------------------------------------------ 失败上报

test_context* current_context() noexcept {
    return current;
}

test_context* exchange_context(test_context* context) noexcept {
    test_context* previous = current;
    current = context;
    return previous;
}

void report_failure(const char* file, int line, const char* left_text, const char* op_text, const char* right_text,
                    const std::string& left_value, const std::string& right_value) {
    std::ostream& out = current != nullptr ? *current->out : std::cerr;
    out << file << ':' << line << ": Failure\n"
        << "  Expression: " << left_text << ' ' << op_text << ' ' << right_text << '\n'
        << "  Values:     " << left_value << ' ' << op_text << ' ' << right_value << '\n';
    if (current != nullptr) {
        current->failed = true;
    }
}

// ------------------------------------------------------------------ 值的打印

std::string display_bool(bool value) {
    return value ? "true" : "false";
}

std::string display_char(char value) {
    const unsigned char byte = static_cast<unsigned char>(value);
    std::string text = "'";
    if (value == '\'' || value == '\\') {
        text += '\\';
        text += value;
    } else if (byte >= 0x20 && byte < 0x7f) {
        text += value;
    } else {
        text += hex_byte(byte);
    }
    text += '\'';
    return text;
}

std::string display_unsigned(unsigned long long value) {
    char buffer[24];
    char* const end = buffer + sizeof(buffer);
    char* begin = end;
    do {
        *--begin = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    return std::string(begin, end);
}

std::string display_signed(long long value) {
    if (value < 0) {
        // 先转成无符号再取反，避免最小值取反时溢出
        return "-" + display_unsigned(0ULL - static_cast<unsigned long long>(value));
    }
    return display_unsigned(static_cast<unsigned long long>(value));
}

std::string display_floating(long double value, int significant_digits) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(significant_digits) << value;
    return stream.str();
}

std::string display_c_string(const char* value) {
    if (value == nullptr) {
        return "nullptr";
    }
    return quote(value, std::strlen(value));
}

std::string display_string(const std::string& value) {
    return quote(value.data(), value.size());
}

std::string display_pointer(const void* value) {
    if (value == nullptr) {
        return "nullptr";
    }
    std::uintptr_t bits = reinterpret_cast<std::uintptr_t>(value);
    std::string digits;
    do {
        digits.insert(digits.begin(), hex_digits[bits & 0x0f]);
        bits >>= 4;
    } while (bits != 0);
    return "0x" + digits;
}

std::string display_unprintable() {
    return "<unprintable>";
}

// ------------------------------------------------------------------ 运行

bool matches_pattern(const char* pattern, const char* text) {
    const char* star = nullptr;   // 最近一个 * 的位置
    const char* retry = nullptr;  // 回溯时 text 从这里之后重新尝试
    while (*text != '\0') {
        if (*pattern == '*') {
            star = pattern++;
            retry = text;
        } else if (*pattern == '?' || *pattern == *text) {
            ++pattern;
            ++text;
        } else if (star != nullptr) {
            pattern = star + 1;
            text = ++retry;
        } else {
            return false;
        }
    }
    while (*pattern == '*') {
        ++pattern;
    }
    return *pattern == '\0';
}

bool matches_filter(const std::string& full_name, const std::string& filter) {
    const std::size_t dash = filter.find('-');
    std::string positive = filter.substr(0, dash);
    const std::string negative = dash == std::string::npos ? std::string() : filter.substr(dash + 1);
    if (positive.empty()) {
        positive = "*";
    }
    if (!matches_any(positive, full_name)) {
        return false;
    }
    return negative.empty() || !matches_any(negative, full_name);
}

parsed_arguments parse_arguments(int argc, const char* const argv[]) {
    parsed_arguments parsed;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.compare(0, 9, "--filter=") == 0) {
            parsed.options.filter = argument.substr(9);
        } else if (argument == "--list") {
            parsed.options.list_only = true;
        } else if (argument == "--help" || argument == "-h") {
            parsed.show_help = true;
        } else {
            parsed.ok = false;
            parsed.error = "unknown argument: " + argument;
            return parsed;
        }
    }
    return parsed;
}

std::string format_duration(std::int64_t nanoseconds) {
    if (nanoseconds < 0) {
        nanoseconds = 0;
    }
    if (nanoseconds < 1000000) {
        return display_signed(nanoseconds / 1000) + " us";
    }
    // 先按毫秒四舍五入到两位小数；进位到 1000.00 ms 时改用秒
    const std::int64_t milliseconds = (nanoseconds + 5000) / 10000;  // 单位：0.01 ms
    if (milliseconds < 100000) {
        return two_decimals(milliseconds) + " ms";
    }
    return two_decimals((nanoseconds + 5000000) / 10000000) + " s";  // 单位：0.01 s
}

run_summary run_tests(const std::vector<test_case>& tests, const run_options& options, std::ostream& out) {
    const clock_function clock = options.clock != nullptr ? options.clock : &steady_now;
    std::vector<const test_case*> selected;
    for (const test_case& test : tests) {
        if (matches_filter(full_name(test), options.filter)) {
            selected.push_back(&test);
        }
    }
    const std::vector<suite_group> groups = group_by_suite(selected);
    run_summary summary;

    if (options.list_only) {
        for (const suite_group& group : groups) {
            out << group.name << ".\n";
            for (const test_case* test : group.tests) {
                out << "  " << test->name << '\n';
            }
        }
        out.flush();
        return summary;
    }

    out << "[==========] Running " << count_text(selected.size(), "test") << " from "
        << count_text(groups.size(), "test suite") << ".\n";
    const std::int64_t run_start = clock();
    for (const suite_group& group : groups) {
        out << "[----------] " << count_text(group.tests.size(), "test") << " from " << group.name << '\n';
        const std::int64_t suite_start = clock();
        for (const test_case* test : group.tests) {
            const std::string name = full_name(*test);
            out << "[ RUN      ] " << name << std::endl;  // 立即刷新：崩溃时最后一行就是出问题的测试
            std::int64_t elapsed = 0;
            const bool passed = run_single_test(*test, out, clock, elapsed);
            ++summary.ran;
            if (passed) {
                out << "[       OK ] " << name << " (" << format_duration(elapsed) << ")\n";
            } else {
                out << "[  FAILED  ] " << name << " (" << format_duration(elapsed) << ")\n";
                ++summary.failed;
                summary.failed_names.push_back(name);
            }
        }
        const std::int64_t suite_end = clock();
        out << "[----------] " << count_text(group.tests.size(), "test") << " from " << group.name << " ("
            << format_duration(suite_end - suite_start) << " total)\n\n";
    }
    const std::int64_t run_end = clock();
    out << "[==========] " << count_text(selected.size(), "test") << " from "
        << count_text(groups.size(), "test suite") << " ran. (" << format_duration(run_end - run_start)
        << " total)\n";
    out << "[  PASSED  ] " << count_text(static_cast<std::size_t>(summary.ran - summary.failed), "test") << ".\n";
    if (summary.failed > 0) {
        out << "[  FAILED  ] " << count_text(static_cast<std::size_t>(summary.failed), "test")
            << ", listed below:\n";
        for (const std::string& name : summary.failed_names) {
            out << "[  FAILED  ] " << name << '\n';
        }
        out << "\n " << summary.failed << " FAILED " << (summary.failed == 1 ? "TEST" : "TESTS") << '\n';
    }
    out.flush();
    return summary;
}

int run_main(const std::vector<test_case>& tests, int argc, const char* const argv[], std::ostream& out,
             std::ostream& err, clock_function clock) {
    const char* const program = argc > 0 && argv[0] != nullptr ? argv[0] : "test";
    parsed_arguments parsed = parse_arguments(argc, argv);
    if (!parsed.ok) {
        err << "error: " << parsed.error << "\n\n";
        print_usage(err, program);
        return 2;
    }
    if (parsed.show_help) {
        print_usage(out, program);
        return 0;
    }
    for (std::size_t i = 0; i < tests.size(); ++i) {
        for (std::size_t j = i + 1; j < tests.size(); ++j) {
            if (std::strcmp(tests[i].suite, tests[j].suite) == 0 && std::strcmp(tests[i].name, tests[j].name) == 0) {
                err << "error: duplicate test name " << full_name(tests[i]) << " (" << tests[i].file << ':'
                    << tests[i].line << " and " << tests[j].file << ':' << tests[j].line << ")\n";
                return 2;
            }
        }
    }
    parsed.options.clock = clock;
    return run_tests(tests, parsed.options, out).failed > 0 ? 1 : 0;
}

}  // namespace detail

int run_all(int argc, char* argv[]) {
    detail::prepare_process();
    return detail::run_main(detail::registry(), argc, argv, std::cout, std::cerr, nullptr);
}

}  // namespace test
}  // namespace wavvix
