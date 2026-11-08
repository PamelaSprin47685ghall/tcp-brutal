#!/bin/sh
# remote-enc64.sh - build and run the sclc-encode candidate on HND0.
#
# The bench measures scalar_ref, enc64, e2e and sse2 in the same process,
# so the scalar-vs-candidate comparison is paired by construction.  We add
# outer rounds to expose host drift and record load1 per invocation.
set -eu

R=${R:-/tmp/sclc-bench}
. "$R/batch/run-enc64.conf"
CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O3 -std=c99 -Wall -Wextra -Werror -march=x86-64 -mtune=generic"}
LOG="$R/log/$TAG-enc64.err"

mkdir -p "$R/bin" "$R/log"
: > "$LOG"
echo "tag=$TAG rounds=$ROUNDS rates=$RATES seconds=$BENCH_SEC runs=$RUNS" >&2
echo "start_utc=$(date -u +%FT%TZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$LOG"

cd "$R/src/enc64"
# shellcheck disable=SC2086
$CC $CFLAGS sc_ldpc.c sc_enc64.c sc_enc64_pack.c sc_enc64_sse2.c \
    sc_enc64_test.c -lm -pthread -o "$R/bin/enc64_test"
# shellcheck disable=SC2086
$CC $CFLAGS sc_ldpc.c sc_enc64.c sc_enc64_pack.c sc_enc64_sse2.c \
    sc_enc64_bench.c -lm -pthread -o "$R/bin/enc64_bench"

for b in "$R/bin/enc64_test" "$R/bin/enc64_bench"; do
    ymm=$(objdump -d "$b" | grep -cE '%[yz]mm' || true)
    vex=$(objdump -d "$b" | grep -cE '\bv[a-z0-9]+[[:space:]]+%xmm' || true)
    echo "BIN,$(basename "$b"),$(sha256sum "$b" | cut -d' ' -f1),$(stat -c %s "$b"),ymm_zmm_refs=$ymm,vex_xmm_refs=$vex" >&2
    [ "$ymm" = 0 ] && [ "$vex" = 0 ] || { echo "AVX found in $b" >&2; exit 3; }
done

if ! taskset -c 0 nice -n 10 "$R/bin/enc64_test" > "$R/log/$TAG-enc64-test.log" 2>&1; then
    tail -20 "$R/log/$TAG-enc64-test.log" >&2
    echo "ERROR: enc64_test failed" >&2
    exit 4
fi
tail -2 "$R/log/$TAG-enc64-test.log" >&2
grep -q 'RESULT: PASS' "$R/log/$TAG-enc64-test.log" || {
    echo "ERROR: enc64_test did not print RESULT: PASS" >&2
    exit 5
}

get() {
    printf '%s\n' "$out" | awk -F= -v k="$1" '$1 == k { print $2 }'
}

# warmup per thread setting
for threads in $THREADS_LIST; do
    case "$threads" in 1) cpus=0 ;; 2) cpus=0,1 ;; *) exit 2 ;; esac
    taskset -c "$cpus" nice -n 10 "$R/bin/enc64_bench" --block "$BLOCK" \
        --L "$L" --rate 0.50 --seed "$SEED_BASE" --seconds 0.20 --runs 1 \
        --threads "$threads" > /dev/null 2>> "$LOG"
done

echo "round,threads,block,L,rate,seed,load1,scalar_ref,enc64,e2e,sse2,enc64_mt"
for threads in $THREADS_LIST; do
    case "$threads" in 1) cpus=0 ;; 2) cpus=0,1 ;; *) exit 2 ;; esac
    for rate in $RATES; do
        round=0
        while [ "$round" -lt "$ROUNDS" ]; do
            round=$((round + 1))
            seed=$((SEED_BASE + round - 1))
            read -r load1 _ < /proc/loadavg
            echo "### r=$round t=$threads rate=$rate seed=$seed" >> "$LOG"
            out=$(taskset -c "$cpus" nice -n 10 "$R/bin/enc64_bench" \
                --block "$BLOCK" --L "$L" --rate "$rate" --seed "$seed" \
                --seconds "$BENCH_SEC" --runs "$RUNS" --threads "$threads" \
                2>> "$LOG")
            scalar=$(get scalar_ref_mbps)
            enc64=$(get enc64_mbps)
            e2e=$(get e2e_mbps)
            sse2=$(get sse2_mbps)
            mt=$(get enc64_mt_mbps)
            for x in "$scalar" "$enc64" "$e2e" "$sse2" "$mt"; do
                case "$x" in ''|*[!0-9.]*) echo "bad metric: $x" >&2; exit 6 ;; esac
            done
            echo "$round,$threads,$BLOCK,$L,$rate,$seed,$load1,$scalar,$enc64,$e2e,$sse2,$mt"
        done
    done
done

echo "end_utc=$(date -u +%FT%TZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$LOG"
