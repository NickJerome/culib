# result 设计

- 日期：2026-10-01
- 状态：已实现；2026-10-10 收敛为固定 `error`，第 11 节保留早期测量与决策记录

## 1. 背景与目标

README 约定：可能失败的操作通过 `result` 返回结果或错误，库不主动抛出异常。本文设计这个 `result`。

目标是通俗易懂：用起来像 `std::optional`，成功时 return 值，失败时 return 错误，用 if 判断，不引入新的概念。同时满足 C++11、不分配内存、不依赖异常。命名与标准库一致，使用 snake_case。

讨论过程中考虑过照搬 Rust 的 `Result`（`ok(...)` / `err(...)`、`match`、`map` / `and_then` 链式调用、`?` 错误传递）。C++11 只能模仿它的外形，`match` 和 `?` 都没有对应的语法，反而多出一套 C++ 程序员不熟悉的写法，因此不采用。

## 2. 范围

**做：**

- `result<T>` 和只关心成败的 `result<void>`；错误类型固定为 `error`。
- 构造：成功时直接从值构造，失败时直接从 `make_error(...)` 构造。
- 查询与取值：`is_ok()`、`is_err()`、`value()`、`error()`、`value_or()`。
- 错误类型 `error` 和错误种类 `error_kind`。
- 错误传递：调用方手写 `if (x.is_err()) return std::move(x).error();`。**不提供宏**，理由见 11.10。

**不做：**

- 表达式形式的错误传递（类似 Rust 的 `?`）。现有 C++ 语法里，能写在表达式中间的写法只有两种：依赖 GCC / Clang 扩展的表达式宏（cl.exe 不支持），或者用异常实现。本机实测，用异常实现时出错路径约慢 670 倍，因此都不采用。语句形式的宏也评估过并曾经实现，后来删除，见 11.10。
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

// 跨层把错误传上去时，在层边界上把上下文拼进消息（见 11.7：错误类型只有 error 一种）
result<config> load(const std::string& path) {
    result<int> port = parse_port(read_key(path, "port"));
    if (port.is_err()) {
        return make_error(port.error().kind(),
                          "读 " + path + ": port = " + port.error().message());
    }
    return config{port.value()};
}
```

## 4. 接口

### 4.1 result

```cpp
namespace wavvix {

template <typename T>
class result {
public:
    // 成功：从能转换成 T 的值构造；失败：从 error 构造
    template <typename U> result(U&& value);
    template <typename G> result(G&& err);

    bool is_ok() const noexcept;
    bool is_err() const noexcept;

    // 取值；类型不对时打印信息并终止程序
    T& value() &;
    const T& value() const&;
    T&& value() &&;
    wavvix::error& error() &;
    const wavvix::error& error() const&;
    wavvix::error&& error() &&;

