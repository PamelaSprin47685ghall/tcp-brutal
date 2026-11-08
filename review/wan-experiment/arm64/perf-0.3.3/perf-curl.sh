#!/bin/sh
# perf-curl.sh - one bounded GET with a pidfile and a curl -w result line.
#
# Usage: perf-curl.sh URL PIDFILE OUTFILE MAXTIME
#
# The caller starts it with setsid, then samples /proc/$(cat PIDFILE)/io.
# `exec` keeps the curl pid equal to the pid written here, so the wchar
# reading belongs to this transfer and not to a wrapper shell.
#
# OUTFILE receives the -w line only when curl exits on its own. A window
# that kills curl at its end gets no line; the window's byte count comes
# from the two /proc/PID/io snapshots instead.
set -u

URL=${1:?usage: perf-curl.sh URL PIDFILE OUTFILE MAXTIME}
PIDFILE=${2:?usage: perf-curl.sh URL PIDFILE OUTFILE MAXTIME}
OUTFILE=${3:?usage: perf-curl.sh URL PIDFILE OUTFILE MAXTIME}
MAXTIME=${4:-600}

printf '%s\n' "$$" >"$PIDFILE"
exec curl -sS -m "$MAXTIME" -o /dev/null \
    -w 'http=%{http_code} size=%{size_download} ttfb=%{time_starttransfer} time=%{time_total} speed=%{speed_download}\n' \
    "$URL" >"$OUTFILE" 2>&1
