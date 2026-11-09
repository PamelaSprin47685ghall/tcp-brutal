#!/usr/bin/env bash
# verify-0.3.0.sh - SC-LDPC 0.3.0 real-hardware acceptance.
#
# Historical 0.3.0 check. The default near address follows the current near
# (.85); run records under results/ still name the .82 deployment. Pass
# NEAR_HOST to reproduce an old run.
#
# The build agent has already deployed the module and joined the session.
# This script judges that session: configuration, first connect, 256-carrier
# steady state, then one 1 GiB single-stream transfer. It is single shot: a
# failed step is recorded, evidence is captured, and the run is not retried.
#
# Shape under test (0.3.0, mode=sc-ldpc, adaptive=off):
#   near TX uplink:   n=8   m=24  carriers=256 w=128 L=16  -> 8x32, rate 1/4
#   far  TX downlink: n=128 m=128 carriers=256 w=256 L=8   -> 1x256, rate 0.5
# The receiver installs the sender's INIT shape, so the two configs differ.
#
# Modes
#   --run       full run (default)
#   --status    read-only: print both configs and the c3/c13 lines
#   --cleanup   stop the far source, remove the near ip rule, rmmod both ends
#
# Exit codes
#   0 PASS   2 usage   3 preflight   4 setup   5 first-connect
#   6 steady    7 one-g   8 checks
set -uo pipefail

PROG=${0##*/}
MODE=run
NEAR_HOST=${NEAR_HOST:-root@192.168.123.85}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
FAR_IP=${FAR_IP:-45.11.1.62}
NEAR_PORT=${NEAR_PORT:-9001}
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
KEEP=${KEEP:-0}

EV=${EV:-}
FAILED=0
START_UTC=
END_UTC=
NEAR_BOOT_BEFORE=
FAR_BOOT_BEFORE=
FAR_SHA=
FAR_SIZE=
NEAR_PHYS_DEV=
FAR_PHYS_DEV=

say() { printf '%s\n' "$*"; }
note() { printf '%s %s\n' "$(date -u +%H:%M:%SZ)" "$*" | tee -a "${EV:-/dev/null}/run.log" >&2; }
mark() { printf '%s\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$1" | tee -a "${EV:-/dev/null}/timeline.txt" >&2; }

usage() {
    cat <<EOF
usage: $PROG [--run|--status|--cleanup] [--keep]

Environment:
  NEAR_HOST=root@192.168.123.85  FAR_HOST=root@45.11.1.62
  WARMUP=20 MEASURE=60 ONE_G_MAX=7200
  KEEP=0    1: leave the deployment loaded/configured after the run
  EVIDENCE_ROOT=<repo>/review/wan-experiment/arm64
EOF
}

while (($#)); do
    case $1 in
        --run) MODE=run ;;
        --status) MODE=status ;;
        --cleanup) MODE=cleanup ;;
        --keep) KEEP=1 ;;
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

gaps_field() {
    awk '/^gaps_total=/ { sub("gaps_total=", ""); print; exit }' "$1" 2>/dev/null
}

cfg_has() { # file token
    grep -qE "(^|[[:space:]])$2([[:space:]]|$)" "$1" 2>/dev/null
}

num() { case ${1:-} in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }

delta() { # before after
    if num "${1:-}" && num "${2:-}"; then printf '%s' "$(( $2 - $1 ))"; else printf '?'; fi
}

netdev_tx() { # /proc/net/dev snapshot, dev
    awk -v d="$2:" '$1 == d { print $10; exit }' "$1" 2>/dev/null
}

cpu_busy() { # /proc/stat snapshot -> "busy idle"
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
s ip_route; ip route 2>/dev/null || true
s netdev; cat /proc/net/dev 2>/dev/null || true
s meminfo; awk '/^(MemTotal|MemAvailable|MemFree):/{print}' /proc/meminfo 2>/dev/null || true
s slab; grep -E '^tbt_(carrier|session|stream)' /proc/slabinfo 2>/dev/null || true
s dmesg; dmesg 2>/dev/null | tail -n 800 || true
EOS
}

