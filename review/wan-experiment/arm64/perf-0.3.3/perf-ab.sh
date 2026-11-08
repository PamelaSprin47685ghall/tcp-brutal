#!/usr/bin/env bash
# perf-ab.sh - same-link performance A/B for tbtun 0.3.0 / 0.3.1 / 0.3.2.
#
# One version per session: unload tbtun, load the .ko named by the manifest,
# write the same SC-LDPC shape on both ends, wait for the 256-carrier join,
# run N x 60 s steady windows, then move to the next version. Blocks run in
# opposite order (ABC, CBA) so a monotonic drift in the link or the host does
# not favour one version. Every run keeps its raw snapshots and one CSV row.
#
# It does not build modules, start the far 1 GiB source, judge correctness,
# or change the kernel. It does not retry a window: a failed window is
# recorded and the next one starts.
#
# Modes
#   --run         full A/B (default)
#   --preflight   read-only checks: ssh, tools, module files, source, route
#   --status      print both configs and the c3/c6/c9/c10/c11 lines
#   --cleanup     del tunnel, rmmod both ends, remove the rule this script added
#   --dry-run     print the plan; no ssh, no writes
#
# Exit codes
#   0 done   2 usage   3 preflight   4 setup   5 join   6 measurement
#
# Measured fields (all per 60 s window):
#   app      bytes the far target was served (capture_runs submit_bytes)
#   wchar    bytes the near curl wrote to /dev/null (/proc/PID/io)
#   wire     bytes the far physical NIC transmitted (/proc/net/dev)
#   ack      c9 fec_ack_tx / fec_ack_rx deltas
#   CPU      /proc/stat busy delta over the window, both ends
#   link     c3 up/lost/replaced/stall*, c6 holes, c11 NACK/replay
#   RTT      c3 rtt_mean_us at the window end (carrier TCP SRTT)
set -uo pipefail

PROG=${0##*/}
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)

MODE=run
NEAR_HOST=${NEAR_HOST:-root@192.168.123.85}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
FAR_IP=${FAR_IP:-45.11.1.62}
NEAR_IP=${NEAR_IP:-192.168.123.85}
NEAR_PORT=${NEAR_PORT:-9001}
FAR_PORT=${FAR_PORT:-9000}
TARGET_PORT=${TARGET_PORT:-5210}
TUNNEL=${TUNNEL:-1}
CARRIERS=${CARRIERS:-256}
NEAR_DEV=${NEAR_DEV:-eth0}
FAR_DEV=${FAR_DEV:-eth0}
NONCE=${NONCE:-00112233445566778899aabbccddeeff}
SOURCE=${SOURCE:-/root/sclc-verify-1g/accept-1g.bin}
SSH_TMO=${SSH_TMO:-20}
JOIN_TIMEOUT=${JOIN_TIMEOUT:-180}
SETTLE_TMO=${SETTLE_TMO:-60}
SETTLE=${SETTLE:-60}

RUNS=${RUNS:-3}
BLOCKS=${BLOCKS:-2}
SECS=${SECS:-60}
WARMUP=${WARMUP:-20}
GAP=${GAP:-10}
VERSIONS=${VERSIONS:-"0.3.0 0.3.1 0.3.2"}
MANIFEST=${MANIFEST:-$SCRIPT_DIR/perf-versions.conf}
OUT=${OUT:-}
TAG=${TAG:-}
KEEP=${KEEP:-0}
DRY=${DRY_RUN:-0}

BYPASS_PREF=${BYPASS_PREF:-100}
BYPASS_MARK=${BYPASS_MARK:-/root/.tbt-perf-bypass-pref100}
CUR_PID=
NEAR_STOP=
FAR_STOP=
REMOTE_DIR=/root/perf-0.3.3
SAMPLER=$REMOTE_DIR/perf-sampler.sh
CURLER=$REMOTE_DIR/perf-curl.sh

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
die() {
    note "FATAL: $*"
    exit "${2:-4}"
}

usage() {
    cat <<EOF
usage: $PROG [--run|--preflight|--status|--cleanup|--dry-run]

  --versions "0.3.0 0.3.1 0.3.2"   versions to compare
  --manifest FILE                  name near_ko far_ko per line
  --runs 3                         steady windows per session
  --blocks 2                       ABC then CBA; 1 disables the reversal
  --secs 60 --warmup 20 --gap 10 --settle 60
  --out DIR                        evidence directory (UTC stamp by default)
  --tag NAME                       suffix for the default output directory
  --keep                           leave modules and config loaded

environment:
  NEAR_HOST=$NEAR_HOST  FAR_HOST=$FAR_HOST  FAR_IP=$FAR_IP
  NEAR_PORT=$NEAR_PORT  FAR_PORT=$FAR_PORT  TARGET_PORT=$TARGET_PORT
  CARRIERS=$CARRIERS    SOURCE=$SOURCE
  JOIN_TIMEOUT=$JOIN_TIMEOUT  SETTLE=$SETTLE  SSH_TMO=$SSH_TMO
  BYPASS_PREF=$BYPASS_PREF    NEAR_DEV=$NEAR_DEV  FAR_DEV=$FAR_DEV
EOF
}

while (($#)); do
    case $1 in
        --run) MODE=run ;;
        --preflight) MODE=preflight ;;
        --status) MODE=status ;;
        --cleanup) MODE=cleanup ;;
        --dry-run) DRY=1 ;;
        --versions) VERSIONS=$2; shift ;;
        --manifest) MANIFEST=$2; shift ;;
        --runs) RUNS=$2; shift ;;
        --blocks) BLOCKS=$2; shift ;;
        --secs) SECS=$2; shift ;;
        --warmup) WARMUP=$2; shift ;;
        --gap) GAP=$2; shift ;;
        --settle) SETTLE=$2; shift ;;
        --out) OUT=$2; shift ;;
        --tag) TAG=$2; shift ;;
        --keep) KEEP=1 ;;
        -h | --help) usage; exit 0 ;;
        *) printf '%s: unknown argument: %s\n' "$PROG" "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

