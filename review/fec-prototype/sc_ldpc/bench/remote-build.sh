#!/bin/sh
# remote-build.sh - build every synced SC-LDPC variant on the HND0 host.
#
# Layout expected:
#   /tmp/sclc-bench/src/<variant>/sc_ldpc.c sc_ldpc.h sc_ldpc_bench.c ...
#   /tmp/sclc-bench/bin/
#
# The ISA is pinned to x86-64 (SSE2) because HND0 is qemu64 with no AVX.
# No -mavx / -mavx2 / -mavx512* flag is ever passed.  The script prints a
# BUILD_OK line only after every variant linked and passed the ISA check.
set -eu

R=${R:-/tmp/sclc-bench}
CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O3 -std=c99 -Wall -Wextra -Werror -march=x86-64 -mtune=generic"}

mkdir -p "$R/bin"
echo "cc=$CC"
echo "cflags=$CFLAGS"
echo "gcc_version=$($CC --version | head -1)"

n=0
for d in "$R"/src/*; do
    [ -d "$d" ] || continue
    v=$(basename "$d")
    cd "$d"

    srcs="sc_ldpc.c sc_ldpc_bench.c"
    for f in sc_ldpc_*.c; do
        [ -f "$f" ] || continue
        case "$f" in
            sc_ldpc.c|sc_ldpc_bench.c|*_test.c) continue ;;
        esac
        srcs="$srcs $f"
    done

    echo "== variant=$v"
    echo "   srcs=$srcs"
    # shellcheck disable=SC2086
    $CC $CFLAGS $srcs -lm -pthread -o "$R/bin/$v"

    sha=$(sha256sum "$R/bin/$v" | cut -d' ' -f1)
    bytes=$(stat -c %s "$R/bin/$v")
    ymm=$(objdump -d "$R/bin/$v" | grep -cE '%[yz]mm' || true)
    vex=$(objdump -d "$R/bin/$v" | grep -cE '\bv[a-z0-9]+[[:space:]]+%xmm' || true)
    echo "BIN,$v,$sha,$bytes,ymm_zmm_refs=$ymm,vex_xmm_refs=$vex"
    if [ "$ymm" != "0" ] || [ "$vex" != "0" ]; then
        echo "ERROR: $v contains AVX encodings" >&2
        exit 3
    fi
    n=$((n + 1))
done

[ "$n" -gt 0 ] || { echo "ERROR: no variants under $R/src" >&2; exit 2; }
echo "BUILD_OK variants=$n"
