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
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32", {public = true})
    elseif is_plat("linux") then
        add_syslinks("pthread", {public = true})
    end
    -- 换栈的汇编：每个（ABI，架构，汇编器）一个文件，只支持 64 位。没匹配上的目标一个都不加，
    -- 由 src/runtime/coroutine.cpp 里的 #error 在编译期拦住。
    if is_arch("x86_64", "x64") then
        if is_plat("windows") then
            add_files("src/runtime/asm/context_x86_64_pe.asm")   -- MSVC 不支持内联汇编，走 ml64
        elseif is_plat("mingw") then
            add_files("src/runtime/asm/context_x86_64_pe.S")
        else
            add_files("src/runtime/asm/context_x86_64_sysv.S")   -- Linux 和 macOS 同一套 ABI
        end
    elseif is_arch("arm64", "arm64-v8a") then
        add_files("src/runtime/asm/context_arm64_aapcs.S")
    end
    -- 每个函数、数据单独成段，使用方链接时可以剔除用不到的代码。
    -- /GT：协程会换线程，MSVC 默认会把线程局部变量的地址留在寄存器里跨过函数调用，这个开关让它每次重新取。
    if is_plat("windows") then
        add_cxflags("/Gy", "/GT")
    else
        add_cxflags("-ffunction-sections", "-fdata-sections")
    end
target_end()

if is_main_project then
    includes("tests")
end
