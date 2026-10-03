# variant：评估后不做

- 日期：2026-10-03
- 状态：已决定不实现。本文记录结论与重开条件，避免重复讨论。

## 结论

不实现 `variant`。它既不能简化库内已有的代码，也没有任何规划模块会用到它。

## 为什么没有内部使用者

把 README 模块规划里的 13 个模块过一遍，「同一块内存存多种类型之一」的需求只有两种形态，variant 对两种都不是最优解：

- **恰好两分支**：`optional`（有值 / 无值）、`lexical_cast` 的 `result<T, 枚举>`。现有的 `detail/result_storage.h`（两分支 union + 一个标志位，127 行）严格更优：实测把一个 `optional<T>` 建在 `detail::result_storage<T, nullopt_t>` 上只需 12 行，且 `optional<int>` 仍是 8 字节平凡可复制。换成泛型 `variant` 等于为用不到的 N 元能力付编译期和可读性的代价。详见 `2026-10-01-result-design.md` 第 11.5 节。
- **类型集合不封闭**：`flag` 的需求里明确「不常用的类型和使用者自己的类型也能用」（见 `2026-10-02-stopwatch-and-test-slimming-design.md` 6.1）。variant 要求候选类型在声明处就封闭列全，因此帮不上 flag——那里需要的是基类加虚函数或类型擦除。

其余模块不涉及：`table` 的单元格经 lexical_cast 转成字符串；`term`、`string`、`fs`、`time`、`thread`、`os`、`log`、`test` 都没有这类需求。

所以这不是「暂时还没用上」，而是这个库的需求形态里没有「封闭的 N≥3 类型集合」这一类。

## 为什么将来做 json 也用不上

反方意见先说完：「所有真实 JSON 库都手写 tagged union」这个论据对追求性能与尺寸的库成立（nlohmann/json、RapidJSON、Boost.JSON、jsoncpp 的 value 类型都是手写 union 加 `kind` 枚举，没有一个用 `std::variant`），但 nlohmann/json 是 2.5 万行。一个面向小团队、够用就好的 json，确实可以建在 variant 上并且会更简单。这点不否认。

但算术不成立：

- json value 的对外 API 必须是 `v.is_string()` / `v.as_string()` / `v["key"]`，不是 `visit`。所以无论底下是什么，都要套一层带具名访问器的 wrapper，variant 只是藏在里面的实现细节。
- 递归分支（array 装 value 自己）在 C++11 下必须走一层间接——标准容器要求元素类型完整，`vector` 支持不完整类型是 C++17 才允许的（GCC 恰好接受，但标准未定义）。这一点 variant 不解决，它自己也受同样约束。
- 净账：写 250 行以上的 variant，换 json 里省掉约 50 行 union 管理。
- 而 variant 最贵的两块能力——`visit` 的分派、跨 N 个类型做重载决议的转换构造函数——json 一块都用不上。

## 没有消费者时设计 API 会判断错

本次评估一度写出了完整设计稿，其中三处判断经实测是错的，一并记下作为教训：

- 认为「`std::variant<std::string, bool> v = "hello"` 会选中 `bool`」是 variant 的著名陷阱，可以作为自造规则更安全的论据。实测 GCC 14 / C++17 下选中的是 `std::string`（index 0）——P0608 已作为缺陷报告实现，这个陷阱今天不存在。
- 认为「索引用 `unsigned char` 而非 `std::size_t` 是相对 `std::variant` 的改进」。实测 libstdc++ 的 `sizeof(std::variant<char, bool>)` 就是 2 字节，它本来就按候选数量压缩索引宽度。
- 由上一条推出的构造规则「恰好一个候选类型能从输入构造时才接受」有设计错误：它会拒绝 `variant<int, double> v = 1;` 这种完全正常的写法（`int` 和 `double` 都能从 `1` 构造）。

另外原定的存储方案 `std::aligned_union` 在 C++23 下是 deprecated，会产生一条告警，与 README 的「零警告」约定冲突（使用方可以用比 C++11 更高的标准编译）。真要实现得改用递归 union 或手写对齐缓冲区。

## 重开条件

出现下面任一情况再回头看：

- 某个模块出现了**封闭的 N≥3 类型集合**，并且希望漏掉分支时编译报错（`visit` 的穷尽性检查是 variant 唯一无法被两分支存储或手写 union 替代的能力）。
- 使用方明确要求 culib 提供 C++11 的 `variant` 作为对外设施，并且愿意接受「没有库内消费者检验」的 API 风险。

在那之前，`optional` 落地时把 `detail/result_storage.h` 换一个与 result 无关的名字共用即可。