case $RUNS in '' | *[!0-9]*) die "--runs must be a positive integer" 2 ;; esac
case $BLOCKS in '' | *[!0-9]*) die "--blocks must be a positive integer" 2 ;; esac
case $SECS in '' | *[!0-9]*) die "--secs must be a positive integer" 2 ;; esac
[ "$RUNS" -ge 1 ] || die "--runs must be >= 1" 2
[ "$BLOCKS" -ge 1 ] || die "--blocks must be >= 1" 2
[ "$SECS" -ge 10 ] || die "--secs must be >= 10" 2
[ "$CARRIERS" = 256 ] || die "the 0.3.x SC-LDPC shape is fixed at 256 carriers" 2

# ---------------------------------------------------------------------------
# remote access

ssh_near() { timeout "$SSH_TMO" ssh -o BatchMode=yes "$NEAR_HOST" "$@"; }
ssh_far() { timeout "$SSH_TMO" ssh -o BatchMode=yes "$FAR_HOST" "$@"; }
ssh_end() { local e=$1; shift; if [ "$e" = far ]; then ssh_far "$@"; else ssh_near "$@"; fi; }
ssh_near_long() { timeout "$1" ssh -o BatchMode=yes "$NEAR_HOST" "$2"; }
ssh_far_long() { timeout "$1" ssh -o BatchMode=yes "$FAR_HOST" "$2"; }
ssh_end_long() { local t=$1 e=$2 c=$3; if [ "$e" = far ]; then ssh_far_long "$t" "$c"; else ssh_near_long "$t" "$c"; fi; }
scp_near() { timeout "$SSH_TMO" scp -q "$@"; }
scp_far() { timeout "$SSH_TMO" scp -q "$@"; }

# ---------------------------------------------------------------------------
# small helpers

num() { case ${1:-} in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }
nz() { if num "${1:-}"; then printf '%s' "$1"; else printf 0; fi; }
delta() {
    if num "${1:-}" && num "${2:-}"; then printf '%s' "$(( $2 - $1 ))"; else printf ''; fi
}
csv_safe() { printf '%s' "${1:-}" | tr -d ',\n"'; }
row_add() {
    if [ -z "${ROW:-}" ]; then ROW=$1; else ROW=$ROW,$1; fi
}
csv_nf() { printf '%s' "$1" | awk -F, '{ print NF }'; }

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

netdev_rx() { awk -v d="$2:" '$1 == d { print $2; exit }' "$1" 2>/dev/null; }
netdev_tx() { awk -v d="$2:" '$1 == d { print $10; exit }' "$1" 2>/dev/null; }

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
        else printf ""
    }'
}

snap_section() { # file name
    awk -v s="=== $2 ===" '$0 == s { getline; print; exit }' "$1" 2>/dev/null
}
snap_date() { snap_section "$1" date; }
snap_tz() { snap_section "$1" thermal; }
snap_freq() { snap_section "$1" freq; }
snap_load() { snap_section "$1" loadavg | awk '{print $1}'; }

thermal_max() {
    awk '{ for (i = 1; i <= NF; i++) if ($i + 0 > m) m = $i + 0 } END {
        if (m > 0) printf "%.1f", m / 1000; else printf ""
    }' <<<"${1:-}" 2>/dev/null
}

# ---------------------------------------------------------------------------
# manifest

manifest_line() {
    [ -f "$MANIFEST" ] || return 1
    awk -v v="$1" '!/^#/ && NF >= 3 && $1 == v { print; exit }' "$MANIFEST"
}

version_layout() {
    local line
    line=$(manifest_line "$1") || return 1
    printf '%s\n' "$line" | awk '{ print $2, $3 }'
}

# ---------------------------------------------------------------------------
# bypass rule (near -> far must leave on the physical NIC, not mihomo)

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

# ---------------------------------------------------------------------------
# snapshots

snapshot() { # near|far outfile
    local fn=ssh_near
    [ "$1" = far ] && fn=ssh_far
    "$fn" 'bash -s' >"$2" 2>&1 <<'EOS' || true
set -u
s() { printf '\n=== %s ===\n' "$1"; }
s date; date +%s
s loadavg; cat /proc/loadavg 2>/dev/null || true
s uptime; cut -d" " -f1 /proc/uptime 2>/dev/null || true
s clk; getconf CLK_TCK 2>/dev/null || echo 100
s procstat; head -1 /proc/stat 2>/dev/null || true
s netdev; cat /proc/net/dev 2>/dev/null || true
s stats; cat /proc/net/tbtun/stats 2>/dev/null || true
s thermal; for z in /sys/class/thermal/thermal_zone*/temp; do
    [ -r "$z" ] && cat "$z" 2>/dev/null && printf ' '
done; printf '\n'
s freq; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || true
s config; cat /proc/net/tbtun/config 2>/dev/null || true
s tunnels; cat /proc/net/tbtun/tunnels 2>/dev/null || true
s module; modinfo -F version tbtun 2>/dev/null; modinfo -F srcversion tbtun 2>/dev/null
s srcversion; cat /sys/module/tbtun/srcversion 2>/dev/null || true
s boot; cat /proc/sys/kernel/random/boot_id 2>/dev/null || true
EOS
}

# ---------------------------------------------------------------------------
# module identity and switching

