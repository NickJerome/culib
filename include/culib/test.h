#pragma once

// 单元测试宏与运行器。

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
#define WAVVIX_TEST(suite, name)                                                                 \
    WAVVIX_DETAIL_ALLOW_COUNTER_BEGIN WAVVIX_DETAIL_TEST(#suite, #name, WAVVIX_DETAIL_UNIQUE_ID) \
    WAVVIX_DETAIL_ALLOW_COUNTER_END

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
