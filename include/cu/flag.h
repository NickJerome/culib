#pragma once

// 命令行选项和子命令。选项是全局对象，main 里调一次 cu::flag::init(argc, argv)：
//
//     cu::flag::string address(":8080", "where to listen, host:port", {"address", "a"});
//
//     int main(int argc, char* argv[]) {
//         cu::flag::init(argc, argv);
//         std::printf("%s\n", address.value().c_str());
//     }
//
// 有子命令时，命令也写成全局对象，子命令直接写在外层命令的大括号里。不再往下分的命令最后给一个函数；命令自己的选项
// 写在函数前面，解析出来的值按顺序作为函数的参数传进去：
//
//     cu::flag::command admin("admin", "manage users", {
//         cu::flag::command("register", "register a user",
//             cu::flag::string("", "user name", {"name", "n"}),
//             [](const std::string& name) {
//                 std::printf("register %s\n", name.c_str());
//             }),
//         cu::flag::command("remove", "remove the users given as arguments", admin_remove),
//     });
//
// init 先把命令行解析完，再调选中的命令的函数，用它的返回值退出进程：函数跑起来之前，--help 已经打印、参数错误已经
// 报了。--help 打印帮助、退出码 0；参数不对时打印一行错误和一行提示、退出码 2。解析规则、帮助的样子和完整的例子见
// docs/flag.md。
//
// 不是线程安全的：声明选项和命令、调 init 都在 main 的线程上。解析完以后，任何线程都能读选项。

#include <cu/detail/macros.h>
#include <cu/string_view.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace cu {

namespace flag {
template <typename T>
class option;
class command;
}  // namespace flag

namespace detail {

enum class flag_kind { boolean, int64, string };

// 选项构造时登记、析构时注销。value 和 ready 指向选项自己的成员：解析时往 value 里写值，写完把 ready 置真。
void flag_register(flag_kind kind, void* value, bool* ready, const char* help,
                   std::initializer_list<const char*> names);
void flag_unregister(const void* value) noexcept;

// 命令行解析完之前读了全局选项：打印诊断后终止。
[[noreturn]] CU_DETAIL_NOINLINE_COLD void flag_read_too_early(const void* value) noexcept;

struct flag_node;   // 命令树上的一个命令，定义在 src/flag/flag.cpp

flag_node* flag_new_command(const char* name, const char* help);

// 写在命令里的选项归这个命令：值从选项对象挪到命令里，选项对象（临时的）不再算全局选项。
void flag_adopt(flag_node* node, void* value);

// 命令要调的函数：建在堆上，类型抹掉。call(function, values) 调它，values[i] 指向第 i 个选项解析出来的值；
// destroy(function) 释放它。
void flag_set_function(flag_node* node, void* function, int (*call)(void*, void* const*), void (*destroy)(void*));

// C++11 没有 std::index_sequence。
template <std::size_t... I>
struct flag_indices {};

template <std::size_t N, std::size_t... I>
struct flag_make_indices : flag_make_indices<N - 1, N - 1, I...> {};

template <std::size_t... I>
struct flag_make_indices<0, I...> {
    typedef flag_indices<I...> type;
};

template <typename... T>
struct flag_types {};

// 命令的参数里，函数前面的都得是选项。
template <typename Part>
struct flag_option_traits {
    static const bool is_option = false;
    typedef void value_type;
};

template <typename T>
struct flag_option_traits<flag::option<T>> {
    static const bool is_option = true;
    typedef T value_type;
};

// 命令的第 I 个参数的类型，和它是选项时值的类型。不用别名模板：GCC 4.8 展开带参数包的别名模板时报 sorry。
template <std::size_t I, typename... Parts>
struct flag_part {
    typedef typename std::tuple_element<I, std::tuple<Parts...>>::type type;
};

template <std::size_t I, typename... Parts>
struct flag_part_value {
    typedef typename flag_option_traits<typename std::decay<typename flag_part<I, Parts...>::type>::type>::value_type
        type;
};

// 用选项的值调 Function 得到什么类型；调不了时是 flag_not_callable。
struct flag_not_callable {};

template <typename Function, typename... T>
struct flag_call_result {
    template <typename F>
    static auto test(int) -> decltype(std::declval<F&>()(std::declval<const T&>()...));
    template <typename F>
    static flag_not_callable test(...);

