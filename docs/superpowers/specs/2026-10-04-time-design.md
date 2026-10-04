# time 设计

- 日期：2026-10-04
- 状态：**接口待确认**（原型已跑通，代码未落地）
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
| 格式化 | `result<std::string> format_local_time(timestamp, precision = precision::seconds)` | `2026-10-04T19:12:51+08:00`，**带偏移**，见 3.6。要查系统时区所以可能失败 |
| 解析 | `result<timestamp> parse_time(string_view)` | 只认 ISO 8601，见 3.8 |
| 拆开 | `date_time utc_parts(timestamp) noexcept` | 年月日时分秒毫秒，给要自己拼格式的调用方 |
| 拆开 | `result<date_time> local_parts(timestamp)` | 同上，本地时区，另带 `offset_minutes` |

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

### 3.4 单位安全：要你定的一处

`format_time(1791112371789)` 和 `format_time(1791112371)` 都是 `int64_t`，混起来**编译通过、静默错 1000 倍**。原型里两种写法都实现了，调用点并排跑过（见第 4 节输出）：

- **写法 A**：裸 `int64_t`，单位写在函数名里（`format_time` / `format_time_millis`）。笔误的实际后果：`format_time(event_millis)` 打印 `58728-02-04T06:23:09Z`，一声不响。
- **写法 B**：`timestamp` 值类型，单位写在类型里。同一个笔误是编译错误：

  ```
  error: could not convert 'event_millis' from 'const long int' to 'timestamp'
  ```

**倾向 B**，理由：「难以误用」排在「少一个概念」前面，而 `timestamp` 是 8 字节平凡可复制、全内联，按值传递和 `int64_t` 生成同样的代码，**零开销这条硬约束没破**。

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

## 4. 原型的完整用例和实际输出

原型在 scratchpad（`time_proto.cpp`），两种写法并排。源码的核心部分：

```cpp
const i64 mtime_seconds = 1791112371;        // 假装是 fs::last_write_time 的返回值
const i64 event_millis = 1791112371789LL;    // 假装是某个事件的毫秒时间戳

// 写法 A
a::format_time(a::unix_seconds())
a::format_time_millis(a::unix_millis())
a::format_time(mtime_seconds)
a::format_time(event_millis)                 // ← 笔误，编译通过

// 写法 B
b::format_time(timestamp::now(), precision::millis)
b::format_time(timestamp::now_coarse())
b::format_time(timestamp::from_unix_seconds(mtime_seconds))
b::format_time(timestamp::from_unix_millis(event_millis), precision::millis)
b::format_time(event_millis)                 // ← 同一个笔误，编译错误
```

实际输出（`TZ=Asia/Shanghai`）：

```
=== 写法 A：裸 int64_t，单位在函数名里 ===
  现在（秒）      2026-10-04T11:14:14Z
  现在（毫秒）    2026-10-04T11:14:14.376Z
  文件修改时间    2026-10-04T11:12:51Z
  事件时间        2026-10-04T11:12:51.789Z
  **把毫秒喂给秒版本**：58728-02-04T06:23:09Z   ← 编译通过，静默错 1000 倍

=== 写法 B：timestamp 值类型，单位在类型里 ===
  现在（毫秒精度）2026-10-04T11:14:14.376Z
  现在（粗时钟）  2026-10-04T11:14:14Z   ← 便宜 10 倍
  文件修改时间    2026-10-04T11:12:51Z
  事件时间        2026-10-04T11:12:51.789Z
  同样的笔误      format_time(event_millis) → **编译错误**
  本地时区        2026-10-04T19:12:51.789+08:00   ← 带偏移，不带就有歧义

=== 边界 ===
               0  1970-01-01T00:00:00Z  参照 1970-01-01T00:00:00Z  一致
              -1  1969-12-31T23:59:59Z  参照 1969-12-31T23:59:59Z  一致
          -86400  1969-12-31T00:00:00Z  参照 1969-12-31T00:00:00Z  一致
    253402300799  9999-12-31T23:59:59Z  参照 9999-12-31T23:59:59Z  一致
      1791112371  2026-10-04T11:12:51Z  参照 2026-10-04T11:12:51Z  一致

=== 毫秒的小数位 ===
   1791112371000  2026-10-04T11:12:51.000Z
   1791112371007  2026-10-04T11:12:51.007Z
   1791112371070  2026-10-04T11:12:51.070Z
   1791112371700  2026-10-04T11:12:51.700Z
              -1  1969-12-31T23:59:59.999Z
```

最后一行是故意的：毫秒为负时要**向下取整**，`-1` 毫秒是 1969 年最后一秒的第 999 毫秒，不是 `.-001`。

## 5. 读过的东西

| 项目 | 看到了什么 | 我们哪里不一样 |
| --- | --- | --- |
| spdlog `pattern_formatter-inl.h` | **按整秒缓存 `std::tm`**（`if (secs != last_log_secs_) cached_tm_ = get_time_(msg)`）。因为它走 `localtime` / `strftime`，每行算一次太贵 | 我们把格式化降到 27 ns，缓存交给 `log` 自己决定（3.7） |
| spdlog `details/os-inl.h` | `localtime` / `gmtime` 的平台包装，**`localtime_s` 和 `localtime_r` 参数顺序相反** | 照抄这个注意点（3.2） |
| coost `co/time.h` | 极简：`now.ns()/us()/ms()`、`now.str(fmt)`、`mono_time`、`timer`。**默认格式是 `"%Y-%m-%d %H:%M:%S"`**，空格分隔的本地时间，不带偏移 | 默认改成 ISO 8601 带 `Z` / 带偏移（3.6）；但它的默认提醒了我「人手写的时间串多用空格」，所以解析那边倾向接受空格（3.8） |
| Howard Hinnant `date_algorithms` | `civil_from_days` / `days_from_civil`，整数运算、无查表 | 直接用，并拿 411 万个时刻和 `gmtime_r` 对过（3.1） |

## 6. 要你拍的三件事

1. **单位安全选 A 还是 B**（3.4）。选 B 要连带把 `fs::last_write_time` 改成返回 `result<timestamp>`。
2. **`format_time` 的默认精度**：秒还是毫秒？倾向秒——毫秒是日志的需求，而日志会显式写 `precision::millis`。
3. **`parse_time` 要不要接受空格代替 `T`**（3.8）。倾向接受。
