#include <culib/flag.h>
#include <culib/test.h>

#include <string>
#include <vector>

namespace {

namespace flag = wavvix::flag;
using wavvix::result;
using wavvix::string_view;

// 测试用的选项。全局对象在 main 之前登记，这是这个模块的用法前提。
flag::int64 port(8080, "监听端口", {"port", "p"});
flag::string host("0.0.0.0", "监听地址", {"host", "addr"});
flag::boolean verbose(false, "打印详细日志", {"verbose", "v"});
flag::number timeout(2.5, "超时秒数", {"timeout", "t"});
flag::uint64 retries(3, "重试次数", {"retries"});

// parse 每次先铺默认值，所以每个测试都从干净状态开始，不互相影响。
result<void> run(const std::vector<const char*>& words, string_view version = "1.2.3") {
    std::vector<char*> argv;
    argv.reserve(words.size());
    for (std::size_t i = 0; i < words.size(); ++i) {
        argv.push_back(const_cast<char*>(words[i]));
    }
    return flag::parse(static_cast<int>(argv.size()), argv.empty() ? 0 : &argv[0], version);
}

std::string positional() {
    std::string out;
    const std::vector<string_view>& found = flag::args();
    for (std::size_t i = 0; i < found.size(); ++i) {
        out += '[';
        out.append(found[i].data(), found[i].size());
        out += ']';
    }
    return out;
}

}  // namespace

TEST(flag, accepts_the_documented_spellings) {
    EXPECT(run({"app", "--port=9000"}).is_ok(), ==, true);
    EXPECT(port.value(), ==, 9000);

    EXPECT(run({"app", "--port", "9001"}).is_ok(), ==, true);
    EXPECT(port.value(), ==, 9001);

    EXPECT(run({"app", "-p", "9002"}).is_ok(), ==, true);
    EXPECT(port.value(), ==, 9002);

    EXPECT(run({"app", "-p=9003"}).is_ok(), ==, true);
    EXPECT(port.value(), ==, 9003);

    // 五种类型都走一遍
    EXPECT(run({"app", "--host=1.2.3.4", "--timeout=0.5", "--retries=7", "--verbose"}).is_ok(),
           ==, true);
    EXPECT(host.value(), ==, "1.2.3.4");
    EXPECT(timeout.value(), ==, 0.5);
    EXPECT(retries.value(), ==, 7u);
    EXPECT(verbose.value(), ==, true);

    EXPECT(run({"app", "--addr=localhost"}).is_ok(), ==, true);
    EXPECT(host.value(), ==, "localhost");
}

TEST(flag, keeps_parsing_options_after_positional_arguments) {
    // 规则 1（GNU 风格重排）。不这样做的话 `crop *.png --width=1024` 会静默失效，
    // 那是 Go flag 包最常被抱怨的一点。
    EXPECT(run({"app", "a.png", "--port=9000", "b.png"}).is_ok(), ==, true);
    EXPECT(port.value(), ==, 9000);
    EXPECT(positional(), ==, "[a.png][b.png]");

    EXPECT(run({"app", "x", "-p", "1", "y", "--verbose", "z"}).is_ok(), ==, true);
    EXPECT(port.value(), ==, 1);
    EXPECT(verbose.value(), ==, true);
    EXPECT(positional(), ==, "[x][y][z]");
}

TEST(flag, boolean_options_do_not_eat_the_next_token) {
    // 规则 2。否则 `crop --verbose a.png` 里的 a.png 会被当成 --verbose 的值。
    EXPECT(run({"app", "--verbose", "a.png"}).is_ok(), ==, true);
    EXPECT(verbose.value(), ==, true);
    EXPECT(positional(), ==, "[a.png]");

    EXPECT(run({"app", "-v", "a.png"}).is_ok(), ==, true);
    EXPECT(verbose.value(), ==, true);
    EXPECT(positional(), ==, "[a.png]");

    // 要显式给值就用 = 形式
    EXPECT(run({"app", "--verbose=false", "a.png"}).is_ok(), ==, true);
    EXPECT(verbose.value(), ==, false);
    EXPECT(positional(), ==, "[a.png]");

    EXPECT(run({"app", "--verbose=yes"}).is_ok(), ==, true);
    EXPECT(verbose.value(), ==, true);
}

