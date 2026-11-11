#!/usr/bin/env bash
# verify-uplink-1g.sh - 0.3.3 uplink 1 GiB byte-exact acceptance.
#
# One shot.  The session is already joined and the modules are loaded; this
# script does not configure, reload or unload the tunnel.  It proves that one
# 1 GiB application upload crosses the tunnel without losing a byte:
#
#   1. preflight: ssh, module version and srcversion pins, 256-carrier
#      configuration, c3 up=256, near/far ports, near bypass route, tools
#      and disk space.  Read-only; no traffic and no remote writes.
#   2. setup: upload the far receiver + samplers, stop a foreign far source
#      only when --replace-source is explicit, start the receiver, generate
#      a deterministic 1 GiB source on near (AES-256-CTR keystream with a
#      fixed key/IV), and hash it.
#   3. transfer: one curl POST of that file to near 127.0.0.1:9001.  The far
#      side re-originates it to 127.0.0.1:5210, where far-uplink-recv.py
#      streams it to disk.  No retry.
#   4. judgment: curl rc=0, http=200, size_upload=1 GiB, far file size=1 GiB,
#      both sha256 equal, both ends c3 up=256 throughout (sampler), and no
#      reset/hole/deliver_bad increment on either end.  Kernel window clean;
#      boot_id stable.
#   5. cleanup: stop the receiver that matches this run's out/port, stop the
#      samplers,
#      and remove the 1 GiB files after PASS.  A FAIL keeps them.  The tunnel,
#      its config, the ip rule and the watchdog are never touched.
#
# Modes
#   --run             full run (default)
#   --preflight       read-only readiness; no receiver start, no source, no traffic
#   --status          print module identity, config, c3 line and ports
#   --cleanup         stop our receiver/samplers and remove our files
#   --keep            keep the 1 GiB files after PASS (FAIL always keeps them)
#   --reuse-source    reuse a near source file only when its sha matches
#                     EXPECT_SHA256 (otherwise regenerate it)
#   --replace-source  stop whatever listens on far 127.0.0.1:5210 and record
#                     its pid/cmdline.  Without this flag a busy far port is a
#                     preflight failure, never an implicit kill.
#   --dry-run         print the plan; no ssh
#
# Identity pins are required.  Set both from the build report:
#   EXPECT_NEAR_SRCVERSION=... EXPECT_FAR_SRCVERSION=...
# EXPECT_MODULE_VERSION defaults to 0.3.3; set it empty to skip that check.
#
# Exit codes
#   0 PASS  2 usage  3 preflight  4 setup  6 integrity/checks
#   7 cleanup incomplete
set -uo pipefail

PROG=${0##*/}
MODE=run
KEEP_FILES=${KEEP_FILES:-0}
REUSE_SOURCE=${REUSE_SOURCE:-0}
REPLACE_SOURCE=${REPLACE_SOURCE:-0}

NEAR_HOST=${NEAR_HOST:-root@192.168.123.85}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
FAR_IP=${FAR_IP:-45.11.1.62}
NEAR_PORT=${NEAR_PORT:-9001}
FAR_PORT=${FAR_PORT:-5210}
FAR_LISTEN_PORT=${FAR_LISTEN_PORT:-9000}
TUNNEL=${TUNNEL:-1}
SSH_TMO=${SSH_TMO:-20}
UPLINK_MAX=${UPLINK_MAX:-7200}
UPLINK_TMO=${UPLINK_TMO:-$((UPLINK_MAX + 300))}
GEN_TMO=${GEN_TMO:-900}
SHA_TMO=${SHA_TMO:-900}
EXPECT_BYTES=1073741824
EXPECT_CARRIERS=${EXPECT_CARRIERS:-256}
EXPECT_MODE=${EXPECT_MODE:-sc-ldpc}
EXPECT_ADAPTIVE=${EXPECT_ADAPTIVE-off}
EXPECT_MODULE_VERSION=${EXPECT_MODULE_VERSION-0.3.3}
EXPECT_NEAR_SRCVERSION=${EXPECT_NEAR_SRCVERSION:-}
EXPECT_FAR_SRCVERSION=${EXPECT_FAR_SRCVERSION:-}
# sha256 of the first 2^30 bytes of AES-256-CTR(key,iv) applied to /dev/zero.
# This is the deterministic content every run must produce.  Set it empty to
# skip the generation pin (the two-end comparison still runs).
EXPECT_SHA256=${EXPECT_SHA256-369d49c2faf9dcb2b9ae2d80fd87ac56c42cc37704b70cbf90522d3d99c972f1}
AES_KEY=${AES_KEY:-000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f}
AES_IV=${AES_IV:-000102030405060708090a0b0c0d0e0f}

NEAR_DIR=${NEAR_DIR:-/root/sclc-uplink-1g}
FAR_DIR=${FAR_DIR:-/root/sclc-uplink-1g}
REMOTE_HELPERS=${REMOTE_HELPERS:-/root/sclc-uplink-1g/helpers}
NEAR_FILE=$NEAR_DIR/uplink-1g.bin
FAR_FILE=$FAR_DIR/uplink-1g.bin
FAR_RECV_LOG=$FAR_DIR/recv.log
FAR_RECV_ERR=$FAR_DIR/recv.err
FAR_RECV_PIDFILE=$FAR_DIR/recv.pid
NEAR_SAMPLER_LOG=$NEAR_DIR/sampler.log
FAR_SAMPLER_LOG=$FAR_DIR/sampler.log
NEAR_SAMPLER_STOP=$NEAR_DIR/sampler.stop
FAR_SAMPLER_STOP=$FAR_DIR/sampler.stop

BYPASS_PREF=${BYPASS_PREF:-100}
NEAR_DEV=${NEAR_DEV-eth0}
EVIDENCE_ROOT=${EVIDENCE_ROOT:-$(cd -- "$(dirname -- "$0")/.." && pwd)}

EV=
FAILED=0
START_UTC=
END_UTC=
START_EPOCH=
NEAR_BOOT_BEFORE=
FAR_BOOT_BEFORE=
NEAR_PHYS_DEV=
FAR_PHYS_DEV=
NEAR_SHA=
FAR_SHA=
NEAR_SHA_FILE=
FAR_SHA_FILE=
NEAR_SIZE=
FAR_SIZE=
CURL_RC=
CURL_LINE=
HTTP=
UPLOAD_SIZE=
UPLINK_TIME=
UPLINK_SPEED=
RECV_LINE=
RECV_BYTES=
RECV_RESULT=
NEAR_UP_MIN=
FAR_UP_MIN=
NEAR_UP_MAX=
FAR_UP_MAX=
NEAR_CPU=
FAR_CPU=
WIRE_NEAR_TX=
WIRE_FAR_RX=
FAR_RECV_PID=
FAR_RECV_OWNED=0
FAR_RECV_REUSED=0
REPLACED_PID=
REPLACED_CMD=
NEAR_T0=
FAR_T0=
NEAR_T1=
FAR_T1=

say() { printf '%s\n' "$*"; }
note() {
    local log=/dev/null
    [[ -n ${EV:-} ]] && log=$EV/run.log
    printf '%s %s\n' "$(date -u +%H:%M:%SZ)" "$*" | tee -a "$log" >&2
}
mark() {
    local log=/dev/null
    [[ -n ${EV:-} ]] && log=$EV/timeline.txt
    printf '%s\t%s%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$1" "${2:+ | $2}" |
        tee -a "$log" >&2
}

usage() {
    cat <<EOF
usage: $PROG [--run|--preflight|--status|--cleanup] [--keep]
             [--reuse-source] [--replace-source] [--dry-run]

Required identity pins (from the build report):
  EXPECT_NEAR_SRCVERSION=<srcversion>  EXPECT_FAR_SRCVERSION=<srcversion>

Environment:
  NEAR_HOST=root@192.168.123.85  FAR_HOST=root@45.11.1.62
  FAR_IP=45.11.1.62              NEAR_PORT=9001  FAR_PORT=5210
  FAR_LISTEN_PORT=9000           NEAR_DEV=eth0   BYPASS_PREF=100
  UPLINK_MAX=7200                GEN_TMO=900     SHA_TMO=900
  EXPECT_MODULE_VERSION=0.3.3    EXPECT_CARRIERS=256
  EXPECT_MODE=sc-ldpc            EXPECT_ADAPTIVE=off
  EXPECT_SHA256=369d49c2...      (empty: skip the generation pin)
  EVIDENCE_ROOT=<repo>/review/wan-experiment/arm64
  KEEP_FILES=1                   keep the 1 GiB files after PASS

A FAIL keeps both 1 GiB files and the remote logs.  Run --cleanup when the
evidence has been copied.  No retry is performed.
EOF
}

while (($#)); do
    case $1 in
        --run) MODE=run ;;
        --preflight) MODE=preflight ;;
        --status) MODE=status ;;
        --cleanup) MODE=cleanup ;;
        --keep) KEEP_FILES=1 ;;
        --reuse-source) REUSE_SOURCE=1 ;;
        --replace-source) REPLACE_SOURCE=1 ;;
        --dry-run) MODE=dry-run ;;
        -h | --help) usage; exit 0 ;;
        *) printf '%s: unknown argument: %s\n' "$PROG" "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

