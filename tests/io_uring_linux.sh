#!/usr/bin/env bash
# 构建并运行 file_uring_test（Linux only，见 tests/process/io_uring_faults.cpp）。
#
# 用法: tests/io_uring_linux.sh [编译器] [native|faults|all]   （默认 g++ all）
#
# required-native 语义：
#   - `native` 模式缺 native 直接非零退出（CI 门禁，不许悄悄 skip）；
#   - `faults` 模式的 setup 故障在任何 Linux 内核上都必须通过；enter 探测错误/
#     消费/回滚三条在缺 native 时打印 [SKIP ...] 跳过，native 覆盖由 native 模式把守。
#
# 库对象缓存在 /var/tmp/culib-file-uring-objs-<compiler>。任一 include/src 下的
# 头文件比对象新就重编全部对象（轻量全量失效，不做逐头依赖跟踪）——私有头里的
# ABI（ring 布局、request 布局）变了绝不能混链旧对象。本脚本只编 x86_64 汇编；
# arm64 或其他架构的 CI 构建必须走 xmake 的目标（file_uring_test），不要用本脚本。

set -u
cd "$(dirname "$0")/.."

CC=${1:-g++}
MODE=${2:-all}
OBJ=/var/tmp/culib-file-uring-objs-$(printf '%s' "$CC" | tr '+/' 'p_')
mkdir -p "$OBJ" || exit 1

FLAGS="-std=c++11 -O2 -g -pthread -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc -Isrc/testing"

# 任一头文件比上次构建新，就废弃整个对象缓存：私有头里的 ABI（ring/request 布局）
# 变了绝不能混链旧对象。轻量全量失效，不做逐头依赖跟踪。
STAMP="$OBJ/.headers-stamp"
if [ ! -f "$STAMP" ] || [ -n "$(find include src -name '*.h' -newer "$STAMP" -print -quit)" ]; then
    rm -f "$OBJ"/*.o
fi

SOURCES=$(find src -name '*.cpp' ! -name '*_win.cpp' ! -name '*_macos.cpp' ! -path '*override*')
for s in $SOURCES; do
    o="$OBJ/$(printf '%s' "${s#src/}" | tr '/' '_').o"
    if [ ! -f "$o" ] || [ "$s" -nt "$o" ]; then
        "$CC" $FLAGS -c "$s" -o "$o" || exit 1
    fi
done

"$CC" -c src/runtime/asm/context_x86_64_sysv.S -o "$OBJ/context.o" || exit 1
touch "$STAMP"

EXE="$OBJ/file_uring_test"
"$CC" $FLAGS tests/process/io_uring_faults.cpp "$OBJ"/*.o -Wl,--wrap=syscall -o "$EXE" || exit 1

run() {
    echo "== file_uring_test $1"
    "$EXE" "$1" || exit 1
}

case "$MODE" in
    native) run native ;;
    faults) run faults ;;
    all)    run faults
            run native ;;
    *) printf 'usage: %s [compiler] [native|faults|all]\n' "$0" >&2
       exit 2 ;;
esac

echo "file_uring_test: ok"
