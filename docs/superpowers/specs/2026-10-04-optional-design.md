# optional 设计

- 日期：2026-10-04
- 状态：已实现
- 第二程的第一项：`os::getenv` 的返回类型要它，所以它排在 `os` 之前。

## 1. 为什么在有了 result 之后还要它

`result` 回答「成功还是失败，失败的原因是什么」。`optional` 回答「有没有」——**而且不带原因**。

| 「没有」是 | 用 | 例子 |
| --- | --- | --- |
| **正常结果** | `optional` | 环境变量没设置、队列是空的、查表没命中 |
| **失败** | `result` | 文件打不开、输入不是数字、权限不足 |

判据是**调用方需不需要知道为什么**。`os::getenv("HOME")` 返回空时没有「为什么」可说——那不是失败，是一个完全正常的答案。包成 `result` 会迫使调用方处理一个不是错误的错误，并且要为一个不存在的原因编一个 `error_kind`。

**先有消费者**：`os::getenv` 和 `thread::try_pop`（第二程第 8 项）。按「实现成本 × API 设计自由度」的判据，`optional` 的设计自由度几乎为零（`std::optional` 已经把形状定死），所以提前造没有设计风险——和 `string_view` 同一个理由。

## 2. 接口

```cpp
namespace wavvix {

struct nullopt_t { explicit constexpr nullopt_t(int) {} };
constexpr nullopt_t nullopt(0);

template <typename T>
class optional {
public:
    optional() noexcept;                       // 没有值
    optional(nullopt_t) noexcept;              // 同上，显式写出来
    template <typename U> optional(U&& value); // 能转换成 T 的输入，隐式

    bool has_value() const noexcept;

    T& value() &;                              // 没有值时诊断并终止
    const T& value() const&;
    T&& value() &&;

    template <typename U> T value_or(U&& fallback) const&;
    template <typename U> T value_or(U&& fallback) &&;

    void reset() noexcept;                     // 变回「没有」，幂等
};

bool operator==(const optional<T>&, nullopt_t) noexcept;   // != 以及两个方向都有
}
```

**不做**：`emplace`、`swap`、`optional<T>` 之间的比较、`optional<T&>`、`make_optional`、`std::hash` 特化、`and_then` / `transform` 这类单子操作。都没有消费者，而且 `std::optional` 的对应物随时可以照着补。

## 3. 关键决定

### 3.1 没有 operator bool / operator* / operator->

这是本模块唯一真正的设计冲突，两条自己的规则对撞：

| 规则 | 说什么 |
| --- | --- |
| **看名字就会用** | 学 `std::optional`：`if (opt)`、`*opt`。更短，而且所有 C++ 程序员都认 |
| **全库一致** | 学 `result`：`has_value()` / `value()`。`result` 是**故意**不提供这三个的（见 result 设计第 2 节「判断成败和取值各只保留一种写法」） |

**全库一致赢。** 决定性的理由是这两个类型会**并排出现在同一个函数里**——`examples/optional.cpp` 的 `load_port()` 就是：`os::getenv` 给 `optional`，`lexical_cast` 给 `result`。那时 `if (home)` 紧挨着 `if (parsed.is_err())` 就是「同一件事两种写法」，正是「每次都要先想一下」要挡的东西。

代价是从 `std::optional` 过来的人要多打几个字，写进了 [API 指南](../../api-guide.md)。

### 3.2 存储结构照抄 result，而且是故意不共用

`detail/optional_storage.h` 和 `detail/result_storage.h` 结构一样：按 `is_trivially_copyable` 分成两个特化，平凡那一侧完全不声明特殊成员（所以 `optional<int>` 是 8 字节、平凡可复制、走寄存器返回），非平凡那一侧只构造活着的成员、用不了的特殊成员靠函数体里的 `static_assert` 挡住。

**没有共用一份代码。** `result` 的存储是「T 和 E 二选一」，`optional` 是「T 有或没有」；把后者硬套成 `result_storage<T, 空标签>` 能少几十行，但那是「聪明而不清楚」的写法（同一类理由让 `result<void, E>` 没去私有继承 `result<unit, E>`，见 result 设计 11.5），而且会让 `optional` 的报错信息里出现 `error` 相关的字眼。

读过一个就懂另一个，这个收益比省下的几十行大。

### 3.3 union 里留一个 char empty_

```cpp
union {
    char empty_;
    T value_;
};
```

没有值时 `empty_` 是活成员，这样 union 总有一个活成员，默认构造可以写成 `empty_()` 而不需要 `aligned_storage` 加 `reinterpret_cast`。

### 3.4 移动之后源仍然 has_value()

和 `std::optional` 一致：移动只移走里面那个 `T`，`optional` 本身仍然是「有值」状态，里面是一个被移走的 `T`。

这**很容易被误读成「移动会清空它」**，所以头文件注释和示例都显式说了一遍，测试里也钉住了。要变回「没有」就显式 `reset()`。

### 3.5 和 nullopt 的比较不碰 T

`operator==(const optional<T>&, nullopt_t)` 只问 `has_value()`，所以 `T` 不需要可比较。测试里用一个没有 `operator==` 的结构体验证了这一点。

不提供 `optional<T>` 之间的比较：那需要 `T` 可比较，而现在没有消费者。

## 4. 实现验证

环境 Linux / GCC 14.2 与 clang 19，`-Wall -Wextra -Wpedantic -Werror`，全程零警告。

### 4.1 构建与测试

142 个测试通过（新增 8 个）：四种构造、`value_or` 的左值与右值版本（右值版本比对 `data()` 指针确认是移动）、和 `nullopt` 的四种比较、复制 / 移动 / 两种赋值、只能移动的 `T`（`std::unique_ptr`）、`reset` 的幂等、以及一个数析构次数的 `counted` 类型——它钉住了四件事：没有值时不构造也不析构、有值时离开作用域析构一次、`reset()` 立刻析构、**赋值不漏掉旧值的析构**。

两条 `static_assert` 把类型形状钉在测试里：

```cpp
static_assert(std::is_trivially_copyable<optional<int> >::value, "...");
static_assert(!std::is_trivially_copyable<optional<std::string> >::value, "...");
```

全部工具链：GCC debug/release、clang debug/release、MinGW-w64 + wine，各 142 通过。头文件单独 `-fsyntax-only` 通过，`examples/optional.cpp` 可运行。

### 4.2 开销

| | 大小 | 平凡可复制 |
| --- | --- | --- |
| `optional<int>` | 8 字节 | 是（走寄存器返回） |
| `optional<std::string>` | 40 字节 | 否（`std::string` 本身就不是） |

没有做对照测量，因为没有性能主张：形状和 `result<T, 空>` 等价，而 result 的开销已经量过（见那份文档 11.2）。按约定，没有对比就不写性能承诺。

### 4.3 误用

release（`-O2 -DNDEBUG`）下没有值时调 `value()`：

```
wavvix::optional::value() called when there is no value
```

退出码 134（SIGABRT）。

### 4.4 没覆盖

- **MSVC 和 macOS 要等 CI。** 本机覆盖 GCC / clang / MinGW。
- 复制赋值里「先复制再销毁旧值」那条路径只在 `counted` 的赋值测试里走过一次，没有专门测「复制构造抛异常时旧值完好」——那需要一个会抛的类型，而 result 的测试里有同样的场景（见 result 设计 11.3），这里没有重复一遍。
- `optional<T>` 之间的比较、`emplace`、`swap` 都没做，所以也没有测试。
