# 公共 API 的用法与设计说明

这里整理了公共头文件里原先过长的用法示例和设计说明。函数本身的参数约束与容易误用的行为仍写在声明旁；完整设计与取舍见各节链接。

## [`bytes.h`](../include/culib/bytes.h)

完整背景见 [设计文档](superpowers/specs/2026-10-05-bytes-design.md)。

字节缓冲：一段可增长的字节 + 一个读游标 + 一个写游标，外加**显式字节序**的整数读写。

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

**不用「返回 bool + 出参」**，虽然那是本库别处的习惯（`fs::read_file`）。那种写法
在这里有个真陷阱：把调用和读出参写进同一个表达式时，求值顺序是未定义的——

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

公开 `coroutine` 是有栈协程的线程式句柄。先初始化当前操作系统线程的 vCPU，创建任务即提交，`join()` 等指定任务结束：

    #include <culib/coroutine.h>
    #include <culib/defer.h>

    wavvix::coroutine::init();
    defer { wavvix::coroutine::shutdown(); };
    wavvix::coroutine worker([&](int count) {
        for (int i = 0; i < count; ++i) {
            produce(i);
            wavvix::coroutine::yield();
        }
    }, 3);
    if (!worker.joinable()) return 1;
    worker.join();

也可用 `scheduler_scope` 管理 vCPU。`coroutine` 可移动；仍可 `join()` 的句柄析构会
`std::terminate()`，所以离开作用域前要 `join()` 或 `detach()`。`detach()` 只放弃句柄，
任务仍需由同一 vCPU 的 `join()`、`run()`、`sleep_for()` 或作用域收尾推进。
构造后若 `joinable()` 为 `false`，说明协程栈分配失败，任务未提交。
构造函数与 `std::thread` 一样接受函数及其参数；参数先按值保存，运行时移动到函数中。
需要自定义栈大小时用 `coroutine::with_stack(bytes, function, args...)`。
协程只在合作式调度点切换，CPU 密集循环不会自动抢占主线程。

`inside()` 判断当前代码是否运行在协程中。协程里执行真正阻塞系统线程的函数会连带阻塞
同一 vCPU 的其他协程。库提供的 TCP、同步、睡眠接口会在等待时让出协程。
底层手动换栈原语在 `detail::fiber`，不是公开的任务接口。实现背景见
[协程设计记录](superpowers/specs/2026-10-04-coroutine-design.md)；当前用法见
[协程调度器](scheduler.md)。

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
- 同一行只能写一个 defer。
- 零开销：不分配内存，不使用虚函数或 std::function，优化后与手写的清理代码等价。

如果与其他库中名为 defer 的标识符冲突（如 Asio 的 asio::defer），
在包含本头文件之前定义 WAVVIX_NO_DEFER_KEYWORD，改用 WAVVIX_DEFER { ... };

## [`flag.h`](../include/culib/flag.h)

完整背景见 [设计文档](superpowers/specs/2026-10-04-flag-design.md)。

命令行参数。选项是全局对象，定义在哪里就能在哪里读，不用从 main 一路传下去。

    namespace flag = wavvix::flag;

    flag::int64  port("port", 8080, "监听端口", {"p"});
    flag::string host("host", "0.0.0.0", "监听地址");
    flag::boolean verbose("verbose", false, "打印详细日志", {"v"});
    flag::string input("input", flag::required, "输入文件");

    int main(int argc, char* argv[]) {
        flag::init(argc, argv, "1.2.3");          // 失败时打印用法并退出进程
        listen(host, port);                        // 像普通变量一样用
        for (wavvix::string_view path : flag::args()) { ... }   // 位置参数
    }

### 陷阱：别用 auto 拷贝

    auto v = verbose;          // 拷的是 flag::boolean（而且编译失败，不可拷贝）
    auto v = verbose.get();    // 这才是 bool

隐式转换是为了「像普通变量一样读取和输出」而保留的，代价就是这一条。选项不可拷贝，
所以第一种写法是编译错误而不是静默的怪行为。

### 接受的写法

  --port=8080   --port 8080   -p 8080   --verbose   --verbose=true   --   -

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

选项的 name / help / 别名字符串必须是静态存续的（字面量就行），库不拷贝它们。

全局选项对象在 main 之前登记，这是「无启动开销」的第二个例外（第一个是 TEST）。

## [`fs.h`](../include/culib/fs.h)

完整背景见 [设计文档](superpowers/specs/2026-10-04-fs-design.md)。

文件系统。C++11 没有 std::filesystem，这是缺失感最强的一块。

    wavvix::result<std::string> text = wavvix::fs::read_file("config.toml");
    if (text.is_err()) return std::move(text).error();

    for (const std::string& name : wavvix::fs::list_directory("/tmp").value_or({})) { ... }

约定：

- 路径一律是 UTF-8 的 std::string / string_view，没有专门的 path 类型。代价是路径
  和普通字符串混用时编译器发现不了，好处是每个接口少一层转换。
