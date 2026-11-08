#!/bin/sh
# Build the GF(16) SC-IRA prototype for ARM64 / HND0.
#   ./build_arm64.sh [CROSS]
#   CROSS=1  -> aarch64-linux-gnu-gcc, -O3 -march=armv8-a+crypto
#   default  -> native cc
set -e
cd "$(dirname "$0")"

if [ "${CROSS:-0}" = "1" ]; then
    CC=${CC:-aarch64-linux-gnu-gcc}
    CFLAGS="-O3 -std=c99 -Wall -Wextra -Werror -march=armv8-a+crypto -mtune=generic"
else
    CC=${CC:-cc}
    CFLAGS="-O3 -std=c99 -Wall -Wextra -Werror"
fi

$CC $CFLAGS sc_ldpc.c sc_ldpc_gf16.c sc_ldpc_gf16_bench.c -lm -pthread \
    -o sc_ldpc_gf16_bench_arm64
echo "built ./sc_ldpc_gf16_bench_arm64"
