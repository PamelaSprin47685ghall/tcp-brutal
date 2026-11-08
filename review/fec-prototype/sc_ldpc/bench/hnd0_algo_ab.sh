#!/bin/sh
# hnd0_algo_ab.sh - sclc-algo fast/opt vs baseline, 2 threads, 20 rounds.
#
# The user's acceptance question: does the scalar structural optimisation
# reach 4.215 MB/s on HND0 in a same-batch 20-round median?  All three
# variants use the same bench (--bench-sec 1.0, worker affinity) so the
# comparison is like-for-like.
set -eu

HOST=${HOST:-root@45.11.1.62}
REMOTE=${REMOTE:-/tmp/sclc-bench}
CTL_SOCK=${CTL_SOCK:-/tmp/sclc-bench-ssh-%r@%h:%p}
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=15 -o ServerAliveCountMax=8 -o ControlMaster=auto -o ControlPath=$CTL_SOCK -o ControlPersist=600"
SSH="ssh $SSH_OPTS"
SCP="scp -q $SSH_OPTS"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BASE_SRC=${BASE_SRC:-$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)}
ALGO_SRC=${ALGO_SRC:-$HOME/tbtun-worktrees/sclc-algo/review/fec-prototype/sc_ldpc}
ROUNDS=${ROUNDS:-20}
TRIALS=${TRIALS:-2000}
CODES=${CODES:-4}
BLOCK=${BLOCK:-256}
L=${L:-16}
RATE=${RATE:-0.50}
P=${P:-0.30}
SEED_BASE=${SEED_BASE:-1}
TAG=${TAG:-$(date -u +%Y%m%dT%H%M%SZ)}
OUT=${OUT:-}
ORIG_ARGS=$*

usage() {
    awk 'NR >= 2 && NR <= 20' "$0"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --base) BASE_SRC=$2; shift 2 ;;
        --algo) ALGO_SRC=$2; shift 2 ;;
        --tag) TAG=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --rounds) ROUNDS=$2; shift 2 ;;
        --trials) TRIALS=$2; shift 2 ;;
        --block) BLOCK=$2; shift 2 ;;
        --L) L=$2; shift 2 ;;
        --rate) RATE=$2; shift 2 ;;
        --p) P=$2; shift 2 ;;
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
    echo "local_args=$ORIG_ARGS"
    echo "rounds=$ROUNDS trials=$TRIALS codes=$CODES"
    echo "point=block$BLOCK L$L rate$RATE p$P seed_base=$SEED_BASE"
    echo "base_src=$BASE_SRC"
    echo "algo_src=$ALGO_SRC"
    echo "## base commit=$(git -C "$BASE_SRC" rev-parse HEAD 2>/dev/null || echo none)"
    echo "## algo commit=$(git -C "$ALGO_SRC" rev-parse HEAD 2>/dev/null || echo none)"
    echo "## algo dirty=$(git -C "$ALGO_SRC" status --porcelain -- . 2>/dev/null | wc -l) entries"
    (cd "$BASE_SRC" && sha256sum sc_ldpc.c sc_ldpc.h sc_ldpc_bench.c 2>/dev/null | awk '{print "base", $0}')
    (cd "$ALGO_SRC" && sha256sum sc_ldpc.c sc_ldpc.h sc_ldpc_bench.c 2>/dev/null | awk '{print "algo", $0}')
    echo "fast_prebuilt_sha256=$(ssh $SSH_OPTS "$HOST" 'sha256sum /tmp/sc_ldpc_algo/bench_fast 2>/dev/null | cut -d" " -f1' 2>/dev/null || echo unknown)"
} > "$OUT/provenance.txt"

sync_src() {
    src=$1
    v=$2
    tar -C "$src" -cf "$OUT/src-$v.tar" \
        --exclude='./bench' --exclude='./results' --exclude='./.git' \
        --exclude='*.csv' --exclude='./sc_ldpc_bench' --exclude='./dump' .
    $SCP "$OUT/src-$v.tar" "$HOST:$REMOTE/batch/src-$v.tar"
    $SSH "$HOST" "rm -rf '$REMOTE/src/$v' && mkdir -p '$REMOTE/src/$v' && tar -C '$REMOTE/src/$v' -xf '$REMOTE/batch/src-$v.tar'"
}

$SSH "$HOST" "mkdir -p '$REMOTE/src' '$REMOTE/bin' '$REMOTE/log' '$REMOTE/batch'"
sync_src "$BASE_SRC" base
sync_src "$ALGO_SRC" algo

{
    echo "TAG=$TAG"
    echo "ROUNDS=$ROUNDS"
    echo "TRIALS=$TRIALS"
    echo "CODES=$CODES"
    echo "BLOCK=$BLOCK"
    echo "L=$L"
    echo "RATE=$RATE"
    echo "P=$P"
    echo "SEED_BASE=$SEED_BASE"
} > "$OUT/run-algo.conf"

$SCP "$SCRIPT_DIR/remote-algo.sh" "$HOST:$REMOTE/batch/remote-algo.sh"
$SCP "$OUT/run-algo.conf" "$HOST:$REMOTE/batch/run-algo.conf"

$SSH "$HOST" '
    echo "## date"; date -u
    echo "## uname"; uname -a
    echo "## lscpu"; lscpu | awk "NR <= 20"
    echo "## cpuinfo_flags"; grep -m1 "^flags" /proc/cpuinfo
    echo "## gcc"; gcc --version | head -1
    echo "## loadavg"; cat /proc/loadavg
    echo "## free"; free -m
    echo "## top_cpu"; ps -eo pid,comm,pcpu --sort=-pcpu | head -8
' > "$OUT/env.txt" 2>&1

echo "running $TAG: rounds=$ROUNDS point=block$BLOCK L$L rate$RATE p$P"
if ! $SSH "$HOST" "sh '$REMOTE/batch/remote-algo.sh'" > "$OUT/runs-algo-$TAG.csv" 2> "$OUT/run-algo-$TAG.err"; then
    echo "benchmark failed; see $OUT/run-algo-$TAG.err" >&2
    tail -30 "$OUT/run-algo-$TAG.err" >&2
    exit 1
fi
$SCP "$HOST:$REMOTE/log/$TAG-algo.err" "$OUT/bench-stderr-$TAG.log" 2>/dev/null || true

python3 "$SCRIPT_DIR/summarize_algo.py" "$OUT/runs-algo-$TAG.csv" "$OUT" "$TAG" \
    > "$OUT/summary-algo-$TAG.txt"
cat "$OUT/summary-algo-$TAG.txt"

{
    echo "local: $0 $ORIG_ARGS"
    echo "remote: sh $REMOTE/batch/remote-algo.sh"
    echo "conf: $REMOTE/batch/run-algo.conf"
    echo "summarize: python3 summarize_algo.py runs-algo-$TAG.csv . $TAG"
} > "$OUT/commands.txt"

echo "files:"
ls -1 "$OUT"
$SSH -O exit "$HOST" 2>/dev/null || true
