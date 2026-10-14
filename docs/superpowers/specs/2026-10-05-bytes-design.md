# bytes：一段字节、两个游标、写明字节序的整数读写

- 日期：2026-10-05
- 状态：**已实现**。路线图第三程第 12 项，`net` 的前置。
- 前置：[预期接口](2026-10-03-module-interfaces.md) 末节已经定了形状，本文只是把它落成签名。

## 1. 形状是已有设计定的

预期接口那份文档在「第三程：只给形状，不给签名」里写着：

> `bytes` 的形状：一个可增长的字节缓冲 + 读写游标 + **显式字节序的读写函数**（`read_u32_be` 这类），不做隐式的主机字节序转换。

本文不改这个形状，只补两件它没说的：**读不够时返回什么**，以及**怎么和 I/O 对接**。

## 2. 为什么没有「主机字节序」那种函数

每个整数读写的名字里都带 `_be` 或 `_le`，**没有不带后缀的版本**。

`htonl` 那一套把「转换」和「目标字节序」混成一件事：读 `htonl(x)` 的人要先知道主机是什么字节序，才能回答「线上到底是什么」。而协议里那个字节序是固定的，跟你在哪台机器上跑没关系。名字里写死，读代码的人就不用想。

实现是**按位移写的，不是 `htonl`**：

```cpp
at[0] = static_cast<unsigned char>(value >> 24);
at[1] = static_cast<unsigned char>(value >> 16);
at[2] = static_cast<unsigned char>(value >> 8);
at[3] = static_cast<unsigned char>(value);
```

这样和主机字节序无关——同一份代码在大端机上也对，不需要 `#if`。

### 2.1 代价：零（汇编对比）

GCC 14.2 `-O2`，x86_64。按约定「性能主张必须有对比」，和手写的等价代码比：

| | 手写 | 通过 `buffer` |
| --- | --- | --- |
| 写 u32 大端 | `bswap esi` / `mov DWORD PTR [rdi], esi` | `bswap ebp` / `mov DWORD PTR [rax], ebp` |
| 读 u32 大端 | `mov eax, DWORD PTR [rdi]` / `bswap eax` | `mov eax, DWORD PTR [rax+rdx]` / `bswap eax` |

**逐条相同。** 编译器认得出这个位移式样，折成一条 `bswap` 加一次访存，所以「可移植」在这里不要钱。

整数读写因此**全部放在头文件里内联**——它们在任何协议解析器的热路径上，而静态库里的普通函数在不开 LTO 时跨库内联不了（开发约定「热路径可内联」）。会分配、会搬家的那部分（`prepare` / `reserve` / 压缩）在 `.cpp` 里。

## 3. 读不够时返回 `optional`，而不是「bool 加出参」

这是本文唯一一处和库里别处不一样的地方，所以理由要写清楚。

**不用 `result`**：「数据还没到齐」不是错误，是流式解析的常态。为它付一次错误消息的分配（`error` 带 `std::string`）是纯浪费，而且调用点会被迫处理一个根本不存在的「失败」。

**不用「返回 bool + 出参」**，虽然当时库里别处采用过这种形状（旧的 `fs::read_file(path, buffer)` 已在 2026-10-14 fs 改版删除；开发约定「参数一律输入在前、输出在后」）。那种写法在这里有个真陷阱：

```cpp
std::printf("%s 值是 %u", in.read_u16_be(v) ? "成了" : "还不够", v);   // v 可能先被读
```

函数实参的求值顺序是未定义的，所以 `v` 可能在 `read_u16_be` 跑之前就被取走。**这不是假想的** ——`examples/bytes.cpp` 第一版就是这么写的，跑出来「值是 0」，而正确答案是 5。返回值形式写不出这个 bug。

这也是开发约定里那条的又一次兑现：**示例没法作弊，它必须按用法写**，所以示例比单元测试更容易发现接口问题——那条测试当时是分行写的，所以它绿着。

**选 `optional` 的第三个理由**：推荐用法下调用点反而更短，还能直接初始化 `const` 局部变量。

