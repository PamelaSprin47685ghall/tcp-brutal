#!/bin/sh
# hnd0-bs-run.sh - interleaved baseline/candidate run on HND0.
#
# Usage: hnd0-bs-run.sh <baseline-binary> <candidate-binary> <remote-dir>
#
# Copies both binaries to <remote-dir> on HND0, then runs every point
# ROUNDS times, alternating base/cand order per round, pinned to one CPU
# at reduced priority.  Emits CSV on stdout (meta lines start with '#').
#
# This script runs only when the caller invokes it; it never starts a
# background or scheduled run.
#
# Measurement noise: each decode sample integrates roughly 0.1-0.2 s on
# HND0's shared 2 vCPU QEMU guest, so single samples swing 1.5-2x.  Compare
# base/cand inside one batch (same round, same point) and raise HND0_ROUNDS
# to 5 or more when the difference you need to resolve is below ~30%.
#
# Environment overrides:
#   HND0_HOST      ssh target          (default root@45.11.1.62)
#   HND0_POINTS    "block:L:rate:p"    (default 4 points below)
#   HND0_TRIALS    decode trials       (default 2000)
#   HND0_CODES     code instances      (default 4)
#   HND0_THREADS   decoder threads     (default 1)
#   HND0_MODEL     systematic|symmetric (default symmetric)
#   HND0_ROUNDS    interleaved rounds  (default 3)
#   HND0_CPUSET    taskset CPU list    (default 0)
#   HND0_NICE      nice adjustment     (default 10)
set -eu

usage() {
    echo "usage: $0 <baseline-binary> <candidate-binary> <remote-dir>" >&2
    echo "  env: HND0_HOST HND0_POINTS HND0_TRIALS HND0_CODES HND0_THREADS" >&2
    echo "       HND0_MODEL HND0_ROUNDS HND0_CPUSET HND0_NICE" >&2
}

[ "$#" -eq 3 ] || { usage; exit 2; }

BASE=$1
CAND=$2
RDIR=$3
HOST=${HND0_HOST:-root@45.11.1.62}
TRIALS=${HND0_TRIALS:-2000}
CODES=${HND0_CODES:-4}
THREADS=${HND0_THREADS:-1}
MODEL=${HND0_MODEL:-symmetric}
ROUNDS=${HND0_ROUNDS:-3}
CPUSET=${HND0_CPUSET:-0}
NICE=${HND0_NICE:-10}
POINTS=${HND0_POINTS:-"256:16:0.50:0.30 256:16:0.60:0.30 256:16:0.70:0.20 512:16:0.50:0.30"}

for f in "$BASE" "$CAND"; do
    if [ ! -x "$f" ]; then
        echo "error: not an executable file: $f" >&2
        exit 1
    fi
done

SSH="ssh -o ConnectTimeout=10 -o BatchMode=yes"
SCP="scp -o ConnectTimeout=10 -o BatchMode=yes"

$SSH "$HOST" "mkdir -p '$RDIR'"
$SCP "$BASE" "$HOST:$RDIR/base_bin"
$SCP "$CAND" "$HOST:$RDIR/cand_bin"
$SSH "$HOST" "chmod +x '$RDIR/base_bin' '$RDIR/cand_bin'"

# Portability smoke: a binary built against a newer glibc fails here.
if ! $SSH "$HOST" "'$RDIR/base_bin' --block 256 --L 16 --rate 0.50 --p 0.30 \
        --trials 1 --codes 1 --no-header >/dev/null 2>&1"; then
    echo "error: baseline binary does not run on $HOST (glibc/arch mismatch?)" >&2
    echo "hint: build both binaries on $HOST, or pass a remote-compiled pair" >&2
    exit 1
fi

cat > /tmp/hnd0-bs-run-remote.$$ <<'REMOTE'
#!/bin/sh
set -eu
cd "$RDIR"
echo "# start $(date -u +%FT%TZ) host=$(hostname) load=$(cut -d' ' -f1-3 /proc/loadavg) avail=$(free -m | awk '/Mem:/{print $7}')MiB"
echo "round,binary,block,L,rate,p,trials,recovery,ci99,iter_mean,enc_mbps,dec_mbps,latency_ms,mem_bytes"

run_one() {
    round=$1
    name=$2
    bin=$3
    block=$4
    L=$5
    rate=$6
    p=$7
    line=$(taskset -c "$CPUSET" nice -n "$NICE" "./$bin" \
        --block "$block" --L "$L" --rate "$rate" --p "$p" \
        --trials "$TRIALS" --codes "$CODES" --threads "$THREADS" \
        --model "$MODEL" --no-header 2>/dev/null)
    printf '%s,%s,%s\n' "$round" "$name" "$line"
}

round=1
while [ "$round" -le "$ROUNDS" ]; do
    for spec in $POINTS; do
        set -- $(echo "$spec" | tr ':' ' ')
        if [ $((round % 2)) -eq 1 ]; then
            run_one "$round" base base_bin "$1" "$2" "$3" "$4"
            run_one "$round" cand cand_bin "$1" "$2" "$3" "$4"
        else
            run_one "$round" cand cand_bin "$1" "$2" "$3" "$4"
            run_one "$round" base base_bin "$1" "$2" "$3" "$4"
        fi
    done
    round=$((round + 1))
done

echo "# end $(date -u +%FT%TZ) load=$(cut -d' ' -f1-3 /proc/loadavg) avail=$(free -m | awk '/Mem:/{print $7}')MiB"
REMOTE

$SCP /tmp/hnd0-bs-run-remote.$$ "$HOST:$RDIR/run.sh"
rm -f /tmp/hnd0-bs-run-remote.$$

$SSH "$HOST" "RDIR='$RDIR' TRIALS='$TRIALS' CODES='$CODES' THREADS='$THREADS' \
    MODEL='$MODEL' ROUNDS='$ROUNDS' CPUSET='$CPUSET' NICE='$NICE' \
    POINTS='$POINTS' sh '$RDIR/run.sh'"
