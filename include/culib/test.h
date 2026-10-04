#pragma once

// 单元测试框架：用法接近 Google Test，只有 TEST 和 EXPECT 两个宏。
//
//     #include <culib/test.h>
//
//     TEST(math, adds_numbers) {              // 组名, 测试名
//         EXPECT(1 + 1, ==, 2);               // EXPECT(a, op, b)
//     }
//
//     int main(int argc, char* argv[]) {     // 整个测试程序只需要一个 main
//         return wavvix::test::run_all(argc, argv);
//     }
//
// EXPECT(a, op, b)：
// - op 只能是 ==、!=、<、<=、>、>=；a 和 b 各只求值一次。
// - 有符号与无符号整数按数学值比较；两边都是 C 字符串（char 指针或数组）时比较内容。
// - 失败时打印位置、表达式和两边的值，测试继续执行。EXPECT 返回 bool，
//   需要失败就停时写 if (!EXPECT(p, !=, nullptr)) { return; }。
// - 操作数里有不在括号内的逗号（如模板参数）时，给操作数再加一层括号。
// - 只能在运行测试的线程里调用。
//
// 与其他库的 TEST / EXPECT 冲突时，在包含本头文件之前定义 WAVVIX_NO_TEST_SHORT_NAMES，
// 改用 WAVVIX_TEST / WAVVIX_EXPECT。
//
// EXPECT 的实现在 culib/detail/expect.h，使用时不需要阅读。

#include <culib/detail/expect.h>
#include <culib/detail/macros.h>

namespace wavvix {
namespace test {

// 运行全部已注册的测试，返回进程退出码：0 表示全部通过，1 表示有测试失败，2 表示参数错误或测试重名。
int run_all(int argc, char* argv[]);

namespace detail {

using test_function = void (*)();

// TEST 宏生成的静态对象，构造时把测试加入注册表。
class registrar {
public:
    registrar(const char* suite, const char* name, test_function function, const char* file, int line);
};

}  // namespace detail
}  // namespace test
}  // namespace wavvix

// 组名和测试名直接字符串化，不会被展开，因此可以和宏同名（例如组名就叫 defer）。
#define WAVVIX_TEST(suite, name) WAVVIX_DETAIL_TEST(#suite, #name, WAVVIX_DETAIL_UNIQUE_ID)

// 先把唯一编号展开成数字，保证下面三处用的是同一个编号。
#define WAVVIX_DETAIL_TEST(suite_text, name_text, id) WAVVIX_DETAIL_TEST_IMPL(suite_text, name_text, id)

#define WAVVIX_DETAIL_TEST_IMPL(suite_text, name_text, id)                                           \
    static void WAVVIX_DETAIL_CONCAT(wavvix_test_, id)();                                           \
    static const ::wavvix::test::detail::registrar WAVVIX_DETAIL_CONCAT(wavvix_test_registrar_, id)( \
        suite_text, name_text, &WAVVIX_DETAIL_CONCAT(wavvix_test_, id), __FILE__, __LINE__);        \
    static void WAVVIX_DETAIL_CONCAT(wavvix_test_, id)()

#define WAVVIX_EXPECT(a, op, b)                                                                            \
    ::wavvix::test::detail::check(::wavvix::test::detail::capture(a) op (b), #a, #op, #b, __FILE__, __LINE__)

#ifndef WAVVIX_NO_TEST_SHORT_NAMES
#define TEST WAVVIX_TEST
#define EXPECT WAVVIX_EXPECT
#endif
