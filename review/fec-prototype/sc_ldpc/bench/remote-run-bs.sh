#!/bin/sh
# remote-run-bs.sh - same-batch A/B of the scalar baseline against the
# bit-sliced candidate on HND0.
#
# Reads /tmp/sclc-bench/batch/run-bs.conf:
#   TAG ROUNDS ROUNDS_B2 ROUNDS_2T TRIALS CODES POINTS BATCH BATCH2 SEED_BASE
# POINTS entries are block:L:rate:p.
#
# Phases:
#   t1b512  base vs cand@batch512, 1 thread, ROUNDS rounds
#   t1b1024 cand@batch512 vs cand@batch1024, 1 thread, ROUNDS_B2 rounds
#   t2b512  base vs cand@batch512, 2 threads, ROUNDS_2T rounds
#
# Order alternates every round and the two variants share the seed, so
# drift on the shared QEMU host cancels in the paired ratio.
#
# stdout: one CSV row per process:
#   phase,round,variant,threads,batch,block,L,rate,p,seed,load1,trials,cw,
#   recovery,iter,enc,dec,dec_scalar,speedup,unres_mismatch,bit_mismatch
set -eu

R=${R:-/tmp/sclc-bench}
. "$R/batch/run-bs.conf"

LOG="$R/log/$TAG.err"
mkdir -p "$R/log"
: > "$LOG"

echo "tag=$TAG rounds=$ROUNDS rounds_b2=$ROUNDS_B2 rounds_2t=$ROUNDS_2T" >&2
echo "trials=$TRIALS codes=$CODES batch=$BATCH batch2=$BATCH2 points=$POINTS" >&2
echo "start_utc=$(date -u +%FT%TZ)" >> "$LOG"

echo "phase,round,variant,threads,batch,block,L,rate,p,seed,load1,trials,cw,recovery,iter,enc,dec,dec_scalar,speedup,unres_mismatch,bit_mismatch"

cpus_for() {
    case "$1" in
        1) echo 0 ;;
        2) echo 0,1 ;;
        *) echo "unsupported thread count: $1" >&2; exit 2 ;;
    esac
}

base_row() {
    phase=$1
    round=$2
    threads=$3
    block=$4
    L=$5
    rate=$6
    p=$7
    seed=$8
    cpus=$(cpus_for "$threads")
    read -r load1 _ < /proc/loadavg
    echo "### $phase r=$round base t=$threads b=$block L=$L rate=$rate p=$p seed=$seed" >> "$LOG"
    row=$(taskset -c "$cpus" nice -n 10 "$R/bin/base_bench" \
        --block "$block" --L "$L" --rate "$rate" --p "$p" \
        --trials "$TRIALS" --codes "$CODES" --model symmetric \
        --term causal --extra-dc 5 --threads "$threads" --seed "$seed" \
        --no-header 2>> "$LOG")
    nf=$(printf '%s\n' "$row" | awk -F, '{ print NF }')
    if [ "$nf" -ne 12 ]; then
        echo "bad base row ($nf fields): $row" >&2
        exit 1
    fi
    oldIFS=$IFS
    IFS=,
    set -- $row
    IFS=$oldIFS
    echo "$phase,$round,base,$threads,0,$1,$2,$3,$4,$seed,$load1,$5,$5,$6,$8,$9,${10},${10},1.00,NA,NA"
}

cand_row() {
    phase=$1
    round=$2
    threads=$3
    batch=$4
    block=$5
    L=$6
    rate=$7
    p=$8
    seed=$9
    cpus=$(cpus_for "$threads")
    read -r load1 _ < /proc/loadavg
    echo "### $phase r=$round cand t=$threads batch=$batch b=$block L=$L rate=$rate p=$p seed=$seed" >> "$LOG"
    # sc_ldpc_bs_bench always prints its CSV header; keep only the data row.
    if ! taskset -c "$cpus" nice -n 10 "$R/bin/cand_bs_bench" \
        --block "$block" --L "$L" --rate "$rate" --p "$p" \
        --trials "$TRIALS" --batch "$batch" --threads "$threads" \
        --model sym --seed "$seed" > "$R/log/.cand-row" 2>> "$LOG"; then
        echo "candidate failed: batch=$batch block=$block rate=$rate p=$p" >&2
        exit 1
    fi
    row=$(tail -n 1 "$R/log/.cand-row")
    nf=$(printf '%s\n' "$row" | awk -F, '{ print NF }')
    if [ "$nf" -ne 20 ]; then
        echo "bad candidate row ($nf fields): $row" >&2
        exit 1
    fi
    oldIFS=$IFS
    IFS=,
    set -- $row
    IFS=$oldIFS
    cw=${10}
    rec=${11}
    unres=${13}
    bit=${14}
    iter=${15}
    dsc=${16}
    dbs=${17}
    sp=${18}
    dbs2=${19}
    sp2=${20}
    if [ "$threads" -eq 2 ]; then
        dec=$dbs2
        speed=$sp2
        dsc=NA
    else
        dec=$dbs
        speed=$sp
    fi
    echo "$phase,$round,cand,$threads,$batch,$2,$3,$4,$5,$seed,$load1,$TRIALS,$cw,$rec,$iter,NA,$dec,$dsc,$speed,$unres,$bit"
}

