#!/bin/sh
# Build the GF(16) SC-IRA prototype.  Host-only C99, no external deps.
#   ./build_gf16.sh
#   ./sc_ldpc_gf16_bench --block 256 --L 16 --rate 0.50 --p 0.30 \
#       --trials 2000 --model symmetric
set -e
cd "$(dirname "$0")"
CC=${CC:-cc}
$CC -O3 -std=c99 -Wall -Wextra -Werror -march=x86-64 -mtune=generic \
    sc_ldpc.c sc_ldpc_gf16.c sc_ldpc_gf16_bench.c -lm -pthread \
    -o sc_ldpc_gf16_bench
echo "built ./sc_ldpc_gf16_bench"
