#!/usr/bin/env bash
# 用 GCC 4.8 编库和测试并运行。CI 在官方 gcc:4.8.5 镜像里跑它（Debian 7，glibc 2.13）。
set -euo pipefail

case "$(g++ -dumpversion)" in
    4.8*) ;;
    *) echo 'expected GCC 4.8' >&2; exit 1 ;;
esac

build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT

flags=(-std=c++11 -Wall -Wextra -Wpedantic -Werror -pthread -Iinclude -Isrc -Isrc/testing)

while IFS= read -r source; do
    case "$source" in
        *_win.cpp|*_macos.cpp|src/override/*) continue ;;
    esac
    g++ "${flags[@]}" -c "$source" -o "$build_dir/${source//\//_}.o"
done < <(find src -type f -name '*.cpp' | sort)
g++ -c src/runtime/asm/context_x86_64_sysv.S -o "$build_dir/context.o"
ar rcs "$build_dir/libculib.a" "$build_dir"/*.o

g++ "${flags[@]}" tests/*.cpp "$build_dir/libculib.a" -o "$build_dir/culib_test"
"$build_dir/culib_test"

g++ "${flags[@]}" tests/fault/coroutine_faults.cpp "$build_dir/libculib.a" -o "$build_dir/coroutine_faults_test"
"$build_dir/coroutine_faults_test"

g++ "${flags[@]}" tests/flag/flag_test.cpp "$build_dir/libculib.a" -o "$build_dir/flag_test"
"$build_dir/flag_test"

g++ "${flags[@]}" -c src/override/new.cpp -o "$build_dir/new_override.o"
g++ "${flags[@]}" tests/override/*.cpp tests/main.cpp "$build_dir/new_override.o" "$build_dir/libculib.a" \
    -o "$build_dir/new_override_test"
"$build_dir/new_override_test"
