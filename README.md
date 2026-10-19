# culib

基于 **C++11** 与 **xmake** 的跨平台（Windows / Linux / macOS）便携开发库，以静态库形式提供。优先保持接口和实现简单，兼顾常见需求下的性能与低占用。

culib 从个人实际需求出发，提供像线程一样使用的 C++11 协程，以及命令行、文件、日志和网络等常用开发工具，统一封装平台差异。

> **状态**：早期开发阶段，目前没有下游消费者。CLI 常用模块已可用；服务器基础设施仍在建设，API 会直接调整。

## 设计目标

- **简单好用、实现易懂**：接口直观、一致，基本操作独立，上层直接组合；默认提前返回、用 RAII 管理资源，不为假想场景保留结构。
- **合理性能、低占用**：少做无用工作，避免不必要的分配和拷贝；性能满足常见需求后，优先降低实现复杂度。不用大量代码换取微小收益，优化须有实际负载的证据。具体取舍见 [CLAUDE.md](CLAUDE.md)。
- **C++11**：C++11 是目前最广泛可用的标准。为保持泛用性，库只使用 C++11 的语言特性与标准库；C++11 缺失但常用的设施按需由 culib 自行提供。
- **跨平台一致**：同一套 API 在 Windows / Linux / macOS 上行为一致，平台差异全部收敛在库内部。
- **静态库**：编译为单个静态库，程序链接后无需额外分发 culib 的动态库。
- **零第三方依赖**：只依赖 C++ 标准库和操作系统 API，拿到源码即可编译。
- **模块化**：模块之间低耦合，按需使用，未用到的代码不会进入最终程序。
- **接入简单**：xmake 工程中一行 `add_deps("culib")` 即可使用，头文件路径与系统库依赖自动传递。

## 平台与工具链

| 平台 | 编译器 | 架构 |
| --- | --- | --- |
| Windows 10 及以上 | MSVC、MinGW-w64 | x64 |
| Linux | GCC、Clang | x86_64、arm64 |
| macOS | Apple Clang（Xcode / Command Line Tools） | x86_64、arm64 |

