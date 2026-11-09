#!/bin/sh
# matrix.sh - the 0.3.0 adaptive-FEC acceptance matrix.
#
#   0/1/3/5/10% x 32/64/256 carriers x fixed/adaptive x 3 repeats
#
# Runs drop_ppm first, then netem, with the mode order flipped on every
# repeat (fixed,adaptive / adaptive,fixed / fixed,adaptive) so a drift in the
# path cannot favour one mode. Each phase writes its own runs.csv; the merged
# matrix.csv, medians.csv, runs.jsonl, verdict.txt and summary.json land in
# the parent directory.
#
#   benches/adaptive/matrix.sh --near root@192.168.123.85 \
#       --far root@45.11.1.62 --near-ip 192.168.123.85 --far-ip 45.11.1.62
#
#   --inject both|ppm|netem   which injectors to run (default both)
#   --repeats 3               per cell
#   --quick                   loss 0,5 only, 1 repeat (smoke, not acceptance)
#   --cleanup                 tc qdiscs, drop_ppm reset, tunnel 1, ip rule
#
# Wall clock: one run is WARMUP + DURATION + join/config (~90 s at the
# defaults). 90 runs per injector is roughly 2.5 h; both is roughly 5 h.
# Use --quick before committing to the full grid.
#
# Success criteria S1..S12 are evaluated in verdict.txt from the measured
# rows; the README states which ones are hard gates.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SCRIPT=matrix
. "$root/benches/adaptive/lib.sh"

REPEATS=${TBT_REPEATS:-3}
LOSS_LIST=${TBT_LOSS:-0,1,3,5,10}
CARRIER_LIST=${TBT_CARRIERS:-32,64,256}
MODE_LIST=${TBT_MODES:-fixed,adaptive}
INJECT=both
QUICK=0

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    usage_common
    cat <<'EOF'
matrix options:
  --inject both|ppm|netem   injectors to run (default both)
  --repeats 3               repeats per cell
  --quick                   loss 0,5 and 1 repeat (smoke)
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
    --inject) INJECT=$2; shift 2 ;;
    --quick) QUICK=1; shift ;;
    *) die "unknown option $1 (try --help)" ;;
    esac
done

case $INJECT in
both | ppm | netem) ;;
*) die "--inject must be both|ppm|netem" ;;
esac

if [ "$QUICK" = 1 ]; then
    REPEATS=1
    LOSS_LIST=0,5
fi

if [ -z "$OUT" ]; then
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    OUT=$root/benches/results/$stamp-adaptive-matrix${TAG:+-$TAG}
fi
matrix_out=$OUT

if [ "$CLEANUP" = 1 ]; then
    CLEAN_PPM=1
    CLEAN_NETEM=1
    CLEAN_TUNNEL=1
    do_cleanup
    exit 0
fi

if [ "$DRY" = 1 ]; then
    say "dry-run: matrix inject=$INJECT repeats=$REPEATS loss=$LOSS_LIST carriers=$CARRIER_LIST"
    [ "$INJECT" = netem ] || { OUT=$matrix_out/drop_ppm; dry_run_plan ppm; }
    [ "$INJECT" = ppm ] || { OUT=$matrix_out/netem; dry_run_plan netem; }
    OUT=$matrix_out
    say "  merged output: $matrix_out/{matrix.csv,medians.csv,runs.jsonl,verdict.txt,summary.json}"
    exit 0
fi

prepare_near
preflight_hosts
[ "$INJECT" = netem ] || preflight_ppm
[ "$INJECT" = ppm ] || preflight_netem
install_trap

rc=0
sources=

if [ "$INJECT" != netem ]; then
    OUT=$matrix_out/drop_ppm
    mkdir -p "$OUT"
    if run_grid ppm; then
        aggregate_to "$OUT/runs.csv" "$OUT/runs.csv"
        sources="$sources $OUT/runs.csv"
    else
        rc=1
        aggregate_to "$OUT/runs.csv" "$OUT/runs.csv" || true
        sources="$sources $OUT/runs.csv"
    fi
fi

if [ "$INJECT" != ppm ] && [ "$rc" -eq 0 ]; then
    OUT=$matrix_out/netem
    mkdir -p "$OUT"
    if run_grid netem; then
        aggregate_to "$OUT/runs.csv" "$OUT/runs.csv"
        sources="$sources $OUT/runs.csv"
    else
        rc=1
        aggregate_to "$OUT/runs.csv" "$OUT/runs.csv" || true
        sources="$sources $OUT/runs.csv"
    fi
fi

OUT=$matrix_out
mkdir -p "$OUT"
if [ -n "$sources" ]; then
    aggregate_to "$OUT/matrix.csv" $sources
fi

if [ "$rc" -eq 0 ]; then
    say "matrix: PASS (bytes and run rows) -> $OUT"
else
    say "matrix: FAIL (see $OUT/verdict.txt)" >&2
fi
exit "$rc"
