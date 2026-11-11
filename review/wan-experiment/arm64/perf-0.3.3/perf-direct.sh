#!/usr/bin/env bash
# perf-direct.sh - single-TCP direct baseline between the same near and far.
#
# It answers one question: what does one TCP flow between these two hosts
# do on this link at this hour, with no tbtun in the path? The answer is the
# control for perf-ab.sh. A tunnel number that moves while this control also
# moves is a link change, not a tbtun change.
#
# Path under test:
#
#   near  curl http://45.11.1.62:5211/accept-1g.bin
#     -> far public NIC -> far-raw-serve.py on 0.0.0.0:5211
#     -> the same 1 GiB file the tunnel's far side reads from 127.0.0.1:5210
#
# One connection at a time, one GET per window, the same 60 s boundary
# method as perf-ab.sh. Latency is 100 sequential HEADs before and after
# the throughput windows. The TCP RTT is read from ss at the window end.
#
# This script does not unload tbtun. Direct traffic does not traverse it,
# and tbtun is idle between sessions; the module state is recorded in
# meta.txt so a reader can see it.
#
# Modes
#   --run        three throughput windows plus latency, then stop the server
#   --preflight  read-only checks
#   --status     print the server and the last result
#   --stop-server stop only the server this script started
#   --cleanup    stop the server and remove the near rule this script added
#   --dry-run    print the plan
#
# Exit codes
#   0 done   2 usage   3 preflight   4 setup   6 measurement
set -uo pipefail

PROG=${0##*/}
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)

MODE=run
NEAR_HOST=${NEAR_HOST:-root@192.168.123.85}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
FAR_IP=${FAR_IP:-45.11.1.62}
NEAR_IP=${NEAR_IP:-192.168.123.85}
NEAR_DEV=${NEAR_DEV:-eth0}
FAR_DEV=${FAR_DEV:-eth0}
DIRECT_PORT=${DIRECT_PORT:-5211}
SOURCE=${SOURCE:-/root/sclc-verify-1g/accept-1g.bin}
SERVER_PY=${SERVER_PY:-/root/sclc-verify/far-raw-serve.py}
FAR_DIR=${FAR_DIR:-/root/perf-direct}
LOG=$FAR_DIR/raw-serve.log
PIDFILE=$FAR_DIR/raw-serve.pid
BYPASS_PREF=${BYPASS_PREF:-100}
BYPASS_MARK=${BYPASS_MARK:-/root/.tbt-perf-direct-bypass-pref100}
CUR_PID=
SERVER_STARTED=0
REMOTE_DIR=/root/perf-0.3.3
CURLER=$REMOTE_DIR/perf-curl.sh

SSH_TMO=${SSH_TMO:-20}
RUNS=${RUNS:-3}
SECS=${SECS:-60}
WARMUP=${WARMUP:-20}
GAP=${GAP:-10}
LAT_ITERS=${LAT_ITERS:-100}
OUT=${OUT:-}
TAG=${TAG:-}
KEEP_SERVER=${KEEP_SERVER:-0}
DRY=${DRY_RUN:-0}

CSV_HEADER='stamp,dataset,block,order,version,run,window_s,warmup_s,app_bytes,app_source,wchar_bytes,curl_requests,stream_alive,app_mbps,wchar_mbps,wire_mbps,wire_over_app,ack_tx,ack_rx,nack_tx,nack_rx,retx_bytes,c3_retx_bytes,cpu_near_pct,cpu_far_pct,load1_near,load1_far,temp_near_c,temp_far_c,freq_near_mhz,freq_far_mhz,rtt_near_us,rtt_far_us,up_min_near,up_min_far,up_end_near,up_end_far,c3_lost,c3_replaced,c3_stall_declared,c3_stall_recovered,c3_stall_dead,c6_holes,c6_stall_us,c11_nack_drop,c11_replay_miss,c9_drop_sid,c9_stream_refused,valid,noisy,rc,note'
CSV_NF=52

