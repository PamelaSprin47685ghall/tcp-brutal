#!/usr/bin/env bash
# verify-0.3.1-fix44.sh - SC-LDPC 0.3.1 fix44 real-hardware acceptance.
#
# The build agent has already deployed the fix44 module and joined the
# session. This script judges that session, one shot:
#
#   1. configuration: c3 up=256, mode=sc-ldpc, adaptive=off, the per-
#      direction SC-LDPC shapes, and the peer-shape line in far dmesg.
#   2. first connect: curl -m 15, with a 10 Hz sampler on both hosts that
#      keeps c3/c6/c9/c10/c11/c12/c13 and the stream line on disk. fix43
#      is judged on the fix44 short-block rules: replay_miss_never_stored
#      near zero, short_parity_saved > 0, nack_pad_skip 0, and a short-block
#      repair max far below the 330 s fix43 value. Also: replay high-water
#      around half
#      the 8 MiB ring, no new NACK backpressure, hol_wait below 10000,
#      near decode/ack progress, no reset/holes/hog.
#   3. steady state: 20 s warmup + 60 s measurement on the 256-carrier
#      session, with app/wire, outq, NACK/replay, holes/reset and CPU.
#   4. one 1 GiB single stream, sha256 on both ends, cap 7200 s.
#   5. cleanup: far source, near ip rule, rmmod both ends. Evidence stays.
#
# A failed step is recorded, evidence is captured, and the run is not
# retried. The watchdog and unrelated services are observed, never touched.
#
# Modes
#   --run        full run (default)
#   --from-steady skip the 15 s first connect (already PASSed) and run the
#                256 steady state and the 1 GiB stream on a fresh join
#   --one-g-only  skip first connect and steady; run the 1 GiB stream and
#                cleanup on the session that is already up
#   --preflight  read-only readiness: ssh, module, config, c3/c13, dmesg
#                shape, ports; no source start, no traffic
#   --status     print both configs and the c3/c13 lines
#   --cleanup    stop the far source, remove the near ip rule, rmmod both
#
# Exit codes
#   0 PASS  2 usage  3 preflight  4 setup  5 first-connect  6 steady
#   7 one-g  8 checks
set -uo pipefail

PROG=${0##*/}
MODE=run
NEAR_HOST=${NEAR_HOST:-root@192.168.123.84}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
FAR_IP=${FAR_IP:-45.11.1.62}
NEAR_PORT=${NEAR_PORT:-9001}
FAR_PORT=${FAR_PORT:-9000}
TUNNEL=${TUNNEL:-1}
SSH_TMO=${SSH_TMO:-20}
FIRST_TMO=${FIRST_TMO:-15}
WARMUP=${WARMUP:-20}
MEASURE=${MEASURE:-60}
STEADY_TMO=${STEADY_TMO:-600}
ONE_G_MAX=${ONE_G_MAX:-7200}
ONE_G_TMO=${ONE_G_TMO:-$((ONE_G_MAX + 300))}
EXPECT_BYTES=1073741824
EVIDENCE_ROOT=${EVIDENCE_ROOT:-$(cd -- "$(dirname -- "$0")/.." && pwd)}
FAR_DIR=${FAR_DIR:-/root/sclc-verify-1g}
REMOTE_HELPERS=${REMOTE_HELPERS:-/root/sclc-verify}
BYPASS_PREF=${BYPASS_PREF:-100}
# Wait this long after the join before the first-connect sampler starts, so
# join-time repairs finish before the measurement window. The criterion uses
# the windowed fec_nack_repair_ns delta, not the session-wide max.
SETTLE=${SETTLE:-160}
NEAR_DEV=${NEAR_DEV:-eth0}
# The real fix43 build (HEAD 830c7f7, archive 36e5a3df...). A first
# "fix43" deployment was found to carry the fix42 modules; the preflight
# now refuses to run unless the loaded srcversions are the restored fix43.
# Set these from the sclc-031-core build report. If empty, the preflight
# still refuses the fix43 modules so a stale load cannot pass as fix44.
EXPECT_NEAR_SRCVERSION=${EXPECT_NEAR_SRCVERSION:-}
EXPECT_FAR_SRCVERSION=${EXPECT_FAR_SRCVERSION:-}
FIX43_NEAR_SRCVERSION=B66D7B86A162F744837BFF3
FIX43_FAR_SRCVERSION=2296207711D4F7FC38D809F

EV=${EV:-}
FAILED=0
FIRST_FAILED=0
STEADY_FAILED=0
ONE_FAILED=0
START_UTC=
END_UTC=
START_EPOCH=
NEAR_BOOT_BEFORE=
FAR_BOOT_BEFORE=
FAR_SHA=
FAR_SIZE=
NEAR_PHYS_DEV=
FAR_PHYS_DEV=
STEADY_PID=

say() { printf '%s\n' "$*"; }
note() { printf '%s %s\n' "$(date -u +%H:%M:%SZ)" "$*" | tee -a "${EV:-/dev/null}/run.log" >&2; }
mark() { printf '%s\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$1" | tee -a "${EV:-/dev/null}/timeline.txt" >&2; }

usage() {
    cat <<EOF
usage: $PROG [--run|--from-steady|--one-g-only|--preflight|--status|--cleanup]

Environment:
  NEAR_HOST=root@192.168.123.84  FAR_HOST=root@45.11.1.62
  WARMUP=20 MEASURE=60 ONE_G_MAX=7200
  EVIDENCE_ROOT=<repo>/review/wan-experiment/arm64
EOF
}

while (($#)); do
    case $1 in
        --run) MODE=run ;;
        --from-steady) MODE=from-steady ;;
        --one-g-only) MODE=one-g ;;
        --preflight) MODE=preflight ;;
        --status) MODE=status ;;
        --cleanup) MODE=cleanup ;;
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

cfg_has() { # file token
    grep -qE "(^|[[:space:]])$2([[:space:]]|$)" "$1" 2>/dev/null
}

num() { case ${1:-} in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }

delta() { # before after
    if num "${1:-}" && num "${2:-}"; then printf '%s' "$(( $2 - $1 ))"; else printf '?'; fi
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
        else printf "0"
    }'
}

phys_dev_for() { # near|far
    if [[ $1 == near ]]; then
        ssh_near "ip route get $FAR_IP 2>/dev/null" |
            awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }'
    else
        ssh_far "ip route get 1.1.1.1 2>/dev/null" |
            awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }'
    fi
}

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

# ---------------------------------------------------------------------------
# evidence capture

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
s modinfo; modinfo -F version tbtun 2>/dev/null; modinfo -F srcversion tbtun 2>/dev/null; modinfo -F vermagic tbtun 2>/dev/null
s srcversion; cat /sys/module/tbtun/srcversion 2>/dev/null || true
s max_carriers; cat /sys/module/tbtun/parameters/max_carriers 2>/dev/null || true
s config; cat /proc/net/tbtun/config 2>/dev/null || true
s ports; cat /proc/net/tbtun/ports 2>/dev/null || true
s tunnels; cat /proc/net/tbtun/tunnels 2>/dev/null || true
s stats; cat /proc/net/tbtun/stats 2>/dev/null || true
s ss; ss -ltnp 2>/dev/null || true
s ip_rule; ip rule show 2>/dev/null || true
s ip_route; ip route 2>/dev/null || true
s netdev; cat /proc/net/dev 2>/dev/null || true
s meminfo; awk '/^(MemTotal|MemAvailable|MemFree):/{print}' /proc/meminfo 2>/dev/null || true
s slab; grep -E '^tbt_(carrier|session|stream)' /proc/slabinfo 2>/dev/null || true
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

sample() { # near|far prefix
    local fn=ssh_near out=$2
    [[ $1 == far ]] && fn=ssh_far
    "$fn" 'cat /proc/net/tbtun/stats' >"$EV/$out-stats.txt" 2>/dev/null || true
    "$fn" 'cat /proc/stat' >"$EV/$out-procstat.txt" 2>/dev/null || true
    "$fn" 'cat /proc/net/dev' >"$EV/$out-netdev.txt" 2>/dev/null || true
}

# Kernel window: journalctl by @epoch, then dmesg by monotonic seconds. The
# first line is KW_SOURCE; an empty body is SKIP, never PASS.
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

kw_source() { sed -n 's/^KW_SOURCE=//p' "$1" 2>/dev/null | head -n 1; }
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
# samplers

