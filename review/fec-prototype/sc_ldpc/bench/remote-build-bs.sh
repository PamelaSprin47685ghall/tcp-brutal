#!/bin/sh
# remote-build-bs.sh - build the scalar baseline and the bit-sliced candidate
# on the HND0 host, then run the candidate's own correctness test.
#
# Layout:
#   /tmp/sclc-bench/src/base  (scalar baseline, sc_ldpc_bench.c)
#   /tmp/sclc-bench/src/cand  (bit-sliced candidate, sc_ldpc_bs_bench.c)
#
# ISA is pinned to x86-64 (SSE2); HND0 is qemu64 with no AVX.
set -eu

R=${R:-/tmp/sclc-bench}
CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O3 -std=c99 -Wall -Wextra -Werror -march=x86-64 -mtune=generic"}
TAG=${TAG:-bs-ab}

mkdir -p "$R/bin" "$R/log"
echo "cc=$CC"
echo "cflags=$CFLAGS"
echo "gcc_version=$($CC --version | head -1)"

check_isa() {
    b=$1
    ymm=$(objdump -d "$b" | grep -cE '%[yz]mm' || true)
    vex=$(objdump -d "$b" | grep -cE '\bv[a-z0-9]+[[:space:]]+%xmm' || true)
    echo "BIN,$(basename "$b"),$(sha256sum "$b" | cut -d' ' -f1),$(stat -c %s "$b"),ymm_zmm_refs=$ymm,vex_xmm_refs=$vex"
    if [ "$ymm" != "0" ] || [ "$vex" != "0" ]; then
        echo "ERROR: $b contains AVX encodings" >&2
        exit 3
    fi
}

echo "== base: scalar baseline"
cd "$R/src/base"
# shellcheck disable=SC2086
$CC $CFLAGS sc_ldpc.c sc_ldpc_bench.c -lm -pthread -o "$R/bin/base_bench"
check_isa "$R/bin/base_bench"

echo "== cand: bit-sliced candidate"
cd "$R/src/cand"
# shellcheck disable=SC2086
$CC $CFLAGS sc_ldpc.c sc_ldpc_bs.c sc_ldpc_bs_test.c -lm -pthread \
    -o "$R/bin/cand_bs_test"
# shellcheck disable=SC2086
$CC $CFLAGS sc_ldpc.c sc_ldpc_bs.c sc_ldpc_bs_bench.c -lm -pthread \
    -o "$R/bin/cand_bs_bench"
# shellcheck disable=SC2086
$CC $CFLAGS sc_ldpc.c sc_ldpc_bench.c -lm -pthread -o "$R/bin/cand_bench"
check_isa "$R/bin/cand_bs_test"
check_isa "$R/bin/cand_bs_bench"
check_isa "$R/bin/cand_bench"

echo "== correctness test"
if ! "$R/bin/cand_bs_test" > "$R/log/$TAG-bstest.log" 2>&1; then
    cat "$R/log/$TAG-bstest.log" >&2
    echo "ERROR: cand_bs_test failed" >&2
    exit 4
fi
tail -3 "$R/log/$TAG-bstest.log"
grep -q 'RESULT: PASS' "$R/log/$TAG-bstest.log" || {
    echo "ERROR: cand_bs_test did not print RESULT: PASS" >&2
    exit 5
}

echo "BUILD_OK"