say() { printf '%s\n' "$*"; }
note() {
    if [ -n "${OUT:-}" ]; then
        printf '%s %s\n' "$(date -u +%H:%M:%SZ)" "$*" | tee -a "$OUT/run.log" >&2
    else
        printf '%s\n' "$*" >&2
    fi
}
die() { note "FATAL: $*"; exit "${2:-4}"; }

usage() {
    cat <<EOF
usage: $PROG [--run|--preflight|--status|--stop-server|--cleanup|--dry-run]

  --port 5211          far public listener for the direct source
  --runs 3 --secs 60 --warmup 20 --gap 10
  --lat-iters 100      sequential HEADs before and after the windows
  --out DIR            evidence directory
  --tag NAME
  --keep-server        leave the far listener running

environment:
  NEAR_HOST=$NEAR_HOST  FAR_HOST=$FAR_HOST  FAR_IP=$FAR_IP
  SOURCE=$SOURCE
  SERVER_PY=$SERVER_PY  FAR_DIR=$FAR_DIR
EOF
}

while (($#)); do
    case $1 in
        --run) MODE=run ;;
        --preflight) MODE=preflight ;;
        --status) MODE=status ;;
        --stop-server) MODE=stop ;;
        --cleanup) MODE=cleanup ;;
        --dry-run) DRY=1 ;;
        --port) DIRECT_PORT=$2; shift ;;
        --runs) RUNS=$2; shift ;;
        --secs) SECS=$2; shift ;;
        --warmup) WARMUP=$2; shift ;;
        --gap) GAP=$2; shift ;;
        --lat-iters) LAT_ITERS=$2; shift ;;
        --out) OUT=$2; shift ;;
        --tag) TAG=$2; shift ;;
        --keep-server) KEEP_SERVER=1 ;;
        -h | --help) usage; exit 0 ;;
        *) printf '%s: unknown argument: %s\n' "$PROG" "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

case $RUNS in '' | *[!0-9]*) die "--runs must be a positive integer" 2 ;; esac
case $SECS in '' | *[!0-9]*) die "--secs must be a positive integer" 2 ;; esac
[ "$RUNS" -ge 1 ] || die "--runs must be >= 1" 2
[ "$SECS" -ge 10 ] || die "--secs must be >= 10" 2

ssh_near() { timeout "$SSH_TMO" ssh -o BatchMode=yes "$NEAR_HOST" "$@"; }
ssh_far() { timeout "$SSH_TMO" ssh -o BatchMode=yes "$FAR_HOST" "$@"; }
ssh_near_long() { timeout "$1" ssh -o BatchMode=yes "$NEAR_HOST" "$2"; }
ssh_end() { local e=$1; shift; if [ "$e" = far ]; then ssh_far "$@"; else ssh_near "$@"; fi; }
scp_near() { timeout "$SSH_TMO" scp -q "$@"; }

num() { case ${1:-} in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }
nz() { if num "${1:-}"; then printf '%s' "$1"; else printf 0; fi; }
delta() { if num "${1:-}" && num "${2:-}"; then printf '%s' "$(( $2 - $1 ))"; else printf ''; fi; }
row_add() { if [ -z "${ROW:-}" ]; then ROW=$1; else ROW=$ROW,$1; fi; }
csv_nf() { printf '%s' "$1" | awk -F, '{ print NF }'; }

# The direct flow must leave through the physical NIC. Without this rule a
# mihomo/VPN policy route captures it and the baseline measures a proxy.
bypass_ensure() {
    local out
    out=$(ssh_near "FAR_IP='$FAR_IP' PREF='$BYPASS_PREF' MARK='$BYPASS_MARK' DEV='$NEAR_DEV' sh -s" <<'EOS'
set -u
exists=0
ip rule show 2>/dev/null | awk -v p="$PREF:" -v ip="$FAR_IP" '
    $1 == p {
        for (i = 2; i < NF; i++)
            if ($i == "to" && ($(i + 1) == ip || $(i + 1) == ip "/32")) f = 1
    }
    END { exit(f ? 0 : 1) }' && exists=1
if [ "$exists" -eq 0 ]; then
    if ip rule add to "$FAR_IP/32" lookup main pref "$PREF" 2>/dev/null; then
        printf 'pref=%s to=%s/32\n' "$PREF" "$FAR_IP" >"$MARK" 2>/dev/null || true
        printf 'RULE=added\n'
    else
        printf 'RULE=failed\n'
    fi
else
    printf 'RULE=preexisting\n'
fi
dev=$(ip route get "$FAR_IP" 2>/dev/null |
    awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }')
printf 'DEV=%s\n' "${dev:-none}"
EOS
)
    printf '%s\n' "$out"
    printf '%s\n' "$out" | grep -q '^RULE=failed' && return 1
    case $(printf '%s\n' "$out" | sed -n 's/^DEV=//p' | tail -n 1) in
        "$NEAR_DEV") return 0 ;;
        *) return 1 ;;
    esac
}

