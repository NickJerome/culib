// optional：「没有值」是正常结果时用它，不是失败。
//
//   - 「没有」是正常结果  -> optional。环境变量没设置、队列空、查表没命中。
//   - 「没有」是失败      -> result。文件打不开、输入不是数字、权限不足。
//
// 判据是「调用方需不需要知道为什么」。optional 一个字节都不带原因。

#include <culib/lexical_cast.h>
#include <culib/optional.h>
#include <culib/result.h>
#include <culib/string_view.h>

#include <cstdio>
#include <map>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using wavvix::nullopt;
using wavvix::optional;
using wavvix::result;

// os::getenv 将来的形状：没设置不是错误，所以是 optional
optional<std::string> fake_getenv(wavvix::string_view name) {
    static const std::map<std::string, std::string> environment = {
        {"HOME", "/home/user"}, {"LANG", "C.UTF-8"}};
    const std::map<std::string, std::string>::const_iterator found =
        environment.find(name.to_string());
    if (found == environment.end()) return nullopt;
    return found->second;
}

// 同一个函数里两种返回类型并排出现——这正是 optional 不提供 operator bool 的理由：
// 否则 if (home) 会紧挨着 if (port.is_err())，同一件事两种写法。
result<int> load_port() {
    const optional<std::string> from_environment = fake_getenv("PORT");
    if (!from_environment.has_value()) {
        return 8080;                       // 没设置是正常的，用默认值
    }
    const result<int> parsed = wavvix::lexical_cast<int>(from_environment.value());
    if (parsed.is_err()) {
        // 这里才是失败：环境变量设了但是设错了，调用方需要知道原因
        return wavvix::make_error(parsed.error().kind(),
                                  "PORT：" + parsed.error().message());
    }
    return parsed.value();
}

}  // namespace

int main() {
    std::printf("查环境变量（「没设置」是正常结果）：\n");
    const char* const names[] = {"HOME", "LANG", "NOT_SET"};
    for (std::size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        const optional<std::string> value = fake_getenv(names[i]);
        std::printf("  %-10s has_value=%-5s value_or(\"(无)\")=%s\n", names[i],
                    value.has_value() ? "true" : "false", value.value_or("(无)").c_str());
    }

    std::printf("\n和 nullopt 比较（不碰 T，所以 T 不必可比较）：\n");
    std::printf("  fake_getenv(\"HOME\")    != nullopt -> %s\n",
                fake_getenv("HOME") != nullopt ? "true" : "false");
    std::printf("  fake_getenv(\"NOT_SET\") == nullopt -> %s\n",
                fake_getenv("NOT_SET") == nullopt ? "true" : "false");

    std::printf("\n两种返回类型并排用（见 load_port 的实现）：\n");
    const result<int> port = load_port();
    std::printf("  load_port() -> %d   （PORT 没设置，用了默认值 8080）\n", port.value_or(-1));

    std::printf("\n开销：\n");
    std::printf("  optional<int>         %zu 字节，平凡可复制=%s（走寄存器返回）\n",
                sizeof(optional<int>),
                std::is_trivially_copyable<optional<int> >::value ? "是" : "否");
    std::printf("  optional<std::string> %zu 字节，平凡可复制=%s\n",
                sizeof(optional<std::string>),
                std::is_trivially_copyable<optional<std::string> >::value ? "是" : "否");

    std::printf("\n两个容易误读的地方：\n");
    optional<std::string> moved_from = std::string("abc");
    const optional<std::string> taken = std::move(moved_from);
    std::printf("  移动之后源仍然 has_value()=%s —— 里面是个被移走的值，\n",
                moved_from.has_value() ? "true" : "false");
    std::printf("    和 std::optional 一样。要变回「没有」就显式 reset()。\n");
    moved_from.reset();
    std::printf("    reset() 之后 has_value()=%s\n", moved_from.has_value() ? "true" : "false");
    std::printf("  没有值时调 value() 会打印诊断并终止（release 同样检查），这里不演示。\n");
    return 0;
}
