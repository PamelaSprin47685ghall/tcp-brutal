#!/bin/sh
# uplink-sampler.sh - append selected tbtun stats lines once per interval.
#
# usage: uplink-sampler.sh <tunnel> <log> <stop-file> [interval-seconds]
#
# The 1 GiB upload is a single stream: a transient carrier loss can be hidden
# by the pre/post deltas if it recovers.  This sampler keeps the c3/c6/c9/c10/
# c11 lines on disk so the verifier can prove up=256 held throughout and see
# any reset/hole/deliver_bad increment while it happened.
set -u

tunnel=${1:-1}
log=${2:?log path required}
stop=${3:?stop path required}
interval=${4:-1}
stats=${TBT_STATS:-/proc/net/tbtun/stats}

while [ ! -e "$stop" ]; do
    {
        printf '%s ' "$(date +%s)"
        awk -v t="$tunnel" '
            $1 == "tunnel" && $2 == t &&
            ($3 == "c3" || $3 == "c6" || $3 == "c9" ||
             $3 == "c10" || $3 == "c11") {
                printf "%s;", $0
            }' "$stats" 2>/dev/null || true
        printf '\n'
    } >>"$log"
    sleep "$interval"
done
