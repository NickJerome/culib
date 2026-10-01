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
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_files("*.cpp")
    add_deps("culib")
    add_tests("default")
target_end()
