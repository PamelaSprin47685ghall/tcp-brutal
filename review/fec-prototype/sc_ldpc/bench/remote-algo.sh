#!/bin/sh
# remote-algo.sh - build and run the sclc-algo three-way A/B on HND0.
#
# Variants:
#   algo_base  baseline ef5f956 sc_ldpc.c + sclc-algo sc_ldpc_bench.c
#              (same --bench-sec window and worker affinity as the others)
#   algo_fast  sclc-algo prebuilt fast binary (source state not in git;
#              sha256 is recorded)
#   algo_opt   sclc-algo 345682d sc_ldpc.c + sc_ldpc_bench.c
#
# All runs: 2 threads, taskset 0,1, nice 10, --bench-sec 1.0, symmetric,
# 2000 trials, 4 code instances, rotating variant order each round.
set -eu

R=${R:-/tmp/sclc-bench}
. "$R/batch/run-algo.conf"
CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O3 -std=c99 -Wall -Wextra -Werror -march=x86-64 -mtune=generic"}
LOG="$R/log/$TAG-algo.err"

mkdir -p "$R/bin" "$R/log"
: > "$LOG"

echo "tag=$TAG rounds=$ROUNDS trials=$TRIALS codes=$CODES point=$BLOCK:$L:$RATE:$P" >&2
echo "start_utc=$(date -u +%FT%TZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$LOG"

check_isa() {
    b=$1
    ymm=$(objdump -d "$b" | grep -cE '%[yz]mm' || true)
    vex=$(objdump -d "$b" | grep -cE '\bv[a-z0-9]+[[:space:]]+%xmm' || true)
    echo "BIN,$(basename "$b"),$(sha256sum "$b" | cut -d' ' -f1),$(stat -c %s "$b"),ymm_zmm_refs=$ymm,vex_xmm_refs=$vex" >&2
    if [ "$ymm" != "0" ] || [ "$vex" != "0" ]; then
        echo "ERROR: $b contains AVX encodings" >&2
        exit 3
    fi
}

echo "== build algo_base (baseline decoder, candidate bench)" >&2
rm -rf "$R/src/base_algo"
mkdir -p "$R/src/base_algo"
cp "$R/src/base/sc_ldpc.c" "$R/src/base_algo/"
cp "$R/src/algo/sc_ldpc.h" "$R/src/algo/sc_ldpc_bench.c" "$R/src/base_algo/"
cd "$R/src/base_algo"
# shellcheck disable=SC2086
$CC $CFLAGS sc_ldpc.c sc_ldpc_bench.c -lm -pthread -o "$R/bin/algo_base"
check_isa "$R/bin/algo_base"

echo "== build algo_opt (345682d)" >&2
cd "$R/src/algo"
# shellcheck disable=SC2086
$CC $CFLAGS sc_ldpc.c sc_ldpc_bench.c -lm -pthread -o "$R/bin/algo_opt"
check_isa "$R/bin/algo_opt"

echo "== algo_fast (prebuilt by sclc-algo)" >&2
if [ ! -x /tmp/sc_ldpc_algo/bench_fast ]; then
    echo "ERROR: /tmp/sc_ldpc_algo/bench_fast is missing" >&2
    exit 4
fi
cp /tmp/sc_ldpc_algo/bench_fast "$R/bin/algo_fast"
check_isa "$R/bin/algo_fast"

# smoke each variant and require a 12-field row
for v in algo_base algo_fast algo_opt; do
    row=$("$R/bin/$v" --block "$BLOCK" --L "$L" --rate "$RATE" --p "$P" \
        --trials 100 --codes "$CODES" --model symmetric --term causal \
        --extra-dc 5 --threads 2 --seed "$SEED_BASE" --bench-sec 0.05 \
        --no-header 2>> "$LOG")
    nf=$(printf '%s\n' "$row" | awk -F, '{ print NF }')
    if [ "$nf" -ne 12 ]; then
        echo "ERROR: $v smoke produced $nf fields: $row" >&2
        exit 5
    fi
    echo "SMOKE,$v,$row" >&2
done

# warmup
for v in algo_base algo_fast algo_opt; do
    "$R/bin/$v" --block "$BLOCK" --L "$L" --rate "$RATE" --p "$P" \
        --trials 100 --codes "$CODES" --model symmetric --term causal \
        --extra-dc 5 --threads 2 --seed "$SEED_BASE" --bench-sec 0.20 \
        --no-header > /dev/null 2>> "$LOG"
done

echo "round,variant,block,L,rate,p,seed,load1,trials,recovery,ci99,iter,enc,dec,latency_ms,mem_bytes"

round=0
while [ "$round" -lt "$ROUNDS" ]; do
    round=$((round + 1))
    seed=$((SEED_BASE + round - 1))
    case $((round % 3)) in
        1) order="algo_base algo_fast algo_opt" ;;
        2) order="algo_fast algo_opt algo_base" ;;
        0) order="algo_opt algo_base algo_fast" ;;
    esac
    for v in $order; do
        read -r load1 _ < /proc/loadavg
        echo "### round=$round variant=$v seed=$seed" >> "$LOG"
        row=$(taskset -c 0,1 nice -n 10 "$R/bin/$v" \
            --block "$BLOCK" --L "$L" --rate "$RATE" --p "$P" \
            --trials "$TRIALS" --codes "$CODES" --model symmetric \
            --term causal --extra-dc 5 --threads 2 --seed "$seed" \
            --bench-sec 1.0 --no-header 2>> "$LOG")
        nf=$(printf '%s\n' "$row" | awk -F, '{ print NF }')
        if [ "$nf" -ne 12 ]; then
            echo "ERROR: $v round $round produced $nf fields: $row" >&2
            exit 6
        fi
        oldIFS=$IFS
        IFS=,
        set -- $row
        IFS=$oldIFS
        echo "$round,$v,$1,$2,$3,$4,$seed,$load1,$5,$6,$7,$8,$9,${10},${11},${12}"
    done
done

echo "end_utc=$(date -u +%FT%TZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$LOG"