    // 失败时返回 fallback
    template <typename U>
    T value_or(U&& fallback) const&;
    template <typename U>
    T value_or(U&& fallback) &&;
};

template <>
class result<void>;  // 默认构造即为成功，写作 return {};

}  // namespace wavvix
```

- 两个构造函数通过模板约束区分：参数只能转换成 `T` 的走成功，只能转换成 `error` 的走失败。若同时可转换成两者，则拒绝构造。复制与移动 result 自身使用特殊成员。
- `T` 不能与 `error` 互相隐式转换；违反时用 `static_assert` 报错。
- 值必须是非数组、非 cv 限定的对象类型，析构不能抛异常；`void` 值由专门版本支持。
- 值类型的 `result<T>` 没有默认构造，不存在「空」的状态；`result<void>` 默认构造表示成功。
- 复制、移动随 `T`：只能移动的值（例如 `std::unique_ptr`）使 result 只能移动。
- 赋值（复制和移动）：只有 `T` 和 `error` 的移动构造都不抛异常时才支持，复制赋值还要求 `T` 能复制；否则禁止赋值，避免赋值到一半失败后处于无效状态。
- 上面两条的「不支持」通过特殊成员函数体里的 `static_assert` 实现：函数体只在真的用到时才实例化，所以误用仍然是编译错误，报错信息是一句英文说明。代价是 `std::is_copy_constructible` 等类型特征只看声明，对 `result` 会乐观地回答「可以」。按类型能力删除特殊成员可以让类型特征准确，但需要四个空基类按位组合继承（约 50 行只为编译器服务的代码），对一个小团队自用的库不值得。

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

### 4.3 错误传递：手写 if，不提供宏

自己不处理错误、原样交给调用方时：

```cpp
result<config> load(const std::string& path) {
    result<std::string> text = read_file(path);
    if (text.is_err()) return std::move(text).error();

    result<int> port = parse_port(text.value());
    if (port.is_err()) return std::move(port).error();

    return config{text.value(), port.value()};
}
```

- **`std::move` 不能省。** `error()` 有左值 / const / 右值三个重载，`x.error()` 在左值上拿到 `error&`，返回的 result 会复制构造错误。实测（长于 SSO 的消息、全局 `operator new` 计数）：`return x.error()` 两次分配，`return std::move(x).error()` 一次。这里没有 RVO 可用——错误是 `x` 的子对象而不是返回值本身。漏写能编译且零警告，所以全库统一写 `std::move(x).error()`。
  - `std::move(x.error())` 的分配次数相同、功能等价，仍然用 `std::move(x).error()`：和标准库一个形状（`std::move(opt).value()`），而且它表达的是「整个 result 被消费掉了」——移走之后 `x` 的消息就是空的，`return` 必须紧跟其后。
- **本节曾经描述一个 `WAVVIX_TRY` 宏，它被实现过又被删除了**，完整理由见 11.10。一句话：它展开后藏着一个 `return`，写出来却像变量声明。

## 5. 规则

- **取错类型**：对失败结果调用 `value()`，或对成功结果调用 `error()`，会向 stderr 打印一行信息后调用 `std::abort()`。release 版本同样检查，代价是一次判断。打印内容：

  ```
  wavvix::result::value() called on an error: invalid_argument: port is empty
  wavvix::result::error() called on a success value
  ```

  诊断打印 `error` 的种类和说明；直接写 stderr，不临时创建字符串。
- **忽略返回值的警告**：类上标注 `WAVVIX_NODISCARD`，在 C++17 编译器上展开为 `[[nodiscard]]`，在 Clang 的 C++11 模式下展开为 `warn_unused_result` 属性，其他情况为空。因此 GCC 在 C++11 模式下没有这个警告。Clang 能否把该属性用在类上，实现时先验证，不支持则同样为空。
- **右值版本**：`value()`、`error()`、`value_or()` 对右值 result 都有移动版本，例如 `auto text = read_file(path).value();` 不会复制字符串。

## 6. 性能

- 值和错误放在同一块内存（union）里，只多一个标志位；不分配内存。
- `error` 带一个 `std::string`，`result<int>` 因此是 48 字节，并且不可平凡复制；说明文字超过短字符串优化的长度时，出错会分配一次内存。**成功路径不分配**——union 只构造活着的那个成员。
- 本节原先据此规定「一般接口用 `error`、性能敏感的接口用 `enum class`」。**那条规定已撤销**：量过之后发现枚举的优势在本库的场景里兑现不了，全库统一用 `error`，数据和理由见 11.7。
- 先判断 `is_err()` 再调用 `value()` 时，`value()` 里的重复检查在开启优化后会被编译器去掉。

## 7. 实现要点

- 存储位于内部头文件 `detail/result_storage.h`：只构造当前有效的成员，用不了的特殊成员靠函数体里的 `static_assert` 挡住（见 4.1 最后一条）。不可赋值但可以构造的值通过销毁重建实现 result 的赋值。
- 两个构造函数用 SFINAE 区分；`T` 与 `error` 能互相转换时用 `static_assert` 给出说明。
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
- 复制、移动、赋值。被禁止的组合（复制只能移动的 result、移动构造会抛异常时的赋值）不能用 `static_assert` 检查，因为类型特征只看声明；这些改为手工编译失败验证，见 11.3。
- 手写传播：取值成功后继续、失败时向上传播，值分支与 `void` 分支各一处。
- `error`：`make_error()`、`to_string()`、`error_kind_name()`、`operator<<`；作用域里有名为 `error` 的局部变量时，`make_error(...)` 仍能正常使用。
- 误用访问的诊断与实际终止：用独立子进程验证退出信号和 stderr，避免中断单测进程；不为测试额外暴露诊断接口。
- `T` 与 `E` 能互相转换时的编译报错，以及取错类型时的终止程序，单测无法覆盖，实现时手动验证。

开销实测，方法和 `defer` 相同：

- 返回 `result<int, 枚举>` 与返回手写的错误码结构体对比汇编。
- 「先判断 `is_err()`，再调用 `value()`」与手写的单次判断对比汇编，确认重复检查被去掉。

## 9. 对现有代码和文档的修改

- README：
  - 「错误处理」一节补充 result 的用法示例，当时还包含 `WAVVIX_TRY`（见 11.10）。该节 2026-10-03 从「开发约定」提为顶级章节，作者侧约束移入 `CLAUDE.md`。
  - 待定事项删掉「result 的具体设计」。

## 10. 实现顺序

排在单测框架之后实现，测试直接使用新框架。整体顺序以 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 6 节为准；下一步是 lexical_cast。


## 11. 实现验证

实现于 2026-10-03 完成，同日按本节第 4 小节瘦身并补入 `WAVVIX_TRY`（次日删除，见 11.10），以下记录瘦身后的状态。环境为 Linux / GCC 14.2，`-Wall -Wextra -Wpedantic -Werror`，全程零警告。

### 11.1 构建与测试

- release（`-O2`）与 debug（`-O0 -g`），C++11：65 个测试通过，其中 result 11 个。
- ASan + UBSan，并关闭复制消除（`-fno-elide-constructors`）：65 个测试通过。
- C++17：65 个测试通过；忽略 result 返回值会触发 nodiscard 警告（GCC 的 C++11 模式下该宏为空，确认无警告）。
- `-fno-exceptions -fno-rtti`：61 个测试通过。3 个异常专用测试按条件编译排除；另排除 1 个已知的 stopwatch 自定义 locale 测试，其崩溃已在不含 culib 的标准库程序中复现，与 result 无关。
- `result.h`、`detail/result_storage.h`、`culib.h` 均可独立包含。
- 单独编译并链接 `examples/result.cpp`，运行输出符合预期。

### 11.2 开销

GCC `-O2` 汇编：`result<int, enum>` 为 8 字节、可平凡复制，`static_assert` 在测试中固定这两点。「构造 result 返回 → 判断 `is_err()` → 取 `value()`」整个调用点与手写 `struct { int; bool; }` 的等价写法生成逐条相同的指令（`leal` / `testl` / `movl` / `cmovs` / `ret`），没有重复检查。

**这条只在被调函数能内联时成立——原文漏了这半句。** 把被调函数放进另一个翻译单元（不开 LTO，静态库里的普通函数就是这种）之后，调用点多一次栈往返：

```
result:                              手写 union{int,enum}+bool:
  call  inner                          call  inner_fair
  movq  %rax, %rdx                     movq  %rax, %rdx
  movq  %rax, 8(%rsp)   ← 存栈         shrq  $32, %rdx
  shrq  $32, %rdx                      testb %dl, %dl
  testb %dl, %dl                       je    .L8
  je    .L3                            ret        ← 值已在 eax，直接返回
  movl  8(%rsp), %eax   ← 读回
  ret
