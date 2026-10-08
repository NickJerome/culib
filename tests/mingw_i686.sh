#!/usr/bin/env bash
set -euo pipefail

compiler="${CXX:-i686-w64-mingw32-g++}"
output="build/mingw-i686-standalone"
mkdir -p "$output"

flags=(-std=c++11 -Wall -Wextra -Wpedantic -Werror
       -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Iinclude -Isrc)

for source in src/*.cpp tests/*.cpp; do
    case "$source" in
        *_posix.cpp|*_linux.cpp|*_macos.cpp) continue ;;
    esac
    object="$output/${source//\//_}.o"
    "$compiler" "${flags[@]}" -c "$source" -o "$object"
done

"$compiler" -c src/asm/context_i386_pe.S -o "$output/context_i386_pe.S.o"
"$compiler" -static -o "$output/culib_test.exe" "$output"/*.o \
    -lws2_32 -lbcrypt -liphlpapi
"$output/culib_test.exe"
