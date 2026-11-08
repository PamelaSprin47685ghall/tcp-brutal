#!/bin/sh
# hnd0_ab.sh - reproducible same-batch SC-LDPC A/B benchmark on HND0.
#
# What it does, in one batch:
#   1. copies the baseline source tree and an optional candidate tree to
#      root@45.11.1.62:/tmp/sclc-bench/src/<variant>/
#   2. builds both there with the same C99/SSE2 flags (no AVX)
#   3. records lscpu, gcc, load, memory and the running services
#   4. runs the requested points with 2000 trials x 4 code instances,
#      symmetric channel, taskset + nice, alternating baseline/candidate
#      order each round, same seed for both variants within a round
#   5. writes raw runs, per-metric median/min/max, and paired A/B CSVs
#
# Without --cand, a second identical copy of the baseline is built and run
# as "ctl"; base-vs-ctl then measures the noise floor of the harness.
#
# Examples:
#   ./hnd0_ab.sh --tag base-20261106
#   ./hnd0_ab.sh --tag ab-simd --cand ~/tbtun-worktrees/sclc-simd/review/fec-prototype/sc_ldpc
#   ./hnd0_ab.sh --rounds 5 --threads 1 --points "0.50:0.25"
set -eu

HOST=${HOST:-root@45.11.1.62}
REMOTE=${REMOTE:-/tmp/sclc-bench}
CTL_SOCK=${CTL_SOCK:-/tmp/sclc-bench-ssh-%r@%h:%p}
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=15 -o ServerAliveCountMax=8 -o ControlMaster=auto -o ControlPath=$CTL_SOCK -o ControlPersist=600"
SSH="ssh $SSH_OPTS"
SCP="scp -q $SSH_OPTS"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BASE_SRC=${BASE_SRC:-$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)}
CAND_SRC=${CAND_SRC:-}
CAND_NAME=${CAND_NAME:-cand}
ROUNDS=${ROUNDS:-7}
TRIALS=${TRIALS:-2000}
CODES=${CODES:-4}
BLOCK=${BLOCK:-256}
L=${L:-16}
THREADS_LIST=${THREADS_LIST:-"1 2"}
POINTS=${POINTS:-"0.30:0.50 0.40:0.40 0.50:0.30 0.50:0.25"}
SEED_BASE=${SEED_BASE:-1}
TAG=${TAG:-$(date -u +%Y%m%dT%H%M%SZ)}
OUT=${OUT:-}
ORIG_ARGS=$*

usage() {
    awk 'NR >= 2 && NR <= 28' "$0"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --base) BASE_SRC=$2; shift 2 ;;
        --cand) CAND_SRC=$2; shift 2 ;;
        --cand-name) CAND_NAME=$2; shift 2 ;;
        --tag) TAG=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --rounds) ROUNDS=$2; shift 2 ;;
        --trials) TRIALS=$2; shift 2 ;;
        --codes) CODES=$2; shift 2 ;;
        --block) BLOCK=$2; shift 2 ;;
        --L) L=$2; shift 2 ;;
        --threads) THREADS_LIST=$2; shift 2 ;;
        --points) POINTS=$2; shift 2 ;;
        --seed) SEED_BASE=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ -z "$CAND_SRC" ]; then
    CAND_SRC=$BASE_SRC
    CAND_NAME=ctl
    echo "note: no --cand, running base against an identical control build" >&2
fi

if [ -z "$OUT" ]; then
    OUT=$SCRIPT_DIR/results/$TAG
fi

for v in base "$CAND_NAME"; do
    case "$v" in
        *[!A-Za-z0-9_-]*|'') echo "bad variant name: $v" >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT"

# --- provenance -----------------------------------------------------------
{
    echo "tag=$TAG"
    echo "date_utc=$(date -u +%FT%TZ)"
    echo "host=$HOST"
    echo "remote=$REMOTE"
    echo "local_args=$ORIG_ARGS"
    echo "rounds=$ROUNDS trials=$TRIALS codes=$CODES block=$BLOCK L=$L"
    echo "threads=$THREADS_LIST points=$POINTS seed_base=$SEED_BASE"
    echo "base_src=$BASE_SRC"
    echo "cand_src=$CAND_SRC"
    echo "cand_name=$CAND_NAME"
    for v in base "$CAND_NAME"; do
        if [ "$v" = base ]; then d=$BASE_SRC; else d=$CAND_SRC; fi
        echo "## $v commit=$(git -C "$d" rev-parse HEAD 2>/dev/null || echo none)"
        echo "## $v dirty=$(git -C "$d" status --porcelain -- . 2>/dev/null | wc -l) entries"
        (cd "$d" && sha256sum *.c *.h build.sh 2>/dev/null | awk -v p="$v" '{ print p, $0 }')
    done
} > "$OUT/provenance.txt"

