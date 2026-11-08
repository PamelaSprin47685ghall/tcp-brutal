#!/bin/sh
# hnd0_bs_ab.sh - same-batch A/B of the scalar baseline against the
# bit-sliced candidate on HND0, built from source on the target.
#
# Defaults:
#   base  = this worktree's sc_ldpc (ef5f956 scalar)
#   cand  = ~/tbtun-worktrees/sclc-simd/review/fec-prototype/sc_ldpc
#   points = the four margin points plus the three capacity points
#   trials = 2048 (bs_bench floors trials/batch; 2048 = 4 x 512)
#
# Phases:
#   t1b512  base vs cand@512, 1 thread, 5 rounds
#   t1b1024 cand@512 vs cand@1024, 1 thread, 3 rounds
#   t2b512  base vs cand@512, 2 threads, 3 rounds
#
# Example:
#   ./hnd0_bs_ab.sh --tag bs-ab-20261106
#   ./hnd0_bs_ab.sh --tag bs-ab-quick --rounds 3 --rounds-b2 1 --rounds-2t 1 \
#       --points "256:16:0.50:0.30"
set -eu

HOST=${HOST:-root@45.11.1.62}
REMOTE=${REMOTE:-/tmp/sclc-bench}
CTL_SOCK=${CTL_SOCK:-/tmp/sclc-bench-ssh-%r@%h:%p}
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=15 -o ServerAliveCountMax=8 -o ControlMaster=auto -o ControlPath=$CTL_SOCK -o ControlPersist=600"
SSH="ssh $SSH_OPTS"
SCP="scp -q $SSH_OPTS"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BASE_SRC=${BASE_SRC:-$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)}
CAND_SRC=${CAND_SRC:-$HOME/tbtun-worktrees/sclc-simd/review/fec-prototype/sc_ldpc}
ROUNDS=${ROUNDS:-5}
ROUNDS_B2=${ROUNDS_B2:-3}
ROUNDS_2T=${ROUNDS_2T:-3}
TRIALS=${TRIALS:-2048}
CODES=${CODES:-4}
BATCH=${BATCH:-512}
BATCH2=${BATCH2:-1024}
SEED_BASE=${SEED_BASE:-1}
POINTS=${POINTS:-"256:16:0.50:0.30 256:16:0.40:0.40 256:16:0.30:0.50 256:16:0.25:0.50 256:16:0.60:0.30 256:16:0.70:0.20 512:16:0.50:0.30"}
TAG=${TAG:-$(date -u +%Y%m%dT%H%M%SZ)}
OUT=${OUT:-}
ORIG_ARGS=$*

usage() {
    awk 'NR >= 2 && NR <= 26' "$0"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --base) BASE_SRC=$2; shift 2 ;;
        --cand) CAND_SRC=$2; shift 2 ;;
        --tag) TAG=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --rounds) ROUNDS=$2; shift 2 ;;
        --rounds-b2) ROUNDS_B2=$2; shift 2 ;;
        --rounds-2t) ROUNDS_2T=$2; shift 2 ;;
        --trials) TRIALS=$2; shift 2 ;;
        --codes) CODES=$2; shift 2 ;;
        --batch) BATCH=$2; shift 2 ;;
        --batch2) BATCH2=$2; shift 2 ;;
        --points) POINTS=$2; shift 2 ;;
        --seed) SEED_BASE=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ -z "$OUT" ]; then
    OUT=$SCRIPT_DIR/results/$TAG
fi
mkdir -p "$OUT"