```

原因是 `value()` 返回 `T&`：**取引用就要求对象有地址**，于是本来活在寄存器里的 8 字节 result 被迫落栈。代价 8.5 对 4.3 ns（跨 TU，100 万次/轮 × 5 轮取最小）。内联之后两边都是 `testl / js / movl / ret`，栈访问次数都是 0，确实逐条相同。

不改接口（按值返回只对小的平凡类型成立，要靠 `conditional` 挑返回类型，换来的 4 ns 不值那个复杂度）。它给「热路径上的小函数放头文件」这条开发约定提供了一个带数字的理由。

**不再是保留平凡/非平凡两个存储特化的理由**——见 11.7，错误类型统一成 `error` 之后，平凡那一侧在库内没有消费者了。

### 11.3 误用

- 独立子进程验证 8 种错误访问，release 下全部打印预期内容并触发 SIGABRT：默认 `error` 带说明与不带说明、`result<void>` 的错误分支、有符号枚举、无符号枚举、`std::string` 作错误类型、以及对成功结果调用 `error()` 的值版本与 void 版本。枚举和字符串错误只打印前半句。
- 编译失败验证 5 种误用，均在用到的那一行报出可读的英文说明：值与错误同类型、可互转类型、输入可转为两个分支、复制只能移动的 result、移动构造会抛异常时的赋值。后两种原先由四个空基类按位组合删除特殊成员来保证，瘦身后改由 `static_assert` 保证。

### 11.4 本次瘦身

动机是实现里「为编译器服务」的代码偏多，不够轻量。改动：

- 删掉 `result_copy_control` 等四个按位组合继承的空基类和 `result_traits`（约 50 行），改为在特殊成员函数体里 `static_assert`。代价见 4.1 最后一条：类型特征不再准确。
- 删掉枚举诊断的 `result_bad_signed_value` / `result_bad_unsigned_value` 两个函数和一组 `is_enum` SFINAE 分发，理由见第 5 节。
- 删掉 `result_base`：它只放三条 `static_assert` 和一个带 `std::conditional` 条件 noexcept 的转发构造函数，前者移入两个 result 版本的类体，后者随之消失，`result` 直接继承存储。
- GCC 5 以前的 `__has_trivial_copy` 等内置特性回退改为「一律走非平凡存储」。
- 新增 `WAVVIX_TRY` / `WAVVIX_TRY_VOID`（**次日即删除，见 11.10**）。两个宏复用 `WAVVIX_DETAIL_UNIQUE_NAME`（`defer` 用的同一个），名字在传给 `_IMPL` 之前就拼好，因此不需要 `TEST` 那样的两层展开，也不会让 MSVC 的 `__COUNTER__` 多取几次。
- 禁止 cv 限定的值和错误类型（`result<const int>` 之类本来也没有意义），换来 `construct()` 里的 placement new 写成一行 `::new (std::addressof(destination)) V(...)`，不再需要 `const_cast<void*>(static_cast<const volatile void*>(...))` 这串用来伺候 cv 类型的转换。
- `result.cpp` 里「种类名 + `: ` + 说明」这条格式规则原先写了三遍（`to_string()`、`operator<<`、abort 诊断）。现在 `to_string()` 是唯一定义，`operator<<` 直接 `os << value.to_string()`；abort 诊断仍不分配内存，但收成一次 `fprintf`，用 `%s%s%.*s` 表达「说明为空时连 `: ` 一起省掉」。三个终止函数共用一个 `die()`，`std::abort()` 只出现在两处。

净效果：`result.h` 129 → 163 行（`WAVVIX_TRY` 及其用法注释 +36；删除后 141 行，比加宏前多 12 行手写传播的说明注释），`detail/result_storage.h` 178 → 127 行，`result.cpp` 67 → 58 行，合计 374 → 348 行。实现本身减少 62 行，其余是新增功能。

### 11.5 试过但没采纳的简化

和 `defer`（36 行代码、单头文件、无 `.cpp`）相比 result 仍有 7 倍体量，以下两处是实测后确认由语言规则锁死、不是取舍问题：

- **把两个存储特化共用的 union 和 tag 构造函数提到一个公共基类**：含非平凡成员的 anonymous union 在不声明析构函数时隐式析构函数被删除，所以公共基类必须自己声明析构函数；而一旦声明，平凡那条特化就不再平凡可复制，11.2 的寄存器返回随之失效。两个特化里重复的那段构造函数是保住平凡性的代价。
- **把两段 SFINAE 构造函数换成 `result(T)` / `result(E)` 两个普通重载**（可省约 10 行密集模板）：`result<std::string> text = "hello";` 会编译失败，因为那需要 `const char[6]` → `std::string` → `result<std::string>` 两次用户定义转换，而隐式转换序列只允许一次。模板构造函数让 `U&&` 直接绑定数组、把转换放到构造函数内部完成，是「成功时直接 return 值」对需要转换的值也成立的前提。顺带一提，构造函数模板上那条 `!std::is_same<std::decay<U>::type, result>` 守卫也是必需的：去掉它，无约束的 `U&&` 会在重载决议里压过复制构造函数。

另外两处是权衡后主动不做：

- **先实现一个 `variant`，再把 result 建在它上面**。方向是反的。支撑 result 现有保证（平凡性穿透、生命周期、按索引就地构造）的「两分支 variant」就是现在的 `detail/result_storage.h` 本身——实测把一个 `optional<T>` 直接建在 `detail::result_storage<T, nullopt_t>` 上只需 12 行，`optional<int>` 仍是 8 字节平凡可复制，`optional<std::string>` 走非平凡路径，复制和 move-only 都正确。也就是说可复用的原语已经存在了。真正的 `variant<Ts...>` 要多出 N 元递归联合、索引类型、`visit` 的分派、以及 `std::variant` 最难的那部分——跨 N 个类型做重载决议的转换构造函数；libstdc++ 的实现约 1700 行。result 只会在 N=2 上使用它，等于为用不到的 N 元能力付编译期和可读性的代价。结论：不写 `variant`；等 `optional` 真正要做时，把这个两分支存储换个与 result 无关的名字共用，在那之前不动（README 的 base 模块列的是 `make_unique` / `optional` / `string_view`，没有 `variant`）。
- **让 `result<void, E>` 私有继承 `result<detail::result_unit, E>`**，以消掉 `is_ok` / `is_err` / `error()` 三个重载 / `check_error` 在两个版本间的重复。约省 5 行，但「特化私有继承主模板并塞一个占位类型」属于聪明而不清楚的写法，还多一层要验证的平凡性传递。那 12 行重复留着。

### 11.6 未覆盖

当前环境没有 xmake 或 Clang，未执行 xmake、Clang、Windows/MSVC、macOS 以及最低版本编译器（GCC 4.8.1 / VS2015）验证。GCC 5 以前的非平凡存储回退路径同样未实测。构建脚本使用源文件通配符，新模块和测试自动纳入现有目标。

### 11.7 错误类型统一成 error（2026-10-04）

本节保留当时的测量和取舍记录。文中「仍保留 `E`」是当时的状态；当前接口见 11.11。

原来的约定是「一般接口用 `error`，失败很常见且对性能敏感的接口用 `enum class`」。撤掉，**全库只用 `error`**。

实测（GCC 14.2 `-O2`，跨 TU 不可内联，100 万次/轮 × 5 轮取最小，分配次数用全局 `operator new` 计数）：

| 路径 | `result<int, error>` | `result<int, 枚举>` | 分配 |
| --- | --- | --- | --- |
| 成功 | 11.4 ns | 8.5 ns | 都是 0 |
| 失败，`make_error(kind)` 不带消息 | 14.8 ns | 8.2 ns | 都是 0 |
| 失败，带一句消息 | 26.7 ns | — | 1 |
| 失败，补完上下文（文件 + 字段 + 值） | 120.8 ns | — | 2 |

两点让这个取舍成立：

- **成功路径两边都是 0 分配。** union 只构造活着的那个成员，成功时 `error` 的 `std::string` 根本没被构造，只剩析构里一个分支和 48 字节走隐藏指针，共 ~3 ns。
- **最坏的失败路径是枚举的 15 倍，但绝对值只有 120 ns。** 要吃掉 1% 的一个核得持续每秒 8.3 万次失败；三个目标场景里没有这样的地方，网络侧光收一个坏包的系统调用就 1–2 µs，比它贵十倍。所以这是量过之后确认不要紧，不是假设不要紧。

换来的：

1. **层边界上不用写一次显式转换。** 两种错误类型时，返回 `result<T, error>` 的函数里直接 `return std::move(x).error()` 一个 `result<U, 枚举>` 的错误会编译失败（`could not convert 'parse_failure' to 'wavvix::result<int>'`），每处都要手写一次映射。
2. **消掉一处「每次都要先想一下」。** 原判据是「算不算性能敏感」——用性能标准回答语义问题。
3. 丢掉的是「按失败种类分支」。可接受，理由见预期接口第 2 节。

当时 **`E` 仍然是模板参数**，传小而平凡可复制的结构体也成立（实测 `struct { parse_error kind; uint32_t position; }` 下 `result` 为 12 字节、仍可平凡复制、`rax:rdx` 返回）。它只被当作未来的逃生口，culib 自己的 API 没有用。

后果：**`result_storage<T, E, true>`（平凡存储特化）在库内不再有消费者。** 当时决定等 `fs` 和 `net` 做完再看；后续确认只有测试在用，按 11.11 删除。

### 11.8 评估过但没做的错误接口

- **多态基类 + `virtual what()`（「像异常那样」）。** 要多态就得按指针持有 → 每次失败都分配；恢复具体类型要 `dynamic_cast`，而库明确支持 `-fno-rtti`，自带类型标签则等于「枚举 + 一个 vtable」；`result<int, E>` 变 16 字节且非平凡，寄存器返回和两个存储特化的意义全没。**决定性的一条**：真异常在不抛时 happy path 上没有指令代价，而 `result` 里的多态对象是每次调用都付——这是异常的成本结构配非异常的收益。要异常语义，异常本身严格更好。
- **CRTP 静态接口。** 实测是零成本的：`struct parse_failure : error_interface<parse_failure>` 下空基类优化生效，8 字节、仍可平凡复制、无 vtable、静态分发可内联；基类的默认实现能让一个成员都没有的错误类型直接拿到 `to_string()`；而且基类**有真正的查询者**——受约束的 `template <typename D> operator<<(ostream&, const error_interface<D>&)`，没有基类就只能写无约束的模板、会劫持所有类型（所以它不同于 11.4 删掉的那四个空基类，那些没有查询者）。

  不做的理由是两条。一，**它解决的不是问题所在**：静态多态没有类型擦除，所以跨层仍然不能转换（实测同样的编译错误），而上下文的缺失是结构性的——`parse_failure::message()` 知道「第 2 个字节」，它从来没拿到过「这是 /etc/app.conf 的 port 字段」，换 virtual `what()` 也一样。二，**C++11 的代价**：带基类的类在 C++17 前不是聚合体，`parse_failure{kind, pos}` 在 C++11 / C++14 下直接编译失败（C++17 要写成 `{{}, kind, pos}`），每个错误类型得手写一个构造函数——正好抵掉「荷载结构体是免费的」那个卖点。

  **重开条件**：出现第三个错误类型时（大概是 `fs` 或 `net` 要带 errno / 平台错误码），那时 `to_string` / `operator<<` 的重复才真实存在。届时先比一下 **ADL 自由函数**（`error_message(e)`，像 `swap` 那样）——它不要基类、保住聚合初始化、没有 C++11 那条税，可能比 CRTP 更便宜。
- **错误链 / cause 列表。** 每层一次分配，而上下文用「在层边界把字符串拼进消息」就够了，实测只多 1 次分配。

### 11.9 删掉 WAVVIX_TRY（2026-10-04）

`WAVVIX_TRY` / `WAVVIX_TRY_VOID` 在 2026-10-03 的瘦身里加入，次日由作者否决并删除。原话：**「谁让你乱加宏的，很明显把内部逻辑藏起来了。」**

```cpp
#define WAVVIX_DETAIL_TRY_IMPL(declaration, expression, holder) \
    auto holder = (expression);                                 \
    if (holder.is_err()) return ::std::move(holder).error();    \
    declaration = ::std::move(holder).value()
