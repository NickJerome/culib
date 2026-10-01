-- 每个 *_test.cpp 是一个独立的测试程序，返回 0 表示通过。
-- 默认不参与构建，执行 xmake test 时自动编译并运行。
for _, file in ipairs(os.files(path.join(os.scriptdir(), "*_test.cpp"))) do
    target(path.basename(file))
        set_kind("binary")
        set_default(false)
        set_group("tests")
        -- MSVC 没有 C++11 模式（最低即 C++14），只在 GCC / Clang 上限定 C++11
        if not is_plat("windows") then
            set_languages("c++11")
        end
        set_warnings("allextra", "pedantic", "error")
        set_encodings("utf-8")
        add_files(file)
        add_deps("culib")
        add_tests("default")
    target_end()
end