{
    echo "tag=$TAG"
    echo "date_utc=$(date -u +%FT%TZ)"
    echo "host=$HOST"
    echo "remote=$REMOTE"
    echo "local_args=$ORIG_ARGS"
    echo "rounds=$ROUNDS rounds_b2=$ROUNDS_B2 rounds_2t=$ROUNDS_2T"
    echo "trials=$TRIALS codes=$CODES batch=$BATCH batch2=$BATCH2"
    echo "seed_base=$SEED_BASE points=$POINTS"
    echo "base_src=$BASE_SRC"
    echo "cand_src=$CAND_SRC"
    for pair in "base:$BASE_SRC" "cand:$CAND_SRC"; do
        v=${pair%%:*}
        d=${pair#*:}
        echo "## $v commit=$(git -C "$d" rev-parse HEAD 2>/dev/null || echo none)"
        echo "## $v dirty=$(git -C "$d" status --porcelain -- . 2>/dev/null | wc -l) entries"
        (cd "$d" && sha256sum sc_ldpc.c sc_ldpc.h sc_ldpc_bench.c sc_ldpc_bs.c sc_ldpc_bs.h sc_ldpc_bs_bench.c sc_ldpc_bs_test.c build.sh 2>/dev/null | awk -v p="$v" '{ print p, $0 }')
    done
} > "$OUT/provenance.txt"

sync_src() {
    src=$1
    v=$2
    tar -C "$src" -cf "$OUT/src-$v.tar" \
        --exclude='./bench' --exclude='./results' --exclude='./.git' \
        --exclude='*.csv' --exclude='./sc_ldpc_bench' \
        --exclude='./sc_ldpc_bs_bench' --exclude='./sc_ldpc_bs_test' \
        --exclude='./hnd0-bs-run.sh' .
    $SCP "$OUT/src-$v.tar" "$HOST:$REMOTE/batch/src-$v.tar"
    $SSH "$HOST" "rm -rf '$REMOTE/src/$v' && mkdir -p '$REMOTE/src/$v' && tar -C '$REMOTE/src/$v' -xf '$REMOTE/batch/src-$v.tar'"
}

$SSH "$HOST" "mkdir -p '$REMOTE/src' '$REMOTE/bin' '$REMOTE/log' '$REMOTE/batch'"
sync_src "$BASE_SRC" base
sync_src "$CAND_SRC" cand

$SCP "$SCRIPT_DIR/remote-build-bs.sh" "$HOST:$REMOTE/batch/remote-build-bs.sh"
if ! $SSH "$HOST" "TAG='$TAG' sh '$REMOTE/batch/remote-build-bs.sh'" > "$OUT/build.log" 2>&1; then
    echo "build failed; see $OUT/build.log" >&2
    cat "$OUT/build.log" >&2
    exit 1
fi
cat "$OUT/build.log"

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

{
    echo "TAG=$TAG"
    echo "ROUNDS=$ROUNDS"
    echo "ROUNDS_B2=$ROUNDS_B2"
    echo "ROUNDS_2T=$ROUNDS_2T"
    echo "TRIALS=$TRIALS"
    echo "CODES=$CODES"
    echo "POINTS=\"$POINTS\""
    echo "BATCH=$BATCH"
    echo "BATCH2=$BATCH2"
    echo "SEED_BASE=$SEED_BASE"
} > "$OUT/run-bs.conf"

$SCP "$SCRIPT_DIR/remote-run-bs.sh" "$HOST:$REMOTE/batch/remote-run-bs.sh"
$SCP "$OUT/run-bs.conf" "$HOST:$REMOTE/batch/run-bs.conf"

echo "running $TAG: rounds=$ROUNDS b2=$ROUNDS_B2 2t=$ROUNDS_2T points=[$POINTS]"
if ! $SSH "$HOST" "sh '$REMOTE/batch/remote-run-bs.sh'" > "$OUT/runs-bs-$TAG.csv" 2> "$OUT/run-bs-$TAG.err"; then
    echo "benchmark failed; see $OUT/run-bs-$TAG.err" >&2
    cat "$OUT/run-bs-$TAG.err" >&2
    exit 1
fi
$SCP "$HOST:$REMOTE/log/$TAG.err" "$OUT/bench-stderr-$TAG.log" 2>/dev/null || true
$SCP "$HOST:$REMOTE/log/$TAG-bstest.log" "$OUT/bstest-$TAG.log" 2>/dev/null || true

python3 "$SCRIPT_DIR/summarize_bs.py" "$OUT/runs-bs-$TAG.csv" "$OUT" "$TAG" \
    > "$OUT/summary-bs-$TAG.txt"
cat "$OUT/summary-bs-$TAG.txt"

{
    echo "local: $0 $ORIG_ARGS"
    echo "remote build: TAG=$TAG sh $REMOTE/batch/remote-build-bs.sh"
    echo "remote run:   sh $REMOTE/batch/remote-run-bs.sh"
    echo "remote conf:  $REMOTE/batch/run-bs.conf"
    echo "summarize:    python3 summarize_bs.py runs-bs-$TAG.csv . $TAG"
} > "$OUT/commands.txt"

echo "files:"
ls -1 "$OUT"

$SSH -O exit "$HOST" 2>/dev/null || true