bypass_cleanup() {
    ssh_near "FAR_IP='$FAR_IP' PREF='$BYPASS_PREF' MARK='$BYPASS_MARK' sh -s" <<'EOS' >/dev/null 2>&1 || true
set -u
[ -f "$MARK" ] || exit 0
grep -qF "to=$FAR_IP/32" "$MARK" 2>/dev/null || exit 0
ip rule del pref "$PREF" to "$FAR_IP/32" lookup main 2>/dev/null ||
    ip rule del pref "$PREF" to "$FAR_IP/32" 2>/dev/null || true
rm -f "$MARK"
EOS
}

netdev_rx() { awk -v d="$2:" '$1 == d { print $2; exit }' "$1" 2>/dev/null; }
netdev_tx() { awk -v d="$2:" '$1 == d { print $10; exit }' "$1" 2>/dev/null; }
cpu_busy() {
    awk 'NR == 1 { b = $2+$3+$4+$7+$8+$9; i = $5+$6; printf "%s %s\n", b, i }' "$1" 2>/dev/null
}
cpu_pct() {
    awk -v b1="$1" -v i1="$2" -v b2="$3" -v i2="$4" 'BEGIN {
        b = b2-b1; i = i2-i1; t = b+i
        if (t > 0) printf "%.1f", b*100/t; else printf ""
    }'
}
snap_section() { awk -v s="=== $2 ===" '$0 == s { getline; print; exit }' "$1" 2>/dev/null; }
snap_date() { snap_section "$1" date; }
snap_tz() { snap_section "$1" thermal; }
snap_freq() { snap_section "$1" freq; }
snap_load() { snap_section "$1" loadavg | awk '{print $1}'; }
thermal_max() {
    awk '{ for (i=1;i<=NF;i++) if ($i+0>m) m=$i+0 } END {
        if (m>0) printf "%.1f", m/1000; else printf ""
    }' <<<"${1:-}" 2>/dev/null
}

snapshot() { # near|far outfile
    ssh_end "$1" 'bash -s' >"$2" 2>&1 <<'EOS' || true
set -u
s() { printf '\n=== %s ===\n' "$1"; }
s date; date +%s
s loadavg; cat /proc/loadavg 2>/dev/null || true
s clk; getconf CLK_TCK 2>/dev/null || echo 100
s procstat; head -1 /proc/stat 2>/dev/null || true
s netdev; cat /proc/net/dev 2>/dev/null || true
s thermal; for z in /sys/class/thermal/thermal_zone*/temp; do
    [ -r "$z" ] && cat "$z" 2>/dev/null && printf ' '
done; printf '\n'
s freq; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || true
s tbtun; lsmod | awk '$1=="tbtun"{print "loaded"; found=1} END{if(!found) print "absent"}' 2>/dev/null || true
s boot; cat /proc/sys/kernel/random/boot_id 2>/dev/null || true
EOS
}

server_pid() { ssh_far "cat '$PIDFILE' 2>/dev/null" 2>/dev/null | tr -d '[:space:]'; }
server_alive() {
    local pid=$1
    [ -n "$pid" ] || return 1
    ssh_far "kill -0 '$pid' 2>/dev/null && echo alive || echo dead" 2>/dev/null | grep -q alive
}

