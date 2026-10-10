#include <culib/fs.h>
#include <culib/os.h>
#include <culib/test.h>

#include <string>

namespace {

namespace os = wavvix::os;
using wavvix::nullopt;

}  // namespace

// 置位停止标志的测试独立运行于 process/os_stop.cpp，避免污染本进程。

TEST(os, reads_environment_variables) {
    // 「没设置」是正常结果，不是失败
    EXPECT(os::getenv("CULIB_DEFINITELY_NOT_SET_12345") == nullopt, ==, true);

    EXPECT(os::setenv("CULIB_TEST_VAR", "hello").is_ok(), ==, true);
    EXPECT(os::getenv("CULIB_TEST_VAR").has_value(), ==, true);
    EXPECT(os::getenv("CULIB_TEST_VAR").value(), ==, "hello");

    // 覆盖已有的值
    EXPECT(os::setenv("CULIB_TEST_VAR", "world").is_ok(), ==, true);
    EXPECT(os::getenv("CULIB_TEST_VAR").value(), ==, "world");

    // 空字符串是合法的值，和「没设置」是两回事
    EXPECT(os::setenv("CULIB_TEST_VAR", "").is_ok(), ==, true);
    EXPECT(os::getenv("CULIB_TEST_VAR").has_value(), ==, true);
    EXPECT(os::getenv("CULIB_TEST_VAR").value(), ==, "");

    // 非 ASCII 的值（Windows 上要走 UTF-8 ⇄ UTF-16）
    EXPECT(os::setenv("CULIB_TEST_VAR", "中文值").is_ok(), ==, true);
    EXPECT(os::getenv("CULIB_TEST_VAR").value(), ==, "中文值");

    EXPECT(os::unsetenv("CULIB_TEST_VAR").is_ok(), ==, true);
    EXPECT(os::getenv("CULIB_TEST_VAR") == nullopt, ==, true);
    // 幂等：本来没设置也算成功
    EXPECT(os::unsetenv("CULIB_TEST_VAR").is_ok(), ==, true);
}

TEST(os, rejects_malformed_environment_variable_names) {
    EXPECT(os::setenv("", "x").error().kind(), ==, wavvix::error_kind::invalid_argument);
    EXPECT(os::setenv("has=equals", "x").error().kind(), ==,
           wavvix::error_kind::invalid_argument);
    EXPECT(os::unsetenv("").is_err(), ==, true);
    EXPECT(os::unsetenv("has=equals").is_err(), ==, true);

    // 消息里要说清是哪条规则
    EXPECT(os::setenv("", "x").error().message().find("不能为空") != std::string::npos, ==, true);
    EXPECT(os::setenv("a=b", "x").error().message().find("'='") != std::string::npos, ==, true);
}

TEST(os, rejects_embedded_nul_without_touching_another_variable) {
    const char name[] = "CULIB_EMBEDDED_NUL_TEST\0suffix";
    const wavvix::string_view hidden_name(name, sizeof name - 1);
    const char value[] = {'x', '\0', 'y'};
    const wavvix::string_view hidden_value(value, sizeof value);
    const char* const prefix = "CULIB_EMBEDDED_NUL_TEST";

    EXPECT(os::setenv(prefix, "original").is_ok(), ==, true);
    EXPECT(os::getenv(hidden_name) == nullopt, ==, true);
    EXPECT(os::setenv(hidden_name, "changed").is_err(), ==, true);
    wavvix::optional<std::string> current = os::getenv(prefix);
    EXPECT(current.has_value() && current.value() == "original", ==, true);
    EXPECT(os::unsetenv(hidden_name).is_err(), ==, true);
    current = os::getenv(prefix);
    EXPECT(current.has_value() && current.value() == "original", ==, true);
    EXPECT(os::setenv(prefix, hidden_value).is_err(), ==, true);
    current = os::getenv(prefix);
    EXPECT(current.has_value() && current.value() == "original", ==, true);
    EXPECT(os::unsetenv(prefix).is_ok(), ==, true);
}

TEST(os, reports_its_own_executable_path) {
    const wavvix::result<std::string> path = os::executable_path();
    EXPECT(path.is_ok(), ==, true);
    if (path.is_err()) return;
    EXPECT(path.value().empty(), ==, false);
    // 必须是绝对路径，否则「找可执行文件旁边的资源」就不成立
    EXPECT(wavvix::fs::is_absolute(path.value()), ==, true);
    // 而且那个文件真的存在
    EXPECT(wavvix::fs::is_file(path.value()), ==, true);
    // 名字里应该有测试程序的名字
    EXPECT(wavvix::fs::stem(path.value()).to_string().find("culib_test") != std::string::npos,
           ==, true);
}

TEST(os, reports_the_hostname) {
    const wavvix::result<std::string> name = os::hostname();
    EXPECT(name.is_ok(), ==, true);
    if (name.is_err()) return;
    EXPECT(name.value().empty(), ==, false);
    // 主机名里不该有空白或分隔符
    EXPECT(name.value().find(' ') == std::string::npos, ==, true);
    EXPECT(name.value().find('\n') == std::string::npos, ==, true);
}

TEST(os, reports_process_id_and_cpu_count) {
    EXPECT(os::process_id() > 0, ==, true);
    EXPECT(os::process_id(), ==, os::process_id());    // 同一个进程里不变

    // 至少 1：0 对线程池没有意义，这正是不直接用 hardware_concurrency 的理由
    EXPECT(os::cpu_count() >= 1u, ==, true);
    EXPECT(os::cpu_count(), ==, os::cpu_count());
}

TEST(os, has_not_been_asked_to_stop) {
    // 没装处理器、也没人调 request_stop，所以应该是 false。
    // 这一条同时保证了本文件其他测试没有偷偷翻起那个标志。
    EXPECT(os::stop_requested(), ==, false);

    // 装上处理器不会自己翻标志，而且重复装是安全的
    EXPECT(os::install_stop_handler().is_ok(), ==, true);
    EXPECT(os::install_stop_handler().is_ok(), ==, true);
    EXPECT(os::stop_requested(), ==, false);
}
