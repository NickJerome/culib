#pragma once

// culib 内部使用的通用宏，使用方不应依赖。

#define WAVVIX_DETAIL_CONCAT_IMPL(a, b) a##b

// 先展开参数再拼接，例如 WAVVIX_DETAIL_CONCAT(name_, __LINE__) 得到 name_42
#define WAVVIX_DETAIL_CONCAT(a, b) WAVVIX_DETAIL_CONCAT_IMPL(a, b)

// 计数器支持同一行多次展开；缺失时退回行号。
#if defined(__COUNTER__)
#define WAVVIX_DETAIL_UNIQUE_ID __COUNTER__
#else
#define WAVVIX_DETAIL_UNIQUE_ID __LINE__
#endif

// 展开 WAVVIX_DETAIL_UNIQUE_ID 的宏用这一对包住。clang 22 起，-Wpedantic 把 __COUNTER__ 报成
// 扩展（-Wc2y-extensions：C2y 才把它标准化），使用方开着 -Werror 写一句 defer 就编不过。
// 只在认识这条警告的 clang 上屏蔽，老版本不用多屏蔽一条「未知警告」。
#if defined(__clang__) && defined(__has_warning)
#  if __has_warning("-Wc2y-extensions")
#    define WAVVIX_DETAIL_ALLOW_COUNTER_BEGIN \
         _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wc2y-extensions\"")
#    define WAVVIX_DETAIL_ALLOW_COUNTER_END _Pragma("clang diagnostic pop")
#  endif
#endif
#ifndef WAVVIX_DETAIL_ALLOW_COUNTER_BEGIN
#  define WAVVIX_DETAIL_ALLOW_COUNTER_BEGIN
#  define WAVVIX_DETAIL_ALLOW_COUNTER_END
#endif

// 生成唯一的标识符，例如 WAVVIX_DETAIL_UNIQUE_NAME(wavvix_defer_)
#define WAVVIX_DETAIL_UNIQUE_NAME(prefix) WAVVIX_DETAIL_CONCAT(prefix, WAVVIX_DETAIL_UNIQUE_ID)

// 编译时是否开启了异常
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define WAVVIX_DETAIL_HAS_EXCEPTIONS 1
#else
#define WAVVIX_DETAIL_HAS_EXCEPTIONS 0
#endif

// 不内联并标记为冷路径的函数（只在出错时执行），让正常路径的代码更紧凑
#if defined(__GNUC__) || defined(__clang__)
#define WAVVIX_DETAIL_NOINLINE_COLD __attribute__((noinline, cold))
#elif defined(_MSC_VER)
#define WAVVIX_DETAIL_NOINLINE_COLD __declspec(noinline)
#else
#define WAVVIX_DETAIL_NOINLINE_COLD
#endif

// 按编译器而非语言标准选择，保证 C++11/17 看到相同的类定义。
#if defined(__clang__)
#define WAVVIX_NODISCARD __attribute__((warn_unused_result))
#elif defined(__GNUC__) && __GNUC__ >= 7
#define WAVVIX_NODISCARD [[nodiscard]]
#elif defined(_MSC_VER) && _MSC_VER >= 1911
#define WAVVIX_NODISCARD [[nodiscard]]
#else
#define WAVVIX_NODISCARD
#endif
