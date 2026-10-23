#!/usr/bin/env bash
# serve 的冒烟测试：起一次服务，用 curl 取文件、比对内容，再停掉。CI 在三个平台上都跑。
#   bash apps/serve/smoke_test.sh [serve 的路径]，不写时在 build 下找
set -euo pipefail

serve=${1:-}
if [ -z "$serve" ]; then
    for candidate in $(find build -type f \( -name serve -o -name serve.exe \)); do
        serve=$candidate
        break
    done
fi
if [ -z "$serve" ]; then
    echo "smoke_test: serve is not built" >&2
    exit 1
fi

dir=$(mktemp -d)
trap 'rm -rf -- "$dir"' EXIT
printf '<h1>hello from serve</h1>\n' > "$dir/index.html"

# 输出写进文件：不占着调用方的管道，serve 停不掉时调用方也能结束。路径单独作一个参数，Windows 的 Git Bash
# 才一定会把它换成 Windows 路径
"$serve" --address=127.0.0.1:0 --file "$dir/index.html" > "$dir/serve.out" 2>&1 &
pid=$!

# 端口由系统挑，从 serve 打的那一行里取；Windows 上行尾带 \r，只取到空白为止
address=
for _ in $(seq 1 100); do
    address=$(sed -n 's/^serve: listening on \([^[:space:]]*\).*/\1/p' "$dir/serve.out")
    if [ -n "$address" ] || ! kill -0 "$pid" 2> /dev/null; then
        break
    fi
    sleep 0.1
done
if [ -z "$address" ]; then
    echo "smoke_test: serve did not start; its output:" >&2
    cat "$dir/serve.out" >&2
    exit 1
fi

status=0
curl --silent --show-error --fail --max-time 10 --output "$dir/got.html" "http://$address/" || status=$?
if [ "$status" -eq 0 ] && ! cmp "$dir/index.html" "$dir/got.html"; then
    status=1
fi

kill "$pid" 2> /dev/null || true
wait "$pid" 2> /dev/null || true
if [ -s "$dir/serve.out" ]; then
    echo "serve printed:"
    cat "$dir/serve.out"
fi
if [ "$status" -ne 0 ]; then
    echo "smoke_test: fetching http://$address/ failed" >&2
    exit "$status"
fi
echo "smoke_test: fetched http://$address/ and stopped serve"
