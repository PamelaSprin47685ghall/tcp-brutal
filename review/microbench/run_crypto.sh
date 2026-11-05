#!/bin/sh
# Compile and run the ChaCha microbenchmark in the current directory.
# Three invocations; the caller takes the median of the reported medians.
# Usage: ./run_crypto.sh [--bytes MiB] [--reps N]
set -e
CC=${CC:-cc}
OUT=${OUT:-./crypto}

if ! $CC -O3 -march=native -o "$OUT" crypto.c 2>/tmp/crypto-build.log; then
	echo "native build failed, falling back to -O3" >&2
	$CC -O3 -o "$OUT" crypto.c
fi
for i in 1 2 3; do
	echo "### run $i"
	./"$OUT" "$@"
done
