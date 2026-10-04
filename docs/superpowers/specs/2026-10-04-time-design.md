# time 设计

- 日期：2026-10-04
- 状态：已实现
- 取代 [预期接口](2026-10-03-module-interfaces.md) 第 8 节。第二程第 7 项。

## 1. 范围

`stopwatch` 和 `format_duration` 已经在 `culib/stopwatch.h` 里了，那两个管的是**时长**。本模块管的是**时刻**：墙上时钟的取值、拆成年月日、格式化、解析。

**做**：

- 当前时刻（秒和毫秒两种精度，成本差一个数量级，见 3.3）
- 格式化成 ISO 8601，UTC 和本地时区各一套
- 从 ISO 8601 解析回来
- 写进调用方缓冲区的重载（`log` 每行都要调一次）

**不做**：

- **strftime 风格的格式串**。见 3.5。
- **自带 tzdata**。只用系统的 `localtime_r` / `localtime_s`，所以本地时区的正确性等于系统的正确性。好处是零数据、零更新；代价是不能做「把这个时刻换算到任意时区」。真要那个就得引第三方库，违反零依赖。
- **闰秒**。Unix 时间戳本身就不表示闰秒，`86400 秒 = 1 天`这条假设贯穿全模块。
- **日历算术**（`add_months`、`end_of_month`、工作日计算）。三个场景里没有消费者，而设计自由度极高（加一个月落到 2 月 30 日怎么办？），按判据必须先有消费者。
- **时区名字和缩写**。`CST` 同时是北京时间和美国中部时间，输出它就是在制造歧义。只输出数字偏移。
- **`std::chrono` 的 `time_point` 作为接口类型**。`system_clock` 的纪元在 C++11 里**未指定**（C++20 才钉成 Unix 纪元），所以拿它当跨模块、跨平台的交换类型不安全。内部用它取值没问题。

## 2. 接口

**单位安全那一处要你定，见 3.4。** 下面先按「写法 B」列。

| 分类 | 签名 | 说明 |
| --- | --- | --- |
| 取值 | `timestamp timestamp::now() noexcept` | 毫秒精度，实测 **49 ns** |
| 取值 | `timestamp timestamp::now_coarse() noexcept` | 秒精度，实测 **4.9 ns**，便宜一个数量级，见 3.3 |
| 构造 | `timestamp timestamp::from_unix_seconds(int64_t) noexcept` | |
| 构造 | `timestamp timestamp::from_unix_millis(int64_t) noexcept` | |
| 取出 | `int64_t timestamp::unix_seconds() const noexcept` | 向下取整，1970 以前也对 |
| 取出 | `int64_t timestamp::unix_millis() const noexcept` | |
| 比较 | `== != < <= > >=` | 时刻之间可比大小 |
| 格式化 | `std::string format_time(timestamp, precision = precision::seconds)` | `2026-10-04T11:12:51Z` |
| 格式化 | `std::size_t format_time(timestamp, precision, char*, std::size_t) noexcept` | 写进调用方缓冲区，不分配；容量不足返回 0。缓冲区给 40 字节够任何输入 |
| 格式化 | `std::size_t format_time(const date_time&, precision, char*, std::size_t) noexcept` | 格式化一个已经拆开的 `date_time`，用它自己的偏移。**这是本地时间的热路径**，见 3.7 |
| 格式化 | `result<std::string> format_local_time(timestamp, precision = precision::seconds)` | `2026-10-04T19:12:51+08:00`，**带偏移**，见 3.6。要查系统时区所以可能失败 |
| 解析 | `result<timestamp> parse_time(string_view)` | 只认 ISO 8601，见 3.8 |
| 拆开 | `date_time utc_parts(timestamp) noexcept` | 年月日时分秒毫秒，给要自己拼格式的调用方 |
| 拆开 | `result<date_time> local_parts(timestamp)` | 同上，本地时区，另带 `offset_minutes`。**范围限 1970..3000**，见 3.10 |

```cpp
enum class precision { seconds, millis };

struct date_time {
    int year;                 // 公历年，如 2026
    unsigned month;           // 1..12
    unsigned day;             // 1..31
    unsigned hour;            // 0..23
    unsigned minute;          // 0..59
    unsigned second;          // 0..59，不处理闰秒
    unsigned millisecond;     // 0..999
    int offset_minutes;       // 相对 UTC 的分钟数，UTC 为 0
};
```

