#!/usr/bin/env bash
set -euo pipefail

case "$(g++ -dumpversion)" in
    4.8*) ;;
    *) echo 'expected GCC 4.8' >&2; exit 1 ;;
esac

build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT

while IFS= read -r source; do
    case "$source" in
        *_win.cpp|*_macos.cpp) continue ;;
    esac
    g++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -pthread \
        -Iinclude -Isrc -Isrc/testing -Isrc/cli -c "$source" \
        -o "$build_dir/${source//\//_}.o"
done < <(find src mem/src -type f -name '*.cpp' | sort)
g++ -c src/runtime/asm/context_x86_64_sysv.S -o "$build_dir/context.o"
ar rcs "$build_dir/libculib.a" "$build_dir"/*.o

g++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -pthread \
    -Iinclude -Isrc -Isrc/testing -Isrc/cli tests/*.cpp "$build_dir/libculib.a" \
    -ldl -lrt -o "$build_dir/culib_test"
"$build_dir/culib_test"

for source in examples/*.cpp; do
    g++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -pthread \
        -Iinclude "$source" "$build_dir/libculib.a" -ldl -lrt \
        -o "$build_dir/$(basename "$source" .cpp)"
done
