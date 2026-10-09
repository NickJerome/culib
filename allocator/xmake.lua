local supported = is_plat("linux", "windows") and is_arch("x86_64", "x64", "arm64", "arm64-v8a")

if supported then
    target("culib_allocator")
        set_kind("static")
        if not is_plat("windows") then set_languages("c++11") end
        set_warnings("allextra", "pedantic", "error")
        set_encodings("utf-8")
        add_includedirs("../include", {public = true})
        add_files("src/*.cpp")
        add_headerfiles("../include/(edumalloc/*.h)")
        if is_plat("windows") then add_defines("_CRT_SECURE_NO_WARNINGS") end
        if is_plat("linux") then add_syslinks("pthread", {public = true}) end
    target_end()

    target("culib_new_override")
        set_kind("object")
        if not is_plat("windows") then set_languages("c++11") end
        set_warnings("allextra", "pedantic", "error")
        add_files("src-override/new.cpp")
        add_deps("culib_allocator")
    target_end()

    if is_plat("linux") then
        target("culib_malloc_override")
            set_kind("shared")
            set_languages("c++11")
            set_warnings("allextra", "pedantic", "error")
            set_encodings("utf-8")
            add_includedirs("../include", {public = true})
            add_files("src/*.cpp", "src-override/override.cpp", "src-override/new.cpp")
            add_defines("EDUMALLOC_BUILD_SHARED")
            add_cxxflags("-ftls-model=initial-exec")
            add_ldflags("-ftls-model=initial-exec")
            add_syslinks("pthread")
        target_end()
    end

end