## 3. 关键决定

### 3.1 UTC 自己算，不调 gmtime_r

实测（Linux / GCC 14.2 `-O2`，30 万次取平均）：

| 做法 | 耗时 | |
| --- | --- | --- |
| `gmtime_r` + `snprintf` | 311.2 ns | |
| `gmtime_r` + `strftime` | 133.4 ns | |
| 自己算 + 自己写数字 + 返回 `std::string` | **40.2 ns** | |
| 自己算 + 自己写数字，写进调用方缓冲区 | **27.2 ns** | 比 strftime 快 **4.9 倍** |

只是拆年月日的话：`gmtime_r` 49.8 ns，`localtime_r` 66.0 ns，自己算 **13.6 ns**。

算法用 Howard Hinnant 公开的 `civil_from_days` / `days_from_civil`（20 行整数运算，无查表、无分支预测压力）。**自己算同时买到跨平台一致**：三个平台逐条指令相同，不受 libc 实现差异影响。

正确性不是推导出来的，是比出来的：1970–2100 之间每 997 秒取一个点，**4,114,790 个时刻和 `gmtime_r` + `strftime` 的输出逐字节比对，不一致 0 个**；1970 以前的负时间戳（`-1`、`-86400`、`-86401`、`-2208988800`、`-1000000000`）全部一致；`days_from_civil` 往返不一致 0 个。

### 3.2 本地时区的偏移量不用 tm_gmtoff

本地时区只能问系统（不自带 tzdata），但**取偏移量这一步有个坑**：`tm_gmtoff` 是 glibc / BSD 扩展，**MSVC 的 `std::tm` 没有这个字段**。

不用它，改成**把同一个 `time_t` 分别用 `localtime` 和 `gmtime` 拆开，两个日历时间相减**，减法用我们自己的 `days_from_civil`。这样三个平台一套代码，只依赖标准里有的字段。

实测对得上（和 `tm_gmtoff` 逐个比，六个时区 × 五个时刻，全部一致）：

| 时区 | 挑它的理由 | 结果 |
| --- | --- | --- |
| `Asia/Shanghai` | 固定 +08:00 | 一致 |
| `America/New_York` | 夏令时，−04:00 / −05:00 | 一致 |
| `Europe/London` | 夏令时跨越 0 偏移 | 一致 |
| `Australia/Lord_Howe` | **半小时的夏令时跳变**（+10:30 / +11:00） | 一致 |
| `Asia/Kathmandu` | **+05:45 这种非整小时**，且 1986 年前是 +05:30 | 一致 |
| `UTC` | 退化情形 | 一致 |

Windows 侧还要确认一件事：`localtime_s` 的**参数顺序和 POSIX 的 `localtime_r` 相反**（`(tm*, time_t*)` 对 `(time_t*, tm*)`），写错了编译器不一定拦得住。

### 3.3 秒和毫秒差一个数量级，两个都给

| 时钟 | 耗时 |
| --- | --- |
| `std::time(0)` | **4.9 ns** |
| `clock_gettime(CLOCK_REALTIME_COARSE)` | 5.3 ns |
| `clock_gettime(CLOCK_REALTIME)` | 27.5 ns |
| `std::chrono::system_clock::now()` | 49.4 ns |

差别有名字：**粗时钟读的是内核按 tick 更新的一页内存，精确时钟要 `rdtsc` 再换算。** 所以 `std::time(0)` 不是「少除一次 1000」，它是另一个时钟源，分辨率只有内核 tick（通常 1–4 ms）。

两个都暴露，因为**这个代价只有调用方知道**（开发约定「不替调用方做只有它才知道代价的决定」）：每行日志打一次时间戳的，10 倍差别是真的；一个 CLI 跑完打一行的，无所谓。文档里把分辨率的代价写明。

### 3.4 单位安全：timestamp 值类型（已定）

`format_time(1791112371789)` 和 `format_time(1791112371)` 都是 `int64_t`，混起来**编译通过、静默错 1000 倍**。原型里两种写法都实现了，调用点并排跑过（见第 4 节输出）：

- **写法 A**：裸 `int64_t`，单位写在函数名里（`format_time` / `format_time_millis`）。笔误的实际后果：`format_time(event_millis)` 打印 `58728-02-04T06:23:09Z`，一声不响。
- **写法 B**：`timestamp` 值类型，单位写在类型里。同一个笔误是编译错误：

  ```
  error: could not convert 'event_millis' from 'const long int' to 'timestamp'
  ```

