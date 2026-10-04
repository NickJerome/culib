set_project("culib")
set_xmakever("2.8.5")

add_rules("mode.debug", "mode.release")

-- culib 是主工程（而不是被其他工程 includes）时，才启用只对自己人有意义的严格检查
local is_main_project = os.projectdir() == os.scriptdir()

target("culib")
    set_kind("static")
    -- MSVC 没有 C++11 模式（最低即 C++14），只在 GCC / Clang 上限定 C++11
    if not is_plat("windows") then
        set_languages("c++11")
    end
    -- 警告级别：allextra 即 -Wall -Wextra（MSVC 为 /W4），与 tests 对齐。
    -- 只在自己是主工程时把警告当错误：被其他工程 includes 时不加 error——
    -- 使用方的编译器版本不同，将来新编译器新增的警告不该打断他们的构建。
    -- MSVC 的 /W4 原先豁免（「只看不拦」），因为在 CI 第一次编译 Windows 代码
    -- 之前根本没法逐条排查；那边全绿之后纳入，三个平台一致。
    if is_main_project then
        set_warnings("allextra", "pedantic", "error")
    else
        set_warnings("allextra", "pedantic")
    end
    set_encodings("utf-8")
    add_includedirs("include", {public = true})
    add_headerfiles("include/(culib/**.h)")
    add_files("src/**.cpp")
    -- 平台相关实现按文件名后缀区分：Windows / MinGW 用 *_win.cpp，其他平台用 *_posix.cpp
    if is_plat("windows", "mingw") then
        remove_files("src/**_posix.cpp")
    else
        remove_files("src/**_win.cpp")
    end
    -- 每个函数、数据单独成段，使用方链接时即可剔除未用到的代码
    if is_plat("windows") then
        add_cxflags("/Gy")
    else
        add_cxflags("-ffunction-sections", "-fdata-sections")
    end
target_end()

-- 测试和示例只在 culib 作为主工程时构建；被其他工程 includes 时跳过
if is_main_project then
    includes("tests")
    includes("examples")
end