start_server() {
    local code length size i
    size=$(ssh_far "stat -c %s '$SOURCE' 2>/dev/null || echo 0" 2>/dev/null || echo 0)
    code=$(ssh_far "curl -sS -o /dev/null -w '%{http_code}' -I -m 3 \
        'http://127.0.0.1:$DIRECT_PORT/accept-1g.bin' 2>/dev/null" || true)
    length=$(ssh_far "curl -sSI -m 3 'http://127.0.0.1:$DIRECT_PORT/accept-1g.bin' 2>/dev/null |
        awk 'BEGIN{IGNORECASE=1} /^Content-Length:/{gsub(/\r/,\"\"); print \$2; exit}'" 2>/dev/null || true)
    if [ "$code" = 200 ] && [ "${length:-0}" = "$size" ]; then
        note "far:$DIRECT_PORT already serves the same source; reusing it"
        SERVER_STARTED=0
        return 0
    fi
    ssh_far "mkdir -p '$FAR_DIR'; rm -f '$PIDFILE'
        setsid python3 '$SERVER_PY' --file '$SOURCE' --bind 0.0.0.0 --port '$DIRECT_PORT' \
            --log '$LOG' --pidfile '$PIDFILE' </dev/null >'$FAR_DIR/server.err' 2>&1 &
        echo started" >/dev/null 2>&1
    SERVER_STARTED=1
    for i in $(seq 1 30); do
        code=$(ssh_near "curl -sS -o /dev/null -w '%{http_code}' -I -m 3 \
            'http://$FAR_IP:$DIRECT_PORT/accept-1g.bin' 2>/dev/null" || true)
        length=$(ssh_near "curl -sSI -m 3 'http://$FAR_IP:$DIRECT_PORT/accept-1g.bin' 2>/dev/null |
            awk 'BEGIN{IGNORECASE=1} /^Content-Length:/{gsub(/\r/,\"\"); print \$2; exit}'" 2>/dev/null || true)
        [ "$code" = 200 ] && [ "${length:-0}" = "$size" ] &&
            { note "far direct source ready after ${i}s (size=$size)"; return 0; }
        sleep 1
    done
    return 1
}

stop_server() {
    local pid cmd
    pid=$(server_pid)
    if [ -n "$pid" ] && server_alive "$pid"; then
        cmd=$(ssh_far "tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null" 2>/dev/null || true)
        case "$cmd" in
            *far-raw-serve.py*)
                ssh_far "kill '$pid' 2>/dev/null || true" >/dev/null 2>&1 || true
                note "far direct server stopped (pid=$pid)"
                ;;
            *)
                note "pidfile pid=$pid is not far-raw-serve.py; leaving it alone"
                ;;
        esac
    else
        note "far direct server was not running"
    fi
    ssh_far "rm -f '$PIDFILE'" >/dev/null 2>&1 || true
}

start_curl() { # url timeout pidfile outfile
    ssh_near "setsid sh '$CURLER' '$1' '$3' '$4' '$2' \
        </dev/null >/dev/null 2>&1 &
        echo started" >/dev/null 2>&1
}
curl_pid() { ssh_near "cat '$1' 2>/dev/null" 2>/dev/null | tr -d '[:space:]'; }
pid_alive() { [ -n "$1" ] || return 1; ssh_near "kill -0 '$1' 2>/dev/null && echo alive || echo dead" 2>/dev/null | grep -q alive; }
wchar_of() { [ -n "$1" ] || { printf ''; return 0; }; ssh_near "awk '/^wchar:/{print \$2; exit}' /proc/$1/io 2>/dev/null" 2>/dev/null | tr -d '[:space:]'; }
kill_curl() {
    [ -n "$1" ] || return 0
    ssh_near "kill -TERM '$1' 2>/dev/null || true
        i=0; while [ \$i -lt 10 ] && kill -0 '$1' 2>/dev/null; do sleep 1; i=\$((i+1)); done
        kill -KILL '$1' 2>/dev/null || true" >/dev/null 2>&1 || true
}