- 输入接受 '/' 和 '\\' 两种分隔符。**join 只为自己插入的那一个分隔符用平台原生字符，
  不改写输入里已有的**：Windows 上 join("/a", "b") 是 "/a\\b" 而不是 "\\a\\b"。混用在
  Windows 上是合法的（"C:/foo\\bar" 能用）；要全部统一成原生分隔符就用 normalize，
  它是按段重建路径的。这条是 CI 第一次在 Windows 上跑时，被一个写错的测试暴露出来的。
- 整文件读写一律二进制，不做换行转换：读到的就是磁盘上的字节。
- **符号链接默认跟随**（用 stat 而不是 lstat）：exists / is_file / file_size 问的是
  「那里有个能打开的文件吗」。唯一的例外是 remove_directory(recursive::yes)，它**不**
  跟进符号链接——跟进去会删掉链接指向的别处内容，那个代价太大。
- 目标存储包含 HDD 和网络挂载。一次 stat 在本地缓存热时 ~475 ns，在冷的 HDD 上
  5–10 ms，在 NFS 上 0.2–1 ms。**所以这里每个函数都只做它名字说的那件事**，
  不替调用方多问一次磁盘，见 CLAUDE.md「性能与占用」。

## [`fs_watcher.h`](../include/culib/fs_watcher.h)

`fs::poller` 已可用：注册目录时拍一次快照，以后按给定间隔遍历并比较文件身份、大小和修改时间。它适合 NFS / SMB 等网络挂载。`wait(timeout, out)` 在协程里等待扫描间隔时让出；返回的事件包含被监视根目录、相对路径和变化类型。视图在下一次 `wait()` 前有效。完整用法见 [`fs_poller.cpp`](../examples/fs_poller.cpp)。

扫描本身是同步文件系统调用；在慢磁盘或网络挂载上，大目录的扫描会阻塞当前 vCPU。选较长的间隔、缩小监视范围，或把这个 poller 放到独立的工作线程。两次扫描之间创建又删除的文件可能看不到。内核通知版 `fs::watcher` 仍在开发，设计见[文件监听](superpowers/specs/2026-10-08-fs-watcher-design.md)。

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

不受 locale 影响：小数点一律是 '.'。整数完全自己解析；浮点的语法自己严格校验，换算
交给 strtod（它是正确舍入的），但先把 '.' 换成当前 locale 的小数点——不调 setlocale，
那是进程级的、会影响别的线程。

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
  - **sink 是进程全局的**，所以写的时候用的是 `std::mutex` 而不是
    `wavvix::mutex`——多个 vCPU 会同时写同一个 sink，而协程锁只在一个 vCPU 内部
    有效。那一小段会真的挡住 vCPU，但它只是「格式化好的一行交给系统调用」。

## [`net.h`](../include/culib/net.h)

完整范围见[网络设计](superpowers/specs/2026-10-08-net-design.md)和[UDP 设计](superpowers/specs/2026-10-08-udp-design.md)。地址层用法：

    wavvix::result<wavvix::net::endpoint> local =
        wavvix::net::endpoint::parse("127.0.0.1", 8080);
    wavvix::result<std::vector<wavvix::net::endpoint>> candidates =
        wavvix::net::resolve("localhost", 8080);

`parse` 只接受数字 IPv4/IPv6；输入主机名请用 `resolve`。后者调用系统 DNS 解析器，
**会阻塞当前线程并分配内存**，所以服务器宜在运行 vCPU 前解析，别在热路径里悄悄调用。
`resolve` 保留系统返回的地址顺序，不替调用方猜该连哪一个。`endpoint::to_string()`
把端口一起写出，IPv6 用 `[::1]:8080` 形状。详情见 `examples/net_address.cpp`。

`tcp_listener` / `tcp_stream` 提供连接式收发；`udp_socket` 的 `send_to` / `recv_from` 保留报文边界与来源地址。完整可运行示例见 [`net_echo.cpp`](../examples/net_echo.cpp) 和 [`net_udp.cpp`](../examples/net_udp.cpp)，边界行为见[网络用法](net.md)。

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
同一个函数里（`fs::read_file` 返回 result、`os::getenv` 返回 optional），那时
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
    而不是「停止」（nginx、apache 都这样），而热加载要等第三程的 fs::watcher。
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
错误类型只能移动时漏写是编译错误，可复制时（默认的 error 就是）却编译通过且零警告，
只是静默多复制一次说明字符串——所以一律写成 std::move(x).error()，不留第二种写法。