**定了 B**，理由：「难以误用」排在「少一个概念」前面，而 `timestamp` 是 8 字节平凡可复制、全内联，按值传递和 `int64_t` 生成同样的代码，**零开销这条硬约束没破**。

B 的代价有两条，得一起看：

1. 多一个概念，以及 `from_unix_seconds` 这种略啰嗦的构造写法。
2. **`fs::last_write_time` 现在返回 `result<std::int64_t>`（秒）。** 选 B 就该一起改成 `result<timestamp>`，否则库内两种表示并存，反而更容易错。改动是三处，现在没有外部使用方，成本很低——但这是个连带决定，所以摆在这里。

### 3.5 不做 strftime 风格的格式串

`format_time(ts, "%Y年%m月%d日")` 看着很香，三条理由不做：

1. **locale 依赖**。`%a` / `%b` / `%c` 的输出取决于 `LC_TIME`，而本库在 `lexical_cast` 上专门做过「不受 locale 影响」（那里为此没调 `setlocale`，改成换小数点）。一个 locale 敏感的格式化函数和那条自相矛盾。
2. **贵 5 倍**。走 `strftime` 就是 133 ns，而固定格式自己写是 27 ns。
3. **没有消费者，设计自由度却很高**（支持哪些 specifier？宽度修饰？`%N` 这种非标准的毫秒？）。按「实现成本 × 设计自由度」判据，自由度高就必须先有消费者。

替代办法：要自定义格式的调用方拿 `utc_parts` / `local_parts` 自己拼——那是 `date_time` 存在的理由。

**重开条件**：出现真的需要运行期可配格式串的场景（例如配置文件里让用户自己写日志时间格式）。那时再定支持哪个子集。

### 3.6 本地时间必须带偏移

输出 `2026-10-04T19:12:51` 而不说明偏移，三个月后读日志的人没法知道那是哪个时刻——尤其是**跨夏令时的那两个小时会重复出现**。所以本地时间一律带 `+08:00` 形式的偏移，UTC 一律带 `Z`。

这也让 `parse_time` 能原样读回来（3.8）。

### 3.7 缓存不内置，交给 log

spdlog 的做法是在 `pattern_formatter` 里缓存 `std::tm`，**只在整秒变化时重算**（`pattern_formatter-inl.h:995`，`if (secs != last_log_secs_)`）。那是对的，但那是 `log` 的活：它知道自己的调用频率和要不要毫秒。

本模块的格式化已经降到 27 ns，缓存与否交给调用方。内置缓存要么带状态（破坏「无启动开销」），要么带锁（让单线程使用者付钱）。

### 3.8 parse_time 只认自己输出的形状

和 `lexical_cast` 同一条纪律：**自己的输出必须能被自己读回来**。接受的形状：

```
2026-10-04T11:12:51Z
2026-10-04T11:12:51.789Z
2026-10-04T19:12:51+08:00
2026-10-04T19:12:51.789+08:00
```

不接受：空格代替 `T`（`"2026-10-04 11:12:51"`）、省略秒、两位年份、`%j` 这类序数日期、不带偏移的本地时间（那是有歧义的，见 3.6）。严格到底，和 `lexical_cast` 的「整个输入必须被消费」一致。

**待定**：要不要接受空格代替 `T`。人手写的配置文件里空格更常见（coost 的 `now.str()` 默认就是 `"%Y-%m-%d %H:%M:%S"`），而 ISO 8601 的 RFC 3339 明确允许用其他分隔符。倾向接受，因为输入宽容和输出严格不矛盾。

### 3.9 stopwatch 不并进来

`stopwatch.h` 现在是**纯头文件、零平台代码**。`time.h` 要带一个 `.cpp`（日历换算 + 系统时区）。并进去会让只想测耗时的使用方连带编译日历代码。

分工也清楚：`stopwatch` / `format_duration` 管时长（单调时钟），`time` 管时刻（墙上时钟）。两个不该混的东西混在一个头文件里，名字反而说不清。

### 3.10 本地时间的范围三个平台一样

