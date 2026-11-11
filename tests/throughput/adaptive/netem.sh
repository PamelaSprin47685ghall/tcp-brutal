#!/bin/sh
# netem.sh - adaptive-FEC acceptance with tc netem loss on the carrier path.
#
# Applies egress loss on far or near (or both) with tc. The default keeps the
# loss inside the carrier 5-tuple:
#
#   near egress: dport 9000 (carriers dial far:9000)
#   far egress:  sport 9000 (carrier replies)
#
# via a prio root qdisc and a u32 filter, so the ssh session and every other
# flow on eth0 stay clean. --simple falls back to `root netem` (ssh included).
#
#   tests/throughput/adaptive/netem.sh --near root@192.168.123.85 \
#       --far root@45.11.1.62 --near-ip 192.168.123.85 --far-ip 45.11.1.62 \
#       --carriers 32,64,256 --loss 0,1,3,5,10 --mode fixed,adaptive
#
#   --burst    add netem's 25% loss correlation (S11)
#   --simple   root netem instead of prio+u32 (ssh is in the lossy band)
#   --netem-where near|far|both   (default far: the download direction)
#   --cleanup  delete only qdiscs this harness added, delete tunnel 1,
#              remove the ip rule this harness added
#
# A per-apply watchdog removes the qdisc if this script dies with ssh gone.
# The marker carries a token, so a stale watchdog never deletes a newer run's
# qdisc.
#
# TCP hides part of the loss: p_est (c13 p_ppm) is expected to sit below the
# configured netem loss. Use drop_ppm.sh for estimator accuracy and this script
# for end-to-end goodput and byte integrity.
#
# Output: tests/throughput/results/<UTC>-adaptive-netem[-tag]/ (same files as drop_ppm.sh)
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SCRIPT=netem
. "$root/tests/throughput/adaptive/lib.sh"

usage() {
    sed -n '2,35p' "$0" | sed 's/^# \{0,1\}//'
    usage_common
    cat <<'EOF'
netem options:
  --netem-where near|far|both   where loss is applied (default far)
  --burst                       loss with 25% correlation (S11)
  --simple                      root netem; ssh shares the loss
EOF
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
    --netem-where) NETEM_WHERE=$2; shift 2 ;;
    --burst) NETEM_BURST=1; shift ;;
    --simple) NETEM_SIMPLE=1; shift ;;
    *) die "unknown option $1 (try --help)" ;;
    esac
done

case $NETEM_WHERE in
near | far | both) ;;
*) die "--netem-where must be near|far|both" ;;
esac

if [ -z "$OUT" ]; then
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    OUT=$root/tests/throughput/results/$stamp-adaptive-netem${TAG:+-$TAG}
fi

if [ "$CLEANUP" = 1 ]; then
    CLEAN_NETEM=1
    CLEAN_TUNNEL=1
    do_cleanup
    exit 0
fi

if [ "$DRY" = 1 ]; then
    dry_run_plan netem
    exit 0
fi

prepare_near
preflight_hosts
preflight_netem
install_trap

rc=0
run_grid netem || rc=1
aggregate_to "$OUT/runs.csv" "$OUT/runs.csv"

if [ "$rc" -eq 0 ]; then
    say "netem: PASS (bytes and run rows) -> $OUT"
else
    say "netem: FAIL (see $OUT/verdict.txt)" >&2
fi
exit "$rc"