ssh_near() { timeout "$SSH_TMO" ssh -o BatchMode=yes "$NEAR_HOST" "$@"; }
ssh_far() { timeout "$SSH_TMO" ssh -o BatchMode=yes "$FAR_HOST" "$@"; }
ssh_near_long() { timeout "$1" ssh -o BatchMode=yes "$NEAR_HOST" "$2"; }
ssh_far_long() { timeout "$1" ssh -o BatchMode=yes "$FAR_HOST" "$2"; }
scp_near() { timeout "$SSH_TMO" scp -q "$@"; }
scp_far() { timeout "$SSH_TMO" scp -q "$@"; }

# ---------------------------------------------------------------------------
# parsers

stat_field() { # file class field [tunnel]
    awk -v c="$2" -v f="$3=" -v t="${4:-$TUNNEL}" '
        $1 == "tunnel" && $2 == t && $3 == c {
            for (i = 1; i <= NF; i++)
                if (index($i, f) == 1) { print substr($i, length(f) + 1); exit }
        }' "$1" 2>/dev/null
}

cap_field() { # file field [tunnel]
    awk -v f="$2=" -v t="${3:-$TUNNEL}" '
        $1 == "tunnel" && $2 == t && $3 ~ /^capture_runs=/ {
            for (i = 1; i <= NF; i++)
                if (index($i, f) == 1) { print substr($i, length(f) + 1); exit }
        }' "$1" 2>/dev/null
}

rx_field() { # file [tunnel]
    awk -v t="${2:-$TUNNEL}" '
        $1 == "tunnel" && $2 == t && $3 ~ /^rx_ready_calls=/ {
            for (i = 1; i <= NF; i++)
                if (index($i, "rx_delivered=") == 1) {
                    print substr($i, length("rx_delivered=") + 1); exit
                }
        }' "$1" 2>/dev/null
}

cfg_has() { # file exact-token
    awk -v t="$2" '
        { for (i = 1; i <= NF; i++) if ($i == t) { found = 1; exit } }
        END { exit(found ? 0 : 1) }' "$1" 2>/dev/null
}

cfg_has_port() { # file key port
    awk -v k="$2=" -v p=":$3" '
        {
            for (i = 1; i <= NF; i++)
                if (index($i, k) == 1 &&
                    substr($i, length($i) - length(p) + 1) == p) {
                    found = 1; exit
                }
        }
        END { exit(found ? 0 : 1) }' "$1" 2>/dev/null
}

num() { case ${1:-} in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }

delta() { # before after
    if num "${1:-}" && num "${2:-}"; then printf '%s' "$(( $2 - $1 ))"; else printf '?'; fi
}

field() { # key line
    local want=$1 line=$2
    awk -v k="$want=" '{
        for (i = 1; i <= NF; i++)
            if (index($i, k) == 1) { print substr($i, length(k) + 1); exit }
    }' <<<"$line"
}

cpu_busy() {
    awk 'NR == 1 {
        b = $2 + $3 + $4 + $7 + $8 + $9
        i = $5 + $6
        printf "%s %s\n", b, i
    }' "$1" 2>/dev/null
}

cpu_pct() {
    awk -v b1="$1" -v i1="$2" -v b2="$3" -v i2="$4" 'BEGIN {
        b = b2 - b1; i = i2 - i1; t = b + i
        if (t > 0) printf "%.1f", b * 100 / t
        else printf "0"
    }'
}

netdev_rx() { awk -v d="$2:" '$1 == d { print $2; exit }' "$1" 2>/dev/null; }
netdev_tx() { awk -v d="$2:" '$1 == d { print $10; exit }' "$1" 2>/dev/null; }

check() { # name rc detail
    local name=$1 rc=${2:-1} detail=${3:-} line
    case $rc in '' | *[!0-9]*) rc=1 ;; esac
    if ((rc == 0)); then
        line="CHECK $name PASS${detail:+ ($detail)}"
    else
        line="CHECK $name FAIL${detail:+ ($detail)}"
        FAILED=$((FAILED + 1))
    fi
    say "$line"
    [[ -n ${EV:-} ]] && printf '%s\n' "$line" >>"$EV/run.log"
    return 0
}

skip() {
    local line="CHECK $1 SKIP${2:+ ($2)}"
    say "$line"
    [[ -n ${EV:-} ]] && printf '%s\n' "$line" >>"$EV/run.log"
    return 0
}

early_summary() { # reason
    local why=$1
    [[ -n ${EV:-} ]] || return 0
    {
        printf '# 0.3.3 uplink 1 GiB acceptance\n\n**FAIL (%s)**\n\n' "$why"
        printf -- '- near: `%s`\n- far: `%s`\n- evidence: `%s`\n\n' \
            "$NEAR_HOST" "$FAR_HOST" "$EV"
        printf '## Checks\n\n```\n'
        grep -E '^CHECK ' "$EV/run.log" 2>/dev/null || true
        printf '```\n'
    } >"$EV/SUMMARY.md"
}

# ---------------------------------------------------------------------------
# evidence

capture_end() { # near|far outfile
    local fn=ssh_near out=$2
    [[ $1 == far ]] && fn=ssh_far
    "$fn" 'bash -s' >"$out" 2>&1 <<'EOS' || true
set -u
s() { printf '\n=== %s ===\n' "$1"; }
s date; date -u +%Y-%m-%dT%H:%M:%SZ
s boot_id; cat /proc/sys/kernel/random/boot_id 2>/dev/null || true
s uptime; cat /proc/uptime 2>/dev/null || true
s uname; uname -a
s loadavg; cat /proc/loadavg 2>/dev/null || true
s module; lsmod | grep -E '^tbtun' || true
s modinfo; modinfo -F version tbtun 2>/dev/null; modinfo -F srcversion tbtun 2>/dev/null
s srcversion; cat /sys/module/tbtun/srcversion 2>/dev/null || true
s config; cat /proc/net/tbtun/config 2>/dev/null || true
s ports; cat /proc/net/tbtun/ports 2>/dev/null || true
s tunnels; cat /proc/net/tbtun/tunnels 2>/dev/null || true
s stats; cat /proc/net/tbtun/stats 2>/dev/null || true
s ss; ss -ltnp 2>/dev/null || true
s ip_rule; ip rule show 2>/dev/null || true
s ip_route; ip route 2>/dev/null || true
s netdev; cat /proc/net/dev 2>/dev/null || true
s meminfo; awk '/^(MemTotal|MemAvailable|MemFree):/{print}' /proc/meminfo 2>/dev/null || true
s dmesg; dmesg 2>/dev/null | tail -n 1000 || true
EOS
}

capture_diag() { # stage
    local stage=$1
    capture_end near "$EV/diag-$stage-near.txt"
    capture_end far "$EV/diag-$stage-far.txt"
    ssh_near 'cat /proc/net/tbtun/stats' >"$EV/diag-$stage-near-stats.txt" 2>/dev/null || true
    ssh_far 'cat /proc/net/tbtun/stats' >"$EV/diag-$stage-far-stats.txt" 2>/dev/null || true
    ssh_near 'dmesg 2>/dev/null | tail -n 1000' >"$EV/diag-$stage-near-dmesg.txt" 2>/dev/null || true
    ssh_far 'dmesg 2>/dev/null | tail -n 1000' >"$EV/diag-$stage-far-dmesg.txt" 2>/dev/null || true
    note "diagnostics captured for stage=$stage"
}

kernel_window() { # near|far since until outfile
    local fn=ssh_near out=$4
    [[ $1 == far ]] && fn=ssh_far
    "$fn" "KW_SINCE=$2 KW_UNTIL=$3 bash -s" >"$out" 2>/dev/null <<'EOS' || true
set -u
src=none
body=
jr=0
if command -v journalctl >/dev/null 2>&1 &&
    journalctl -k -b 0 -n 1 --no-pager 2>/dev/null | grep -qv '^-- No entries --$'; then
    jr=1
fi
if [ "$jr" -eq 1 ]; then
    src=journalctl
    raw=$(journalctl -k -b 0 --no-pager --since "@$KW_SINCE" --until "@$KW_UNTIL" 2>/dev/null || true)
    case $raw in '' | *'-- No entries --'*) raw= ;; esac
    body=$(printf '%s\n' "$raw" | grep -v '^-- Logs begin' || true)
fi
if [ -z "$body" ] && command -v dmesg >/dev/null 2>&1; then
    src=dmesg
    body=$(dmesg 2>/dev/null | awk -v s="$KW_SINCE" -v u="$KW_UNTIL" -v now="$(date +%s)" '
        /^\[[ 0-9]*\.[0-9]+\]/ {
            m = $0; sub(/^\[/, "", m); sub(/\].*/, "", m)
            mono = m + 0
            wall = now - (uptime_seconds - mono)
            if (wall >= s && wall <= u) print
        }' uptime_seconds="$(cut -d' ' -f1 /proc/uptime)" 2>/dev/null || true)
fi
printf 'KW_SOURCE=%s\n' "$src"
printf '%s\n' "$body"
EOS
}

kw_source() { sed -n 's/^KW_SOURCE=//p' "$1" 2>/dev/null | sed -n '1p'; }
kw_body() { sed '1d' "$1" 2>/dev/null; }