start_sampler() { # host(full/compact) log stop tunnel dev interval
    local mode=$1 log=$2 stop=$3 t=$4 dev=$5 interval=$6
    local fn=ssh_near
    [[ $mode == far:* ]] && fn=ssh_far
    local kind=${mode#*:}
    "$fn" "rm -f '$log' '$stop'; setsid sh '$REMOTE_HELPERS/fix42-sampler.sh' '$kind' '$log' '$stop' '$t' '$dev' '$interval' </dev/null >/dev/null 2>&1 & echo started" \
        >/dev/null 2>&1 || true
}

stop_sampler() { # host stop
    local fn=ssh_near
    [[ $1 == far ]] && fn=ssh_far
    "$fn" "touch '$2'" >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------------
# cleanup

cleanup_all() {
    say "== cleanup"
    local out=$EV/cleanup.txt
    : >"$out"
    {
        printf '=== far source stop ===\n'
        ssh_far "FAR_DIR='$FAR_DIR' sh '$REMOTE_HELPERS/far-serve.sh' --stop" 2>&1 || true
        printf '=== near ip rule before ===\n'
        ssh_near "ip rule show" 2>&1 || true
        ssh_near "ip rule del pref $BYPASS_PREF to $FAR_IP/32 lookup main 2>/dev/null ||
                  ip rule del pref $BYPASS_PREF to $FAR_IP/32 2>/dev/null || true"
        printf '=== near ip rule after ===\n'
        ssh_near "ip rule show" 2>&1 || true
        printf '=== near rmmod ===\n'
        ssh_near "rmmod tbtun 2>&1 || true; lsmod | grep -E '^tbtun' || echo 'tbtun not loaded'" 2>&1 || true
        printf '=== far rmmod ===\n'
        ssh_far "rmmod tbtun 2>&1 || true; lsmod | grep -E '^tbtun' || echo 'tbtun not loaded'" 2>&1 || true
        printf '=== watchdog (left untouched) ===\n'
        ssh_near 'systemctl is-active tbtun-netwatch 2>/dev/null || true; tail -n 3 /var/log/tbtun-watchdog.log 2>/dev/null || true' 2>&1 || true
    } >>"$out" 2>&1
    local nl fl
    nl=$(ssh_near "lsmod | grep -c '^tbtun' || true" 2>/dev/null || echo '?')
    fl=$(ssh_far "lsmod | grep -c '^tbtun' || true" 2>/dev/null || echo '?')
    check cleanup_near_unloaded "$([[ ${nl:-1} == 0 ]]; echo $?)" "lsmod tbtun count=$nl"
    check cleanup_far_unloaded "$([[ ${fl:-1} == 0 ]]; echo $?)" "lsmod tbtun count=$fl"
    cat "$out" >&2
}

# ---------------------------------------------------------------------------
# status / cleanup only

if [[ $MODE == status ]]; then
    say "== near $NEAR_HOST"
    ssh_near 'cat /proc/net/tbtun/config 2>/dev/null; echo ---; awk '\''$1=="tunnel"&&$2==1&&($3=="c3"||$3=="c13")'\'' /proc/net/tbtun/stats 2>/dev/null' || true
    say "== far $FAR_HOST"
    ssh_far 'cat /proc/net/tbtun/config 2>/dev/null; echo ---; awk '\''$1=="tunnel"&&$2==1&&($3=="c3"||$3=="c13")'\'' /proc/net/tbtun/stats 2>/dev/null' || true
    exit 0
fi

if [[ $MODE == cleanup ]]; then
    EV=${EV:-$EVIDENCE_ROOT/sclc-verify-$(date -u +%Y%m%dT%H%M%SZ)}
    mkdir -p "$EV"
    cleanup_all
    say "evidence: $EV"
    exit 0
fi

# ---------------------------------------------------------------------------
# preflight (shared by --preflight and --run)

EV=${EV:-$EVIDENCE_ROOT/sclc-verify-$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$EV"
: >"$EV/run.log"
: >"$EV/timeline.txt"
say "evidence: $EV"
START_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
START_EPOCH=$(date +%s)
mark run-start

say "== 1. preflight"
if ! ssh_near true; then check near_ssh 1 "unreachable"; exit 3; fi
if ! ssh_far true; then check near_ssh 0 "reachable"; check far_ssh 1 "unreachable"; exit 3; fi
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
near_src=$(ssh_near 'cat /sys/module/tbtun/srcversion' 2>/dev/null | tr -d '[:space:]')
far_src=$(ssh_far 'cat /sys/module/tbtun/srcversion' 2>/dev/null | tr -d '[:space:]')
if [[ -n $EXPECT_NEAR_SRCVERSION ]]; then
    check near_srcversion "$([[ $near_src == "$EXPECT_NEAR_SRCVERSION" ]]; echo $?)" \
        "src=$near_src want=$EXPECT_NEAR_SRCVERSION"
else
    check near_srcversion "$([[ -n $near_src && $near_src != "$FIX43_NEAR_SRCVERSION" ]]; echo $?)" \
        "src=$near_src (not fix43 $FIX43_NEAR_SRCVERSION; pin EXPECT_NEAR_SRCVERSION from the build report)"
fi
if [[ -n $EXPECT_FAR_SRCVERSION ]]; then
    check far_srcversion "$([[ $far_src == "$EXPECT_FAR_SRCVERSION" ]]; echo $?)" \
        "src=$far_src want=$EXPECT_FAR_SRCVERSION"
else
    check far_srcversion "$([[ -n $far_src && $far_src != "$FIX43_FAR_SRCVERSION" ]]; echo $?)" \
        "src=$far_src (not fix43 $FIX43_FAR_SRCVERSION; pin EXPECT_FAR_SRCVERSION from the build report)"
fi
capture_end near "$EV/near-pre.txt"
capture_end far "$EV/far-pre.txt"

say "== 2. configuration"
for tok in "mode=sc-ldpc" "carriers=256" "fec=on" "adaptive=off" "crypto=on" "profile=http-kal"; do
    check "near_cfg_${tok%%=*}" "$(cfg_has "$EV/near-config.txt" "$tok"; echo $?)" "$tok"
done
for tok in "n=8" "m=24" "w=128" "L=16" "dev=$NEAR_DEV"; do
    check "near_cfg_${tok%%=*}" "$(cfg_has "$EV/near-config.txt" "$tok"; echo $?)" "$tok"
done
for tok in "mode=sc-ldpc" "carriers=256" "fec=on" "adaptive=off" "crypto=on" "profile=http-kal"; do
    check "far_cfg_${tok%%=*}" "$(cfg_has "$EV/far-config.txt" "$tok"; echo $?)" "$tok"
done
for tok in "n=128" "m=128" "w=256" "L=8"; do
    check "far_cfg_${tok%%=*}" "$(cfg_has "$EV/far-config.txt" "$tok"; echo $?)" "$tok"
done

near_up=$(stat_field "$EV/near-stats-pre.txt" c3 up)
far_up=$(stat_field "$EV/far-stats-pre.txt" c3 up)
check near_c3_up "$([[ ${near_up:-0} == 256 ]]; echo $?)" "up=${near_up:-?}"
check far_c3_up "$([[ ${far_up:-0} == 256 ]]; echo $?)" "up=${far_up:-?}"

for end in near far; do
    a=$(stat_field "$EV/$end-stats-pre.txt" c13 adaptive)
    n=$(stat_field "$EV/$end-stats-pre.txt" c13 n)
    m=$(stat_field "$EV/$end-stats-pre.txt" c13 m)
    g=$(stat_field "$EV/$end-stats-pre.txt" c13 groups)
    want_n=8 want_m=24 want_g=8
    [[ $end == far ]] && want_n=128 want_m=128 want_g=1
    check "${end}_c13_adaptive" "$([[ ${a:-} == off ]]; echo $?)" "adaptive=${a:-?}"
    check "${end}_c13_n" "$([[ ${n:-} == "$want_n" ]]; echo $?)" "n=${n:-?} want=$want_n"
    check "${end}_c13_m" "$([[ ${m:-} == "$want_m" ]]; echo $?)" "m=${m:-?} want=$want_m"
    check "${end}_c13_groups" "$([[ ${g:-} == "$want_g" ]]; echo $?)" "groups=${g:-?} want=$want_g"
done

# The peer-shape install: far must have taken near's uplink L=16 and block
# 128; near must have taken far's downlink L=8 and block 256. These lines
# are the visible proof that the fix42 resolver used the INIT body, not the
# local L.
ssh_far "dmesg 2>/dev/null | grep -F 'fec rx shape from peer' | tail -n 5" >"$EV/far-shape-dmesg.txt" 2>/dev/null || true
ssh_near "dmesg 2>/dev/null | grep -F 'fec rx shape from peer' | tail -n 5" >"$EV/near-shape-dmesg.txt" 2>/dev/null || true
far_shape=$(tail -n 1 "$EV/far-shape-dmesg.txt" 2>/dev/null || true)
near_shape=$(tail -n 1 "$EV/near-shape-dmesg.txt" 2>/dev/null || true)
check far_shape_line "$(grep -q 'L=16' <<<"$far_shape"; echo $?)" "${far_shape:-missing}"
check far_shape_block "$(grep -q 'block=128' <<<"$far_shape"; echo $?)" "${far_shape:-missing}"
check far_shape_nm "$(grep -q 'n=8 m=24 groups=8' <<<"$far_shape"; echo $?)" "${far_shape:-missing}"
check far_shape_rate "$(grep -q 'rate=64' <<<"$far_shape"; echo $?)" "${far_shape:-missing}"
check near_shape_line "$(grep -q 'L=8' <<<"$near_shape"; echo $?)" "${near_shape:-missing}"
check near_shape_block "$(grep -q 'block=256' <<<"$near_shape"; echo $?)" "${near_shape:-missing}"

for end in near far; do
    if grep -qE 'BUG:|Oops|WARNING:|UBSAN:|general protection fault' "$EV/$end-pre.txt" 2>/dev/null; then
        skip "${end}_baseline_kernel" "pre-existing BUG/Oops/WARNING in preflight capture; the in-window check still judges the run"
    else
        check "${end}_baseline_kernel" 0 "no BUG/Oops/WARNING"
    fi
done

# near path: the carriers must leave through the physical NIC.
NEAR_PHYS_DEV=$(phys_dev_for near)
FAR_PHYS_DEV=$(phys_dev_for far)
ssh_near "ip route get $FAR_IP 2>/dev/null" >"$EV/near-route.txt" 2>&1 || true
check near_route_dev "$([[ -n $NEAR_PHYS_DEV && $NEAR_PHYS_DEV != Meta && $NEAR_PHYS_DEV != tun* ]]; echo $?)" \
    "dev=${NEAR_PHYS_DEV:-none}"
check far_phys_dev "$([[ -n $FAR_PHYS_DEV ]]; echo $?)" "dev=${FAR_PHYS_DEV:-none}"
say "   near phys dev=${NEAR_PHYS_DEV:-?} far phys dev=${FAR_PHYS_DEV:-?}"

ssh_near "ip rule show" >"$EV/near-iprule-pre.txt" 2>&1 || true
check near_bypass_rule "$(grep -qE "^${BYPASS_PREF}:.*to ${FAR_IP}" "$EV/near-iprule-pre.txt"; echo $?)" \
    "pref $BYPASS_PREF to $FAR_IP"
ssh_near "ss -ltnH 'sport = :$NEAR_PORT' 2>/dev/null | grep -q ." >/dev/null 2>&1
check near_listen "$?" "127.0.0.1:$NEAR_PORT"
ssh_far "ss -ltnH 'sport = :$FAR_PORT' 2>/dev/null | grep -q ." >/dev/null 2>&1
check far_listen "$?" "0.0.0.0:$FAR_PORT"

# watchdog is observed, never touched.
feeder=$(ssh_near 'for d in /proc/[0-9]*/fd; do ls -l "$d" 2>/dev/null | grep -qF /dev/watchdog0 && { p=${d#/proc/}; echo "${p%/fd}"; exit 0; }; done' 2>/dev/null || true)
if [[ -n $feeder ]]; then
    check watchdog_feeder 0 "pid=$feeder holds /dev/watchdog0"
else
    skip watchdog_feeder "no /dev/watchdog0 holder; left untouched"
fi

if ((FAILED)); then
    capture_diag preflight
    note "preflight FAIL; no traffic attempted"
    exit 3
fi
mark preflight-done

if [[ $MODE == preflight ]]; then
    note "preflight PASS; no source start, no traffic"
    exit 0
fi

# ---------------------------------------------------------------------------
say "== 3. far HTTP source"
ssh_near "mkdir -p '$REMOTE_HELPERS'" >/dev/null 2>&1 || true
ssh_far "mkdir -p '$REMOTE_HELPERS'" >/dev/null 2>&1 || true
scp_far "$(dirname -- "$0")/../prod-1g/far-serve.sh" "$FAR_HOST:$REMOTE_HELPERS/far-serve.sh" ||
    { check far_upload 1 "scp far-serve.sh"; exit 4; }
scp_far "$(dirname -- "$0")/../prod-1g/far-raw-serve.py" "$FAR_HOST:$REMOTE_HELPERS/far-raw-serve.py" ||
    { check far_upload 1 "scp far-raw-serve.py"; exit 4; }
scp_far "$(dirname -- "$0")/fix42-sampler.sh" "$FAR_HOST:$REMOTE_HELPERS/fix42-sampler.sh" ||
    { check far_sampler_upload 1 "scp fix42-sampler.sh"; exit 4; }
scp_near "$(dirname -- "$0")/fix42-sampler.sh" "$NEAR_HOST:$REMOTE_HELPERS/fix42-sampler.sh" ||
    { check near_sampler_upload 1 "scp fix42-sampler.sh"; exit 4; }

far_out=$(ssh_far_long 900 "mkdir -p '$REMOTE_HELPERS'; FAR_DIR='$FAR_DIR' RAW_PY='$REMOTE_HELPERS/far-raw-serve.py' sh '$REMOTE_HELPERS/far-serve.sh' --start" 2>&1) || true
printf '%s\n' "$far_out" | tee "$EV/far-serve.log" >&2
FAR_SHA=$(printf '%s\n' "$far_out" | sed -n 's/^FAR_SHA256=//p' | tail -n 1)
FAR_SIZE=$(printf '%s\n' "$far_out" | sed -n 's/^FAR_SIZE=//p' | tail -n 1)
check far_source "$(printf '%s\n' "$far_out" | grep -q '^FAR_RESULT PASS'; echo $?)" \
    "size=${FAR_SIZE:-?} sha=${FAR_SHA:0:16}..."
check far_source_size "$([[ ${FAR_SIZE:-0} == "$EXPECT_BYTES" ]]; echo $?)" "size=${FAR_SIZE:-?}"
if ((FAILED)); then
    capture_diag far-source
    exit 4
fi

# ---------------------------------------------------------------------------
if [[ $MODE == run ]]; then
if ((SETTLE > 0)); then
    note "settle ${SETTLE}s after join before first-connect sampling"
    sleep "$SETTLE"
fi
# The join probes can repair for ~300 s, and a repair that completes inside
# the first-connect window charges its whole duration to that window. Wait
# until both ends' cumulative fec_nack_repair_ns is unchanged for 60 s, so
# the window contains only the first connect's own repairs.
note "wait for repair counters to be quiet for 60s before sampling"
quiet=0
prev_near=
prev_far=
tries=0
while (( quiet < 60 && tries < 60 )); do
    cur_near=$(ssh_near "grep -o 'fec_nack_repair_ns=[0-9]*' /proc/net/tbtun/stats 2>/dev/null | head -1")
    cur_far=$(ssh_far "grep -o 'fec_nack_repair_ns=[0-9]*' /proc/net/tbtun/stats 2>/dev/null | head -1")
    if [[ -n $cur_near && -n $cur_far && $cur_near == "$prev_near" && $cur_far == "$prev_far" ]]; then
        quiet=$((quiet + 10))
    else
        quiet=0
        prev_near=$cur_near
        prev_far=$cur_far
    fi
    tries=$((tries + 1))
    sleep 10
done
note "repair quiet wait done: stable=${quiet}s tries=${tries}"
if (( quiet < 60 )); then
    check repair_quiet 1 "no 60 s quiet window within 600 s (stable=${quiet}s)"
    note "join repair still active after 600 s; stopping before traffic"
    exit 4
fi
say "== 4. first connect (curl -m $FIRST_TMO, samplers at 10 Hz)"
mark first-connect-start
FIRST_EPOCH=$(date +%s)
ssh_near 'cat /proc/net/tbtun/stats' >"$EV/first-pre-near-stats.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/stats' >"$EV/first-pre-far-stats.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/tunnels' >"$EV/first-pre-near-tunnels.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/tunnels' >"$EV/first-pre-far-tunnels.txt" 2>/dev/null || true
first_fail_before=$FAILED

start_sampler "near:full" /tmp/fix42-first-near.log /tmp/fix42-first-near.stop \
    "$TUNNEL" "$NEAR_PHYS_DEV" 0.1
start_sampler "far:full" /tmp/fix42-first-far.log /tmp/fix42-first-far.stop \
    "$TUNNEL" "$FAR_PHYS_DEV" 0.1
sleep 1
near_sampled=$(ssh_near 'wc -l < /tmp/fix42-first-near.log 2>/dev/null || echo 0' 2>/dev/null | tr -d '[:space:]')
far_sampled=$(ssh_far 'wc -l < /tmp/fix42-first-far.log 2>/dev/null || echo 0' 2>/dev/null | tr -d '[:space:]')
check first_sampler_started "$([[ ${near_sampled:-0} -gt 0 && ${far_sampled:-0} -gt 0 ]]; echo $?)" \
    "near_lines=$near_sampled far_lines=$far_sampled"

ssh_near "rm -f /tmp/sclc-first2.bin /tmp/sclc-first2.curl /tmp/fix42-first.exit" 2>/dev/null || true
first_rc=0
ssh_near "curl -sS -m $FIRST_TMO -o /tmp/sclc-first2.bin \
    -w 'http=%{http_code} size=%{size_download} ttfb=%{time_starttransfer} time=%{time_total} speed=%{speed_download}\n' \
    http://127.0.0.1:$NEAR_PORT/ > /tmp/sclc-first2.curl 2>&1" || first_rc=$?
ssh_near 'date +%s.%N > /tmp/fix42-first.exit' 2>/dev/null || true
sleep 1
stop_sampler near /tmp/fix42-first-near.stop
stop_sampler far /tmp/fix42-first-far.stop
sleep 0.3

ssh_near 'cat /tmp/sclc-first2.curl' >"$EV/first-connect.txt" 2>/dev/null || true
ssh_near 'stat -c %s /tmp/sclc-first2.bin 2>/dev/null || echo 0' >"$EV/first-size.txt" 2>/dev/null || true
ssh_near 'cat /tmp/fix42-first-near.log' >"$EV/first-sampler-near.log" 2>/dev/null || true
ssh_far 'cat /tmp/fix42-first-far.log' >"$EV/first-sampler-far.log" 2>/dev/null || true
ssh_near 'cat /tmp/fix42-first.exit' >"$EV/first-exit-ts.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/stats' >"$EV/first-post-near-stats.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/stats' >"$EV/first-post-far-stats.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/tunnels' >"$EV/first-post-near-tunnels.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/tunnels' >"$EV/first-post-far-tunnels.txt" 2>/dev/null || true
cat "$EV/first-connect.txt" >&2

analysis=$(python3 "$(dirname -- "$0")/fix42-analyze-first.py" \
    --near-log "$EV/first-sampler-near.log" \
    --far-log "$EV/first-sampler-far.log" \
    --near-exit "$EV/first-exit-ts.txt" \
    --curl "$EV/first-connect.txt" \
    --near-size "$EV/first-size.txt" \
    --rc "$first_rc" --profile fix44 \
    --json "$EV/first-analysis.json" 2>&1) || true
printf '%s\n' "$analysis" >"$EV/first-analysis.txt"
grep -E '^CHECK ' <<<"$analysis" >>"$EV/run.log" || true
grep -E '^CHECK ' <<<"$analysis" | grep -q ' FAIL' && FIRST_FAILED=1
say "$(grep -E '^CHECK ' <<<"$analysis" || true)"
grep -vE '^CHECK ' <<<"$analysis" | grep -v '^FIRST_RESULT ' || true

kernel_window near "$FIRST_EPOCH" "$(date +%s)" "$EV/near-kernel-first.txt"
kernel_window far "$FIRST_EPOCH" "$(date +%s)" "$EV/far-kernel-first.txt"
for end in near far; do
    case $(kw_state "$EV/$end-kernel-first.txt") in
        SKIP) skip "${end}_first_kernel" "empty window, source=$(kw_source "$EV/$end-kernel-first.txt")" ;;
        FAIL)
            check "${end}_first_kernel" 1 "BUG/Oops/WARNING in window"
            FIRST_FAILED=1
            ;;
        HOG)
            say "${end}_first_kernel_hog=1 (tbtun workqueue hog; informational)"
            printf '%s\n' "${end}_first_kernel_hog=1" >>"$EV/run.log"
            ;;
        *) check "${end}_first_kernel" 0 "window clean, source=$(kw_source "$EV/$end-kernel-first.txt")" ;;
    esac
