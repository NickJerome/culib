# culib

基于 **C++11** 与 **xmake** 的跨平台（Windows / Linux / macOS）便携开发库，以静态库形式提供。追求高性能、低占用、尽可能零开销，同时让使用者零理解负担。

culib 从个人实际需求出发，把各个项目里反复编写的基础代码沉淀下来、统一封装平台差异，让新程序可以直接在它之上快速搭建，而不必每次从零开始。

> **状态**：早期规划阶段，模块与 API 尚未稳定。

## 设计目标

- **高性能、低占用、尽可能零开销**：最高优先级。遵循零开销原则：不用的功能不产生任何代价，用到的功能不比手写的更慢。具体约束见「开发约定」中的「性能与占用」。
- **使用者友好、零理解负担**：接口直观、一致、难以误用，看名字就会用，不需要了解内部实现。好用与高效不是二选一：尽量让最直观的写法同时也是最高效的写法。具体约束见「开发约定」中的「易用性」。
- **C++11**：C++11 是目前最广泛可用的标准。为保持泛用性，库只使用 C++11 的语言特性与标准库；C++11 缺失但常用的设施按需由 culib 自行提供。
- **跨平台一致**：同一套 API 在 Windows / Linux / macOS 上行为一致，平台差异全部收敛在库内部。
- **静态库**：编译为单个静态库，程序链接后无需额外分发 culib 的动态库。
- **零第三方依赖**：只依赖 C++ 标准库和操作系统 API，拿到源码即可编译。
- **模块化**：模块之间低耦合，按需使用，未用到的代码不会进入最终程序。
- **接入简单**：xmake 工程中一行 `add_deps("culib")` 即可使用，头文件路径与系统库依赖自动传递。

## 平台与工具链

| 平台 | 编译器 | 架构 |
| --- | --- | --- |
| Windows | MSVC（Visual Studio 2015+）、MinGW-w64 | x86、x64 |
| Linux | GCC 4.8.1+、Clang 3.3+ | x86_64、arm64 |
| macOS | Apple Clang（Xcode / Command Line Tools） | x86_64、arm64 |