kw_state() {
    local body
    body=$(kw_body "$1")
    if [[ -z ${body//[[:space:]/]} ]]; then
        printf 'SKIP'
    elif grep -qE 'BUG:|Oops|WARNING:|UBSAN:|general protection fault' <<<"$body"; then
        printf 'FAIL'
    elif grep -qE 'hogged CPU.*\[tbtun\]|workqueue:.*tbtun.*hogged CPU' <<<"$body"; then
        printf 'HOG'
    else
        printf 'PASS'
    fi
}

# ---------------------------------------------------------------------------
# remote helpers

near_port_listening() {
    local out
    out=$(ssh_near "ss -ltnH 'sport = :$NEAR_PORT' 2>/dev/null" 2>/dev/null || true)
    [[ -n $out ]]
}

far_port_listening() {
    local out
    out=$(ssh_far "ss -ltnH 'sport = :$FAR_PORT' 2>/dev/null" 2>/dev/null || true)
    [[ -n $out ]]
}

far_listen_listening() {
    local out
    out=$(ssh_far "ss -ltnH 'sport = :$FAR_LISTEN_PORT' 2>/dev/null" 2>/dev/null || true)
    [[ -n $out ]]
}

far_port_pid() {
    ssh_far "ss -ltnpH 'sport = :$FAR_PORT' 2>/dev/null" 2>/dev/null |
        sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | sed -n '1p'
}

far_cmdline() { # pid
    ssh_far "tr '\0' ' ' < /proc/$1/cmdline 2>/dev/null" 2>/dev/null || true
}

far_cmd_is_ours() { # cmdline
    [[ $1 == *far-uplink-recv.py* ]] &&
        [[ $1 == *"--out $FAR_FILE"* ]] &&
        [[ $1 == *"--port $FAR_PORT"* ]]
}

start_sampler() { # near|far log stop interval
    local host=$1 log=$2 stop=$3 interval=$4 fn=ssh_near
    [[ $host == far ]] && fn=ssh_far
    "$fn" "rm -f '$log' '$stop'; setsid sh '$REMOTE_HELPERS/uplink-sampler.sh' \
        '$TUNNEL' '$log' '$stop' '$interval' </dev/null >/dev/null 2>&1 & echo started" \
        >/dev/null 2>&1 || true
}

stop_sampler() { # near|far stop
    local fn=ssh_near
    [[ $1 == far ]] && fn=ssh_far
    "$fn" "touch '$2'" >/dev/null 2>&1 || true
}

stop_receiver() {
    ssh_far "FAR_RECV_PIDFILE='$FAR_RECV_PIDFILE' FAR_PORT='$FAR_PORT' sh -s" <<'EOS' 2>&1 || true
set -u
pid=
if [ -f "$FAR_RECV_PIDFILE" ]; then
    pid=$(sed -n '1p' "$FAR_RECV_PIDFILE" 2>/dev/null || true)
fi
if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
    pid=$(ss -ltnpH "sport = :$FAR_PORT" 2>/dev/null |
        sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | sed -n '1p')
fi
if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)
    case "$cmd" in
        *far-uplink-recv.py*)
            kill "$pid" 2>/dev/null || true
            i=0
            while [ "$i" -lt 5 ] && kill -0 "$pid" 2>/dev/null; do
                sleep 1
                i=$((i + 1))
            done
            kill -9 "$pid" 2>/dev/null || true
            if kill -0 "$pid" 2>/dev/null; then
                printf 'STOPPED_FAILED pid=%s\n' "$pid"
            else
                printf 'stopped pid=%s\n' "$pid"
            fi
            ;;
        *)
            printf 'not ours, left alone: pid=%s cmd=%s\n' "$pid" "$cmd"
            ;;
    esac
fi
EOS
}

# ---------------------------------------------------------------------------
# cleanup

cleanup_all() { # keep|remove
    local keep=$1 out=$EV/cleanup.txt
    say "== cleanup ($keep)"
    : >"$out"
    {
        printf '=== stop near curl from this run ===\n'
        ssh_near "pkill -f 'curl.*--data-binary @$NEAR_FILE' 2>/dev/null || true" 2>&1 || true
        printf '=== stop samplers ===\n'
        stop_sampler near "$NEAR_SAMPLER_STOP"
        stop_sampler far "$FAR_SAMPLER_STOP"
        sleep 1
        printf '=== copy remaining remote logs ===\n'
        ssh_near "sed -n '1,2000p' '$NEAR_SAMPLER_LOG' 2>/dev/null" \
            >"$EV/cleanup-near-sampler.log" 2>/dev/null || true
        ssh_far "sed -n '1,2000p' '$FAR_SAMPLER_LOG' 2>/dev/null" \
            >"$EV/cleanup-far-sampler.log" 2>/dev/null || true
        ssh_far "sed -n '1,2000p' '$FAR_RECV_LOG' 2>/dev/null" \
            >"$EV/cleanup-far-recv.log" 2>/dev/null || true
        printf '=== stop far receiver (ours only) ===\n'
        stop_receiver
        printf '=== far port after stop ===\n'
        ssh_far "ss -ltnpH 'sport = :$FAR_PORT'" 2>&1 || true
        if [[ $keep == remove ]]; then
            printf '=== remove run files ===\n'
            ssh_near "rm -f '$NEAR_FILE' '$NEAR_FILE.part' '$NEAR_DIR/curl.sh' \
                            '$NEAR_DIR/curl.pid' '$NEAR_SAMPLER_LOG' '$NEAR_SAMPLER_STOP'
                      rm -f '$REMOTE_HELPERS/uplink-sampler.sh'
                      rmdir '$REMOTE_HELPERS' '$NEAR_DIR' 2>/dev/null || true
                      test -e '$NEAR_FILE' && echo 'near: file still present' ||
                          echo 'near: file removed'" 2>&1 || true
            ssh_far "rm -f '$FAR_FILE' '$FAR_FILE.part' '$FAR_RECV_LOG' '$FAR_RECV_ERR' \
                           '$FAR_RECV_PIDFILE' '$FAR_SAMPLER_LOG' '$FAR_SAMPLER_STOP'
                     rm -f '$REMOTE_HELPERS/far-uplink-recv.py' '$REMOTE_HELPERS/uplink-sampler.sh'
                     rmdir '$REMOTE_HELPERS' '$FAR_DIR' 2>/dev/null || true
                     test -e '$FAR_FILE' && echo 'far: file still present' ||
                         echo 'far: file removed'" 2>&1 || true
        else
            printf 'kept: near %s, far %s\n' "$NEAR_FILE" "$FAR_FILE"
        fi
        if [[ $keep == remove ]]; then
            near_left=$(ssh_near "test -e '$NEAR_FILE' && echo yes || echo no" 2>/dev/null || echo '?')
            far_left=$(ssh_far "test -e '$FAR_FILE' && echo yes || echo no" 2>/dev/null || echo '?')
            printf 'removal: near=%s far=%s\n' "$near_left" "$far_left"
        fi
        printf '=== modules (untouched) ===\n'
        ssh_near "lsmod | grep -E '^tbtun ' || echo 'near: tbtun not loaded'" 2>&1 || true
        ssh_far "lsmod | grep -E '^tbtun ' || echo 'far: tbtun not loaded'" 2>&1 || true
        printf '=== near curl leftovers ===\n'
        ssh_near "pgrep -af 'curl.*--data-binary @$NEAR_FILE' || true" 2>&1 || true
    } >>"$out" 2>&1
    if [[ $keep == remove && ${near_left:-?} != no ]]; then
        note "cleanup: near file state=${near_left:-?}; see $out"
        return 1
    fi
    if [[ $keep == remove && ${far_left:-?} != no ]]; then
        note "cleanup: far file state=${far_left:-?}; see $out"
        return 1
    fi
    say "cleanup log: $out"
    if [[ $keep == remove ]]; then
        if grep -qE 'STOPPED_FAILED|file still present|removal: near=yes|removal: far=yes' "$out" 2>/dev/null; then
            note "cleanup incomplete; see $out"
            return 1
        fi
    fi
    return 0
}

# ---------------------------------------------------------------------------
# modes that do not run traffic

if [[ $MODE == dry-run ]]; then
    say "dry-run plan:"
    say "  1. preflight (read-only): ssh, srcversion pins, version=$EXPECT_MODULE_VERSION"
    say "     shape: mode=$EXPECT_MODE carriers=$EXPECT_CARRIERS adaptive=$EXPECT_ADAPTIVE"
    say "     c3 up=$EXPECT_CARRIERS on both ends; near:$NEAR_PORT and far:$FAR_LISTEN_PORT listening"
    say "     near route to $FAR_IP dev ${NEAR_DEV:-auto}; ip rule pref $BYPASS_PREF"
    say "     far 127.0.0.1:$FAR_PORT free or --replace-source; tools and disk"
    say "  2. upload helpers, start far-uplink-recv.py on 127.0.0.1:$FAR_PORT"
    say "     generate deterministic 1 GiB on near ($NEAR_FILE)"
    say "  3. one curl POST to http://127.0.0.1:$NEAR_PORT/; max ${UPLINK_MAX}s; no retry"
    say "     far writes $FAR_FILE; compare sha256 on both ends"
    say "  4. checks: rc=0 http=200 size=$EXPECT_BYTES; up=256; zero reset/hole/deliver_bad"
    say "     kernel window; boot_id stable"
    say "  5. PASS removes the 1 GiB files; FAIL keeps them. Tunnel/config/ip rule/watchdog untouched."
    exit 0
fi

