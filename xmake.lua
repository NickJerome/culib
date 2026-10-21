set_project("culib")
set_xmakever("2.8.5")

add_rules("mode.debug", "mode.release")

-- 自己是主工程时才开只对自己有意义的检查：被别的工程 includes 时，警告不当错误，也不构建测试。
local is_main_project = os.projectdir() == os.scriptdir()

target("culib")
    set_kind("static")
    -- MSVC 没有 C++11 模式（最低是 C++14），只在 GCC / Clang 上限定 C++11
    if not is_plat("windows") then
        set_languages("c++11")
    end
    if is_main_project then
        set_warnings("allextra", "pedantic", "error")
    else
        set_warnings("allextra", "pedantic")
    end
    set_encodings("utf-8")
    add_includedirs("include", {public = true})
    add_includedirs("src")
    add_headerfiles("include/(cu/**.h)")
    add_files("src/**.cpp")
    -- 平台代码按文件名后缀分：Windows 和 MinGW 用 _win，其余用 _posix；_linux、_macos 只给对应的平台
    if is_plat("windows", "mingw") then
        remove_files("src/**_posix.cpp")
    else
        remove_files("src/**_win.cpp")
    end
    if not is_plat("linux") then
        remove_files("src/**_linux.cpp")
    end
    if not is_plat("macosx") then
        remove_files("src/**_macos.cpp")
    end
    -- 最低支持 Windows 10；只给 culib 自己的编译单元，公共头文件不含平台头
    if is_plat("windows", "mingw") then
        add_defines("_WIN32_WINNT=0x0A00", "WINVER=0x0A00")
    end
    if is_plat("linux") then
        add_syslinks("pthread", {public = true})
    end
    -- 每个函数、数据单独成段，使用方链接时可以剔除用不到的代码
    if is_plat("windows") then
        add_cxflags("/Gy")
    else
        add_cxflags("-ffunction-sections", "-fdata-sections")
    end
target_end()

if is_main_project then
    includes("tests")
end
