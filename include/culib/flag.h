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

struct required_t {};
extern const required_t required;

namespace detail {

enum class kind { boolean, signed_integer, unsigned_integer, number, text };

void register_flag(kind which, void* storage, const char* help,
                   std::initializer_list<const char*> names, bool is_required,
                   std::string default_text);
void unregister_flag(const void* storage) noexcept;

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

    // names 与 help 必须比选项对象活得久；至少提供一个不带 '-' 的名字。
    option(T default_value, const char* help, std::initializer_list<const char*> names)
        : stored_(std::move(default_value)) {
        detail::register_flag(tag(), &stored_, help, names, false, default_text(stored_));
    }

    option(required_t, const char* help, std::initializer_list<const char*> names)
        : stored_() {
        detail::register_flag(tag(), &stored_, help, names, true, default_text(stored_));
    }

    ~option() noexcept { detail::unregister_flag(&stored_); }

    const T& value() const noexcept {
        if (!detail::parsed()) detail::read_before_parse(&stored_);
        return stored_;
    }

    operator const T&() const noexcept { return value(); }

    bool was_given() const noexcept { return detail::was_given(&stored_); }

    option(const option&) = delete;
    option& operator=(const option&) = delete;

private:
    static detail::kind tag() noexcept;
    static std::string default_text(const T& value) { return to_string(value); }

    T stored_;
};

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

// 失败时打印用法并退出 2；--help / --version 退出 0。
void init(int argc, char* argv[], string_view version = string_view());

// 不退出进程；可重复调用，先恢复默认值再解析。
result<void> parse(int argc, char* argv[], string_view version = string_view());
bool help_requested() noexcept;
bool version_requested() noexcept;

// 位置参数的视图指向 argv；解析前或失败后返回空。
const std::vector<string_view>& args() noexcept;

// 生成用法文本，不打印。
std::string usage();

// 须先成功解析；不改变命令行来源标记。
result<void> set(string_view name, string_view text);

// 最近一次命令行解析是否给过该选项（别名也算）。
bool was_given(string_view name) noexcept;

}  // namespace flag
}  // namespace wavvix
