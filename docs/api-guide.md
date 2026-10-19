# 公共 API 的用法与设计说明

这里整理了公共头文件里原先过长的用法示例和设计说明。函数本身的参数约束与容易误用的行为仍写在声明旁；完整设计与取舍见各节链接。

全部公共接口的签名和可运行示例见根目录 [api.md](../api.md)。

## [`bytes.h`](../include/culib/bytes.h)

完整背景见 [设计文档](superpowers/specs/2026-10-05-bytes-design.md)。

字节缓冲：一段可增长的字节 + 一个读游标 + 一个写游标，外加**显式字节序**的整数读写。

`write()` 可以把缓冲区里尚未消费的字节再次追加到自身；即使这次写入触发压缩或扩容，源数据仍会被正确保留。源指针指向本缓冲区时，整个源区间须落在尚未消费的数据内。

复制缓冲区时只复制未读字节，不继承已消费的前缀、闲置容量或尚未 `commit()` 的空间；移动保留容量，但同样清除未提交额度。

    wavvix::buffer out;
    out.write_u32_be(payload.size());
    out.write(payload);
    send(out.data(), out.size());

### 字节序一律写明，不做「主机字节序」这种事

每个整数读写函数的名字里都带 `_be` 或 `_le`。**没有不带后缀的版本**，因为那种函数
的含义取决于你在哪台机器上跑，而协议不是。`htonl` 那套把「转换」和「目标字节序」
混成一件事，读代码的人得先知道主机是什么字节序才能回答「线上是什么」。

实现是按位移写的，不是 `htonl`：**和主机字节序无关**，所以同一份代码在大端机上
也对，而且编译器认得出这个式样、会折成一条 bswap 加一次存储（量过，见设计文档）。

### 读不够的时候

整数读函数返回 **`optional<T>`**，**不够就是空的，而且游标不动**。

不用 `result` 是因为「数据还没到齐」不是错误——那是流式解析的常态，不该为它付一次
错误消息的分配。而 `optional` 正是本库表达「可能没有值」的那个类型。

**不用「返回 bool + 出参」**（旧 `fs::read_file` 曾有一个 `std::string&` 出参重载，
已随 2026-10-14 的 fs 改版删除）。那种写法在这里有个真陷阱：把调用和读出参写进同一个
表达式时，求值顺序是未定义的——

    std::printf("%s 值是 %u", in.read_u16_be(v) ? "成了" : "不够", v);   // v 可能先被读

这不是假想的，是 `examples/bytes.cpp` 第一版真踩出来的。返回值形式写不出这个 bug。

推荐的用法是**先确认整帧都到了，再一口气解析**：

    if (in.size() < 6) return need_more;        // 2 字节类型 + 4 字节长度
    const std::uint16_t kind = in.read_u16_be().value();
    const std::uint32_t length = in.read_u32_be().value();

这样还能直接初始化 const 局部变量。逐字段判断也是安全的（不够时游标不动）：

    const wavvix::optional<std::uint32_t> length = in.read_u32_be();
    if (!length.has_value()) return need_more;

### 给 I/O 用的那三个

`prepare` / `commit` / `consume` 是给「直接读进缓冲、直接从缓冲发出去」用的，
可以少一次拷贝：

    unsigned char* room = in.prepare(4096);
    const ssize_t got = read(fd, room, 4096);
    if (got > 0) in.commit(got);                // 只认账真读到的那些

    const ssize_t sent = write(fd, out.data(), out.size());
    if (sent > 0) out.consume(sent);            // 发出去多少就丢掉多少

`commit()` 可分次调用，但累计字节数不能超过最近一次 `prepare(n)` 的 `n`；超出时即使底层还留有容量也会终止进程。再次调用 `prepare()`、`reserve()`、`consume()` 或 `clear()` 会使前一次取得的可写空间失效。`prepare(0)` 不分配内存，空缓冲区返回空指针。

`prepare()` 返回的空间内容未初始化，只能 `commit()` 已写入的字节。扩容只复制未读数据，不会清零新增容量。

## [`config.h`](../include/culib/config.h)

完整背景见 [设计文档](superpowers/specs/2026-10-05-config-design.md)。

配置文件：ini 的一个子集。

    const wavvix::result<wavvix::config> loaded = wavvix::config::load("app.conf");
    if (!loaded.is_ok()) return loaded.error();
    const wavvix::config& settings = loaded.value();

    const wavvix::result<long long> port = settings.get_int64("server.port", 8080);
    if (!port.is_ok()) return port.error();        // 文件里写错了

**行尾注释的规则：前面有空白的 `#` 或 `;` 开始注释。** 不在任何 `#` 上都切，
是因为 URL 的片段和密码里带 `#` 是常事；要求有空白隔开既符合直觉又好记。

认的语法就这些：

    # 井号开头是注释
    ; 分号也是
    name = culib                 # 等号两边的空白都会去掉，行尾注释也会去掉
    greeting = " 带空格 "        # 带引号的值到下一个引号结束，首尾空格留得住
    url = http://x/#fragment     # `#` 前面没有空白，所以它不是注释
    note = "a # b"               # 引号里的 `#` 也不是注释

    [server]                     # 段，下面的键变成 server.xxx
    port = 8080
    host = 0.0.0.0

**嵌套用 `section.key` 这种扁平键表示，不做任意深度的树。** 这样就不需要
`variant` 那种递归值类型（而 `variant` 已经评估过不做，见
superpowers/specs/2026-10-03-variant-design.md）。

### 为什么是 ini 而不是 toml 或 json

路线图定了「ini 或 toml 子集，不要 json」，这里取 ini。**toml 要回答的问题比 ini
多得多**：数组、内联表、多行字符串、日期时间、各种数字写法——每一条都是设计面，
而服务配置一条都用不上。ini 的子集反而**没有设计自由度**：注释、段、键等于值，
就这三样。

