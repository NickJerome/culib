#!/usr/bin/env bash
# 不经 xmake，直接用 MinGW-w64 编库和测试并运行。CI 的 MinGW 任务在 MSYS2 里跑它；
# 本机交叉编译时 CXX 取 x86_64-w64-mingw32-g++，再用 wine 跑。
set -euo pipefail

compiler="${CXX:-x86_64-w64-mingw32-g++}"
output="build/mingw-x86_64-standalone"
mkdir -p "$output"

flags=(-std=c++11 -Wall -Wextra -Wpedantic -Werror
       -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Iinclude -Isrc -Isrc/testing)

library_objects=()
while IFS= read -r source; do
    case "$source" in
        *_posix.cpp|*_linux.cpp|*_macos.cpp|src/override/*) continue ;;
    esac
    object="$output/${source//\//_}.o"
    "$compiler" "${flags[@]}" -c "$source" -o "$object"
    library_objects+=("$object")
done < <(find src -type f -name '*.cpp' | sort)

context_object="$output/context.S.o"
"$compiler" -c src/runtime/asm/context_x86_64_pe.S -o "$context_object"
library_objects+=("$context_object")

test_objects=()
for source in tests/*.cpp; do
    object="$output/${source//\//_}.o"
    "$compiler" "${flags[@]}" -c "$source" -o "$object"
    test_objects+=("$object")
done

"$compiler" -static -o "$output/culib_test.exe" "${library_objects[@]}" "${test_objects[@]}" -lws2_32
"$output/culib_test.exe"

"$compiler" "${flags[@]}" -static -o "$output/coroutine_faults_test.exe" \
    tests/fault/coroutine_faults.cpp "${library_objects[@]}" -lws2_32
"$output/coroutine_faults_test.exe"

"$compiler" "${flags[@]}" -static -o "$output/flag_test.exe" \
    tests/flag/flag_test.cpp "${library_objects[@]}" -lws2_32
"$output/flag_test.exe"

"$compiler" "${flags[@]}" -static -o "$output/new_override_test.exe" \
    tests/override/*.cpp tests/main.cpp src/override/new.cpp "${library_objects[@]}" -lws2_32
"$output/new_override_test.exe"
