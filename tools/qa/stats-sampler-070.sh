#!/bin/sh
# stats-sampler-070.sh - timestamped c3/c6/c9/c10/c11/c12 samples.
#
# usage: stats-sampler-070.sh <tunnel> <log> <stop-file> [interval-seconds]
#
# The pre/post deltas hide *when* a long run lost its rate. This sampler
# keeps one line per interval with the classes that name the loss, so a
# 1 GiB downlink can be attributed in time: c3 carrier stalls, c9 credit
# waits, c10 decode/late shares, c11 NACK/replay, c12 outq/slow/txring.
set -u

tunnel=${1:-1}
log=${2:?log path required}
stop=${3:?stop path required}
interval=${4:-10}
stats=${TBT_STATS:-/proc/net/tbtun/stats}

while [ ! -e "$stop" ]; do
    {
        printf '%s ' "$(date +%s)"
        awk -v t="$tunnel" '
            $1 == "tunnel" && $2 == t &&
            ($3 == "c3" || $3 == "c6" || $3 == "c9" ||
             $3 == "c10" || $3 == "c11" || $3 == "c12") {
                printf "%s;", $0
            }' "$stats" 2>/dev/null || true
        printf '\n'
    } >>"$log"
    sleep "$interval"
done
