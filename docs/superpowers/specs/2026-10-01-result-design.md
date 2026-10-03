# result 设计

- 日期：2026-10-01
- 状态：已实现（2026-10-03）

## 1. 背景与目标

README 约定：可能失败的操作通过 `result` 返回结果或错误，库不主动抛出异常。本文设计这个 `result`。

目标是通俗易懂：用起来像 `std::optional`，成功时 return 值，失败时 return 错误，用 if 判断，不引入新的概念。同时满足 C++11、不分配内存、不依赖异常。命名与标准库一致，使用 snake_case。

讨论过程中考虑过照搬 Rust 的 `Result`（`ok(...)` / `err(...)`、`match`、`map` / `and_then` 链式调用、`?` 错误传递）。C++11 只能模仿它的外形，`match` 和 `?` 都没有对应的语法，反而多出一套 C++ 程序员不熟悉的写法，因此不采用。

## 2. 范围

**做：**

- `result<T, E = error>` 和只关心成败的 `result<void, E>`。
- 构造：成功时直接从值构造，失败时直接从错误构造；默认错误类型的对象用 `make_error(...)` 创建。
- 查询与取值：`is_ok()`、`is_err()`、`value()`、`error()`、`value_or()`。
- 默认错误类型 `error` 和错误种类 `error_kind`。

**不做：**

- 错误传递宏（类似 Rust 的 `?`）。现有 C++ 语法里，能写在表达式中间的写法只有两种：依赖 GCC / Clang 扩展的表达式宏（cl.exe 不支持），或者用异常实现。本机实测，用异常实现时出错路径约慢 670 倍，因此都不采用。语句形式的宏外观不理想，也不采用。
- `match`、`map`、`and_then` 等函数式写法。两种情况都要处理时，用普通的 if / else；以后确有需要再加，不影响已有代码。
- `operator bool`、`operator*`、`operator->`：判断成败和取值各只保留一种写法。
- 引用类型 `result<T&>`，需要时用指针代替。
- 两个 result 之间的 `==` 比较、直接打印 result。
- 系统错误（errno / GetLastError）的接入，等做文件和系统相关模块时再加。

## 3. 用法

```cpp
#include <culib/result.h>

using namespace wavvix;

result<int> parse_port(const std::string& text) {
    if (text.empty()) {
        return make_error(error_kind::invalid_argument, "port is empty");   // 失败：直接 return 错误
    }
    int port = 0;
    // ... 解析 text
    return port;                                                       // 成功：直接 return 值
}

auto port = parse_port(s);
if (port.is_err()) {
    std::cerr << port.error() << '\n';     // 输出：invalid_argument: port is empty
    return;
}
use(port.value());

int p = parse_port(s).value_or(8080);      // 失败时用默认值

// 只关心成败
result<void> save(const config& cfg) {
    // ...
    return {};                              // 成功时 return {}
}

// 失败很常见、对性能敏感时，用枚举作为错误类型
enum class parse_error { empty, not_a_number, overflow };
result<int, parse_error> parse_int(const std::string& text);
```

## 4. 接口

### 4.1 result

```cpp
namespace wavvix {

template <typename T, typename E = error>
class result {
public:
    // 成功：从能转换成 T 的值构造；失败：从能转换成 E 的值构造（都可以隐式转换）
    template <typename U> result(U&& value);
    template <typename G> result(G&& err);

    bool is_ok() const noexcept;
    bool is_err() const noexcept;

    // 取值；类型不对时打印信息并终止程序
    T& value() &;
    const T& value() const&;
    T&& value() &&;
    E& error() &;
    const E& error() const&;
    E&& error() &&;

    // 失败时返回 fallback
    template <typename U>
    T value_or(U&& fallback) const&;
    template <typename U>
    T value_or(U&& fallback) &&;
};

template <typename E>
class result<void, E>;  // 默认构造即为成功，写作 return {};

}  // namespace wavvix
```

