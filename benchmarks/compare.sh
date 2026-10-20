#!/usr/bin/env bash
# culib 和 Go 的记分板同场轮流跑：每一项各跑 ROUNDS 轮（默认 5），奇数轮 culib 先跑、偶数轮 Go 先跑，
# 取中位数。
#
#   bash benchmarks/compare.sh CULIB_SCOREBOARD GO_SCOREBOARD [项目...]
#
# 不给项目就跑全部。设了 PROCS 时同时作为 WAVVIX_MAXPROCS 和 GOMAXPROCS。
# ratio 是 culib 比 Go 快几倍：大于 1 是 culib 快（MB/s 越大越好，其余越小越好）。
# 某一轮失败时那一边的中位数后面带 *，只用成功的几轮。
set -u

culib=$1
go=$2
shift 2
rounds=${ROUNDS:-5}
if [ -n "${PROCS:-}" ]; then
    export WAVVIX_MAXPROCS=$PROCS GOMAXPROCS=$PROCS
fi

# 跑一项，打印数值；失败打印 x。
value_of() {
    local output
    if ! output=$("$1" "$2" 2>/dev/null); then
        echo x
        return
    fi
    echo "$output" | awk '{print $2}'
}

median() {
    tr ' ' '\n' | grep -E '^[0-9][0-9.e+-]*$' | sort -g |
        awk '{v[NR] = $1} END {
            if (NR == 0) { print "-" }
            else if (NR % 2) { print v[(NR + 1) / 2] }
            else { print (v[NR / 2] + v[NR / 2 + 1]) / 2 }
        }'
}

# 中位数，有失败的轮次时后面带 *。
summary() {
    local values=$1 middle
    middle=$(echo $values | median)
    case " $values " in
        *" x "*) echo "$middle*" ;;
        *) echo "$middle" ;;
    esac
}

if [ $# -gt 0 ]; then
    names="$*"
else
    names=$("$culib" | awk '{print $1}')
fi

printf "%-18s %11s %11s %7s  %s\n" case culib go ratio unit
for name in $names; do
    unit=$("$culib" | awk -v n="$name" '$1 == n {print $2}')
    a=""
    b=""
    for round in $(seq 1 "$rounds"); do
        if [ $((round % 2)) -eq 1 ]; then
            a="$a $(value_of "$culib" "$name")"
            b="$b $(value_of "$go" "$name")"
        else
            b="$b $(value_of "$go" "$name")"
            a="$a $(value_of "$culib" "$name")"
        fi
    done
    ma=$(summary "$a")
    mb=$(summary "$b")
    ratio=$(awk -v a="${ma%\*}" -v b="${mb%\*}" -v u="$unit" 'BEGIN {
        if (a == "-" || b == "-" || a + 0 == 0 || b + 0 == 0) { print "-"; exit }
        printf "%.2f", (u == "MB/s") ? a / b : b / a
    }')
    printf "%-18s %11s %11s %7s  %s\n" "$name" "$ma" "$mb" "$ratio" "$unit"
done
