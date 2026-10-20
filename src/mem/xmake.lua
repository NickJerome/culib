target("culib_new_override")
    set_kind("object")
    set_default(false)
    if not is_plat("windows") then set_languages("c++11") end
    set_warnings("allextra", "pedantic", "error")
    add_files("override/new.cpp")
    add_deps("culib")
target_end()

if is_plat("linux") then
    target("culib_malloc_override")
        set_kind("shared")
        set_default(false)
        set_languages("c++11")
        set_warnings("allextra", "pedantic", "error")
        set_encodings("utf-8")
        add_includedirs("../../include", {public = true})
        -- 不编入 new.cpp：C++ 程序的 operator new 由 libstdc++ 实现，它调用的正是这里的 malloc；
        -- 编进来就得依赖 libstdc++，C 程序加载时会白占 1 MB 多常驻内存。
        -- 只导出 C 入口（见 override.cpp），内部调用不经过 PLT；没有异常，也就不依赖 libgcc_s。
        add_files("pages_posix.cpp", "allocator/allocator.cpp", "allocator/native_posix.cpp",
                  "override/override.cpp")
        add_cxxflags("-fvisibility=hidden", "-fno-exceptions", "-ftls-model=initial-exec")
        add_ldflags("-ftls-model=initial-exec")
        add_syslinks("pthread")
    target_end()
end