- 只支持 64 位：协程的换栈汇编只有 x86_64 和 arm64 两种，栈来自只支持 64 位的分配器，32 位目标在编译期报错。
- Linux 需要 glibc 2.10、内核 2.6.28 以上（建套接字用了 `accept4` 和 `SOCK_CLOEXEC`）。CI 里最老的是 GCC 4.8 那个任务：Debian 7，glibc 2.13。
- 常规 CI 使用托管 runner 的工具链，另有 GCC 4.8 专项构建与测试。VS2015、Clang 3.3 等旧版本尚未验证；C++11 语言要求不等于对所有 C++11 编译器版本的兼容承诺。
- 构建工具：[xmake](https://xmake.io) 2.8.5 及以上（`xmake test` 自该版本起提供），推荐使用最新版。

## 模块规划

> 以下是候选模块，最终以实际需求为准，逐步补充和调整。「第一程 / 第二程」是实现顺序，见下方路线图链接。

| 模块 | 内容 | 状态 |
| --- | --- | --- |
| base | `defer`、版本信息、`result` 错误处理、`string_view` 和 `optional` | **已完成** |
| test | 单元测试框架：`TEST` / `EXPECT`、按组运行、耗时、过滤和彩色报告 | **可用**；持续在三平台 CI 运行 |
| lexical_cast | 字符串 ⇄ 数值 / 布尔。`lexical_cast<T>` 严格解析（整个输入必须被消费）、`to_string` / `to_chars` 反向 | **已完成** |
| flag | 命令行参数：`flag::int64`、`flag::string` 等类型，必填项检查，自带 `--help` 和 `--version`；位置参数、GNU 风格重排、`set()` 和按名字问的 `was_given()` 给配置文件用 | **已完成**（`was_given(string_view)` 是 `config` 逼出来的） |
| strings | 操作字符串的自由函数：split / join / trim / 大小写 / replace、UTF-8 ⇄ UTF-16 转换。**不是字符串类**，全库一律用 `std::string`；大小写只做 ASCII，格式化另行缩范围 | **已完成**（East Asian Width 推后） |
| fs | 路径值类型 `fs::path`（UTF-8，名字对齐 `std::filesystem`）、`result` 三态查询（「不存在」是 `false`，「问不成」才是错误）、目录与文件操作、`directory_iterator` 遍历、`fs::file` 句柄与 `fs::ofstream` 流（C++11 没有 `std::filesystem`） | 接口 2026-10-14 定稿为[标准接口改版](docs/superpowers/specs/2026-10-14-fs-standard-interface-design.md)：`path` 值类型取代字符串自由函数，整文件读写 / `output_file` / 回调式遍历被句柄、流和迭代器取代，不留兼容；接口已实现，最终 POSIX FIFO 复制修复后在 Linux x86_64 GCC debug、Clang release 和 GCC 无异常 / 无 RTTI release 跑过完整本地构建、测试与示例；Windows MSVC debug 与 release 在这次仅涉及 POSIX 的修复之前本地跑过，修复之后未在本地重跑，但由[提交 `e7b6b8e` 的三平台 CI](https://github.com/NickJerome/culib/actions/runs/38567733275) 24/24 通过，含 Windows MSVC、macOS、MinGW、GCC 4.8；完整日志核对与边界见[设计文档](docs/superpowers/specs/2026-10-14-fs-standard-interface-design.md)第 5 节。Windows 共享模式那处修复随新句柄保留 |
| term | 终端彩色输出：`fg::red("x")` 形状，样式是可组合的值类型（**漏掉括号就编译报错**，所以不会出现「设了颜色没恢复」）；自动判断是否该上色（终端 / `NO_COLOR` / `TERM`） | **已完成**，见 [设计](docs/superpowers/specs/2026-10-01-test-term-table-design.md) 第 5 节。测试框架的彩色报告就是它的第一个消费者。Windows 走 VT 模式，开不起来就降级成纯文字（不做旧版控制台 API，理由见[实现说明](docs/implementation-notes.md)） |
| os | 环境变量、程序自身路径、主机名、进程 ID、CPU 数、**信号与优雅退出**（只提供停止标志，不提供在信号里跑的回调）。动态库加载和崩溃调用栈都不做，理由见设计文档 | **已完成** |
| time | 时刻：`timestamp`（单位写在类型里）、ISO 8601 格式化与解析、UTC 与本地时区；时长与秒表在 `stopwatch.h` | **已完成**（无 strftime 格式串、不自带 tzdata，理由见设计文档） |
| 运行时与 I/O | 第一次 `go()` 时起运行时，同时跑协程的线程数默认取 CPU 数（`WAVVIX_MAXPROCS` 可覆盖），卡住的工作线程由监控线程把名额交给另一根；跨线程可用、在普通线程上也能等的 mutex / condition_variable / channel / wait_group；`call_blocking` 共用的后台线程；三个事件后端（`epoll` / `kqueue` / `IOCP`）与超时 | **可用**。2026-10-18 改成 Go 风格的接口：没有 runtime 对象和协程句柄，`main` 返回时进程直接退出。见[协程运行时](docs/runtime.md)和[网络用法](docs/net.md) |
| log | 日志：`LOG_INFO << ...`，级别不足时不求值参数；多个输出目标与文件轮转。消息使用固定栈缓冲，文件整行拼接可能分配；文件写入不使用 stdio 缓冲 | **已完成**，见 [设计](docs/superpowers/specs/2026-10-05-log-design.md) |
| 配置文件 | ini 子集（`culib/config.h`）：注释、段（变成 `section.key` 扁平键）、键等于值；**「键不存在」用默认值而「值写错了」报错**；`apply_to_flags()` 让配置和命令行共用一套定义、命令行优先 | **已完成**，见 [设计](docs/superpowers/specs/2026-10-05-config-design.md)。那边定掉了预期接口留的三个待定项，并记了两处**示例先抓出来**的问题 |
| coroutine | 有栈协程，汇编换栈。**库的并发模型就是它**——协程取代线程，跑在运行时的工作线程上 | `go()` 起协程、`wait_group` 等协程；协程在挂起点可能换到别的工作线程上继续（和 Go 一样），栈在第一次运行时才向 `mem` 要。换栈汇编覆盖 x86_64 和 arm64 |
| mem | 自带通用分配器 `wavvix::mem`：依赖 `culib_new_override` 就让全局 `new/delete` 和标准容器改用它，代码不用改；Linux 还可以覆盖 `malloc/free`；另有 C 语义的显式接口。协程栈也向它要 | 64 位 Linux、macOS、Windows。本机验证过 Linux（GCC、Clang、ASan + UBSan）和 Windows（MSVC）；[`09f952d` 的三平台 CI](https://github.com/NickJerome/culib/actions/runs/38219833032) 27/27 通过，含 macOS arm64 / x86_64 和 GCC 4.8。平台边界与覆盖方式见[分配器用法](docs/mem.md) |
| bytes | 字节缓冲与字节序转换（`culib/bytes.h` 的 `buffer`）。读写游标 + **字节序写在函数名里**，没有「主机字节序」那种函数；`prepare` / `commit` / `consume` 给 I/O 省一次拷贝 | **已完成**，见 [设计](docs/superpowers/specs/2026-10-05-bytes-design.md)。整数读写内联，汇编和手写逐条相同 |
| net | socket、地址解析、多路复用；API 做成同步阻塞的样子，多路复用藏在运行时的事件引擎里 | 数字 IP 的 `endpoint`、显式 `resolve()`（协程内自动经 `call_blocking`，不卡工作线程）、TCP/UDP 收发已落地。2026-10-13 把 `tcp_listener` / `tcp_stream` / `udp_socket` 合并为统一的 `net::socket`（协议构造时指定，惰性建套接字），见[网络用法](docs/net.md)和[统一 socket 设计](docs/superpowers/specs/2026-10-13-unified-socket-design.md)；[2026-10-08 的网络](docs/superpowers/specs/2026-10-08-net-design.md)与 [UDP 设计](docs/superpowers/specs/2026-10-08-udp-design.md)描述被取代的旧接口，保留作历史 |

顺序与理由见 [路线图](docs/superpowers/specs/2026-10-03-roadmap.md)。`table` 已从计划中移除，`variant` 评估后不做。

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
-- MSVC 不提供 C++11 模式，默认语言模式已包含 C++11；其他编译器显式限定。
if not is_plat("windows") then set_languages("c++11") end

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
├── src/                        # 按职责划分的实现
│   ├── base/                   # 基础类型、字符串与转换
│   ├── cli/                    # 参数、配置、日志与终端
│   ├── fs/                     # 文件与目录操作
│   ├── mem/                    # 协程栈池、通用分配器及覆盖目标
│   ├── net/                    # 地址、TCP 与 UDP
│   ├── platform/               # 系统与时间的平台实现
│   ├── runtime/                # 协程、调度器、I/O 引擎及换栈汇编
│   └── testing/                # 测试框架
├── tests/                      # culib 单元与平台集成测试
├── benchmarks/                 # 分配器等性能测量程序
├── examples/                   # 每个 .cpp 对应一个可运行示例
├── docs/                       # 使用文档、设计记录和实现计划
├── CLAUDE.md                   # 开发约定
└── .github/workflows/          # Linux、macOS、Windows 的构建与测试 CI
```

## 错误处理

可能失败的操作通常通过 `result` 返回错误，由调用方显式检查。`go()` 是例外：它没法返回错误，内存不够时打印诊断后终止进程。库不依赖 RTTI。

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
- **错误类型固定为 `error`。** culib 不为「高频转换」之类的场景另开枚举错误类型——量过了，成功路径两边都是零分配、相差 ~3 ns，而两种错误类型会让每个层边界上多一次显式转换。真遇到热失败路径时，`make_error(kind)` 不带说明字符串就是零分配。
- 跨层把错误传上去时，在中间那一层把上下文拼进消息：底层只知道「不是合法整数」，只有你这一层同时知道「这是 `/etc/app.conf` 的 `port` 字段」。
- 支持复制、移动及只能移动的值；赋值要求值能无异常移动构造，错误类型固定为 `error`。用不了的组合（如复制只能移动的 `result`）在用到的那一行编译报错并说明原因，但 `std::is_copy_constructible` 这类类型特征只看声明，对 `result` 会乐观地回答「可以」。
- `value()` / `error()` 访问错误分支会打印诊断并终止程序，release 同样检查。
- 结果容器自身不分配内存；值或错误自身仍可能分配内存。
- 值和错误必须是非数组、非 `const` / `volatile` 的对象类型；需要引用语义时用指针。

自己不处理错误、原样交给调用方时，手写 `if` 把错误移出去：

```cpp
wavvix::result<config> load(const std::string& path) {
    wavvix::result<std::string> text = read_text(path);
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

- **可关闭异常和 RTTI**：除 `coroutine.h` 里 `call_blocking` 按异常开关转发异常的分支外，公共头文件不使用 `throw`、`try`、`dynamic_cast`、`typeid`；`-fno-exceptions` / `-fno-rtti` 下仍可构建和使用全库。[Linux GCC/Clang CI](https://github.com/NickJerome/culib/actions/runs/37845192319) 在这两个选项下构建库和示例，并运行测试。
- **全局登记成本可见**：定义全局 `flag` 选项或使用 `TEST` 会在 `main` 前登记；不使用这两个功能时不会产生这两笔登记成本。
- **可裁剪**：库以 `-ffunction-sections -fdata-sections`（MSVC 为 `/Gy`）编译，链接时开启 `--gc-sections` / `-dead_strip` / `/OPT:REF` 即可剔除未用到的代码。
- **字符串一律 UTF-8**：包括文件路径；Windows 下由库内部转成 UTF-16 调用 `W` 系列 API。
- **不污染你的构建**：公共头文件不包含 `<windows.h>`、`<unistd.h>`、`<iostream>`；被 `includes` 引入时不会把 `-Werror` 强加给你。
- **头文件可独立包含**：每个公共头文件都能单独 `#include`，不依赖包含顺序。

## 文档

- [全部公共 API 与用法](api.md)：按模块列出当前接口、参数与返回值、生命周期约束和可运行示例。
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
python .github/ci_report.py # 检查当前提交的 CI 状态和测试输出
```

CI 报告支持指定运行链接、筛选 job，并区分完整日志与公开摘要，见 [检查 CI 输出](docs/ci.md)。

三个平台都是警告即错误（MSVC 上是 `/W4` 加 `/WX`）；[CI](https://github.com/NickJerome/culib/actions/runs/38411545278) 覆盖 Linux GCC、Linux Clang、macOS Clang、Windows MSVC x64 各 debug / release 两种模式，以及 MinGW-w64 x86_64、GCC 4.8、公共头文件和源码依赖引入场景，MinGW 运行测试并编译全部示例。

## 已确定的项目约定

- [x] 最低系统版本：**Windows 10**（2026-10-07 定）。构建时给 culib 定义 `_WIN32_WINNT=0x0A00`，见[路线图](docs/superpowers/specs/2026-10-03-roadmap.md)第 6 节
- [x] 头文件注释的语言与编码：**已定**，中文注释保留，公共头文件存成带 BOM 的 UTF-8，使用方不加 `/utf-8` 也不会报 C4819。CI 的 `headers` job 检查 BOM 在不在

## 版本

遵循 [语义化版本](https://semver.org/lang/zh-CN/)。1.0 之前 API 可能发生不兼容变更。

## 许可证

项目自身代码目前仅供作者自用，**保留所有权利，暂不授予使用、修改或分发许可**。详见 [权利声明](LICENSE)。

第三方代码继续适用其原有许可证：`src/runtime/asm/` 中源自 Boost.Context 的汇编保留原版权声明，并遵循 [Boost Software License 1.0](licenses/BOOST-1.0.txt)。