    typedef decltype(test<Function>(0)) type;
};

template <typename Function, typename... T, std::size_t... I>
int flag_call_with(Function& function, void* const* values, flag_types<T...>, flag_indices<I...>,
                   std::true_type /* 函数不返回值 */) {
    (void)values;   // 没有选项时用不到
    function(*static_cast<const T*>(values[I])...);
    return 0;
}

template <typename Function, typename... T, std::size_t... I>
int flag_call_with(Function& function, void* const* values, flag_types<T...>, flag_indices<I...>, std::false_type) {
    (void)values;
    return function(*static_cast<const T*>(values[I])...);
}

template <typename Function, typename... T>
int flag_call(void* function, void* const* values) {
    typedef typename flag_call_result<Function, T...>::type result_type;
    return flag_call_with(*static_cast<Function*>(function), values, flag_types<T...>(),
                          typename flag_make_indices<sizeof...(T)>::type(), std::is_void<result_type>());
}

template <typename Function>
void flag_delete(void* function) {
    delete static_cast<Function*>(function);
}

// 大括号里的一个子命令：接手临时的 cu::flag::command。只在构造外层命令时出现。
class flag_subcommand {
public:
    flag_subcommand(flag::command&& child) noexcept;   // 不加 explicit：大括号里直接写 command(...)

    flag_node* node() const noexcept { return node_; }

private:
    flag_node* node_;
};

}  // namespace detail

namespace flag {

// 一个选项，值是 bool、std::int64_t 或 std::string，分别写成 cu::flag::boolean、int64、string。不可复制。
template <typename T>
class option {
    static_assert(std::is_same<T, bool>::value || std::is_same<T, std::int64_t>::value ||
                      std::is_same<T, std::string>::value,
                  "cu::flag::option holds bool, std::int64_t or std::string");

public:
    // names 是不带 '-' 的名字，第一个是主名字。一个字母的写成 -v，长的写成 --verbose；命令行里两种写法都认。
    // help 和 names 里的字符串要比选项活得久，一般是字面量。
    // 全局对象和 main 里 init 之前的局部变量是全局选项，命令行里哪一级都认；写在命令里的只属于这个命令，值作为参数
    // 传给它的函数。别的地方（命令的函数里）声明，以及名字是空的、以 '-' 开头、含 '=' 或空白、是 help 或 h、和能同时
    // 用的选项重名，都打印诊断后终止。
    option(T default_value, const char* help, std::initializer_list<const char*> names)
        : value_(std::move(default_value)), ready_(false) {
        detail::flag_register(kind(), &value_, &ready_, help, names);
    }

    ~option() { detail::flag_unregister(&value_); }

    option(const option&) = delete;
    option& operator=(const option&) = delete;

    // 全局选项解析完的值，命令行里没给时是默认值。在 init 之前读，打印诊断后终止。
    const T& value() const noexcept {
        if (!ready_) {
            detail::flag_read_too_early(&value_);
        }
        return value_;
    }

    operator const T&() const noexcept { return value(); }

private:
    friend class command;   // 写在命令里的选项由命令接手

    static detail::flag_kind kind() noexcept;

    mutable T value_;      // 解析时由 src/flag/flag.cpp 写入；选项声明成 const 也得能写
    mutable bool ready_;
};

template <>
inline detail::flag_kind option<bool>::kind() noexcept {
    return detail::flag_kind::boolean;
}

template <>
inline detail::flag_kind option<std::int64_t>::kind() noexcept {
    return detail::flag_kind::int64;
}

template <>
inline detail::flag_kind option<std::string>::kind() noexcept {
    return detail::flag_kind::string;
}

using boolean = option<bool>;
using int64 = option<std::int64_t>;
using string = option<std::string>;

// 一个命令。顶层命令写成全局对象；子命令直接写在外层命令的大括号里，可以一层层套。不可复制。
// 名字是空的、以 '-' 开头、含 '=' 或空白，或者同一层有两个命令同名，打印诊断后终止。
class command {
public:
    // 有子命令的命令：命令行里它后面必须跟一个子命令。
    command(const char* name, const char* help, std::initializer_list<detail::flag_subcommand> subcommands);

