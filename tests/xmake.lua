-- 普通单元测试编译成 culib_test；进程级状态和故障测试使用独立程序。
-- 在终端里查看完整报告：xmake run culib_test
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

target("scheduler_deadlock_test")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    if not is_plat("windows") then set_languages("c++11") end
    set_warnings("allextra", "pedantic", "error")
    if is_plat("windows") then add_defines("_CRT_SECURE_NO_WARNINGS") end
    add_files("fault/scheduler_deadlock.cpp")
    add_deps("culib")
    add_tests("default")
target_end()

-- 独立进程替换 nothrow new，验证提交前分配失败不会留下半个协程。
target("coroutine_allocation_test")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    if not is_plat("windows") then set_languages("c++11") end
    set_warnings("allextra", "pedantic", "error")
    add_includedirs("../src")
    add_files("fault/coroutine_allocation.cpp")
    add_deps("culib")
    add_tests("default")
target_end()
