// 配置文件（ini 子集）。
//
// 重点在两处：**出错时说第几行**（配置的错是人改出来的，人需要知道改哪一行），
// 以及**「不存在」和「写错了」是两件事**（后者绝不能悄悄变成默认值）。

#include <culib/config.h>
#include <culib/flag.h>
#include <culib/fs.h>
#include <culib/test.h>

#include <string>
#include <vector>

namespace {

using wavvix::config;

template <typename T>
void ignore(const wavvix::result<T>&) {}

// 解析成功时拿到 config，失败时测试直接失败。
config parsed(const char* text) {
    const wavvix::result<config> got = config::parse(text);
    if (!got.is_ok()) return config();
    return got.value();
}

}  // namespace

TEST(config, reads_plain_keys) {
    const config one = parsed("name = culib\nport = 8080\n");
    EXPECT(one.size(), ==, 2u);
    EXPECT(one.has("name"), ==, true);
    EXPECT(one.get("name").value(), ==, "culib");
    EXPECT(one.get("port").value(), ==, "8080");
}

TEST(config, turns_sections_into_dotted_keys) {
    const config one = parsed(
        "top = 1\n"
        "[server]\n"
        "port = 8080\n"
        "host = 0.0.0.0\n"
        "[client]\n"
        "retries = 3\n");
    EXPECT(one.has("top"), ==, true);
    EXPECT(one.has("server.port"), ==, true);
    EXPECT(one.has("client.retries"), ==, true);
    EXPECT(one.has("port"), ==, false);          // 不带段前缀的不存在
    EXPECT(one.get("server.host").value(), ==, "0.0.0.0");
}

TEST(config, ignores_comments_and_blank_lines) {
    const config one = parsed(
        "# 井号注释\n"
        "\n"
        "; 分号注释\n"
        "   \n"
        "name = culib\n");
    EXPECT(one.size(), ==, 1u);
    EXPECT(one.get("name").value(), ==, "culib");
}

TEST(config, trims_whitespace_around_the_key_and_the_value) {
    const config one = parsed("   name   =    culib   \n");
    EXPECT(one.get("name").value(), ==, "culib");
}

TEST(config, strips_a_trailing_comment_but_not_a_hash_inside_a_value) {
    // **规则：前面有空白的 # 或 ; 开始注释。** 不在任何 # 上都切，因为 URL 片段和
    // 密码里带 # 是常事。这条是 examples/config.cpp 撞出来的——原来行尾注释会被
    // 吃进值里，于是 `port = 8080 # 端口` 解析不了。
    const config one = parsed(
        "port = 8080        # 端口\n"
        "semi = 7 ; 也是注释\n"
        "url = http://x/#fragment\n"
        "quoted = \"a # b\"\n"
        "only_comment = # 值是空的\n");
    EXPECT(one.get("port").value(), ==, "8080");
    EXPECT(one.get("semi").value(), ==, "7");
    EXPECT(one.get("url").value(), ==, "http://x/#fragment");   // # 前面没空白
    EXPECT(one.get("quoted").value(), ==, "a # b");             // 引号里的不算
    EXPECT(one.get("only_comment").value(), ==, "");
    // 真正要紧的后果：带注释的数字读得出来
    EXPECT(one.get_int64("port", 0).value(), ==, 8080);
}

TEST(config, reads_a_quoted_value_up_to_the_closing_quote) {
    const config one = parsed(
        "padded = \" a \"\n"
        "bare = plain\n"
        "empty = \"\"\n"
        "commented = \"x\"   # 闭引号后面的注释没问题\n");
    EXPECT(one.get("padded").value(), ==, " a ");      // 首尾空格留住了
    EXPECT(one.get("bare").value(), ==, "plain");
    EXPECT(one.get("empty").value(), ==, "");
    EXPECT(one.get("commented").value(), ==, "x");
}

TEST(config, rejects_a_quote_that_is_not_closed_or_has_junk_after_it) {
    // `x = "a" junk` 里的 junk 静默丢掉的话，一个手误就变成「值莫名其妙少了一截」。
    EXPECT(config::parse("x = \"unterminated\n").is_ok(), ==, false);
    EXPECT(config::parse("x = \"a\" junk\n").is_ok(), ==, false);
    EXPECT(config::parse("x = \"\"inner\"\"\n").is_ok(), ==, false);
}