capture_diag() { # stage
    local stage=$1
    capture_end near "$EV/diag-$stage-near.txt"
    capture_end far "$EV/diag-$stage-far.txt"
    ssh_near 'cat /proc/net/tbtun/stats' >"$EV/diag-$stage-near-stats.txt" 2>/dev/null || true
    ssh_far 'cat /proc/net/tbtun/stats' >"$EV/diag-$stage-far-stats.txt" 2>/dev/null || true
    ssh_near 'dmesg 2>/dev/null | tail -n 800' >"$EV/diag-$stage-near-dmesg.txt" 2>/dev/null || true
    ssh_far 'dmesg 2>/dev/null | tail -n 800' >"$EV/diag-$stage-far-dmesg.txt" 2>/dev/null || true
    note "diagnostics captured for stage=$stage"
}

sample() { # end prefix
    local fn=ssh_near out=$2
    [[ $1 == far ]] && fn=ssh_far
    "$fn" 'cat /proc/net/tbtun/stats' >"$EV/$out-stats.txt" 2>/dev/null || true
    "$fn" 'cat /proc/stat' >"$EV/$out-procstat.txt" 2>/dev/null || true
    "$fn" 'cat /proc/net/dev' >"$EV/$out-netdev.txt" 2>/dev/null || true
}

far_log_lines() {
    ssh_far "wc -l < '$FAR_DIR/httpd.log' 2>/dev/null || echo 0" 2>/dev/null |
        tr -d '[:space:]'
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
    if [[ -z ${body//[[:space:]]/} ]]; then
        printf 'SKIP'
    elif grep -qE 'BUG:|Oops|WARNING:|UBSAN:|general protection fault' <<<"$body"; then
        printf 'FAIL'
    else
        printf 'PASS'
    fi
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
        printf '=== near ip rule ===\n'
        ssh_near "ip rule show" 2>&1 || true
        ssh_near "ip rule del pref $BYPASS_PREF to $FAR_IP/32 lookup main 2>/dev/null ||
                  ip rule del pref $BYPASS_PREF to $FAR_IP/32 2>/dev/null || true"
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

maybe_cleanup() {
    if ((KEEP)); then
        note "KEEP=1: deployment left loaded/configured; cleanup skipped"
    else
        cleanup_all
    fi
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
# run

EV=$EVIDENCE_ROOT/sclc-verify-$(date -u +%Y%m%dT%H%M%SZ)
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
capture_end near "$EV/near-pre.txt"
capture_end far "$EV/far-pre.txt"

say "== 2. configuration"
for tok in "mode=sc-ldpc" "carriers=256" "fec=on" "adaptive=off" "crypto=on" "profile=http-kal"; do
    check "near_cfg_${tok%%=*}" "$(cfg_has "$EV/near-config.txt" "$tok"; echo $?)" "$tok"
done
for tok in "n=8" "m=24" "w=128" "L=16"; do
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

# The peer's graph as installed by the receiver (fix42 logs it).
for end in near far; do
    fn=ssh_near
    [[ $end == far ]] && fn=ssh_far
    "$fn" "dmesg 2>/dev/null | grep -F 'fec rx shape from peer'" >"$EV/$end-fec-rx-shape.txt" 2>/dev/null || true
    say "   $end rx shape: $(tail -n 1 "$EV/$end-fec-rx-shape.txt" 2>/dev/null || echo 'not logged')"
done

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
say "   near phys dev=${NEAR_PHYS_DEV:-?} far phys dev=${FAR_PHYS_DEV:-?}"

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

say "== 3. far HTTP source"
scp_far "$(dirname -- "$0")/../prod-1g/far-serve.sh" "$FAR_HOST:$REMOTE_HELPERS/far-serve.sh" ||
    { check far_upload 1 "scp far-serve.sh"; exit 4; }
scp_far "$(dirname -- "$0")/../prod-1g/far-raw-serve.py" "$FAR_HOST:$REMOTE_HELPERS/far-raw-serve.py" ||
    { check far_upload 1 "scp far-raw-serve.py"; exit 4; }
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
say "== 4. first connect (curl -m $FIRST_TMO)"
mark first-connect-start
ssh_near "rm -f /tmp/sclc-first.bin /tmp/sclc-first.curl" 2>/dev/null || true
first_rc=0
ssh_near "curl -sS -m $FIRST_TMO -o /tmp/sclc-first.bin \
    -w 'http=%{http_code} size=%{size_download} ttfb=%{time_starttransfer} time=%{time_total} speed=%{speed_download}\n' \
    http://127.0.0.1:$NEAR_PORT/ > /tmp/sclc-first.curl 2>&1" || first_rc=$?
ssh_near 'cat /tmp/sclc-first.curl' >"$EV/first-connect.txt" 2>/dev/null || true
ssh_near 'stat -c %s /tmp/sclc-first.bin 2>/dev/null || echo 0' >"$EV/first-size.txt" 2>/dev/null || true
cat "$EV/first-connect.txt" >&2
first_line=$(head -n 1 "$EV/first-connect.txt" 2>/dev/null || true)
first_http=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^http=/){sub("http=","",$i); print $i}}' <<<"$first_line")
first_size=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^size=/){sub("size=","",$i); print $i}}' <<<"$first_line")
first_ttfb=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^ttfb=/){sub("ttfb=","",$i); print $i}}' <<<"$first_line")
first_file=$(tr -d '[:space:]' <"$EV/first-size.txt" 2>/dev/null || true)
check first_curl_rc "$([ "$first_rc" -eq 0 ]; echo $?)" "rc=$first_rc"
check first_http "$([[ ${first_http:-000} == 200 ]]; echo $?)" "http=${first_http:-000}"
check first_body "$([[ ${first_size:-0} -gt 0 && ${first_size:-0} == "${first_file:-0}" ]]; echo $?)" \
    "size_download=${first_size:-0} file=${first_file:-0}"
