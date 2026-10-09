#pragma once

// 全局选项的命令行解析。

#include <culib/lexical_cast.h>
#include <culib/result.h>
#include <culib/string_view.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace wavvix {
namespace flag {

// 标记必填项：flag::string input("input", flag::required, "输入文件");
struct required_t {};
extern const required_t required;

namespace detail {

// 选项的类型标签。解析时按它决定「要不要吃下一个 token」和怎么转换字符串。
enum class kind { boolean, signed_integer, unsigned_integer, number, text };

// 登记一个选项。storage 指向 value<T> 里的那个 T，解析时直接写进去。
// default_text 是默认值的文本形式：parse 先把它应用一遍再处理命令行，所以
// parse 可以重复调用而结果确定；usage() 也直接用它显示默认值。
void register_flag(kind which, void* storage, const char* name, const char* help,
                   std::initializer_list<const char*> aliases, bool is_required,
                   std::string default_text);

// init / parse 之前读取是编程错误，打印诊断并终止。
[[noreturn]] WAVVIX_DETAIL_NOINLINE_COLD void read_before_parse(const char* name) noexcept;

bool was_given(const void* storage) noexcept;
bool parsed() noexcept;

}  // namespace detail

template <typename T>
class value {
public:
    // name 不带前导 '-'；aliases 是短名字或别名，也不带 '-'。
    value(const char* name, T default_value, const char* help,
          std::initializer_list<const char*> aliases = std::initializer_list<const char*>())
        : stored_(default_value), name_(name) {
        detail::register_flag(tag(), &stored_, name, help, aliases, false,
                              default_text(default_value));
    }

    value(const char* name, required_t, const char* help,
          std::initializer_list<const char*> aliases = std::initializer_list<const char*>())
        : stored_(), name_(name) {
        detail::register_flag(tag(), &stored_, name, help, aliases, true, std::string());
    }

    const T& get() const noexcept {
        if (!detail::parsed()) detail::read_before_parse(name_);
        return stored_;
    }

    // 像普通变量一样用。注意上面那条 auto 陷阱。
    operator const T&() const noexcept { return get(); }

    // 命令行里是否真的给了（而不是用了默认值）。配置文件要据此决定谁覆盖谁。
    bool was_given() const noexcept { return detail::was_given(&stored_); }

    value(const value&) = delete;
    value& operator=(const value&) = delete;

private:
    static detail::kind tag() noexcept;
    static std::string default_text(const T& value);

    T stored_;
    const char* name_;
};

// 五个具体类型。tag() 和 default_text() 在这里特化，所以 value<其他类型> 会链接失败
// 而不是静默跑出奇怪的行为。
template <> inline detail::kind value<bool>::tag() noexcept { return detail::kind::boolean; }
template <> inline detail::kind value<long long>::tag() noexcept {
    return detail::kind::signed_integer;
}
template <> inline detail::kind value<unsigned long long>::tag() noexcept {
    return detail::kind::unsigned_integer;
}
template <> inline detail::kind value<double>::tag() noexcept { return detail::kind::number; }
template <> inline detail::kind value<std::string>::tag() noexcept { return detail::kind::text; }

template <> inline std::string value<bool>::default_text(const bool& v) { return to_string(v); }
template <> inline std::string value<long long>::default_text(const long long& v) {
    return to_string(v);
}
template <> inline std::string value<unsigned long long>::default_text(
    const unsigned long long& v) {
    return to_string(v);
}
template <> inline std::string value<double>::default_text(const double& v) {
    return to_string(v);
}
template <> inline std::string value<std::string>::default_text(const std::string& v) {
    return v;
}

using boolean = value<bool>;
using int64 = value<long long>;
using uint64 = value<unsigned long long>;
using number = value<double>;
using string = value<std::string>;

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
//
// 消费者是 `config::apply_to_flags()`：**命令行要优先于配置文件**，所以它得先问一句
// 「这个已经在命令行上给过了吗」，给过就跳过。按存储地址问的那个重载在那里用不了，
// 因为配置文件手里只有名字。
bool was_given(string_view name) noexcept;

}  // namespace flag
}  // namespace wavvix
