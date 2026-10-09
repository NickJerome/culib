#!/usr/bin/env bash
set -euo pipefail

arch="${1:-}"
case "$arch" in
    i686)
        default_compiler=i686-w64-mingw32-g++
        context=src/runtime/asm/context_i386_pe.S
        ;;
    x86_64)
        default_compiler=x86_64-w64-mingw32-g++
        context=src/runtime/asm/context_x86_64_pe.S
        ;;
    *)
        echo "usage: bash tests/mingw_windows.sh i686|x86_64" >&2
        exit 2
        ;;
esac

compiler="${CXX:-$default_compiler}"
output="build/mingw-$arch-standalone"
mkdir -p "$output"

flags=(-std=c++11 -Wall -Wextra -Wpedantic -Werror
       -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Iinclude -Isrc -Isrc/testing -Isrc/cli)

library_objects=()
while IFS= read -r source; do
    case "$source" in
        *_posix.cpp|*_linux.cpp|*_macos.cpp|src/mem/override/*) continue ;;
        src/mem/allocator/*) [[ "$arch" == x86_64 ]] || continue ;;
    esac
    object="$output/${source//\//_}.o"
    "$compiler" "${flags[@]}" -c "$source" -o "$object"
    library_objects+=("$object")
done < <(find src -type f -name '*.cpp' | sort)

context_object="$output/context.S.o"
"$compiler" -c "$context" -o "$context_object"
library_objects+=("$context_object")

test_objects=()
for source in tests/*.cpp; do
    object="$output/${source//\//_}.o"
    "$compiler" "${flags[@]}" -c "$source" -o "$object"
    test_objects+=("$object")
done

"$compiler" -static -o "$output/culib_test.exe" \
    "${library_objects[@]}" "${test_objects[@]}" \
    -lws2_32

"$output/culib_test.exe"

for source in examples/*.cpp; do
    example="$output/example_$(basename "$source" .cpp).exe"
    "$compiler" "${flags[@]}" -static -o "$example" \
        "$source" "${library_objects[@]}" \
        -lws2_32
done
