# 命令行选项（flag）

- 日期：2026-10-23
- 代码：`include/cu/flag.h`，`src/flag/flag.cpp`，`src/platform/` 里的 `command_line*`

## 1. 做什么

解析命令行里的选项和子命令。

- 选项是全局对象，`main` 里调一次 `cu::flag::init(argc, argv)`。
- 有子命令时，命令也是全局对象。子命令直接嵌在外层命令的大括号里，代码怎么套，命令行就怎么套。
- 不再往下分的命令最后给一个函数（普通函数、lambda 或函数对象）。这个命令自己的选项写在函数前面，解析出来的值按顺序作为函数的参数传进去：命令的选项就是函数的参数。

`init` 先把命令行解析完，再调选中的命令的函数。函数跑起来之前，`--help` 已经打印，参数错误已经报了。

serve 用它取监听地址和要发的文件。

## 2. 接口

| 分类 | 签名 | 说明 |
| --- | --- | --- |
| 选项 | `cu::flag::boolean x(bool default_value, const char* help, {"name", "n"})` | 布尔选项，不吃下一个参数；要给值写成 `--name=false` |
| | `cu::flag::int64 x(std::int64_t default_value, help, names)` | 十进制整数，可以带正负号 |
| | `cu::flag::string x(std::string default_value, help, names)` | 原样的字符串 |
| 读 | `const T& x.value() const`，也可以直接当 `const T&` 用 | 全局选项的值。`init` 之前读，打印诊断后终止 |
| 命令 | `cu::flag::command x(const char* name, const char* help, {cu::flag::command(...), ...})` | 有子命令的命令，子命令写在大括号里 |
| | `cu::flag::command x(const char* name, const char* help, 选项..., function)` | 不再往下分的命令。选项直接写 `cu::flag::string(...)` 这样的临时对象，可以没有；`function` 的参数依次接这些选项的值（`bool`、`std::int64_t`、`std::string`，按常量引用传），返回 `int` 是进程的退出码，不返回值按 0 退出 |
| 解析 | `void cu::flag::init(int argc, char* argv[])` | 只调一次。有命令时跑选中的命令，再用它的返回值退出进程，不返回 |
| 位置参数 | `const std::vector<cu::string_view>& cu::flag::args()` | 不是选项也不是命令名的参数，按顺序；`--` 以后的全算 |

用的时候要知道的：

- names 写不带 `-` 的名字，第一个是主名字。一个字母的显示成 `-v`，长的显示成 `--verbose`；命令行上两种写法都认，`-verbose` 也行。
- 选项声明在哪儿，就在哪一级认：
  - 全局对象（和 `main` 里 `init` 之前的局部变量）是全局选项，每一级都认，写在命令名前后都行；
  - 写在命令里的只属于这个命令，只能写在命令名后面，值作为参数传给它的函数。
  
  别的地方声明选项（比如命令的函数里），打印诊断后终止。
- 函数的参数和选项按位置对应。个数、类型对不上，或者返回值不是 `int` 也不是 `void`，编译时报错；两个同类型的选项写反了顺序查不出来，所以选项和参数要挨着写。
- 函数可以是捕获东西的 lambda，也可以是函数对象，库存的是它的一份拷贝。
- 有命令的程序里，`init` 不返回，`main` 里写在它后面的代码不会执行，准备工作要放在它前面。
- 不是线程安全的：声明选项和命令、调 `init` 都在 `main` 的线程上。解析完以后，任何线程都能读。

### 例子

```cpp
#include <cstdio>

#include <cu/flag.h>

cu::flag::boolean verbose(false, "print more", {"verbose", "v"});   // 全局选项：每个命令都认

cu::flag::command admin("admin", "manage users", {
    // 命令自己的选项写在函数前面，值按顺序作为参数传进去
    cu::flag::command("register", "register a user",
        cu::flag::string("", "user name", {"name", "n"}),
        cu::flag::string("", "email address", {"email", "e"}),
        [](const std::string& name, const std::string& email) {
            std::printf("register %s <%s>\n", name.c_str(), email.c_str());
        }),
    cu::flag::command("remove", "remove the users given as arguments", [] {
        for (cu::string_view user : cu::flag::args()) {
            std::printf("remove %.*s\n", static_cast<int>(user.size()), user.data());
        }
    }),
});

cu::flag::command status("status", "show the service status", [] {
    std::printf("status, verbose=%d\n", verbose.value());
});

int main(int argc, char* argv[]) {
    cu::flag::init(argc, argv);
}
```

下面的输出来自 `tests/flag/flag_test.cpp` 里的程序。它和上面的例子一个样子，多了一个全局选项 `--retries` 和几个命令。CI 上逐字比对这些输出：

