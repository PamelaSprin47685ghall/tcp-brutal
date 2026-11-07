#!/bin/sh
# far-serve.sh - the 1 GiB HTTP source for the 0.2.0 acceptance run.
#
# Runs on the far host (45.11.1.62). Creates a 1 GiB random file under /root
# (never /tmp: far's /tmp is a 478 M tmpfs and would hit ENOSPC), publishes
# its sha256, and serves it from far-raw-serve.py: a loopback-only HTTP
# sender with no application-level write timeout. Busybox httpd's write path
# is not used; the 1 GiB body must never be cut by a server timeout.
#
# The tunnel does not reach this server directly: tbtun's far side is
# configured with target=127.0.0.1:5210, so the HTTP request crosses the
# tunnel and is re-originated here by the far side.
#
# Modes
#   --start    create/verify the file, hash it, start the raw sender, prove readiness
#   --stop     stop the raw sender and free the port
#   --status   read-only report
#   --hash     print the sha256 only
#
# Output markers (machine-judged by run-1g.sh):
#   FAR_CHECK <name> PASS|FAIL (detail)
#   FAR_SHA256=<hex>
#   FAR_SIZE=<bytes>
#   FAR_HTTPD_PID=<pid>
#   FAR_RESULT PASS|FAIL
set -eu

MODE=start
FAR_DIR=${FAR_DIR:-/root/accept-1g}
FILE=${FILE:-accept-1g.bin}
SIZE_MIB=${SIZE_MIB:-1024}
PORT=${PORT:-5210}
BIND=${BIND:-127.0.0.1}
BYTES=$((SIZE_MIB * 1024 * 1024))
PATH_FILE=$FAR_DIR/$FILE
SHA_FILE=$PATH_FILE.sha256
PID_FILE=$FAR_DIR/httpd.pid
HTTP_LOG=$FAR_DIR/httpd.log
RAW_PY=${RAW_PY:-/root/far-raw-serve.py}
RAW_ERR=$FAR_DIR/raw-serve.err

FAILED=0

say() { printf '%s\n' "$*"; }
check() {
    name=$1
    rc=$2
    detail=${3:-}
    if [ "$rc" -eq 0 ]; then
        say "FAR_CHECK $name PASS${detail:+ ($detail)}"
    else
        say "FAR_CHECK $name FAIL${detail:+ ($detail)}"
        FAILED=$((FAILED + 1))
    fi
}
have() { command -v "$1" >/dev/null 2>&1; }

for arg in "$@"; do
    case "$arg" in
        --start) MODE=start ;;
        --stop) MODE=stop ;;
        --status) MODE=status ;;
        --hash) MODE=hash ;;
        -h | --help)
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            say "far-serve: unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

file_size() {
    if [ -f "$PATH_FILE" ]; then
        stat -c '%s' "$PATH_FILE"
    else
        echo 0
    fi
}

file_hash() {
    if [ -f "$SHA_FILE" ]; then
        awk 'NR==1{print $1}' "$SHA_FILE"
    fi
}

httpd_pid() {
    if [ -f "$PID_FILE" ]; then
        pid=$(cat "$PID_FILE" 2>/dev/null || true)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            printf '%s\n' "$pid"
            return 0
        fi
    fi
    pgrep -f "far-raw-serve.py" 2>/dev/null | head -n 1
}

legacy_httpd_pid() {
    pgrep -f "busybox httpd -f -p $BIND:$PORT" 2>/dev/null | head -n 1
}

port_listening() {
    have ss || return 1
    ss -ltnH "sport = :$PORT" 2>/dev/null | grep -qF "$BIND:$PORT"
}

http_code() {
    # HEAD first; fall back to a 1-byte range GET for servers that dislike HEAD.
    code=$(curl -sS -o /dev/null -w '%{http_code}' -I -m 5 \
        "http://$BIND:$PORT/$FILE" 2>/dev/null || true)
    case "$code" in
        2*) printf '%s\n' "$code"; return 0 ;;
    esac
    curl -sS -o /dev/null -w '%{http_code}' -r 0-0 -m 5 \
        "http://$BIND:$PORT/$FILE" 2>/dev/null || true
}

content_length() {
    curl -sSI -m 5 "http://$BIND:$PORT/$FILE" 2>/dev/null |
        awk 'BEGIN{IGNORECASE=1} /^Content-Length:/{gsub(/\r/,""); print $2; exit}'
}

case "$MODE" in
hash)
    h=$(file_hash)
    if [ -z "$h" ]; then
        say "far-serve: no hash yet; run --start" >&2
        exit 3
    fi
    printf '%s\n' "$h"
    exit 0
    ;;
status)
    say "far_dir=$FAR_DIR"
    say "file=$PATH_FILE"
    say "size=$(file_size) expected=$BYTES"
    say "sha256=$(file_hash)"
    say "httpd_pid=$(httpd_pid || true)"
    say "listening=$(port_listening && echo yes || echo no)"
    say "http_code=$(http_code)"
    exit 0
    ;;
