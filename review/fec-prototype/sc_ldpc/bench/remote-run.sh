#!/bin/sh
# remote-run.sh - one same-batch alternating A/B benchmark on the HND0 host.
#
# Reads /tmp/sclc-bench/batch/run.conf (BATCH, ROUNDS, TRIALS, CODES,
# BLOCK, L, POINTS, THREADS_LIST, SEED_BASE, V1, V2).  Each round runs the
# two variants in alternating order (V1,V2 then V2,V1) with the same seed,
# so slow drift on the shared QEMU host cancels.  All runs are pinned with
# taskset and niced; 2-thread runs use both vCPUs.
#
# stdout: one CSV row per run
#   batch,round,variant,threads,block,L,rate,p,seed,load1,trials,recovery,
#   ci99,iter,enc,dec,latency_ms,mem_bytes,order
# stderr of every bench process goes to /tmp/sclc-bench/log/<batch>.err with
# a marker line per run.
set -eu

R=${R:-/tmp/sclc-bench}
. "$R/batch/run.conf"

BIN="$R/bin"
LOG="$R/log/$BATCH.err"
mkdir -p "$R/log"
: > "$LOG"

echo "batch=$BATCH rounds=$ROUNDS trials=$TRIALS codes=$CODES block=$BLOCK L=$L" >&2
echo "points=$POINTS threads=$THREADS_LIST seed_base=$SEED_BASE v1=$V1 v2=$V2" >&2
echo "start_utc=$(date -u +%FT%TZ)" >> "$LOG"

echo "batch,round,variant,threads,block,L,rate,p,seed,load1,trials,recovery,ci99,iter,enc,dec,latency_ms,mem_bytes,order"

first=${POINTS%% *}
wp=${first%%:*}
wr=${first#*:}

for threads in $THREADS_LIST; do
    case "$threads" in
        1) cpus=0 ;;
        2) cpus=0,1 ;;
        *)
            echo "unsupported thread count: $threads" >&2
            exit 2
            ;;
    esac

    # One discarded warmup per variant per thread setting.
    for v in $V1 $V2; do
        echo "### warmup variant=$v threads=$threads" >> "$LOG"
        taskset -c "$cpus" nice -n 10 "$BIN/$v" \
            --block "$BLOCK" --L "$L" --rate "$wr" --p "$wp" \
            --trials 200 --codes "$CODES" --model symmetric --term causal \
            --extra-dc 5 --threads "$threads" --seed "$SEED_BASE" \
            --no-header > /dev/null 2>> "$LOG"
    done

    for spec in $POINTS; do
        p=${spec%%:*}
        rate=${spec#*:}
        round=0
        while [ "$round" -lt "$ROUNDS" ]; do
            round=$((round + 1))
            seed=$((SEED_BASE + round - 1))
            if [ $((round % 2)) -eq 1 ]; then
                order="$V1 $V2"
            else
                order="$V2 $V1"
            fi
            pos=0
            for v in $order; do
                pos=$((pos + 1))
                read -r load1 _ < /proc/loadavg
                echo "### round=$round variant=$v threads=$threads p=$p rate=$rate seed=$seed" >> "$LOG"
                row=$(taskset -c "$cpus" nice -n 10 "$BIN/$v" \
                    --block "$BLOCK" --L "$L" --rate "$rate" --p "$p" \
                    --trials "$TRIALS" --codes "$CODES" --model symmetric \
                    --term causal --extra-dc 5 --threads "$threads" \
                    --seed "$seed" --no-header 2>> "$LOG")
                oldIFS=$IFS
                IFS=,
                set -- $row
                IFS=$oldIFS
                echo "$BATCH,$round,$v,$threads,$1,$2,$3,$4,$seed,$load1,$5,$6,$7,$8,$9,${10},${11},${12},$pos"
            done
        done
    done
done

echo "end_utc=$(date -u +%FT%TZ)" >> "$LOG"