```
$ flag_test --help
Usage: flag_test [options] <command>

Commands:
  admin   manage users
  fail    exit with code 5
  status  show the service status

Options:
      --retries <int>  how many times to retry (default 3)
  -v, --verbose        print more
  -h, --help           show this help

$ flag_test admin register --help
Usage: flag_test admin register [options]

register a user

Options:
      --admin           give the user admin rights
  -e, --email <string>  email address
  -n, --name <string>   user name
      --retries <int>   how many times to retry (default 3)
  -v, --verbose         print more
  -h, --help            show this help

$ flag_test -v admin register -n bob
register name=bob email= admin=0 verbose=1

$ flag_test admin register --nmae=alice
flag_test admin register: unknown option --nmae
Run 'flag_test admin register --help' for usage.      （退出码 2）

$ flag_test admin
flag_test admin: missing command
Run 'flag_test admin --help' for usage.               （退出码 2）
```

### 命令行怎么写

照搬上一版的四条：

1. 选项和位置参数可以穿插，GNU 式：`cli admin remove alice -v bob`。Go 的 flag 遇到第一个位置参数就不再认选项，`crop *.png --width=1024` 会悄悄失效。
2. 布尔选项不吃下一个参数。
3. `--` 以后的全算位置参数。
4. 单独一个 `-` 是位置参数，惯例表示标准输入。

另外：

- 值写成 `--name=v`、`--name v`、`-n v`、`-n=v`。不认 `-n8080`，也不把 `-abc` 拆成三个短选项。不带 `=` 时，下一个参数无论是什么都是值，所以 `--retries -5` 是负五。
- 同一个选项给两次，以后一次为准，和 Go 一样。
- 未知选项报错，不收进位置参数：悄悄接受拼错的 `--widht` 是命令行程序最常见的坑。
- 命令名从左往右认。有子命令的命令后面必须跟一个子命令；位置参数只归最后那个命令。

### 帮助和报错

- `--help` 或 `-h` 打印认到那一级的帮助，写到 stdout，退出码 0。依次是：用法一行；命令自己的说明；子命令；这一级能用的选项（全局的，加上命令类的成员）；最后是 `-h, --help`。子命令和选项都按名字排。
- 默认值写在说明后面，零值（`false`、`0`、空串）不写，和 Go 一样。
- 参数不对时往 stderr 写两行，退出码 2：
  - 第一行是「程序名加认到的命令名: 原因」。原因有这几种：`missing command`、`unknown command x`、`unknown option --x`、`option --x needs a value`、`invalid value "v" for option --x: not an integer`（或 `out of range`、`not true or false`）、`option --help does not take a value`；
  - 第二行提示怎么看帮助。
  
  消息用英文，形状照 Go。
- 程序名取 `argv[0]` 去掉目录，再去掉结尾的 `.exe`。

## 3. 取舍

### 选项和命令都是全局对象

作者定的：选项在哪儿声明，就在哪儿用，`main` 只有一行。上一版也是全局对象。代价有两条：

- 要在 `main` 之前登记，是 CLAUDE.md 里「不在 `main` 之前执行代码」的例外；
- 一个程序只有一棵命令树，`init` 又会退出进程，所以测试都在子进程里跑。

登记表在第一次用时建，永不析构：进程退出时可能还有协程在读选项，全局的选项和命令析构时也要来表里注销。

### 子命令嵌在大括号里

大括号里的 `cu::flag::command(...)` 是外层构造函数的参数，一个临时对象。每个命令构造时先当顶层命令登记，外层接手时再从顶层拿掉，最后表里只剩全局对象。这样同一个类型既能当全局对象，也能嵌在里面。

名字重复的检查分两处：同一个大括号里的，外层接手时查；顶层的到 `init` 时才查，因为同名的子命令在被接手之前，也会当一会儿顶层命令。

嵌在里面的命令没有变量名，`main` 没法判断选中了哪个，所以命令带上要调的函数，由 `init` 调。

### 命令自己的选项写在命令上，值作为函数的参数

库要先知道一个命令的全部选项，才能打印它的帮助、查出拼错的选项。选项写在命令上，和命令一起在 `main` 之前登记好，`init` 解析完才调函数，所以不存在「什么时候算声明完了」的问题。

几点好处：

- 一个命令的东西都写在命令树里的一处：名字、说明、选项、要做的事；
- 函数里拿到的直接是值，`name` 就是 `std::string`，不用 `.value()`；
- 普通函数的参数表就是这个命令的选项。

写在命令里的 `cu::flag::string(...)` 是临时对象，构造时和全局选项一样登记。命令的构造函数接手它们：把值拷进命令自己的存储，登记表里那一项改成归这个命令。等临时对象析构时，它按原来的地址注销，已经找不到，什么也不做。

重名的检查也分两处：
- 同一个命令里的、和前面已经登记的全局选项重名的，临时对象登记时就查出来；
- 和写在后面或者别的文件里的全局选项重名的，到 `init` 时查。

