#!/bin/sh
# Build the SC-LDPC prototype.  Host-only C99, no external dependencies.
#   ./build.sh
#   ./sc_ldpc_bench --block 256 --L 8 --rate 0.50 --p 0.30 --trials 2000
#   ./sc_ldpc_bench --full --report sc-ldpc-sim.csv
set -e
cd "$(dirname "$0")"
CC=${CC:-cc}
$CC -O3 -std=c99 -Wall -Wextra -Werror sc_ldpc.c sc_ldpc_bench.c -lm -pthread \
    -o sc_ldpc_bench
echo "built ./sc_ldpc_bench"