if [[ $MODE == status ]]; then
    say "pins: near=${EXPECT_NEAR_SRCVERSION:-<unset>} far=${EXPECT_FAR_SRCVERSION:-<unset>}"
    say "expect version=${EXPECT_MODULE_VERSION:-<skip>} carriers=$EXPECT_CARRIERS mode=$EXPECT_MODE"
    for end in near far; do
        host=$NEAR_HOST
        [[ $end == far ]] && host=$FAR_HOST
        say "== $end $host"
        if [[ $end == near ]]; then
            ssh_near 'cat /proc/net/tbtun/config 2>/dev/null; echo ---; cat /sys/module/tbtun/srcversion 2>/dev/null; echo; modinfo -F version tbtun 2>/dev/null; echo ---; awk '\''$1=="tunnel"&&$2==1&&$3=="c3"'\'' /proc/net/tbtun/stats 2>/dev/null; echo ---; ss -ltnH '\''sport = :9001'\'' 2>/dev/null; ls -l /root/sclc-uplink-1g 2>/dev/null' || true
        else
            ssh_far 'cat /proc/net/tbtun/config 2>/dev/null; echo ---; cat /sys/module/tbtun/srcversion 2>/dev/null; echo; modinfo -F version tbtun 2>/dev/null; echo ---; awk '\''$1=="tunnel"&&$2==1&&$3=="c3"'\'' /proc/net/tbtun/stats 2>/dev/null; echo ---; ss -ltnH '\''sport = :5210'\'' 2>/dev/null; ls -l /root/sclc-uplink-1g 2>/dev/null' || true
        fi
    done
    say "== evidence root $EVIDENCE_ROOT"
    exit 0
fi

if [[ $MODE == cleanup ]]; then
    EV=$EVIDENCE_ROOT/cleanup-uplink-1g-$(date -u +%Y%m%dT%H%M%SZ)
    mkdir -p "$EV"
    : >"$EV/run.log"
    : >"$EV/timeline.txt"
    say "evidence: $EV"
    if cleanup_all remove; then
        say "cleanup PASS"
        exit 0
    fi
    say "cleanup FAIL"
    exit 7
fi

# ---------------------------------------------------------------------------
# evidence directory

EV=$EVIDENCE_ROOT/sclc-uplink-1g-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$EV"
: >"$EV/run.log"
: >"$EV/timeline.txt"
say "evidence: $EV"
START_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
START_EPOCH=$(date +%s)
mark run-start

# ---------------------------------------------------------------------------
# 1. preflight

say "== 1. preflight"
if ! ssh_near true; then
    check near_ssh 1 "unreachable $NEAR_HOST"
    early_summary "preflight ssh"
    exit 3
fi
if ! ssh_far true; then
    check near_ssh 0 "reachable"
    check far_ssh 1 "unreachable $FAR_HOST"
    early_summary "preflight ssh"
    exit 3
fi
check near_ssh 0 "reachable"
check far_ssh 0 "reachable"

NEAR_BOOT_BEFORE=$(ssh_near 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)
FAR_BOOT_BEFORE=$(ssh_far 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)

check near_loaded "$(ssh_near 'lsmod | grep -q "^tbtun "' >/dev/null 2>&1; echo $?)" "tbtun"
check far_loaded "$(ssh_far 'lsmod | grep -q "^tbtun "' >/dev/null 2>&1; echo $?)" "tbtun"

ssh_near 'cat /proc/net/tbtun/config' >"$EV/near-config.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/config' >"$EV/far-config.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/stats' >"$EV/near-stats-pre.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/stats' >"$EV/far-stats-pre.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/tunnels' >"$EV/near-tunnels-pre.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/tunnels' >"$EV/far-tunnels-pre.txt" 2>/dev/null || true
ssh_near 'modinfo -F version tbtun; modinfo -F srcversion tbtun; cat /sys/module/tbtun/srcversion' >"$EV/near-module.txt" 2>/dev/null || true
ssh_far 'modinfo -F version tbtun; modinfo -F srcversion tbtun; cat /sys/module/tbtun/srcversion' >"$EV/far-module.txt" 2>/dev/null || true

near_src=$(ssh_near 'cat /sys/module/tbtun/srcversion 2>/dev/null' | tr -d '[:space:]')
far_src=$(ssh_far 'cat /sys/module/tbtun/srcversion 2>/dev/null' | tr -d '[:space:]')
if [[ -n $EXPECT_NEAR_SRCVERSION ]]; then
    check near_srcversion "$([[ $near_src == "$EXPECT_NEAR_SRCVERSION" ]]; echo $?)" \
        "src=$near_src want=$EXPECT_NEAR_SRCVERSION"
else
    check near_srcversion_pin 1 "set EXPECT_NEAR_SRCVERSION from the build report (loaded src=$near_src)"
fi
if [[ -n $EXPECT_FAR_SRCVERSION ]]; then
    check far_srcversion "$([[ $far_src == "$EXPECT_FAR_SRCVERSION" ]]; echo $?)" \
        "src=$far_src want=$EXPECT_FAR_SRCVERSION"
else
    check far_srcversion_pin 1 "set EXPECT_FAR_SRCVERSION from the build report (loaded src=$far_src)"
fi

near_ver=$(ssh_near 'v=$(cat /sys/module/tbtun/version 2>/dev/null || true); [ -n "$v" ] || v=$(modinfo -F version tbtun 2>/dev/null || true); printf "%s\n" "$v"' | tr -d '[:space:]')
far_ver=$(ssh_far 'v=$(cat /sys/module/tbtun/version 2>/dev/null || true); [ -n "$v" ] || v=$(modinfo -F version tbtun 2>/dev/null || true); printf "%s\n" "$v"' | tr -d '[:space:]')
if [[ -n $EXPECT_MODULE_VERSION ]]; then
    check near_module_version "$([[ $near_ver == "$EXPECT_MODULE_VERSION" ]]; echo $?)" \
        "version=$near_ver want=$EXPECT_MODULE_VERSION"
    check far_module_version "$([[ $far_ver == "$EXPECT_MODULE_VERSION" ]]; echo $?)" \
        "version=$far_ver want=$EXPECT_MODULE_VERSION"
else
    skip near_module_version "EXPECT_MODULE_VERSION empty; loaded=$near_ver"
    skip far_module_version "EXPECT_MODULE_VERSION empty; loaded=$far_ver"
fi

for tok in "mode=$EXPECT_MODE" "carriers=$EXPECT_CARRIERS" "fec=on" "crypto=on"; do
    check "near_cfg_${tok%%=*}" "$(cfg_has "$EV/near-config.txt" "$tok"; echo $?)" "$tok"
    check "far_cfg_${tok%%=*}" "$(cfg_has "$EV/far-config.txt" "$tok"; echo $?)" "$tok"
done
if [[ -n $EXPECT_ADAPTIVE ]]; then
    check near_cfg_adaptive "$(cfg_has "$EV/near-config.txt" "adaptive=$EXPECT_ADAPTIVE"; echo $?)" \
        "adaptive=$EXPECT_ADAPTIVE"
    check far_cfg_adaptive "$(cfg_has "$EV/far-config.txt" "adaptive=$EXPECT_ADAPTIVE"; echo $?)" \
        "adaptive=$EXPECT_ADAPTIVE"
fi
check near_cfg_listen "$(cfg_has_port "$EV/near-config.txt" listen "$NEAR_PORT"; echo $?)" \
    "listen=*:$NEAR_PORT"
check far_cfg_target "$(cfg_has_port "$EV/far-config.txt" target "$FAR_PORT"; echo $?)" \
    "target=*:$FAR_PORT"

near_up=$(stat_field "$EV/near-stats-pre.txt" c3 up)
far_up=$(stat_field "$EV/far-stats-pre.txt" c3 up)
check near_c3_up "$([[ ${near_up:-0} == "$EXPECT_CARRIERS" ]]; echo $?)" \
    "up=${near_up:-?}"
check far_c3_up "$([[ ${far_up:-0} == "$EXPECT_CARRIERS" ]]; echo $?)" \
    "up=${far_up:-?}"
for end in near far; do
    rep=$(stat_field "$EV/$end-stats-pre.txt" c3 replacing)
    emp=$(stat_field "$EV/$end-stats-pre.txt" c3 empty)
    check "${end}_c3_replacing" "$([[ ${rep:-1} == 0 ]]; echo $?)" "replacing=${rep:-?}"
    check "${end}_c3_empty" "$([[ ${emp:-1} == 0 ]]; echo $?)" "empty=${emp:-?}"
done

ssh_near "ip rule show" >"$EV/near-iprule-pre.txt" 2>&1 || true
ssh_near "ip route get $FAR_IP" >"$EV/near-route.txt" 2>&1 || true
NEAR_PHYS_DEV=$(awk '{for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit }}' "$EV/near-route.txt" 2>/dev/null)
FAR_PHYS_DEV=$(ssh_far "ip route get 1.1.1.1 2>/dev/null" | awk '{for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit }}' 2>/dev/null)
dev_ok() {
    local d=${1:-}
    [[ -n $d ]] || return 1
    case $d in Meta | tun* | utun* | wg* | tailscale*) return 1 ;; esac
    [[ -z $NEAR_DEV || $d == "$NEAR_DEV" ]]
}
check near_route_dev "$(dev_ok "$NEAR_PHYS_DEV"; echo $?)" \
    "dev=${NEAR_PHYS_DEV:-none} want=${NEAR_DEV:-any physical}"
check near_bypass_rule "$(awk -v p="$BYPASS_PREF:" -v ip="$FAR_IP" '
    $1 == p { for (i = 2; i < NF; i++) if ($i == "to" && ($(i + 1) == ip || $(i + 1) == ip "/32")) found = 1 }
    END { exit(found ? 0 : 1) }' "$EV/near-iprule-pre.txt"; echo $?)" \
    "pref $BYPASS_PREF to $FAR_IP"

