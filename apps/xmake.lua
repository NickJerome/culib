-- 参考程序：库的使用方，和库一起编，接口改坏了马上编不过。
target("serve")
    set_kind("binary")
    set_group("apps")
    -- MSVC 没有 C++11 模式（最低是 C++14），只在 GCC / Clang 上限定 C++11
    if not is_plat("windows") then
        set_languages("c++11")
    end
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_files("serve/*.cpp")
    add_deps("culib_new_override", "culib")   -- new / delete 走 culib 的分配器
target_end()
