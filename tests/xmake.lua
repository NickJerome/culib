-- 普通单元测试编译成 culib_test；进程级状态和故障测试使用独立程序。
-- 在终端里查看完整报告：xmake run culib_test
target("layers_test")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    if not is_plat("windows") then set_languages("c++11") end
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_includedirs("../include", "../src")
    add_files("process/layers.cpp", "../src/cli/config.cpp", "../src/cli/flag.cpp",
              "../src/fs/path.cpp", "../src/base/lexical_cast.cpp",
              "../src/base/string_view.cpp", "../src/base/result.cpp", "../src/base/optional.cpp")
    if is_plat("windows", "mingw") then
        add_files("../src/mem/pages_win.cpp")
        add_defines("_WIN32_WINNT=0x0A00", "WINVER=0x0A00")
    else
        add_files("../src/mem/pages_posix.cpp")
        add_syslinks("pthread")
    end
    add_tests("default")
target_end()

target("culib_test")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    -- MSVC 没有 C++11 模式（最低即 C++14），只在 GCC / Clang 上限定 C++11
    if not is_plat("windows") then
        set_languages("c++11")
    end
    -- 与根 xmake.lua 的库目标保持一致：警告即错误，三个平台都一样
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_files("*.cpp")
    -- 测试框架和终端测试需要各自模块的内部头文件。
    add_includedirs("../src", "../src/testing", "../src/cli")
    add_deps("culib")
    add_tests("default")
target_end()

target("os_stop_test")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    if not is_plat("windows") then set_languages("c++11") end
    set_warnings("allextra", "pedantic", "error")
    add_files("process/os_stop.cpp")
    add_deps("culib")
    add_tests("threads", {runargs = {"threads"}, run_timeout = 15000})
    if is_plat("windows", "mingw") then
        add_tests("ctrl_c", {runargs = {"ctrl-c"}, run_timeout = 15000})
        add_tests("ctrl_break", {runargs = {"ctrl-break"}, run_timeout = 15000})
    else
        add_tests("sigint", {runargs = {"sigint"}, run_timeout = 15000})
        add_tests("sigterm", {runargs = {"sigterm"}, run_timeout = 15000})
    end
target_end()

-- Linux 文件 io_uring 的两个专用目标。faults 注册为常规测试：setup 层故障
-- （ENOSYS/EPERM/ENOMEM/EMFILE 的注入与缓存）在任何 Linux 内核上都必须通过，
-- enter 两条在缺 native 时自行打印 [SKIP]，受限环境照常可跑。native 是
-- required 门禁，缺 native 必须非零退出——不注册进 xmake test（否则受限环境的
-- 普通 suite 会硬失败），由 CI 和开发者显式 `xmake run file_uring_test native`。
-- 故障注入只发生在这个测试二进制的 --wrap=syscall 包装里，生产代码没有故障开关。
if is_plat("linux") then
target("file_uring_test")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    set_languages("c++11")
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_includedirs("../src")
    add_files("process/io_uring_faults.cpp")
    add_deps("culib")
    add_ldflags("-Wl,--wrap=syscall")
    add_tests("faults", {runargs = {"faults"}, run_timeout = 60000})
target_end()

-- 私有 UAPI（src/runtime/io_uring_uapi.h）与现代真实内核头的编译期对照：
-- size / offsetof / 常量。断言全在编译期，只编译不跑。只在装有现代
-- <linux/io_uring.h> 的环境构建（CI 的 Linux 任务显式构建它）；旧 GCC 容器
-- 没有这个头也不编它，legacy_gcc48.sh 只编 src/** 和 tests/*.cpp，
-- 不进 tests/process/。不注册为常规测试：头缺失的环境不应构建失败。
target("file_uring_uapi_check")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    set_languages("c++11")
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_includedirs("../src")
    add_files("process/io_uring_uapi.cpp")
target_end()
end

-- 运行时的误用和内存耗尽都是打印诊断后终止，每种情形在子进程里跑；还替换了全局 nothrow new。
target("runtime_faults_test")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    if not is_plat("windows") then set_languages("c++11") end
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_includedirs("../src")
    add_files("fault/runtime_faults.cpp")
    add_deps("culib")
    add_tests("default", {run_timeout = 120000})
target_end()
