#!/bin/sh
# perf-sampler.sh - write one compact /proc/net/tbtun sample per second.
#
# Runs on near or far while a steady window is open. perf-ab.sh starts it
# before the warm-up and stops it after the measurement window. The log gives
# the per-second up count and the link counters between the two snapshots, so
# a carrier that drops for 2 s inside a 60 s window is visible.
#
# Usage:
#   perf-sampler.sh STATS LOG STOP [INTERVAL]
#
#   STATS     /proc/net/tbtun/stats
#   LOG       output file, appended
#   STOP      stop when this file exists
#   INTERVAL  seconds between samples, default 1
#
# Output: one header line, then one line per sample. Fields are positional:
#
#   epoch c3_up c3_replacing c3_empty c3_lost c3_replaced
#   c3_stall_declared c3_stall_recovered c3_stall_dead
#   c6_holes_unrecoverable c6_rx_max_stall_us
#   c11_retx_bytes c11_nack_tx c11_nack_rx c11_nack_drop c11_replay_miss
#   c9_ack_tx c9_ack_rx load1
#
# A field the build does not export prints "-". A missing STATS file prints
# one line of "-" fields; the sampler keeps running, because the reader must
# see that the window had no stats rather than an empty log.
set -u

STATS=${1:-/proc/net/tbtun/stats}
LOG=${2:?usage: perf-sampler.sh STATS LOG STOP [INTERVAL]}
STOP=${3:?usage: perf-sampler.sh STATS LOG STOP [INTERVAL]}
INTERVAL=${4:-1}

printf '%s\n' \
    '# epoch c3_up c3_replacing c3_empty c3_lost c3_replaced' \
    '# c3_stall_declared c3_stall_recovered c3_stall_dead' \
    '# c6_holes_unrecoverable c6_rx_max_stall_us' \
    '# c11_retx_bytes c11_nack_tx c11_nack_rx c11_nack_drop c11_replay_miss' \
    '# c9_ack_tx c9_ack_rx load1' >"$LOG"

while [ ! -e "$STOP" ]; do
    now=$(date +%s)
    load=$(cut -d' ' -f1 /proc/loadavg 2>/dev/null || printf '-')
    awk -v ep="$now" -v ld="$load" '
        $1 == "tunnel" && $2 == 1 {
            cls = $3
            for (i = 4; i <= NF; i++) {
                if (split($i, kv, "=") == 2) v[cls, kv[1]] = kv[2]
            }
        }
        function g(c, k) { return ((c, k) in v) ? v[c, k] : "-" }
        END {
            printf "%s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s\n",
                ep,
                g("c3", "up"), g("c3", "replacing"), g("c3", "empty"),
                g("c3", "lost"), g("c3", "replaced"),
                g("c3", "stall_declared"), g("c3", "stall_recovered"),
                g("c3", "stall_dead"),
                g("c6", "holes_unrecoverable"), g("c6", "rx_max_stall_us"),
                g("c11", "fec_nack_retx_bytes"),
                g("c11", "fec_nack_tx"), g("c11", "fec_nack_rx"),
                g("c11", "fec_nack_drop"), g("c11", "fec_replay_miss"),
                g("c9", "fec_ack_tx"), g("c9", "fec_ack_rx"),
                ld
        }' "$STATS" >>"$LOG" 2>/dev/null || {
        printf '%s - - - - - - - - - - - - - - - - - - %s\n' "$now" "$load" >>"$LOG"
    }
    sleep "$INTERVAL"
done
