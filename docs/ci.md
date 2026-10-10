# 检查 CI 输出

在仓库根目录运行，要求 Python 3.8+，无需第三方包：

```sh
python .github/ci_report.py                         # 当前 HEAD 对应的 build.yml
python .github/ci_report.py 38025351576              # 指定运行 ID，也可传完整运行 URL
python .github/ci_report.py --sha origin/master      # 指定提交
python .github/ci_report.py 38025351576 --job "allocator L2"
```

脚本读取指定运行的最新 attempt；用 `--attempt N` 检查重跑之前的结果。
默认从 origin 识别 GitHub 仓库，也可显式传 `--repo NickJerome/culib`。
按 job 列出状态、失败步骤、测试汇总、跳过原因和异常诊断，保留日志中的行号。
不把不同编译配置的用例数累加成一个容易误导的“总测试数”。

结果保存到 `build/ci/<run>-<attempt>/report.txt` 和 `report.json`；取得的完整 job 日志
或原始 annotations 也保存在同一目录。`--output PATH` 可改目录。
整轮已结束且有日志权限时，一次下载 `logs.zip` 后逐 job 扫描；未结束、筛选 job 或整包不可用时，
改用各 job 的日志接口。压缩包只在内存中读取，不解压执行其中的内容。
有失败或可疑输出时，先看具体断言、跳过原因与上下文，再报告结论。

## 日志权限与覆盖范围

脚本优先使用 `GH_TOKEN` 或 `GITHUB_TOKEN`；未设置时，通过 `git credential fill`
复用已有的 GitHub 登录，禁用交互提示。有该仓库 Actions 读取权限时，下载并扫描
每个 job 的完整日志。令牌只留在当前进程环境中，不打印、不写进报告；日志下载
重定向时不向其他域名转发认证头。2026-10-10 实测匿名下载 job 日志返回 HTTP 403，
本机现有的 GitHub Git 凭据能够读取完整日志。

没有令牌或下载失败时，脚本读取公开的 check-run annotations。
CI 的 `.github/report-failure.sh` 在命令成功和失败时都会扫描 stdout/stderr，
发布测试汇总、SKIP 行、编译诊断、断言、allocator fatal 与 sanitizer 报告，
同时写入 Actions 的步骤摘要。每类最多公开 40 行，每行最多 240 字符，保留真实条数；
失败但没有匹配诊断时附末尾 20 行。完整输出仍在 Actions 日志中。

这份摘要是按已知输出格式提取的，不代表人工读过所有日志，也不覆盖未经过包装脚本的步骤。
旧运行没有成功输出摘要时，明确显示不可获取。job 为 success 不会掩盖 `fatal` 等诊断；
安全注入的预期崩溃也需要核对测试上下文，不能一律忽略这些关键字。

退出码：

| 值 | 含义 |
| --- | --- |
| 0 | 所选 jobs 已完成、成功，完整日志已扫描，未匹配异常；测试 job 找到汇总 |
| 1 | CI 失败，或发现需要审查的诊断（即使 CI 是绿色） |
| 2 | CI 未完成、读取失败，或只有摘要，无法完整核验日志 |

查询不自动轮询。整包模式通常只需 3–4 次 API 请求；按 job 下载或匿名读取 annotations 时，
每个 job 还需一次请求。匿名 GitHub API 通常限制每小时 60 次。不要把退出码 2 当作测试失败或测试通过。

## 本地验证报告脚本

```sh
python -m unittest discover -s tests -p ci_report_test.py
bash .github/report-failure.sh xmake test -v
```

包装脚本保留被执行命令的非零退出码；报告生成失败也会让原本成功的命令返回失败，
避免 CI 在没有发布输出时悄悄通过。脚本测试随 `public headers` job 执行。

ASan/UBSan job 设置 `CULIB_CI_STRICT=1`：即使测试进程返回 0，扫描到诊断也让该步骤失败。
这会拦住 sanitizer 自身降级检测的警告，不能只靠 `halt_on_error=1`。
