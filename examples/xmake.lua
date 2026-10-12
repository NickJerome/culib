-- 每个示例编译成一个单独的程序，默认不参与构建：
--     xmake build -g examples      编译全部示例
--     xmake run example_result     运行其中一个
--
-- 把示例纳入构建，是为了让 README「新增功能」里「在 examples/ 下添加用法示例」这条约定
-- 有执行机制：接口改了而示例没跟着改时，构建会失败，而不是等到哪天有人去看示例才发现。
for _, file in ipairs(os.files(path.join(os.scriptdir(), "*.cpp"))) do
    target("example_" .. path.basename(file))
        set_kind("binary")
        set_default(false)
        set_group("examples")
        -- MSVC 没有 C++11 模式（最低即 C++14），只在 GCC / Clang 上限定 C++11
        if not is_plat("windows") then
            set_languages("c++11")
        end
        -- 与根 xmake.lua 的库目标保持一致：警告即错误，三个平台都一样
        set_warnings("allextra", "pedantic", "error")
        set_encodings("utf-8")
        add_files(file)
        add_deps("culib")
        -- mem 示例演示「代码不用改」：接上全局 new/delete 覆盖。条件和 src/mem/xmake.lua 定义覆盖目标的一样
        if path.basename(file) == "mem" and is_plat("linux", "macosx", "windows", "mingw") and
                is_arch("x86_64", "x64", "arm64", "arm64-v8a") then
            add_deps("culib_new_override")
        end
    target_end()
end