```cpp
if (in.size() < 6) return need_more;                      // 先确认整帧到齐
const std::uint16_t kind   = in.read_u16_be().value();    // 再一口气解析
const std::uint32_t length = in.read_u32_be().value();
```

本库的 `optional` 没有 `operator bool` 也没有 `operator*`（判断有无和取值分开，不能漏），所以 `.value()` 在空的时候会打诊断后 abort——**在这个用法里那正是想要的**：`size()` 检查错了是调用方的 bug，当场炸比静默读出垃圾好。

逐字段判断也安全，因为**不够时游标不动**：

```cpp
const wavvix::optional<std::uint32_t> length = in.read_u32_be();
if (!length.has_value()) return need_more;
```

`read(void*, n)` 仍然返回 `bool`——它没有值要返回，`optional<void>` 是个笑话。

## 4. 和 I/O 对接：prepare / commit / consume

```cpp
unsigned char* room = in.prepare(4096);
const ssize_t got = read(fd, room, 4096);
if (got > 0) in.commit(got);              // 只认账真读到的那些

const ssize_t sent = write(fd, out.data(), out.size());
if (sent > 0) out.consume(sent);          // 发出去多少就丢掉多少
```

这三个是为了**少一次拷贝**：直接读进缓冲的内存，而不是读进临时数组再 `write()` 进来。`net` 会这么用。

`commit` 认账超过最近一次 `prepare(n)` 的 `n` 会把未写入的字节当作有效数据；即使底层缓冲还有容量也必须拒绝。`buffer` 用一个 `size_t` 记录尚可提交的字节数，超额时打印诊断并 `abort`，release 下同样检查。64 位平台上的对象因此多占 8 字节，`prepare` / `commit` 各更新一次计数。`prepare(0)` 不分配内存，空缓冲区返回空指针。

### 4.1 稳态下不长

内部是一块 `vector<unsigned char>` 加读写两个游标。`prepare` 发现空间不够时**先试着把已经读掉的那段腾出来**（`memmove` 剩下的到开头），真不够才翻倍扩容。

所以「读一点写一点」的连接上容量不会一路往上爬。有一条测试钉着：一万轮「写 32 读 32」之后容量和初始的 256 一模一样，**一次都没再长**。

## 5. 没做什么

- **有符号整数**：调用方自己 `static_cast`。协议里的字段几乎都是无符号的，而「先有消费者」。
- **浮点**：同上，而且 IEEE-754 的线上表示还要定一个说法，没有消费者就不定。
- **裸指针版的 `get_u32_be(const void*)` / `put_u32_be(void*, v)`**：解析定长头时会想要，但缓冲这边的游标版已经够 `net` 用。**什么条件下加**：出现「要在一块不属于缓冲的内存上读写整数」的调用方。
- **变长整数（varint）**：protobuf 那类才需要，而配置文件选的是 ini / toml 子集（路线图），没有消费者。
- **跨协程安全**：`buffer` 就是个普通对象，没有任何同步。多根协程共用一个要自己加 `wavvix::mutex`。

## 6. 实现验证

| 平台 / 工具链 | 结果 |
| --- | --- |
| Linux x86_64，GCC 14.2 / clang 19，debug / release | **225 个测试通过**，零警告 |
| MinGW-w64 x86_64 + wine | 223 个通过，零警告 |
| MinGW-w64 i686 | 编译链接通过 |
| ASan + UBSan，`detect_leaks=1` | 新增 11 条干净 |

新增 11 条，其中三条值得点名：

- `puts_integers_on_the_wire_in_the_order_the_name_says`：**对着写死的字节序列核**，不是「写进去再读出来相等」。后者把字节序写反这种错漏得一干二净。
- `leaves_the_cursor_alone_when_there_is_not_enough`：不够时什么都不动，所以逐字段判断不是陷阱。
- `reuses_its_memory_instead_of_growing_without_end`：一万轮读写后容量不变。

`examples/bytes.cpp` 六段，覆盖字节序、组帧、拆帧、流式喂入、`prepare` / `commit`、稳态不长。
