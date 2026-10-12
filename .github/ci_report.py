#!/usr/bin/env python3
"""Fetch CI status and inspect test output. Standard library only; see docs/ci.md."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile


TITLE = "culib CI report"
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
TIMESTAMP = re.compile(r"^\d{4}-\d\d-\d\dT[\d:.]+Z\s+")
SUMMARY = re.compile(r"^\[\s*PASSED\s*\]|^\[PASS\]|^== summary:|"
                     r"^\[\s*\d+%\]:.*(?:passed|failed)|\d+% tests passed,|^Ran \d+ tests? in ")
SKIP = re.compile(r"\[\s*SKIP(?:PED)?\s*\]|-> SKIP\b")
DIAGNOSTIC = re.compile(
    r"\b(?:fatal|error|warning)(?: [A-Z]+\d+)?:|: Failure$|"
    r"^\s*(?:Expression|Values):|\[\s*FAIL(?:ED)?\s*\]|-> FAIL\b|"
    r"(?:Address|Leak|Thread|Memory)Sanitizer:|runtime error:|culib mem: leak(?: report)?:|"
    r"Segmentation fault|core dumped|^wavvix::|terminate called|^what\(\):|\bFAIL [1-9]\d*\b|"
    r"\b[1-9]\d* test\(s\) failed|\[\s*\d+%\]:.*\bfailed\b", re.IGNORECASE)
# setup-xmake builds xmake from source on Linux runners; warnings from its own tree are not ours.
XMAKE_SOURCE = re.compile(r"^core/src/")


def scan(raw):
    lines = [TIMESTAMP.sub("", ANSI.sub("", s)) for s in raw.splitlines()]
    found = {"summaries": [], "skips": [], "diagnostics": [], "lines": len(lines)}
    echoing = False
    current_test = None
    for number, line in enumerate(lines, 1):
        if line.startswith("##[group]Run "):
            echoing = True
        elif line.startswith("##[endgroup]"):
            echoing = False
        if line.startswith(("##[warning]", "##[error]")) and "culib-ci:" not in line:
            line = line.replace("##[warning]", "warning: ", 1).replace("##[error]", "error: ", 1)
        if echoing or line.startswith(("##[", "::", "culib-ci:")):
            continue
        line = re.sub(r"^(?:stdout|stderr):\s*", "", line).strip()
        if XMAKE_SOURCE.match(line):
            continue
        if re.match(r"\[\s*RUN\s*\]", line) and not re.search(r"-> (?:PASS|FAIL|SKIP)\b", line):
            current_test = "L{}: {}".format(number, line)
        elif re.match(r"\[\s*OK\s*\]", line) or SUMMARY.search(line):
            current_test = None
        for key, pattern in (("summaries", SUMMARY), ("skips", SKIP), ("diagnostics", DIAGNOSTIC)):
            if pattern.search(line):
                if key == "diagnostics" and current_test:
                    found[key].append(current_test)
                    current_test = None
                found[key].append("L{}: {}".format(number, line))
    return found


def excerpt(result):
    lines = []
    for key in ("summaries", "skips", "diagnostics"):
        entries = result[key]
        count = result.get(key + "_count", len(entries))
        if count:
            lines.append("{} ({}):".format(key, count))
            lines.extend(entries[:40])
            if count > 40:
                lines.append("... {} more; inspect full log".format(count - 40))
    return "\n".join(lines)


def publish(path, code, command):
    raw = Path(path).read_text(encoding="utf-8", errors="replace")
    result = scan(raw)
    report = dict(result, exit_code=code, command=command)
    for key in ("summaries", "skips", "diagnostics"):
        report[key + "_count"] = len(result[key])
        report[key] = [line[:240] for line in result[key][:40]]
    if code and not result["diagnostics"]:
        report["tail"] = [line[:240] for line in raw.splitlines()[-20:]]
    message = "culib-ci: " + json.dumps(report, ensure_ascii=False)
    message = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    level = "error" if code else "warning" if result["diagnostics"] else "notice"
    print("::{} title={}::{}".format(level, TITLE, message))
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as out:
            body = "{}\nexit={} / scanned={} lines\n{}".format(command, code, result["lines"], excerpt(result))
            out.write("\n### Command output\n\n" + "\n".join("    " + s for s in body.splitlines()) + "\n")
    return 1 if result["diagnostics"] and os.environ.get("CULIB_CI_STRICT") == "1" else 0


class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
        if urllib.parse.urlsplit(newurl).hostname != "api.github.com":
            redirected.remove_header("Authorization")
        return redirected


def api(path, raw=False, binary=False):
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "culib-ci-report"}
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request("https://api.github.com" + path, headers=headers)
    try:
        with urllib.request.build_opener(SafeRedirect()).open(request, timeout=30) as response:
            data = response.read()
            if binary:
                return data
            return data.decode("utf-8-sig", errors="replace") if raw else json.loads(data)
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")
        try:
            detail = json.loads(detail).get("message", detail)
        except ValueError:
            pass
        raise RuntimeError("HTTP {}: {}".format(error.code, detail[:200])) from None


def pages(path, key=None):
    items = []
    for page in range(1, 100):
        data = api(path + ("&" if "?" in path else "?") + "per_page=100&page=" + str(page))
        batch = data[key] if key else data
        items.extend(batch)
        if len(batch) < 100:
            return items
    raise RuntimeError("pagination limit reached")


def git(*args):
    return subprocess.check_output(["git"] + list(args), text=True, stderr=subprocess.PIPE).strip()


def use_git_credentials():
    if os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN"):
        return
    try:
        result = subprocess.run(["git", "credential", "fill"],
            input="protocol=https\nhost=github.com\n\n", text=True, capture_output=True,
            env=dict(os.environ, GIT_TERMINAL_PROMPT="0", GCM_INTERACTIVE="never"), timeout=10)
        fields = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
        if result.returncode == 0 and fields.get("password"):
            os.environ["GH_TOKEN"] = fields["password"]
    except (OSError, subprocess.SubprocessError):
        pass


def select_run(args):
    run, repo = args.run, args.repo
    if run and run.startswith("https://"):
        match = re.fullmatch(r"https://github.com/([^/]+/[^/]+)/actions/runs/(\d+)(?:/attempts/(\d+))?/?", run)
        if not match:
            raise RuntimeError("expected a GitHub Actions run URL or numeric run ID")
        repo, run, attempt = match.groups()
        if attempt:
            args.attempt = int(attempt)
    if not repo:
        match = re.search(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?$", git("remote", "get-url", "origin"))
        if not match:
            raise RuntimeError("specify --repo OWNER/REPO (origin is not on github.com)")
        repo = match.group(1)
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repo):
        raise RuntimeError("invalid --repo; expected OWNER/REPO")
    base = "/repos/" + repo
    if not run:
        sha = git("rev-parse", args.sha or "HEAD")
        query = urllib.parse.urlencode({"head_sha": sha, "per_page": 1})
        runs = api(base + "/actions/workflows/build.yml/runs?" + query)["workflow_runs"]
        if not runs:
            raise RuntimeError("no CI run for " + sha + "; push or specify a run ID")
        run = str(runs[0]["id"])
    if not run.isdigit():
        raise RuntimeError("run ID must be numeric")
    path = base + "/actions/runs/" + run
    return base, api(path + ("/attempts/" + str(args.attempt) if args.attempt else ""))


def archive_logs(path, output):
    data = api(path + "/logs", binary=True)
    logs = {}
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for entry in archive.infolist():
            match = re.fullmatch(r"\d+_([^/]+)\.txt", entry.filename)
            if match:
                name = match.group(1)
                # Sanitized job names can collide; those jobs use the ID endpoint.
                logs[name] = None if name in logs else archive.read(entry).decode("utf-8-sig", errors="replace")
    (output / "logs.zip").write_bytes(data)
    return logs


def inspect_job(base, job, output, full_logs, archived=None):
    record = dict(job, coverage="unavailable", reports=[], fetch_errors=[])
    if job["status"] == "queued":
        record["coverage"] = "pending (job has not started)"
        return record
    if full_logs and job["status"] == "completed":
        try:
            name = re.sub(r'[\\/:*?"<>|]', "_", job["name"])
            raw = archived.get(name) if archived else None
            if raw is None:
                raw = api(base + "/actions/jobs/{}/logs".format(job["id"]), raw=True)
            (output / (str(job["id"]) + ".log")).write_bytes(raw.encode("utf-8"))
            record.update(coverage="full log", reports=[scan(raw)])
            return record
        except (RuntimeError, OSError) as error:
            record["fetch_errors"].append(str(error))
    check = job["check_run_url"].rsplit("/", 1)[-1]
    try:
        notes = pages(base + "/check-runs/" + check + "/annotations")
        (output / (str(job["id"]) + ".annotations.json")).write_text(
            json.dumps(notes, ensure_ascii=False, indent=2), encoding="utf-8")
        for note in notes:
            if note.get("title") == TITLE and note["message"].startswith("culib-ci: "):
                record["reports"].append(json.loads(note["message"][len("culib-ci: "):]))
                record["coverage"] = "CI excerpts (not full log)"
            else:
                record["reports"].append(dict(scan(note["message"]), annotation=note["annotation_level"],
                                              message=note["message"]))
        if notes and record["coverage"] == "unavailable":
            record["coverage"] = "annotations only (no output report)"
    except (RuntimeError, OSError, ValueError) as error:
        record["fetch_errors"].append(str(error))
    return record


def query(args):
    base, run = select_run(args)
    jobs = pages(base + "/actions/runs/{}/attempts/{}/jobs".format(run["id"], run["run_attempt"]), "jobs")
    if args.job:
        jobs = [j for j in jobs if args.job.casefold() in j["name"].casefold()]
    if not jobs:
        raise RuntimeError("no matching jobs")
    completed = sum(j["status"] == "completed" for j in jobs)
    print("Inspecting {} jobs ({} completed); fetching logs and test output...".format(
          len(jobs), completed), flush=True)
    output = Path(args.output or "build/ci/{}-{}".format(run["id"], run["run_attempt"]))
    output.mkdir(parents=True, exist_ok=True)
    full_logs = bool(os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN"))
    archived = None
    archive_error = None
    if full_logs and run["status"] == "completed" and not args.job:
        try:
            archived = archive_logs(base + "/actions/runs/{}/attempts/{}".format(run["id"], run["run_attempt"]), output)
        except (RuntimeError, OSError, zipfile.BadZipFile) as error:
            archive_error = str(error)
    with ThreadPoolExecutor(max_workers=4) as pool:
        records = list(pool.map(lambda job: inspect_job(base, job, output, full_logs, archived), jobs))
    lines = [run["html_url"], "commit {} / attempt {} / {} / {}".format(
        run["head_sha"], run["run_attempt"], run["status"], run["conclusion"])]
    lines.append("Jobs: {}/{} completed, {} successful; full logs: {}/{}.".format(
        completed, len(jobs), sum(j["conclusion"] == "success" for j in jobs),
        sum(r["coverage"] == "full log" for r in records), len(jobs)))
    if not full_logs:
        lines.append("No GitHub credential available: full logs unavailable; reading public CI excerpts.")
    if archive_error:
        lines.append("Archive unavailable; used job endpoints: " + archive_error)
    bad = run["conclusion"] not in (None, "success")
    incomplete = run["status"] != "completed"
    for record in records:
        lines.append("\n[{}] {} | {}".format(record["conclusion"] or record["status"], record["name"], record["coverage"]))
        bad |= record["conclusion"] not in (None, "success", "skipped", "neutral")
        incomplete |= record["coverage"] != "full log" or record["status"] != "completed"
        for step in record.get("steps", []):
            if step.get("conclusion") in ("failure", "timed_out", "cancelled"):
                lines.append("  FAILED STEP: " + step["name"])
        for report in record["reports"]:
            if report.get("command"):
                lines.append("  $ {} (scanned {} lines)".format(report["command"], report["lines"]))
            if report.get("message"):
                lines.append("  {}: {}".format(report["annotation"].upper(), report["message"]))
            lines.extend("  " + line for line in excerpt(report).splitlines())
            lines.extend("  " + line for line in report.get("tail", []))
            bad |= bool(report["diagnostics"] or report.get("exit_code") or
                        report.get("annotation") in ("failure", "warning"))
        if not any(r["summaries"] for r in record["reports"]):
            lines.append("  No test summary found; test counts are unverified.")
            if any("test" in s["name"].lower() for s in record.get("steps", [])):
                incomplete = True
        lines.extend("  FETCH: " + error for error in record["fetch_errors"])
    code = 1 if bad else 2 if incomplete else 0
    result = []
    if bad:
        result.append("failure or diagnostics need review")
    if incomplete:
        result.append("incomplete log coverage / pending")
    lines.append("\nResult: {} (exit {}).".format("; ".join(result) or "completed; full logs scanned", code))
    report_text = "\n".join(lines) + "\n"
    (output / "report.txt").write_text(report_text, encoding="utf-8")
    (output / "report.json").write_text(json.dumps({"run": run, "jobs": records, "exit_code": code},
                                                   ensure_ascii=False, indent=2), encoding="utf-8")
    print(report_text + "\nSaved: " + str(output.resolve()))
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", nargs="?", help="run URL/ID; default: build.yml for current HEAD")
    parser.add_argument("--repo", help="OWNER/REPO; default: git origin")
    parser.add_argument("--sha", help="git revision; default: HEAD")
    parser.add_argument("--attempt", type=int, help="default: latest attempt")
    parser.add_argument("--job", help="job name substring (case-insensitive)")
    parser.add_argument("--output", help="default: build/ci/RUN-ATTEMPT")
    parser.add_argument("--publish", metavar="LOG", help="CI internal: publish command output")
    parser.add_argument("--exit-code", type=int, default=0)
    parser.add_argument("--command", default="")
    args = parser.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    try:
        if args.publish:
            return publish(args.publish, args.exit_code, args.command)
        use_git_credentials()
        return query(args)
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
        print("ci_report: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