ko_identity() { # end ko -> "srcversion sha256 version"
    ssh_end "$1" "src=\$(modinfo -F srcversion '$2' 2>/dev/null) || src=-
        sha=\$(sha256sum '$2' 2>/dev/null | awk '{print \$1}') || sha=-
        ver=\$(modinfo -F version '$2' 2>/dev/null) || ver=-
        printf '%s %s %s\n' \"\${src:--}\" \"\${sha:--}\" \"\${ver:--}\""
}

unload_end() { # near|far
    ssh_end "$1" "printf 'del $TUNNEL\n' > /proc/net/tbtun/config 2>/dev/null || true
          rmmod tbtun 2>/dev/null || true
          sleep 1
          lsmod | grep -q '^tbtun ' && echo LOADED || echo UNLOADED" 2>/dev/null
}

load_version() { # version
    local v=$1 layout near_ko far_ko src sha ver loaded end ko
    layout=$(version_layout "$v") || die "version $v is not in $MANIFEST" 3
    near_ko=$(printf '%s\n' "$layout" | awk '{print $1}')
    far_ko=$(printf '%s\n' "$layout" | awk '{print $2}')
    note "load $v: near=$near_ko far=$far_ko"

    unload_end near >/dev/null
    unload_end far >/dev/null

    ssh_far "insmod '$far_ko' max_carriers=$CARRIERS max_tunnels=1 debug=0" ||
        die "far insmod failed for $v ($far_ko)"
    ssh_near "insmod '$near_ko' max_carriers=$CARRIERS max_tunnels=1 debug=0" ||
        die "near insmod failed for $v ($near_ko)"
    sleep 1

    for end in near far; do
        ko=$near_ko
        [ "$end" = far ] && ko=$far_ko
        read -r src sha ver <<<"$(ko_identity "$end" "$ko")"
        loaded=$(ssh_end "$end" 'cat /sys/module/tbtun/srcversion 2>/dev/null' 2>/dev/null || true)
        [ -n "$loaded" ] || die "$end: loaded srcversion is empty after insmod"
        [ "$loaded" = "$src" ] ||
            die "$end: loaded srcversion $loaded != ko srcversion $src (wrong file loaded)"
        printf '%s version=%s srcversion=%s sha256=%s ko=%s\n' \
            "$end" "$ver" "$src" "$sha" "$ko" >>"$OUT/identity.txt"
    done

    ssh_near "printf 'ports 30000-30511\n' > /proc/net/tbtun/ports" ||
        die "near ports write failed"
    ssh_far "printf 'ports 31000-31511\n' > /proc/net/tbtun/ports" ||
        die "far ports write failed"

    ssh_far "cat > /proc/net/tbtun/config <<'EOF'
tunnel $TUNNEL role=far listen=0.0.0.0:$FAR_PORT target=127.0.0.1:$TARGET_PORT n=128 m=128 fec=on adaptive=off mode=sc-ldpc w=256 carriers=$CARRIERS crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=16777216 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31511 L=8 nonce=$NONCE
EOF" || die "far config write failed for $v"

    ssh_near "cat > /proc/net/tbtun/config <<'EOF'
tunnel $TUNNEL role=near listen=127.0.0.1:$NEAR_PORT peer=$FAR_IP:$FAR_PORT n=8 m=24 fec=on adaptive=off mode=sc-ldpc w=128 carriers=$CARRIERS crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=16777216 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30511 L=16 dev=$NEAR_DEV nonce=$NONCE
EOF" || die "near config write failed for $v"

    for end in near far; do
        cfg=$(ssh_end "$end" 'cat /proc/net/tbtun/config' 2>/dev/null || true)
        case " $cfg " in
            *" mode=sc-ldpc "*) ;;
            *) die "$end: config read-back lacks mode=sc-ldpc; build does not match the shape" ;;
        esac
        case " $cfg " in
            *" carriers=$CARRIERS "*) ;;
            *) die "$end: config read-back lacks carriers=$CARRIERS" ;;
        esac
        printf '%s config: %s\n' "$end" "$cfg" >>"$OUT/identity.txt"
    done
}

