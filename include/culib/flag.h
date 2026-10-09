#pragma once

// 全局选项的命令行解析。

#include <culib/lexical_cast.h>
#include <culib/result.h>
#include <culib/string_view.h>

#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace wavvix {
namespace flag {

// 标记必填项：flag::string input(flag::required, "输入文件", {"input"});
struct required_t {};
extern const required_t required;

namespace detail {

// 选项的类型标签。解析时按它决定「要不要吃下一个 token」和怎么转换字符串。
enum class kind { boolean, signed_integer, unsigned_integer, number, text };

// storage 指向 option<T> 保存的值；default_text 用于恢复默认值和生成用法。
void register_flag(kind which, void* storage, const char* help,
                   std::initializer_list<const char*> names, bool is_required,
                   std::string default_text);
void unregister_flag(const void* storage) noexcept;

// init / parse 之前读取是编程错误，打印诊断并终止。
[[noreturn]] WAVVIX_DETAIL_NOINLINE_COLD void read_before_parse(const void* storage) noexcept;

bool was_given(const void* storage) noexcept;
bool parsed() noexcept;

}  // namespace detail

template <typename T>
class option {
public:
    static_assert(std::is_same<T, bool>::value ||
                  std::is_same<T, long long>::value ||
                  std::is_same<T, unsigned long long>::value ||
                  std::is_same<T, double>::value ||
                  std::is_same<T, std::string>::value,
                  "flag::option supports only bool, int64, uint64, double and string");

    // names 至少有一个；第一个是主名字，其余是别名，都不带前导 '-'。
    // 名字和 help 须比选项对象活得久；对象销毁时自动注销。
    option(T default_value, const char* help, std::initializer_list<const char*> names)
        : stored_(std::move(default_value)) {
        detail::register_flag(tag(), &stored_, help, names, false, default_text(stored_));
    }

    option(required_t, const char* help, std::initializer_list<const char*> names)
        : stored_() {
        detail::register_flag(tag(), &stored_, help, names, true, std::string());
    }

    ~option() noexcept { detail::unregister_flag(&stored_); }

    const T& value() const noexcept {
        if (!detail::parsed()) detail::read_before_parse(&stored_);
        return stored_;
    }

    // 像普通变量一样用。
    operator const T&() const noexcept { return value(); }

    // 命令行里是否真的给了（而不是用了默认值）。配置文件要据此决定谁覆盖谁。
    bool was_given() const noexcept { return detail::was_given(&stored_); }

    option(const option&) = delete;
    option& operator=(const option&) = delete;

private:
    static detail::kind tag() noexcept;
    static std::string default_text(const T& value) { return to_string(value); }

    T stored_;
};

// 五个具体类型对应各自的解析方式和默认值文本。
template <> inline detail::kind option<bool>::tag() noexcept { return detail::kind::boolean; }
template <> inline detail::kind option<long long>::tag() noexcept {
    return detail::kind::signed_integer;
}
template <> inline detail::kind option<unsigned long long>::tag() noexcept {
    return detail::kind::unsigned_integer;
}
template <> inline detail::kind option<double>::tag() noexcept { return detail::kind::number; }
template <> inline detail::kind option<std::string>::tag() noexcept { return detail::kind::text; }

template <> inline std::string option<std::string>::default_text(const std::string& v) {
    return v;
}

using boolean = option<bool>;
using int64 = option<long long>;
using uint64 = option<unsigned long long>;
using number = option<double>;
using string = option<std::string>;

// 解析命令行。**失败时打印用法到 stderr 并退出进程**（退出码 2，和测试框架一致），
// 这是 CLI 想要的行为。--help 打印用法后退出 0；version 非空时额外提供 --version。
void init(int argc, char* argv[], string_view version = string_view());

// 不希望它退出进程时用这个（测试框架自己要控制退出码）。
// --help / --version 不会退出，而是返回成功并让下面两个查询为真。
// 可以重复调用：每次先把所有选项恢复成默认值，再处理这一次的 argv。
result<void> parse(int argc, char* argv[], string_view version = string_view());
bool help_requested() noexcept;
bool version_requested() noexcept;

// 位置参数：argv 里既不是程序名、也不是选项、也不是某个选项的值的那些元素，按出现顺序。
// 视图指向 argv，不拷字符串。init / parse 之前调用或解析失败时返回空。
const std::vector<string_view>& args() noexcept;

// 生成的用法文本（纯查询，不打印；测试框架要自己打印时用）。
std::string usage();

// 按名字设值，给配置文件用；须先成功 parse / init。
// 不改变 was_given()：它只表示命令行是否给过这个选项。
result<void> set(string_view name, string_view text);

// 这个选项在最近一次成功解析的命令行上被给过吗（别名也算）。
// 没有这个选项或最近一次解析失败时返回 false。
bool was_given(string_view name) noexcept;

}  // namespace flag
}  // namespace wavvix
