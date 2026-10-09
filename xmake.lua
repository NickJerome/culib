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
    if is_main_project then
        set_warnings("allextra", "pedantic", "error")
    else
        set_warnings("allextra", "pedantic")
    end
    set_encodings("utf-8")
    add_includedirs("include", {public = true})
    add_includedirs("src")
    add_headerfiles("include/(culib/**.h)")
    add_files("src/**.cpp")
    -- 平台相关实现按文件名后缀区分：Windows / MinGW 用 *_win.cpp，其他平台用 *_posix.cpp
    if is_plat("windows", "mingw") then
        remove_files("src/**_posix.cpp")
    else
        remove_files("src/**_win.cpp")
    end
    -- 事件引擎的三个后端在 POSIX 内部还要再分一次：Linux 是 epoll、macOS 是 kqueue，
    -- 两边的头文件完全不同，_posix 那一档装不下。没匹配上的平台一个后端都不会进来，
    -- 由 src/runtime/io_engine.cpp 的 #error 在编译期拦住（不是链接期缺符号）。
    if not is_plat("linux") then
        remove_files("src/**_linux.cpp")
    end
    if not is_plat("macosx") then
        remove_files("src/**_macos.cpp")
    end
    -- 最低支持 Windows 10（路线图第 6 节）。_WIN32_WINNT 决定 SDK 头文件声明哪些 API，
    -- 各工具链的默认值不同（MinGW 随版本变、MSVC 跟着 SDK 走），在这里定死，两边看到
    -- 同一套。只给 culib 自己的编译单元：公共头文件不含平台头，使用方不受影响。
    if is_plat("windows", "mingw") then
        add_defines("_WIN32_WINNT=0x0A00", "WINVER=0x0A00")
        add_syslinks("ws2_32", {public = true})
    elseif is_plat("macosx") then
        add_frameworks("CoreServices", "CoreFoundation", {public = true})
    end

    -- 协程的换栈汇编：每个 (ABI, 架构, 汇编器) 一个文件，按目标挑一个。
    -- 协程是全有或全无的——程序一旦用了，发到哪个平台就必须有那个平台的汇编，
    -- 所以这里**不给兜底分支**：没覆盖的目标应该在构建时就失败，而不是链接时缺符号。
    if is_arch("x86_64", "x64") then
        if is_plat("windows") then
            add_files("src/runtime/asm/context_x86_64_pe.asm")    -- MSVC 不支持内联汇编，走 ml64
        elseif is_plat("mingw") then
            add_files("src/runtime/asm/context_x86_64_pe.S")
        else
            add_files("src/runtime/asm/context_x86_64_sysv.S")    -- Linux 和 macOS 同一套 ABI
        end
    elseif is_arch("arm64", "arm64-v8a") then
        add_files("src/runtime/asm/context_arm64_aapcs.S")
    elseif is_arch("x86", "i386") and is_plat("windows") then
        add_files("src/runtime/asm/context_i386_pe.asm")
    elseif is_arch("x86", "i386") and is_plat("mingw") then
        add_files("src/runtime/asm/context_i386_pe.S")
    end
    -- 没匹配上的组合（32 位非 Windows）这里什么都不加，由
    -- src/runtime/coroutine.cpp 的 #error 在编译期报错——比链接期缺符号清楚得多。
    -- 没匹配上的架构这里什么都不加，由 src/runtime/coroutine.cpp 里的 #error 报错——
    -- xmake 的描述作用域里 raise / assert / error 全是 nil（都报 unknown interface），
    -- 而那个错误会让 xmake f 失败、却让后续的 xmake 沿用上一份还有效的配置继续构建。
    -- 结果是改了配置看不出来、二进制还是旧的，白跑两轮验证。门禁放在 C++ 里更可靠。
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
