#pragma once

// culib 内部使用的通用宏，使用方不应依赖。

#define CU_DETAIL_CONCAT_IMPL(a, b) a##b

// 先展开参数再拼接，例如 CU_DETAIL_CONCAT(name_, __LINE__) 得到 name_42
#define CU_DETAIL_CONCAT(a, b) CU_DETAIL_CONCAT_IMPL(a, b)

// 计数器支持同一行多次展开；缺失时退回行号。
#if defined(__COUNTER__)
#define CU_DETAIL_UNIQUE_ID __COUNTER__
#else
#define CU_DETAIL_UNIQUE_ID __LINE__
#endif

// 展开 CU_DETAIL_UNIQUE_ID 的宏用这一对包住。clang 22 起，-Wpedantic 把 __COUNTER__ 报成
// 扩展（-Wc2y-extensions：C2y 才把它标准化），使用方开着 -Werror 写一句 defer 就编不过。
// 只在认识这条警告的 clang 上屏蔽，老版本不用多屏蔽一条「未知警告」。
#if defined(__clang__) && defined(__has_warning)
#  if __has_warning("-Wc2y-extensions")
#    define CU_DETAIL_ALLOW_COUNTER_BEGIN \
         _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wc2y-extensions\"")
#    define CU_DETAIL_ALLOW_COUNTER_END _Pragma("clang diagnostic pop")
#  endif
#endif
#ifndef CU_DETAIL_ALLOW_COUNTER_BEGIN
#  define CU_DETAIL_ALLOW_COUNTER_BEGIN
#  define CU_DETAIL_ALLOW_COUNTER_END
#endif

// 生成唯一的标识符，例如 CU_DETAIL_UNIQUE_NAME(cu_defer_)
#define CU_DETAIL_UNIQUE_NAME(prefix) CU_DETAIL_CONCAT(prefix, CU_DETAIL_UNIQUE_ID)

// 编译时是否开启了异常
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define CU_DETAIL_HAS_EXCEPTIONS 1
#else
#define CU_DETAIL_HAS_EXCEPTIONS 0
#endif

// 不内联。_COLD 另外标明只在出错时执行，让正常路径的代码更紧凑。
#if defined(__GNUC__) || defined(__clang__)
#define CU_DETAIL_NOINLINE __attribute__((noinline))
#define CU_DETAIL_NOINLINE_COLD __attribute__((noinline, cold))
#elif defined(_MSC_VER)
#define CU_DETAIL_NOINLINE __declspec(noinline)
#define CU_DETAIL_NOINLINE_COLD __declspec(noinline)
#else
#define CU_DETAIL_NOINLINE
#define CU_DETAIL_NOINLINE_COLD
#endif

// printf 风格的参数检查。GCC 写 gnu_printf：MinGW 上 printf 指 msvcrt 的格式，不认 %zu；
// clang 不认 gnu_printf。
#if defined(__clang__)
#define CU_DETAIL_PRINTF(format_index, first_index) \
    __attribute__((__format__(__printf__, format_index, first_index)))
#elif defined(__GNUC__)
#define CU_DETAIL_PRINTF(format_index, first_index) \
    __attribute__((__format__(__gnu_printf__, format_index, first_index)))
#else
#define CU_DETAIL_PRINTF(format_index, first_index)
#endif

// 是否开着 AddressSanitizer / ThreadSanitizer。__has_feature 要单独一层 #if：
// GCC 和 MSVC 上它不是宏，和 defined 写在同一个 #if 里是语法错误。
#if defined(__SANITIZE_ADDRESS__)
#define CU_DETAIL_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define CU_DETAIL_ASAN 1
#  endif
#endif
#ifndef CU_DETAIL_ASAN
#define CU_DETAIL_ASAN 0
#endif

#if defined(__SANITIZE_THREAD__)
#define CU_DETAIL_TSAN 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define CU_DETAIL_TSAN 1
#  endif
#endif
#ifndef CU_DETAIL_TSAN
#define CU_DETAIL_TSAN 0
#endif

// 按编译器而非语言标准选择，保证 C++11/17 看到相同的类定义。
#if defined(__clang__)
#define CU_NODISCARD __attribute__((warn_unused_result))
#elif defined(__GNUC__) && __GNUC__ >= 7
#define CU_NODISCARD [[nodiscard]]
#elif defined(_MSC_VER) && _MSC_VER >= 1911
#define CU_NODISCARD [[nodiscard]]
#else
#define CU_NODISCARD
#endif
