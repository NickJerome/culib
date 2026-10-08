set_project("culib_mesh")
set_xmakever("2.8.5")
add_rules("mode.debug", "mode.release")

add_requires("libsodium 1.0.22")
includes("../..")

target("mesh_crypto")
    set_kind("static")
    set_default(false)
    if not is_plat("windows") then
        set_languages("c++11")
    end
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_files("src/crypto.cpp", "src/traffic.cpp")
    add_includedirs("include", {public = true})
    add_packages("libsodium", {public = true})
    add_deps("culib")
target_end()

target("mesh_crypto_test")
    set_kind("binary")
    set_default(false)
    if not is_plat("windows") then
        set_languages("c++11")
    end
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_files("tests/crypto_test.cpp")
    add_deps("mesh_crypto")
    add_tests("default")
target_end()

target("mesh_traffic_test")
    set_kind("binary")
    set_default(false)
    if not is_plat("windows") then
        set_languages("c++11")
    end
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_files("tests/traffic_test.cpp")
    add_deps("mesh_crypto")
    add_tests("default")
target_end()