# --- copy sources ---------------------------------------------------------
sync_src() {
    src=$1
    v=$2
    tar -C "$src" -cf "$OUT/src-$v.tar" \
        --exclude='./bench' --exclude='./results' --exclude='./.git' \
        --exclude='*_test.c' --exclude='./sc_ldpc_bench' .
    $SCP "$OUT/src-$v.tar" "$HOST:$REMOTE/batch/src-$v.tar"
    $SSH "$HOST" "rm -rf '$REMOTE/src/$v' && mkdir -p '$REMOTE/src/$v' && tar -C '$REMOTE/src/$v' -xf '$REMOTE/batch/src-$v.tar'"
}

$SSH "$HOST" "mkdir -p '$REMOTE/src' '$REMOTE/bin' '$REMOTE/log' '$REMOTE/batch'"
sync_src "$BASE_SRC" base
sync_src "$CAND_SRC" "$CAND_NAME"

# --- build ----------------------------------------------------------------
$SCP "$SCRIPT_DIR/remote-build.sh" "$HOST:$REMOTE/batch/remote-build.sh"
if ! $SSH "$HOST" "sh '$REMOTE/batch/remote-build.sh'" > "$OUT/build.log" 2>&1; then
    echo "build failed; see $OUT/build.log" >&2
    cat "$OUT/build.log" >&2
    exit 1
fi
cat "$OUT/build.log"

# --- environment ----------------------------------------------------------
$SSH "$HOST" '
    echo "## date"; date -u
    echo "## hostname"; hostname
    echo "## uname"; uname -a
    echo "## lscpu"; lscpu
    echo "## cpuinfo_flags"; grep -m1 "^flags" /proc/cpuinfo
    echo "## gcc"; gcc --version; gcc -dumpmachine
    echo "## gcc_target"; gcc -Q --help=target 2>/dev/null | grep -E "march=|mtune="
    echo "## loadavg"; cat /proc/loadavg
    echo "## free"; free -m
    echo "## df_tmp"; df -h /tmp
    echo "## top_cpu"; ps -eo pid,comm,pcpu --sort=-pcpu | head -8
    echo "## running_services"; systemctl list-units --type=service --state=running --no-pager --no-legend | head -20
' > "$OUT/env.txt" 2>&1

# --- run ------------------------------------------------------------------
{
    echo "BATCH=$TAG"
    echo "ROUNDS=$ROUNDS"
    echo "TRIALS=$TRIALS"
    echo "CODES=$CODES"
    echo "BLOCK=$BLOCK"
    echo "L=$L"
    echo "THREADS_LIST=\"$THREADS_LIST\""
    echo "POINTS=\"$POINTS\""
    echo "SEED_BASE=$SEED_BASE"
    echo "V1=base"
    echo "V2=$CAND_NAME"
} > "$OUT/run.conf"

$SCP "$SCRIPT_DIR/remote-run.sh" "$HOST:$REMOTE/batch/remote-run.sh"
$SCP "$OUT/run.conf" "$HOST:$REMOTE/batch/run.conf"

echo "running batch $TAG: rounds=$ROUNDS points=[$POINTS] threads=[$THREADS_LIST]"
if ! $SSH "$HOST" "sh '$REMOTE/batch/remote-run.sh'" > "$OUT/runs-$TAG.csv" 2> "$OUT/run-$TAG.err"; then
    echo "benchmark failed; see $OUT/run-$TAG.err" >&2
    cat "$OUT/run-$TAG.err" >&2
    exit 1
fi
$SCP "$HOST:$REMOTE/log/$TAG.err" "$OUT/bench-stderr-$TAG.log" 2>/dev/null || true

# --- summarize ------------------------------------------------------------
awk -v cand="$CAND_NAME" -f "$SCRIPT_DIR/summarize.awk" \
    "$OUT/runs-$TAG.csv" > "$OUT/summary-$TAG.txt"

rm -f "$OUT/metrics-$TAG.csv" "$OUT/summary-wide-$TAG.csv" "$OUT/ab-$TAG.csv"
awk -v dir="$OUT" -v tag="$TAG" '
    /^$/ { sec++; next }
    {
        f = dir "/" (sec == 0 ? "metrics" : sec == 1 ? "summary-wide" : "ab") "-" tag ".csv"
        print >> f
    }
' "$OUT/summary-$TAG.txt"

{
    echo "local: $0 $ORIG_ARGS"
    echo "remote build: sh $REMOTE/batch/remote-build.sh"
    echo "remote run:   sh $REMOTE/batch/remote-run.sh"
    echo "remote conf:  $REMOTE/batch/run.conf"
    echo "summarize:    awk -v cand=$CAND_NAME -f summarize.awk runs-$TAG.csv"
} > "$OUT/commands.txt"

echo "--- wide summary ---"
cat "$OUT/summary-wide-$TAG.csv"
echo "--- paired A/B ---"
cat "$OUT/ab-$TAG.csv"
echo "files:"
ls -1 "$OUT"

$SSH -O exit "$HOST" 2>/dev/null || true
