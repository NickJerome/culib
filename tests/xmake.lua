-- 所有测试编译成一个程序 culib_test：默认不参与构建，执行 xmake test 时自动编译并运行。
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
    -- 事件引擎的测试要造一对连起来的套接字。Windows 上没有 socketpair，只能在回环上
    -- 真连一次，而那要 winsock。**库本身不需要**（IOCP 那些调用全在 kernel32 里），
    -- 所以这条加在测试目标上而不是库上；将来 net 落地时库那边才需要。
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32")
    end
    add_deps("culib")
    add_tests("default")
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