### 「不存在」和「写错了」是两件事

  - **键不存在不是错误**，用你给的默认值；
  - **键在、但值解析不了是错误**，一定报出来。

所以带类型的读法返回 `result<T>`：

    settings.get_int64("port", 8080)      // 没写 port → ok(8080)
                                          // 写了 port = 80x80 → error

**不把第二种悄悄当成默认值**：`port = 80 80` 静默变成 8080 是那种要查一下午的事。

## [`coroutine.h`](../include/culib/coroutine.h)

协程是这个库的并发模型：`go()` 起一根，`wait_group` 等一组。不用建运行时，第一次 `go()` 时自己起来：

    #include <culib/coroutine.h>
    wavvix::wait_group group;
    group.go([&](int count) {
        for (int i = 0; i < count; ++i) {
            produce(i);
            wavvix::yield();
        }
    }, 3);
    group.wait();                        // 在 main 里等就是阻塞 main 线程

`go(f, args...)` 和 `std::thread` 一样接受函数及其参数；参数先按值保存，运行时移动到函数中，传引用用 `std::ref`。没有句柄，所以没有「忘了 join 就 terminate」这回事；要等就放进 `wait_group`，要结果就送进 `channel`。`wait_group` 只有 `go()` 和 `wait()` 两个成员：没有 Go 经典的 `add` / `done`，也就不会出现忘了 `done`、`add` 晚于协程启动这类错误（Go 1.25 加的 `WaitGroup.Go` 是同一个理由）。

### 工作线程

运行时的工作线程数取 `os::cpu_count()`（Linux 上按 CPU 亲和性和 cgroup 配额算），环境变量 `WAVVIX_MAXPROCS` 可以覆盖，和 Go 的 `GOMAXPROCS` 一样不在代码里设。普通线程里起的协程排进全局队列，协程里起的协程先排进本线程的队列；闲着的工作线程从全局队列取，也从别的工作线程的队列里拿。