cleanup_traffic() {
    [ -n "${CUR_PID:-}" ] && ssh_near "kill -KILL '$CUR_PID' 2>/dev/null || true" >/dev/null 2>&1 || true
    CUR_PID=
    [ "${SERVER_STARTED:-0}" = 1 ] && [ "${KEEP_SERVER:-0}" = 0 ] && stop_server
}

latency_phase() { # phase outfile
    local phase=$1 out=$2
    local remote=/tmp/perf-direct-lat-$phase.csv
    ssh_near_long 900 "rm -f '$remote'
        i=0
        while [ \$i -lt $LAT_ITERS ]; do
            curl -sS -o /dev/null -I -m 5 \
                -w '%{http_code},%{time_connect},%{time_starttransfer},%{time_total}\n' \
                'http://$FAR_IP:$DIRECT_PORT/accept-1g.bin' >>'$remote' 2>/dev/null || true
            i=\$((i+1))
        done
        echo done" >/dev/null 2>&1
    ssh_near "cat '$remote' 2>/dev/null" >"$out.tmp" 2>/dev/null || true
    [ -f "$out.tmp" ] || : >"$out.tmp"
    : >"$out"
    printf '%s\n' 'phase,idx,http,connect_s,ttfb_s,total_s' >"$out"
    awk -F, -v p="$phase" '{ printf "%s,%d,%s,%s,%s,%s\n", p, NR, $1, $2, $3, $4 }' "$out.tmp" >>"$out"
    rm -f "$out.tmp"
}