stop)
    for pid in $(httpd_pid || true) $(legacy_httpd_pid || true); do
        [ -n "$pid" ] || continue
        kill "$pid" 2>/dev/null || true
        i=0
        while [ "$i" -lt 5 ] && kill -0 "$pid" 2>/dev/null; do
            sleep 1
            i=$((i + 1))
        done
        kill -9 "$pid" 2>/dev/null || true
    done
    rm -f "$PID_FILE"
    if port_listening; then
        say "FAR_STOP FAIL (port $BIND:$PORT still listening)"
        exit 1
    fi
    say "FAR_STOP PASS (raw sender stopped, port free)"
    exit 0
    ;;
start) ;;
esac

say "== 1. tools"
if have python3; then rc_python=0; else rc_python=1; fi
check tools_python3 "$rc_python" "python3 raw sender"
if have dd; then rc_dd=0; else rc_dd=1; fi
check tools_dd "$rc_dd" "dd"
if have sha256sum; then rc_sha=0; else rc_sha=1; fi
check tools_sha256sum "$rc_sha" "sha256sum"
if have curl; then rc_curl=0; else rc_curl=1; fi
check tools_curl "$rc_curl" "curl"
if [ "$FAILED" -ne 0 ]; then
    say "FAR_RESULT FAIL"
    exit 3
fi

say "== 2. file"
mkdir -p "$FAR_DIR"
size=$(file_size)
if [ "$size" -ne "$BYTES" ]; then
    if [ "$size" -ne 0 ]; then
        say "   size=$size != $BYTES, regenerating"
    fi
    avail_kb=$(df -P -k "$FAR_DIR" | awk 'NR==2{print $4}')
    need_kb=$((BYTES / 1024 + 131072))
    if [ "${avail_kb:-0}" -lt "$need_kb" ]; then
        check disk_space 1 "available ${avail_kb}KiB < ${need_kb}KiB"
        say "FAR_RESULT FAIL"
        exit 3
    fi
    rm -f "$PATH_FILE" "$SHA_FILE"
    say "   generating $SIZE_MIB MiB from /dev/urandom (timeout 300 s)"
    if ! timeout 300 dd if=/dev/urandom of="$PATH_FILE" bs=1M \
        count="$SIZE_MIB" status=none 2>"$FAR_DIR/dd.log"; then
        check dd 1 "$(tail -n 1 "$FAR_DIR/dd.log" 2>/dev/null)"
        say "FAR_RESULT FAIL"
        exit 3
    fi
fi
size=$(file_size)
check size "$([ "$size" -eq "$BYTES" ]; echo $?)" "size=$size expected=$BYTES"

say "== 3. sha256"
sha256sum "$PATH_FILE" >"$SHA_FILE"
hash=$(file_hash)
hash_head=$(printf '%s' "$hash" | cut -c1-16)
check sha256 "$([ -n "$hash" ]; echo $?)" "sha256=$hash_head..."

say "== 4. raw sender (no write timeout)"
legacy=$(legacy_httpd_pid || true)
if [ -n "${legacy:-}" ]; then
    say "   stopping legacy busybox httpd pid=$legacy"
    kill "$legacy" 2>/dev/null || true
    i=0
    while [ "$i" -lt 5 ] && kill -0 "$legacy" 2>/dev/null; do
        sleep 1
        i=$((i + 1))
    done
    kill -9 "$legacy" 2>/dev/null || true
fi
pid=$(httpd_pid || true)
if [ -n "${pid:-}" ] && port_listening; then
    say "   already running pid=$pid"
else
    ( cd "$FAR_DIR" && setsid python3 "$RAW_PY" --file "$PATH_FILE" \
        --bind "$BIND" --port "$PORT" --log "$HTTP_LOG" \
        --pidfile "$PID_FILE" >>"$RAW_ERR" 2>&1 & )
    sleep 1
    pid=$(httpd_pid || true)
    if [ -z "${pid:-}" ]; then
        check httpd_start 1 "no far-raw-serve.py pid"
        say "FAR_RESULT FAIL"
        exit 3
    fi
fi

say "== 5. readiness"
i=0
ready=1
while [ "$i" -lt 10 ]; do
    if port_listening; then
        code=$(http_code)
        clen=$(content_length)
        if [ "$code" = "200" ] && [ "$clen" = "$BYTES" ]; then
            ready=0
            break
        fi
    fi
    sleep 1
    i=$((i + 1))
done
check listen "$(port_listening; echo $?)" "ss $BIND:$PORT"
check http_status "$([ "${code:-000}" = "200" ]; echo $?)" "code=${code:-000}"
check content_length "$([ "${clen:-0}" = "$BYTES" ]; echo $?)" \
    "Content-Length=${clen:-missing} expected=$BYTES"

say "FAR_SHA256=$hash"
say "FAR_SIZE=$size"
say "FAR_HTTPD_PID=$pid"
if [ "$FAILED" -eq 0 ]; then
    say "FAR_RESULT PASS"
    exit 0
fi
say "FAR_RESULT FAIL"
exit 4