TEST(flag, treats_everything_after_a_double_dash_as_positional) {
    // 规则 3。处理以 '-' 开头的文件名要靠它。
    EXPECT(run({"app", "--", "--weird.png", "-x", "--port=1"}).is_ok(), ==, true);
    EXPECT(port.value(), ==, 8080);                       // --port=1 在 -- 之后，没生效
    EXPECT(positional(), ==, "[--weird.png][-x][--port=1]");

    EXPECT(run({"app", "--port=2", "--", "-y"}).is_ok(), ==, true);
    EXPECT(port.value(), ==, 2);                          // -- 之前的照常解析
    EXPECT(positional(), ==, "[-y]");

    EXPECT(run({"app", "--"}).is_ok(), ==, true);
    EXPECT(positional(), ==, "");
}

TEST(flag, treats_a_lone_dash_as_positional) {
    // 规则 4：惯例表示标准输入
    EXPECT(run({"app", "-", "a.png"}).is_ok(), ==, true);
    EXPECT(positional(), ==, "[-][a.png]");
}

TEST(flag, rejects_unknown_options_instead_of_collecting_them) {
    // 静默接受拼错的 --widht 是最常见的 CLI 坑
    const result<void> misspelled = run({"app", "--widht=1024"});
    EXPECT(misspelled.is_err(), ==, true);
    EXPECT(misspelled.error().kind(), ==, wavvix::error_kind::invalid_argument);
    EXPECT(misspelled.error().message().find("widht") != std::string::npos, ==, true);
    EXPECT(positional(), ==, "");                       // 没有悄悄收进位置参数

    EXPECT(run({"app", "-z"}).is_err(), ==, true);
    EXPECT(run({"app", "--unknown", "value"}).is_err(), ==, true);
}

TEST(flag, reports_missing_values_and_bad_conversions) {
    const result<void> no_value = run({"app", "--port"});
    EXPECT(no_value.is_err(), ==, true);
    EXPECT(no_value.error().message().find("缺少值") != std::string::npos, ==, true);

    // 转换失败的消息要带上选项名——lexical_cast 只知道 "80x" 不是整数，
    // 只有 flag 这一层知道那是 --port 的值
    const result<void> bad = run({"app", "--port=80x"});
    EXPECT(bad.is_err(), ==, true);
    EXPECT(bad.error().message().find("--port") != std::string::npos, ==, true);
    EXPECT(bad.error().message().find("80x") != std::string::npos, ==, true);

    EXPECT(run({"app", "--timeout=soon"}).is_err(), ==, true);
    EXPECT(run({"app", "--verbose=maybe"}).is_err(), ==, true);
    EXPECT(run({"app", "--retries=-1"}).is_err(), ==, true);     // 无符号拒负
    EXPECT(run({"app", "--port=99999999999999999999"}).is_err(), ==, true);
}

TEST(flag, restores_defaults_on_every_parse) {
    EXPECT(run({"app", "--port=1", "--host=x", "--verbose"}).is_ok(), ==, true);
    EXPECT(port.value(), ==, 1);

    // 第二次不给，必须回到默认值而不是留着上次的
    EXPECT(run({"app"}).is_ok(), ==, true);
    EXPECT(port.value(), ==, 8080);
    EXPECT(host.value(), ==, "0.0.0.0");
    EXPECT(verbose.value(), ==, false);
    EXPECT(timeout.value(), ==, 2.5);
    EXPECT(retries.value(), ==, 3u);
    EXPECT(positional(), ==, "");
}