ss_rtt_us() { # near -> far:DIRECT_PORT
    local r
    r=$(ssh_near "ss -tinH state established '( dport = :$DIRECT_PORT )' 2>/dev/null |
        sed -n 's/.*rtt:\\([0-9.]*\\)\\/.*/\\1/p' | head -n 1" 2>/dev/null | tr -d '[:space:]')
    if [ -n "$r" ]; then awk -v r="$r" 'BEGIN{printf "%.0f", r*1000}'; fi
}

run_window() { # run
    local run=$1 dir rdir pid alive wa wb app wire
    local cpa cpb cfa cfb n2 n3 f2 f3
    local t_a t_b window far_window n_cpu f_cpu n_load f_load n_temp f_temp n_freq f_freq
    local rtt valid noisy note_txt

    dir=$(printf '%s/runs/direct-r%d' "$OUT" "$run")
    rdir=/tmp/perf-direct/run-$run
    mkdir -p "$dir"
    ssh_near "mkdir -p '$rdir'" >/dev/null 2>&1 || die "cannot create $rdir on near"
    note "direct run $run"

    start_curl "http://$FAR_IP:$DIRECT_PORT/accept-1g.bin" "$((WARMUP + SECS + 60))" \
        "$rdir/curl.pid" "$rdir/curl.out"
    sleep 2
    pid=$(curl_pid "$rdir/curl.pid")
    CUR_PID=$pid
    alive=0
    pid_alive "$pid" && alive=1

    sleep "$WARMUP"
    snapshot near "$dir/near-a.txt"
    snapshot far "$dir/far-a.txt"
    wa=$(wchar_of "$pid")

    sleep "$SECS"
    alive=0
    pid_alive "$pid" && alive=1
    snapshot near "$dir/near-b.txt"
    snapshot far "$dir/far-b.txt"
    wb=$(wchar_of "$pid")
    rtt=$(ss_rtt_us)
    kill_curl "$pid"
    CUR_PID=
    ssh_near "cat '$rdir/curl.out' 2>/dev/null" >"$dir/curl.out" 2>/dev/null || true
    ssh_near "cat '$rdir/curl.pid' 2>/dev/null" >"$dir/curl.pid" 2>/dev/null || true

    t_a=$(snap_date "$dir/near-a.txt")
    t_b=$(snap_date "$dir/near-b.txt")
    window=$(delta "$t_a" "$t_b")
    case ${window:-} in '' | *[!0-9]*) window=0 ;; esac
    far_window=$(delta "$(snap_date "$dir/far-a.txt")" "$(snap_date "$dir/far-b.txt")")
    case ${far_window:-} in '' | *[!0-9]*) far_window=$window ;; esac

    app=$(delta "${wa:-}" "${wb:-}")
    case ${app:-} in '' | *[!0-9]*) app=0 ;; esac
    wire=$(delta "$(netdev_tx "$dir/far-a.txt" "$FAR_DEV")" "$(netdev_tx "$dir/far-b.txt" "$FAR_DEV")")
    case ${wire:-} in '' | *[!0-9]*) wire=0 ;; esac

    read -r cpa cpb < <(cpu_busy "$dir/near-a.txt") || true
    read -r cfa cfb < <(cpu_busy "$dir/far-a.txt") || true
    read -r n2 n3 < <(cpu_busy "$dir/near-b.txt") || true
    read -r f2 f3 < <(cpu_busy "$dir/far-b.txt") || true
    n_cpu=$(cpu_pct "${cpa:-0}" "${cpb:-0}" "${n2:-0}" "${n3:-0}")
    f_cpu=$(cpu_pct "${cfa:-0}" "${cfb:-0}" "${f2:-0}" "${f3:-0}")
    n_load=$(snap_load "$dir/near-b.txt")
    f_load=$(snap_load "$dir/far-b.txt")
    n_temp=$(thermal_max "$(snap_tz "$dir/near-b.txt")")
    f_temp=$(thermal_max "$(snap_tz "$dir/far-b.txt")")
    n_freq=$(snap_freq "$dir/near-b.txt")
    f_freq=$(snap_freq "$dir/far-b.txt")
    [ -n "$n_freq" ] && n_freq=$(awk -v f="$n_freq" 'BEGIN{printf "%.0f", f/1000}') || n_freq=
    [ -n "$f_freq" ] && f_freq=$(awk -v f="$f_freq" 'BEGIN{printf "%.0f", f/1000}') || f_freq=

    valid=1
    [ "$window" -ge $((SECS - 2)) ] && [ "$window" -le $((SECS + 5)) ] || valid=0
    [ "$app" -gt 0 ] || valid=0
    [ "$alive" = 1 ] || valid=0
    noisy=0
    note_txt=direct
    [ "$alive" = 1 ] || { noisy=1; note_txt="$note_txt+short_stream"; }
    [ "$valid" = 1 ] || note_txt="$note_txt+invalid"

    app_mbps=$(awk -v b="$app" -v s="$window" 'BEGIN{ if(s>0) printf "%.3f", b*8/s/1e6; else printf "0" }')
    wire_mbps=$(awk -v b="$wire" -v s="$far_window" 'BEGIN{ if(s>0) printf "%.3f", b*8/s/1e6; else printf "0" }')
    wire_over=$(awk -v w="$wire" -v a="$app" 'BEGIN{ if(a>0) printf "%.4f", w/a; else printf "0" }')

    {
        printf 'window_s=%s far_window_s=%s\n' "$window" "$far_window"
        printf 'app_bytes=%s source=curl_wchar wire_bytes=%s\n' "$app" "$wire"
        printf 'app_mbps=%s wire_mbps=%s wire_over_app=%s tcp_rtt_us=%s\n' \
            "$app_mbps" "$wire_mbps" "$wire_over" "${rtt:-?}"
        printf 'cpu_near=%s cpu_far=%s\n' "${n_cpu:-?}" "${f_cpu:-?}"
        printf 'valid=%s noisy=%s note=%s\n' "$valid" "$noisy" "$note_txt"
    } >"$dir/result.txt"

    ROW=
    row_add "$(date -u +%Y%m%dT%H%M%SZ)"
    row_add direct
    row_add 1
    row_add 1
    row_add direct
    row_add "$run"
    row_add "$window"
    row_add "$WARMUP"
    row_add "$app"
    row_add curl_wchar
    row_add "$app"
    row_add 1
    row_add "$alive"
    row_add "$app_mbps"
    row_add "$app_mbps"
    row_add "$wire_mbps"
    row_add "$wire_over"
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add "${n_cpu:-}"
    row_add "${f_cpu:-}"
    row_add "${n_load:-}"
    row_add "${f_load:-}"
    row_add "${n_temp:-}"
    row_add "${f_temp:-}"
    row_add "${n_freq:-}"
    row_add "${f_freq:-}"
    row_add "${rtt:-}"
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add ''
    row_add "$valid"
    row_add "$noisy"
    row_add 0
    row_add "$note_txt"

    nf=$(csv_nf "$ROW")
    [ "$nf" = "$CSV_NF" ] || die "internal: row has $nf fields, expected $CSV_NF" 6
    printf '%s\n' "$ROW" >>"$OUT/runs.csv"
    note "direct run $run: app=${app_mbps} Mbit/s wire=${wire_mbps} rtt=${rtt:-?}us valid=$valid"
    sleep "$GAP"
}