大括号里的元素按顺序初始化，一个子命令接手完自己的选项，下一个才开始构造，所以兄弟命令之间同名的选项不算冲突。

代价：

- **参数和选项按位置对应。** 个数、类型对不上编译报错（`static_assert`），但两个同类型的写反了查不出来。
- **选项多的命令参数会很长。**

先后实现过两种别的写法，都换掉了：

1. **选项是命令函数里的局部变量，函数第一次读选项（或调 `args()`）时才解析。** 要靠猜「什么时候声明完了」：命令要是在读选项之前就动手，`--help` 和拼错的选项要等它跑完才查出来，`cli db drop --help` 会先删库再打印帮助，而且平时看不出来。
2. **命令写成一个类，选项是成员，`run()` 做事，树里写 `cu::flag::run<类名>`。** 没有时机问题（对象构造完，选项就全有了），CI 也全过了。但要在命令树之外另写一个类，作者觉得别扭。

### `init` 跑命令、退出进程

没有命令的程序和有命令的程序都只写 `cu::flag::init(argc, argv)`，不用选另一个入口。函数返回 `int` 时用它当退出码，不返回值时按 0。

函数可以不返回值，是为了让 lambda 好写：C++11 只在函数体只有一条 `return` 时才推导 lambda 的返回类型。多于一条语句又返回 `int` 的，要写成 `[]() -> int { ... }`。GCC 和 Clang 把多条语句的推导当扩展支持，但开 `-pedantic` 会警告，CI 里警告即错误。

### 帮助按名字排

全局对象跨文件的构造顺序不定。如果按登记的顺序排，同一份代码编出来的程序，帮助可能不一样。Go 的 flag 也按名字排。

### Windows 的命令行

`main` 收到的 `argv` 是 C 运行库按系统代码页从 `GetCommandLineA()` 拆出来的，代码页里没有的字符已经变成了问号。`init` 先确认传进来的是进程自己的 `argv`：照同样的规则拆一遍 `GetCommandLineA()`，和 `argv` 逐个比。确认了就拆 `GetCommandLineW()`，转成 UTF-8；对不上（测试拼出来的 `argv`）就原样用。

- 不比 `__argv` 的地址：MinGW 在 `-std=c++11` 下不一定声明它。
- 拆的规则照 UCRT 的 `argv_parsing.cpp`。不用 `CommandLineToArgvW`：要多链一个 shell32，括起来的一段里连着两个引号时，规则也和 C 运行库不一样。

### 和别的库比

| | 命令自己的选项写在哪儿 | 子命令 | 入口 | 我们为什么不一样 |
| --- | --- | --- | --- | --- |
| Go `flag` | 没有命令，选项全局，`flag.String(...)` | 没有，要自己建 `FlagSet` | `flag.Parse()` | 它遇到第一个位置参数就停；我们照上一版，GNU 式穿插 |
| cobra（Go） | 命令对象上，`cmd.Flags()` | `AddCommand` 一条条挂 | `rootCmd.Execute()` | 树在 `init()` 里平着挂，代码里看不出层次 |
| click（Python） | 装饰器，值作为参数传给函数 | `@group.command()` | 调用最外层的组 | 和我们一样是参数；它按参数名对应，C++ 没有反射，我们只能按位置 |
| picocli（Java）、oclif（JS）、Symfony Console（PHP） | 命令是一个类，选项是字段 | 注解或注册 | 框架建对象、解析、再调执行的方法 | 我们实现过，见上一节；要在命令树之外另写一个类 |
| CLI11（C++） | 局部的 `App`，`add_option` 绑到变量 | `add_subcommand` | `CLI11_PARSE` 宏 | 要建对象、要异常；宏里藏着 `return` |
| git | 每个命令自己调 `parse_options` | 一张 `commands[]` 表 | `handle_builtin` | 表里平铺，嵌套的命令要写全路径 |

代码约 950 行，不算空行和注释：`flag.cpp` 640 行，头文件 189 行（一半是把选项的值交给函数的模板），Windows 拆命令行 101 行，其余是 POSIX 那一份。

## 4. 不做的事

