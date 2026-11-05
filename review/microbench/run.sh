#!/bin/sh
# Compile and run the tbtun microbenchmark in the current directory.
# Usage: ./run.sh [--mem MiB] [--threads N]
# The caller is expected to wrap this in `nice -n 19 taskset -c ...`.
set -e
CC=${CC:-cc}
OUT=${OUT:-./microbench}

if ! $CC -O3 -march=native -pthread -o "$OUT" microbench.c 2>/tmp/mb-build.log; then
	echo "native build failed, falling back to -O3" >&2
	$CC -O3 -pthread -o "$OUT" microbench.c
fi
exec ./"$OUT" "$@"