- 两个构造函数通过模板约束区分：参数只能转换成 `T` 的走成功，只能转换成 `E` 的走失败。第三种输入类型如果同时能转换成 `T` 和 `E`，拒绝构造，不隐式选择分支。复制与移动 result 自身使用特殊成员，不走这两个构造函数。
- 为了让编译器分得清，`T` 和 `E` 不能互相转换，例如 `result<int, int>`，或者错误类型是可以隐式转成整数的普通枚举。遇到这种情况用 `static_assert` 报错并说明原因；错误类型请用 `enum class` 或类。
- 值与错误必须是非数组对象类型，析构不能抛异常；`void` 值由专门版本支持。
- 值类型的 `result<T, E>` 没有默认构造，不存在「空」的状态；`result<void, E>` 默认构造表示成功。
- 复制、移动随 `T` 和 `E`：两者都能复制时 result 能复制，有一个只能移动时 result 只能移动（例如装 `std::unique_ptr`）。
- 赋值（复制和移动）：只有 `T` 和 `E` 的移动构造都不抛异常时才支持，复制赋值还要求两者都能复制；否则禁止赋值，避免赋值到一半失败后处于无效状态。

### 4.2 error 和 error_kind

```cpp
enum class error_kind {
    unknown,
    invalid_argument,   // 参数或输入内容不合法
    out_of_range,       // 超出范围，比如数字溢出
    not_found,
    already_exists,
    permission_denied,
    timeout,
    unsupported,
    io,                 // 其他输入输出错误
};

class error {
public:
    explicit error(error_kind kind, std::string message = std::string());

    error_kind kind() const noexcept;
    const std::string& message() const noexcept;  // 可以为空
    std::string to_string() const;                 // "invalid_argument: port is empty"，没有说明文字时只有种类名
};

// 创建错误对象，文档和示例统一使用它
error make_error(error_kind kind, std::string message = std::string());

const char* error_kind_name(error_kind kind) noexcept;         // "invalid_argument"
std::ostream& operator<<(std::ostream& os, const error& e);    // 输出 to_string() 的内容
```

- 创建错误统一用 `make_error(...)`，不直接写 `error(...)`。`error` 也是最常用的变量名，作用域里有名为 `error` 的变量或成员函数时，`error(...)` 会指向它们而不是类型，导致编译错误；`make_error` 不会和变量名冲突，命名也与标准库的 `std::make_error_code` 一致。`error` 的构造函数仍然保留。
- 错误种类取名 `error_kind` 而不是 `error_code`，避免和标准库的 `std::error_code` 混淆。
- `result.h` 只引入 `<iosfwd>`，`operator<<` 在 `result.cpp` 里实现。

## 5. 规则

- **取错类型**：对失败结果调用 `value()`，或对成功结果调用 `error()`，会向 stderr 打印一行信息后调用 `std::abort()`。release 版本同样检查，代价是一次判断。打印内容：

  ```
  wavvix::result::value() called on an error: invalid_argument: port is empty
  wavvix::result::error() called on a success value
  ```

  错误类型是 `error` 时打印与 `to_string()` 一致的内容；是枚举时打印它的数值（保留有符号性）；其他类型只说明这是一个错误。诊断直接写 stderr，不临时创建字符串。
- **忽略返回值的警告**：类上标注 `WAVVIX_NODISCARD`，在 C++17 编译器上展开为 `[[nodiscard]]`，在 Clang 的 C++11 模式下展开为 `warn_unused_result` 属性，其他情况为空。因此 GCC 在 C++11 模式下没有这个警告。Clang 能否把该属性用在类上，实现时先验证，不支持则同样为空。
- **右值版本**：`value()`、`error()`、`value_or()` 对右值 result 都有移动版本，例如 `auto text = read_file(path).value();` 不会复制字符串。

## 6. 性能

- 值和错误放在同一块内存（union）里，只多一个标志位；不分配内存。
- `T` 和 `E` 都可平凡复制且支持复制与移动赋值时（例如 `result<int, parse_error>`），result 本身也可平凡复制；体积较小时，函数返回可以走寄存器，和手写的错误码结构体一样快。
- 默认的 `error` 带一个 `std::string`，`result<int>` 因此有几十个字节，并且不可平凡复制；说明文字超过短字符串优化的长度时，出错会分配一次内存。所以：
  - 一般接口用 `error`。
  - 失败很常见、对性能敏感的接口（例如 lexical_cast）用一个 `enum class` 作为错误类型。
- 先判断 `is_err()` 再调用 `value()` 时，`value()` 里的重复检查在开启优化后会被编译器去掉。

## 7. 实现要点