TEST(flag, failed_parse_clears_the_previous_command_line_state) {
    EXPECT(run({"app", "--port=9000", "first"}).is_ok(), ==, true);
    EXPECT(port.was_given(), ==, true);
    EXPECT(positional(), ==, "[first]");

    EXPECT(run({"app", "second", "--port=9001", "--unknown"}).is_err(), ==, true);
    EXPECT(port.was_given(), ==, false);
    EXPECT(positional(), ==, "");
    EXPECT(flag::help_requested(), ==, false);
    EXPECT(flag::set("port", "7000").is_err(), ==, true);

    EXPECT(run({"app", "--port=8000"}).is_ok(), ==, true);
    EXPECT(port.value(), ==, 8000);
}

TEST(flag, tracks_whether_an_option_was_actually_given) {
    EXPECT(run({"app", "--host=1.2.3.4"}).is_ok(), ==, true);
    EXPECT(host.was_given(), ==, true);
    EXPECT(port.was_given(), ==, false);     // 用的是默认值

    EXPECT(run({"app"}).is_ok(), ==, true);
    EXPECT(host.was_given(), ==, false);     // 上次的状态不会留下来
}

TEST(flag, handles_help_and_version_without_exiting_in_parse) {
    EXPECT(run({"app", "--help"}).is_ok(), ==, true);
    EXPECT(flag::help_requested(), ==, true);
    EXPECT(flag::version_requested(), ==, false);

    EXPECT(run({"app", "-h"}).is_ok(), ==, true);
    EXPECT(flag::help_requested(), ==, true);

    EXPECT(run({"app", "--version"}).is_ok(), ==, true);
    EXPECT(flag::version_requested(), ==, true);
    EXPECT(flag::help_requested(), ==, false);

    EXPECT(run({"app", "--help=ignored"}).is_err(), ==, true);
    EXPECT(flag::help_requested(), ==, false);
    EXPECT(run({"app", "-h=ignored"}).is_err(), ==, true);
    EXPECT(run({"app", "--version=ignored"}).is_err(), ==, true);
    EXPECT(flag::version_requested(), ==, false);

    // version 为空时不提供 --version，所以它是未知选项
    EXPECT(run({"app", "--version"}, string_view()).is_err(), ==, true);

    // 两个查询每次 parse 都会清掉
    EXPECT(run({"app"}).is_ok(), ==, true);
    EXPECT(flag::help_requested(), ==, false);
    EXPECT(flag::version_requested(), ==, false);
}

TEST(flag, generates_usage_text) {
    EXPECT(run({"app", "--port=1"}).is_ok(), ==, true);
    const std::string text = flag::usage();

    EXPECT(text.find("--port") != std::string::npos, ==, true);
    EXPECT(text.find("-p,") != std::string::npos, ==, true);          // 别名也列出来
    EXPECT(text.find("--addr, --host") != std::string::npos, ==, true);
    EXPECT(text.find("监听端口") != std::string::npos, ==, true);     // 帮助文字
    EXPECT(text.find("默认 8080") != std::string::npos, ==, true);    // 默认值
    EXPECT(text.find("--help") != std::string::npos, ==, true);
    EXPECT(text.find("--version") != std::string::npos, ==, true);
    EXPECT(text.find("<int>") != std::string::npos, ==, true);        // 类型标签是 ASCII
    EXPECT(text.find("app") != std::string::npos, ==, true);          // 程序名取自 argv[0]

    // 布尔选项不带值，所以不显示类型标签
    const std::string::size_type at = text.find("--verbose");
    EXPECT(at != std::string::npos, ==, true);
    EXPECT(text.substr(at, 20).find("<") == std::string::npos, ==, true);

    // version 为空时不列 --version
    EXPECT(run({"app"}, string_view()).is_ok(), ==, true);
    EXPECT(flag::usage().find("--version") == std::string::npos, ==, true);
}

TEST(flag, shows_a_single_character_primary_name_as_a_short_option) {
    flag::boolean enabled(false, "enable feature", {"x"});
    EXPECT(run({"app", "-x"}).is_ok(), ==, true);
    EXPECT(enabled.value(), ==, true);
    const std::string text = flag::usage();
    EXPECT(text.find("  -x") != std::string::npos, ==, true);
    EXPECT(text.find("--x") == std::string::npos, ==, true);
}

