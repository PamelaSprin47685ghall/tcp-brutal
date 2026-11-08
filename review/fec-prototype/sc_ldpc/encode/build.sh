#!/bin/sh
# Build the SC-LDPC prototype.  Host-only C99, no external dependencies.
#   ./build.sh
#   ./sc_ldpc_bench --block 256 --L 8 --rate 0.50 --p 0.30 --trials 2000
#   ./sc_ldpc_bench --full --report sc-ldpc-sim.csv
#   ./sc_enc64_test                      bit-exact 64-codeword encoder
#   ./sc_enc64_bench --block 256 --L 16 --rate 0.50 --runs 5
set -e
cd "$(dirname "$0")"
CC=${CC:-cc}
FLAGS="-O3 -std=c99 -Wall -Wextra -Werror"
$CC $FLAGS sc_ldpc.c sc_ldpc_bench.c -lm -pthread -o sc_ldpc_bench
$CC $FLAGS sc_ldpc.c sc_enc64.c sc_enc64_pack.c sc_enc64_sse2.c \
    sc_enc64_test.c -lm -pthread -o sc_enc64_test
$CC $FLAGS sc_ldpc.c sc_enc64.c sc_enc64_pack.c sc_enc64_sse2.c \
    sc_enc128_test.c -lm -pthread -o sc_enc128_test
$CC $FLAGS sc_ldpc.c sc_enc64.c sc_enc64_pack.c sc_enc64_sse2.c \
    sc_enc64_bench.c -lm -pthread -o sc_enc64_bench
echo "built ./sc_ldpc_bench ./sc_enc64_test ./sc_enc128_test ./sc_enc64_bench"
