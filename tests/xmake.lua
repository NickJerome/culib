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
    -- 框架自己的测试要用到 src/ 下的私有头文件 test_runner.h，
    -- 事件引擎的测试还要 io_engine.h / scheduler_detail.h
    add_includedirs("../src")
    -- 事件引擎的测试要造一对连起来的套接字。Windows 上没有 socketpair，只能在回环上
    -- 真连一次，而那要 winsock。**库本身不需要**（IOCP 那些调用全在 kernel32 里），
    -- 所以这条加在测试目标上而不是库上；将来 net 落地时库那边才需要。
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32")
    end
    add_deps("culib")
    add_tests("default")
target_end()