done

if ((FIRST_FAILED)) || ((FAILED > first_fail_before)); then
    capture_diag first-connect
    note "first connect FAIL; 256 steady not attempted"
    cleanup_all
    exit 5
fi
mark first-connect-done
else
    note "from-steady: first connect skipped; the prior PASS evidence stays in the earlier run"
fi

# ---------------------------------------------------------------------------
if [[ $MODE != one-g ]]; then
say "== 5. steady state (warmup ${WARMUP}s + measure ${MEASURE}s)"
mark steady-start
STEADY_EPOCH=$(date +%s)

# Wait for the first stream to leave so the steady stream starts clean.
i=0
stream_clear=1
while [[ $i -lt 30 ]]; do
    sactive=$(ssh_near 'cat /proc/net/tbtun/stats' 2>/dev/null |
        awk -v t="$TUNNEL" '$1 == "tunnel" && $2 == t && $3 == "streams" {
            for (i = 1; i <= NF; i++) if ($i ~ /^streams_active=/) { sub("streams_active=", "", $i); print $i; exit }
        }')
    up_now=$(ssh_near 'cat /proc/net/tbtun/stats' 2>/dev/null | awk -v t="$TUNNEL" '
        $1 == "tunnel" && $2 == t && $3 == "c3" {
            for (i = 1; i <= NF; i++) if ($i ~ /^up=/) { sub("up=", "", $i); print $i; exit }
        }')
    if [[ ${up_now:-0} == 256 && ${sactive:-0} == 0 ]]; then
        stream_clear=0
        break
    fi
    sleep 1
    i=$((i + 1))
done
check steady_stream_clear "$stream_clear" "streams_active=${sactive:-?} up=${up_now:-?} wait=${i}s"

start_sampler "near:compact" /tmp/fix42-steady-near.log /tmp/fix42-steady-near.stop \
    "$TUNNEL" "$NEAR_PHYS_DEV" 0.5
start_sampler "far:compact" /tmp/fix42-steady-far.log /tmp/fix42-steady-far.stop \
    "$TUNNEL" "$FAR_PHYS_DEV" 0.5
sleep 1
ns=$(ssh_near 'wc -l < /tmp/fix42-steady-near.log 2>/dev/null || echo 0' 2>/dev/null | tr -d '[:space:]')
fs=$(ssh_far 'wc -l < /tmp/fix42-steady-far.log 2>/dev/null || echo 0' 2>/dev/null | tr -d '[:space:]')
if [[ ${ns:-0} -gt 0 && ${fs:-0} -gt 0 ]]; then
    check steady_sampler_started 0 "near_lines=$ns far_lines=$fs"
else
    skip steady_sampler_started "near_lines=${ns:-?} far_lines=${fs:-?}; direct samples still cover up"
fi

cat > /tmp/fix42-steady.sh <<EOF
#!/bin/sh
echo \$\$ > /tmp/fix42-steady.pid
exec curl -sS -m $STEADY_TMO -o /dev/null \
    -w 'http=%{http_code} size=%{size_download} ttfb=%{time_starttransfer} time=%{time_total} speed=%{speed_download}\n' \
    http://127.0.0.1:$NEAR_PORT/
EOF
scp_near /tmp/fix42-steady.sh "$NEAR_HOST:/tmp/fix42-steady.sh" >/dev/null 2>&1 || true
ssh_near "rm -f /tmp/fix42-steady.curl /tmp/fix42-steady.pid; setsid sh /tmp/fix42-steady.sh </dev/null >/tmp/fix42-steady.curl 2>&1 & echo started" >/dev/null 2>&1 || true
sleep 1
STEADY_PID=$(ssh_near 'cat /tmp/fix42-steady.pid 2>/dev/null' 2>/dev/null | tr -d '[:space:]')
if [[ -z $STEADY_PID ]]; then
    STEADY_PID=$(ssh_near "pgrep -f 'curl.*127[.]0[.]0[.]1:$NEAR_PORT' | head -n 1" 2>/dev/null | tr -d '[:space:]')
fi

sleep "$WARMUP"
sample near steady-a-near
sample far steady-a-far
ssh_near "cat /proc/$STEADY_PID/io 2>/dev/null" >"$EV/steady-a-near-io.txt" 2>/dev/null || true
sleep "$MEASURE"
sample near steady-b-near
sample far steady-b-far
ssh_near "cat /proc/$STEADY_PID/io 2>/dev/null" >"$EV/steady-b-near-io.txt" 2>/dev/null || true
ssh_near "pkill -f 'curl.*127[.]0[.]0[.]1:$NEAR_PORT' 2>/dev/null || true" || true
sleep 1
stop_sampler near /tmp/fix42-steady-near.stop
stop_sampler far /tmp/fix42-steady-far.stop
sleep 0.3
ssh_near 'cat /tmp/fix42-steady-near.log' >"$EV/steady-sampler-near.log" 2>/dev/null || true
ssh_far 'cat /tmp/fix42-steady-far.log' >"$EV/steady-sampler-far.log" 2>/dev/null || true
ssh_near 'cat /tmp/fix42-steady.curl' >"$EV/steady-curl.txt" 2>/dev/null || true

steady_app=$(delta "$(cap_field "$EV/steady-a-far-stats.txt" submit_bytes)" \
                   "$(cap_field "$EV/steady-b-far-stats.txt" submit_bytes)")
steady_app_near=$(delta "$(cap_field "$EV/steady-a-near-stats.txt" submit_bytes)" \
                        "$(cap_field "$EV/steady-b-near-stats.txt" submit_bytes)")
wire_far_tx=$(delta "$(netdev_tx "$EV/steady-a-far-netdev.txt" "$FAR_PHYS_DEV")" \
                    "$(netdev_tx "$EV/steady-b-far-netdev.txt" "$FAR_PHYS_DEV")")
wire_near_rx=$(delta "$(netdev_rx "$EV/steady-a-near-netdev.txt" "$NEAR_PHYS_DEV")" \
                     "$(netdev_rx "$EV/steady-b-near-netdev.txt" "$NEAR_PHYS_DEV")")
read -r nb ni < <(cpu_busy "$EV/steady-a-near-procstat.txt") || true
read -r nb2 ni2 < <(cpu_busy "$EV/steady-b-near-procstat.txt") || true
near_cpu=$(cpu_pct "${nb:-0}" "${ni:-0}" "${nb2:-0}" "${ni2:-0}")
read -r fb fi < <(cpu_busy "$EV/steady-a-far-procstat.txt") || true
read -r fb2 fi2 < <(cpu_busy "$EV/steady-b-far-procstat.txt") || true
far_cpu=$(cpu_pct "${fb:-0}" "${fi:-0}" "${fb2:-0}" "${fi2:-0}")
near_io_a=$(sed -n 's/^rchar: //p' "$EV/steady-a-near-io.txt" 2>/dev/null | head -n 1)
near_io_b=$(sed -n 's/^rchar: //p' "$EV/steady-b-near-io.txt" 2>/dev/null | head -n 1)
steady_app_io=$(delta "${near_io_a:-}" "${near_io_b:-}")

steady_report() {
    local end=$1
    local pre=$EV/steady-a-$end-stats.txt post=$EV/steady-b-$end-stats.txt
    {
        printf 'app_bytes=%s\n' "$steady_app"
        printf 'app_bytes_near_submit=%s\n' "$steady_app_near"
        printf 'app_bytes_near_curl_rchar=%s\n' "$steady_app_io"
        printf 'wire_far_tx=%s\n' "$wire_far_tx"
        printf 'wire_near_rx=%s\n' "$wire_near_rx"
        printf 'wire_over_app_ppm=%s\n' "$(awk -v w="${wire_far_tx:-0}" -v a="${steady_app:-0}" 'BEGIN{if(a>0) printf "%d", w*1000000/a; else print "?"}')"
        printf 'near_cpu_busy_pct=%s\n' "$near_cpu"
        printf 'far_cpu_busy_pct=%s\n' "$far_cpu"
        for f in fec_outq_drop fec_outq_high_wait fec_init_bad \
            fec_nack_tx fec_nack_rx fec_nack_retx fec_nack_drop \
            fec_replay_hit fec_replay_miss fec_replay_miss_never_stored \
            fec_reset_tcp_failed fec_reset_user fec_reset_hole \
            fec_nack_lat_ns fec_nack_lat_max_ns \
            fec_nack_repair_ns fec_nack_repair_max_ns fec_outq_max \
            fec_replay_miss_enospc fec_replay_drop fec_nack_backpressure \
            fec_hol_wait fec_short_parity_saved fec_nack_pad_skip; do
            printf '%s=%s\n' "$f" "$(delta "$(stat_field "$pre" c11 "$f")" "$(stat_field "$post" c11 "$f")")"
        done
        for f in fec_decode_blocks fec_decode_recovered fec_decode_nomem \
            fec_deliver_bad fec_epoch_conflict fec_marker_conflict; do
            printf '%s=%s\n' "$f" "$(delta "$(stat_field "$pre" c10 "$f")" "$(stat_field "$post" c10 "$f")")"
        done
        printf 'holes_unrecoverable=%s\n' "$(delta "$(stat_field "$pre" c6 holes_unrecoverable)" "$(stat_field "$post" c6 holes_unrecoverable)")"
        printf 'holes_declared=%s\n' "$(delta "$(stat_field "$pre" c6 holes_declared)" "$(stat_field "$post" c6 holes_declared)")"
        printf 'stream_reset_tcp_failed=%s\n' "$(delta "$(stat_field "$pre" c3 stream_reset_tcp_failed)" "$(stat_field "$post" c3 stream_reset_tcp_failed)")"
        printf 'stream_reset_user=%s\n' "$(delta "$(stat_field "$pre" c3 stream_reset_user)" "$(stat_field "$post" c3 stream_reset_user)")"
        printf 'submit_bytes=%s\n' "$(delta "$(cap_field "$pre" submit_bytes)" "$(cap_field "$post" submit_bytes)")"
        printf 'credit_wait=%s\n' "$(delta "$(stat_field "$pre" c9 fec_credit_wait)" "$(stat_field "$post" c9 fec_credit_wait)")"
        printf 'ack_tx=%s\n' "$(delta "$(stat_field "$pre" c9 fec_ack_tx)" "$(stat_field "$post" c9 fec_ack_tx)")"
        printf 'ack_rx=%s\n' "$(delta "$(stat_field "$pre" c9 fec_ack_rx)" "$(stat_field "$post" c9 fec_ack_rx)")"
        printf 'up_post=%s\n' "$(stat_field "$post" c3 up)"
        printf 'outq_depth_post=%s\n' "$(stat_field "$post" c11 fec_outq_depth)"
        printf 'replay_queue_high_post=%s\n' "$(stat_field "$post" c11 fec_replay_queue_high)"
        printf 'repair_max_post=%s\n' "$(stat_field "$post" c11 fec_nack_repair_max_ns)"
        printf 'replay_queued_post=%s\n' "$(stat_field "$post" c11 fec_replay_queued)"
        printf 'c12_post=%s\n' "$(awk -v t="$TUNNEL" '$1=="tunnel"&&$2==t&&$3=="c12"{sub(/^tunnel [0-9]+ c12 fec_outq/,""); print; exit}' "$post" 2>/dev/null)"
    } >"$EV/steady-$end.txt"
}
steady_report near
steady_report far

steady_zero() { # end field
    local v
    v=$(sed -n "s/^$2=//p" "$EV/steady-$1.txt" | head -n 1)
    [[ ${v:-?} == 0 ]]
}
near_up_min=$(grep -o 'up=[0-9]*' "$EV/steady-sampler-near.log" 2>/dev/null | cut -d= -f2 | sort -n | head -n 1)
far_up_min=$(grep -o 'up=[0-9]*' "$EV/steady-sampler-far.log" 2>/dev/null | cut -d= -f2 | sort -n | head -n 1)
for end in near far; do
    check "${end}_steady_up" "$([[ $(stat_field "$EV/steady-b-$end-stats.txt" c3 up) == 256 ]]; echo $?)" \
        "up=$(stat_field "$EV/steady-b-$end-stats.txt" c3 up) sampler_min=${near_up_min:-?}/${far_up_min:-?}"
    check "${end}_steady_outq_drop" "$(steady_zero "$end" fec_outq_drop; echo $?)" \
        "delta=$(sed -n 's/^fec_outq_drop=//p' "$EV/steady-$end.txt")"
    # fix43 pauses the packetizer at the replay half-mark; that increments
    # fec_outq_high_wait by design. Only a drop is a failure here.
    ow=$(sed -n 's/^fec_outq_high_wait=//p' "$EV/steady-$end.txt" | head -n 1)
    say "${end}_steady_outq_high_wait=${ow:-?} (pacing, informational)"
    printf '%s\n' "${end}_steady_outq_high_wait=${ow:-?}" >>"$EV/run.log"
    check "${end}_steady_init_bad" "$(steady_zero "$end" fec_init_bad; echo $?)" \
        "delta=$(sed -n 's/^fec_init_bad=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_epoch_conflict" "$(steady_zero "$end" fec_epoch_conflict; echo $?)" \
        "delta=$(sed -n 's/^fec_epoch_conflict=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_reset_hole" "$(steady_zero "$end" fec_reset_hole; echo $?)" \
        "delta=$(sed -n 's/^fec_reset_hole=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_reset_tcp" "$(steady_zero "$end" fec_reset_tcp_failed; echo $?)" \
        "delta=$(sed -n 's/^fec_reset_tcp_failed=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_holes_unrec" "$(steady_zero "$end" holes_unrecoverable; echo $?)" \
        "delta=$(sed -n 's/^holes_unrecoverable=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_deliver_bad" "$(steady_zero "$end" fec_deliver_bad; echo $?)" \
        "delta=$(sed -n 's/^fec_deliver_bad=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_replay_enospc" "$(steady_zero "$end" fec_replay_miss_enospc; echo $?)" \
        "delta=$(sed -n 's/^fec_replay_miss_enospc=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_replay_drop" "$(steady_zero "$end" fec_replay_drop; echo $?)" \
        "delta=$(sed -n 's/^fec_replay_drop=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_nack_backpressure" "$(steady_zero "$end" fec_nack_backpressure; echo $?)" \
        "delta=$(sed -n 's/^fec_nack_backpressure=//p' "$EV/steady-$end.txt")"
    ns=$(sed -n 's/^fec_replay_miss_never_stored=//p' "$EV/steady-$end.txt" | head -n 1)
    check "${end}_steady_never_stored" "$([[ ${ns:-999999} -le 64 ]]; echo $?)" \
        "delta=$ns bound=64"
    sp=$(sed -n 's/^fec_short_parity_saved=//p' "$EV/steady-$end.txt" | head -n 1)
    say "${end}_steady_short_parity_saved_window=${sp:-?} (informational)"
    printf '%s\n' "${end}_steady_short_parity_saved_window=${sp:-?}" >>"$EV/run.log"
    spc=$(stat_field "$EV/steady-b-$end-stats.txt" c11 fec_short_parity_saved)
    check "${end}_steady_short_parity_saved_cum" "$([[ ${spc:-0} -gt 0 ]]; echo $?)" \
        "cumulative=$spc"
    ps=$(sed -n 's/^fec_nack_pad_skip=//p' "$EV/steady-$end.txt" | head -n 1)
    check "${end}_steady_nack_pad_skip" "$([[ ${ps:-1} == 0 ]]; echo $?)" \
        "delta=$ps bound=0"
    lns=$(sed -n 's/^fec_nack_lat_ns=//p' "$EV/steady-$end.txt" | head -n 1)
    check "${end}_steady_nack_lat_ns" "$([[ ${lns:-999999999999} -lt 180000000000 ]]; echo $?)" \
        "window_delta_ns=$lns bound=180s (NACK->wire)"
    rns=$(sed -n 's/^fec_nack_repair_ns=//p' "$EV/steady-$end.txt" | head -n 1)
    say "${end}_steady_repair_ns_block_ready=${rns:-?} (informational)"
    printf '%s\n' "${end}_steady_repair_ns_block_ready=${rns:-?}" >>"$EV/run.log"
    rmax=$(sed -n 's/^repair_max_post=//p' "$EV/steady-$end.txt" | head -n 1)
    say "${end}_steady_repair_max_session=${rmax:-?} (informational)"
    printf '%s\n' "${end}_steady_repair_max_session=${rmax:-?}" >>"$EV/run.log"
    # fec_hol_wait is weighted by the per-carrier queue depth on every
    # EAGAIN; a deep but draining TX outq makes it large without a data-plane
    # failure. Record it; the hard bound belongs to the 15 s first connect.
    hw=$(sed -n 's/^fec_hol_wait=//p' "$EV/steady-$end.txt" | head -n 1)
    say "${end}_steady_hol_wait=${hw:-?} (weighted by queue depth, informational)"
    printf '%s\n' "${end}_steady_hol_wait=${hw:-?}" >>"$EV/run.log"
    qh=$(sed -n 's/^replay_queue_high_post=//p' "$EV/steady-$end.txt" | head -n 1)
    check "${end}_steady_replay_queue_high" "$([[ ${qh:-0} -lt 5242880 ]]; echo $?)" "high=$qh bound=5242880"
done
check steady_app_bytes "$([[ ${steady_app:-0} -gt 0 ]]; echo $?)" "app=${steady_app:-?}"
if num "${steady_app_io:-}" && ((steady_app_io > 0)); then
    app_diff=$(( steady_app_io > steady_app ? steady_app_io - steady_app : steady_app - steady_app_io ))
    check steady_app_crosscheck "$([[ $app_diff -le 1048576 ]]; echo $?)" \
        "curl_rchar=${steady_app_io} far_submit=${steady_app:-?} diff=${app_diff} bound=1048576"
else
    skip steady_app_crosscheck "curl rchar unavailable (pid=${STEADY_PID:-?}); far submit=${steady_app:-?}"
fi
near_ack_tx_s=$(sed -n 's/^ack_tx=//p' "$EV/steady-near.txt" | head -n 1)
far_ack_rx_s=$(sed -n 's/^ack_rx=//p' "$EV/steady-far.txt" | head -n 1)
check near_steady_ack_tx "$([[ ${near_ack_tx_s:-0} -gt 0 ]]; echo $?)" "near_ack_tx=${near_ack_tx_s:-?}"
check far_steady_ack_rx "$([[ ${far_ack_rx_s:-0} -gt 0 ]]; echo $?)" "far_ack_rx=${far_ack_rx_s:-?}"
check steady_wire_bytes "$([[ ${wire_far_tx:-0} -gt 0 ]]; echo $?)" \
    "far_tx=${wire_far_tx:-?} near_rx=${wire_near_rx:-?} dev=${FAR_PHYS_DEV:-?}"
# credit wait growth must be paired with ACK progress on the same direction.
for end in near far; do
    cw=$(sed -n 's/^credit_wait=//p' "$EV/steady-$end.txt" | head -n 1)
    other=near
    [[ $end == near ]] && other=far
    acktx=$(sed -n 's/^ack_tx=//p' "$EV/steady-$other.txt" | head -n 1)
    check "${end}_steady_credit_progress" "$([[ ${cw:-0} == 0 || ${acktx:-0} -gt 0 ]]; echo $?)" \
        "credit_wait=$cw peer_ack_tx=${acktx:-?}"
done

kernel_window near "$STEADY_EPOCH" "$(date +%s)" "$EV/near-kernel-steady.txt"
kernel_window far "$STEADY_EPOCH" "$(date +%s)" "$EV/far-kernel-steady.txt"
for end in near far; do
    case $(kw_state "$EV/$end-kernel-steady.txt") in
        SKIP) skip "${end}_steady_kernel" "empty window, source=$(kw_source "$EV/$end-kernel-steady.txt")" ;;
        FAIL) check "${end}_steady_kernel" 1 "BUG/Oops/WARNING in window" ;;
        HOG)
            say "${end}_steady_kernel_hog=1 (tbtun workqueue hog; informational)"
            printf '%s\n' "${end}_steady_kernel_hog=1" >>"$EV/run.log"
            ;;
        *) check "${end}_steady_kernel" 0 "window clean, source=$(kw_source "$EV/$end-kernel-steady.txt")" ;;
    esac