TEST(flag, sets_values_by_name_for_config_files) {
    EXPECT(run({"app"}).is_ok(), ==, true);

    EXPECT(flag::set("port", "7000").is_ok(), ==, true);
    EXPECT(port.value(), ==, 7000);
    EXPECT(port.was_given(), ==, false);         // 仅命令行算「给过」

    // 别名也能用
    EXPECT(flag::set("p", "7001").is_ok(), ==, true);
    EXPECT(port.value(), ==, 7001);

    const result<void> missing = flag::set("nope", "x");
    EXPECT(missing.is_err(), ==, true);
    EXPECT(missing.error().kind(), ==, wavvix::error_kind::not_found);

    const result<void> bad = flag::set("port", "zzz");
    EXPECT(bad.is_err(), ==, true);
    EXPECT(bad.error().message().find("--port") != std::string::npos, ==, true);
    EXPECT(port.value(), ==, 7001);                // 失败时不改动原值
}

TEST(flag, converts_implicitly_at_the_call_site) {
    EXPECT(run({"app", "--port=9000", "--host=h", "--verbose", "--timeout=1.5"}).is_ok(), ==, true);

    // 像普通变量一样用，不写 .value()
    struct helper {
        static long long doubled(long long value) { return value * 2; }
        static std::size_t length(const std::string& text) { return text.size(); }
    };
    EXPECT(helper::doubled(port), ==, 18000);
    EXPECT(helper::length(host), ==, 1u);
    EXPECT(verbose ? 1 : 0, ==, 1);
    EXPECT(timeout + 0.5, ==, 2.0);
}

TEST(flag, args_point_into_argv_without_copying) {
    const char* words[] = {"app", "first.png", "second.png"};
    std::vector<char*> argv;
    for (std::size_t i = 0; i < 3; ++i) argv.push_back(const_cast<char*>(words[i]));
    EXPECT(flag::parse(3, &argv[0], "1.0").is_ok(), ==, true);

    const std::vector<string_view>& found = flag::args();
    EXPECT(found.size(), ==, 2u);
    EXPECT(found[0].data(), ==, words[1]);     // 同一块内存，不是拷贝
    EXPECT(found[1].data(), ==, words[2]);
}

TEST(flag, parses_an_empty_command_line) {
    EXPECT(run({"app"}).is_ok(), ==, true);
    EXPECT(positional(), ==, "");
    EXPECT(port.value(), ==, 8080);

    // argc 为 0（没有 argv[0]）也不能崩
    EXPECT(flag::parse(0, 0, "1.0").is_ok(), ==, true);
    EXPECT(flag::usage().find("program") != std::string::npos, ==, true);
}

TEST(flag, unregisters_local_options_before_the_next_parse) {
    {
        flag::string temporary("default", "temporary option", {"temporary"});
        EXPECT(run({"app", "--temporary=chosen"}).is_ok(), ==, true);
        EXPECT(temporary.value(), ==, "chosen");
    }

    EXPECT(port.value(), ==, 8080);
    EXPECT(run({"app"}).is_ok(), ==, true);
    EXPECT(run({"app", "--temporary=x"}).is_err(), ==, true);
}

TEST(flag, adding_an_option_invalidates_the_previous_parse) {
    EXPECT(run({"app", "--port=9000"}).is_ok(), ==, true);
    {
        flag::int64 temporary(7, "temporary option", {"temporary"});
        EXPECT(flag::set("port", "1").is_err(), ==, true);
        EXPECT(port.was_given(), ==, false);
        EXPECT(run({"app", "--temporary=8"}).is_ok(), ==, true);
        EXPECT(temporary.value(), ==, 8);
    }
    EXPECT(run({"app"}).is_ok(), ==, true);
    EXPECT(port.value(), ==, 8080);
}
