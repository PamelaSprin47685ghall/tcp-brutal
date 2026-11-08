#!/bin/sh
# remote-rec.sh - 20k-trial recovery per seed on HND0, scalar ACC bench.
#
# The bit-sliced decoder is bit-identical to the scalar path (unres and bit
# mismatch are zero in the A/B), so the scalar bench gives the recovery
# statistic for 160k codewords at a fraction of the runtime.
set -eu

R=${R:-/tmp/sclc-bench}
. "$R/batch/run-rec.conf"
LOG="$R/log/$TAG-rec.err"

mkdir -p "$R/log"
: > "$LOG"
echo "tag=$TAG points=$POINTS trials=$TRIALS seeds=$SEEDS" >&2
echo "start_utc=$(date -u +%FT%TZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$LOG"

echo "point,seed,block,L,rate,p,trials,recovery,ci99,iter,load1"

for spec in $POINTS; do
    block=${spec%%:*}
    rest=${spec#*:}
    L=${rest%%:*}
    rest=${rest#*:}
    rate=${rest%%:*}
    p=${rest#*:}
    seed=0
    while [ "$seed" -lt "$SEEDS" ]; do
        seed=$((seed + 1))
        read -r load1 _ < /proc/loadavg
        echo "### point=$spec seed=$seed" >> "$LOG"
        row=$(taskset -c 0 nice -n 10 "$R/bin/base_bench" \
            --block "$block" --L "$L" --rate "$rate" --p "$p" \
            --trials "$TRIALS" --codes 1 --model symmetric --term causal \
            --extra-dc 5 --threads 1 --seed "$seed" --no-header 2>> "$LOG")
        nf=$(printf '%s\n' "$row" | awk -F, '{ print NF }')
        if [ "$nf" -ne 12 ]; then
            echo "bad row ($nf fields): $row" >&2
            exit 1
        fi
        oldIFS=$IFS
        IFS=,
        set -- $row
        IFS=$oldIFS
        echo "$spec,$seed,$1,$2,$3,$4,$5,$6,$7,$8,$load1"
    done
done

echo "end_utc=$(date -u +%FT%TZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$LOG"
