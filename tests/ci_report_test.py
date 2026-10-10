"""CI reporting must not equate a successful job with clean, available output."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace


spec = importlib.util.spec_from_file_location("ci_report", Path(__file__).resolve().parents[1] / ".github/ci_report.py")
ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci)


class ReportTests(unittest.TestCase):
    def test_success_summaries_and_skips(self):
        result = ci.scan("2026-10-10T12:00:00.123Z \x1b[32m[  PASSED  ] 366 tests.\x1b[0m\n"
                         "[SKIP] overflow_canary: disabled\n"
                         "== summary: 10 cases | PASS 9 | SKIP 1 | FAIL 0 ==\n"
                         "100% tests passed, 0 test(s) failed out of 11\n")
        self.assertEqual(len(result["summaries"]), 3)
        self.assertEqual(len(result["skips"]), 1)
        self.assertEqual(result["diagnostics"], [])

    def test_errors_are_not_hidden_by_success(self):
        for message in ("stderr: culib mem: fatal: double free detected ptr=0x1",
                        "ERROR: AddressSanitizer: heap-use-after-free", "x.cpp:3: Failure",
                        "[  FAILED  ] scheduler.join", "error C2039: broken",
                        "runtime error: signed integer overflow",
                        "== summary: 10 cases | PASS 9 | SKIP 0 | FAIL 1 =="):
            with self.subTest(message=message):
                self.assertTrue(ci.scan(message + "\n[PASS] test_security")['diagnostics'])

    def test_echoed_workflow_source_is_not_test_output(self):
        result = ci.scan("##[group]Run echo 'fatal: example'\necho 'fatal: example'\n"
                         "##[endgroup]\n[  PASSED  ] 3 tests.\n"
                         "##[notice]culib-ci: {\"diagnostics\": []}\n")
        self.assertEqual(len(result["summaries"]), 1)
        self.assertFalse(result["diagnostics"])

    def test_scheduler_crash_includes_the_running_test(self):
        result = ci.scan("[ RUN      ] scheduler.remote_join\n\n"
                         "stderr: wavvix::scheduler: no wakeup source\n")
        self.assertEqual(len(result["diagnostics"]), 2)
        self.assertIn("scheduler.remote_join", result["diagnostics"][0])

    def test_runner_annotations_remain_visible_in_full_logs(self):
        self.assertTrue(ci.scan("##[warning]Node.js 20 is deprecated")['diagnostics'])

    def test_published_output_escapes_workflow_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.log"
            path.write_text("100% tests passed, 0 test(s) failed out of 2\n[SKIP] reason\n", encoding="utf-8")
            out = io.StringIO()
            with patch.dict(ci.os.environ, {}, clear=True), contextlib.redirect_stdout(out):
                ci.publish(path, 0, "xmake test")
            wire = out.getvalue().strip()
            self.assertIn("::notice title=culib CI report::", wire)
            self.assertIn("100%25 tests passed", wire)
            payload = json.loads(wire.split("culib-ci: ", 1)[1].replace("%25", "%"))
            self.assertEqual(payload["skips_count"], 1)

    def query_fixture(self, log=None, notes=None, conclusion="success"):
        run = dict(id=123, run_attempt=2, head_sha="abc", status="completed",
                   conclusion=conclusion, html_url="https://github.com/a/b/actions/runs/123")
        job = dict(id=456, name="Linux", status="completed", conclusion="success",
                   check_run_url="https://api.github.com/repos/a/b/check-runs/789",
                   steps=[dict(name="run tests", conclusion="success")])

        def api(path, raw=False):
            if "/attempts/2/jobs?" in path:
                return {"jobs": [job]}
            if path.endswith("/logs"):
                if isinstance(log, Exception):
                    raise log
                return log
            if "/annotations?" in path:
                return notes or []
            raise AssertionError("unexpected request " + path)

        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(job=None, output=directory)
            with patch.object(ci, "select_run", return_value=("/repos/a/b", run)), \
                    patch.object(ci, "api", side_effect=api), \
                    patch.dict(ci.os.environ, {"GH_TOKEN": "test-token"} if log is not None else {}, clear=True), \
                    contextlib.redirect_stdout(io.StringIO()):
                code = ci.query(args)
            saved = json.loads((Path(directory) / "report.json").read_text(encoding="utf-8"))
            return code, saved

    def test_full_log_success_and_success_with_fatal(self):
        self.assertEqual(self.query_fixture(log="[  PASSED  ] 3 tests.")[0], 0)
        self.assertEqual(self.query_fixture(log="[  PASSED  ] 3 tests.\nculib mem: fatal: invalid free")[0], 1)

    def test_no_logs_or_no_test_summary_never_claims_verified(self):
        self.assertEqual(self.query_fixture()[0], 2)
        self.assertEqual(self.query_fixture(log="Build completed")[0], 2)
        self.assertEqual(self.query_fixture(conclusion="failure")[0], 1)

    def test_forbidden_logs_fall_back_to_excerpts(self):
        report = dict(ci.scan("[  PASSED  ] 3 tests."), command="xmake test", exit_code=0)
        note = dict(title=ci.TITLE, message="culib-ci: " + json.dumps(report), annotation_level="notice")
        code, saved = self.query_fixture(log=RuntimeError("HTTP 403: forbidden"), notes=[note])
        self.assertEqual(code, 2)
        self.assertEqual(saved["jobs"][0]["coverage"], "CI excerpts (not full log)")
        self.assertTrue(saved["jobs"][0]["reports"][0]["summaries"])

    def test_pagination_reads_all_annotations(self):
        with patch.object(ci, "api", side_effect=[[{}] * 100, [{}] * 2]) as api:
            self.assertEqual(len(ci.pages("/annotations")), 102)
            self.assertTrue(api.call_args.args[0].endswith("page=2"))


if __name__ == "__main__":
    unittest.main()