- 编译器下限取各家首个完整支持 C++11 的版本（VS2015 为基本完整）。
- 构建工具：[xmake](https://xmake.io) 2.8.5 及以上（`xmake test` 自该版本起提供），推荐使用最新版。

## 模块规划

> 以下是候选模块，最终以实际需求为准，逐步补充和调整。

| 模块 | 内容 | 状态 |
| --- | --- | --- |
| base | 平台 / 编译器检测宏、版本信息、`Result` 错误处理、作用域守卫；C++14/17 常用设施的 C++11 实现（对应 `make_unique`、`optional`、`string_view` 等） | 规划中 |
| string | split / join / trim / 大小写转换 / 格式化；UTF-8 ⇄ UTF-16 转换 | 规划中 |
| fs | 路径处理、文件与目录操作、整文件读写（C++11 没有 `std::filesystem`） | 规划中 |
| time | 时间戳、日期时间格式化、高精度计时 | 规划中 |
| thread | 线程池、任务队列、读写锁（C++11 没有 `shared_mutex`）等 | 规划中 |
| os | 进程、环境变量、系统信息、程序自身路径、动态库加载 | 规划中 |
| log | 轻量级日志 | 规划中 |

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
xmake test                     # 编译并运行全部测试
xmake test "<测试目标>/*"       # 只运行某个测试目标（zsh 下需要加引号）
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
#include <culib/culib.h>   // 汇总头文件；也可以只包含用到的模块头文件
#include <cstdio>

int main() {
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

## 目录结构

```
culib/
├── xmake.lua                   # 构建脚本
├── README.md
├── include/
│   └── culib/                  # 公共头文件（对外 API）
│       ├── culib.h             # 汇总头文件
│       └── <模块>.h
├── src/                        # 实现代码（不对外暴露）
│   └── <模块>/
│       ├── <模块>.cpp          # 平台无关实现
│       ├── <模块>_win.cpp      # Windows 实现
│       └── <模块>_posix.cpp    # Linux / macOS 共用实现
├── tests/                      # 单元测试
├── benchmarks/                 # 基准测试
└── examples/                   # 示例程序
```

## 开发约定

### 命名

- **命名空间**：`wavvix`，全小写；内部实现放在 `wavvix::detail`，使用方不应依赖。
- **类型**（类、结构体、枚举、类型别名）：UpperCamelCase，如 `Result`、`ThreadPool`。
- **函数、方法、变量、参数**：lowerCamelCase，如 `readFile()`、`isOk()`。
- **枚举值**：UpperCamelCase，如 `LogLevel::Warning`。
- **宏**：全大写加 `WAVVIX_` 前缀，如 `WAVVIX_PLATFORM_WINDOWS`。

### 错误处理

- 可能失败的操作通过 `Result` 返回结果或错误，由调用方显式检查；库不主动抛出异常。
- 不依赖 RTTI。公共头文件中不出现 `throw`、`try`、`dynamic_cast`、`typeid`，因此可以用于关闭了异常和 RTTI（`-fno-exceptions` / `-fno-rtti`）的工程。

### 性能与占用

- **热路径可内联**：热路径上的小函数放在头文件中（`inline` / 模板）。静态库里的普通函数在不开启 LTO 时无法被跨库内联。
- **少分配、少拷贝**：避免不必要的堆分配和拷贝；参数优先传引用或字符串视图，支持移动语义；需要反复产出结果的接口，额外提供写入调用方缓冲区的重载。
- **慎用有隐性开销的设施**：虚函数、`std::function`、`std::shared_ptr`、`<iostream>` 等，优先使用模板与值语义。
- **无启动开销**：不在 `main` 之前执行任何代码，避免需要动态初始化的全局对象。
- **按需同步**：只在必要处加锁，不让单线程使用者为同步付出代价。
- **可裁剪**：库以 `-ffunction-sections -fdata-sections`（MSVC 为 `/Gy`）编译，使用方链接时开启 `--gc-sections` / `-dead_strip` / `/OPT:REF` 即可剔除未用到的代码。
- **以数据为准**：性能敏感的实现配套基准测试，优化以测量结果为依据。

### 易用性

- **看名字就会用**：名字直接说明行为，使用完整单词（公认缩写除外）；同类操作命名统一，如 `isXxx` / `hasXxx` 返回 bool，`toXxx` 表示转换。
- **全库一致**：同类问题只用同一种方式处理，如可能失败的操作一律返回 `Result`，字符串一律 UTF-8，参数一律输入在前、输出在后。
- **简单的事一行完成**：常用功能一行调用即可，提供合理的默认值。
- **名字描述行为，不标性能档次**（参考 `std::vector` 的 `push_back` / `emplace_back` / `reserve`）：
  - 同一件事、只是结果放的位置不同，用同名重载，两个版本平级：`readFile(path)` 返回新字符串，`readFile(path, buffer)` 覆盖写入已有的 buffer 并复用其容量；
  - 行为确实不同时才换名字，且名字说明做了什么（如 push 放入已有对象、emplace 就地构造），而不是标注「快速版」；
  - 能自动生效的优化直接做进接口（如移动语义、右值重载），使用者无需知道它的存在；
  - 调优类接口（类似 `reserve`）只是可选的附加操作，不用也完全正确；
  - 任何一个版本都不能比其他版本更容易用错。
- **零配置、零管理**：包含头文件即可使用，无需 `init()` / `cleanup()`，无需预定义宏；资源全部由 RAII 管理，接口中不出现需要使用者手动释放的指针。
- **难以误用**：用 `enum class` 代替 bool / 整数开关，如 `copyFile(src, dst, Overwrite::Yes)` 而不是 `copyFile(src, dst, true)`；编译器支持时，忽略 `Result` 返回值会产生警告。
- **报错看得懂**：模板用错时用 `static_assert` 给出明确提示，而不是一长串模板错误；运行时错误可以直接取得可读的描述。
- **没有意外**：行为符合直觉；会阻塞、会分配内存、非线程安全等需要留意的地方，在接口注释中写明。
- **头文件即文档**：每个公共头文件都能单独包含、不依赖包含顺序；公共接口在头文件中有简短注释，主要模块在 `examples/` 中有可直接运行的示例。

### 跨平台与代码组织

- **字符串编码**：对外接口中的字符串（包括文件路径）一律使用 UTF-8；Windows 下由库内部转换为 UTF-16 后调用 `W` 系列 API。
- **公共头文件保持干净**：不包含 `<windows.h>`、`<unistd.h>` 等平台头文件，避免把 `min` / `max` 之类的宏泄漏给使用方。
- **平台代码隔离**：平台相关实现按文件名后缀区分，即 `_win`（Windows）、`_posix`（Linux + macOS）、`_linux`、`_macos`，由 `xmake.lua` 按平台选择参与编译的文件。
- **C++11 合规**：MSVC 没有 C++11 模式（最低即 C++14），因此以 GCC / Clang 在 `-std=c++11` 下的编译结果作为合规标准。
- **头文件**：使用 `#pragma once`；头文件中不写 `using namespace`。
- **零警告**：以较高的警告级别编译，三个平台上保持零警告。

### 新增模块

1. 在 `include/culib/` 下添加公共头文件（附接口注释），并加入 `culib.h`
2. 在 `src/<模块>/` 下添加实现，平台相关代码按后缀拆分
3. 在 `tests/` 下添加对应测试；性能敏感的部分在 `benchmarks/` 下补充基准测试
4. 在 `examples/` 下添加用法示例
5. 更新本文档「模块规划」中的状态

## 待定事项

- [ ] 模块清单与优先级
- [ ] `Result` 的具体设计（错误的表示方式、接口形式）
- [ ] 最低系统版本（如 Windows 7 还是 Windows 10），影响可用的系统 API
- [ ] 测试与基准测试框架：自带极简实现，或引入 doctest、nanobench 等单头文件库（仅测试使用，不影响库本身零依赖）

## 版本

遵循 [语义化版本](https://semver.org/lang/zh-CN/)。1.0 之前 API 可能发生不兼容变更。

## 许可证

待定。
