# culib

基于 **C++11** 与 **xmake** 的跨平台（Windows / Linux / macOS）便携开发库，以静态库形式提供。基调是**简单好用、高性能低占用、尽可能零开销**。

culib 从个人实际需求出发，把各个项目里反复编写的基础代码沉淀下来、统一封装平台差异，让新程序可以直接在它之上快速搭建，而不必每次从零开始。

> **状态**：早期开发阶段。CLI 常用模块已可用；服务器基础设施仍在建设，API 尚未稳定。

## 设计目标

- **高性能、低占用、尽可能零开销**：硬约束，不是可以用别的优点换的加分项。遵循零开销原则：不用的功能不产生任何代价，用到的功能不比手写的更慢。具体约束见 [CLAUDE.md](CLAUDE.md) 的「性能与占用」。
- **简单好用、零理解负担**：接口直观、一致、难以误用，看名字就会用，不需要了解内部实现；实现本身也要简单，不为假想的通用场景留结构。好用与高效不是二选一：尽量让最直观的写法同时也是最高效的写法，真的必须二选一时（很少）以好用为先、代价写进设计文档。具体约束见 [CLAUDE.md](CLAUDE.md) 的「易用性」，判据见那里的「基调」。
- **C++11**：C++11 是目前最广泛可用的标准。为保持泛用性，库只使用 C++11 的语言特性与标准库；C++11 缺失但常用的设施按需由 culib 自行提供。
- **跨平台一致**：同一套 API 在 Windows / Linux / macOS 上行为一致，平台差异全部收敛在库内部。
- **静态库**：编译为单个静态库，程序链接后无需额外分发 culib 的动态库。
- **零第三方依赖**：只依赖 C++ 标准库和操作系统 API，拿到源码即可编译。
- **模块化**：模块之间低耦合，按需使用，未用到的代码不会进入最终程序。
- **接入简单**：xmake 工程中一行 `add_deps("culib")` 即可使用，头文件路径与系统库依赖自动传递。

## 平台与工具链

| 平台 | 编译器 | 架构 |
| --- | --- | --- |
| Windows 10 及以上 | MSVC（Visual Studio 2015+）、MinGW-w64 | x86、x64 |
| Linux | GCC 4.8.1+、Clang 3.3+ | x86_64、arm64 |
| macOS | Apple Clang（Xcode / Command Line Tools） | x86_64、arm64 |

