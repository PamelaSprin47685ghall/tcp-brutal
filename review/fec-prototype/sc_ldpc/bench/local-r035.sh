#!/bin/sh
# local-r035.sh - local-only check of the p0.40/rate0.35 candidate point.
#
# No HND0 access.  Recovery is machine-independent for a fixed code/seed
# (BEC peeling is deterministic), so 160k codewords here answer the same
# recovery question as HND0.  Throughput is local only and is labelled as
# such; the HND0 >4.215 confirmation waits for T5.2 to end.
set -eu

BASE_SRC=${BASE_SRC:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
CAND_SRC=${CAND_SRC:-$HOME/tbtun-worktrees/sclc-simd/review/fec-prototype/sc_ldpc}
OUT=${OUT:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/results/local-r035}
BUILD=${BUILD:-/tmp/sclc-local}
CFLAGS=${CFLAGS:-"-O3 -std=c99 -Wall -Wextra -Werror -march=x86-64 -mtune=generic"}

mkdir -p "$OUT" "$BUILD"

echo "== build scalar and bit-sliced benches" >&2
# shellcheck disable=SC2086
cc $CFLAGS "$BASE_SRC/sc_ldpc.c" "$BASE_SRC/sc_ldpc_bench.c" -lm -pthread \
    -o "$BUILD/bench-base"
# shellcheck disable=SC2086
cc $CFLAGS "$CAND_SRC/sc_ldpc.c" "$CAND_SRC/sc_ldpc_bs.c" \
    "$CAND_SRC/sc_ldpc_bs_bench.c" -lm -pthread -o "$BUILD/bench-bs"

echo "## local host" > "$OUT/env.txt"
uname -a >> "$OUT/env.txt"
lscpu | awk 'NR <= 22' >> "$OUT/env.txt"
gcc --version | head -1 >> "$OUT/env.txt"
cat /proc/loadavg >> "$OUT/env.txt"

# --- recovery: 20k or 40k trials x 8 seeds, scalar ACC -------------------
{
    echo "point,seed,block,L,rate,p,trials,recovery,ci99,iter,load1"
    for spec in "0.35:0.40:20000" "0.50:0.30:40000" "0.40:0.40:20000"; do
        rate=${spec%%:*}
        rest=${spec#*:}
        p=${rest%%:*}
        trials=${rest#*:}
        seed=0
        while [ "$seed" -lt 8 ]; do
            seed=$((seed + 1))
            read -r load1 _ < /proc/loadavg
            row=$(taskset -c 0 nice -n 10 "$BUILD/bench-base" \
                --block 256 --L 16 --rate "$rate" --p "$p" \
                --trials "$trials" --codes 1 --model symmetric \
                --term causal --extra-dc 5 --threads 1 --seed "$seed" \
                --no-header 2>> "$OUT/rec.err")
            oldIFS=$IFS
            IFS=,
            set -- $row
            IFS=$oldIFS
            echo "256:16:$rate:$p,$seed,$1,$2,$3,$4,$5,$6,$7,$8,$load1"
        done
    done
} > "$OUT/rec-local.csv"

# --- local throughput: scalar and bit-sliced, 5 rounds, 1t and 2t --------
{
    echo "round,threads,block,L,rate,p,seed,recovery,iter,enc,dec"
    for threads in 1 2; do
        case "$threads" in 1) cpus=0 ;; 2) cpus=0,1 ;; esac
        for spec in "0.35:0.40" "0.50:0.30"; do
            rate=${spec%%:*}
            p=${spec#*:}
            round=0
            while [ "$round" -lt 5 ]; do
                round=$((round + 1))
                row=$(taskset -c "$cpus" nice -n 10 "$BUILD/bench-base" \
                    --block 256 --L 16 --rate "$rate" --p "$p" \
                    --trials 2000 --codes 4 --model symmetric --term causal \
                    --extra-dc 5 --threads "$threads" --seed "$round" \
                    --no-header 2>> "$OUT/thr-base.err")
                oldIFS=$IFS
                IFS=,
                set -- $row
                IFS=$oldIFS
                echo "$round,$threads,$1,$2,$3,$4,$round,$6,$8,$9,${10}"
            done
        done
    done
} > "$OUT/thr-base-local.csv"

{
    echo "round,threads,block,L,rate,p,seed,cw,recovery_bs,recovery_scalar,unres,bit,iter,dec_scalar,dec_bs,speedup,dec_bs_2t,speedup_2t"
    for threads in 1 2; do
        case "$threads" in 1) cpus=0 ;; 2) cpus=0,1 ;; esac
        for spec in "0.35:0.40" "0.50:0.30"; do
            rate=${spec%%:*}
            p=${spec#*:}
            round=0
            while [ "$round" -lt 5 ]; do
                round=$((round + 1))
                row=$(taskset -c "$cpus" nice -n 10 "$BUILD/bench-bs" \
                    --block 256 --L 16 --rate "$rate" --p "$p" \
                    --trials 2048 --batch 512 --threads "$threads" \
                    --model sym --seed "$round" 2>> "$OUT/thr-bs.err" | tail -n 1)
                oldIFS=$IFS
                IFS=,
                set -- $row
                IFS=$oldIFS
                echo "$round,$threads,$2,$3,$4,$5,$round,${10},${11},${12},${13},${14},${15},${16},${17},${18},${19},${20}"
            done
        done
    done
} > "$OUT/thr-bs-local.csv"

echo "== recovery" >&2
python3 "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/summarize_rec.py" \
    "$OUT/rec-local.csv" "$OUT" local-r035 | tee "$OUT/summary-rec-local.txt"
echo "== local files: $OUT" >&2
ls -1 "$OUT" >&2