- 「可平凡复制则 result 也可平凡复制」通过按条件选择存储基类实现。存储位于内部头文件 `detail/result_storage.h`，特殊成员按类型能力启用或删除，类型特征查询与实际能力一致。不可赋值但可以构造的值通过销毁重建实现 result 的赋值；这种情况下不承诺 result 可平凡复制。判断条件优先用 `std::is_trivially_copyable` 等标准特性；GCC 5 以前的 libstdc++ 没有这些特性，改用编译器内置的 `__has_trivial_copy` 等。
- 两个构造函数用 SFINAE 区分；`T` 与 `E` 能互相转换时用 `static_assert` 给出说明。
- `WAVVIX_NODISCARD` 放在 `include/culib/detail/macros.h`，与单测框架设计中的 `WAVVIX_DETAIL_CONCAT` 等宏放在一起。
- 打印信息并终止程序的逻辑放在 `result.cpp` 的一个不内联函数里，避免在每个调用点展开。
- 编译期提示信息一律用英文，避免 MSVC 在非 UTF-8 代码页下乱码。

## 8. 文件与测试

- `include/culib/result.h`
- `src/result.cpp`：错误种类的名字、`error::to_string()`、`operator<<`、终止程序前的打印
- `tests/result_test.cpp`：用新的单测框架编写

测试覆盖：

- 构造：从值构造（包括需要转换的情况，如 `"abc"` 构造 `result<std::string>`）、从错误构造、`result<void>` 的成功和失败。
- 查询与取值：`is_ok`、`is_err`、`value`、`error`、`value_or`，左值、const 和右值三种情况。
- 只能移动的值类型（`std::unique_ptr`）。
- 复制、移动、赋值，以及可平凡复制的传递（用 `static_assert` 检查）。
- `error`：`make_error()`、`to_string()`、`error_kind_name()`、`operator<<`；作用域里有名为 `error` 的局部变量时，`make_error(...)` 仍能正常使用。
- 误用访问的诊断与实际终止：用独立子进程验证退出信号和 stderr，避免中断单测进程；不为测试额外暴露诊断接口。
- `T` 与 `E` 能互相转换时的编译报错，以及取错类型时的终止程序，单测无法覆盖，实现时手动验证。

开销实测，方法和 `defer` 相同：

- 返回 `result<int, 枚举>` 与返回手写的错误码结构体对比汇编。
- 「先判断 `is_err()`，再调用 `value()`」与手写的单次判断对比汇编，确认重复检查被去掉。

## 9. 对现有代码和文档的修改

- README：
  - 「开发约定 · 错误处理」补充 result 的用法示例。
  - 待定事项删掉「result 的具体设计」。

## 10. 实现顺序

排在单测框架之后实现，测试直接使用新框架。整体顺序以 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 6 节为准；下一步是 lexical_cast。


## 11. 实现验证（2026-10-03）

本次环境为 Linux / GCC 14.2，C++11，开启 `-Wall -Wextra -Wpedantic -Werror`：

- debug 与 release：64 个测试通过，其中 result 新增 10 个。
- ASan + UBSan，并关闭复制消除：64 个测试通过。
- `-fno-exceptions -fno-rtti`：60 个测试通过。3 个异常专用测试不编译；排除 1 个已知的 stopwatch 自定义 locale 测试，其崩溃已在不含 culib 的标准库程序中复现。
- 独立子进程验证 7 种错误访问：值与 void 的错误分支访问、默认错误、自定义错误、有符号和无符号枚举诊断；release 下均打印预期内容并触发 SIGABRT。
- 编译失败验证 5 种误用：值与错误同类型、可互转类型、输入可转为两个分支、复制只可移动类型、不安全的赋值。
- `result.h`、`detail/result_storage.h`、`culib.h` 可独立包含，且可关闭异常与 RTTI；C++17 下忽略 result 会触发 nodiscard 警告。
- 构建静态库，单独编译并链接 `examples/result.cpp`，运行成功。
- GCC -O2 汇编：`result<int, enum>` 为 8 字节，与手写 `int + bool` 结构相同，返回走寄存器；先判断再取值与手写结构的访问指令相同，没有重复检查。构造返回的指令序列仍有差异，不声称完整汇编一致。

当前环境没有 xmake 或 Clang，未执行 xmake、Clang、Windows/MSVC、macOS 以及最低版本编译器验证。构建脚本使用源文件通配符，新模块和测试自动纳入现有目标。
