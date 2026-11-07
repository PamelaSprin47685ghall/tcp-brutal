#!/bin/sh
# far-stream-watch.sh - sample the far per-stream line around the 1 GiB run.
#
# The harness starts this before the download and touches $STOP after it.
# The watcher keeps the last live "tunnel 1 stream ..." line (all fields,
# including tgt_state/tgt_err/tx_seq/tx_fin_written/close_since_ns/
# fec_out_queued/fec_outq_queued when the build exports them), and records
# the transition when the line disappears. It only reads
# /proc/net/tbtun/stats; it never writes to the tunnel.
#
# Files:
#   $OUT     start/first/transition/stop markers
#   $LAST    the last live per-stream line (full, all fields)
#   $FIELDS  the named fields extracted from the last live line
set -u
OUT=${OUT:-/root/accept-1g/far-stream-watch.txt}
LAST=${LAST:-/root/accept-1g/far-stream-watch.last}
FIELDS=${FIELDS:-/root/accept-1g/far-stream-watch.fields}
STOP=${STOP:-/root/accept-1g/.far-stream-watch.stop}
STATS=${STATS:-/proc/net/tbtun/stats}
INTERVAL=${INTERVAL:-0.2}
FIELDS_RE='(tgt_state|tgt_err|tx_seq|tx_fin_written|close_since_ns|fec_out_queued|fec_outq_queued)=[^ ]*'

: >"$OUT"
: >"$LAST"
: >"$FIELDS"
printf '%s\tWATCH_START\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$OUT"
seen=0
last=""

save_last() {
    [ -n "$last" ] || return 0
    printf '%s\n' "$last" >"$LAST"
    printf '%s\n' "$last" | grep -oE "$FIELDS_RE" | tr '\n' ' ' >>"$FIELDS"
    printf '\n' >>"$FIELDS"
}

while :; do
    if [ -e "$STOP" ]; then
        save_last
        printf '%s\tWATCH_STOP\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$OUT"
        exit 0
    fi
    ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    line=$(awk '/^tunnel 1 stream /{print; exit}' "$STATS" 2>/dev/null || true)
    if [ -n "$line" ]; then
        last="$ts	$line"
        printf '%s\n' "$last" >"$LAST"
        if [ "$seen" -eq 0 ]; then
            printf '%s\tFIRST_STREAM\n' "$ts" >>"$OUT"
            seen=1
        fi
    elif [ "$seen" -eq 1 ]; then
        save_last
        printf '%s\tNO_STREAM_LINE\n' "$ts" >>"$OUT"
        break
    fi
    sleep "$INTERVAL"
done