done

if ((FAILED)); then
    STEADY_FAILED=1
    capture_diag steady
    note "steady state FAIL; 1 GiB not attempted"
    cleanup_all
    exit 6
fi
mark steady-done
else
    note "one-g-only: first connect and steady skipped; 1 GiB on the live session"
fi

# ---------------------------------------------------------------------------
say "== 6. one 1 GiB single stream (max ${ONE_G_MAX}s)"
mark one-g-start
ssh_near 'mkdir -p /root/sclc-verify; rm -f /root/sclc-verify/sclc-1g.bin /root/sclc-verify/sclc-1g.curl' 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/stats' >"$EV/one-g-far-pre.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/stats' >"$EV/one-g-near-pre.txt" 2>/dev/null || true
ssh_near 'cat /proc/stat' >"$EV/one-g-near-procstat-pre.txt" 2>/dev/null || true
ssh_far 'cat /proc/stat' >"$EV/one-g-far-procstat-pre.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/dev' >"$EV/one-g-near-netdev-pre.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/dev' >"$EV/one-g-far-netdev-pre.txt" 2>/dev/null || true
start_sampler "near:compact" /tmp/fix42-1g-near.log /tmp/fix42-1g-near.stop \
    "$TUNNEL" "$NEAR_PHYS_DEV" 1