- 编译器下限取各家首个完整支持 C++11 的版本（VS2015 为基本完整）。
- 构建工具：[xmake](https://xmake.io) 2.8.5 及以上（`xmake test` 自该版本起提供），推荐使用最新版。

## 模块规划

> 以下是候选模块，最终以实际需求为准，逐步补充和调整。「第一程 / 第二程」是实现顺序，见下方路线图链接。

| 模块 | 内容 | 状态 |
| --- | --- | --- |
| base | `defer`（作用域结束时执行，类似 Go）、版本信息、`result` 错误处理、平台 / 编译器检测宏；C++14/17 常用设施的 C++11 实现（`string_view`、`optional`、`make_unique` 等） | 进行中：`defer`、版本信息、`result`、`string_view`、`optional` 已完成；`make_unique` 按需再加 |
| test | 单元测试框架：`TEST` / `EXPECT`、按组运行、耗时、过滤和彩色报告 | **可用**；持续在三平台 CI 运行 |
| lexical_cast | 字符串 ⇄ 数值 / 布尔。`lexical_cast<T>` 严格解析（整个输入必须被消费）、`to_string` / `to_chars` 反向 | **已完成** |
| flag | 命令行参数：`flag::int64`、`flag::string` 等类型，必填项检查，自带 `--help` 和 `--version`；位置参数、GNU 风格重排、`set()` 和按名字问的 `was_given()` 给配置文件用 | **已完成**（`was_given(string_view)` 是 `config` 逼出来的） |
| strings | 操作字符串的自由函数：split / join / trim / 大小写 / replace、UTF-8 ⇄ UTF-16 转换。**不是字符串类**，全库一律用 `std::string`；大小写只做 ASCII，格式化另行缩范围 | **已完成**（East Asian Width 推后） |
| fs | 路径拆分与拼接、文件与目录操作、整文件读写、目录遍历（C++11 没有 `std::filesystem`）、**常开的追加写句柄** `output_file`（日志那种连续写同一个文件的场合，比每行开关一次快 7.7 倍） | **已完成**。`output_file` 和 Windows 共享模式那处修复是 `log` 那一期逼出来的 |
| term | 终端彩色输出：`fg::red("x")` 形状，样式是可组合的值类型（**漏掉括号就编译报错**，所以不会出现「设了颜色没恢复」）；自动判断是否该上色（终端 / `NO_COLOR` / `TERM`） | **已完成**，见 [设计](docs/superpowers/specs/2026-10-01-test-term-table-design.md) 第 5 节。测试框架的彩色报告就是它的第一个消费者。Windows 走 VT 模式，开不起来就降级成纯文字（不做旧版控制台 API，理由见[实现说明](docs/implementation-notes.md)） |
| os | 环境变量、程序自身路径、主机名、进程 ID、CPU 数、**信号与优雅退出**（只提供停止标志，不提供在信号里跑的回调）。动态库加载和崩溃调用栈都不做，理由见设计文档 | **已完成** |
| time | 时刻：`timestamp`（单位写在类型里）、ISO 8601 格式化与解析、UTC 与本地时区；时长与秒表在 `stopwatch.h` | **已完成**（无 strftime 格式串、不自带 tzdata，理由见设计文档） |
| 调度器与 I/O | vCPU 调度器、四个事件后端（`io_uring` / `epoll` / `kqueue` / `IOCP`）、超时与协程感知的同步原语 | **可用**：四个后端和 TCP/UDP 已通过三平台 CI；Linux `io_uring` 已在 WSL2 的 GCC / Clang 下验证，不可用时退回 epoll。显式多 vCPU 分发已通过[三平台 CI](https://github.com/NickJerome/culib/actions/runs/37874443895) 21 项，包括 GCC 4.8、MinGW i686 和无异常构建。见 [调度器用法](docs/scheduler.md)、[网络用法](docs/net.md)及 [io_uring 设计](docs/superpowers/specs/2026-10-08-io-uring-design.md) |
| blocking_pool | 让必须阻塞的操作在系统工作线程上执行，等待期间只挂起调用它的协程 | **初版已实现**；[三平台 CI](https://github.com/NickJerome/culib/actions/runs/37870009886) 21/21 通过，含 GCC 4.8、MinGW i686 与无异常构建。见 [用法](docs/blocking-pool.md) |
| log | 日志：`LOG_INFO << ...`，**级别不够时参数一行都不求值**（1.5 ns/行）；级别、多个输出目标、文件轮转；一行日志**不分配内存**也**没有用户态缓冲**（崩了不丢已写出的行） | **已完成**，见 [设计](docs/superpowers/specs/2026-10-05-log-design.md)。那边逐条定掉了预期接口留的四个待定项 |
| 配置文件 | ini 子集（`culib/config.h`）：注释、段（变成 `section.key` 扁平键）、键等于值；**「键不存在」用默认值而「值写错了」报错**；`apply_to_flags()` 让配置和命令行共用一套定义、命令行优先 | **已完成**，见 [设计](docs/superpowers/specs/2026-10-05-config-design.md)。那边定掉了预期接口留的三个待定项，并记了两处**示例先抓出来**的问题 |
| coroutine | 有栈协程，汇编换栈。**库的并发模型就是它**——协程取代线程，调度器见下一行 | 公开接口为构造即提交、`join()` / `detach()` 的线程式句柄；底层换栈原语已覆盖 x86_64、arm64 与 Windows x86。Windows x86 已在 MSVC 本机和 CI 运行验证 |
| bytes | 字节缓冲与字节序转换（`culib/bytes.h` 的 `buffer`）。读写游标 + **字节序写在函数名里**，没有「主机字节序」那种函数；`prepare` / `commit` / `consume` 给 I/O 省一次拷贝 | **已完成**，见 [设计](docs/superpowers/specs/2026-10-05-bytes-design.md)。整数读写内联，汇编和手写逐条相同 |
| net | socket、地址解析、多路复用；API 做成同步阻塞的样子，多路复用藏在协程调度器里 | 数字 IP 的 `endpoint`、显式同步 `resolve()`、TCP/UDP 收发、STUN Binding 地址发现、候选枚举和 UDP 双向探测已落地。应用层 `apps/mesh` 已实现加密信令、保活和失联重连，并通过可控 NAT 测试；真实公网 NAT 仍待现场验收。见[网络用法](docs/net.md)、[mesh 应用](apps/mesh/README.md)和[设计](docs/superpowers/specs/2026-10-08-net-design.md) |
| net::virtual_interface | 临时三层虚拟网卡，协程中按整包读写 IPv4/IPv6；不替应用配置地址和路由 | **基础接口可用**。Linux TUN 已做真实收发；[macOS arm64/x86_64 debug/release CI](https://github.com/NickJerome/culib/actions/runs/37841586928) 均完成同机双 utun 双向 ICMP 包转发，但没有收到 Echo Reply；Windows 真实 Wintun 已通过本机 ICMP IP 往返测试。独立 macOS 主机间的往返仍待验证。见[用法](docs/net.md)和[第四程设计](docs/superpowers/specs/2026-10-08-mesh-core-design.md) |
| fs::watcher / fs::poller | 文件变化监听（`culib/fs_watcher.h`）。`watcher` 用内核通知监听本机目录；`poller` 用目录快照轮询，也适用于网络挂载 | **可用**；见[用法](docs/fs-watcher.md)和[设计](docs/superpowers/specs/2026-10-08-fs-watcher-design.md) |

顺序与理由见 [路线图](docs/superpowers/specs/2026-10-03-roadmap.md)，它按三个目标使用场景（小工具 CLI、多平台服务器、easytier 式精简核心）推导而来。`table` 已从计划中移除，`variant` 评估后不做。

## 快速开始

### 1. 准备环境

安装 xmake（更多方式见 [xmake 官网](https://xmake.io)）：

```bash
brew install xmake                               # macOS
curl -fsSL https://xmake.io/shget.text | bash    # Linux
winget install xmake                             # Windows
```

安装编译器：

- **Windows**：Visual Studio（勾选「使用 C++ 的桌面开发」）或 MinGW-w64
- **Linux**：`sudo apt install build-essential`，或安装 clang
- **macOS**：`xcode-select --install`

### 2. 构建

在 culib 根目录下执行：

```bash
xmake                          # 按默认配置构建（当前平台、release 模式）
xmake f -m debug               # 切换到 debug 模式
xmake                          # 重新构建
```

常用配置：

```bash
xmake f -p windows -a x64      # Windows 64 位（MSVC）
xmake f -p windows -a x86      # Windows 32 位（MSVC）
xmake f -p mingw               # MinGW
xmake f -p macosx -a arm64     # macOS Apple Silicon
xmake f -p macosx -a x86_64    # macOS Intel
xmake f -c                     # 清除配置缓存，重新检测环境
```

构建产物位于 `build/<平台>/<架构>/<模式>/`，例如 `build/linux/x86_64/release/libculib.a`、`build/windows/x64/release/culib.lib`。

### 3. 运行测试

```bash
xmake test                                # 编译并运行全部测试，只显示是否通过（适合 CI）
xmake test -v                             # 有失败时显示完整报告
xmake run culib_test                      # 在终端里查看完整报告：分组、每个测试和每组的耗时
xmake run culib_test --filter="defer.*"   # 只运行 defer 组（zsh 下模式要加引号）
xmake run culib_test --list               # 列出所有测试
```

### 4. 安装

```bash
xmake install -o <安装目录>
```

安装后的目录结构：

```
<安装目录>/
├── include/
│   └── culib/                 # 公共头文件
└── lib/
    └── libculib.a             # MSVC 下为 culib.lib
```

### 5. 生成 IDE 工程（可选）

```bash
xmake project -k vsxmake            # Visual Studio 解决方案
xmake project -k compile_commands   # compile_commands.json，供 clangd / VS Code / CLion 使用
```

## 在项目中使用

### 方式一：源码引入（推荐）

把 culib 放进项目（git submodule 或直接拷贝均可）：

```
myapp/
├── xmake.lua
├── src/
│   └── main.cpp
└── third_party/
    └── culib/
```

```lua
-- myapp/xmake.lua
add_rules("mode.debug", "mode.release")
set_languages("c++11")              -- 使用方也可以用更高的标准

includes("third_party/culib")

target("myapp")
    set_kind("binary")
    add_files("src/*.cpp")
    add_deps("culib")               -- 自动继承 culib 的头文件路径与系统库依赖
```

```cpp
// myapp/src/main.cpp
#include <culib/culib.h>   // 汇总头文件；也可以只包含用到的功能头文件，如 <culib/defer.h>
#include <cstdio>

int main() {
    std::FILE* file = std::fopen("data.txt", "rb");
    if (!file) {
        return 1;
    }
    defer { std::fclose(file); };   // 离开作用域时自动关闭，不管从哪条路径返回

    std::printf("culib %s\n", wavvix::version());
    return 0;
}
```

源码引入时 culib 跟随主工程的全局配置（平台、架构、模式等）一起编译，省去对齐版本和编译配置的麻烦。

### 方式二：链接预编译库

先 `xmake install -o <安装目录>`，再在使用方工程中配置：

```lua
target("myapp")
    set_kind("binary")
    add_files("src/*.cpp")
    add_includedirs("<安装目录>/include")
    add_linkdirs("<安装目录>/lib")
    add_links("culib")
    -- 还需链接 culib 用到的系统库，以 culib 的 xmake.lua 中的 add_syslinks 为准
```

非 xmake 工程（CMake、Visual Studio 等）同理：添加头文件目录，链接 `culib` 及其依赖的系统库即可。

### Windows 运行时（MSVC）

MSVC 下，culib 与使用方必须使用完全相同的 C 运行时（`/MT`、`/MTd`、`/MD`、`/MDd` 四者之一），否则链接时会报 `LNK2038`。建议通过全局配置统一指定，而不是只在某个 target 里 `set_runtimes`（那样 culib 不会跟随）：

```bash
xmake f -p windows --runtimes=MT    # 静态 CRT，程序无需附带 VC++ 运行库
xmake f -p windows --runtimes=MD    # 动态 CRT
```

使用预编译库时，构建 culib 所用的运行时也必须和使用方一致。

## 编写测试

culib 自带一个用法接近 Google Test 的单元测试框架，只有 `TEST` 和 `EXPECT` 两个宏：

```cpp
#include <culib/test.h>

TEST(math, adds_numbers) {                     // 组名, 测试名
    EXPECT(1 + 1, ==, 2);                      // EXPECT(a, op, b)
    EXPECT(std::string("abc"), <, "abd");
}

int main(int argc, char* argv[]) {             // 整个测试程序只需要一个 main
    return wavvix::test::run_all(argc, argv);
}
```

- `op` 只能是 `==`、`!=`、`<`、`<=`、`>`、`>=`；`a` 和 `b` 各只求值一次。
- 有符号与无符号整数按数学值比较，`EXPECT(v.size(), ==, 3)` 不会产生警告；两边都是 C 字符串时比较内容。
- 失败时打印位置、表达式和两边的值，测试继续执行。`EXPECT` 返回 `bool`，需要失败就停时写 `if (!EXPECT(p, !=, nullptr)) { return; }`。
- 运行参数：`--filter=模式`（如 `defer.*`，`-` 之后的模式表示排除）、`--list`、`--help`。退出码：全部通过为 0，有失败为 1，参数错误或测试重名为 2。
- 启用异常时，测试函数抛出未捕获的异常会记为失败，并继续运行后面的测试。
- 汇总头文件 `culib.h` 不包含测试框架，需要单独包含 `<culib/test.h>`。

在自己的 xmake 工程里加一个测试程序：

```lua
target("myapp_test")
    set_kind("binary")
    set_default(false)
    add_files("tests/*.cpp")
    add_deps("culib")
    add_tests("default")
```

## 目录结构

```
culib/
├── xmake.lua                   # 构建脚本
├── README.md
├── include/
│   └── culib/                  # 公共 API；detail/ 为内部实现
├── src/                        # 静态库实现、平台后端及 asm/ 换栈代码
├── tests/                      # culib 单元与平台集成测试
├── examples/                   # 每个 .cpp 对应一个可运行示例
├── apps/mesh/                  # 独立的加密 mesh 应用与测试
├── docs/                       # 使用文档、设计记录和实现计划
├── CLAUDE.md                   # 开发约定
└── .github/workflows/          # Linux、macOS、Windows 的构建与测试 CI
```

## 错误处理

- 可能失败的操作通过 `result` 返回结果或错误，由调用方显式检查；库不主动抛出异常。
- 库不主动抛出异常，也不依赖 RTTI；详见下面的「对使用方的保证」。

可能失败的函数返回 `wavvix::result<T>`：成功直接返回值，失败返回 `make_error(...)`。

```cpp
#include <culib/result.h>

wavvix::result<int> checked_port(int port) {
    if (port < 1 || port > 65535) {
        return wavvix::make_error(wavvix::error_kind::out_of_range, "port must be between 1 and 65535");
    }
    return port;
}

auto port = checked_port(8080);
if (port.is_err()) {
    report(port.error().to_string());
    return;
}
use(port.value());
// 或在失败时使用默认值：checked_port(input).value_or(8080)
```

- 只关心成败时用 `result<void>`，`return {};` 表示成功。
- **错误类型只有一种，就是默认的 `error`。** culib 不为「高频转换」之类的场景另开枚举错误类型——量过了，成功路径两边都是零分配、相差 ~3 ns，而两种错误类型会让每个层边界上多一次显式转换。真遇到热失败路径时，`make_error(kind)` 不带说明字符串就是零分配。
- 跨层把错误传上去时，在中间那一层把上下文拼进消息：底层只知道「不是合法整数」，只有你这一层同时知道「这是 `/etc/app.conf` 的 `port` 字段」。
- `E` 仍是模板参数，自己的代码需要时可以传任意小而平凡可复制的类型，但要自己负责层边界上的转换。
- 支持复制、移动及只能移动的值或错误；赋值要求两种类型都能无异常移动构造。用不了的组合（如复制只能移动的 `result`）在用到的那一行编译报错并说明原因，但 `std::is_copy_constructible` 这类类型特征只看声明，对 `result` 会乐观地回答「可以」。
- `value()` / `error()` 访问错误分支会打印诊断并终止程序，release 同样检查。
- 结果容器自身不分配内存；值或错误自身仍可能分配内存。
- 值和错误必须是非数组、非 `const` / `volatile` 的对象类型；需要引用语义时用指针。

自己不处理错误、原样交给调用方时，手写 `if` 把错误移出去：

```cpp
wavvix::result<config> load(const std::string& path) {
    wavvix::result<std::string> text = read_file(path);
    if (text.is_err()) return std::move(text).error();

    wavvix::result<int> port = parse_port(text.value());
    if (port.is_err()) return std::move(port).error();

    return config{text.value(), port.value()};
}
```

- `std::move(x).error()` 用的是右值重载，错误被移出来，不会多复制一次说明字符串。
- **culib 不为这件事提供宏。** 一个 `TRY(decl, expr)` 能把每处压成一行，但它展开后藏着一个 `return`，看起来却像变量声明——函数里哪几行会提前返回，读代码的人看不出来。多写的那一行换到的是「每一处 `return` 都看得见」。这个宏曾经存在过，已删除，理由见 [result 设计](docs/superpowers/specs/2026-10-01-result-design.md) 11.10。

完整示例见 [examples/result.cpp](examples/result.cpp)，接口规则见 [result 设计](docs/superpowers/specs/2026-10-01-result-design.md)。

## 对使用方的保证

- **不抛异常、不依赖 RTTI**：公共头文件里没有 `throw`、`try`、`dynamic_cast`、`typeid`，可以用在 `-fno-exceptions` / `-fno-rtti` 的工程里。[Linux GCC/Clang CI](https://github.com/NickJerome/culib/actions/runs/37845192319) 会在这两个选项下构建库和示例，并运行测试。
- **无启动开销**：`main` 之前不执行任何代码，没有需要动态初始化的全局对象。唯一的例外是测试程序里 `TEST` 的自动注册，正式程序不受影响。
- **可裁剪**：库以 `-ffunction-sections -fdata-sections`（MSVC 为 `/Gy`）编译，链接时开启 `--gc-sections` / `-dead_strip` / `/OPT:REF` 即可剔除未用到的代码。
- **字符串一律 UTF-8**：包括文件路径；Windows 下由库内部转成 UTF-16 调用 `W` 系列 API。
- **不污染你的构建**：公共头文件不包含 `<windows.h>`、`<unistd.h>`、`<iostream>`；被 `includes` 引入时不会把 `-Werror` 强加给你。
- **头文件可独立包含**：每个公共头文件都能单独 `#include`，不依赖包含顺序。

## 文档

- [公共 API 用法与设计说明](docs/api-guide.md)：各模块的示例、行为边界和接口取舍。
- [实现说明](docs/implementation-notes.md)：调度器、事件引擎等实现文件的结构与平台决策。
- [路线图](docs/superpowers/specs/2026-10-03-roadmap.md)：模块顺序、已完成范围与后续工作。

## 参与开发

命名、实现约束、新增模块的步骤见 [CLAUDE.md](CLAUDE.md)；各模块的设计文档在 `docs/superpowers/specs/`。

本地常用命令：

```bash
xmake                       # 构建库
xmake test                  # 跑测试
xmake build -g examples     # 编译全部示例
xmake run example_result    # 运行一个示例
```

三个平台都是警告即错误（MSVC 上是 `/W4` 加 `/WX`）；[CI](https://github.com/NickJerome/culib/actions/runs/37877826017) 覆盖 Linux GCC、Linux Clang、macOS Clang、Windows MSVC 各 debug / release 两种模式，以及 MinGW-w64 x86_64/i686、GCC 4.8、公共头文件和源码依赖引入场景。MinGW 两种架构均运行测试并编译全部示例。

## 待定事项

- [x] 最低系统版本：**Windows 10**（2026-10-07 定）。构建时给 culib 定义 `_WIN32_WINNT=0x0A00`，见[路线图](docs/superpowers/specs/2026-10-03-roadmap.md)第 6 节
- [x] 头文件注释的语言与编码：**已定**，中文注释保留，公共头文件存成带 BOM 的 UTF-8，使用方不加 `/utf-8` 也不会报 C4819。CI 的 `headers` job 检查 BOM 在不在

## 版本

遵循 [语义化版本](https://semver.org/lang/zh-CN/)。1.0 之前 API 可能发生不兼容变更。

## 许可证

待定。
