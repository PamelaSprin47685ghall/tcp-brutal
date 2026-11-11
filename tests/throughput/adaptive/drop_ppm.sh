#!/bin/sh
# drop_ppm.sh - adaptive-FEC acceptance with FEC-record loss injection.
#
# Writes `carrier <i> drop_ppm <ppm>` for every carrier on the injection end
# (default: far, the download sender). tbt_fec_data.c drops the record before
# it reaches the socket, so TCP never retransmits it: only parity/NACK can
# repair the share. That isolates the estimator and the control law from TCP.
#
#   tests/throughput/adaptive/drop_ppm.sh --near root@192.168.123.85 \
#       --far root@45.11.1.62 --near-ip 192.168.123.85 --far-ip 45.11.1.62 \
#       --carriers 256 --loss 0,1,3,5,10 --mode fixed,adaptive --repeats 1
#
# Each run: rewrite tunnel 1 on both ends (adaptive=on|off), wait for up=N,
# inject, transfer for WARMUP + DURATION seconds, sample at the warm-up
# boundary and at the end, then verify the received body against the far
# source prefix with sha256.
#
# Preconditions:
#   * both ends run the 0.3.0 build (adaptive= and, for p_est, c13);
#   * the far module was loaded with debug_loss=1;
#   * the far source file exists (see --source) and far's raw sender listens
#     on 127.0.0.1:5210;
#   * near reaches far on eth0 (the idempotent pref 100 ip rule is added).
#
# Options beyond the common set (--help prints them):
#   --inject-end near|far|both   where drop_ppm is written (default far)
#   --kill-at SEC --kill-carrier N   S12: carrier down mid-window
#   --cleanup                    reset drop_ppm, delete tunnel 1, remove the
#                                ip rule this harness added
#
# Output: tests/throughput/results/<UTC>-adaptive-drop_ppm[-tag]/
#   runs.csv runs.jsonl medians.csv verdict.txt summary.json run.log runs/<name>/
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SCRIPT=drop_ppm
. "$root/tests/throughput/adaptive/lib.sh"

usage() {
    sed -n '2,35p' "$0" | sed 's/^# \{0,1\}//'
    usage_common
}

while [ $# -gt 0 ]; do
    case $1 in
    -h | --help) usage; exit 0 ;;
    esac
    if common_arg "$1" "${2:-}"; then
        shift "$SHIFT_BY"
        continue
    fi
    case $1 in
    --inject-end) INJECT_ENDS=$2; shift 2 ;;
    *) die "unknown option $1 (try --help)" ;;
    esac
done

case $INJECT_ENDS in
near | far | both) ;;
*) die "--inject-end must be near|far|both" ;;
esac

if [ -z "$OUT" ]; then
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    OUT=$root/tests/throughput/results/$stamp-adaptive-drop_ppm${TAG:+-$TAG}
fi

if [ "$CLEANUP" = 1 ]; then
    CLEAN_PPM=1
    CLEAN_TUNNEL=1
    do_cleanup
    exit 0
fi

if [ "$DRY" = 1 ]; then
    dry_run_plan ppm
    exit 0
fi

prepare_near
preflight_hosts
preflight_ppm
install_trap

rc=0
run_grid ppm || rc=1
aggregate_to "$OUT/runs.csv" "$OUT/runs.csv"

if [ "$rc" -eq 0 ]; then
    say "drop_ppm: PASS (bytes and run rows) -> $OUT"
else
    say "drop_ppm: FAIL (see $OUT/verdict.txt)" >&2
fi
exit "$rc"