check first_ttfb "$([[ -n ${first_ttfb:-} ]]; echo $?)" "ttfb=${first_ttfb:-?}s"
if ((first_rc != 0 || ${first_size:-0} <= 0)); then
    capture_diag first-connect
    note "first connect FAIL"
    # Evidence stays; the run is not retried.
    maybe_cleanup
    exit 5
fi
mark first-connect-done

# ---------------------------------------------------------------------------
say "== 5. steady state (warmup ${WARMUP}s + measure ${MEASURE}s)"
mark steady-start
# One stream, repeatedly refetched: a single 1 GiB GET can finish inside the
# warmup on a fast path. The far sender's log gives the exact application
# bytes that crossed the tunnel in the measurement window.
ssh_near "setsid sh -c 'while :; do curl -sS -m $STEADY_TMO -o /dev/null http://127.0.0.1:$NEAR_PORT/ || break; sleep 0.2; done' </dev/null >/dev/null 2>&1 &" || true
sleep "$WARMUP"
sample near steady-a-near
sample far steady-a-far
far_lines_a=$(far_log_lines)
sleep "$MEASURE"
sample near steady-b-near
sample far steady-b-far
far_lines_b=$(far_log_lines)
ssh_near "pkill -f 'curl.*127[.]0[.]0[.]1:$NEAR_PORT' 2>/dev/null || true" || true
sleep 1
steady_app=$(ssh_far "tail -n +$(( ${far_lines_a:-0} + 1 )) '$FAR_DIR/httpd.log' 2>/dev/null" |
    awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^sent=/) { sub("sent=", "", $i); s += $i } } END { print s + 0 }')
printf 'far_log_lines_a=%s\nfar_log_lines_b=%s\napp_bytes=%s\n' \
    "${far_lines_a:-?}" "${far_lines_b:-?}" "${steady_app:-?}" >"$EV/steady-app.txt"
wire_a=$(netdev_tx "$EV/steady-a-near-netdev.txt" "$NEAR_PHYS_DEV")
wire_b=$(netdev_tx "$EV/steady-b-near-netdev.txt" "$NEAR_PHYS_DEV")
wire=$(delta "${wire_a:-0}" "${wire_b:-0}")

read -r nb ni < <(cpu_busy "$EV/steady-a-near-procstat.txt") || true
read -r nb2 ni2 < <(cpu_busy "$EV/steady-b-near-procstat.txt") || true
near_cpu=$(cpu_pct "${nb:-0}" "${ni:-0}" "${nb2:-0}" "${ni2:-0}")
read -r fb fi < <(cpu_busy "$EV/steady-a-far-procstat.txt") || true
read -r fb2 fi2 < <(cpu_busy "$EV/steady-b-far-procstat.txt") || true
far_cpu=$(cpu_pct "${fb:-0}" "${fi:-0}" "${fb2:-0}" "${fi2:-0}")

