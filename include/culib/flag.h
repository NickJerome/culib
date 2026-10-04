#pragma once

// 命令行参数。选项是全局对象，定义在哪里就能在哪里读，不用从 main 一路传下去。
//
//     namespace flag = wavvix::flag;
//
//     flag::int64  port("port", 8080, "监听端口", {"p"});
//     flag::string host("host", "0.0.0.0", "监听地址");
//     flag::boolean verbose("verbose", false, "打印详细日志", {"v"});
//     flag::string input("input", flag::required, "输入文件");
//
//     int main(int argc, char* argv[]) {
//         flag::init(argc, argv, "1.2.3");          // 失败时打印用法并退出进程
//         listen(host, port);                        // 像普通变量一样用
//         for (wavvix::string_view path : flag::args()) { ... }   // 位置参数
//     }
//
// ---- 陷阱：别用 auto 拷贝 -------------------------------------------------------
//
//     auto v = verbose;          // 拷的是 flag::boolean（而且编译失败，不可拷贝）
//     auto v = verbose.get();    // 这才是 bool
//
// 隐式转换是为了「像普通变量一样读取和输出」而保留的，代价就是这一条。选项不可拷贝，
// 所以第一种写法是编译错误而不是静默的怪行为。
//
// ---- 接受的写法 ---------------------------------------------------------------
//
//   --port=8080   --port 8080   -p 8080   --verbose   --verbose=true   --   -
//
// 不支持粘连短选项 -p8080（和 -p -8080 容易混），也不支持合并 -abc。
//
// 四条解析规则：
//
//   1. 位置参数之后继续解析选项（GNU 风格）。`crop a.png --width=1024 b.png` 里
//      --width 生效。不用「遇到第一个非选项就停」那套——那会让 `crop *.png --width=1024`
//      静默失效，是 Go flag 包最常被抱怨的一点。
//   2. **布尔选项不吃下一个 token**，只接受 --verbose 和 --verbose=true。否则
//      `crop --verbose a.png` 里的 a.png 会被当成 --verbose 的值。
//   3. `--` 之后全部算位置参数，哪怕长得像选项。处理以 '-' 开头的文件名要靠它。
//   4. 单独一个 `-` 算位置参数（惯例表示标准输入）。
//
// **未知选项报错退出**，不收进位置参数——静默接受拼错的 --widht 是最常见的 CLI 坑。
//
// ---- 生命周期 -----------------------------------------------------------------
//
// args() 返回的视图指向 argv，和进程同寿，不拷字符串。C++ 标准允许程序修改 argv，
// 所以**init 之后不要改写 argv**（例如 setproctitle 那样改进程标题），否则视图会跟着变。
//
// 选项的 name / help / 别名字符串必须是静态存续的（字面量就行），库不拷贝它们。
//
// 全局选项对象在 main 之前登记，这是「无启动开销」的第二个例外（第一个是 TEST）。

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

// 按名字设值，给配置文件用：读出键值后调用它即可，库不解析配置文件格式。
// 在 init 之前调用则命令行会覆盖它；在之后调用则它覆盖命令行。
result<void> set(string_view name, string_view text);

}  // namespace flag
}  // namespace wavvix