| 不做 | 为什么 | 什么条件下重开 |
| --- | --- | --- |
| `--version` | 没有调用方 | 参考程序要打版本号时 |
| 必填选项 | serve 的选项都有默认值 | 有调用方时 |
| 判断选项给没给过（`was_given`）、程序里改选项（`set`），上一版有 | 没有调用方 | 有调用方时 |
| 无符号整数、浮点数、时长（上一版有前两种） | 没有调用方 | 有调用方时 |
| `-abc` 合写、`-n8080` | 和 `-name` 这种写法冲突 | 不重开 |
| 有子命令、自己也能跑的命令（`git remote`） | 拼错的子命令会变成它的位置参数，悄悄跑了别的 | 有这种命令的使用方时 |
| 有子命令的命令自己的选项（`cli admin --dry-run register`） | 全局选项够用 | 有调用方时 |
| 不退出进程的解析（返回 `result`）、单独取帮助文本 | 测试在子进程里跑；一件事一种做法 | 有使用方要自己处理参数错误时 |
| 拼错时提示最像的名字（did you mean） | 没有调用方 | 命令多到记不住时 |
| 帮助按终端宽度折行 | 没有调用方 | 说明长到要折行时 |
| 从环境变量、配置文件取默认值 | 是 config 模块的事 | 做 config 时 |
| 隐藏的命令、命令的别名 | 没有调用方 | 有调用方时 |

和作者一起看过、没有选的写法：

| 写法 | 为什么没选 |
| --- | --- |
| 局部的解析器对象，选项挂在它上面（CLI11、Go 的 `FlagSet`） | 作者要全局对象，`main` 只写一行 |
| `main` 里一张表：`dispatch(argc, argv, {{"admin register", ...}})`（git 的做法） | 每一条都写全路径字符串，看不出层次 |
| group 和 command 两个类型，树写在 `main` 里 | 有子命令的和没有的本来是一种东西；树应该在全局写好 |
| 每个命令一个全局对象，外层在大括号里列子命令的名字 | 代码里看不出树的层次 |
| 命令的选项是全局对象，再列进命令的大括号里 | 选项定义在一处、还要列在另一处 |
| 选项是命令函数里的局部变量，第一次读选项时解析（实现过） | 要猜什么时候声明完了，猜错时 `--help` 和拼错的选项要等命令跑完才查，见第 3 节 |
| 同上，但函数里声明完再显式调一次解析 | 作者觉得这一行多余 |
| 选项全部是全局的，不分命令 | 作者要按命令分选项 |
| 命令是一个类，选项是成员，`run()` 做事（实现过，CI 全过） | 要在命令树之外另写一个类，作者觉得别扭 |

## 5. 测了什么

`tests/flag/flag_test.cpp`（目标 `flag_test`）本身就是一个带两层子命令的命令行程序。命令的函数里，普通函数和 lambda、带选项参数和不带的、返回 `int` 和不返回值的都有。48 条用例，每条带不同的参数起一次子进程，比对退出码、stdout 和 stderr；Linux、macOS 上 fork 后直接调，Windows 上 `CreateProcessW` 起真的进程。

- 帮助：顶层、有子命令的命令、两层下的命令，`-h` 和 `--help`，命令的选项后面跟 `-h`。
- 命令跑起来之前：`--help` 和拼错的选项都在命令打印任何东西之前处理。
- 解析：
  - 长短两种写法、带 `=` 和不带；
  - 全局选项写在命令名前后；
  - 同一个选项给两次以后一次为准；
  - 选项和位置参数穿插，`--`，单独的 `-`；
  - 单横线的长名字，以 `-` 开头的值；
  - 布尔的 `=true`、`=false`；
  - 整数的正负号和最小值；
  - 命令函数的返回值当退出码。
- Windows：参数里有 runner 的代码页里没有的字符（走 `GetCommandLineW` 那条路）；参数里有空格、引号、结尾的反斜杠，还有空参数（查拆命令行的规则）。Linux 和 macOS 上同一批用例照样跑。
- 报错（退出码 2，两行输出）：缺命令、`--` 写在命令的位置、未知命令、命令的选项写在命令名前面、未知选项、缺值、不是整数、溢出、不是 true 或 false、`--help=x`。
- 误用（终止，stderr 里有那句诊断），14 种：
  - 选项：`init` 之前读选项、`init` 之前调 `args()`、选项重名、用 `h` 当名字、名字以 `-` 开头、同一个选项里名字写两遍、在命令的函数里声明选项、命令的选项和全局选项重名、一个命令的两个选项重名；
  - 命令：顶层命令重名、同一层的子命令重名、命令名里有空格、`init` 之后再声明命令；
  - `init` 调两次（从一个捕获了 `argc`、`argv` 的 lambda 里调）。
- serve 的冒烟测试改用 `--address=...` 和 `--file ...` 起服务。
- CI 里当作依赖引入的那个工程声明了一个全局选项，C++11 和 C++17 都编。

没测：

- 编译期的检查（函数的参数和选项对不上、返回值类型不对、函数前面放了不是选项的东西）：没有测试，也没确认过报出来的正是那句 `static_assert`。
- Windows 上 `argv` 是进程自己的、却和系统的命令行对不上，退回原样用 `argv` 的那条路。
- `flag_test` 不在 ASan、UBSan、TSan 下跑，那个任务只跑 `culib_test`。
- 很长的命令行、很多的选项。
- 进程退出时别的线程还在读选项。