steady_report() {
    local end=$1
    # Expand $end only after it is bound. `local end=$1 pre=...$end...` uses
    # the caller's $end (the previous loop's value), so both reports read the
    # same side's stats; that is the fix42 steady-state false failure.
    local pre=$EV/steady-a-$end-stats.txt post=$EV/steady-b-$end-stats.txt
    {
        printf 'app_bytes=%s\n' "$steady_app"
        printf 'wire_bytes=%s\n' "$wire"
        printf 'wire_over_app_ppm=%s\n' "$(awk -v w="${wire:-0}" -v a="${steady_app:-0}" 'BEGIN{if(a>0) printf "%d", w*1000000/a; else print "?"}')"
        printf 'near_cpu_busy_pct=%s\n' "$near_cpu"
        printf 'far_cpu_busy_pct=%s\n' "$far_cpu"
        for f in fec_outq_drop fec_outq_high_wait fec_init_bad fec_epoch_bad \
            fec_nack_tx fec_nack_rx fec_nack_retx fec_nack_drop \
            fec_replay_hit fec_replay_miss fec_replay_miss_never_stored \
            fec_reset_tcp_failed fec_reset_user fec_reset_hole \
            fec_nack_repair_ns fec_nack_repair_max_ns fec_outq_max \
            fec_decode_blocks fec_decode_recovered fec_decode_nomem \
            fec_deliver_bad fec_epoch_conflict fec_marker_conflict; do
            printf '%s=%s\n' "$f" "$(delta "$(stat_field "$pre" c11 "$f")" "$(stat_field "$post" c11 "$f")")"
        done
        printf 'holes_unrecoverable=%s\n' "$(delta "$(stat_field "$pre" c6 holes_unrecoverable)" "$(stat_field "$post" c6 holes_unrecoverable)")"
        printf 'holes_declared=%s\n' "$(delta "$(stat_field "$pre" c6 holes_declared)" "$(stat_field "$post" c6 holes_declared)")"
        printf 'holes_repaired=%s\n' "$(delta "$(stat_field "$pre" c6 holes_repaired)" "$(stat_field "$post" c6 holes_repaired)")"
        printf 'stream_reset_tcp_failed=%s\n' "$(delta "$(stat_field "$pre" c3 stream_reset_tcp_failed)" "$(stat_field "$post" c3 stream_reset_tcp_failed)")"
        printf 'stream_reset_user=%s\n' "$(delta "$(stat_field "$pre" c3 stream_reset_user)" "$(stat_field "$post" c3 stream_reset_user)")"
        printf 'data_tx=%s\n' "$(delta "$(stat_field "$pre" c3 data_tx)" "$(stat_field "$post" c3 data_tx)")"
        printf 'retx_bytes=%s\n' "$(delta "$(stat_field "$pre" c3 retx_bytes)" "$(stat_field "$post" c3 retx_bytes)")"
        printf 'gaps_total=%s\n' "$(delta "$(gaps_field "$pre")" "$(gaps_field "$post")")"
        printf 'up_post=%s\n' "$(stat_field "$post" c3 up)"
        printf 'outq_depth_post=%s\n' "$(stat_field "$post" c11 fec_outq_depth)"
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
for end in near far; do
    check "${end}_steady_up" "$([[ $(stat_field "$EV/steady-b-$end-stats.txt" c3 up) == 256 ]]; echo $?)" \
        "up=$(stat_field "$EV/steady-b-$end-stats.txt" c3 up)"
    check "${end}_steady_outq_drop" "$(steady_zero "$end" fec_outq_drop; echo $?)" \
        "delta=$(sed -n 's/^fec_outq_drop=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_init_bad" "$(steady_zero "$end" fec_init_bad; echo $?)" \
        "delta=$(sed -n 's/^fec_init_bad=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_epoch_bad" "$(steady_zero "$end" fec_epoch_bad; echo $?)" \
        "delta=$(sed -n 's/^fec_epoch_bad=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_reset_hole" "$(steady_zero "$end" fec_reset_hole; echo $?)" \
        "delta=$(sed -n 's/^fec_reset_hole=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_reset_tcp" "$(steady_zero "$end" fec_reset_tcp_failed; echo $?)" \
        "delta=$(sed -n 's/^fec_reset_tcp_failed=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_reset_user" "$(steady_zero "$end" fec_reset_user; echo $?)" \
        "delta=$(sed -n 's/^fec_reset_user=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_holes_unrec" "$(steady_zero "$end" holes_unrecoverable; echo $?)" \
        "delta=$(sed -n 's/^holes_unrecoverable=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_stream_reset" "$(steady_zero "$end" stream_reset_tcp_failed; echo $?)" \
        "delta=$(sed -n 's/^stream_reset_tcp_failed=//p' "$EV/steady-$end.txt")"
    check "${end}_steady_deliver_bad" "$(steady_zero "$end" fec_deliver_bad; echo $?)" \
        "delta=$(sed -n 's/^fec_deliver_bad=//p' "$EV/steady-$end.txt")"
done
check steady_app_bytes "$([[ ${steady_app:-0} -gt 0 ]]; echo $?)" "app=${steady_app:-?}"
check steady_wire_bytes "$([[ ${wire:-0} -gt 0 ]]; echo $?)" "wire=${wire:-?} dev=${NEAR_PHYS_DEV:-?}"

kernel_window near "$START_EPOCH" "$(date +%s)" "$EV/near-kernel-steady.txt"
kernel_window far "$START_EPOCH" "$(date +%s)" "$EV/far-kernel-steady.txt"
for end in near far; do
    case $(kw_state "$EV/$end-kernel-steady.txt") in
        SKIP) skip "${end}_steady_kernel" "empty window, source=$(kw_source "$EV/$end-kernel-steady.txt")" ;;
        FAIL) check "${end}_steady_kernel" 1 "BUG/Oops/WARNING in window" ;;
        *) check "${end}_steady_kernel" 0 "window clean, source=$(kw_source "$EV/$end-kernel-steady.txt")" ;;
    esac
