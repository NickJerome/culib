#!/usr/bin/env bash
set -euo pipefail

build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT

for source in src/*.cpp; do
    case "$source" in
        *_win.cpp|*_macos.cpp) continue ;;
    esac
    g++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -pthread \
        -Iinclude -Isrc -c "$source" \
        -o "$build_dir/$(basename "$source" .cpp).o"
done
g++ -c src/asm/context_x86_64_sysv.S -o "$build_dir/context.o"
ar rcs "$build_dir/libculib.a" "$build_dir"/*.o

g++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -pthread \
    -Iinclude -Isrc tests/*.cpp "$build_dir/libculib.a" \
    -ldl -lrt -o "$build_dir/culib_test"
"$build_dir/culib_test"

for source in examples/*.cpp; do
    g++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -pthread \
        -Iinclude "$source" "$build_dir/libculib.a" -ldl -lrt \
        -o "$build_dir/$(basename "$source" .cpp)"
done
