-- 性能测量程序。不默认构建：xmake build -g benchmarks，或者按名字构建。
-- 和 Go 对比用 compare.sh，Go 那一侧在 go/ 里，见 README.md。

target("scoreboard")
    set_kind("binary")
    set_default(false)
    set_group("benchmarks")
    if not is_plat("windows") then set_languages("c++11") end
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_files("scoreboard.cpp")
    add_deps("culib")
target_end()

target("mem_benchmark")
    set_kind("binary")
    set_default(false)
    set_group("benchmarks")
    if not is_plat("windows") then set_languages("c++11") end
    set_warnings("allextra", "pedantic", "error")
    add_files("mem_alloc.cpp")
    add_deps("culib")
target_end()
