local supported = is_plat("linux", "macosx", "windows", "mingw") and is_arch("x86_64", "x64", "arm64", "arm64-v8a")

if supported then
    target("mem_benchmark")
        set_kind("binary")
        set_default(false)
        set_group("benchmarks")
        if not is_plat("windows") then set_languages("c++11") end
        set_warnings("allextra", "pedantic", "error")
        add_files("../../benchmarks/mem_alloc.cpp")
        add_deps("culib")
    target_end()

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
            add_includedirs("allocator")
            add_files("allocator/*.cpp", "override/override.cpp", "override/new.cpp")
            add_cxxflags("-ftls-model=initial-exec")
            add_ldflags("-ftls-model=initial-exec")
            add_syslinks("pthread")
        target_end()
    end

end