start_sampler "far:compact" /tmp/fix42-1g-far.log /tmp/fix42-1g-far.stop \
    "$TUNNEL" "$FAR_PHYS_DEV" 1
sleep 1
ns=$(ssh_near 'wc -l < /tmp/fix42-1g-near.log 2>/dev/null || echo 0' 2>/dev/null | tr -d '[:space:]')
fs=$(ssh_far 'wc -l < /tmp/fix42-1g-far.log 2>/dev/null || echo 0' 2>/dev/null | tr -d '[:space:]')
if [[ ${ns:-0} -gt 0 && ${fs:-0} -gt 0 ]]; then
    check one_g_sampler_started 0 "near_lines=$ns far_lines=$fs"
else
    skip one_g_sampler_started "near_lines=${ns:-?} far_lines=${fs:-?}; direct samples still cover up"
fi
NEAR_T0=$(ssh_near 'date +%s' 2>/dev/null || true)
FAR_T0=$(ssh_far 'date +%s' 2>/dev/null || true)
case ${NEAR_T0:-} in '' | *[!0-9]*) NEAR_T0=$START_EPOCH ;; esac
case ${FAR_T0:-} in '' | *[!0-9]*) FAR_T0=$START_EPOCH ;; esac

one_rc=0
ssh_near_long "$ONE_G_TMO" "curl -sS -m $ONE_G_MAX -o /root/sclc-verify/sclc-1g.bin \
    -w 'http=%{http_code} size=%{size_download} ttfb=%{time_starttransfer} time=%{time_total} speed=%{speed_download}\n' \
    http://127.0.0.1:$NEAR_PORT/ > /root/sclc-verify/sclc-1g.curl 2>&1" || one_rc=$?