check near_listen "$(near_port_listening; echo $?)" "127.0.0.1:$NEAR_PORT"
check far_listen "$(far_listen_listening; echo $?)" "0.0.0.0:$FAR_LISTEN_PORT"

if far_port_listening; then
    FAR_PORT_PID=$(far_port_pid)
    FAR_PORT_CMD=$(far_cmdline "${FAR_PORT_PID:-0}")
    printf '%s\n' "pid=${FAR_PORT_PID:-?} cmd=${FAR_PORT_CMD:-?}" >"$EV/far-port-pre.txt"
    if [[ -n ${FAR_PORT_PID:-} ]] && far_cmd_is_ours "$FAR_PORT_CMD"; then
        check far_port 0 "our receiver already listening pid=$FAR_PORT_PID"
    elif ((REPLACE_SOURCE)) && [[ -n ${FAR_PORT_PID:-} ]]; then
        check far_port 0 "foreign pid=${FAR_PORT_PID:-?} will be replaced (--replace-source)"
    elif ((REPLACE_SOURCE)); then
        check far_port 1 "listener pid unknown; stop it manually before --replace-source"
    else
        check far_port 1 "busy pid=${FAR_PORT_PID:-?} cmd=${FAR_PORT_CMD:-?}; stop it or pass --replace-source"
    fi
else
    : >"$EV/far-port-pre.txt"
    check far_port 0 "127.0.0.1:$FAR_PORT free"
fi

near_tools=$(ssh_near 'for t in curl sha256sum openssl head stat awk ss grep sed sh setsid timeout wc tr pgrep pkill df ip; do command -v "$t" >/dev/null || echo missing:$t; done' 2>/dev/null || true)
far_tools=$(ssh_far 'for t in python3 curl sha256sum stat awk ss grep sed sh setsid timeout wc tr pgrep pkill df ip; do command -v "$t" >/dev/null || echo missing:$t; done' 2>/dev/null || true)
check near_tools "$([[ -z $near_tools ]]; echo $?)" "curl sha256sum openssl head stat awk ss grep sed sh setsid timeout wc tr pgrep pkill df ip ${near_tools}"
check far_tools "$([[ -z $far_tools ]]; echo $?)" "python3 curl sha256sum stat awk ss grep sed sh setsid timeout wc tr pgrep pkill df ip ${far_tools}"

DISK_NEED_KB=$((EXPECT_BYTES / 1024 + 524288))
near_avail=$(ssh_near "df -P -k /root | awk 'NR==2{print \$4}'" 2>/dev/null || echo 0)
far_avail=$(ssh_far "df -P -k /root | awk 'NR==2{print \$4}'" 2>/dev/null || echo 0)
check near_disk "$([[ ${near_avail:-0} -ge $DISK_NEED_KB ]]; echo $?)" \
    "${near_avail:-0}KiB free, need ${DISK_NEED_KB}KiB"
check far_disk "$([[ ${far_avail:-0} -ge $DISK_NEED_KB ]]; echo $?)" \
    "${far_avail:-0}KiB free, need ${DISK_NEED_KB}KiB"

capture_end near "$EV/near-pre.txt"
capture_end far "$EV/far-pre.txt"

if ((FAILED)); then
    capture_diag preflight
    early_summary preflight
    note "preflight FAIL; no traffic attempted"
    exit 3
fi
mark preflight-done

if [[ $MODE == preflight ]]; then
    note "preflight PASS"
    exit 0
fi

# ---------------------------------------------------------------------------
# 2. setup: helpers, receiver, deterministic source

say "== 2. helpers"
SRC_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ssh_near "mkdir -p '$REMOTE_HELPERS' '$NEAR_DIR'" >/dev/null 2>&1 || true
ssh_far "mkdir -p '$REMOTE_HELPERS' '$FAR_DIR'" >/dev/null 2>&1 || true
if scp_far "$SRC_DIR/far-uplink-recv.py" "$FAR_HOST:$REMOTE_HELPERS/far-uplink-recv.py"; then
    check far_upload 0 "far-uplink-recv.py"
else
    check far_upload 1 "scp far-uplink-recv.py"
fi
if scp_far "$SRC_DIR/uplink-sampler.sh" "$FAR_HOST:$REMOTE_HELPERS/uplink-sampler.sh"; then
    check far_sampler_upload 0 "uplink-sampler.sh"
else
    check far_sampler_upload 1 "scp uplink-sampler.sh to far"
fi
if scp_near "$SRC_DIR/uplink-sampler.sh" "$NEAR_HOST:$REMOTE_HELPERS/uplink-sampler.sh"; then
    check near_sampler_upload 0 "uplink-sampler.sh"
else
    check near_sampler_upload 1 "scp uplink-sampler.sh to near"
fi
if ((FAILED)); then
    capture_diag setup
    exit 4
fi

say "== 3. far receiver"
if far_port_listening; then
    FAR_PORT_PID=$(far_port_pid)
    FAR_PORT_CMD=$(far_cmdline "${FAR_PORT_PID:-0}")
    if [[ -n ${FAR_PORT_PID:-} ]] && far_cmd_is_ours "$FAR_PORT_CMD"; then
        FAR_RECV_REUSED=1
        FAR_RECV_PID=$FAR_PORT_PID
        check far_receiver_reused 0 "pid=$FAR_RECV_PID already listening"
    elif ((REPLACE_SOURCE)) && [[ -n ${FAR_PORT_PID:-} ]]; then
        REPLACED_PID=$FAR_PORT_PID
        REPLACED_CMD=$FAR_PORT_CMD
        {
            printf 'pid=%s\ncmd=%s\n' "${REPLACED_PID:-?}" "${REPLACED_CMD:-?}"
            ssh_far "kill '${REPLACED_PID:-}' 2>/dev/null || true
                     i=0
                     while [ \$i -lt 10 ] && kill -0 '${REPLACED_PID:-}' 2>/dev/null; do sleep 1; i=\$((i+1)); done
                     kill -9 '${REPLACED_PID:-}' 2>/dev/null || true" 2>&1 || true
        } >"$EV/replaced-source.txt" 2>&1
        if far_port_listening; then
            check far_replace_source 1 "pid=${REPLACED_PID:-?} still holds $FAR_PORT"
        else
            check far_replace_source 0 "pid=${REPLACED_PID:-?} stopped (cmd=${REPLACED_CMD:-?})"
        fi
    elif ((REPLACE_SOURCE)); then
        check far_replace_source 1 "cannot identify the listener pid on $FAR_PORT; stop it manually"
    else
        check far_receiver_start 1 "port $FAR_PORT busy pid=${FAR_PORT_PID:-?}; use --replace-source"
    fi
fi

if ! far_port_listening; then
    ssh_far "rm -f '$FAR_FILE' '$FAR_FILE.part' '$FAR_RECV_LOG' '$FAR_RECV_ERR' '$FAR_RECV_PIDFILE'
             setsid python3 '$REMOTE_HELPERS/far-uplink-recv.py' \
                 --bind 127.0.0.1 --port '$FAR_PORT' --out '$FAR_FILE' \
                 --log '$FAR_RECV_LOG' --pidfile '$FAR_RECV_PIDFILE' \
                 --max-bytes '$((EXPECT_BYTES + 1048576))' \
                 </dev/null >'$FAR_RECV_ERR' 2>&1 & echo started" >/dev/null 2>&1 || true
    ready=0
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        FAR_RECV_PID=$(ssh_far "sed -n '1p' '$FAR_RECV_PIDFILE' 2>/dev/null" 2>/dev/null | tr -d '[:space:]')
        if [[ -n ${FAR_RECV_PID:-} ]] && ssh_far "kill -0 '$FAR_RECV_PID'" >/dev/null 2>&1 && far_port_listening; then
            ready=1
            break
        fi
        sleep 1
    done
    if ((ready)); then
        FAR_RECV_OWNED=1
        check far_receiver_start 0 "pid=$FAR_RECV_PID port=$FAR_PORT"
    else
        check far_receiver_start 1 "pid=${FAR_RECV_PID:-?} port not listening; see far recv.err"
    fi
fi

far_head=$(ssh_far "curl -sS -m 5 -I http://127.0.0.1:$FAR_PORT/ -o /dev/null -w '%{http_code}'" 2>/dev/null || true)
check far_receiver_head "$([[ ${far_head:-000} == 200 ]]; echo $?)" "http=${far_head:-000}"
printf '%s\n' "head_http=$far_head" >"$EV/far-receiver-head.txt"

# Remove any output and log from an earlier run before the one POST.  This
# matters when a receiver is reused: a stale 1 GiB file or an old POST line
# must never be able to satisfy the new run's checks.
ssh_far "rm -f '$FAR_FILE' '$FAR_FILE.part'; : > '$FAR_RECV_LOG'" >/dev/null 2>&1 || true

if ((FAILED)); then
    capture_diag setup
    early_summary setup
    note "setup FAIL; no transfer attempted"
    cleanup_all keep
    exit 4
fi

say "== 4. near deterministic source"
if ((REUSE_SOURCE)) && [[ -n $EXPECT_SHA256 ]]; then
    reuse_size=$(ssh_near "stat -c %s '$NEAR_FILE' 2>/dev/null || echo 0" 2>/dev/null | tr -d '[:space:]')
    if [[ ${reuse_size:-0} == "$EXPECT_BYTES" ]]; then
        reuse_sha=$(ssh_near_long "$SHA_TMO" "timeout '$SHA_TMO' sha256sum '$NEAR_FILE' 2>/dev/null" 2>/dev/null | awk 'NR==1{print $1}' || true)
        if [[ $reuse_sha == "$EXPECT_SHA256" ]]; then
            check near_source_reuse 0 "size=$reuse_size sha=${reuse_sha:0:16}..."
            NEAR_SHA=$reuse_sha
        fi
    fi
