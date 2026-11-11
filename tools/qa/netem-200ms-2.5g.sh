#!/usr/bin/env bash
# netem-200ms-2.5g.sh - 0.7.0 acceptance at RTT 200 ms and 2.5 Gbit/s.
#
# WHAT IT PROVES
#   `tc qdisc replace dev <if> root netem delay 100ms` on both ends gives the
#   tunnel a 200 ms RTT. One 256-carrier SC-LDPC session (far TX 1x256, near
#   TX GF16 8x32) carries a single unidirectional download for 60 s:
#
#       312.5 MB/s x 60 s = 18,750,000,000 B
#       = 2.5 Gbit/s, 1.220 MB/s of payload per carrier
#
#   The exact byte volume makes the sender's and receiver's SHA-256 cover the
#   same range. The run only downloads (the 2.5G port definition is one
#   direction); --dirs both turns the upload on as well.
#
# PASS
#   * SHA-256 and bytes agree at both ends and equal the requested volume;
#   * aggregate app throughput >= 312.5 MB/s;
#   * per-carrier median >= 90% of 1.220 MB/s and >= 98% of carriers moved
#     bytes;
#   * `ss -ti` reports cwnd/pacing_rate for >= 95% of the carriers per end;
#   * c3 up stays 256 on both ends.
#
# USAGE
#   tools/qa/netem-200ms-2.5g.sh --run
#   tools/qa/netem-200ms-2.5g.sh --dry-run
#   tools/qa/netem-200ms-2.5g.sh --cleanup   # qdisc, clients, target, tunnel
#
#   --rtt MS            round trip (default 200; netem delay is RTT/2 per end)
#   --seconds S         byte window (default 60)
#   --target-mbps M     decimal MB/s aggregate (default 312.5 -> 312500000 B/s)
#   --dirs dl|both      traffic directions (default dl)
#   --loss P            netem loss per end (default 0%)
#   --min-carrier-pct / --min-active-pct / --min-ss-pct
#   --near/--far/--near-dev/--far-dev/--near-ip/--far-ip/--near-port/
#   --far-listen/--target-port/--carriers/--tunnel/--evidence/--expect-version
#
# PRECONDITIONS
#   Both ends run the 0.7.0 tbtun; root ssh; tc, ss, python3, sha256sum.
#   Root netem is installed on both carrier interfaces (a foreign non-default
#   root qdisc is refused); --cleanup removes it. SSH is delayed by the
#   netem, so --ssh-timeout defaults to 60 s.
#
# OUTPUT
#   <repo>/<stamp>-netem-200ms-2.5g/
#     carriers.csv ss-ti.csv netem-runs.csv netem-verdict.txt
#   stdout: CHECK lines, NETEM_RESULT PASS|FAIL
# Exit: 0 PASS, 2 usage/preflight, 3 setup, 4 checks, 130 interrupt.
set -uo pipefail

SELF=$(cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib-070.sh
. "$SELF/lib-070.sh"

SCRIPT=netem-200ms-2.5g
MODE=run
RTT_MS=${RTT_MS:-200}
SECONDS_RUN=${SECONDS_RUN:-60}
TARGET_BPS=${TARGET_BPS:-312500000}
DIRS=${DIRS:-dl}
LOSS=${LOSS:-0%}
SSH_TMO=${SSH_TMO:-60}
MARK_NEAR=/tmp/tbt-070-$SCRIPT-near.mark
MARK_FAR=/tmp/tbt-070-$SCRIPT-far.mark

usage() {
    sed -n '2,54p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

while [ $# -gt 0 ]; do
    case $1 in
    -h | --help) usage ;;
    --run) MODE=run; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    --cleanup) MODE=cleanup; shift ;;
    --rtt) RTT_MS=$2; shift 2 ;;
    --seconds) SECONDS_RUN=$2; shift 2 ;;
    --target-mbps) TARGET_BPS=$(awk -v m="$2" 'BEGIN { printf "%.0f", m * 1000000 }'); shift 2 ;;
    --dirs) DIRS=$2; shift 2 ;;
    --loss) LOSS=$2; shift 2 ;;
    --min-carrier-pct) MIN_CARRIER_PCT=$2; shift 2 ;;
    --min-active-pct) MIN_ACTIVE_PCT=$2; shift 2 ;;
    --min-ss-pct) MIN_SS_PCT=$2; shift 2 ;;
    *)
        if q070_common_arg "$1" "${2:-}"; then
            shift "$SHIFT_BY"
        else
            q070_die "unknown option $1 (try --help)"
        fi
        ;;
    esac
done

q070_num "$RTT_MS" || q070_die "--rtt must be a number"
q070_num "$SECONDS_RUN" || q070_die "--seconds must be a number"
q070_num "$TARGET_BPS" || q070_die "--target-mbps must be a number"
[ "$RTT_MS" -ge 2 ] || q070_die "--rtt must be at least 2 ms"
[ "$SECONDS_RUN" -ge 30 ] || q070_die "--seconds must be at least 30"
[ "$TARGET_BPS" -ge 1000000 ] || q070_die "--target-mbps must be positive"
case $DIRS in both | dl) ;; *) q070_die "--dirs must be both or dl" ;; esac
case $LOSS in *% | [0-9]*) ;; *) q070_die "--loss must be a percentage" ;; esac

on_exit() {
    local rc=$?
    if [ "$MODE" = run ] && [ "${EV:-}" != "" ]; then
        q070_netem_abort "$SCRIPT" "$MARK_NEAR" "$MARK_FAR"
    fi
    trap - EXIT
    exit "$rc"
}

case $MODE in
cleanup)
    q070_netem_cleanup "$SCRIPT" "$MARK_NEAR" "$MARK_FAR"
    q070_say "netem-200ms-2.5g: cleanup done"
    exit 0
    ;;
esac

if [ "$DRY_RUN" = 1 ]; then
    q070_netem_plan "$SCRIPT" "$RTT_MS" "$SECONDS_RUN" "$TARGET_BPS" "$DIRS" "$LOSS"
    exit 0
fi

q070_evidence_init "$SCRIPT" >/dev/null || q070_die "cannot create evidence dir"
trap on_exit EXIT INT TERM
FAILED=0
q070_netem_case "$SCRIPT" "$RTT_MS" "$SECONDS_RUN" "$TARGET_BPS" "$DIRS" \
    "$LOSS" "$MARK_NEAR" "$MARK_FAR"
