#!/usr/bin/env bash
# Capture and publish command output on success and failure; see docs/ci.md.
set -o pipefail

log=$(mktemp)
trap 'rm -f -- "$log"' EXIT
"$@" 2>&1 | tee "$log"
status=$?

python3 "$(dirname "$0")/ci_report.py" --publish "$log" --exit-code "$status" --command "$*"
report_status=$?
if [ "$report_status" -ne 0 ]; then
    echo "::error::CI output reporting failed (exit $report_status)"
    if [ "$status" -eq 0 ]; then status=$report_status; fi
fi
exit "$status"
