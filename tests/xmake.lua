-- 单元测试编成 culib_test：xmake test 构建并运行，xmake run culib_test 看完整报告。
target("culib_test")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    -- MSVC 没有 C++11 模式（最低是 C++14），只在 GCC / Clang 上限定 C++11
    if not is_plat("windows") then
        set_languages("c++11")
    end
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_files("*.cpp")
    -- 测试框架自己的测试要用它的内部头文件
    add_includedirs("../src", "../src/testing")
    add_deps("culib")
    add_tests("default")
target_end()
