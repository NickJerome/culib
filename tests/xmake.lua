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
              "../src/base/string_view.cpp", "../src/base/result.cpp", "../src/base/optional.cpp",
              "../src/base/fatal.cpp")
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

-- 断言执行次序的测试：只开一根工作线程（main 里设 WAVVIX_MAXPROCS=1），次序才确定。
-- culib_test 那边开 4 根，两个程序的运行时互不影响。
target("runtime_order_test")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    if not is_plat("windows") then set_languages("c++11") end
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_files("order/*.cpp")
    add_includedirs("../src")
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

-- 用 /CETCOMPAT 链接：支持 CET 的机器上进程开着影子栈，协程换栈不切影子栈，运行时要在第一根协程
-- 跑起来之前打印诊断并终止。只有 MSVC 这样链接；机器不支持 CET 时测试打印 SKIP。
if is_plat("windows") then
target("cet_test")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_files("process/cet.cpp")
    add_deps("culib")
    add_ldflags("/CETCOMPAT", {force = true})
    add_tests("default", {run_timeout = 30000})
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
