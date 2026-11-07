#!/bin/sh
# judge-dkms-log.sh - machine-judge a run-dkms-65.sh (or guest-test.sh) log.
#
# Rules (review/s2-release-design.md section 4.4):
#   * DKMS_RESULT must exist and be PASS
#   * no DKMS_STEP line with rc=[1-9]
#   * no DKMS_CHECK line containing " FAIL"
#   * no DKMS_ROLLBACK line (a rollback means the run did not pass)
#
# Usage: judge-dkms-log.sh <log>
# Output: DKMS_JUDGE PASS|FAIL plus one line per reason.
set -eu

if [ $# -ne 1 ] || [ ! -r "$1" ]; then
    printf 'usage: %s <log>\n' "$0" >&2
    exit 2
fi
LOG=$1
fail=0

result=$(grep -E '^DKMS_RESULT ' "$LOG" | tail -n 1 || true)
if [ -z "$result" ]; then
    printf 'DKMS_JUDGE_REASON missing DKMS_RESULT\n'
    fail=1
elif ! printf '%s\n' "$result" | grep -q '^DKMS_RESULT PASS'; then
    printf 'DKMS_JUDGE_REASON %s\n' "$result"
    fail=1
fi

bad_steps=$(grep -cE '^DKMS_STEP .* rc=[1-9]' "$LOG" || true)
if [ "${bad_steps:-0}" -ne 0 ]; then
    printf 'DKMS_JUDGE_REASON %s step(s) with non-zero rc\n' "$bad_steps"
    grep -E '^DKMS_STEP .* rc=[1-9]' "$LOG" | sed 's/^/  /'
    fail=1
fi

bad_checks=$(grep -cE '^DKMS_CHECK .* FAIL' "$LOG" || true)
if [ "${bad_checks:-0}" -ne 0 ]; then
    printf 'DKMS_JUDGE_REASON %s failed check(s)\n' "$bad_checks"
    grep -E '^DKMS_CHECK .* FAIL' "$LOG" | sed 's/^/  /'
    fail=1
fi

if grep -qE '^DKMS_ROLLBACK ' "$LOG"; then
    printf 'DKMS_JUDGE_REASON rollback happened\n'
    grep -E '^DKMS_ROLLBACK ' "$LOG" | sed 's/^/  /'
    fail=1
fi

steps=$(grep -cE '^DKMS_STEP ' "$LOG" || true)
checks=$(grep -cE '^DKMS_CHECK ' "$LOG" || true)
printf 'DKMS_JUDGE_SUMMARY steps=%s checks=%s failed_checks=%s\n' \
    "${steps:-0}" "${checks:-0}" "${bad_checks:-0}"

if [ "$fail" -eq 0 ]; then
    printf 'DKMS_JUDGE PASS\n'
    exit 0
fi
printf 'DKMS_JUDGE FAIL\n'
exit 1
