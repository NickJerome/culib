#!/usr/bin/env bash
set -euo pipefail

compiler="${CXX:-x86_64-w64-mingw32-g++}"
output="build/mingw-x86_64-standalone"
mkdir -p "$output"

flags=(-std=c++11 -Wall -Wextra -Wpedantic -Werror
       -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Iinclude -Isrc -Isrc/testing -Isrc/cli)

library_objects=()
while IFS= read -r source; do
    case "$source" in
        *_posix.cpp|*_linux.cpp|*_macos.cpp|src/mem/override/*) continue ;;
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

"$compiler" -static -o "$output/culib_test.exe" \
    "${library_objects[@]}" "${test_objects[@]}" \
    -lws2_32

"$output/culib_test.exe"

"$compiler" "${flags[@]}" -static -o "$output/os_stop_test.exe" \
    tests/process/os_stop.cpp "${library_objects[@]}" -lws2_32
"$output/os_stop_test.exe" threads
"$output/os_stop_test.exe" ctrl-c
"$output/os_stop_test.exe" ctrl-break

"$compiler" "${flags[@]}" -Isrc/mem/allocator -static \
    tests/mem/test_stress.cpp "${library_objects[@]}" -lws2_32 \
    -o "$output/mem_test_stress.exe"
"$output/mem_test_stress.exe"

for source in examples/*.cpp; do
    example="$output/example_$(basename "$source" .cpp).exe"
    "$compiler" "${flags[@]}" -static -o "$example" \
        "$source" "${library_objects[@]}" \
        -lws2_32
done
