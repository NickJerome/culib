# fs 标准接口设计

- 日期：2026-10-14
- 状态：**接口已实现；本地验证与提交 `e7b6b8e` 的三平台 CI 已通过（见第 5 节）。** `include/culib/fs.h`、`include/culib/fs_stream.h`、`src/fs/`、测试与示例已按本文落地。第 5 节分别记录改动期间的本地验证和最终提交的远程验证，不将旧接口的 CI 算作新接口的结果。
- 2026-10-18：`call_blocking` 已删除，协程里碰磁盘的操作直接做系统调用；工作线程被慢盘占住时，监控线程把它的名额交给另一根线程，其余协程照常跑。3.4 末尾「协程语义定死」一段是当时的做法，现行规则见[协程运行时](../../runtime.md#卡住的工作线程)。
- 取代 [2026-10-04 fs 设计](2026-10-04-fs-design.md)。那份文档保留作历史记录：它文中的「已实现」「CI 通过」只适用于旧接口。`fs::watcher` / `fs::poller` 的移除结论不变（历史见 [fs watcher 设计](2026-10-08-fs-watcher-design.md)）。

## 1. 动机

2026-10-04 版是没有消费者时的第一版设计：路径是 `std::string` / `string_view` 自由函数，布尔查询把「不存在」和「问不成」压成同一个 `false`，碰磁盘靠整文件读写三件套 `read_file` / `write_file` / `append_file`，常开追加句柄是 `output_file`，目录遍历是模板回调加「一次取全并排序」。

改版要消灭的是三类摩擦，全部指向同一个方向——**名字和形状对齐使用方已经会用的标准设施**（`std::filesystem`、`std::ostream`），删掉每个调用点都要先做一次选择的地方：

| 旧接口 | 问题 | 新接口 |
| --- | --- | --- |
| `join/parent/filename/...`（string_view 自由函数） | 路径和普通字符串混用编译器管不了；拆分函数返回视图，多一条生命周期规矩 | `fs::path` 值类型，名字对齐 `std::filesystem` |
| `exists/is_file(...)` 返回 `bool`，失败也是 false | 「没有」和「问不成」分不开 | `result<bool>`：`ok(false)` 是没有，error 是问不成 |
| `read_file/write_file/append_file` | 「整文件」是替调用方做的、只有它自己知道代价的决定；对管道、`/proc`、超大文件要么错要么得再出一版 | `fs::file` + `fs::open(path, open_mode)`，循环在调用方 |
| `output_file`：流的样子，`result` 的接口 | 同一个对象上 open/write 看 `result`、别的看句柄状态，两种失败通道并排 | `fs::ofstream`（`<culib/fs_stream.h>`）：失败只走标准流状态 |
| `list_directory` 模板回调 / 一次取全 | 提前退出、`entry::name` 视图生命周期都是要背的规矩；错误走返回值、条目走回调，两条通道 | `directory_iterator`，range-for，元素是 `result<directory_entry>` |

**不保留任何兼容层。** 库里没有下游消费者（CLAUDE.md「定位与范围」），旧名字直接删除，内部调用方（`src/cli/config_file.cpp`、`src/cli/log.cpp`、`examples/` 等）和文档在同一批改动里迁移。

## 2. 接口总览

`<culib/fs.h>`，命名空间 `wavvix::fs`：

| 分类 | 签名 | 说明 |
| --- | --- | --- |
| 路径 | `path`（`string()`、`empty()`、`is_absolute()`、`parent_path()`、`filename()`、`stem()`、`extension()`、`lexically_normal()`、`operator/=`；非成员 `operator/`、`==`、`!=`） | 拥有 UTF-8 字符串的值类型；纯字符串操作，不碰磁盘；相等是精确比较 |
| 当前目录 | `result<path> absolute(p)`；`result<path> current_path()` / `result<void> current_path(d)`；`result<path> temp_directory_path()` | `absolute` 接 cwd 后做词法整理；改 cwd 影响整个进程 |
| 查询 | `result<bool> exists / is_regular_file / is_directory`；`result<std::uint64_t> file_size`；`result<wavvix::timestamp> last_write_time` | 「不存在」是 `ok(false)`；跟随符号链接 |
| 目录与文件 | `result<bool> create_directory / create_directories / remove`；`result<std::uint64_t> remove_all` | 新建 true、已存在 false；`remove` 非空目录报 `already_exists`；`remove_all` 返回删掉的条目数；目录树稳定时只删链接本身，不跟进目标；并发替换目录不保证隔离 |
| 复制改名 | `enum class copy_options { none, overwrite_existing }`；`result<void> copy_file(from, to, options = none)`；`result<void> rename(from, to)` | 默认不覆盖；同文件复制报错不动内容；Windows 文件身份不可确认时拒绝覆盖 |
| 遍历 | `directory_entry`（`path()`、按需 `result<bool> is_regular_file / is_directory`）；`directory_iterator` + `begin/end` | 输入迭代器；range-for 元素是 `const result<directory_entry>&` |
| 句柄 | `enum class open_mode { read, write, append }`；`result<file> open(p, mode)`；`file`（`is_open()`、`result<std::size_t> read(void*, size)`、`result<void> write(const void*, size)`、`result<void> close()`） | 移动 only；`read` 0 是 EOF；`write` 写满为止 |

`<culib/fs_stream.h>`：

| 分类 | 签名 | 说明 |
| --- | --- | --- |
| 文件流 | `class ofstream : public std::ostream`，`explicit ofstream(const path&, std::ios_base::openmode = std::ios_base::out)`；`bool is_open() const noexcept`；`void close()` | 默认截断、`std::ios::app` 追加；`binary` 允许但不改变语义（`file` 层本来就一律二进制）；失败只看流状态；无 `open()` 重开、无 `std::error_code` |

完整签名和可运行形状见 [api.md](../../../api.md#fs) 与 [API 指南](../../api-guide.md)。

## 3. 关键决定

### 3.1 `path` 是拥有字符串的值类型

推翻旧 2.1「没有 path 类型」的结论。旧设计的收益是「每个接口少一层转换」，代价是路径和普通字符串混用时编译器发现不了。值类型两边都要：构造接受 `const char*` / `string_view` / `std::string`，调用点没有额外仪式；返回一律给值（`parent_path()` 等），视图生命周期规矩整个消失。

名字直接采用 `std::filesystem` 的形状（`parent_path` / `filename` / `stem` / `extension` / `lexically_normal` / `is_absolute` / `operator/`）：「看名字就会用」是双向的，标准库定过名字的东西用标准的。相等比较是精确字符串比较、不做归一，和 `std::filesystem::path` 一致，也就不存在「比较路径会不会悄悄碰磁盘」的问题。

路径运算仍是纯字符串操作、零系统调用——旧 2.3「每个函数只做名字说的那件事」和 HDD/网络盘的账原样保留。`lexically_normal` 不解析符号链接，`absolute` 只补 cwd 再归一。Windows 接受 `/` 与 `\`，`operator/` 只为它自己插入的那一个分隔符用原生字符——这两条旧结论不变。

### 3.2 三态结果：「不存在」是答案，不是错误

所有碰磁盘的操作返回 `result`。「那里没有」是可用的信息（`exists` 的 `ok(false)`、`remove` 的 false、`remove_all` 的 0）；「问不成」（权限、I/O、非法路径）才是 error。旧版用 bool 把两者压成一个答案，调用方想知道为什么还得再问一次——多付一次磁盘往返，还没了答案。`error_kind` 的映射沿用旧 3.5 那张表（`not_found` / `permission_denied` / `already_exists` / `invalid_argument` / `io`），消息一律带路径。

路径含内嵌 `'\0'` 的一律 `invalid_argument`，在动手之前检查，磁盘碰不到；`ofstream` 的对应形状是打开失败反映在流状态上。绝不把 NUL 截掉去开前缀路径。

`create_directory` 返回 `result<bool>`：「确保存在」的真实需求下，新建与否是调用方偶尔要用的信息，被同名文件挡住报 `already_exists`、不删文件让路。`copy_file` 只给 `none` / `overwrite_existing`——「目标存在就跳过」不是复制的选项，调用方先 `exists` 再 `copy`，让两次 I/O 显式在调用点上；默认不覆盖、源目标同文件报错不 truncate，两条旧教训原样保留。Windows 上无法确认文件身份时拒绝覆盖，不把源文件交给 `CopyFileW` 的未承诺行为冒险。

### 3.3 遍历：输入迭代器，错误是元素类型的一部分

旧 3.1 用「头文件里的模板循环 + `detail::directory_handle`」同时满足「回调零开销」和「公共头文件不含平台头」。这次改版把冲突连根拿掉：**回调不再是接口**。`directory_iterator` 是输入迭代器，公共头文件只有一个指向引用计数共享状态的指针，状态（原生目录句柄 + 当前位置 + 当前元素）定义在 `src/fs`——平台头不进来，`std::function` 也不进来。

- 元素是 `result<directory_entry>`：「这一项读失败了」和「这是一个目录项」本来就是两种东西，放进同一个类型里，调用方一个 `is_err()` 就分开。**打开失败在第一个元素报出来**（之后遍历结束），不是异常也不是静默的空遍历——静默空遍历把「问不成」答成「空的」，正是 3.2 消灭过的那种假答案，而且它只在网络盘断连时才出现。中途出错在这一项报错误元素、错误项之后结束。
- 拷贝共享同一个遍历位置（标准输入迭代器的语义）。不做「拷贝各自一份快照」：那要求预读整个目录，在 HDD 和网络挂载上是替调用方做的没有依据的决定。
- 顺序不保证、不排序、只遍历一级，递归自己套。旧的「一次取全并排序」重载随回调一起删除：可重现性拿 `std::sort` 就有，不该让每次遍历都替它付钱。
- `directory_entry` 类型判断按需 `stat`，建条目不多做一次——同 3.1 的账。

### 3.4 删掉整文件三件套：句柄 + 调用方的循环

`fs::file` 是原生字节句柄：`read` 返回读到的字节数（0 表示文件末尾，不是错误），`write` 是 `result<void>`、**写满为止**——短写循环收进库，因为「部分写入却报成功」不可能是一种成功，而每次让调用方自己写短写循环必然有人漏。`close` 返回 `result<void>`：POSIX 的 close 错误带着真实信息（缓冲刷不出去就死在这）；析构照样关，但没法上报，头文件写明。移动 only，一律二进制（旧 3.6 不变）。

不提供 `read_file` / `write_file` / `append_file` 包装：整文件读写的循环写出来就是示例里那七八行，而「整文件」这个决定（要不要先问大小、缓冲区给多大、遇到无大小的目标怎么办）只有调用方知道代价——这正是 CLAUDE.md「不替调用方做只有它才知道代价的决定」。真出现需要一行的消费者时再议。

协程语义定死：**协程里调碰磁盘的操作，库自动经 `call_blocking` 放到 runtime 的后台线程，等待期间只挂起当前协程**；协程外就在调用线程上同步执行。和 `net::resolve` 同一个形状。理由：文件操作天生阻塞，「要不要自己包一层」是每个调用点都要做的选择，忘了就卡住整根工作线程——把选择删掉。`tests/fs_stream_test.cpp` 里 `fs_stream.bare_file_io_in_a_coroutine_does_not_stall_the_worker` 用 FIFO / 命名管道把「等待期间别的协程照常跑」钉成验收标准。

### 3.5 `output_file` → `fs::ofstream`：失败只说流的语言

`output_file` 的接口是流的样子、`result` 的内心：`open()` 返回 `result`、`write()` 返回 `result`、`close()` 却是 `noexcept void`，还有个「open 会先关旧句柄」的重开口子。使用方在同一个对象上要同时看两种失败通道——「每个调用点先想一下」的标准气味。

新接口是一个真正的 `std::ostream` 子类：构造带路径（加 `std::ios::app` 可选），写入走 `<<` 或 `std::ostream&`，**失败只有标准流状态这一种说法**——`good()` / `fail()` / `operator bool`；`close()` 返回 `void`，关闭失败置失败位；接口里不出现 `std::error_code`；没有成员 `open()`，换目标就构造新的。解法和 result 错误类型那次一样：不给选择写更好的指南，把选择删掉——碰原生字节用 `file`（处处 `result`），要流用 `ofstream`（处处流状态），没有并排。

缓冲与 `flush()` 行为跟 `std::ostream`；写失败（Linux `/dev/full` 那种：打开成功、刷出去才炸）表现为状态变坏，`tests/fs_stream_test.cpp` 有对应检查。旧的「比每行开关一次快 N 倍」式的性能说法随旧接口一起作废，新接口不做任何性能主张。

`ofstream` 单独成头（`culib/fs_stream.h`）：`fs.h` 的公共面保持「result + 原生句柄」，流类型的设施按需包含。

## 4. 不做

- **旧接口不留兼容**：无别名、无 deprecated 重载，内部调用方与示例同批迁移。
- **`ifstream` / `fstream`**：没有消费者（日志侧全是写）。做的时候失败通道同样只用流状态，与 3.5 一致。
- 符号链接的创建与读取、权限读写、硬链接、文件锁、内存映射、递归遍历：旧第 1 节的否决不变。
- 文件监听：仍无消费者，历史设计见 [2026-10-08-fs-watcher-design.md](2026-10-08-fs-watcher-design.md)。
- 排序的目录列表、`path` 隐式转字符串（读的一律 `string()`）、`operator<<`（不往公共头文件塞流设施）。

## 5. 实现验证

实现与迁移已完成：`include/culib/fs.h`、`include/culib/fs_stream.h`、`src/fs/`（`directory_iterator` 共享状态、`file_streambuf`），`culib.h` 汇总包含，测试（`fs_test.cpp`、`fs_stream_test.cpp`、`fs_copy_source_test.cpp`、`fs_win_edge_test.cpp`）与调用方（`src/cli/`、`examples/`）均已切到新接口。本地验证（2026-10-14，提交和远程 CI 之前在本地检出上跑）：

- **最终 POSIX FIFO 复制修复后的集成复测**：Linux x86_64 GCC 11.4 debug 与 Clang 14 release 的 `xmake` 库、全部示例构建及 13/13 测试目标通过；GCC debug 的 `culib_test` 为 406/406。复制源、覆盖目标为 FIFO 的两个专项用例在修复前分别被 5 秒超时中止，修复后在 GCC 与 Clang release 中各通过 2/2；无读者的目标 FIFO 可返回 `ENXIO` 对应的 `io` 错误，测试只要求及时报错且文件内容不变。Windows MSVC 2022 x64 debug 与 release 在这次仅涉及 POSIX 的复制修复**之前**通过本地库、示例构建和各 12/12 测试目标，修复之后没有在本地重跑 Windows；最终提交的 Windows 远程验证见下文。`include/` 全部 30 个公共头文件的 C++11 独立编译与 BOM 检查通过。`tests/fs_stream_test.cpp` 的 POSIX FIFO 夹具曾漏建管道，导致一次无异常配置测试因普通文件打开竞态失败；补 `mkfifo` 后该用例单独重复 30 次及完整无异常测试通过。
- **最终 POSIX FIFO 复制修复后重跑并通过**：Linux x86_64 GCC 11.4 release `-fno-exceptions -fno-rtti` 的库、全部示例构建与 13/13 测试目标。
- **Sanitizer**：最后两条 POSIX FIFO 复制回归加入并修复后，Linux GCC debug 以 `-fsanitize=address,undefined -fno-sanitize-recover=all` 重新编译；按 CI 的限定目标运行 10/10 测试目标，`culib_test` 的 406/406 用例通过。ThreadSanitizer 按 CI 的进程级检查构建并运行 `threads`、`sigint`、`sigterm` 三种模式，均通过；没有在 TSan 下运行全部文件系统测试。此项是本地运行，不是 CI 结果。
- **api.md 示例**：从 [api.md](../../../api.md) 提取的全部 23 个独立 C++ 程序（含 fs 一节）在 Linux x86_64 GCC 11.4（`-std=c++11`）与 Windows x64 MSVC 2022 下警告即错误编译并运行，30 次调用的退出码与输出均与正文一致；`examples/fs.cpp` 同日在两个平台编译运行成功。记录见 api.md「本文示例验证」。
- **最终远程 CI**：[GitHub Actions build 运行](https://github.com/NickJerome/culib/actions/runs/38567733275) 对应提交 `e7b6b8e`（2026-10-14），**24/24 job 通过**。使用 `python .github/ci_report.py 38567733275` 取得并检查了 24/24 完整日志：Linux GCC/Clang x86_64、GCC arm64、macOS Clang arm64/x86_64、Windows MSVC x64 的 debug/release；MinGW x86_64、GCC 4.8、公共头文件、无异常/无 RTTI、ASan + UBSan + 进程级 TSan 以及三平台 C++11/C++17 源码依赖消费均通过。标准测试目标 Linux 13/13、macOS 与 Windows 各 12/12；`culib_test` 标准 Linux 矩阵 406、macOS 401、Windows/MinGW 404、GCC 4.8 406 项通过；Linux GCC/Clang 无异常/无 RTTI 配置各 399 项，Sanitizer job 的限定测试目标 10/10。日志提取无 SKIP；唯一诊断是 Linux C++11 消费 job 的缓存未命中警告（未找到 `.xmake-build-cache`，随后构建通过），因此报告脚本按严格诊断规则退出 1，**不是 CI 失败**。源码依赖消费 job 只做编译/运行检查，不从缺失的测试摘要推断单元测试项数。此前 `9c46198` 的第一次 CI 为 23/24（GCC 4.8 编译失败）；默认构造函数移到源文件定义后以本次成功运行作最终依据。
- **边界**：旧接口的 CI 与验证记录（2026-10-04 设计文档第 4 节）不适用于本文；本次远程 CI 不能代替未在矩阵覆盖的 VS2015 或 Windows 零索引卷实测。
