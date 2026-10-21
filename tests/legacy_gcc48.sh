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
        *_win.cpp|*_macos.cpp) continue ;;
    esac
    g++ "${flags[@]}" -c "$source" -o "$build_dir/${source//\//_}.o"
done < <(find src -type f -name '*.cpp' | sort)
ar rcs "$build_dir/libculib.a" "$build_dir"/*.o

g++ "${flags[@]}" tests/*.cpp "$build_dir/libculib.a" -o "$build_dir/culib_test"
"$build_dir/culib_test"