TEST(config, keeps_the_order_the_file_had) {
    const config one = parsed("c = 3\na = 1\nb = 2\n");
    const std::vector<wavvix::string_view> all = one.keys();
    if (!EXPECT(all.size(), ==, 3u)) return;
    EXPECT(all[0], ==, "c");
    EXPECT(all[1], ==, "a");
    EXPECT(all[2], ==, "b");
}

TEST(config, lets_a_later_key_win) {
    const config one = parsed("port = 1\nport = 2\n");
    EXPECT(one.size(), ==, 1u);
    EXPECT(one.get("port").value(), ==, "2");
}

TEST(config, says_which_line_is_wrong) {
    // **出错时一定要说第几行。** 配置的错是人改出来的。
    const wavvix::result<config> missing_equals = config::parse("good = 1\nthis line is wrong\n");
    if (!EXPECT(missing_equals.is_ok(), ==, false)) return;
    EXPECT(missing_equals.error().message().find("第 2 行") != std::string::npos, ==, true);

    const wavvix::result<config> bad_section = config::parse("[unterminated\n");
    if (!EXPECT(bad_section.is_ok(), ==, false)) return;
    EXPECT(bad_section.error().message().find("第 1 行") != std::string::npos, ==, true);

    const wavvix::result<config> empty_name = config::parse("x = 1\n = 2\n");
    if (!EXPECT(empty_name.is_ok(), ==, false)) return;
    EXPECT(empty_name.error().message().find("第 2 行") != std::string::npos, ==, true);
}

TEST(config, treats_a_missing_key_as_not_an_error) {
    const config one = parsed("name = culib\n");
    EXPECT(one.get("absent").has_value(), ==, false);

    const wavvix::result<long long> port = one.get_int64("absent", 8080);
    if (!EXPECT(port.is_ok(), ==, true)) return;
    EXPECT(port.value(), ==, 8080);

    const wavvix::result<bool> flag = one.get_bool("absent", true);
    if (!EXPECT(flag.is_ok(), ==, true)) return;
    EXPECT(flag.value(), ==, true);
}

TEST(config, treats_a_malformed_value_as_an_error_not_a_default) {
    // **这条是整个模块最要紧的性质。** `port = 80 80` 静默变成 8080 是那种要查
    // 一下午的事，所以写错了一定要报出来，而且错误里要带上键名和原始文本。
    const config one = parsed("port = 80 80\nflag = perhaps\n");

    const wavvix::result<long long> port = one.get_int64("port", 8080);
    if (!EXPECT(port.is_ok(), ==, false)) return;
    EXPECT(port.error().message().find("port") != std::string::npos, ==, true);
    EXPECT(port.error().message().find("80 80") != std::string::npos, ==, true);

    const wavvix::result<bool> flag = one.get_bool("flag", false);
    EXPECT(flag.is_ok(), ==, false);
}

TEST(config, converts_the_types_it_promises) {
    const config one = parsed(
        "count = -42\n"
        "ratio = 2.5\n"
        "on = true\n"
        "off = no\n"
        "text = hello\n");
    EXPECT(one.get_int64("count", 0).value(), ==, -42);
    EXPECT(one.get_double("ratio", 0.0).value(), ==, 2.5);
    EXPECT(one.get_bool("on", false).value(), ==, true);
    EXPECT(one.get_bool("off", true).value(), ==, false);
    EXPECT(one.get_string("text", "").value(), ==, "hello");
    EXPECT(one.get_string("absent", "fallback").value(), ==, "fallback");
}

TEST(config, handles_a_last_line_without_a_newline) {
    const config one = parsed("a = 1\nb = 2");
    EXPECT(one.size(), ==, 2u);
    EXPECT(one.get("b").value(), ==, "2");
}

TEST(config, parses_an_empty_file) {
    const wavvix::result<config> got = config::parse("");
    if (!EXPECT(got.is_ok(), ==, true)) return;
    EXPECT(got.value().empty(), ==, true);
}

