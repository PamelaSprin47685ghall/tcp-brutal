#!/bin/sh
# test_http_live.sh - host-side conformance runner for the http-kal container.
#
#   tests/unit/test_http_live.sh [capture]
#
# Always runs three synthetic captures produced by the codec:
#   - --emit-req/--emit-resp framed as TBT-HTTP-CAPTURE 1 (2 DATA chunks)
#   - --capture (PAD + drip + 2 DATA chunks)
#   - --capture-text (frozen proc text, first chunk is the PAD chunk)
# and, when a capture path is given, that file through --from-proc.
#
# The C harness and the optional h11 oracle both read the same file. The last
# line is exactly:
#   LIVE checks=N failures=M skips=K
#
# The Makefile is owned by the fuzz work, so the live binary is compiled here
# directly; only the existing tests/unit/test_http target is used.
set -u

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root" || exit 1

make -s tests/unit/test_http || exit 1

CC=${CC:-cc}
$CC -std=gnu99 -O2 -Wall -Wextra -Werror -I tbtun -o tests/unit/test_http_live \
    tests/unit/test_http_live.c src/kernel/tbt_http.c src/kernel/tbt_proto.c src/kernel/tbt_rec.c || exit 1

tmp=$(mktemp -d "${TMPDIR:-/tmp}/tbt-http-live.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

checks=0
failures=0
skips=0
h11_fail=0

run_c() {
    name=$1
    file=$2
    mode=${3:-stdin}
    if [ "$mode" = proc ]; then
        out=$(./tests/unit/test_http_live --from-proc "$file")
    else
        out=$(./tests/unit/test_http_live < "$file")
    fi
    rc=$?
    printf '%s\n' "$out"
    line=$(printf '%s\n' "$out" |
        sed -n 's/^test_http_live: checks=\([0-9][0-9]*\) failures=\([0-9][0-9]*\) skips=\([0-9][0-9]*\)$/\1 \2 \3/p' |
        tail -1)
    if [ -z "$line" ]; then
        echo "LIVE-INTERNAL: $name produced no summary line" >&2
        failures=$((failures + 1))
        return
    fi
    set -- $line
    checks=$((checks + $1))
    failures=$((failures + $2))
    skips=$((skips + $3))
    if [ "$rc" -ne 0 ] && [ "$2" -eq 0 ]; then
        failures=$((failures + 1))
    fi
}

run_h11() {
    file=$1
    if command -v uv >/dev/null 2>&1 &&
        uv run --offline --no-project --with h11 -q python -c 'import h11' >/dev/null 2>&1; then
        if ! uv run --offline --no-project --with h11 -q python tests/unit/test_http_live_h11.py "$file"; then
            h11_fail=$((h11_fail + 1))
        fi
    else
        echo "h11: SKIP uv or cached h11 unavailable"
    fi
}

# 1. The plain --emit pair, framed for the binary capture parser.
./tests/unit/test_http --emit-req > "$tmp/emit.req" || exit 1
./tests/unit/test_http --emit-resp > "$tmp/emit.resp" || exit 1
{
    printf 'TBT-HTTP-CAPTURE 1\nrequest %s\n' "$(wc -c < "$tmp/emit.req" | tr -d ' ')"
    cat "$tmp/emit.req"
    printf 'response %s\n' "$(wc -c < "$tmp/emit.resp" | tr -d ' ')"
    cat "$tmp/emit.resp"
} > "$tmp/emit.bin"
run_c emit "$tmp/emit.bin"
run_h11 "$tmp/emit.bin"

# 2. PAD + drip + two DATA records.
./tests/unit/test_http --capture > "$tmp/capture.bin" || exit 1
run_c capture "$tmp/capture.bin"
run_h11 "$tmp/capture.bin"

# 3. Frozen proc text (first chunk is the PAD chunk).
./tests/unit/test_http --capture-text > "$tmp/capture.txt" || exit 1
run_c text "$tmp/capture.txt"
run_h11 "$tmp/capture.txt"

# 4. A real capture, if one was saved. --from-proc auto-detects frozen text,
#    the current raw request-header block, or the binary framing.
if [ $# -ge 1 ]; then
    run_c proc "$1" proc
    run_h11 "$1"
fi

echo "LIVE checks=$checks failures=$failures skips=$skips"
[ "$failures" -eq 0 ] && [ "$h11_fail" -eq 0 ]