wait_join() {
    local i=0 nu nf
    while [ "$i" -lt "$JOIN_TIMEOUT" ]; do
        nu=$(ssh_near "awk '\$1==\"tunnel\" && \$2==$TUNNEL && \$3==\"c3\" {
            for (i=1;i<=NF;i++) if (\$i ~ /^up=/){ sub(/^up=/,\"\",\$i); print \$i; exit } }' /proc/net/tbtun/stats" 2>/dev/null || true)
        nf=$(ssh_far "awk '\$1==\"tunnel\" && \$2==$TUNNEL && \$3==\"c3\" {
            for (i=1;i<=NF;i++) if (\$i ~ /^up=/){ sub(/^up=/,\"\",\$i); print \$i; exit } }' /proc/net/tbtun/stats" 2>/dev/null || true)
        if [ "${nu:-0}" = "$CARRIERS" ] && [ "${nf:-0}" = "$CARRIERS" ]; then
            note "join ok: up=$CARRIERS on both ends after ${i}s"
            return 0
        fi
        [ $((i % 10)) -eq 0 ] && note "join: near=${nu:-?} far=${nf:-?} want=$CARRIERS (${i}s)"
        sleep 1
        i=$((i + 1))
    done
    note "join timeout: near=${nu:-?} far=${nf:-?}"
    return 1
}

wait_stream_clear() {
    local i=0 active
    while [ "$i" -lt "$SETTLE_TMO" ]; do
        active=$(ssh_near "awk '\$1==\"tunnel\" && \$2==$TUNNEL && \$3==\"streams\" {
            for (i=1;i<=NF;i++) if (\$i ~ /^streams_active=/){ sub(/^streams_active=/,\"\",\$i); print \$i; exit } }' /proc/net/tbtun/stats" 2>/dev/null || true)
        [ "${active:-1}" = 0 ] && return 0
        sleep 1
        i=$((i + 1))
    done
    note "stream still active after ${SETTLE_TMO}s; measuring anyway"
    return 0
}

# ---------------------------------------------------------------------------
# sampler and traffic

start_sampler() { # near|far log stop interval
    ssh_end "$1" "rm -f '$2' '$3'
          setsid sh '$SAMPLER' /proc/net/tbtun/stats '$2' '$3' '${4:-1}' \
              </dev/null >/dev/null 2>&1 &
          echo started" >/dev/null 2>&1
}

stop_sampler() { # near|far log stop outfile
    ssh_end "$1" "touch '$3'" >/dev/null 2>&1 || true
    sleep 1
    ssh_end "$1" "cat '$2' 2>/dev/null" >"$4" 2>/dev/null || true
}

start_curl() { # url timeout pidfile outfile
    ssh_near "setsid sh '$CURLER' '$1' '$3' '$4' '$2' \
        </dev/null >/dev/null 2>&1 &
        echo started" >/dev/null 2>&1
}

curl_pid() { ssh_near "cat '$1' 2>/dev/null" 2>/dev/null | tr -d '[:space:]'; }
pid_alive() { # pid
    [ -n "$1" ] || return 1
    ssh_near "kill -0 '$1' 2>/dev/null && echo alive || echo dead" 2>/dev/null | grep -q alive
}
wchar_of() { # pid
    [ -n "$1" ] || { printf ''; return 0; }
    ssh_near "awk '/^wchar:/{print \$2; exit}' /proc/$1/io 2>/dev/null" 2>/dev/null | tr -d '[:space:]'
}
kill_curl() { # pid
    [ -n "$1" ] || return 0
    ssh_near "kill -TERM '$1' 2>/dev/null || true
        i=0; while [ \$i -lt 10 ] && kill -0 '$1' 2>/dev/null; do sleep 1; i=\$((i+1)); done
        kill -KILL '$1' 2>/dev/null || true" >/dev/null 2>&1 || true
}

# An error must not leave a sampler writing forever or a curl running.
cleanup_traffic() {
    [ -n "${CUR_PID:-}" ] && ssh_near "kill -KILL '$CUR_PID' 2>/dev/null || true" >/dev/null 2>&1 || true
    [ -n "${NEAR_STOP:-}" ] && ssh_near "touch '$NEAR_STOP'" >/dev/null 2>&1 || true
    [ -n "${FAR_STOP:-}" ] && ssh_far "touch '$FAR_STOP'" >/dev/null 2>&1 || true
    CUR_PID= NEAR_STOP= FAR_STOP=
}

sampler_min() { # file a b column
    [ -f "$1" ] || return 1
    awk -v a="$2" -v b="$3" -v c="$4" '
        NR <= 5 { next }
        $1 ~ /^[0-9]+$/ && $1 >= a && $1 <= b && $c != "-" {
            if (min == "" || $c + 0 < min) min = $c + 0
        }
        END { if (min != "") print min }' "$1" 2>/dev/null
}

# ---------------------------------------------------------------------------
# one 60 s window

run_window() { # version block order run
    local v=$1 blk=$2 ord=$3 run=$4
    local dir rdir pid alive wa wb app_src app wchar wire
    local cpa cpb cfa cfb n2 n3 f2 f3
    local t_a t_b window f_t_a f_t_b far_window
    local a_app b_app a_w b_w
    local n_cpu f_cpu n_load f_load n_temp f_temp n_freq f_freq
    local n_up_min f_up_min n_up_end f_up_end
    local c3_lost c3_repl c3_sd c3_sr c3_sdead c6_holes c6_stall
    local nack_tx nack_rx nack_drop replay_miss drop_sid refused
    local retx retx2 c3_retx rtt_n rtt_f ack_tx ack_rx
    local stream_reset valid noisy note_txt

    dir=$(printf '%s/runs/b%d-o%d-%s-r%d' "$OUT" "$blk" "$ord" "$v" "$run")
    rdir=/tmp/perf-ab/b${blk}-o${ord}-${v}-r${run}
    mkdir -p "$dir"
    ssh_near "mkdir -p '$rdir'" >/dev/null 2>&1 || die "cannot create $rdir on near"
    ssh_far "mkdir -p '$rdir'" >/dev/null 2>&1 || die "cannot create $rdir on far"
    note "run $v block=$blk order=$ord run=$run"

    wait_stream_clear
    NEAR_STOP=$rdir/near.stop
    FAR_STOP=$rdir/far.stop
    start_sampler near "$rdir/near.sampler" "$rdir/near.stop" 1
    start_sampler far "$rdir/far.sampler" "$rdir/far.stop" 1
    sleep 1

    start_curl "http://127.0.0.1:$NEAR_PORT/" "$((WARMUP + SECS + 60))" \
        "$rdir/curl.pid" "$rdir/curl.out"
    sleep 2
    pid=$(curl_pid "$rdir/curl.pid")
    CUR_PID=$pid
    alive=0
    pid_alive "$pid" && alive=1
    [ "$alive" = 1 ] || note "run $v: curl did not start; row will be invalid"

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
    kill_curl "$pid"

    stop_sampler near "$rdir/near.sampler" "$rdir/near.stop" "$dir/near.sampler"
    stop_sampler far "$rdir/far.sampler" "$rdir/far.stop" "$dir/far.sampler"
    ssh_near "cat '$rdir/curl.out' 2>/dev/null" >"$dir/curl.out" 2>/dev/null || true
    ssh_near "cat '$rdir/curl.pid' 2>/dev/null" >"$dir/curl.pid" 2>/dev/null || true
    CUR_PID= NEAR_STOP= FAR_STOP=

    t_a=$(snap_date "$dir/near-a.txt")
    t_b=$(snap_date "$dir/near-b.txt")
    window=$(delta "$t_a" "$t_b")
    case ${window:-} in '' | *[!0-9]*) window=0 ;; esac
    f_t_a=$(snap_date "$dir/far-a.txt")
    f_t_b=$(snap_date "$dir/far-b.txt")
    far_window=$(delta "$f_t_a" "$f_t_b")
    case ${far_window:-} in '' | *[!0-9]*) far_window=$window ;; esac

    a_app=$(cap_field "$dir/far-a.txt" submit_bytes)
    b_app=$(cap_field "$dir/far-b.txt" submit_bytes)
    app=0
    app_src=missing
    if num "${a_app:-}" && num "${b_app:-}"; then
        app=$(delta "$a_app" "$b_app")
        app_src=submit_bytes
    fi
    case ${app:-} in '' | *[!0-9]*) app=0 ;; esac

    wchar=$(delta "${wa:-}" "${wb:-}")
    case ${wchar:-} in '' | *[!0-9]*) wchar=0 ;; esac

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

    n_up_end=$(nz "$(stat_field "$dir/near-b.txt" c3 up)")
    f_up_end=$(nz "$(stat_field "$dir/far-b.txt" c3 up)")
    n_up_min=$(sampler_min "$dir/near.sampler" "$t_a" "$t_b" 2)
    f_up_min=$(sampler_min "$dir/far.sampler" "$f_t_a" "$f_t_b" 2)
    n_up_min=${n_up_min:-$n_up_end}
    f_up_min=${f_up_min:-$f_up_end}

    c3_lost=$(nz "$(delta "$(stat_field "$dir/near-a.txt" c3 lost)" "$(stat_field "$dir/near-b.txt" c3 lost)")")
    c3_repl=$(nz "$(delta "$(stat_field "$dir/near-a.txt" c3 replaced)" "$(stat_field "$dir/near-b.txt" c3 replaced)")")
    c3_sd=$(nz "$(delta "$(stat_field "$dir/near-a.txt" c3 stall_declared)" "$(stat_field "$dir/near-b.txt" c3 stall_declared)")")
    c3_sr=$(nz "$(delta "$(stat_field "$dir/near-a.txt" c3 stall_recovered)" "$(stat_field "$dir/near-b.txt" c3 stall_recovered)")")
    c3_sdead=$(nz "$(delta "$(stat_field "$dir/near-a.txt" c3 stall_dead)" "$(stat_field "$dir/near-b.txt" c3 stall_dead)")")
    c6_holes=$(nz "$(delta "$(stat_field "$dir/near-a.txt" c6 holes_unrecoverable)" "$(stat_field "$dir/near-b.txt" c6 holes_unrecoverable)")")
    c6_stall=$(stat_field "$dir/near-b.txt" c6 rx_max_stall_us)
    nack_tx=$(delta "$(stat_field "$dir/near-a.txt" c11 fec_nack_tx)" "$(stat_field "$dir/near-b.txt" c11 fec_nack_tx)")
    nack_rx=$(delta "$(stat_field "$dir/near-a.txt" c11 fec_nack_rx)" "$(stat_field "$dir/near-b.txt" c11 fec_nack_rx)")
    nack_drop=$(delta "$(stat_field "$dir/near-a.txt" c11 fec_nack_drop)" "$(stat_field "$dir/near-b.txt" c11 fec_nack_drop)")
    replay_miss=$(delta "$(stat_field "$dir/near-a.txt" c11 fec_replay_miss)" "$(stat_field "$dir/near-b.txt" c11 fec_replay_miss)")
    drop_sid=$(delta "$(stat_field "$dir/near-a.txt" c9 fec_drop_sid)" "$(stat_field "$dir/near-b.txt" c9 fec_drop_sid)")
    refused=$(delta "$(stat_field "$dir/near-a.txt" c9 fec_stream_refused)" "$(stat_field "$dir/near-b.txt" c9 fec_stream_refused)")
    stream_reset=$(delta "$(stat_field "$dir/near-a.txt" c3 stream_reset_tcp_failed)" "$(stat_field "$dir/near-b.txt" c3 stream_reset_tcp_failed)")
    retx=$(delta "$(stat_field "$dir/far-a.txt" c11 fec_nack_retx_bytes)" "$(stat_field "$dir/far-b.txt" c11 fec_nack_retx_bytes)")
    retx2=$(delta "$(stat_field "$dir/far-a.txt" c11 fec_replace_retx_bytes)" "$(stat_field "$dir/far-b.txt" c11 fec_replace_retx_bytes)")
    if num "${retx:-}" && num "${retx2:-}"; then retx=$((retx + retx2)); else retx=; fi
    c3_retx=$(delta "$(stat_field "$dir/far-a.txt" c3 retx_bytes)" "$(stat_field "$dir/far-b.txt" c3 retx_bytes)")
    ack_tx=$(delta "$(stat_field "$dir/near-a.txt" c9 fec_ack_tx)" "$(stat_field "$dir/near-b.txt" c9 fec_ack_tx)")
    ack_rx=$(delta "$(stat_field "$dir/far-a.txt" c9 fec_ack_rx)" "$(stat_field "$dir/far-b.txt" c9 fec_ack_rx)")
    rtt_n=$(stat_field "$dir/near-b.txt" c3 rtt_mean_us)
    rtt_f=$(stat_field "$dir/far-b.txt" c3 rtt_mean_us)

    valid=1
    [ "$window" -ge $((SECS - 2)) ] && [ "$window" -le $((SECS + 5)) ] || valid=0
    [ "$app" -gt 0 ] && [ "$wchar" -gt 0 ] || valid=0
    [ "$alive" = 1 ] || valid=0

    noisy=0
    note_txt=
    if [ "$n_up_min" != "-" ] && [ "$n_up_min" -lt "$CARRIERS" ]; then noisy=1; note_txt="${note_txt:+$note_txt+}up_near"; fi
    if [ "$f_up_min" != "-" ] && [ "$f_up_min" -lt "$CARRIERS" ]; then noisy=1; note_txt="${note_txt:+$note_txt+}up_far"; fi
    [ "$c3_lost" -gt 0 ] && { noisy=1; note_txt="${note_txt:+$note_txt+}lost"; }
    [ "$c3_repl" -gt 0 ] && { noisy=1; note_txt="${note_txt:+$note_txt+}replaced"; }
    [ "$c3_sd" -gt 0 ] && { noisy=1; note_txt="${note_txt:+$note_txt+}stall"; }
    [ "$c6_holes" -gt 0 ] && { noisy=1; note_txt="${note_txt:+$note_txt+}holes"; }
    [ "$(nz "${stream_reset:-}")" -gt 0 ] && { noisy=1; note_txt="${note_txt:+$note_txt+}reset"; }
    [ "$(nz "${drop_sid:-}")" -gt 0 ] && { noisy=1; note_txt="${note_txt:+$note_txt+}drop_sid"; }
    if [ "$app" -gt 0 ]; then
        awk -v a="$app" -v w="$wchar" 'BEGIN{exit !(((a>w?a-w:w-a)/a) > 0.05)}' &&
            { noisy=1; note_txt="${note_txt:+$note_txt+}app_wchar"; }
    fi
    [ "$app_src" = submit_bytes ] || { noisy=1; note_txt="${note_txt:+$note_txt+}app_missing"; }
    [ "$valid" = 1 ] || note_txt="${note_txt:+$note_txt+}invalid"

    app_mbps=$(awk -v b="$app" -v s="$far_window" 'BEGIN{ if(s>0) printf "%.3f", b*8/s/1e6; else printf "0" }')
    wchar_mbps=$(awk -v b="$wchar" -v s="$window" 'BEGIN{ if(s>0) printf "%.3f", b*8/s/1e6; else printf "0" }')
    wire_mbps=$(awk -v b="$wire" -v s="$far_window" 'BEGIN{ if(s>0) printf "%.3f", b*8/s/1e6; else printf "0" }')
    wire_over=$(awk -v w="$wire" -v a="$app" 'BEGIN{ if(a>0) printf "%.4f", w/a; else printf "0" }')

    {
        printf 'window_s=%s far_window_s=%s\n' "$window" "$far_window"
        printf 'app_bytes=%s app_source=%s wchar_bytes=%s\n' "$app" "$app_src" "$wchar"
        printf 'app_mbps=%s wchar_mbps=%s wire_mbps=%s wire_over_app=%s\n' \
            "$app_mbps" "$wchar_mbps" "$wire_mbps" "$wire_over"
        printf 'ack_tx=%s ack_rx=%s retx=%s c3_retx=%s\n' \
            "${ack_tx:-?}" "${ack_rx:-?}" "${retx:-?}" "${c3_retx:-?}"
        printf 'cpu_near=%s cpu_far=%s up_min=%s/%s up_end=%s/%s\n' \
            "${n_cpu:-?}" "${f_cpu:-?}" "${n_up_min:-?}" "${f_up_min:-?}" \
            "${n_up_end:-?}" "${f_up_end:-?}"
        printf 'valid=%s noisy=%s note=%s\n' "$valid" "$noisy" "${note_txt:--}"
    } >"$dir/result.txt"

    ROW=
    row_add "$(date -u +%Y%m%dT%H%M%SZ)"
    row_add ab
    row_add "$blk"
    row_add "$ord"
    row_add "$v"
    row_add "$run"
    row_add "$window"
    row_add "$WARMUP"
    row_add "$app"
    row_add "$app_src"
    row_add "$wchar"
    row_add 1
    row_add "$alive"
    row_add "$app_mbps"
    row_add "$wchar_mbps"
    row_add "$wire_mbps"
    row_add "$wire_over"
    row_add "${ack_tx:-}"
    row_add "${ack_rx:-}"
    row_add "${nack_tx:-}"
    row_add "${nack_rx:-}"
    row_add "${retx:-}"
    row_add "${c3_retx:-}"
    row_add "${n_cpu:-}"
    row_add "${f_cpu:-}"
    row_add "${n_load:-}"
    row_add "${f_load:-}"
    row_add "${n_temp:-}"
    row_add "${f_temp:-}"
    row_add "${n_freq:-}"
    row_add "${f_freq:-}"
    row_add "${rtt_n:-}"
    row_add "${rtt_f:-}"
    row_add "${n_up_min:-}"
    row_add "${f_up_min:-}"
    row_add "${n_up_end:-}"
    row_add "${f_up_end:-}"
    row_add "$c3_lost"
    row_add "$c3_repl"
    row_add "$c3_sd"
    row_add "$c3_sr"
    row_add "$c3_sdead"
    row_add "$c6_holes"
    row_add "${c6_stall:-}"
    row_add "${nack_drop:-}"
    row_add "${replay_miss:-}"
    row_add "${drop_sid:-}"
    row_add "${refused:-}"
    row_add "$valid"
    row_add "$noisy"
    row_add 0
    row_add "${note_txt:--}"

    nf=$(csv_nf "$ROW")
    [ "$nf" = "$CSV_NF" ] || die "internal: row has $nf fields, expected $CSV_NF" 6
    printf '%s\n' "$ROW" >>"$OUT/runs.csv"
    note "run $v: app=${app_mbps} Mbit/s wchar=${wchar_mbps} wire=${wire_mbps} valid=$valid noisy=$noisy"
    sleep "$GAP"
}

# ---------------------------------------------------------------------------
# preflight, status, cleanup

preflight() {
    local v line near_ko far_ko src_size code length route dev far_dev rule
    say "== preflight"
    ssh_near true >/dev/null 2>&1 || die "near unreachable: $NEAR_HOST" 3
    ssh_far true >/dev/null 2>&1 || die "far unreachable: $FAR_HOST" 3

    for h in "$NEAR_HOST" "$FAR_HOST"; do
        ssh -o BatchMode=yes -o ConnectTimeout="$SSH_TMO" "$h" \
            'test "$(id -u)" = 0 && command -v curl >/dev/null && command -v awk >/dev/null &&
             command -v sha256sum >/dev/null && command -v modinfo >/dev/null &&
             command -v insmod >/dev/null && command -v rmmod >/dev/null' ||
            die "$h: need root plus curl awk sha256sum modinfo insmod rmmod" 3
    done

    [ -f "$MANIFEST" ] || die "manifest missing: $MANIFEST" 3
    for v in $VERSIONS; do
        line=$(manifest_line "$v") || die "version $v is not in $MANIFEST" 3
        near_ko=$(printf '%s\n' "$line" | awk '{print $2}')
        far_ko=$(printf '%s\n' "$line" | awk '{print $3}')
        ssh_near "test -f '$near_ko'" || die "near module missing: $near_ko" 3
        ssh_far "test -f '$far_ko'" || die "far module missing: $far_ko" 3
        say "   $v near=$(ko_identity near "$near_ko") far=$(ko_identity far "$far_ko")"
    done

    src_size=$(ssh_far "stat -c %s '$SOURCE' 2>/dev/null || echo 0" 2>/dev/null || echo 0)
    [ "${src_size:-0}" -gt 0 ] || die "far source missing: $SOURCE" 3
    code=$(ssh_far "curl -sS -o /dev/null -w '%{http_code}' -I -m 5 \
        'http://127.0.0.1:$TARGET_PORT/' 2>/dev/null" || true)
    [ "$code" = 200 ] ||
        die "far target 127.0.0.1:$TARGET_PORT is not answering HEAD 200 (got ${code:-none}); run far-serve.sh --start" 3
    length=$(ssh_far "curl -sSI -m 5 'http://127.0.0.1:$TARGET_PORT/' 2>/dev/null |
        awk 'BEGIN{IGNORECASE=1} /^Content-Length:/{gsub(/\r/,\"\"); print \$2; exit}'" 2>/dev/null || true)
    [ "${length:-0}" = "$src_size" ] ||
        die "far target Content-Length=${length:-none} != $SOURCE size $src_size" 3

    rule=$(ssh_near "ip rule show 2>/dev/null" 2>/dev/null |
        awk -v p="$BYPASS_PREF:" -v ip="$FAR_IP" '
            $1 == p { for (i = 2; i < NF; i++)
                if ($i == "to" && ($(i+1) == ip || $(i+1) == ip "/32")) f = 1 }
            END { print(f ? "present" : "absent") }')
    route=$(ssh_near "ip route get $FAR_IP 2>/dev/null" 2>/dev/null || true)
    dev=$(printf '%s\n' "$route" | awk '{ for (i=1;i<NF;i++) if ($i=="dev") { print $(i+1); exit } }')
    far_dev=$(ssh_far "ip route get $NEAR_IP 2>/dev/null" 2>/dev/null |
        awk '{ for (i=1;i<NF;i++) if ($i=="dev") { print $(i+1); exit } }')
    if [ -n "$far_dev" ] && [ "$far_dev" != "$FAR_DEV" ]; then
        die "far route to $NEAR_IP is dev=$far_dev, want $FAR_DEV (set FAR_DEV if the NIC differs)" 3
    fi
    if [ "$rule" = present ] && [ "$dev" != "$NEAR_DEV" ]; then
        die "near pref $BYPASS_PREF rule exists but route dev=$dev, want $NEAR_DEV" 3
    fi
    say "   source $SOURCE ($src_size bytes), target HEAD 200 length=$length"
    say "   near dev=$dev far dev=$FAR_DEV  bypass rule=$rule"
    [ "$rule" = present ] || say "   bypass rule absent: --run will add pref $BYPASS_PREF"
    say "   versions: $VERSIONS  runs=$RUNS blocks=$BLOCKS secs=$SECS warmup=$WARMUP"
    say "   manifest: $MANIFEST"
}

status_mode() {
    for h in "$NEAR_HOST" "$FAR_HOST"; do
        say "== $h"
        ssh -o BatchMode=yes -o ConnectTimeout="$SSH_TMO" "$h" \
            'modinfo -F version tbtun 2>/dev/null; modinfo -F srcversion tbtun 2>/dev/null
             echo "--- config"; cat /proc/net/tbtun/config 2>/dev/null
             echo "--- c3"; awk "\$1==\"tunnel\"&&\$2==1&&\$3==\"c3\"" /proc/net/tbtun/stats 2>/dev/null
             echo "--- c6/c9/c10/c11"; awk "\$1==\"tunnel\"&&\$2==1&&(\$3==\"c6\"||\$3==\"c9\"||\$3==\"c10\"||\$3==\"c11\")" /proc/net/tbtun/stats 2>/dev/null' || true
    done
}

cleanup_mode() {
    say "== cleanup"
    for end in near far; do
        printf '%s: %s\n' "$end" "$(unload_end "$end")"
    done
    bypass_cleanup
    ssh_near "rm -rf '$REMOTE_DIR' /tmp/perf-ab" >/dev/null 2>&1 || true
    ssh_far "rm -rf /tmp/perf-ab" >/dev/null 2>&1 || true
    say "cleanup done (far 1 GiB source and watchdog untouched)"
}

# ---------------------------------------------------------------------------
# plan

plan() {
    local b order
    say "dry-run: $PROG"
    say "  near=$NEAR_HOST ($NEAR_IP) far=$FAR_HOST ($FAR_IP)"
    say "  shape: mode=sc-ldpc carriers=$CARRIERS near 8x32/L16 far 1x256/L8"
    say "  source: $SOURCE -> far 127.0.0.1:$TARGET_PORT; near curl 127.0.0.1:$NEAR_PORT"
    say "  versions: $VERSIONS"
    say "  repetitions: $RUNS x ${SECS}s + ${WARMUP}s warm-up per session, $BLOCKS block(s)"
    say "  settle after join: ${SETTLE}s"
    say "  block order:"
    for ((b = 1; b <= BLOCKS; b++)); do
        if [ $((b % 2)) -eq 1 ]; then order=$VERSIONS; else order=$(printf '%s\n' $VERSIONS | tac | tr '\n' ' '); fi
        say "    block $b: $order"
    done
    say "  per run: snapshot A -> ${SECS}s -> snapshot B; app=submit_bytes, wchar=/proc/PID/io"
    say "  output: ${OUT:-<script>/results/<UTC>-ab}"
    say "  load: del 1; rmmod; insmod <ko> max_carriers=$CARRIERS max_tunnels=1 debug=0"
    say "  stats: perf-stats.py runs.csv -> median, range, 5% verdict"
    say "  no ssh, no writes (dry-run)"
}

# ---------------------------------------------------------------------------
# main

if [ "$DRY" = 1 ]; then plan; exit 0; fi

case $MODE in
    preflight) preflight; exit 0 ;;
    status) status_mode; exit 0 ;;
    cleanup) cleanup_mode; exit 0 ;;
    run) ;;
