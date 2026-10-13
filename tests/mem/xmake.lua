for _, file in ipairs(os.files(path.join(os.scriptdir(), "*.cpp"))) do
    local name = path.basename(file)
    local linux_only = name == "test_malloc_override"
    if not linux_only or is_plat("linux") then
        target("mem_" .. name)
            set_kind("binary")
            set_default(false)
            if not is_plat("windows") then
                set_languages("c++11")
            end
            set_warnings("all", "extra")
            set_encodings("utf-8")
            add_includedirs("../../src/mem/allocator")
            add_files(file)
            if name == "test_malloc_override" then
                add_deps("culib_malloc_override")
            else
                add_deps("culib")
            end
            if name == "test_new_override" then
                add_deps("culib_new_override")
            end
            if is_plat("windows") then
                add_defines("_CRT_SECURE_NO_WARNINGS")
            end
            add_tests("default")
        target_end()
    end
end
