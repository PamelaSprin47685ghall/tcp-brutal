#!/bin/sh
# Build and run the Bernstein-reference ChaCha benchmark.
# Usage: ./run_chacha_ref.sh [--bytes MiB] [--reps N]
set -e
CC=${CC:-cc}
CFLAGS="-O3 -march=native"
OUT=./chacha_ref_bench

build_one() {
	src=$1; tag=$2
	$CC $CFLAGS -I chacha-ref -c "chacha-ref/$src" -o "/tmp/chacha-$tag.o" \
		-DECRYPT_init="ref${tag}_init" \
		-DECRYPT_keysetup="ref${tag}_keysetup" \
		-DECRYPT_ivsetup="ref${tag}_ivsetup" \
		-DECRYPT_encrypt_bytes="ref${tag}_encrypt_bytes" \
		-DECRYPT_decrypt_bytes="ref${tag}_decrypt_bytes" \
		-DECRYPT_keystream_bytes="ref${tag}_keystream_bytes"
}

build_one chacha8.c 8
build_one chacha12.c 12
build_one chacha20.c 20
$CC $CFLAGS -I chacha-ref -o "$OUT" chacha_ref_bench.c \
	/tmp/chacha-8.o /tmp/chacha-12.o /tmp/chacha-20.o
rm -f /tmp/chacha-8.o /tmp/chacha-12.o /tmp/chacha-20.o

for i in 1 2 3; do
	echo "### run $i"
	timeout "${CASE_TIMEOUT:-30}" ./"$OUT" "$@"
done