esac

OUT=${OUT:-$SCRIPT_DIR/results/$(date -u +%Y%m%dT%H%M%SZ)-ab${TAG:+-$TAG}}
mkdir -p "$OUT/runs"
: >"$OUT/run.log"
printf '%s\n' "$CSV_HEADER" >"$OUT/runs.csv"
printf 'start=%s\nhost_near=%s\nhost_far=%s\nversions=%s\nruns=%s\nblocks=%s\nsecs=%s\nwarmup=%s\nsettle=%s\n' \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$NEAR_HOST" "$FAR_HOST" "$VERSIONS" "$RUNS" "$BLOCKS" "$SECS" "$WARMUP" "$SETTLE" \
    >"$OUT/meta.txt"
git -C "$SCRIPT_DIR" rev-parse HEAD >>"$OUT/meta.txt" 2>/dev/null || true
sha256sum "$0" >>"$OUT/meta.txt" 2>/dev/null || true

trap cleanup_traffic EXIT
preflight || exit 3
bypass_out=$(bypass_ensure) || {
    printf '%s\n' "$bypass_out" >&2
    die "near bypass failed: carriers would leave through the wrong device" 3
}
printf '%s\n' "$bypass_out" >>"$OUT/meta.txt"

ssh_near "mkdir -p '$REMOTE_DIR'" >/dev/null 2>&1 || die "cannot create $REMOTE_DIR on near" 4
ssh_far "mkdir -p '$REMOTE_DIR'" >/dev/null 2>&1 || die "cannot create $REMOTE_DIR on far" 4
scp_near "$SCRIPT_DIR/perf-sampler.sh" "$NEAR_HOST:$REMOTE_DIR/perf-sampler.sh" ||
    die "cannot upload sampler to near" 4