done

if ((FAILED)); then
    capture_diag steady
    note "steady state FAIL; 1 GiB not attempted"
    maybe_cleanup
    exit 6
fi
mark steady-done

# ---------------------------------------------------------------------------
say "== 6. one 1 GiB single stream (max ${ONE_G_MAX}s)"
mark one-g-start
ssh_near 'mkdir -p /root/sclc-verify; rm -f /root/sclc-verify/sclc-1g.bin /root/sclc-verify/sclc-1g.curl' 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/stats' >"$EV/one-g-far-pre.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/stats' >"$EV/one-g-near-pre.txt" 2>/dev/null || true
ssh_near 'cat /proc/stat' >"$EV/one-g-near-procstat-pre.txt" 2>/dev/null || true
ssh_far 'cat /proc/stat' >"$EV/one-g-far-procstat-pre.txt" 2>/dev/null || true
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
ssh_near 'cat /root/sclc-verify/sclc-1g.curl' >"$EV/one-g-curl.txt" 2>/dev/null || true
ssh_near 'stat -c %s /root/sclc-verify/sclc-1g.bin 2>/dev/null || echo 0' >"$EV/one-g-size.txt" 2>/dev/null || true
ssh_near_long 300 'sha256sum /root/sclc-verify/sclc-1g.bin 2>/dev/null | awk "{print \$1}"' >"$EV/one-g-near-sha.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/stats' >"$EV/one-g-far-post.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/stats' >"$EV/one-g-near-post.txt" 2>/dev/null || true
ssh_near 'cat /proc/stat' >"$EV/one-g-near-procstat-post.txt" 2>/dev/null || true
ssh_far 'cat /proc/stat' >"$EV/one-g-far-procstat-post.txt" 2>/dev/null || true
mark one-g-done "rc=$one_rc"

