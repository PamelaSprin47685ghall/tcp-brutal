#!/usr/bin/env bash
# netem-400ms.sh - 0.7.0 acceptance at RTT 400 ms with 0.5% loss.
#
# WHAT IT PROVES
#   Both ends put `tc qdisc replace dev <if> root netem delay 200ms loss 0.5%`
#   on the carrier interface, so the tunnel sees a 400 ms RTT. One 256-carrier
#   SC-LDPC session (far TX 1x256, near TX GF16 8x32) then carries one
#   download and one upload for 120 s, each moving
#
#       256 carriers x 0.5 MB/s x 120 s = 15,360,000,000 B
#
#   (0.5 MB/s of application payload per carrier, per direction). The byte
#   volume is exact, so the sender and receiver hash the same range; a slow
#   link fails on the transfer deadline rather than producing a short hash.
#
# MEASURED
#   * app bytes/s from the near client, wire bytes/s from the sender's
#     /proc/net/tbtun/peers tx_bytes over the same interval, per-carrier
#     tx_bytes/rate (256 rows per direction);
#   * one mid-run `ss -ti` sample on each end: cwnd, pacing_rate and
#     delivery_rate for every carrier socket;
#   * SHA-256 and byte count on both ends, per direction.
#
# PASS
#   * SHA-256 and bytes agree per direction (and equal the requested volume);
#   * app throughput >= --target-mbps (default 128 MB/s per direction);
#   * per-carrier median >= 90% of 0.5 MB/s and >= 98% of carriers moved
#     bytes (MIN_CARRIER_PCT/MIN_ACTIVE_PCT override);
#   * `ss -ti` reports cwnd and pacing_rate for >= 95% of the carriers on
#     each end;
#   * c3 up stays 256 on both ends.
#
# USAGE
#   scripts/qa/netem-400ms.sh --run
#   scripts/qa/netem-400ms.sh --dry-run
#   scripts/qa/netem-400ms.sh --cleanup    # qdisc, clients, target, tunnel
#
#   --rtt MS            round trip (default 400; netem delay is RTT/2 per end)
#   --seconds S         per-direction byte window (default 120)
#   --target-mbps M     decimal MB/s per direction (default 128)
#   --dirs both|dl      traffic directions (default both)
#   --loss P            netem loss per end (default 0.5%)
#   --min-carrier-pct / --min-active-pct / --min-ss-pct
#   --near/--far/--near-dev/--far-dev/--near-ip/--far-ip/--near-port/
#   --far-listen/--target-port/--carriers/--tunnel/--evidence/--expect-version
#
# PRECONDITIONS
#   Both ends run the 0.7.0 tbtun and accept the config in lib-070.sh. The
#   script replaces each carrier interface's root qdisc with netem (that is
#   the point of the test) and refuses a foreign non-default root; --cleanup
#   deletes it, leaving the kernel default. SSH becomes RTT-delayed too, so
#   --ssh-timeout defaults to 60 s here. Root ssh on both ends.
#
# OUTPUT
#   <repo>/review/wan-experiment/arm64/qa-070-<stamp>-netem-400ms/
#     carriers.csv ss-ti.csv netem-runs.csv netem-verdict.txt
#   stdout: CHECK lines, NETEM_RESULT PASS|FAIL
# Exit: 0 PASS, 2 usage/preflight, 3 setup, 4 checks, 130 interrupt.
set -uo pipefail

SELF=$(cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib-070.sh
. "$SELF/lib-070.sh"

SCRIPT=netem-400ms
MODE=run
RTT_MS=${RTT_MS:-400}
SECONDS_RUN=${SECONDS_RUN:-120}
TARGET_MBPS=${TARGET_MBPS:-128}
DIRS=${DIRS:-both}
LOSS=${LOSS:-0.5%}
SSH_TMO=${SSH_TMO:-60}
MARK_NEAR=/tmp/tbt-070-$SCRIPT-near.mark
MARK_FAR=/tmp/tbt-070-$SCRIPT-far.mark
CLEANED=0
TARGET_BPS=0

usage() {
    sed -n '2,55p' "$0" | sed 's/^# \{0,1\}//'
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
    --target-mbps) TARGET_MBPS=$2; shift 2 ;;
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
q070_num "$TARGET_MBPS" || q070_die "--target-mbps must be a number"
[ "$RTT_MS" -ge 2 ] || q070_die "--rtt must be at least 2 ms"
[ "$SECONDS_RUN" -ge 30 ] || q070_die "--seconds must be at least 30"
[ "$TARGET_MBPS" -ge 1 ] || q070_die "--target-mbps must be positive"
case $DIRS in both | dl) ;; *) q070_die "--dirs must be both or dl" ;; esac
case $LOSS in *% | [0-9]*) ;; *) q070_die "--loss must be a percentage" ;; esac
TARGET_BPS=$((TARGET_MBPS * 1000000))

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
    q070_say "netem-400ms: cleanup done"
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