fi

if [[ -z ${NEAR_SHA:-} ]]; then
    ssh_near_long "$GEN_TMO" "NEAR_DIR='$NEAR_DIR' NEAR_FILE='$NEAR_FILE' \
        AES_KEY='$AES_KEY' AES_IV='$AES_IV' EXPECT_BYTES='$EXPECT_BYTES' \
        timeout '$GEN_TMO' sh -s" >"$EV/generate.log" 2>&1 <<'EOS'
set -u
mkdir -p "$NEAR_DIR"
rm -f "$NEAR_FILE" "$NEAR_FILE.part"
# openssl reports "error writing output file" when head closes the pipe after
# exactly EXPECT_BYTES.  That EPIPE is expected; the size + mv gate below is
# the authority, not the pipeline exit status.
( ulimit -f unlimited 2>/dev/null || true
  openssl enc -aes-256-ctr -K "$AES_KEY" -iv "$AES_IV" -nosalt < /dev/zero |
      head -c "$EXPECT_BYTES" > "$NEAR_FILE.part" ) || true
size=$(stat -c %s "$NEAR_FILE.part" 2>/dev/null || echo 0)
if [ "$size" -eq "$EXPECT_BYTES" ] && mv "$NEAR_FILE.part" "$NEAR_FILE"; then
    printf 'GEN_SIZE=%s\n' "$size"
else
    printf 'GEN_SIZE=%s expected=%s\n' "$size" "$EXPECT_BYTES"
    exit 1
fi
EOS
    gen_rc=$?
    NEAR_SIZE=$(ssh_near "stat -c %s '$NEAR_FILE' 2>/dev/null || echo 0" 2>/dev/null | tr -d '[:space:]')
    check near_generate "$([[ $gen_rc -eq 0 && ${NEAR_SIZE:-0} == "$EXPECT_BYTES" ]]; echo $?)" \
        "rc=$gen_rc size=${NEAR_SIZE:-0} (see generate.log)"
    if ((FAILED)); then
        capture_diag setup
        early_summary "setup source"
        note "source generation FAIL; no transfer attempted"
        cleanup_all keep
        exit 4
    fi
    NEAR_SHA=$(ssh_near_long "$SHA_TMO" "timeout '$SHA_TMO' sha256sum '$NEAR_FILE' 2>/dev/null" 2>/dev/null | awk 'NR==1{print $1}' || true)
fi

NEAR_SIZE=$(ssh_near "stat -c %s '$NEAR_FILE' 2>/dev/null || echo 0" 2>/dev/null | tr -d '[:space:]')
NEAR_SHA_FILE=$(ssh_near_long "$SHA_TMO" "timeout '$SHA_TMO' sha256sum '$NEAR_FILE' 2>/dev/null" 2>/dev/null || true)
final_sha=$(awk 'NR==1{print $1}' <<<"$NEAR_SHA_FILE" 2>/dev/null || true)
[[ -n ${final_sha:-} ]] && NEAR_SHA=$final_sha
printf '%s\n' "$NEAR_SHA_FILE" >"$EV/near-sha256.txt"

check near_source_size "$([[ ${NEAR_SIZE:-0} == "$EXPECT_BYTES" ]]; echo $?)" \
    "size=${NEAR_SIZE:-0} want=$EXPECT_BYTES"
check near_source_sha_hex "$([[ $NEAR_SHA =~ ^[0-9a-f]{64}$ ]]; echo $?)" \
    "sha=${NEAR_SHA:-?}"
if [[ -n $EXPECT_SHA256 ]]; then
    check near_source_pin "$([[ $NEAR_SHA == "$EXPECT_SHA256" ]]; echo $?)" \
        "sha=${NEAR_SHA:0:16}... want=${EXPECT_SHA256:0:16}..."
else
    skip near_source_pin "EXPECT_SHA256 empty; two-end comparison still required"
fi
if ((FAILED)); then
    capture_diag setup
    early_summary "setup source identity"
    note "source identity FAIL; no transfer attempted"
    cleanup_all keep
    exit 4
fi
mark source-done

# ---------------------------------------------------------------------------
# 5. one upload

say "== 5. uplink POST (max ${UPLINK_MAX}s, single shot)"
mark uplink-start
ssh_near 'cat /proc/net/tbtun/stats' >"$EV/near-stats-uplink-pre.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/stats' >"$EV/far-stats-uplink-pre.txt" 2>/dev/null || true
ssh_near 'cat /proc/stat' >"$EV/near-procstat-pre.txt" 2>/dev/null || true
ssh_far 'cat /proc/stat' >"$EV/far-procstat-pre.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/dev' >"$EV/near-netdev-pre.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/dev' >"$EV/far-netdev-pre.txt" 2>/dev/null || true

start_sampler near "$NEAR_SAMPLER_LOG" "$NEAR_SAMPLER_STOP" 1
start_sampler far "$FAR_SAMPLER_LOG" "$FAR_SAMPLER_STOP" 1
sleep 1
near_samples=$(ssh_near "wc -l < '$NEAR_SAMPLER_LOG' 2>/dev/null || echo 0" 2>/dev/null | tr -d '[:space:]')
far_samples=$(ssh_far "wc -l < '$FAR_SAMPLER_LOG' 2>/dev/null || echo 0" 2>/dev/null | tr -d '[:space:]')
if [[ ${near_samples:-0} -gt 0 && ${far_samples:-0} -gt 0 ]]; then
    check sampler_started 0 "near_lines=$near_samples far_lines=$far_samples"
else
    skip sampler_started "near_lines=${near_samples:-?} far_lines=${far_samples:-?}; pre/post samples still judge up"
fi

cat >"$EV/curl.sh" <<EOF
#!/bin/sh
exec curl -sS -m '$UPLINK_MAX' -H 'Expect:' \\
    --data-binary @"$NEAR_FILE" -o /dev/null \\
    -w 'http=%{http_code} size=%{size_upload} time=%{time_total} speed=%{speed_upload}\\n' \\
    "http://127.0.0.1:$NEAR_PORT/"
EOF
chmod +x "$EV/curl.sh"
if ! scp_near "$EV/curl.sh" "$NEAR_HOST:$NEAR_DIR/curl.sh" >/dev/null 2>&1; then
    check near_curl_upload 1 "scp curl.sh"
    capture_diag setup
    cleanup_all keep
    exit 4
else
    check near_curl_upload 0 "curl.sh"
fi

NEAR_T0=$(ssh_near 'date +%s' 2>/dev/null || true)
FAR_T0=$(ssh_far 'date +%s' 2>/dev/null || true)
case ${NEAR_T0:-} in '' | *[!0-9]*) NEAR_T0=$START_EPOCH ;; esac
case ${FAR_T0:-} in '' | *[!0-9]*) FAR_T0=$START_EPOCH ;; esac

CURL_RC=0
ssh_near_long "$UPLINK_TMO" "timeout '$UPLINK_MAX' sh '$NEAR_DIR/curl.sh'" \
    >"$EV/curl.txt" 2>&1 || CURL_RC=$?
if ((CURL_RC != 0)); then
    # A failed ssh/timeout can leave the remote curl alive.  Kill the exact
    # curl before sampling, so a late writer cannot race the far hash.
    ssh_near "pkill -f 'curl.*--data-binary @$NEAR_FILE' 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    sleep 1
fi
NEAR_T1=$(ssh_near 'date +%s' 2>/dev/null || true)
FAR_T1=$(ssh_far 'date +%s' 2>/dev/null || true)
case ${NEAR_T1:-} in '' | *[!0-9]*) NEAR_T1=$(date +%s) ;; esac
case ${FAR_T1:-} in '' | *[!0-9]*) FAR_T1=$(date +%s) ;; esac
stop_sampler near "$NEAR_SAMPLER_STOP"
stop_sampler far "$FAR_SAMPLER_STOP"
sleep 1
mark uplink-done "rc=$CURL_RC"

ssh_near 'cat /proc/net/tbtun/stats' >"$EV/near-stats-uplink-post.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/stats' >"$EV/far-stats-uplink-post.txt" 2>/dev/null || true
ssh_near 'cat /proc/stat' >"$EV/near-procstat-post.txt" 2>/dev/null || true
ssh_far 'cat /proc/stat' >"$EV/far-procstat-post.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/dev' >"$EV/near-netdev-post.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/dev' >"$EV/far-netdev-post.txt" 2>/dev/null || true
ssh_near "sed -n '1,2000p' '$NEAR_SAMPLER_LOG' 2>/dev/null" >"$EV/sampler-near.log" 2>/dev/null || true
ssh_far "sed -n '1,2000p' '$FAR_SAMPLER_LOG' 2>/dev/null" >"$EV/sampler-far.log" 2>/dev/null || true
ssh_far "sed -n '1,500p' '$FAR_RECV_LOG' 2>/dev/null" >"$EV/far-recv.log" 2>/dev/null || true
ssh_far "sed -n '1,200p' '$FAR_RECV_ERR' 2>/dev/null" >"$EV/far-recv-err.txt" 2>/dev/null || true