MSVC 的 `localtime_s` 只接受 `[0, 32535215999]`（1970-01-01 .. 3000-12-31），`localtime_r` 本来能走得更远。**第一版把范围判断写在各自的平台文件里，于是同一个调用在 Linux 上成功、在 Windows 上失败**——那正是 README 承诺过的「三平台行为一致」被破掉。

改成把范围判断统一放在 `local_parts` 里，对三个平台一视同仁，取 Windows 那个更窄的界。代价是 Linux 上损失了本来能用的区间，换来的是「同一个调用在哪儿都一样」。

**只限制本地时间**：UTC 是自己算的，任何时刻都能格式化（1900 年、9999 年都测了）。1970 以前的本地偏移本来也靠不住，那时候很多地区还没进 tzdata 的记录。

这条有测试钉着（`local_time_has_the_same_range_on_every_platform`），不是只写在文档里。

## 4. 实现验证

环境：Linux / GCC 14.2 与 clang 19 × debug/release，`-Wall -Wextra -Wpedantic -Werror`，零警告；Windows 侧本机 MinGW-w64 + wine；**CI 四种编译器 × debug/release 共 10 个格子一次过**。

### 4.1 构建与测试

**164 个测试通过，本模块新增 15 个**：单位与向下取整、比较、UTC 格式化（含 1900 / 9999 / 闰日 / 百年不闰）、毫秒补零到三位、缓冲区重载与容量不足返回 0、拆字段、本地时间带偏移且自洽、本地时间的跨平台范围、解析自己输出的每种形状、往返、11 种畸形输入、12 种越界或不存在的日期、以及和 libc 的大范围比对。

`fs::last_write_time` 的返回类型从 `result<std::int64_t>` 改成 `result<timestamp>`（3.4 的连带决定），改动四处：头文件、两个平台实现、一条测试。

### 4.2 手写日历必须和 libc 对得上

这是本模块最要紧的一条测试（`agrees_with_libc_across_130_years`）：1970–2100 之间步长 100003 秒（和 86400 互素，所以会扫过各种时分秒组合）取 41023 个时刻，和 `gmtime_r` + `strftime` 的输出**逐字节比对，不一致 0 个**。

它留在单元测试里而不是只在设计阶段跑一次，因为手写的日历换算一旦哪天被改坏，没有这条测试就没人知道。代价是 22 ms，是整个测试集里最慢的一条。

设计阶段还跑过更密的一遍：步长 997 秒、411 万个时刻，同样 0 不一致；`days_from_civil` 往返 0 不一致。

### 4.3 时区

本地时间那几条测试**不假设跑在哪个时区**，断言的是自洽：本地时间带着自己的偏移，解析回来必须是同一个时刻。在八个时区下各跑一遍都是 15/15：

```
UTC / Asia/Shanghai / America/New_York / Australia/Lord_Howe
Asia/Kathmandu / Pacific/Chatham / America/St_Johns / Pacific/Kiritimati
```

挑的都是难的：Lord Howe 的夏令时只跳半小时、Chatham 是 +12:45、St_Johns 是 −03:30、Kathmandu 是 +05:45 且 1986 年前还是 +05:30。

偏移量的算法（本地日历秒减 Unix 秒）另外单独验证过：七个时区 × 113 万个时刻和 `tm_gmtoff` 比对，**796 万次零不一致**。

### 4.4 实际输出

`examples/time.cpp`，`TZ=Asia/Shanghai`：

```
现在：
  UTC          2026-10-04T11:25:59Z
  UTC 带毫秒   2026-10-04T11:25:59.663Z
  本地时区     2026-10-04T19:25:59.663+08:00   （带偏移，不带就有歧义）

两个精度差一个数量级（不是「少除一次 1000」，是两个时钟源）：
  now()          65.4 ns   毫秒精度，走 CLOCK_REALTIME
  now_coarse()    8.6 ns   秒精度，读内核按 tick 更新的一页内存

解析（输出严格用 T，输入也认空格）：
  2026-10-04T11:12:51Z         → 2026-10-04T11:12:51.000Z
  2026-10-04T11:12:51.789Z     → 2026-10-04T11:12:51.789Z
  2026-10-04 11:12:51Z         → 2026-10-04T11:12:51.000Z
  2026-10-04T19:12:51+08:00    → 2026-10-04T11:12:51.000Z
  2026-10-04T16:57:51+05:45    → 2026-10-04T11:12:51.000Z
  2026-10-04T11:12:51          → invalid_argument: 不是合法的 ISO 8601 时刻：
                                 "2026-10-04T11:12:51"（要 2026-10-04T11:12:51Z
                                 这种形状，必须带 Z 或 ±HH:MM）
  2026-02-30T11:12:51Z         → out_of_range: 这一天不存在："2026-02-30T11:12:51Z"
  2026-10-04T11:12:60Z         → out_of_range: 日期或时间字段越界："2026-10-04T11:12:60Z"

和 fs 搭起来用：
  可执行文件的修改时间 2026-10-04T11:25:53Z
  也就是 6.00 s 以前   （last_write_time 返回 timestamp，不是一个光秃秃的 int64）
```