多出来的那一行换到的是「每一处 return 都看得见」。库里曾经有个 WAVVIX_TRY 宏，
展开后藏着一个 return，看起来却像变量声明——已删除，理由见 result 设计文档 11.10。
result<void> 默认构造表示成功。
错误类型一律用默认的 error：culib 不为「性能敏感」的接口另开枚举，代价量过，见 CLAUDE.md。
E 仍是模板参数（约束只有「非数组对象类型、非 cv、析构不抛、和 T 不可互转」，并不要求是枚举），
自己的代码真需要时可以传任意小而平凡可复制的类型，但跨层传给返回 result<T> 的函数要自己转换。
value() / error() 取错分支会打印信息并终止程序，release 同样检查。
容器自身不分配内存；值或错误自身可能分配（如 error 的说明字符串）。
不支持引用类型；需要引用语义时使用指针。

## [`scheduler.h`](../include/culib/scheduler.h)

完整背景见 [设计文档](superpowers/specs/2026-10-04-scheduler-design.md)。

调度器：协程是这个库的并发模型，这个头文件是它的主入口。

    wavvix::spawn([] {
        wavvix::sleep_for(std::chrono::milliseconds(10));   // 睡的是协程，不是线程
        do_work();
    });
    wavvix::spawn([] { other_work(); });
    wavvix::run();                                          // 跑到全部结束

`spawn()` 用于不需要句柄的任务。需要等待指定任务时，用上面的 `coroutine` 和 `join()`。

### vCPU 模型

**一根操作系统线程 = 一个 vCPU**，上面跑一个调度器和它的一堆协程。可用
`coroutine::init()` / `shutdown()` 或 `scheduler_scope` 管理其生命周期；不用显式初始化时，
首次创建任务会按需建立 vCPU。想用多核就在多根系统线程上各建一个 vCPU。

协程**绑在它所在的 vCPU 上**，不会被偷到别的 vCPU 去（为什么不偷：换线程会让
thread_local 和异常状态在协程脚下变掉，见 coroutine 设计文档 3.12，那条库修不了）。

### 不 spawn 就什么都不初始化

不显式初始化时，第一次创建任务才建立当前线程的 vCPU。
一个不用协程的程序（比如 CLI）一分钱不付，「不在 main 之前执行任何代码」也没破。

### 当前边界

事件引擎已有 io_uring、epoll、kqueue、IOCP 四个后端；Linux 上 io_uring 不可用时自动退回 epoll。
[sync.h](../include/culib/sync.h) 提供协程感知的同步原语，
[net.h](../include/culib/net.h) 提供 TCP 连接、监听和收发。调度策略是轮转，不承诺抢占或优先级。
分期和已完成范围见 [调度器设计](superpowers/specs/2026-10-04-scheduler-design.md)
及 [超时与同步原语](superpowers/specs/2026-10-05-sync-and-timeout-design.md)。

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
标准，别名会让同一个名字在两边是不同类型，以它为参数的公共函数链接不上。C++17 下额外提供
一个从 std::string_view 的转换构造函数，那是纯增量。

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

大小写只处理 ASCII，这是明确的范围缩减：Unicode 的大小写映射要内置上千码点的表、
还有 locale 特例（土耳其语的 i），而三个目标场景没一个需要。

starts_with / ends_with / contains / find 在 string_view 上，不在这里重复一遍。

## [`sync.h`](../include/culib/sync.h)

完整背景见 [设计文档](superpowers/specs/2026-10-05-sync-and-timeout-design.md)。

协程感知的同步原语：等不到的时候**挂起这根协程，而不是卡住整个 vCPU**。

    wavvix::mutex lock;
    wavvix::spawn([&] {
        wavvix::lock_guard<wavvix::mutex> held(lock);
        update(shared);                    // 这中间可以放心让出
    });

名字和 std:: 对齐（`lock` / `try_lock` / `try_lock_for` / `unlock`、
`acquire` / `release`、`wait` / `notify_one` / `notify_all`），所以看名字就会用。

### 先问一句：你真的需要锁吗

**同一个 vCPU 上的协程是协作式调度的，不会被抢占。** 所以一段**不让出**的临界区
本来就是原子的——中间没有任何别的协程能插进来，加锁只是多几条指令。

    ++counter;                             // 不需要锁
    map[key] = value;                      // 不需要锁

需要锁的是**临界区中间会让出**的情形，那时别的协程真的会跑起来并看到中间状态：

    auto text = read_file(path);           // 这里会让出
    cache[path] = text;                    // 回来时 cache 可能已经被别人动过

判据就一句：**这段代码从读到写之间，有没有可能让出。** 有就要锁，没有就别加。

### 跨线程不要用这些

这里的每个类型都**只在自己那个 vCPU 内部有效**：它们一个原子操作都没有，因为
协程不跨 vCPU 迁移，同一个 vCPU 上不存在并发。要在**线程**之间同步，用
`std::mutex` 那一套——但注意在协程里调 `std::mutex::lock()` 阻塞住的是整根 vCPU，
连带卡死它上面所有协程。

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

    timestamp::from_unix_seconds(mtime)    // fs::last_write_time 给的就是这个
    timestamp::from_unix_millis(event)

笔误是编译错误，不是一个 58728 年的时间戳。

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
