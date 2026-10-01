set_project("culib")
set_xmakever("2.8.5")

add_rules("mode.debug", "mode.release")

target("culib")
    set_kind("static")
    -- MSVC 没有 C++11 模式（最低即 C++14），只在 GCC / Clang 上限定 C++11
    if not is_plat("windows") then
        set_languages("c++11")
    end
    set_warnings("allextra")
    set_encodings("utf-8")
    add_includedirs("include", {public = true})
    add_headerfiles("include/(culib/**.h)")
    add_files("src/**.cpp")
    -- 每个函数、数据单独成段，使用方链接时即可剔除未用到的代码
    if is_plat("windows") then
        add_cxflags("/Gy")
    else
        add_cxflags("-ffunction-sections", "-fdata-sections")
    end
target_end()

-- 只在 culib 作为主工程时构建测试；被其他工程 includes 时跳过
if os.projectdir() == os.scriptdir() then
    includes("tests")
end