one_line=$(head -n 1 "$EV/one-g-curl.txt" 2>/dev/null || true)
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
        for f in fec_outq_drop fec_outq_high_wait fec_init_bad fec_epoch_bad \
            fec_nack_tx fec_nack_rx fec_nack_retx fec_nack_drop \
            fec_replay_hit fec_replay_miss fec_replay_miss_never_stored \
            fec_reset_tcp_failed fec_reset_user fec_reset_hole \
            fec_nack_repair_max_ns fec_outq_max \
            fec_decode_blocks fec_decode_recovered fec_decode_nomem \
            fec_deliver_bad fec_epoch_conflict fec_marker_conflict; do
            printf '%s=%s\n' "$f" "$(delta "$(stat_field "$pre" c11 "$f")" "$(stat_field "$post" c11 "$f")")"
        done
        printf 'holes_unrecoverable=%s\n' "$(delta "$(stat_field "$pre" c6 holes_unrecoverable)" "$(stat_field "$post" c6 holes_unrecoverable)")"
        printf 'holes_declared=%s\n' "$(delta "$(stat_field "$pre" c6 holes_declared)" "$(stat_field "$post" c6 holes_declared)")"
        printf 'stream_reset_tcp_failed=%s\n' "$(delta "$(stat_field "$pre" c3 stream_reset_tcp_failed)" "$(stat_field "$post" c3 stream_reset_tcp_failed)")"
        printf 'stream_reset_user=%s\n' "$(delta "$(stat_field "$pre" c3 stream_reset_user)" "$(stat_field "$post" c3 stream_reset_user)")"
        printf 'data_tx=%s\n' "$(delta "$(stat_field "$pre" c3 data_tx)" "$(stat_field "$post" c3 data_tx)")"
        printf 'retx_bytes=%s\n' "$(delta "$(stat_field "$pre" c3 retx_bytes)" "$(stat_field "$post" c3 retx_bytes)")"
        printf 'up_post=%s\n' "$(stat_field "$post" c3 up)"
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
    check "${end}_one_g_init_bad" "$(one_zero "$end" fec_init_bad; echo $?)" \
        "delta=$(sed -n 's/^fec_init_bad=//p' "$EV/one-g-$end.txt")"
    check "${end}_one_g_epoch_bad" "$(one_zero "$end" fec_epoch_bad; echo $?)" \
        "delta=$(sed -n 's/^fec_epoch_bad=//p' "$EV/one-g-$end.txt")"
    check "${end}_one_g_reset_hole" "$(one_zero "$end" fec_reset_hole; echo $?)" \
        "delta=$(sed -n 's/^fec_reset_hole=//p' "$EV/one-g-$end.txt")"
    check "${end}_one_g_reset_tcp" "$(one_zero "$end" fec_reset_tcp_failed; echo $?)" \
        "delta=$(sed -n 's/^fec_reset_tcp_failed=//p' "$EV/one-g-$end.txt")"
    check "${end}_one_g_reset_user" "$(one_zero "$end" fec_reset_user; echo $?)" \
        "delta=$(sed -n 's/^fec_reset_user=//p' "$EV/one-g-$end.txt")"
    check "${end}_one_g_holes_unrec" "$(one_zero "$end" holes_unrecoverable; echo $?)" \
        "delta=$(sed -n 's/^holes_unrecoverable=//p' "$EV/one-g-$end.txt")"
    check "${end}_one_g_deliver_bad" "$(one_zero "$end" fec_deliver_bad; echo $?)" \
        "delta=$(sed -n 's/^fec_deliver_bad=//p' "$EV/one-g-$end.txt")"
done

kernel_window near "$NEAR_T0" "$NEAR_T1" "$EV/near-kernel-one-g.txt"
kernel_window far "$FAR_T0" "$FAR_T1" "$EV/far-kernel-one-g.txt"
for end in near far; do
    case $(kw_state "$EV/$end-kernel-one-g.txt") in
        SKIP) skip "${end}_one_g_kernel" "empty window, source=$(kw_source "$EV/$end-kernel-one-g.txt")" ;;
        FAIL) check "${end}_one_g_kernel" 1 "BUG/Oops/WARNING in window" ;;
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

NEAR_BOOT_AFTER=$(ssh_near 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)
FAR_BOOT_AFTER=$(ssh_far 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)
check near_no_restart "$([[ -n $NEAR_BOOT_BEFORE && $NEAR_BOOT_BEFORE == "$NEAR_BOOT_AFTER" ]]; echo $?)" \
    "boot_id ${NEAR_BOOT_BEFORE:0:8}.. -> ${NEAR_BOOT_AFTER:0:8}.."
check far_no_restart "$([[ -n $FAR_BOOT_BEFORE && $FAR_BOOT_BEFORE == "$FAR_BOOT_AFTER" ]]; echo $?)" \
    "boot_id ${FAR_BOOT_BEFORE:0:8}.. -> ${FAR_BOOT_AFTER:0:8}.."