于是：**不同工作线程上的协程是并行的**，共享数据要加锁。**协程在挂起点可能换线程**，和 Go 一样：`thread_local`、`errno`、`std::this_thread::get_id()` 和拿在手里的 `std::mutex` 都不能跨过挂起点，catch 块里不能挂起（打印诊断后终止）。规则和原因见[协程运行时](runtime.md#协程在哪根线程上跑)。

### main 返回就退出

运行时启动以后一直在，没有关掉它的接口。`main` 返回时进程直接退出，还没结束的协程不再运行，和 Go 一样。

### 阻塞调用

协程只在挂起点切换：等 I/O、等锁、`sleep_for`、`yield`、`wait`。协程里执行真正阻塞线程的函数会占住所在的工作线程，这种调用交给 `call_blocking(f, args...)`：放到后台线程上，等待期间只挂起这根协程。返回值和异常都带回调用处，参数和直接调用一样传。后台线程整个进程共用，最多 64 根。

### 不用协程就什么都不初始化

一个不用协程的程序（比如 CLI）一分钱不付，「不在 main 之前执行任何代码」也没破。

底层手动换栈原语在 `detail::fiber`，不是公开的任务接口。事件引擎在 Linux、macOS、Windows 上分别使用 epoll、kqueue、IOCP，整个进程一个。实现背景见[协程设计记录](superpowers/specs/2026-10-04-coroutine-design.md)；当前用法见[协程运行时](runtime.md)。

## [`defer.h`](../include/culib/defer.h)

defer：在当前作用域结束时执行一段代码，类似 Go 的 defer。

    std::FILE* file = std::fopen(path, "rb");
    if (!file) {
        return ...;
    }
    defer { std::fclose(file); };

行为说明：
- 在所在作用域（最近的一对花括号）结束时执行，而不是函数返回时；
  写在循环体里则每轮结束都执行一次。
- 同一作用域内有多个 defer 时，按声明的相反顺序执行（与 Go 相同）。
- 以引用方式捕获外部变量，执行时读到的是变量当时的值
  （Go 会在 defer 语句处就对参数求值）。
- 块内不应抛出异常，抛出会直接终止程序；块内的 return 只结束 defer 块本身。
- 支持 `__COUNTER__` 的编译器允许同一行写多个 defer；回退到 `__LINE__` 时须分行写。
- 零开销：不分配内存，不使用虚函数或 std::function，优化后与手写的清理代码等价。

如果与其他库中名为 defer 的标识符冲突（如 Asio 的 asio::defer），
在包含本头文件之前定义 WAVVIX_NO_DEFER_KEYWORD，改用 WAVVIX_DEFER { ... };

## [`flag.h`](../include/culib/flag.h)

完整背景见 [设计文档](superpowers/specs/2026-10-04-flag-design.md)。

命令行参数。通常把选项定义为全局对象，定义在哪里就能在哪里读，不用从 main 一路传下去。

    #include <culib/flag.h>
    #include <iostream>

    wavvix::flag::string address("localhost", "server listen address", {"address", "addr"});
    wavvix::flag::int64 port(wavvix::flag::required, "server listen port", {"port", "p"});
    wavvix::flag::boolean verbose(false, "print details", {"verbose", "v"});

    int main(int argc, char* argv[]) {
        wavvix::flag::init(argc, argv); // 失败时打印用法并退出进程
        std::cout << address.value() << ':' << port.value() << '\n';
    }

例如传入 `--port=9000` 会输出 `localhost:9000`；不传 `--port` 会报告缺少必填选项并以退出码 2 结束。
`--help` 和 `--version` 不要求填写必填选项，因此即使尚未准备好运行参数，也能查看用法和版本。
`init()` 在帮助或版本输出后以 0 退出；`parse()` 不退出进程，改由
`help_requested()` / `version_requested()` 报告请求。重复解析会先恢复全部默认值。

### 陷阱：别用 auto 拷贝

    auto v = verbose;          // 拷的是 flag::boolean（而且编译失败，不可拷贝）
    auto v = verbose.value();  // 这才是 bool

隐式转换是为了「像普通变量一样读取和输出」而保留的，代价就是这一条。选项不可拷贝，
所以第一种写法是编译错误而不是静默的怪行为。

### 接受的写法

  --port=8080   --port 8080   -p 8080   --addr=localhost   --verbose   --verbose=true   --   -

帮助文本与错误消息把单字符名字显示为 `-p`，多字符名字显示为 `--addr`；主名字和别名遵循同一规则。

不支持粘连短选项 -p8080（和 -p -8080 容易混），也不支持合并 -abc。

四条解析规则：

  1. 位置参数之后继续解析选项（GNU 风格）。`crop a.png --width=1024 b.png` 里
     --width 生效。不用「遇到第一个非选项就停」那套——那会让 `crop *.png --width=1024`
     静默失效，是 Go flag 包最常被抱怨的一点。
  2. **布尔选项不吃下一个 token**，只接受 --verbose 和 --verbose=true。否则
     `crop --verbose a.png` 里的 a.png 会被当成 --verbose 的值。
  3. `--` 之后全部算位置参数，哪怕长得像选项。处理以 '-' 开头的文件名要靠它。
  4. 单独一个 `-` 算位置参数（惯例表示标准输入）。

**未知选项报错退出**，不收进位置参数——静默接受拼错的 --widht 是最常见的 CLI 坑。

### 生命周期

args() 返回的视图指向 argv，和进程同寿，不拷字符串。C++ 标准允许程序修改 argv，
所以**init 之后不要改写 argv**（例如 setproctitle 那样改进程标题），否则视图会跟着变。

需要用配置文件补充选项时，先成功调用 `flag::parse()`，再调用
`config::apply_to_flags()`。`was_given()` 只表示最近一次命令行解析是否给过该选项；
配置文件通过 `flag::set()` 写入的值不会改变它，因此重新加载配置时仍能更新未被命令行
指定的选项。`set()` 在解析成功前会报错。一次解析失败会清空位置参数和来源标记，
先前成功解析出的值也不可再读取；修正参数后重新解析即可。`value()` 在成功解析前
被读取时会打印诊断并终止程序。

选项的名字和说明字符串必须是静态存续的（字面量就行），库不拷贝它们；名字列表的第一个元素是主名字。
局部选项也可以使用，析构时会从登记表注销。若在一次成功解析后再定义选项，
之前的解析状态失效，需要重新调用 `parse()` 或 `init()` 才能读取选项值。

全局选项对象在 main 之前登记，这是「无启动开销」的第二个例外（第一个是 TEST）。

## [`fs.h`](../include/culib/fs.h)

完整背景见 [fs 标准接口设计](superpowers/specs/2026-10-14-fs-standard-interface-design.md)。
[2026-10-04 的 fs 设计](superpowers/specs/2026-10-04-fs-design.md)描述的是被取代的旧接口
（string_view 路径、整文件读写、`output_file`、回调式 `list_directory`），保留作历史记录。

文件系统。C++11 没有 std::filesystem，这是缺失感最强的一块。

    const wavvix::fs::path config = dir / "app.conf";
    const wavvix::result<bool> present = wavvix::fs::exists(config);
    if (present.is_err()) return std::move(present).error();
    if (!present.value()) return use_defaults();

    for (const wavvix::result<wavvix::fs::directory_entry>& item :
         wavvix::fs::directory_iterator(dir)) { ... }

约定：

- 路径是值类型 `fs::path`，拥有自己的 UTF-8 字符串（内部 `std::string`），复制、移动
  都安全。名字对齐 `std::filesystem`：`parent_path` / `filename` / `stem` /
  `extension` / `lexically_normal` / `is_absolute` / `operator/`，看名字就会用。
  相等比较是精确字符串比较，不做归一；要先统一形状再比就先 `lexically_normal()`。
- 碰磁盘的接口一律返回 `result`，路径里含内嵌 NUL 的一律报 `invalid_argument`，
  动手之前磁盘碰不到，也不会把 NUL 前的路径误当成目标文件。
- **「不存在」和「问不成」分开**：`exists` / `is_regular_file` / `is_directory` 返回
  `result<bool>`，不存在是 `ok(false)`，权限不足之类才是错误。旧版那种「bool 查询、
  失败返回 false」把两种情况压成一个答案，已经废掉。
- Windows 接受 '/' 和 '\\' 两种分隔符；POSIX 上 '\\' 是普通文件名字符。
  **`operator/` 只为自己插入的那一个分隔符用平台原生字符，不改写输入里已有的**：
  Windows 上 `path("/a") / "b"` 是 `"/a\\b"` 而不是 `"\\a\\b"`。混用在 Windows 上是
  合法的（"C:/foo\\bar" 能用）；要把形状统一就 `lexically_normal()`。这条是 CI 第一次
  在 Windows 上跑时，被一个写错的测试暴露出来的。POSIX 上 `/b` 是绝对路径，Windows
  上它只是当前盘的根，完整的绝对路径须带盘符或 UNC 前缀。
- 路径运算全是纯字符串操作，零系统调用：`lexically_normal()` 消去 `.`、`..` 和重复
  分隔符，不解析符号链接；`absolute()` 把相对路径接到当前目录后再做同样的词法整理，
  也不解析符号链接。
- **符号链接默认跟随**（用 stat 而不是 lstat）：`exists` / `is_regular_file` /
  `file_size` 问的是「那里有个能打开的文件吗」。例外全在删除一侧：`remove` 删链接
  本身不跟进，目录树保持稳定时，`remove_all` 递归删除链接本身，不进入链接目标。
  如果其他进程同时把目录替换成链接，基于路径的删除无法保证隔离；不要用它
  清理可被不可信进程并发修改的树。
- Windows 上接近 `MAX_PATH` 的普通长路径会在调用系统 API 前转成扩展长度的绝对路径；
  调用方无需手写 `\\?\` 前缀。短路径不额外查询完整路径。输入很短但当前工作目录极深
  的相对路径仍可能碰到 Win32 长度限制。
- 目标存储包含 HDD 和网络挂载。一次 stat 在本地缓存热时 ~475 ns，在冷的 HDD 上
  5–10 ms，在 NFS 上 0.2–1 ms。**所以这里每个函数都只做它名字说的那件事**，
  不替调用方多问一次磁盘：需要类型就直接 `is_regular_file` / `is_directory`，不用先问
  `exists`；`directory_entry` 建条目不多做一次 stat。见 CLAUDE.md「性能与占用」。
- 读写一律二进制，不做换行转换：读到的就是磁盘上的字节，内嵌 `'\0'` 保留。
- **协程里的文件操作自动经 `call_blocking`** 放到 runtime 的后台线程执行，等待期间
  只挂起当前协程，工作线程照常跑别的协程；协程外同步执行，调用方不用自己包。

`fs::file` 是原生字节句柄（`fs::open(path, open_mode::read/write/append)`）：
移动 only，析构关闭；`read` 返回读到的字节数、0 表示末尾，`write` 写满为止
（`result<void>`，内部处理短写），`close` 返回 `result<void>`。整文件读写这种组合
由调用方用分块循环写出来，库不再提供 `read_file` / `write_file`——那三个函数每种都要
先分配再拷贝一次，而且对管道、`/proc` 这类无大小目标反而碍事。`open_mode::append`
使用系统的追加模式，每次底层写入都落在当时的文件末尾；大内容可能拆成多次写入，
不保证整段内容与其他进程的写入互不交错。

`directory_iterator` 是输入迭代器：拷贝共享同一个遍历位置，不是各自一份快照。
range-for 的元素是 `const result<directory_entry>&`——打开失败在第一个元素里报出，
中途出错在这一项报出错误元素、错误元素之后即到尾（错误只报一次），永远不会「静默的空目录」和「目录项、错误分两种通道」
并存。遍历顺序不保证、不排序，只遍历一级，要递归自己套。旧的回调式
`list_directory`（模板回调 + 提前返回 `false`）和「一次取全并排序」的版本都已删除：
回调的提前退出、名字视图的生命周期这些规矩，range-for + `break` 不需要。

`create_directory` 新建返回 true、已存在返回 false；`create_directories` 逐级创建，
被同名文件挡住报 `already_exists`，不删文件让路。`remove` 对不存在返回 false，
对非空目录报 `already_exists`——这是安全阀，整棵树交给 `remove_all`（返回删掉的条目
数，不存在是 0）。`copy_file` 默认不覆盖，覆盖必须显式写 `copy_options::overwrite_existing`；Windows 上如果卷无法提供可靠的文件身份，覆盖操作会返回错误而不是冒险清空可能相同的源文件。
「跳过已存在的目标」不是复制的选项，先自己 `exists` 判断。POSIX 复制会保留权限位；
若设置权限失败则返回错误，此时目标可能已有复制内容；复制不保证保留权限以外的元数据。
`rename` 在同一文件系统内是原子操作；POSIX 跨文件系统会返回错误。

## [`fs_stream.h`](../include/culib/fs_stream.h)

`fs::ofstream`：继承 `std::ostream` 的输出文件流，取代旧的 `fs::output_file`。

    wavvix::fs::ofstream report(path);              // 默认截断，写一律二进制
    report << "count=" << n << '\n';
    report.close();                                 // void：成没成看流状态
    if (report.fail()) return report_failure();

    wavvix::fs::ofstream again(path, std::ios::app);  // 追加到当时的末尾
    static_cast<std::ostream&>(again) << "line\n";    // 走通用的 std::ostream& 路径

**它的失败只有标准流状态这一种说法**：打开失败、写入失败、`close()` 失败都反映在
`good()` / `fail()` / `operator bool` 上；`close()` 返回 `void`，接口里没有
`std::error_code`，也没有成员 `open()` 可以重开——要换目标或重开就构造新对象。
这是和 `fs::file` 的分工，不是两套并存的选择：碰原生字节用 `file`，每个操作自己查
`result`；要 `<<`、要交给收 `std::ostream&` 的通用代码，用 `ofstream`，它按标准流的
规矩报告失败。同一段代码里不存在「这一行看 result、下一行看流状态」的先想一下。

缓冲与 `flush()` 的行为跟着 `std::ostream` 走；`close()` 会先 flush，失败置失败位。
坏流上的写入是 no-op：打开失败（比如路径含 NUL）之后继续 `<<` 不会崩，也不会把
NUL 截掉去打开前缀路径。

## [`lexical_cast.h`](../include/culib/lexical_cast.h)

完整背景见 [设计文档](superpowers/specs/2026-10-04-lexical_cast-design.md)。

字符串 ⇄ 数值 / 布尔。

    wavvix::result<int> port = wavvix::lexical_cast<int>(text);
    if (port.is_err()) return std::move(port).error();

    std::string s = wavvix::to_string(8080);        // 反方向不会失败，直接返回值

解析是严格的，和 strtol 的默认行为相反：整个输入必须被完全消费，不接受前后空白。
lexical_cast<int>("12abc")、lexical_cast<int>(" 12")、lexical_cast<int>("") 都失败。
理由是「没有意外」——宽松解析会把 --port=80x 静默当成 80。要容忍空白自己先 trim。

失败时 kind 是 error_kind::invalid_argument（格式不对）或 error_kind::out_of_range
（超出目标类型范围），消息里带上出错的那段输入和允许的范围。要说明「这是哪个字段」
时，在自己那一层把上下文拼进去，见 CLAUDE.md「错误处理」。

不受 locale 影响：小数点一律是 '.'。整数完全自己解析；浮点的语法自己严格校验，
`float` 交给 `strtof`、`double` 交给 `strtod` 正确舍入，先把 '.' 换成当前 locale 的
小数点（包括多字节小数点）。库不调用进程级的 `setlocale`。`long double` 目前没有实现，
传给 `lexical_cast`、`to_string` 或 `to_chars` 会在编译期报错。

## [`log.h`](../include/culib/log.h)

完整背景见 [设计文档](superpowers/specs/2026-10-05-log-design.md)。

日志。

    LOG_INFO << "listening on port " << port;
    LOG_ERROR << "open " << path << " failed: " << why;

级别不够时**整条语句的参数都不求值**——`LOG_DEBUG << expensive()` 在 info 级别下
不会调用 `expensive()`。这是用宏而不是函数的唯一理由。

### 宏展开成什么

展开成一个最多跑一次的 `for`：

    for (bool once = enabled(level::info); once; once = false) line(...).stream()

所以它是**一条语句**，`if (x) LOG_INFO << "y"; else ...` 里的 else 绑定是对的，
也不会出现「某条分支上漏了收尾」。写出来像流，控制流就是「也许执行一次」，
没有藏 return 之类的东西（开发约定对新增宏的判据）。

和别处一样，短名字可以关掉：包含本头文件之前定义 `WAVVIX_NO_LOG_SHORT_NAMES`，
改用 `WAVVIX_LOG_INFO` 这些带前缀的。

### 输出去哪

**一行都不配就有一个到 stderr 的输出**（第一次写日志时才建，所以 main 之前不跑
任何代码）。不想要就 `clear_sinks()`。想加文件就 `add_file_sink(path)`。
可以同时有多个。

控制台那一路的颜色交给 `term` 判断：重定向到文件、管道给别的程序、或者设了
`NO_COLOR` 时自动变回纯文字，所以日志文件和 CI 日志里不会多出转义序列。

### 几条要知道的

  - **一行日志不分配内存**：消息写进调用方栈上的一块固定缓冲（见 `line_capacity`），
    超长就截断。日志丢几个字比让业务路径去分配内存好。
  - **写出去就不会丢**：文件那一路没有用户态缓冲，每行一次系统调用。进程崩掉时
    已经写出的行都在。
  - **`fatal` 不会 abort**，它只是最高的一级。一句日志顺手把进程杀掉是「意外」，
    要优雅退出用 `culib/os.h` 的停止标志。
  - **sink 是进程全局的**，写入用一把 `std::mutex` 护着。协程里那一行连同拿锁和写入
    整段放到后台线程上（`call_blocking`），所以等慢盘的是后台线程，锁也不会跨着挂起点被拿着；
    协程外直接写。

## [`net.h`](../include/culib/net.h)

监听、连接和收发由一个统一的 `net::socket` 承担（`protocol::tcp` / `protocol::udp`，默认 TCP；构造只记协议，`bind` / `connect` 时才建原生套接字），取代原先的 `tcp_listener` / `tcp_stream` / `udp_socket`。取舍见[统一 socket 设计](superpowers/specs/2026-10-13-unified-socket-design.md)；[网络设计](superpowers/specs/2026-10-08-net-design.md)和 [UDP 设计](superpowers/specs/2026-10-08-udp-design.md) 描述的是被取代的旧接口，保留作历史记录。地址层用法不变：

    wavvix::result<wavvix::net::endpoint> local =
        wavvix::net::endpoint::parse("127.0.0.1", 8080);
    wavvix::result<std::vector<wavvix::net::endpoint>> candidates =
        wavvix::net::resolve("localhost", 8080);

`parse` 只接受数字 IPv4/IPv6；输入主机名用 `resolve`，或直接把主机名交给 `connect(host, port)`。`resolve` 调用系统 DNS 解析器，**会阻塞并分配内存**：协程内自动经 `call_blocking` 执行、不卡工作线程，协程外同步执行，调用方无需再包一层；别在热路径里频繁解析。
`resolve` 保留系统返回的地址顺序，不替调用方猜该连哪一个。`endpoint::to_string()`
把端口一起写出，IPv6 用 `[::1]:8080` 形状。详情见 `examples/net_address.cpp`。

`socket` 一个类型覆盖监听器、连接和报文套接字：`bind` → `listen` → `accept` 是服务端，`connect` 是客户端，`send_to` / `recv_from` 保留报文边界与来源地址。`read` 可能只交付一部分，`read_exactly` / `write` 把完整传输的循环收进库里、共用一份超时预算。完整可运行示例见 [`net_echo.cpp`](../examples/net_echo.cpp) 和 [`net_udp.cpp`](../examples/net_udp.cpp)，边界行为见[网络用法](net.md)。

## [`optional.h`](../include/culib/optional.h)

完整背景见 [设计文档](superpowers/specs/2026-10-04-optional-design.md)。

可能没有值的返回：有值时直接 return 值，没有时 return wavvix::nullopt。

    wavvix::optional<std::string> home = wavvix::os::getenv("HOME");
    if (!home.has_value()) return use_default();
    use(home.value());

### 什么时候用 optional，什么时候用 result

  - 「没有」是**正常结果**  -> optional。环境变量没设置、队列是空的、查表没命中。
  - 「没有」是**失败**      -> result。文件打不开、输入不是数字、权限不足。

判据是「调用方需不需要知道为什么」：需要原因就用 result，`optional` 一个字节都
不带原因。把正常结果包成 result 会迫使调用方处理一个不是错误的错误。

### 和 std::optional 的区别

**没有 operator bool、operator*、operator->**，和 `result` 一致：判断有无和取值
各只保留一种写法。标准库那套 `if (opt)` / `*opt` 更短，但这两个类型会并排出现在
同一个函数里（`fs::exists` 返回 `result`、`os::getenv` 返回 optional），那时
`if (opt)` 紧挨着 `if (text.is_ok())` 就是「同一件事两种写法」。

其余行为和 std::optional 一致：

  - T 可平凡复制时 optional<T> 也可平凡复制（optional<int> 是 8 字节、走寄存器返回）；
  - value() 在没有值时打印诊断并终止，release 同样检查；
  - **移动之后源对象仍然 has_value() == true**，里面是一个被移走的 T。这和 std::optional
    一样，但容易被误读成「移动会清空它」。要变回「没有」就显式 reset()。

## [`os.h`](../include/culib/os.h)

完整背景见 [设计文档](superpowers/specs/2026-10-04-os-design.md)。

操作系统的那些差异：环境变量、进程与机器信息、优雅退出。

这个模块的核心是**优雅退出**——让长期运行的进程被 Ctrl-C 或 kill 时能把手上的活
做完再停。其余几项是顺带的，因为它们同样是「一件事三个平台 API 名字不同」。

### 环境变量

`getenv(name)` 返回 `optional<std::string>`：未设置或名称无效时是 `nullopt`，
已设置的空值是空字符串。`setenv(name, value)` 和 `unsetenv(name)` 返回 `result<void>`；
名称不能为空或包含 `=`、NUL，值不能包含 NUL。拒绝带内嵌 NUL 的输入可以避免
底层 C/Windows API 在 NUL 处截断后误读、修改另一个变量。删除不存在的变量也算成功。

环境变量的并发读写没有同步保护；运行期间要同时读写时，调用方需自行加锁。

### 优雅退出：唯一安全的形状

    int main() {
        if (wavvix::os::install_stop_handler().is_err()) return 1;
        while (!wavvix::os::stop_requested()) {
            do_one_unit_of_work();          // 一个工作单元要么做完要么没开始
        }
        flush_and_close();                  // 一定会执行到
        return 0;
    }

**信号处理函数里几乎什么都不能调。** printf、fclose、malloc、加锁全都不是
async-signal-safe 的——信号可能正好打在 malloc 持锁的时候，此时再 malloc 就是死锁。
POSIX 只允许调 signal-safety(7) 里那张很短的白名单。

所以这里**只提供一个标志**，不提供「注册一个在信号里跑的回调」那种接口：那个接口
没法安全使用，给出来就是发一把枪。真要自定义处理就自己调 std::signal，后果自负。

收到信号后 stop_requested() 永远返回 true，不会自己复位——「该停了」是单向的。
第二次 Ctrl-C 不做特殊处理：想要「再按一次强杀」就自己数次数然后 std::_Exit。

### 平台差异

  - POSIX：SIGINT 和 SIGTERM。**SIGHUP 保持默认行为**，因为按惯例它是「重载配置」
    而不是「停止」（nginx、apache 都这样）。
  - Windows：SetConsoleCtrlHandler，它能收到 Ctrl-C、Ctrl-Break、关窗口、注销、
    关机五种事件。注意**回调在一根独立线程里跑**，不是信号上下文，但接口形状不变。

项目目前以 Windows 10 为最低支持版本；POSIX 平台的具体覆盖见
[路线图的平台验证](superpowers/specs/2026-10-03-roadmap.md)。

### 不做

  - **动态库加载**（dlopen / LoadLibrary）。三个目标场景里没有插件架构，而它是
    本模块平台差异最大的部分（符号修饰、RTLD 标志、DLL 搜索路径）。真要做插件时再加。
  - **崩溃时打印调用栈**。要 dbghelp（Windows，还得分发 PDB）或 backtrace（glibc
    扩展，musl 没有），跨平台一致性最差；而 core dump 加 gdb 能解决同样的问题。
  - 进程创建与管道（fork/exec、CreateProcess）：没有消费者。

## [`result.h`](../include/culib/result.h)

完整背景见 [设计文档](superpowers/specs/2026-10-01-result-design.md)。

可能失败的操作：成功时直接 return 值，失败时 return make_error(...)。

    wavvix::result<int> port = 8080;
    if (port.is_err()) { log(port.error().to_string()); return; }
    use(port.value());

把失败原样传给调用方时自己写 if，别包成宏：

    wavvix::result<int> port = parse_port(text);
    if (port.is_err()) return std::move(port).error();

std::move 不能省：没有它取到的是 error() 的左值重载，错误会被复制一次。
漏写 std::move 会静默多复制一次错误说明字符串，所以一律写成 std::move(x).error()。

多出来的那一行换到的是「每一处 return 都看得见」。库里曾经有个 WAVVIX_TRY 宏，
展开后藏着一个 return，看起来却像变量声明——已删除，理由见 result 设计文档 11.10。
result<void> 默认构造表示成功。
错误类型固定为 error：culib 不为「性能敏感」的接口另开枚举，代价量过，见 CLAUDE.md。
`value_or({})` 无法推导模板参数；空默认值应写出类型，如 `value_or(std::string())`。
value() / error() 取错分支会打印信息并终止程序，release 同样检查。
容器自身不分配内存；值或错误自身可能分配（如 error 的说明字符串）。
不支持引用类型；需要引用语义时使用指针。

## [`stopwatch.h`](../include/culib/stopwatch.h)

完整背景见 [设计文档](superpowers/specs/2026-10-02-stopwatch-and-test-slimming-design.md)。

秒表：测量经过的时间，构造时开始计时。

    #include <culib/stopwatch.h>

    wavvix::stopwatch watch;                          // 开始计时
    do_work();
    std::chrono::nanoseconds cost = watch.elapsed();  // 可以多次读取，不影响计时
    std::cout << wavvix::format_duration(cost);       // 例如 "1.25 ms"
    watch.restart();                                  // 从现在重新开始计时

需要在离开作用域时记录耗时，就和 defer 组合：

    wavvix::stopwatch watch;
    defer { log(wavvix::format_duration(watch.elapsed())); };

使用 std::chrono::steady_clock：单调递增，不受修改系统时间的影响。

## [`string_view.h`](../include/culib/string_view.h)

完整背景见 [设计文档](superpowers/specs/2026-10-04-string_view-design.md)。

只读的字符串视图：一个指针加一个长度，不拥有数据、不分配内存。

    void log(wavvix::string_view message);     // 传 const char*、std::string 都不分配
    log("hello");
    log(text);                                 // text 是 std::string

三件必须知道的事：

1. 没有 c_str()，视图不保证以 '\0' 结尾。要传给 C API 时先 to_string()。
2. 视图不拥有数据，不要让它活过它看的那个字符串：
       string_view bad = std::string("临时的");   // 悬垂，语句结束字符串就没了
3. 越界访问（operator[] / front / back / substr 的 pos > size()）打印诊断并终止，
   release 同样检查。那是编程错误，和 result::value() 取错分支一样 fail fast。

不要把它在 C++17 下别名到 std::string_view：culib 自己按 C++11 编译而使用方可以用更高
标准，别名会让同一个名字在两边是不同类型，以它为参数的公共函数链接不上。
转换构造函数是跨标准定义一致的受约束模板；C++17 的 `std::string_view` 满足约束，
可以隐式传入。若只在 C++17 编译单元给类增加成员，类定义本身会不同，违反 ODR。

## [`strings.h`](../include/culib/strings.h)

完整背景见 [设计文档](superpowers/specs/2026-10-04-strings-design.md)。

操作字符串的自由函数，**不是**字符串类——全库一律用 std::string。

    std::vector<wavvix::string_view> parts = wavvix::split("a,b,c", ',');
    std::string joined = wavvix::join(parts, " / ");
    wavvix::string_view clean = wavvix::trim("  x  ");          // 不分配

两类函数，返回类型就说明了会不会分配：

  - 返回 string_view 的（trim 系、split 的元素）不分配，视图指向输入。
    **调用方必须保证输入活得比结果长**，这是视图的固有约束。
  - 返回 std::string / std::vector 的会分配；需要反复产出时用带 out 参数的重载复用缓冲区。

`to_lower(text, out)` / `to_upper(text, out)` 也可原地转换：`text` 可以引用 `out` 自身或其中一段。

大小写只处理 ASCII，这是明确的范围缩减：Unicode 的大小写映射要内置上千码点的表、
还有 locale 特例（土耳其语的 i），而三个目标场景没一个需要。

starts_with / ends_with / contains / find 在 string_view 上，不在这里重复一遍。

## [`sync.h`](../include/culib/sync.h)

完整背景见 [设计文档](superpowers/specs/2026-10-05-sync-and-timeout-design.md)。

协程感知的同步原语：在协程里等不到的时候**挂起这根协程，工作线程照常跑别的协程**；在普通线程（包括 `main`）里就阻塞那根线程。等待者和唤醒者可以在不同的线程上。

    wavvix::mutex lock;
    wavvix::go([&] {
        wavvix::lock_guard<wavvix::mutex> held(lock);
        update(shared);                    // 这中间可以放心让出
    });

名字和 std:: 对齐（`lock` / `try_lock` / `try_lock_for` / `unlock`、`wait` / `notify_one` / `notify_all`），所以看名字就会用。
`channel<T>` 是容量固定的先进先出队列：`send` 满了等，`receive` 空了等，`close` 以后 `receive` 把剩下的取完就返回 `false`。

### 先问一句：你真的需要锁吗

协程可能在任何一根工作线程上跑，所以两根协程共享的数据和两根线程共享的一样：要么加锁，要么用原子量，
要么干脆不共享——数据归一根协程独占，别的协程经 `channel` 把请求发给它。只有一根协程用的数据不用锁，
它中途换了线程也一样。

用 `wavvix::mutex` 而不是 `std::mutex`，是因为**临界区中间会挂起**：

    wavvix::lock_guard<wavvix::mutex> held(lock);
    auto info = wavvix::fs::file_size(p);  // 协程里走 call_blocking，这里会挂起
    cache[p] = info;                       // 挂起期间别的协程拿不到锁，看不到一半的状态

### 和 std::mutex 怎么选

`wavvix::mutex` 在协程里拿不到时挂起协程，工作线程去跑别的；在普通线程里拿不到时阻塞那根线程，
所以协程和 `main`、普通线程之间共享的锁也用它。`std::mutex` 拿不到时卡住整根工作线程；拿着它也不能
挂起——醒来时协程可能已经换了线程，换了线程再解锁是未定义行为。所以临界区里会挂起（I/O、睡眠、等锁）
就只能用 `wavvix::mutex`，`std::mutex` 只适合很短、绝不挂起的临界区。

### 要 semaphore 时用 channel

限制同时在跑的协程数，用容量为 N 的 `channel`：`send` 占一个名额，满了就挂起；`receive` 还一个。
[`examples/sync.cpp`](../examples/sync.cpp) 第 5 节是完整写法。原来的 `semaphore` 去掉了：channel 能做它做的事，同一件事只留一种写法。

### 这里没有 rwlock

读写锁的实现成本和 API 设计自由度都比上面三个高（写者优先？能不能升级？），
而现在**没有调用方**。按开发约定「先有消费者，再有模块」先不做。
**什么条件下重开**：出现真实的「读远多于写、而且读临界区里会让出」的场合。

## [`term.h`](../include/culib/term.h)

完整背景见 [设计文档](superpowers/specs/2026-10-01-test-term-table-design.md)。

终端彩色输出。

    #include <culib/term.h>
    using namespace wavvix::term;

    std::cout << fg::red("error") << '\n';                     // 红字
    std::cout << bg::yellow("warning") << '\n';                // 黄底
    std::cout << (fg::white | bg::red)(" FAIL ") << '\n';      // 白字红底
    std::cout << fg::green(42) << '\n';                        // 任何能 << 的值

    const style fail = fg::white | bg::red;                    // 样式是值，可以存
    const style levels[] = {style(), fg::cyan, fg::yellow, fail};   // 也可以放数组

### 为什么是 `fg::red("x")` 而不是 `cout << fg::red << "x" << reset`

**`style` 不能直接 `<<`。** 写成 `std::cout << fail` 而忘了括号和文字时是**编译
报错**，所以不可能出现「颜色设上了却没恢复」——那是另一种写法最常见的 bug，而且
它只在某条分支上才暴露（提前 return 跳过了 reset）。

代价是颜色作用的范围必须写在一对括号里。换来的是这个错误类别整个消失。

### 什么时候真的上色

默认 `automatic`：**同时**满足才上色——流是 `std::cout` / `std::cerr` / `std::clog`、
对应的 stdout / stderr 是终端、`NO_COLOR` 没设置或为空、`TERM` 不是 `dumb`
（Windows 不看 `TERM`）。写进文件流或 `std::ostringstream` 时只出纯文字。

`set_color_mode(color_mode::always)` 强制上色（给「管道给 less -R」那种场合），
`never` 强制不上色。

### 开销

`style` 两个字节，`|` 和颜色常量都在编译期算完。`fg::red(x)` 对左值只存引用、对临时
对象移动一次，**不拷贝字符串也不分配**。每次输出多一次函数调用和几次比较，用来判断
要不要上色——相对一次流输出本身可以忽略。

## [`test.h`](../include/culib/test.h)

完整背景见 [设计文档](superpowers/specs/2026-10-02-stopwatch-and-test-slimming-design.md)。

单元测试框架：用法接近 Google Test，只有 TEST 和 EXPECT 两个宏。

    #include <culib/test.h>

    TEST(math, adds_numbers) {              // 组名, 测试名
        EXPECT(1 + 1, ==, 2);               // EXPECT(a, op, b)
    }

    int main(int argc, char* argv[]) {     // 整个测试程序只需要一个 main
        return wavvix::test::run_all(argc, argv);
    }

EXPECT(a, op, b)：
- op 只能是 ==、!=、<、<=、>、>=；a 和 b 各只求值一次。
- 有符号与无符号整数按数学值比较；两边都是 C 字符串（char 指针或数组）时比较内容。
- 失败时打印位置、表达式和两边的值，测试继续执行。EXPECT 返回 bool，
  需要失败就停时写 if (!EXPECT(p, !=, nullptr)) { return; }。
- 操作数里有不在括号内的逗号（如模板参数）时，给操作数再加一层括号。
- 只能在运行测试的线程里调用。

与其他库的 TEST / EXPECT 冲突时，在包含本头文件之前定义 WAVVIX_NO_TEST_SHORT_NAMES，
改用 WAVVIX_TEST / WAVVIX_EXPECT。

EXPECT 的实现在 culib/detail/expect.h，使用时不需要阅读。

## [`time.h`](../include/culib/time.h)

完整背景见 [设计文档](superpowers/specs/2026-10-04-time-design.md)。

时刻：取当前时间、拆成年月日、格式化、解析。

时长在 <culib/stopwatch.h>（stopwatch 和 format_duration），那边用单调时钟。
这里是墙上时钟，会被改系统时间影响——所以**别拿它测耗时**。

    wavvix::timestamp now = wavvix::timestamp::now();
    std::string text = wavvix::format_time(now);           // "2026-10-04T11:12:51Z"

### 单位写在类型里

秒和毫秒都是 int64_t，混起来编译通过、静默错 1000 倍。所以构造一律写明单位：

    timestamp::from_unix_seconds(mtime_seconds)
    timestamp::from_unix_millis(event)

函数名让输入单位在调用点可见。`from_unix_seconds` 的输入必须能换算为有符号 64 位
Unix 毫秒；超出 `INT64_MAX / 1000` 秒的正负范围会打印诊断并终止。`unix_seconds()`
对负毫秒向下取整，包括 `INT64_MIN`。

`parse_time` 接受 `2026-10-04T11:12:51Z`、带毫秒和 `+08:00` 这类偏移的写法；
日期与时间之间可用 `T`、`t` 或空格，UTC 标记可用 `Z` 或 `z`。超过三位的小数截到
毫秒，不四舍五入。省略秒、省略偏移和 `+0800` 形式的偏移都不接受，输入必须完整消费。

### 只有一个 now()

系统还有一个便宜十倍的粗时钟（std::time 走的那个，4.9 ns 对 now() 的 49 ns），
**但没有包装成第二个函数**：省下的 44 ns 要每秒调 22 万次才值 1% 的一个核，
三个目标场景里没有这样的地方，而多一个函数就等于每个调用点都要先想一下
「这里算不算性能敏感」。真需要的话退路是一行标准 C++：

    timestamp::from_unix_seconds(std::time(0))    // 分辨率只有内核 tick

### 不做

  - **strftime 风格的格式串**。%a / %b 的输出取决于 locale，而本库在
    lexical_cast 上专门做过「不受 locale 影响」；而且走 strftime 要 133 ns，
    固定格式自己写只要 27 ns。要自定义格式就拿 utc_parts / local_parts 自己拼。
  - **自带时区数据**。本地时区只问系统（localtime_r / localtime_s），所以不能
    「把这个时刻换算到任意时区」。要那个就得引第三方库。
  - **闰秒**。Unix 时间戳本身不表示闰秒，全模块假设 86400 秒 = 1 天。
  - **日历算术**（加一个月、月末、工作日）。没有消费者，而「加一个月落到 2 月
    30 日怎么办」这类问题的设计自由度极高。
  - **时区名字和缩写**。CST 同时是北京时间和美国中部时间，输出它就是造歧义。