```

问题在第二行：**`WAVVIX_TRY(int port, parse_port(text));` 写出来像一句变量声明，展开后却含一个 `return`。** 一个函数有哪几条提前返回的路径，是读代码时最先要确定的事，而这个宏把它藏进了一个看起来不会改变控制流的语法形式里。`defer` 也是宏，但它只在作用域结束时执行一段代码，不会替你返回；`TEST` / `EXPECT` 同理。**判据因此是「宏会不会让人看错控制流」**，不是「宏省了几行」——这条写进了 `CLAUDE.md` 的命名一节。

删除的代价是每处传播多一行 `if`。换到的是每一处 `return` 都在明面上。

两个连带后果：

1. **它曾经是「立项错误上下文」的全部论据，那个论据没了。** 当时的说法是「补上下文就用不了 `WAVVIX_TRY`，5 行对 1 行」。宏删掉之后，`examples/error_context.cpp` 里两种写法都只是一个 `if`，差别只在 `return` 那一行里多一段拼接——**跨层补上下文几乎是免费的**，见 11.10。
2. 11.7 论证统一错误类型时，理由之一写的是「`WAVVIX_TRY` 在层边界上转换不过去」。改成了「每个层边界上要多写一次显式转换」，结论不变但不再依赖这个宏。

**不重开。** 表达式形式（Rust 的 `?`）早在第 2 节就排除了（需要编译器扩展或异常）；语句形式就是这次删掉的东西。

### 11.10 不加补上下文的宏（2026-10-04）

`examples/error_context.cpp` 把「补上下文」和「不补」并排写出来之后，能看到两边都只是一个 `if`：

```cpp
// 不补上下文
result<int> port = to_int(value_of("port"));
if (port.is_err()) return std::move(port).error();