preflight_mode() {
    local src_size code route dev far_dev tbt rule
    say "== preflight"
    ssh_near true >/dev/null 2>&1 || die "near unreachable: $NEAR_HOST" 3
    ssh_far true >/dev/null 2>&1 || die "far unreachable: $FAR_HOST" 3
    for h in "$NEAR_HOST" "$FAR_HOST"; do
        ssh -o BatchMode=yes -o ConnectTimeout="$SSH_TMO" "$h" \
            'test "$(id -u)" = 0 && command -v curl >/dev/null && command -v awk >/dev/null &&
             command -v sha256sum >/dev/null' ||
            die "$h: need root plus curl awk sha256sum" 3
    done
    ssh_far "test -f '$SERVER_PY'" || die "far server missing: $SERVER_PY" 3
    ssh_far "command -v python3 >/dev/null" || die "far has no python3" 3
    src_size=$(ssh_far "stat -c %s '$SOURCE' 2>/dev/null || echo 0" 2>/dev/null || echo 0)
    [ "${src_size:-0}" -gt 0 ] || die "far source missing: $SOURCE" 3

    tbt=$(ssh_near 'lsmod | grep -q "^tbtun " && echo loaded || echo absent' 2>/dev/null || true)
    [ "$tbt" = loaded ] && say "   note: tbtun is loaded; direct traffic still bypasses it"

    rule=$(ssh_near "ip rule show 2>/dev/null" 2>/dev/null |
        awk -v p="$BYPASS_PREF:" -v ip="$FAR_IP" '
            $1 == p { for (i = 2; i < NF; i++)
                if ($i == "to" && ($(i+1) == ip || $(i+1) == ip "/32")) f = 1 }
            END { print(f ? "present" : "absent") }')
    route=$(ssh_near "ip route get $FAR_IP 2>/dev/null" 2>/dev/null || true)
    dev=$(printf '%s\n' "$route" | awk '{ for (i=1;i<NF;i++) if ($i=="dev") { print $(i+1); exit } }')
    if [ "$rule" = present ] && [ "$dev" != "$NEAR_DEV" ]; then
        die "near pref $BYPASS_PREF rule exists but route dev=$dev, want $NEAR_DEV" 3
    fi
    far_dev=$(ssh_far "ip route get $NEAR_IP 2>/dev/null" 2>/dev/null |
        awk '{ for (i=1;i<NF;i++) if ($i=="dev") { print $(i+1); exit } }')
    if [ -n "$far_dev" ] && [ "$far_dev" != "$FAR_DEV" ]; then
        die "far route to $NEAR_IP is dev=$far_dev, want $FAR_DEV (set FAR_DEV if the NIC differs)" 3
    fi
    say "   source $SOURCE ($src_size bytes) via far:$DIRECT_PORT"
    say "   near dev=$dev far dev=$FAR_DEV tbtun=$tbt  bypass rule=$rule"
    [ "$rule" = present ] || say "   bypass rule absent: --run will add pref $BYPASS_PREF"
}

status_mode() {
    say "== far server"
    ssh_far "echo pid=\$(cat '$PIDFILE' 2>/dev/null || echo none)
        ss -ltnH 'sport = :$DIRECT_PORT' 2>/dev/null || true
        tail -n 5 '$LOG' 2>/dev/null || true" 2>/dev/null || true
    if [ -f "${OUT:-/nonexistent}/runs.csv" ]; then
        say "== last rows"
        tail -n 5 "$OUT/runs.csv"
    fi
}

plan() {
    say "dry-run: $PROG"
    say "  near=$NEAR_HOST ($NEAR_IP) far=$FAR_HOST ($FAR_IP)"
    say "  direct URL: http://$FAR_IP:$DIRECT_PORT/accept-1g.bin"
    say "  source: $SOURCE; server: $SERVER_PY on far 0.0.0.0:$DIRECT_PORT"
    say "  windows: $RUNS x ${SECS}s + ${WARMUP}s warm-up; latency $LAT_ITERS HEADs before and after"
    say "  one TCP flow per window; no tbtun in the path"
    say "  output: ${OUT:-<script>/results/<UTC>-direct}"
    say "  no ssh, no writes (dry-run)"
}

if [ "$DRY" = 1 ]; then plan; exit 0; fi

case $MODE in
    preflight) preflight_mode; exit 0 ;;
    status) status_mode; exit 0 ;;
    stop) stop_server; exit 0 ;;
    cleanup)
        stop_server
        bypass_cleanup
        ssh_near "rm -rf /tmp/perf-direct /tmp/perf-direct-lat-*.csv" >/dev/null 2>&1 || true
        exit 0
        ;;
    run) ;;
