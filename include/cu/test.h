#pragma once

// 单元测试宏与运行器。

#include <cu/detail/expect.h>
#include <cu/detail/macros.h>

namespace cu {
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
}  // namespace cu

// 组名和测试名直接字符串化，不会被展开，因此可以和宏同名（例如组名就叫 defer）。
#define CU_TEST(suite, name)                                                                 \
    CU_DETAIL_ALLOW_COUNTER_BEGIN CU_DETAIL_TEST(#suite, #name, CU_DETAIL_UNIQUE_ID) \
    CU_DETAIL_ALLOW_COUNTER_END

// 先把唯一编号展开成数字，保证下面三处用的是同一个编号。
#define CU_DETAIL_TEST(suite_text, name_text, id) CU_DETAIL_TEST_IMPL(suite_text, name_text, id)

#define CU_DETAIL_TEST_IMPL(suite_text, name_text, id)                                           \
    static void CU_DETAIL_CONCAT(cu_test_, id)();                                           \
    static const ::cu::test::detail::registrar CU_DETAIL_CONCAT(cu_test_registrar_, id)( \
        suite_text, name_text, &CU_DETAIL_CONCAT(cu_test_, id), __FILE__, __LINE__);        \
    static void CU_DETAIL_CONCAT(cu_test_, id)()

#define CU_EXPECT(a, op, b)                                                                            \
    ::cu::test::detail::check(::cu::test::detail::capture(a) op (b), #a, #op, #b, __FILE__, __LINE__)

#ifndef CU_NO_TEST_SHORT_NAMES
#define TEST CU_TEST
#define EXPECT CU_EXPECT
#endif