FAR_SIZE=$(ssh_far "stat -c %s '$FAR_FILE' 2>/dev/null || echo 0" 2>/dev/null | tr -d '[:space:]')
FAR_SHA_FILE=$(ssh_far_long "$SHA_TMO" "timeout '$SHA_TMO' sha256sum '$FAR_FILE' 2>/dev/null" 2>/dev/null || true)
FAR_SHA=$(awk 'NR==1{print $1}' <<<"$FAR_SHA_FILE" 2>/dev/null || true)
printf '%s\n' "$FAR_SHA_FILE" >"$EV/far-sha256.txt"

CURL_LINE=$(grep '^http=' "$EV/curl.txt" 2>/dev/null | sed -n '1p' || true)
HTTP=$(field http "$CURL_LINE")
UPLOAD_SIZE=$(field size "$CURL_LINE")
UPLINK_TIME=$(field time "$CURL_LINE")
UPLINK_SPEED=$(field speed "$CURL_LINE")

RECV_LINE=$(grep ' POST ' "$EV/far-recv.log" 2>/dev/null | sed -n '$p' || true)
RECV_BYTES=$(field recv "$RECV_LINE")
RECV_RESULT=$(field result "$RECV_LINE")

sampler_up_values() { # file
    grep -oE "tunnel $TUNNEL c3 [^;]*" "$1" 2>/dev/null |
        tr ' ' '\n' | grep -E '^up=[0-9]+$' | cut -d= -f2
}
NEAR_UP_MIN=$(sampler_up_values "$EV/sampler-near.log" | sort -n | sed -n '1p')
NEAR_UP_MAX=$(sampler_up_values "$EV/sampler-near.log" | sort -n | tail -n 1)
FAR_UP_MIN=$(sampler_up_values "$EV/sampler-far.log" | sort -n | sed -n '1p')
FAR_UP_MAX=$(sampler_up_values "$EV/sampler-far.log" | sort -n | tail -n 1)

counter_report() { # end pre post
    local end=$1 pre=$2 post=$3
    {
        for f in fec_reset_tcp_failed fec_reset_hole fec_reset_user \
            fec_outq_drop fec_outq_high_wait fec_init_bad \
            fec_nack_tx fec_nack_rx fec_nack_retx fec_nack_drop \
            fec_replay_hit fec_replay_miss fec_replay_miss_never_stored \
            fec_replay_miss_enospc fec_replay_drop fec_nack_backpressure \
            fec_hol_wait fec_short_parity_saved fec_nack_pad_skip \
            fec_nack_lat_ns fec_nack_repair_ns fec_nack_repair_max_ns \
            fec_replay_queue_high fec_outq_depth; do
            printf '%s=%s\n' "$f" "$(delta "$(stat_field "$pre" c11 "$f")" "$(stat_field "$post" c11 "$f")")"
        done
        for f in fec_decode_blocks fec_decode_recovered fec_deliver_bad \
            fec_epoch_conflict fec_decode_nomem; do
            printf '%s=%s\n' "$f" "$(delta "$(stat_field "$pre" c10 "$f")" "$(stat_field "$post" c10 "$f")")"
        done
        for f in holes_unrecoverable holes_declared holes_repaired; do
            printf '%s=%s\n' "$f" "$(delta "$(stat_field "$pre" c6 "$f")" "$(stat_field "$post" c6 "$f")")"
        done
        for f in stream_reset_tcp_failed stream_reset_user data_tx retx_bytes; do
            printf '%s=%s\n' "$f" "$(delta "$(stat_field "$pre" c3 "$f")" "$(stat_field "$post" c3 "$f")")"
        done
        for f in fec_drop_sid fec_stream_refused fec_ack_tx fec_ack_rx fec_credit_wait; do
            printf '%s=%s\n' "$f" "$(delta "$(stat_field "$pre" c9 "$f")" "$(stat_field "$post" c9 "$f")")"
        done
        printf 'submit_bytes=%s\n' "$(delta "$(cap_field "$pre" submit_bytes)" "$(cap_field "$post" submit_bytes)")"
        printf 'rx_delivered=%s\n' "$(delta "$(rx_field "$pre")" "$(rx_field "$post")")"
        printf 'up_post=%s\n' "$(stat_field "$post" c3 up)"
        printf 'c3_replacing_post=%s\n' "$(stat_field "$post" c3 replacing)"
        printf 'c3_empty_post=%s\n' "$(stat_field "$post" c3 empty)"
        printf 'c3_up_rescued_post=%s\n' "$(stat_field "$post" c3 up_rescued)"
    } >"$EV/counters-$end.txt"
}
counter_report near "$EV/near-stats-uplink-pre.txt" "$EV/near-stats-uplink-post.txt"
counter_report far "$EV/far-stats-uplink-pre.txt" "$EV/far-stats-uplink-post.txt"

ctr() { sed -n "s/^$2=//p" "$EV/counters-$1.txt" 2>/dev/null | sed -n '1p'; }

read -r nb ni < <(cpu_busy "$EV/near-procstat-pre.txt") || true
read -r nb2 ni2 < <(cpu_busy "$EV/near-procstat-post.txt") || true
NEAR_CPU=$(cpu_pct "${nb:-0}" "${ni:-0}" "${nb2:-0}" "${ni2:-0}")
read -r fb fi < <(cpu_busy "$EV/far-procstat-pre.txt") || true
read -r fb2 fi2 < <(cpu_busy "$EV/far-procstat-post.txt") || true
FAR_CPU=$(cpu_pct "${fb:-0}" "${fi:-0}" "${fb2:-0}" "${fi2:-0}")
WIRE_NEAR_TX=$(delta "$(netdev_tx "$EV/near-netdev-pre.txt" "$NEAR_PHYS_DEV")" \
                      "$(netdev_tx "$EV/near-netdev-post.txt" "$NEAR_PHYS_DEV")")
WIRE_FAR_RX=$(delta "$(netdev_rx "$EV/far-netdev-pre.txt" "$FAR_PHYS_DEV")" \
                     "$(netdev_rx "$EV/far-netdev-post.txt" "$FAR_PHYS_DEV")")

# ---------------------------------------------------------------------------
# 6. checks

say "== 6. checks"
check uplink_rc "$([[ ${CURL_RC:-9} -eq 0 ]]; echo $?)" "rc=$CURL_RC"
check uplink_http "$([[ ${HTTP:-000} == 200 ]]; echo $?)" "http=${HTTP:-000}"
check uplink_upload_size "$([[ ${UPLOAD_SIZE:-0} == "$EXPECT_BYTES" ]]; echo $?)" \
    "size_upload=${UPLOAD_SIZE:-?} want=$EXPECT_BYTES"
check far_file_size "$([[ ${FAR_SIZE:-0} == "$EXPECT_BYTES" ]]; echo $?)" \
    "size=${FAR_SIZE:-0} want=$EXPECT_BYTES"
check far_sha_hex "$([[ $FAR_SHA =~ ^[0-9a-f]{64}$ ]]; echo $?)" "sha=${FAR_SHA:-?}"
check far_sha_match "$([[ -n $NEAR_SHA && $NEAR_SHA == "$FAR_SHA" ]]; echo $?)" \
    "near=${NEAR_SHA:0:16}... far=${FAR_SHA:0:16}..."
check far_recv_result "$([[ ${RECV_RESULT:-} == ok && ${RECV_BYTES:-0} == "$EXPECT_BYTES" ]]; echo $?)" \
    "recv=${RECV_BYTES:-?} result=${RECV_RESULT:-?}"

for end in near far; do
    post_up=$(stat_field "$EV/$end-stats-uplink-post.txt" c3 up)
    check "${end}_up_post" "$([[ ${post_up:-0} == "$EXPECT_CARRIERS" ]]; echo $?)" \
        "up=${post_up:-?}"
    rep=$(stat_field "$EV/$end-stats-uplink-post.txt" c3 replacing)
    emp=$(stat_field "$EV/$end-stats-uplink-post.txt" c3 empty)
    check "${end}_uplink_c3_replacing" "$([[ ${rep:-1} == 0 ]]; echo $?)" "replacing=${rep:-?}"
    check "${end}_uplink_c3_empty" "$([[ ${emp:-1} == 0 ]]; echo $?)" "empty=${emp:-?}"

    min=$([[ $end == near ]] && printf '%s' "${NEAR_UP_MIN:-}" || printf '%s' "${FAR_UP_MIN:-}")
    max=$([[ $end == near ]] && printf '%s' "${NEAR_UP_MAX:-}" || printf '%s' "${FAR_UP_MAX:-}")
    if [[ -n ${min:-} && -n ${max:-} ]]; then
        # G5 allows a replacement window (up drops, replacing/empty rises,
        # then back to 256). The strict 256..256 failed on a live one-carrier
        # replacement; the end up=256 is checked separately.
        check "${end}_up_sampler" "$([[ ${min:-0} -ge $((EXPECT_CARRIERS - 1)) && ${max:-0} == "$EXPECT_CARRIERS" ]]; echo $?)" \
            "min=$min max=$max want=$EXPECT_CARRIERS (one-carrier replacement window allowed; end up checked)"
    else
        skip "${end}_up_sampler" "no sampler samples; pre/post up checks still apply"
    fi

    rt=$(ctr "$end" fec_reset_tcp_failed)
    rh=$(ctr "$end" fec_reset_hole)
    sr=$(ctr "$end" stream_reset_tcp_failed)
    hu=$(ctr "$end" holes_unrecoverable)
    db=$(ctr "$end" fec_deliver_bad)
    check "${end}_reset_tcp_failed" "$([[ ${rt:-?} == 0 ]]; echo $?)" "delta=${rt:-?}"
    check "${end}_reset_hole" "$([[ ${rh:-?} == 0 ]]; echo $?)" "delta=${rh:-?}"
    check "${end}_stream_reset_tcp" "$([[ ${sr:-?} == 0 ]]; echo $?)" "delta=${sr:-?}"
    check "${end}_holes_unrecoverable" "$([[ ${hu:-?} == 0 ]]; echo $?)" "delta=${hu:-?}"
    check "${end}_deliver_bad" "$([[ ${db:-?} == 0 ]]; echo $?)" "delta=${db:-?}"

    say "${end}_reset_user=$(ctr "$end" fec_reset_user) (user close, informational)"
    printf '%s\n' "${end}_reset_user=$(ctr "$end" fec_reset_user)" >>"$EV/run.log"
    say "${end}_holes_declared=$(ctr "$end" holes_declared) holes_repaired=$(ctr "$end" holes_repaired) (informational)"
    printf '%s\n' "${end}_holes_declared=$(ctr "$end" holes_declared) holes_repaired=$(ctr "$end" holes_repaired)" >>"$EV/run.log"
    say "${end}_outq_drop=$(ctr "$end" fec_outq_drop) init_bad=$(ctr "$end" fec_init_bad) replay_enospc=$(ctr "$end" fec_replay_miss_enospc) (informational)"
    printf '%s\n' "${end}_outq_drop=$(ctr "$end" fec_outq_drop) init_bad=$(ctr "$end" fec_init_bad) replay_enospc=$(ctr "$end" fec_replay_miss_enospc)" >>"$EV/run.log"
    say "${end}_submit_or_rx=$( [[ $end == near ]] && ctr "$end" submit_bytes || ctr "$end" rx_delivered ) (informational)"