esac

OUT=${OUT:-$SCRIPT_DIR/results/$(date -u +%Y%m%dT%H%M%SZ)-direct${TAG:+-$TAG}}
mkdir -p "$OUT/runs"
: >"$OUT/run.log"
printf '%s\n' "$CSV_HEADER" >"$OUT/runs.csv"
printf 'start=%s\nhost_near=%s\nhost_far=%s\nport=%s\nsource=%s\nruns=%s\nsecs=%s\nwarmup=%s\nlat_iters=%s\n' \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$NEAR_HOST" "$FAR_HOST" "$DIRECT_PORT" "$SOURCE" "$RUNS" "$SECS" "$WARMUP" "$LAT_ITERS" \
    >"$OUT/meta.txt"
git -C "$SCRIPT_DIR" rev-parse HEAD >>"$OUT/meta.txt" 2>/dev/null || true
sha256sum "$0" >>"$OUT/meta.txt" 2>/dev/null || true

trap cleanup_traffic EXIT
preflight_mode || exit 3
bypass_out=$(bypass_ensure) || {
    printf '%s\n' "$bypass_out" >&2
    die "near bypass failed: the direct flow would leave through the wrong device" 3
}
printf '%s\n' "$bypass_out" >>"$OUT/meta.txt"

ssh_near "mkdir -p '$REMOTE_DIR'" >/dev/null 2>&1 || die "cannot create $REMOTE_DIR on near" 4
scp_near "$SCRIPT_DIR/perf-curl.sh" "$NEAR_HOST:$REMOTE_DIR/perf-curl.sh" ||
    die "cannot upload curl helper to near" 4

start_server || die "far direct source did not become ready on $DIRECT_PORT" 4

latency_phase pre "$OUT/latency-pre.csv"
for ((r = 1; r <= RUNS; r++)); do
    run_window "$r"
done
latency_phase post "$OUT/latency-post.csv"

cat "$OUT/latency-pre.csv" >"$OUT/latency.csv"
tail -n +2 "$OUT/latency-post.csv" >>"$OUT/latency.csv"

if [ "$KEEP_SERVER" = 0 ]; then
    stop_server
    SERVER_STARTED=0
else
    note "keep-server=1: far listener left running"
fi
bypass_cleanup
ssh_near "rm -rf /tmp/perf-direct" >/dev/null 2>&1 || true
printf 'end=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$OUT/meta.txt"

if [ -x "$SCRIPT_DIR/perf-stats.py" ]; then
    "$SCRIPT_DIR/perf-stats.py" "$OUT/runs.csv" --latency "$OUT/latency.csv" --out "$OUT/analysis" >/dev/null 2>&1 &&
        note "analysis: $OUT/analysis/summary.md"
fi
note "done: $OUT/runs.csv"
say "$OUT"
exit 0