    // 不再往下分的命令：最后一个参数是函数（普通函数、lambda 或函数对象），前面是这个命令自己的选项，直接写
    // cu::flag::string(...) 这样的临时对象。选中时 init 解析完参数，把这些选项的值按顺序传给函数
    // （bool、std::int64_t、std::string，以常量引用传），再用函数的返回值退出进程；函数不返回值时按 0 退出。
    // 函数的参数和选项对不上、返回值不是 int 或 void，编译报错。两个同类型的选项写反了顺序查不出来。
    template <typename... Parts>
    command(const char* name, const char* help, Parts&&... parts) : node_(detail::flag_new_command(name, help)) {
        static_assert(sizeof...(Parts) >= 1,
                      "cu::flag::command: give the command a function, or subcommands in braces");
        attach(detail::flag_types<Parts...>(),
               typename detail::flag_make_indices<(sizeof...(Parts) > 0 ? sizeof...(Parts) - 1 : 0)>::type(),
               std::forward_as_tuple(std::forward<Parts>(parts)...));
    }

    ~command();

    command(const command&) = delete;
    command& operator=(const command&) = delete;

private:
    friend class detail::flag_subcommand;

    template <typename Part, typename Option>
    void adopt(Option& item) {
        static_assert(detail::flag_option_traits<typename std::decay<Part>::type>::is_option,
                      "cu::flag::command: everything before the function must be an option, such as "
                      "cu::flag::string(...)");
        static_assert(!std::is_lvalue_reference<Part>::value,
                      "cu::flag::command: write the command's options in place; global options are read with value()");
        detail::flag_adopt(node_, &item.value_);
    }

    // parts 是 std::forward_as_tuple 得到的引用元组：前 sizeof...(I) 个是选项，最后一个是函数。
    template <typename... Parts, std::size_t... I, typename Tuple>
    void attach(detail::flag_types<Parts...>, detail::flag_indices<I...>, Tuple&& parts) {
        int adopted[] = {0, (adopt<typename detail::flag_part<I, Parts...>::type>(std::get<I>(parts)), 0)...};
        (void)adopted;

        typedef typename detail::flag_part<sizeof...(I), Parts...>::type function_part;
        typedef typename std::decay<function_part>::type function_type;
        typedef typename detail::flag_call_result<function_type,
                                                  typename detail::flag_part_value<I, Parts...>::type...>::type
            result_type;
        static_assert(!std::is_same<result_type, detail::flag_not_callable>::value,
                      "cu::flag::command: the function must take the command's option values in order "
                      "(bool, std::int64_t, std::string)");
        static_assert(std::is_void<result_type>::value || std::is_same<result_type, int>::value,
                      "cu::flag::command: the function must return int or void");

        int (*const call)(void*, void* const*) =
            &detail::flag_call<function_type, typename detail::flag_part_value<I, Parts...>::type...>;
        function_type* const function =
            new function_type(std::forward<function_part>(std::get<sizeof...(I)>(parts)));
        detail::flag_set_function(node_, function, call, &detail::flag_delete<function_type>);
    }

    detail::flag_node* node_;
};

// 解析命令行。--help 把帮助打印到 stdout、退出码 0；参数不对时把一行错误和一行提示打印到 stderr、退出码 2。
// 有命令时，解析完以后调选中的命令的函数，再用它的返回值退出进程，所以不返回；没有命令时解析完就返回。
// Windows 上 argv 是 main 收到的那一份时，改从系统取 UTF-16 的命令行转成 UTF-8。只能调一次。
void init(int argc, char* argv[]);

// 位置参数：不是选项、也不是命令名的参数，按命令行里的顺序，"--" 以后的全算。解析完之前调，打印诊断后终止。
const std::vector<string_view>& args();

}  // namespace flag
}  // namespace cu
