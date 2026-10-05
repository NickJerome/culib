#!/usr/bin/env bash
#
# 跑一条命令；失败时把它的完整输出发成 GitHub 的 annotation。
#
#     bash .github/report-failure.sh xmake build -w -y -g examples
#
# 为什么需要这个：job 日志要仓库 admin 权限才能下载，而 run: 步骤里编译器的原始警告和
# 错误**不会**自动变成 annotation。所以不这样做的话，一次 Windows 失败从公开 API 那边
# 看只有一句「Process completed with exit code 1」，连哪个文件哪一行都不知道。
#
# 真实代价：examples/os.cpp 里一个 std::fopen 在 MSVC 上是 C4996（弃用，开了警告即错误
# 就编不过），MinGW 没有这个弃用所以本机交叉编译是绿的。定位它花了三个来回，全花在
# 「看不见编译器说了什么」上面——两次来回只换来「失败的是 build examples 这一步」。
#
# 每个会跑编译器或测试的步骤都该经过这里，不是只有 build library 那一个：第一版只包了
# 那一步，而真正红的是 build examples，于是机制在最需要它的那次没起作用。
set -o pipefail

log=$(mktemp)
if "$@" 2>&1 | tee "$log"; then exit 0; fi

echo "::group::失败命令的完整输出：$*"
cat "$log"
echo "::endgroup::"

# annotation 有数量上限，而且太长的单行会被截断，所以只挑诊断行。
#
# 匹配的是「诊断」而不是光一个 error 字样：GCC / Clang 写 `error:` / `warning:`，
# MSVC 写 `error C2039:` / `warning C4996:`。光匹配 error 会把目标名也捞进来——
# 本仓库就有个 example_error_context，`grep -i error` 对它永远有输出，于是「有没有
# 报错」这个判断整个失效。失败与否只看退出码，这里只负责挑出要给人看的行。
#
# **测试失败的输出也要挑出来**，而原先漏了。第一版只认编译器的格式，所以一次测试失败
# 在公开 API 那边只剩 xmake 自己那句 `error: <no error object>`——连哪条测试、断言的两
# 边各是什么都看不到。macOS 那两格就是这么红了好几个提交没人知道原因的。
# 本库测试框架失败时打的是：
#
#     <文件>:<行>: Failure
#       Expression: 1 + 1 == 3
#       Values:     2 == 3
#     [  FAILED  ] demo.deliberately_fails (17 us)
#
# 这几个式样都够具体，不会像光匹配 error 那样把目标名捞进来。
grep -nE "error:|warning:|error [A-Z]+[0-9]+|warning [A-Z]+[0-9]+|: Failure$|^ +(Expression|Values): |^\[  FAILED  \]" "$log" |
    head -40 | while IFS= read -r line; do
        echo "::error::$line"
    done

exit 1