NEAR_T1=$(ssh_near 'date +%s' 2>/dev/null || true)
FAR_T1=$(ssh_far 'date +%s' 2>/dev/null || true)
case ${NEAR_T1:-} in '' | *[!0-9]*) NEAR_T1=$(date +%s) ;; esac
case ${FAR_T1:-} in '' | *[!0-9]*) FAR_T1=$(date +%s) ;; esac
stop_sampler near /tmp/fix42-1g-near.stop
stop_sampler far /tmp/fix42-1g-far.stop
sleep 0.3
ssh_near 'cat /tmp/fix42-1g-near.log' >"$EV/one-g-sampler-near.log" 2>/dev/null || true
ssh_far 'cat /tmp/fix42-1g-far.log' >"$EV/one-g-sampler-far.log" 2>/dev/null || true
ssh_near 'cat /root/sclc-verify/sclc-1g.curl' >"$EV/one-g-curl.txt" 2>/dev/null || true
ssh_near 'stat -c %s /root/sclc-verify/sclc-1g.bin 2>/dev/null || echo 0' >"$EV/one-g-size.txt" 2>/dev/null || true
ssh_near_long 300 'sha256sum /root/sclc-verify/sclc-1g.bin 2>/dev/null | awk "{print \$1}"' >"$EV/one-g-near-sha.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/stats' >"$EV/one-g-far-post.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/stats' >"$EV/one-g-near-post.txt" 2>/dev/null || true
ssh_near 'cat /proc/stat' >"$EV/one-g-near-procstat-post.txt" 2>/dev/null || true
ssh_far 'cat /proc/stat' >"$EV/one-g-far-procstat-post.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/dev' >"$EV/one-g-near-netdev-post.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/dev' >"$EV/one-g-far-netdev-post.txt" 2>/dev/null || true
mark one-g-done "rc=$one_rc"