capture_end near "$EV/near-post.txt"
capture_end far "$EV/far-post.txt"
ssh_far 'dmesg 2>/dev/null | tail -n 800' >"$EV/far-dmesg.txt" 2>/dev/null || true
ssh_near 'dmesg 2>/dev/null | tail -n 800' >"$EV/near-dmesg.txt" 2>/dev/null || true
for end in near far; do
    if grep -qE 'BUG:|Oops|UBSAN:|general protection fault|WARNING:' "$EV/$end-dmesg.txt" 2>/dev/null; then
        check "${end}_dmesg_tail" 1 "BUG/Oops/WARNING in last 800 lines"
    else
        check "${end}_dmesg_tail" 0 "no BUG/Oops/WARNING in last 800 lines"
    fi
done

END_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
mark run-done

# ---------------------------------------------------------------------------
say "== 7. verdict"
verdict=PASS
((FAILED > 0)) && verdict=FAIL
{
    printf '# SC-LDPC 0.3.0 acceptance\n\n'
    printf -- '- start: %s\n- end: %s\n' "$START_UTC" "$END_UTC"
    printf -- '- near: `%s` (aarch64)\n- far: `%s` (x86_64)\n' "$NEAR_HOST" "$FAR_HOST"
    printf -- '- shape: mode=sc-ldpc, adaptive=off, 256 carriers\n'
    printf -- '  - near TX uplink 8x32: n=8 m=24 w=128 L=16, rate 1/4\n'
    printf -- '  - far TX downlink 1x256: n=128 m=128 w=256 L=8, rate 0.5\n'
    printf -- '- evidence: `%s`\n\n' "$EV"
    printf '## Verdict\n\n**%s**\n\n' "$verdict"
    printf '| item | value |\n|---|---|\n'
    printf '| first connect | http=%s size=%s ttfb=%ss |\n' "${first_http:-?}" "${first_size:-?}" "${first_ttfb:-?}"
    printf '| steady app bytes | %s |\n' "${steady_app:-?}"
    printf '| steady wire bytes (%s tx) | %s |\n' "${NEAR_PHYS_DEV:-?}" "${wire:-?}"
    printf '| steady near/far CPU | %s%% / %s%% |\n' "$near_cpu" "$far_cpu"
    printf '| 1 GiB http | %s |\n' "${one_http:-?}"
    printf '| 1 GiB size | %s |\n' "${one_size:-?}"
    printf '| 1 GiB sha256 near | %s |\n' "${one_sha:-unavailable}"
    printf '| 1 GiB sha256 far | %s |\n' "${FAR_SHA:-unavailable}"
    printf '| 1 GiB time | %s s |\n' "${one_time:-?}"
    printf '| 1 GiB throughput | %s Mbit/s |\n' "$one_mbit"
    printf '| 1 GiB near/far CPU | %s%% / %s%% |\n' "$one_near_cpu" "$one_far_cpu"
    printf '| near boot_id stable | %s |\n' "$([[ $NEAR_BOOT_BEFORE == "$NEAR_BOOT_AFTER" ]] && echo yes || echo no)"
    printf '| far boot_id stable | %s |\n' "$([[ $FAR_BOOT_BEFORE == "$FAR_BOOT_AFTER" ]] && echo yes || echo no)"
    printf '| watchdog | %s |\n\n' "${feeder:-no holder; untouched}"
    printf '## Counter increments\n\n```\nsteady near:\n'; cat "$EV/steady-near.txt" 2>/dev/null
    printf '\nsteady far:\n'; cat "$EV/steady-far.txt" 2>/dev/null
    printf '\n1 GiB near:\n'; cat "$EV/one-g-near.txt" 2>/dev/null
    printf '\n1 GiB far:\n'; cat "$EV/one-g-far.txt" 2>/dev/null
    printf '```\n\n## Checks\n\n```\n'
    grep -E '^(CHECK|NEAR_CHECK) ' "$EV/run.log" 2>/dev/null || true
    printf '```\n\n## Cleanup\n\n'
    if ((KEEP)); then
        printf 'skipped (--keep): deployment left loaded/configured\n'
    else
        printf 'see `cleanup.txt`\n'
    fi
} >"$EV/SUMMARY.md"

maybe_cleanup

if ((FAILED > 0)); then
    note "verdict FAIL ($FAILED failed checks); evidence kept in $EV"
    exit 8
fi
note "verdict PASS; evidence kept in $EV"
exit 0