TEST(config, loads_from_a_file_and_says_which_file_was_wrong) {
    const wavvix::result<std::string> temp = wavvix::fs::temp_directory();
    const std::string path =
        wavvix::fs::join(temp.is_ok() ? temp.value() : ".", "culib_config_test.conf");

    ignore(wavvix::fs::write_file(path, "[server]\nport = 9000\n"));
    const wavvix::result<config> good = config::load(path);
    if (EXPECT(good.is_ok(), ==, true)) {
        EXPECT(good.value().get_int64("server.port", 0).value(), ==, 9000);
    }

    // 文件里有错时，错误要同时带上**文件名**和**行号**——底层只知道第几行。
    ignore(wavvix::fs::write_file(path, "ok = 1\nbroken\n"));
    const wavvix::result<config> bad = config::load(path);
    if (EXPECT(bad.is_ok(), ==, false)) {
        EXPECT(bad.error().message().find("第 2 行") != std::string::npos, ==, true);
        EXPECT(bad.error().message().find("culib_config_test.conf") != std::string::npos, ==,
               true);
    }

    ignore(wavvix::fs::remove_file(path));
}

TEST(config, reports_a_file_that_is_not_there) {
    const wavvix::result<config> got = config::load("/no/such/file/at/all.conf");
    EXPECT(got.is_ok(), ==, false);
    EXPECT(got.error().kind() == wavvix::error_kind::not_found, ==, true);
}

// ---- apply_to_flags ----
//
// 这几个旗标只给这组测试用。flag 的登记是在 main 之前做的（开发约定里「无启动开销」
// 的两处例外之一），所以放在文件作用域。

namespace {

wavvix::flag::int64 config_port("config_port", 1111, "测试用：端口");
wavvix::flag::string config_host("config_host", "default", "测试用：地址");

}  // namespace

// flag 有个门禁：parse() 之前读取选项的值会 abort（值要等命令行解析完才有意义）。
// 所以这几条先用一个只有程序名的命令行跑一次 parse。flag_test 里的 run() 也是每条
// 测试自己 parse 一次，所以这样不会和它互相干扰。
namespace {

void parse_empty_command_line() {
    const char* argv[] = {"culib_test"};
    ignore(wavvix::flag::parse(1, const_cast<char**>(argv)));
}

}  // namespace

TEST(config, feeds_its_values_into_flag) {
    parse_empty_command_line();
    const config one = parsed("config_port = 2222\nconfig_host = from_file\n");
    if (!EXPECT(one.apply_to_flags().is_ok(), ==, true)) return;
    EXPECT(config_port.get(), ==, 2222);
    EXPECT(config_host.get(), ==, "from_file");
    EXPECT(config_port.was_given(), ==, false);

    const config updated = parsed("config_port = 4444\n");
    if (!EXPECT(updated.apply_to_flags().is_ok(), ==, true)) return;
    EXPECT(config_port.get(), ==, 4444);
}

TEST(config, leaves_alone_what_the_command_line_already_set) {
    // **命令行优先。** 配置文件不该盖掉用户在命令行上明确给过的值。
    const char* argv[] = {"culib_test", "--config_port=3333"};
    if (!EXPECT(wavvix::flag::parse(2, const_cast<char**>(argv)).is_ok(), ==, true)) return;
    EXPECT(wavvix::flag::was_given("config_port"), ==, true);

    const config one = parsed("config_port = 4444\n");
    if (!EXPECT(one.apply_to_flags().is_ok(), ==, true)) return;
    EXPECT(config_port.get(), ==, 3333);     // 配置文件没盖掉它
}

TEST(config, skips_keys_that_no_flag_defines) {
    // 配置文件通常比命令行多（日志路径那些不走命令行），所以多出来的键不算错。
    const config one = parsed("no_such_flag_anywhere = 1\n");
    EXPECT(one.apply_to_flags().is_ok(), ==, true);
}

TEST(config, reports_a_value_that_the_flag_cannot_take) {
    // 「flag 里没有这个键」不算错，但「有这个键、值却不对」一定要报。
    parse_empty_command_line();
    EXPECT(wavvix::flag::was_given("config_port"), ==, false);
    const config one = parsed("config_port = not_a_number\n");
    const wavvix::result<void> applied = one.apply_to_flags();
    EXPECT(applied.is_ok(), ==, false);
}