示例里那两个 ns 数比微基准（49 / 4.9）高一点，因为跨翻译单元调用没内联、循环本身也有开销。倍数一致（7.6 倍对 10 倍）。

### 4.5 「2 月 30 日」不靠查表

每个字段都在范围内但日期不存在（`2026-02-30`、`2026-02-29`、`2100-02-29`、`2026-04-31`）这类输入，判法是**换算成天数再换回来，不一致就说明这一天不存在**。这样不必维护「每月几天 + 闰年规则」那张表——闰年规则已经在 `days_from_civil` 里了，再写一遍就是两份会不一致的真相。

### 4.6 没覆盖

- ~~MSVC 和 macOS 要等 CI。~~ **CI 10/10 一次过**，所以 `time_win.cpp` 的 `localtime_s`（参数顺序和 POSIX 相反那个）在真 MSVC 上成立，macOS 的 BSD `localtime_r` 也成立。CI runner 的时区是 UTC，所以**带偏移的那条路径在 CI 上走的是退化情形**（偏移为 0，输出 `Z`）；非零偏移只在本机的八个时区下验证过。
- **32 位 time_t 的那条分支没触发过**。`time_posix.cpp` 里有一个 `sizeof(std::time_t) < sizeof(std::int64_t)` 的判断，本机是 64 位，那一行从来没真的执行。
- **闰秒附近的行为没测**，因为 Unix 时间戳本身不表示闰秒——`23:59:60` 这种输入直接被拒（有测试），但「系统时钟正在跨闰秒」这个状态没法构造。
- **`now_coarse()` 的分辨率没量**。只验证了它和 `now()` 相差不超过 2 秒，没测它实际多久跳一次（那取决于内核的 `CONFIG_HZ`）。
- **改系统时间时的行为没测**。墙上时钟会跳，所以 `timestamp` 的差值可能为负——头文件里写了「别拿它测耗时」，但没有机制拦住。

## 5. 读过的东西

| 项目 | 看到了什么 | 我们哪里不一样 |
| --- | --- | --- |
| spdlog `pattern_formatter-inl.h` | **按整秒缓存 `std::tm`**（`if (secs != last_log_secs_) cached_tm_ = get_time_(msg)`）。因为它走 `localtime` / `strftime`，每行算一次太贵 | 我们把格式化降到 27 ns，缓存交给 `log` 自己决定（3.7） |
| spdlog `details/os-inl.h` | `localtime` / `gmtime` 的平台包装，**`localtime_s` 和 `localtime_r` 参数顺序相反** | 照抄这个注意点（3.2） |
| coost `co/time.h` | 极简：`now.ns()/us()/ms()`、`now.str(fmt)`、`mono_time`、`timer`。**默认格式是 `"%Y-%m-%d %H:%M:%S"`**，空格分隔的本地时间，不带偏移 | 默认改成 ISO 8601 带 `Z` / 带偏移（3.6）；但它的默认提醒了我「人手写的时间串多用空格」，所以解析那边倾向接受空格（3.8） |
| Howard Hinnant `date_algorithms` | `civil_from_days` / `days_from_civil`，整数运算、无查表 | 直接用，并拿 411 万个时刻和 `gmtime_r` 对过（3.1） |

## 6. 三件已拍的事

1. **单位安全：选了 B**，`timestamp` 值类型。连带把 `fs::last_write_time` 改成返回 `result<timestamp>`。
2. **`format_time` 默认到秒**。毫秒是日志的需求，日志会显式写 `precision::millis`。
3. **`parse_time` 接受空格代替 `T`**，也接受小写的 `t` 和 `z`。输出仍然一律用 `T`——输入宽容和输出严格不矛盾。
