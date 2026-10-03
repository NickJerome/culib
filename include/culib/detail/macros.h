#pragma once

// culib 内部使用的通用宏，使用方不应依赖。

#define WAVVIX_DETAIL_CONCAT_IMPL(a, b) a##b

// 先展开参数再拼接，例如 WAVVIX_DETAIL_CONCAT(name_, __LINE__) 得到 name_42
#define WAVVIX_DETAIL_CONCAT(a, b) WAVVIX_DETAIL_CONCAT_IMPL(a, b)

// 生成名字用的唯一编号。MSVC 开启 /ZI（Visual Studio 调试配置的默认选项）时
// __LINE__ 不是字面量，拼接名字会编译失败，因此 MSVC 下改用 __COUNTER__。
#if defined(_MSC_VER)
#define WAVVIX_DETAIL_UNIQUE_ID __COUNTER__
#else
#define WAVVIX_DETAIL_UNIQUE_ID __LINE__
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

// 返回值应当检查；不支持类级别属性的编译器保持兼容。
#if __cplusplus >= 201703L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)
#define WAVVIX_NODISCARD [[nodiscard]]
#elif defined(__clang__)
#define WAVVIX_NODISCARD __attribute__((warn_unused_result))
#else
#define WAVVIX_NODISCARD
#endif