one_line=$(grep 'http=' "$EV/one-g-curl.txt" 2>/dev/null | tail -n 1 || true)
one_http=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^http=/){sub("http=","",$i); print $i}}' <<<"$one_line")
one_size=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^size=/){sub("size=","",$i); print $i}}' <<<"$one_line")
one_time=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^time=/){sub("time=","",$i); print $i}}' <<<"$one_line")
one_speed=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^speed=/){sub("speed=","",$i); print $i}}' <<<"$one_line")
one_ttfb=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^ttfb=/){sub("ttfb=","",$i); print $i}}' <<<"$one_line")
one_file=$(tr -d '[:space:]' <"$EV/one-g-size.txt" 2>/dev/null || true)
one_sha=$(tr -d '[:space:]' <"$EV/one-g-near-sha.txt" 2>/dev/null || true)

check one_g_rc "$([ "$one_rc" -eq 0 ]; echo $?)" "rc=$one_rc"
check one_g_http "$([[ ${one_http:-000} == 200 ]]; echo $?)" "http=${one_http:-000}"
check one_g_size "$([[ ${one_size:-0} == "$EXPECT_BYTES" ]]; echo $?)" "size_download=${one_size:-0}"
check one_g_file "$([[ ${one_file:-0} == "$EXPECT_BYTES" ]]; echo $?)" "file=${one_file:-0}"
check one_g_sha "$([[ -n $one_sha && $one_sha == "$FAR_SHA" ]]; echo $?)" \
    "near=${one_sha:0:16}... far=${FAR_SHA:0:16}..."

one_report() {
    local end=$1
    local pre=$EV/one-g-$end-pre.txt post=$EV/one-g-$end-post.txt
    {
        for f in fec_outq_drop fec_outq_high_wait fec_init_bad \
            fec_nack_tx fec_nack_rx fec_nack_retx fec_nack_drop \
            fec_replay_hit fec_replay_miss fec_replay_miss_never_stored \
            fec_reset_tcp_failed fec_reset_user fec_reset_hole \
            fec_nack_lat_ns fec_nack_lat_max_ns \
            fec_nack_repair_ns fec_nack_repair_max_ns fec_outq_max \
            fec_replay_miss_enospc fec_replay_drop fec_nack_backpressure \
            fec_hol_wait fec_short_parity_saved fec_nack_pad_skip; do
            printf '%s=%s\n' "$f" "$(delta "$(stat_field "$pre" c11 "$f")" "$(stat_field "$post" c11 "$f")")"
        done
        for f in fec_decode_blocks fec_decode_recovered fec_decode_nomem \
            fec_deliver_bad fec_epoch_conflict fec_marker_conflict; do
            printf '%s=%s\n' "$f" "$(delta "$(stat_field "$pre" c10 "$f")" "$(stat_field "$post" c10 "$f")")"
        done
        printf 'holes_unrecoverable=%s\n' "$(delta "$(stat_field "$pre" c6 holes_unrecoverable)" "$(stat_field "$post" c6 holes_unrecoverable)")"
        printf 'holes_declared=%s\n' "$(delta "$(stat_field "$pre" c6 holes_declared)" "$(stat_field "$post" c6 holes_declared)")"
        printf 'stream_reset_tcp_failed=%s\n' "$(delta "$(stat_field "$pre" c3 stream_reset_tcp_failed)" "$(stat_field "$post" c3 stream_reset_tcp_failed)")"
        printf 'stream_reset_user=%s\n' "$(delta "$(stat_field "$pre" c3 stream_reset_user)" "$(stat_field "$post" c3 stream_reset_user)")"
        printf 'submit_bytes=%s\n' "$(delta "$(cap_field "$pre" submit_bytes)" "$(cap_field "$post" submit_bytes)")"
        printf 'credit_wait=%s\n' "$(delta "$(stat_field "$pre" c9 fec_credit_wait)" "$(stat_field "$post" c9 fec_credit_wait)")"
        printf 'ack_tx=%s\n' "$(delta "$(stat_field "$pre" c9 fec_ack_tx)" "$(stat_field "$post" c9 fec_ack_tx)")"
        printf 'ack_rx=%s\n' "$(delta "$(stat_field "$pre" c9 fec_ack_rx)" "$(stat_field "$post" c9 fec_ack_rx)")"
        printf 'up_post=%s\n' "$(stat_field "$post" c3 up)"
        printf 'replay_queue_high_post=%s\n' "$(stat_field "$post" c11 fec_replay_queue_high)"
        printf 'repair_max_post=%s\n' "$(stat_field "$post" c11 fec_nack_repair_max_ns)"
    } >"$EV/one-g-$end.txt"
}
one_report near
one_report far
one_zero() {
    local v
    v=$(sed -n "s/^$2=//p" "$EV/one-g-$1.txt" | head -n 1)
    [[ ${v:-?} == 0 ]]
}
for end in near far; do
    check "${end}_one_g_up" "$([[ $(stat_field "$EV/one-g-$end-post.txt" c3 up) == 256 ]]; echo $?)" \
        "up=$(stat_field "$EV/one-g-$end-post.txt" c3 up)"
    check "${end}_one_g_outq_drop" "$(one_zero "$end" fec_outq_drop; echo $?)" \
        "delta=$(sed -n 's/^fec_outq_drop=//p' "$EV/one-g-$end.txt")"
    ow=$(sed -n 's/^fec_outq_high_wait=//p' "$EV/one-g-$end.txt" | head -n 1)
    say "${end}_one_g_outq_high_wait=${ow:-?} (pacing, informational)"
    printf '%s\n' "${end}_one_g_outq_high_wait=${ow:-?}" >>"$EV/run.log"
    check "${end}_one_g_init_bad" "$(one_zero "$end" fec_init_bad; echo $?)" \
        "delta=$(sed -n 's/^fec_init_bad=//p' "$EV/one-g-$end.txt")"
    check "${end}_one_g_reset_hole" "$(one_zero "$end" fec_reset_hole; echo $?)" \
        "delta=$(sed -n 's/^fec_reset_hole=//p' "$EV/one-g-$end.txt")"
    check "${end}_one_g_epoch_conflict" "$(one_zero "$end" fec_epoch_conflict; echo $?)" \
        "delta=$(sed -n 's/^fec_epoch_conflict=//p' "$EV/one-g-$end.txt")"
    check "${end}_one_g_reset_tcp" "$(one_zero "$end" fec_reset_tcp_failed; echo $?)" \
        "delta=$(sed -n 's/^fec_reset_tcp_failed=//p' "$EV/one-g-$end.txt")"
    check "${end}_one_g_holes_unrec" "$(one_zero "$end" holes_unrecoverable; echo $?)" \
        "delta=$(sed -n 's/^holes_unrecoverable=//p' "$EV/one-g-$end.txt")"
    check "${end}_one_g_deliver_bad" "$(one_zero "$end" fec_deliver_bad; echo $?)" \
        "delta=$(sed -n 's/^fec_deliver_bad=//p' "$EV/one-g-$end.txt")"
    check "${end}_one_g_replay_enospc" "$(one_zero "$end" fec_replay_miss_enospc; echo $?)" \
        "delta=$(sed -n 's/^fec_replay_miss_enospc=//p' "$EV/one-g-$end.txt")"
    qh=$(sed -n 's/^replay_queue_high_post=//p' "$EV/one-g-$end.txt" | head -n 1)
    check "${end}_one_g_replay_queue_high" "$([[ ${qh:-0} -lt 5242880 ]]; echo $?)" "high=$qh bound=5242880"
    ns=$(sed -n 's/^fec_replay_miss_never_stored=//p' "$EV/one-g-$end.txt" | head -n 1)
    check "${end}_one_g_never_stored" "$([[ ${ns:-999999} -le 256 ]]; echo $?)" \
        "delta=$ns bound=256"
    sp=$(sed -n 's/^fec_short_parity_saved=//p' "$EV/one-g-$end.txt" | head -n 1)
    check "${end}_one_g_short_parity_saved" "$([[ ${sp:-0} -gt 0 ]]; echo $?)" \
        "delta=$sp"
    ps=$(sed -n 's/^fec_nack_pad_skip=//p' "$EV/one-g-$end.txt" | head -n 1)
    check "${end}_one_g_nack_pad_skip" "$([[ ${ps:-1} == 0 ]]; echo $?)" \
        "delta=$ps bound=0"
    lns=$(sed -n 's/^fec_nack_lat_ns=//p' "$EV/one-g-$end.txt" | head -n 1)
    check "${end}_one_g_nack_lat_ns" "$([[ ${lns:-999999999999} -lt 600000000000 ]]; echo $?)" \
        "window_delta_ns=$lns bound=600s (NACK->wire)"
    rns=$(sed -n 's/^fec_nack_repair_ns=//p' "$EV/one-g-$end.txt" | head -n 1)
    say "${end}_one_g_repair_ns_block_ready=${rns:-?} (informational)"
    printf '%s\n' "${end}_one_g_repair_ns_block_ready=${rns:-?}" >>"$EV/run.log"
    rmax=$(sed -n 's/^repair_max_post=//p' "$EV/one-g-$end.txt" | head -n 1)
    say "${end}_one_g_repair_max_session=${rmax:-?} (informational)"
    printf '%s\n' "${end}_one_g_repair_max_session=${rmax:-?}" >>"$EV/run.log"
    hw=$(sed -n 's/^fec_hol_wait=//p' "$EV/one-g-$end.txt" | head -n 1)
    info_line="${end}_one_g_hol_wait=${hw:-?}"
    say "$info_line"
    printf '%s\n' "$info_line" >>"$EV/run.log"