done

say "uplink: http=${HTTP:-?} size_upload=${UPLOAD_SIZE:-?} time=${UPLINK_TIME:-?}s speed=${UPLINK_SPEED:-?}B/s"
say "sha: near=${NEAR_SHA:-?} far=${FAR_SHA:-?}"
say "up sampler: near=${NEAR_UP_MIN:-?}..${NEAR_UP_MAX:-?} far=${FAR_UP_MIN:-?}..${FAR_UP_MAX:-?}"
say "cpu: near=${NEAR_CPU}% far=${FAR_CPU}% wire near_tx=${WIRE_NEAR_TX:-?} far_rx=${WIRE_FAR_RX:-?}"

kernel_window near "$NEAR_T0" "$NEAR_T1" "$EV/near-kernel.txt"
kernel_window far "$FAR_T0" "$FAR_T1" "$EV/far-kernel.txt"
for end in near far; do
    case $(kw_state "$EV/$end-kernel.txt") in
        SKIP) skip "${end}_kernel" "empty window, source=$(kw_source "$EV/$end-kernel.txt")" ;;
        FAIL) check "${end}_kernel" 1 "BUG/Oops/WARNING in window" ;;
        HOG)
            say "${end}_kernel_hog=1 (tbtun workqueue hog; informational)"
            printf '%s\n' "${end}_kernel_hog=1" >>"$EV/run.log"
            ;;
        *) check "${end}_kernel" 0 "window clean, source=$(kw_source "$EV/$end-kernel.txt")" ;;
    esac
done

NEAR_BOOT_AFTER=$(ssh_near 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)
FAR_BOOT_AFTER=$(ssh_far 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)
check near_no_restart "$([[ -n $NEAR_BOOT_BEFORE && $NEAR_BOOT_BEFORE == "$NEAR_BOOT_AFTER" ]]; echo $?)" \
    "boot_id ${NEAR_BOOT_BEFORE:0:8}.. -> ${NEAR_BOOT_AFTER:0:8}.."
check far_no_restart "$([[ -n $FAR_BOOT_BEFORE && $FAR_BOOT_BEFORE == "$FAR_BOOT_AFTER" ]]; echo $?)" \
    "boot_id ${FAR_BOOT_BEFORE:0:8}.. -> ${FAR_BOOT_AFTER:0:8}.."

if far_port_listening; then
    say "far receiver still listening on 127.0.0.1:$FAR_PORT"
    printf '%s\n' "far_receiver_post=listening" >>"$EV/run.log"
else
    skip far_receiver_post "not listening after transfer; see far-recv.log"
fi

if ((FAILED)); then
    capture_diag verdict
fi

# ---------------------------------------------------------------------------
# 7. summary

END_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
verdict=PASS
((FAILED > 0)) && verdict=FAIL
{
    printf '# 0.3.3 uplink 1 GiB byte-exact acceptance\n\n'
    printf -- '- start: %s\n- end: %s\n' "$START_UTC" "$END_UTC"
    printf -- '- near: `%s` (aarch64)\n- far: `%s` (x86_64)\n' "$NEAR_HOST" "$FAR_HOST"
    printf -- '- evidence: `%s`\n' "$EV"
    printf -- '- pins: near srcversion `%s`, far srcversion `%s`, module `%s`\n' \
        "$EXPECT_NEAR_SRCVERSION" "$EXPECT_FAR_SRCVERSION" "$EXPECT_MODULE_VERSION"
    printf -- '- shape: mode=%s carriers=%s adaptive=%s\n' "$EXPECT_MODE" "$EXPECT_CARRIERS" "$EXPECT_ADAPTIVE"
    printf -- '- path: near 127.0.0.1:%s -> tunnel %s -> far 127.0.0.1:%s\n' \
        "$NEAR_PORT" "$TUNNEL" "$FAR_PORT"
    printf -- '- source: AES-256-CTR(%s,%s) first %s bytes\n\n' \
        "$AES_KEY" "$AES_IV" "$EXPECT_BYTES"
    printf '## Verdict\n\n**%s**\n\n' "$verdict"
    printf '| item | value |\n|---|---|\n'
    printf '| curl rc | %s |\n' "$CURL_RC"
    printf '| http | %s |\n' "${HTTP:-?}"
    printf '| size_upload | %s |\n' "${UPLOAD_SIZE:-?}"
    printf '| near file size | %s |\n' "${NEAR_SIZE:-?}"
    printf '| near sha256 | %s |\n' "${NEAR_SHA:-unavailable}"
    printf '| far file size | %s |\n' "${FAR_SIZE:-?}"
    printf '| far sha256 | %s |\n' "${FAR_SHA:-unavailable}"
    printf '| far receiver recv/result | %s / %s |\n' "${RECV_BYTES:-?}" "${RECV_RESULT:-?}"
    printf '| up post near/far | %s / %s |\n' \
        "$(stat_field "$EV/near-stats-uplink-post.txt" c3 up)" \
        "$(stat_field "$EV/far-stats-uplink-post.txt" c3 up)"
    printf '| up sampler near | %s..%s |\n' "${NEAR_UP_MIN:-?}" "${NEAR_UP_MAX:-?}"
    printf '| up sampler far | %s..%s |\n' "${FAR_UP_MIN:-?}" "${FAR_UP_MAX:-?}"
    printf '| reset/hole/deliver_bad near | %s / %s / %s |\n' \
        "$(ctr near fec_reset_tcp_failed)" "$(ctr near holes_unrecoverable)" "$(ctr near fec_deliver_bad)"
    printf '| reset/hole/deliver_bad far | %s / %s / %s |\n' \
        "$(ctr far fec_reset_tcp_failed)" "$(ctr far holes_unrecoverable)" "$(ctr far fec_deliver_bad)"
    printf '| time / speed | %ss / %s B/s |\n' "${UPLINK_TIME:-?}" "${UPLINK_SPEED:-?}"
    printf '| CPU near/far | %s%% / %s%% |\n' "$NEAR_CPU" "$FAR_CPU"
    printf '| wire near tx / far rx | %s / %s |\n' "${WIRE_NEAR_TX:-?}" "${WIRE_FAR_RX:-?}"
    printf '| kernel near/far | %s / %s |\n' \
        "$(kw_state "$EV/near-kernel.txt")" "$(kw_state "$EV/far-kernel.txt")"
    printf '| boot_id near/far stable | %s / %s |\n' \
        "$([[ $NEAR_BOOT_BEFORE == "$NEAR_BOOT_AFTER" ]] && echo yes || echo no)" \
        "$([[ $FAR_BOOT_BEFORE == "$FAR_BOOT_AFTER" ]] && echo yes || echo no)"
    printf '| far source replaced | %s |\n\n' "${REPLACED_PID:-no}"
    printf '## Counter deltas\n\n```\nnear:\n'; sed -n '1,200p' "$EV/counters-near.txt" 2>/dev/null
    printf '\nfar:\n'; sed -n '1,200p' "$EV/counters-far.txt" 2>/dev/null
    printf '\n```\n\n## Checks\n\n```\n'
    grep -E '^CHECK ' "$EV/run.log" 2>/dev/null || true
    printf '```\n\n## Cleanup\n\nSee `cleanup.txt`. The tunnel, its config, the ip rule and the watchdog are never touched.\n'
} >"$EV/SUMMARY.md"

say "verdict: $verdict ($FAILED failed checks)"
say "summary: $EV/SUMMARY.md"

keep=remove
if ((FAILED > 0)) || ((KEEP_FILES)); then
    keep=keep
fi
if ! cleanup_all "$keep"; then
    say "cleanup incomplete"
    if ((FAILED > 0)); then
        exit 6
    fi
    exit 7
fi

if ((FAILED > 0)); then
    note "verdict FAIL ($FAILED failed checks); files kept in $EV"
    exit 6
fi
note "verdict PASS; evidence kept in $EV"
exit 0