// 补上下文：多的只是 return 那一行里的拼接
result<int> port = to_int(value_of(key));
if (port.is_err()) {
    return make_error(port.error().kind(),
                      "读 " + path + ": " + key + " = " + port.error().message());
}
```

错误类型统一成 `error` 之后，一个 `TRY_CTX(declaration, expression, prefix)` 在技术上很干净（内层错误的 `kind()` 和 `message()` 都拿得到）。**不加**，三条理由，第一条最硬：

- **它和 `WAVVIX_TRY` 是同一个毛病**，同样藏一个 `return`，见 11.9。这一条单独就够了。
- **省下的行数本来就不多。** 这正是「只看输出不看源码」会判断错的地方：我当时以为是 5 行对 1 行，源码一摊开是 1 个 `if` 对 1 个 `if`。
- **这正是不该省的地方。** 上下文的价值全在它具体——哪个文件、哪个字段、什么值。一个吃字符串前缀的宏会鼓励随手写个笼统的 `"load failed"`，那和不补没区别；手写反而逼着把信息写全。

**重开条件**：写完 `fs` + `os` + 配置文件之后，如果那段拼接在很多处长得一模一样，加一个**普通函数** `error with_context(const error&, std::string)`——它不藏控制流，所以不受 11.9 那条判据的约束。宏仍然不加。

### 11.11 删除未使用的错误类型参数（2026-10-10）

`fs`、`net` 和其他模块完成后，源码里所有 `result` 都使用 `error`；自定义错误类型只出现在 `result_test.cpp`。目前也没有下游消费者。把公开接口收敛为 `result<T>` / `result<void>`，删去 `E` 参数、平凡存储特化及只验证这些未使用用法的测试。

这不会改变现有库调用点的成功路径：它们本来就使用含 `std::string` 的 `error`，走非平凡存储。11.2 中 `result<int, 枚举>` 与手写错误码结构逐条相同的汇编仍是有效的历史测量，但已不描述当前可调用接口。若将来真的出现热失败路径，应先量该调用点，再决定是否需要另一种错误表示。
