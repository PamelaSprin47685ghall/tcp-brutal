#!/bin/sh
# hnd0_enc_ab.sh - sclc-encode candidate A/B on HND0.
#
# The bench measures scalar_ref and enc64/e2e/sse2 in one process, so the
# paired comparison is internal; outer rounds expose host drift.
set -eu

HOST=${HOST:-root@45.11.1.62}
REMOTE=${REMOTE:-/tmp/sclc-bench}
CTL_SOCK=${CTL_SOCK:-/tmp/sclc-bench-ssh-%r@%h:%p}
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=15 -o ServerAliveCountMax=8 -o ControlMaster=auto -o ControlPath=$CTL_SOCK -o ControlPersist=600"
SSH="ssh $SSH_OPTS"
SCP="scp -q $SSH_OPTS"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ENC_SRC=${ENC_SRC:-$HOME/tbtun-worktrees/sclc-encode/review/fec-prototype/sc_ldpc}
ROUNDS=${ROUNDS:-5}
RATES=${RATES:-"0.50 0.40 0.30"}
BENCH_SEC=${BENCH_SEC:-0.20}
RUNS=${RUNS:-5}
THREADS_LIST=${THREADS_LIST:-"1 2"}
BLOCK=${BLOCK:-256}
L=${L:-16}
SEED_BASE=${SEED_BASE:-1}
TAG=${TAG:-$(date -u +%Y%m%dT%H%M%SZ)}
OUT=${OUT:-}
ORIG_ARGS=$*

while [ $# -gt 0 ]; do
    case "$1" in
        --enc) ENC_SRC=$2; shift 2 ;;
        --tag) TAG=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --rounds) ROUNDS=$2; shift 2 ;;
        --rates) RATES=$2; shift 2 ;;
        --seconds) BENCH_SEC=$2; shift 2 ;;
        --runs) RUNS=$2; shift 2 ;;
        --threads) THREADS_LIST=$2; shift 2 ;;
        --block) BLOCK=$2; shift 2 ;;
        --L) L=$2; shift 2 ;;
        --seed) SEED_BASE=$2; shift 2 ;;
        -h|--help) awk 'NR >= 2 && NR <= 18' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
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
    echo "rounds=$ROUNDS rates=$RATES seconds=$BENCH_SEC runs=$RUNS threads=$THREADS_LIST"
    echo "point=block$BLOCK L$L seed_base=$SEED_BASE"
    echo "enc_src=$ENC_SRC"
    echo "## enc commit=$(git -C "$ENC_SRC" rev-parse HEAD 2>/dev/null || echo none)"
    echo "## enc dirty=$(git -C "$ENC_SRC" status --porcelain -- . 2>/dev/null | wc -l) entries"
    (cd "$ENC_SRC" && sha256sum sc_ldpc.c sc_ldpc.h sc_enc64.c sc_enc64.h \
        sc_enc64_internal.h sc_enc64_pack.c sc_enc64_sse2.c \
        sc_enc64_bench.c sc_enc64_test.c 2>/dev/null | awk '{print "enc", $0}')
} > "$OUT/provenance.txt"

tar -C "$ENC_SRC" -cf "$OUT/src-enc64.tar" \
    sc_ldpc.c sc_ldpc.h sc_enc64.c sc_enc64.h sc_enc64_internal.h \
    sc_enc64_pack.c sc_enc64_sse2.c sc_enc64_bench.c sc_enc64_test.c
$SSH "$HOST" "mkdir -p '$REMOTE/src/enc64' '$REMOTE/bin' '$REMOTE/log' '$REMOTE/batch'"
$SCP "$OUT/src-enc64.tar" "$HOST:$REMOTE/batch/src-enc64.tar"
$SSH "$HOST" "rm -rf '$REMOTE/src/enc64' && mkdir -p '$REMOTE/src/enc64' && tar -C '$REMOTE/src/enc64' -xf '$REMOTE/batch/src-enc64.tar'"

{
    echo "TAG=$TAG"
    echo "ROUNDS=$ROUNDS"
    echo "RATES=\"$RATES\""
    echo "BENCH_SEC=$BENCH_SEC"
    echo "RUNS=$RUNS"
    echo "THREADS_LIST=\"$THREADS_LIST\""
    echo "BLOCK=$BLOCK"
    echo "L=$L"
    echo "SEED_BASE=$SEED_BASE"
} > "$OUT/run-enc64.conf"

$SCP "$SCRIPT_DIR/remote-enc64.sh" "$HOST:$REMOTE/batch/remote-enc64.sh"
$SCP "$OUT/run-enc64.conf" "$HOST:$REMOTE/batch/run-enc64.conf"

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

echo "running $TAG: rounds=$ROUNDS rates=[$RATES] threads=[$THREADS_LIST]"
if ! $SSH "$HOST" "sh '$REMOTE/batch/remote-enc64.sh'" > "$OUT/runs-enc-$TAG.csv" 2> "$OUT/run-enc-$TAG.err"; then
    echo "encode A/B failed; see $OUT/run-enc-$TAG.err" >&2
    tail -30 "$OUT/run-enc-$TAG.err" >&2
    exit 1
fi
$SCP "$HOST:$REMOTE/log/$TAG-enc64.err" "$OUT/bench-stderr-$TAG.log" 2>/dev/null || true
$SCP "$HOST:$REMOTE/log/$TAG-enc64-test.log" "$OUT/enctest-$TAG.log" 2>/dev/null || true

python3 "$SCRIPT_DIR/summarize_enc.py" "$OUT/runs-enc-$TAG.csv" "$OUT" "$TAG" \
    > "$OUT/summary-enc-$TAG.txt"
cat "$OUT/summary-enc-$TAG.txt"

{
    echo "local: $0 $ORIG_ARGS"
    echo "remote: sh $REMOTE/batch/remote-enc64.sh"
    echo "conf: $REMOTE/batch/run-enc64.conf"
    echo "summarize: python3 summarize_enc.py runs-enc-$TAG.csv . $TAG"
} > "$OUT/commands.txt"

echo "files:"
ls -1 "$OUT"
$SSH -O exit "$HOST" 2>/dev/null || true
