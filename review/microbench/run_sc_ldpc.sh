#!/bin/sh
# Run the SC-LDPC bench matrix (fec-core2's sc_ldpc_bench) on one host.
# Usage: nice -n 19 taskset -c N ./run_sc_ldpc.sh ./sc_ldpc_bench [trials]
# The binary must accept:
#   --block B --L L --rate R --p P --trials T
# and print one CSV row per case.
set -e
BIN=${1:?usage: run_sc_ldpc.sh <binary> [trials]}
TRIALS=${2:-2000}

for block in 256 512; do
	for L in 8 16 32; do
		for rate in 0.50 0.40 0.35; do
			for p in 0.30 0.40 0.50; do
				"$BIN" --block "$block" --L "$L" \
				       --rate "$rate" --p "$p" \
				       --trials "$TRIALS"
			done
		done
	done
done