parse_point() {
    P_BLOCK=${1%%:*}
    rest=${1#*:}
    P_L=${rest%%:*}
    rest=${rest#*:}
    P_RATE=${rest%%:*}
    P_P=${rest#*:}
}

first=${POINTS%% *}
parse_point "$first"
W_BLOCK=$P_BLOCK
W_L=$P_L
W_RATE=$P_RATE
W_P=$P_P

# --- warmups (discarded) --------------------------------------------------
echo "### warmup t1 base" >> "$LOG"
taskset -c 0 nice -n 10 "$R/bin/base_bench" --block "$W_BLOCK" --L "$W_L" \
    --rate "$W_RATE" --p "$W_P" --trials 256 --codes "$CODES" \
    --model symmetric --term causal --extra-dc 5 --threads 1 --seed "$SEED_BASE" \
    --no-header > /dev/null 2>> "$LOG"
echo "### warmup t1 cand b512" >> "$LOG"
taskset -c 0 nice -n 10 "$R/bin/cand_bs_bench" --block "$W_BLOCK" --L "$W_L" \
    --rate "$W_RATE" --p "$W_P" --trials 256 --batch "$BATCH" --threads 1 \
    --model sym --seed "$SEED_BASE" > /dev/null 2>> "$LOG"

# --- phase 1: base vs cand@512, one thread --------------------------------
for spec in $POINTS; do
    parse_point "$spec"
    round=0
    while [ "$round" -lt "$ROUNDS" ]; do
        round=$((round + 1))
        seed=$((SEED_BASE + round - 1))
        if [ $((round % 2)) -eq 1 ]; then
            base_row t1b512 "$round" 1 "$P_BLOCK" "$P_L" "$P_RATE" "$P_P" "$seed"
            cand_row t1b512 "$round" 1 "$BATCH" "$P_BLOCK" "$P_L" "$P_RATE" "$P_P" "$seed"
        else
            cand_row t1b512 "$round" 1 "$BATCH" "$P_BLOCK" "$P_L" "$P_RATE" "$P_P" "$seed"
            base_row t1b512 "$round" 1 "$P_BLOCK" "$P_L" "$P_RATE" "$P_P" "$seed"
        fi
    done
done

# --- phase 2: cand batch 512 vs 1024, one thread --------------------------
echo "### warmup t1 cand b1024" >> "$LOG"
taskset -c 0 nice -n 10 "$R/bin/cand_bs_bench" --block "$W_BLOCK" --L "$W_L" \
    --rate "$W_RATE" --p "$W_P" --trials 256 --batch "$BATCH2" --threads 1 \
    --model sym --seed "$SEED_BASE" > /dev/null 2>> "$LOG"

for spec in $POINTS; do
    parse_point "$spec"
    round=0
    while [ "$round" -lt "$ROUNDS_B2" ]; do
        round=$((round + 1))
        seed=$((SEED_BASE + round - 1))
        if [ $((round % 2)) -eq 1 ]; then
            cand_row t1b1024 "$round" 1 "$BATCH" "$P_BLOCK" "$P_L" "$P_RATE" "$P_P" "$seed"
            cand_row t1b1024 "$round" 1 "$BATCH2" "$P_BLOCK" "$P_L" "$P_RATE" "$P_P" "$seed"
        else
            cand_row t1b1024 "$round" 1 "$BATCH2" "$P_BLOCK" "$P_L" "$P_RATE" "$P_P" "$seed"
            cand_row t1b1024 "$round" 1 "$BATCH" "$P_BLOCK" "$P_L" "$P_RATE" "$P_P" "$seed"
        fi
    done
done

# --- phase 3: base vs cand@512, two threads -------------------------------
echo "### warmup t2 base" >> "$LOG"
taskset -c 0,1 nice -n 10 "$R/bin/base_bench" --block "$W_BLOCK" --L "$W_L" \
    --rate "$W_RATE" --p "$W_P" --trials 256 --codes "$CODES" \
    --model symmetric --term causal --extra-dc 5 --threads 2 --seed "$SEED_BASE" \
    --no-header > /dev/null 2>> "$LOG"
echo "### warmup t2 cand b512" >> "$LOG"
taskset -c 0,1 nice -n 10 "$R/bin/cand_bs_bench" --block "$W_BLOCK" --L "$W_L" \
    --rate "$W_RATE" --p "$W_P" --trials 256 --batch "$BATCH" --threads 2 \
    --model sym --seed "$SEED_BASE" > /dev/null 2>> "$LOG"

for spec in $POINTS; do
    parse_point "$spec"
    round=0
    while [ "$round" -lt "$ROUNDS_2T" ]; do
        round=$((round + 1))
        seed=$((SEED_BASE + round - 1))
        if [ $((round % 2)) -eq 1 ]; then
            base_row t2b512 "$round" 2 "$P_BLOCK" "$P_L" "$P_RATE" "$P_P" "$seed"
            cand_row t2b512 "$round" 2 "$BATCH" "$P_BLOCK" "$P_L" "$P_RATE" "$P_P" "$seed"
        else
            cand_row t2b512 "$round" 2 "$BATCH" "$P_BLOCK" "$P_L" "$P_RATE" "$P_P" "$seed"
            base_row t2b512 "$round" 2 "$P_BLOCK" "$P_L" "$P_RATE" "$P_P" "$seed"
        fi
    done
done

echo "end_utc=$(date -u +%FT%TZ)" >> "$LOG"