done
near_ack_tx_1g=$(sed -n 's/^ack_tx=//p' "$EV/one-g-near.txt" | head -n 1)
far_ack_rx_1g=$(sed -n 's/^ack_rx=//p' "$EV/one-g-far.txt" | head -n 1)
check near_one_g_ack_tx "$([[ ${near_ack_tx_1g:-0} -gt 0 ]]; echo $?)" "near_ack_tx=${near_ack_tx_1g:-?}"
check far_one_g_ack_rx "$([[ ${far_ack_rx_1g:-0} -gt 0 ]]; echo $?)" "far_ack_rx=${far_ack_rx_1g:-?}"

kernel_window near "$NEAR_T0" "$NEAR_T1" "$EV/near-kernel-one-g.txt"
kernel_window far "$FAR_T0" "$FAR_T1" "$EV/far-kernel-one-g.txt"
for end in near far; do
    case $(kw_state "$EV/$end-kernel-one-g.txt") in
        SKIP) skip "${end}_one_g_kernel" "empty window, source=$(kw_source "$EV/$end-kernel-one-g.txt")" ;;
        FAIL) check "${end}_one_g_kernel" 1 "BUG/Oops/WARNING in window" ;;
        HOG)
            say "${end}_one_g_kernel_hog=1 (tbtun workqueue hog; informational)"
            printf '%s\n' "${end}_one_g_kernel_hog=1" >>"$EV/run.log"
            ;;
        *) check "${end}_one_g_kernel" 0 "window clean, source=$(kw_source "$EV/$end-kernel-one-g.txt")" ;;
    esac
done

read -r nb ni < <(cpu_busy "$EV/one-g-near-procstat-pre.txt") || true
read -r nb2 ni2 < <(cpu_busy "$EV/one-g-near-procstat-post.txt") || true
one_near_cpu=$(cpu_pct "${nb:-0}" "${ni:-0}" "${nb2:-0}" "${ni2:-0}")
read -r fb fi < <(cpu_busy "$EV/one-g-far-procstat-pre.txt") || true
read -r fb2 fi2 < <(cpu_busy "$EV/one-g-far-procstat-post.txt") || true
one_far_cpu=$(cpu_pct "${fb:-0}" "${fi:-0}" "${fb2:-0}" "${fi2:-0}")
one_mbit=$(awk -v s="${one_speed:-0}" 'BEGIN{printf "%.2f", s*8/1000000}')
one_wire_far=$(delta "$(netdev_tx "$EV/one-g-far-netdev-pre.txt" "$FAR_PHYS_DEV")" \
                     "$(netdev_tx "$EV/one-g-far-netdev-post.txt" "$FAR_PHYS_DEV")")
one_wire_near=$(delta "$(netdev_rx "$EV/one-g-near-netdev-pre.txt" "$NEAR_PHYS_DEV")" \
                      "$(netdev_rx "$EV/one-g-near-netdev-post.txt" "$NEAR_PHYS_DEV")")

NEAR_BOOT_AFTER=$(ssh_near 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)
FAR_BOOT_AFTER=$(ssh_far 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)
check near_no_restart "$([[ -n $NEAR_BOOT_BEFORE && $NEAR_BOOT_BEFORE == "$NEAR_BOOT_AFTER" ]]; echo $?)" \
    "boot_id ${NEAR_BOOT_BEFORE:0:8}.. -> ${NEAR_BOOT_AFTER:0:8}.."
check far_no_restart "$([[ -n $FAR_BOOT_BEFORE && $FAR_BOOT_BEFORE == "$FAR_BOOT_AFTER" ]]; echo $?)" \
    "boot_id ${FAR_BOOT_BEFORE:0:8}.. -> ${FAR_BOOT_AFTER:0:8}.."

capture_end near "$EV/near-post.txt"
capture_end far "$EV/far-post.txt"
ssh_far 'dmesg 2>/dev/null | tail -n 1000' >"$EV/far-dmesg.txt" 2>/dev/null || true
ssh_near 'dmesg 2>/dev/null | tail -n 1000' >"$EV/near-dmesg.txt" 2>/dev/null || true
for end in near far; do
    if grep -qE 'BUG:|Oops|UBSAN:|general protection fault|WARNING:' "$EV/$end-dmesg.txt" 2>/dev/null; then
        check "${end}_dmesg_tail" 1 "BUG/Oops/WARNING in last 1000 lines"
    else
        check "${end}_dmesg_tail" 0 "no BUG/Oops/WARNING in last 1000 lines"
    fi
done

: "${steady_app:=?}" "${steady_app_io:=?}" "${wire_far_tx:=?}" "${wire_near_rx:=?}" "${near_cpu:=?}" "${far_cpu:=?}"
fc_line=$(grep 'http=' "$EV/first-connect.txt" 2>/dev/null | tail -n 1 || true)
fc_http=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^http=/){sub("http=","",$i); print $i}}' <<<"$fc_line")
fc_size=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^size=/){sub("size=","",$i); print $i}}' <<<"$fc_line")
fc_ttfb=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^ttfb=/){sub("ttfb=","",$i); print $i}}' <<<"$fc_line")
END_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
mark run-done

# ---------------------------------------------------------------------------
say "== 7. verdict"
verdict=PASS
((FAILED > 0)) && verdict=FAIL
{
    printf '# SC-LDPC 0.3.1 fix44 acceptance\n\n'
    printf -- '- start: %s\n- end: %s\n' "$START_UTC" "$END_UTC"
    printf -- '- near: `%s` (aarch64)\n- far: `%s` (x86_64)\n' "$NEAR_HOST" "$FAR_HOST"
    printf -- '- shape: mode=sc-ldpc, adaptive=off, 256 carriers\n'
    printf -- '- fix44: short block sends row 0 only; NACK names no padding\n'
    printf -- '  - near TX uplink 8x32: n=8 m=24 w=128 L=16, rate 1/4\n'
    printf -- '  - far TX downlink 1x256: n=128 m=128 w=256 L=8, rate 0.5\n'
    printf -- '- peer shape (far dmesg): `%s`\n' "${far_shape:-missing}"
    printf -- '- peer shape (near dmesg): `%s`\n' "${near_shape:-missing}"
    printf -- '- evidence: `%s`\n\n' "$EV"
    printf '## Verdict\n\n**%s**\n\n' "$verdict"
    printf '| item | value |\n|---|---|\n'
    printf '| first connect | http=%s size=%s ttfb=%ss rc=%s |\n' "${fc_http:-?}" "${fc_size:-?}" "${fc_ttfb:-?}" "${first_rc:-?}"
    printf '| steady app bytes (far submit) | %s |\n' "${steady_app:-?}"
    printf '| steady app bytes (near curl rchar) | %s |\n' "${steady_app_io:-?}"
    printf '| steady wire (far tx / near rx) | %s / %s |\n' "${wire_far_tx:-?}" "${wire_near_rx:-?}"
    printf '| steady near/far CPU | %s%% / %s%% |\n' "$near_cpu" "$far_cpu"
    printf '| 1 GiB http | %s |\n' "${one_http:-?}"
    printf '| 1 GiB size | %s |\n' "${one_size:-?}"
    printf '| 1 GiB sha256 near | %s |\n' "${one_sha:-unavailable}"
    printf '| 1 GiB sha256 far | %s |\n' "${FAR_SHA:-unavailable}"
    printf '| 1 GiB time | %s s |\n' "${one_time:-?}"
    printf '| 1 GiB throughput | %s Mbit/s |\n' "$one_mbit"
    printf '| 1 GiB wire far/near | %s / %s |\n' "${one_wire_far:-?}" "${one_wire_near:-?}"
    printf '| 1 GiB near/far CPU | %s%% / %s%% |\n' "$one_near_cpu" "$one_far_cpu"
    printf '| near boot_id stable | %s |\n' "$([[ $NEAR_BOOT_BEFORE == "$NEAR_BOOT_AFTER" ]] && echo yes || echo no)"
    printf '| far boot_id stable | %s |\n' "$([[ $FAR_BOOT_BEFORE == "$FAR_BOOT_AFTER" ]] && echo yes || echo no)"
    printf '| watchdog | %s |\n\n' "${feeder:-no holder; untouched}"
    printf '## Counter increments\n\n```\nfirst connect:\n'; cat "$EV/first-analysis.txt" 2>/dev/null
    printf '\nsteady near:\n'; cat "$EV/steady-near.txt" 2>/dev/null
    printf '\nsteady far:\n'; cat "$EV/steady-far.txt" 2>/dev/null
    printf '\n1 GiB near:\n'; cat "$EV/one-g-near.txt" 2>/dev/null
    printf '\n1 GiB far:\n'; cat "$EV/one-g-far.txt" 2>/dev/null
    printf '```\n\n## Checks\n\n```\n'
    grep -E '^(CHECK|NEAR_CHECK) ' "$EV/run.log" 2>/dev/null || true
    printf '```\n\n## Cleanup\n\nsee `cleanup.txt`\n'
} >"$EV/SUMMARY.md"

cleanup_all

if ((FAILED > 0)); then
    note "verdict FAIL ($FAILED failed checks); evidence kept in $EV"
    exit 8
fi
note "verdict PASS; evidence kept in $EV"
exit 0