scp_near "$SCRIPT_DIR/perf-curl.sh" "$NEAR_HOST:$REMOTE_DIR/perf-curl.sh" ||
    die "cannot upload curl helper to near" 4
scp_far "$SCRIPT_DIR/perf-sampler.sh" "$FAR_HOST:$REMOTE_DIR/perf-sampler.sh" ||
    die "cannot upload sampler to far" 4

order_no=0
for ((blk = 1; blk <= BLOCKS; blk++)); do
    if [ $((blk % 2)) -eq 1 ]; then
        block_versions=$VERSIONS
    else
        block_versions=$(printf '%s\n' $VERSIONS | tac | tr '\n' ' ')
    fi
    for v in $block_versions; do
        order_no=$((order_no + 1))
        load_version "$v" || exit 4
        wait_join || exit 5
        note "settle ${SETTLE}s after join"
        sleep "$SETTLE"
        wait_stream_clear
        for ((r = 1; r <= RUNS; r++)); do
            run_window "$v" "$blk" "$order_no" "$r"
        done
    done
done

if [ "$KEEP" = 1 ]; then
    note "keep=1: modules and config left loaded"
else
    for end in near far; do unload_end "$end" >/dev/null; done
    bypass_cleanup
fi
ssh_near "rm -rf /tmp/perf-ab" >/dev/null 2>&1 || true
ssh_far "rm -rf /tmp/perf-ab" >/dev/null 2>&1 || true
printf 'end=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$OUT/meta.txt"

if [ -x "$SCRIPT_DIR/perf-stats.py" ]; then
    "$SCRIPT_DIR/perf-stats.py" "$OUT/runs.csv" --out "$OUT/analysis" >/dev/null 2>&1 &&
        note "analysis: $OUT/analysis/summary.md"
fi
note "done: $OUT/runs.csv"
say "$OUT"
exit 0
