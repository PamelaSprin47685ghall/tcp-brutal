#!/bin/sh
# perf-curl-loop.sh - keep one direct GET running through the whole window.
#
# Usage: perf-curl-loop.sh URL PIDFILE OUTFILE MAXTIME STOPFILE
#
# A 1 GiB source can finish inside a 60 s window on a 75-115 Mbit/s link.
# One GET then leaves the second half of the window idle, and the final
# /proc/PID/io read fails because curl is gone.  This loop starts the next
# GET immediately after the previous one completes, so the window always
# carries traffic.  Each request appends its curl -w line to OUTFILE; the
# caller sums those sizes and adds the current request's wchar delta.
#
# PIDFILE holds the current curl pid.  PIDFILE.loop holds this loop's pid.
# A request that ends with rc=0 (complete) or rc=28 (-m timeout) is followed
# by another request; any other rc stops the loop.
set -u

URL=${1:?usage: perf-curl-loop.sh URL PIDFILE OUTFILE MAXTIME STOPFILE}
PIDFILE=${2:?usage: perf-curl-loop.sh URL PIDFILE OUTFILE MAXTIME STOPFILE}
OUTFILE=${3:?usage: perf-curl-loop.sh URL PIDFILE OUTFILE MAXTIME STOPFILE}
MAXTIME=${4:?usage: perf-curl-loop.sh URL PIDFILE OUTFILE MAXTIME STOPFILE}
STOPFILE=${5:?usage: perf-curl-loop.sh URL PIDFILE OUTFILE MAXTIME STOPFILE}
HERE=$(cd -- "$(dirname -- "$0")" && pwd)

printf '%s\n' "$$" >"$PIDFILE.loop"
: >"$OUTFILE"

while [ ! -e "$STOPFILE" ]; do
    TMP=$OUTFILE.tmp
    rm -f "$TMP"
    sh "$HERE/perf-curl.sh" "$URL" "$PIDFILE" "$TMP" "$MAXTIME"
    rc=$?
    if [ -f "$TMP" ]; then
        cat "$TMP" >>"$OUTFILE"
    fi
    rm -f "$TMP"
    case $rc in
    0 | 28) ;;
    *) break ;;
    esac
done
rm -f "$PIDFILE.loop"
