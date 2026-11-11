#!/bin/sh
# check-bytes.sh - sha256 / byte-count gate for the adaptive-FEC benches.
#
# Three ways to answer the same question: are these the bytes the far source
# sent, in order, with nothing lost or corrupted?
#
#   1. two hashes:      check-bytes.sh --sha HEX --expect HEX \
#                                       [--bytes N --expect-bytes M]
#   2. a local file:    check-bytes.sh --file PATH --expect HEX [--bytes N]
#   3. near and far:    check-bytes.sh --near HOST --near-file PATH \
#                                       --far HOST --far-source PATH [--bytes N]
#
# Mode 3 hashes the near file and `head -c N <far-source> | sha256sum` on the
# far host, so a truncated but in-order transfer still verifies as a prefix.
# N defaults to the near file size.
#
# Output (one line, machine-readable):
#   BYTES PASS sha=<hex> bytes=<n>
#   BYTES FAIL reason=<...> sha=<hex> expect=<hex> bytes=<n>
# Exit codes: 0 PASS, 2 usage, 3 FAIL. Callers stop the run on 3.
set -eu

PROG=${0##*/}
NEAR=
FAR=
NEAR_FILE=
FAR_SOURCE=
SHA=
EXPECT=
FILE=
BYTES=
EXPECT_BYTES=
LABEL=

die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 2; }
fail() {
    printf 'BYTES FAIL reason=%s sha=%s expect=%s bytes=%s%s\n' \
        "$1" "${SHA:--}" "${EXPECT:--}" "${BYTES:-0}" "${LABEL:+ label=$LABEL}"
    exit 3
}
pass() {
    printf 'BYTES PASS sha=%s bytes=%s%s\n' "$SHA" "${BYTES:-0}" "${LABEL:+ label=$LABEL}"
    exit 0
}

while [ $# -gt 0 ]; do
    case $1 in
    --near) NEAR=$2; shift 2 ;;
    --far) FAR=$2; shift 2 ;;
    --near-file) NEAR_FILE=$2; shift 2 ;;
    --far-source) FAR_SOURCE=$2; shift 2 ;;
    --sha) SHA=$2; shift 2 ;;
    --expect) EXPECT=$2; shift 2 ;;
    --file) FILE=$2; shift 2 ;;
    --bytes) BYTES=$2; shift 2 ;;
    --expect-bytes) EXPECT_BYTES=$2; shift 2 ;;
    --label) LABEL=$2; shift 2 ;;
    -h | --help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option $1" ;;
    esac
done

# `sha256sum < fifo` prints "HEX  -"; accept a raw sha256sum line and compare
# only the digest. A bare hex string is unchanged.
SHA=$(printf '%s' "$SHA" | awk 'NR == 1 { print $1 }')
EXPECT=$(printf '%s' "$EXPECT" | awk 'NR == 1 { print $1 }')

rsh() {
    rh=$1
    shift
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$rh" "$@"
}

if [ -n "$NEAR_FILE" ] || [ -n "$FAR_SOURCE" ]; then
    [ -n "$NEAR" ] && [ -n "$FAR" ] && [ -n "$NEAR_FILE" ] && [ -n "$FAR_SOURCE" ] ||
        die "mode 3 needs --near --near-file --far --far-source"
    near_bytes=$(rsh "$NEAR" "stat -c %s '$NEAR_FILE' 2>/dev/null || echo 0") || die "cannot stat $NEAR_FILE on $NEAR"
    [ -n "$BYTES" ] || BYTES=$near_bytes
    [ "$near_bytes" = "$BYTES" ] ||
        fail "near-size"
    far_bytes=$(rsh "$FAR" "stat -c %s '$FAR_SOURCE' 2>/dev/null || echo 0") || die "cannot stat $FAR_SOURCE on $FAR"
    [ "$BYTES" -le "$far_bytes" ] || fail "bytes-gt-source"
    SHA=$(rsh "$NEAR" "sha256sum '$NEAR_FILE' | awk '{print \$1}'") || die "sha256 on $NEAR failed"
    EXPECT=$(rsh "$FAR" "head -c $BYTES '$FAR_SOURCE' | sha256sum | awk '{print \$1}'") || die "prefix sha256 on $FAR failed"
    [ -n "$SHA" ] && [ -n "$EXPECT" ] || fail "empty-hash"
    [ "$SHA" = "$EXPECT" ] || fail "sha-mismatch"
    pass
fi

if [ -n "$FILE" ]; then
    [ -n "$EXPECT" ] || die "mode 2 needs --expect"
    [ -f "$FILE" ] || die "no such file: $FILE"
    file_bytes=$(stat -c %s "$FILE")
    if [ -n "$BYTES" ]; then
        [ "$BYTES" -le "$file_bytes" ] || fail "bytes-gt-file"
        SHA=$(head -c "$BYTES" "$FILE" | sha256sum | awk '{print $1}')
    else
        BYTES=$file_bytes
        SHA=$(sha256sum "$FILE" | awk '{print $1}')
    fi
    [ "$SHA" = "$EXPECT" ] || fail "sha-mismatch"
    pass
fi

[ -n "$SHA" ] && [ -n "$EXPECT" ] || die "give --file, --near/--far, or --sha/--expect"
if [ -n "$BYTES" ] && [ -n "$EXPECT_BYTES" ]; then
    [ "$BYTES" = "$EXPECT_BYTES" ] || fail "byte-count"
fi
[ "$SHA" = "$EXPECT" ] || fail "sha-mismatch"
pass
