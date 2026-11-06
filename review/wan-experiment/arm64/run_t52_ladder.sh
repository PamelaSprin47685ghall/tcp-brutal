#!/usr/bin/env bash
# run_t52_ladder.sh - T5.2 carrier ladder on .65 near vs HND0 far.
#
# Runs on the workstation. Drives .65 (near) and HND0 (far) over ssh.
# The far side must already be up from deploy_hnd0.sh with carriers=256.
#
# One ladder level at a time: brings the far side up at the level's carrier
# count, brings the near side up, gates the join, runs N concurrent
# tbt_burst echo checks, samples both ends, tears the tunnel down, and moves to
# the next level. A router/.65/HND0 anomaly aborts the whole ladder.
#
# Usage:
#   review/wan-experiment/arm64/run_t52_ladder.sh <level> [<level> ...]
#
#   level names:  smoke   1 carrier, 37-byte tbt_ping, then a 1 MiB burst
#                 c8      8 carriers, 1 x 1 MiB burst, 5 s
#                 c16     16 carriers, 1 x 1 MiB burst, 5 s
#                 c32     32 carriers, 1 x 1 MiB burst, 5 s
#                 c64     64 carriers, 1 x 1 MiB burst, 5 s
#                 c128    128 carriers, 1 x 1 MiB burst, 5 s
#                 c256    256 carriers, 1 x 1 MiB burst, 5 s
#                 c256x8  256 carriers, 8 x 1 MiB bursts, 5 s
#                 m8      256 carriers, 8 x 8 MiB bursts, 10 s
#                 m16     256 carriers, 8 x 16 MiB bursts, 15 s
#
# Ordering is the operator's; this script runs exactly the levels given, in
# order. smoke is the only level that uses tbt_ping; the rest use tbt_burst.
#
# Per level:
#   far:   rewrite /proc/net/tbtun/config with carriers=<N>, restart echo unit
#   near:  insmod b41b259 module, ports 30000-30<2N-1>, near config carriers=<N>
#   gate:  up=<N> on both ends, parse_err/mismatch/drop counters == 0
#   run:   <procs> x tbt_burst 127.0.0.1 9001 <mib> <secs> interleaved
#   sample: both ends stats/CPU/RAM/wire before and after
#   teardown both ends
#
# Environment:
#   NEAR_HOST      ssh target for .65 (default root@192.168.123.65)
#   FAR_HOST       ssh target for HND0 (default root@45.11.1.62)
#   TBT_SRC_DIR    staged tree on .65 (default /root/tbtun-t52-arm64-b41b25970b18)
#   EVIDENCE_DIR   evidence root (default <repo>/review/wan-experiment/arm64)
#   LOAD_LIMIT     load1 ceiling on either host (default 4)
#   JOIN_TIMEOUT   seconds to wait for up=N (default 300)
#   RUN_TIMEOUT    multiplier for tbt_burst timeout (default 3)
#
# Evidence: $EVIDENCE_DIR/ladder-<UTC>-<level>/
#   pre-*.txt, joined-*.txt, post-*.txt, burst-*.txt, summary.txt, cleanup.txt
#
# Exit codes:
#   0 all requested levels passed
#   2 usage
#   3 precondition failed (ssh, arch, far module absent, stale state)
#   4 load above LOAD_LIMIT
#   5 lock timeout
#   6 burst build failed
#   7 setup failed (rule, sysctl, insmod, ports, config)
#   8 join or counter gate failed
#   9 tbt_burst returned non-zero
#  10 run finished but cleanup incomplete
#  11 anomaly abort (router/.65/HND0 health check failed)

set -euo pipefail

PROG=${0##*/}
NEAR_HOST=${NEAR_HOST:-root@192.168.123.65}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
NEAR_LOCK=/tmp/tbtun-rk3588.lock
FAR_LOCK=/tmp/tbtun-hnd0.lock
LOCK_WAIT=${LOCK_WAIT:-1800}
LOCK_HOLD_SECS=${LOCK_HOLD_SECS:-14400}
LOAD_LIMIT=${LOAD_LIMIT:-4}
JOIN_TIMEOUT=${JOIN_TIMEOUT:-300}
RUN_TIMEOUT_MULT=${RUN_TIMEOUT:-3}
TBT_SRC_DIR=${TBT_SRC_DIR:-/root/tbtun-t52-arm64-b41b25970b18}
EVIDENCE_ROOT=${EVIDENCE_DIR:-}

NEAR_PORTBASE=30000
FAR_PORTBASE=31000
NEAR_PORT=9001
FAR_PORT=9000
ECHO_PORT=5202
SYSCTL_MAX=67108864

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=4)

EX_OK=0
EX_USAGE=2
EX_PRECOND=3
EX_BUSY=4
EX_LOCK=5
EX_BUILD=6
EX_SETUP=7
EX_GATE=8
EX_RUN=9
EX_CLEAN=10
EX_ANOMALY=11

note() { printf '%s: %s\n' "$PROG" "$*" >&2; }
die() { local rc=$1; shift; printf '%s: %s\n' "$PROG" "$*" >&2; exit "$rc"; }
usage() { sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-$EX_OK}"; }

# --- level table ------------------------------------------------------------
declare -A LEVEL_CARRIERS LEVEL_PROCS LEVEL_MIB LEVEL_SECS LEVEL_PING
declare -a LEVEL_ORDER=()

level_parse() {
    local name=$1
    case $name in
        smoke)  LEVEL_CARRIERS[$name]=1   ; LEVEL_PROCS[$name]=1 ; LEVEL_MIB[$name]=1  ; LEVEL_SECS[$name]=5  ; LEVEL_PING[$name]=1 ;;
        c8)     LEVEL_CARRIERS[$name]=8   ; LEVEL_PROCS[$name]=1 ; LEVEL_MIB[$name]=1  ; LEVEL_SECS[$name]=5  ; LEVEL_PING[$name]=0 ;;
        c16)    LEVEL_CARRIERS[$name]=16  ; LEVEL_PROCS[$name]=1 ; LEVEL_MIB[$name]=1  ; LEVEL_SECS[$name]=5  ; LEVEL_PING[$name]=0 ;;
        c32)    LEVEL_CARRIERS[$name]=32  ; LEVEL_PROCS[$name]=1 ; LEVEL_MIB[$name]=1  ; LEVEL_SECS[$name]=5  ; LEVEL_PING[$name]=0 ;;
        c64)    LEVEL_CARRIERS[$name]=64  ; LEVEL_PROCS[$name]=1 ; LEVEL_MIB[$name]=1  ; LEVEL_SECS[$name]=5  ; LEVEL_PING[$name]=0 ;;
        c128)   LEVEL_CARRIERS[$name]=128 ; LEVEL_PROCS[$name]=1 ; LEVEL_MIB[$name]=1  ; LEVEL_SECS[$name]=5  ; LEVEL_PING[$name]=0 ;;
        c256)   LEVEL_CARRIERS[$name]=256 ; LEVEL_PROCS[$name]=1 ; LEVEL_MIB[$name]=1  ; LEVEL_SECS[$name]=5  ; LEVEL_PING[$name]=0 ;;
        c256x8) LEVEL_CARRIERS[$name]=256 ; LEVEL_PROCS[$name]=8 ; LEVEL_MIB[$name]=1  ; LEVEL_SECS[$name]=5  ; LEVEL_PING[$name]=0 ;;
        m8)     LEVEL_CARRIERS[$name]=256 ; LEVEL_PROCS[$name]=8 ; LEVEL_MIB[$name]=8  ; LEVEL_SECS[$name]=10 ; LEVEL_PING[$name]=0 ;;
        m16)    LEVEL_CARRIERS[$name]=256 ; LEVEL_PROCS[$name]=8 ; LEVEL_MIB[$name]=16 ; LEVEL_SECS[$name]=15 ; LEVEL_PING[$name]=0 ;;
        *) return 1 ;;
    esac
    LEVEL_ORDER+=("$name")
}

[[ $# -ge 1 ]] || usage "$EX_USAGE"
LEVELS=()
for arg in "$@"; do
    case $arg in
        -h | --help) usage "$EX_OK" ;;
        *)
            if level_parse "$arg"; then LEVELS+=("$arg"); else die "$EX_USAGE" "unknown level: $arg"; fi
            ;;
    esac
done

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1 || die "$EX_USAGE" "not a git checkout: $REPO"
EVIDENCE_ROOT=${EVIDENCE_ROOT:-$REPO/review/wan-experiment/arm64}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
EVROOT=$EVIDENCE_ROOT/ladder-$STAMP
mkdir -p "$EVROOT"

rrun_near() { ssh "${SSH_OPTS[@]}" "$NEAR_HOST" "$@"; }
rrun_far() { ssh "${SSH_OPTS[@]}" "$FAR_HOST" "$@"; }

# --- locks ------------------------------------------------------------------
NEAR_LOCK_PID=""
NEAR_LOCK_TMP=""
FAR_LOCK_PID=""
FAR_LOCK_TMP=""

lock_acquire() {
    local host=$1 lock=$2 purpose=$3 prefix=$4 reply tmp pid
    case $purpose in *"'"*) die "$EX_USAGE" "lock purpose must not contain a single quote" ;; esac
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/tbtun-lock.XXXXXX")
    mkfifo "$tmp/fifo"
    ssh "${SSH_OPTS[@]}" "$host" \
        "exec 9>>'$lock' || { echo 'cannot open lock file' >&2; exit 3; }
         flock -w $LOCK_WAIT 9 || { echo 'lock busy (another holder)' >&2; exit 4; }
         printf '%s %s %s %s\n' \$\$ \"\$(date -Is)\" \"\$(id -un)\" '$purpose' > '$lock'
         echo LOCKED
         sleep $LOCK_HOLD_SECS 9>&-" >"$tmp/fifo" 2>"$tmp/err" &
    pid=$!
    if ! read -r reply <"$tmp/fifo"; then
        note "lock: cannot acquire $lock on $host: $(tr '\n' ' ' <"$tmp/err" | cut -c1-300)"
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        rm -rf "$tmp"
        return 1
    fi
    if [[ $reply != LOCKED ]]; then
        note "lock: unexpected reply from $host: $reply"
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        rm -rf "$tmp"
        return 1
    fi
    printf -v "${prefix}_LOCK_PID" '%s' "$pid"
    printf -v "${prefix}_LOCK_TMP" '%s' "$tmp"
    note "lock: $lock held (holder line written)"
}

lock_release() {
    local prefix=$1 pid tmp
    eval "pid=\${${prefix}_LOCK_PID:-}"
    eval "tmp=\${${prefix}_LOCK_TMP:-}"
    if [[ -n $pid ]]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    if [[ -n $tmp ]]; then
        rm -rf "$tmp"
    fi
    printf -v "${prefix}_LOCK_PID" '%s' ""
    printf -v "${prefix}_LOCK_TMP" '%s' ""
}

# --- load -------------------------------------------------------------------
load1() { ssh "${SSH_OPTS[@]}" "$1" "cut -d' ' -f1 /proc/loadavg"; }

wait_load() {
    local host=$1 limit=$2 deadline l1
    deadline=$((SECONDS + 60))
    while :; do
        l1=$(load1 "$host") || return 3
        if awk -v a="$l1" -v b="$limit" 'BEGIN { exit !(a + 0 <= b + 0) }'; then
            printf '%s\n' "$l1"
            return 0
        fi
        if ((SECONDS >= deadline)); then
            note "load: $host load1=$l1 > $limit; refusing"
            return 4
        fi
        note "load: $host load1=$l1 > $limit; waiting"
        sleep 5
    done
}

# --- snapshots --------------------------------------------------------------
snapshot() {
    ssh "${SSH_OPTS[@]}" "$1" 'bash -s' >"$2" 2>&1 <<'EOS'
set -u
say() { printf '\n===%s===\n' "$1"; }
run() { say "$1"; shift; "$@" 2>&1; printf '[rc=%d]\n' "$?"; }
run uname uname -a
run date date -u +%Y-%m-%dT%H:%M:%SZ
run loadavg cat /proc/loadavg
run lsmod sh -c 'lsmod | grep -E "^tbtun" || true'
run proc_ls sh -c 'ls -l /proc/net/tbtun 2>&1 || true'
run config sh -c 'cat /proc/net/tbtun/config 2>&1 || true'
run ports sh -c 'cat /proc/net/tbtun/ports 2>&1 || true'
run tunnels sh -c 'cat /proc/net/tbtun/tunnels 2>&1 || true'
run stats sh -c 'cat /proc/net/tbtun/stats 2>&1 || true'
run dmesg sh -c 'dmesg 2>&1 | tail -n 200'
run dmesg_count sh -c 'dmesg 2>/dev/null | wc -l'
run procstat cat /proc/stat
run freem free -m
run netdev cat /proc/net/dev
run ss_listen sh -c 'ss -ltnp 2>&1 || true'
run ss_summary sh -c 'ss -s 2>&1 || true'
run iprule sh -c 'ip rule show 2>&1 || true'
run iproute sh -c 'ip route get 45.11.1.62 2>&1 || true'
run sysctl sh -c 'sysctl net.core.rmem_max net.core.wmem_max 2>&1 || true'
EOS
}

sec() { awk -v s="===$2===" '$0 == s { f = 1; next } f && /^===/ { exit } f' "$1"; }

sec_number() {
    awk -v s="===$2===" '
        $0 == s { f = 1; next }
        f && /^===/ { exit }
        f && !done {
            v = $0; gsub(/[^0-9]/, "", v)
            if (v != "") { print v; done = 1 }
        }' "$1"
}

stat_field_text() {
    printf '%s\n' "$1" | awk -v k="tunnel 1 $2 " -v f="$3=" '
        index($0, k) == 1 {
            for (i = 1; i <= NF; i++) {
                if (index($i, f) == 1) {
                    v = substr($i, length(f) + 1)
                    if (v ~ /^[0-9]+$/) { print v; exit }
                }
            }
        }'
}

field() { stat_field_text "$(sec "$1" stats)" "$2" "$3"; }
live_field() { stat_field_text "$(rrun_on "$1" 'cat /proc/net/tbtun/stats')" "$2" "$3"; }
rrun_on() { ssh "${SSH_OPTS[@]}" "$1" "$2"; }

module_loaded_on() { ssh "${SSH_OPTS[@]}" "$1" 'lsmod | grep -q "^tbtun "'; }
listening_on() { [[ -n $(rrun_on "$1" "ss -lntH 'sport = :$2'") ]]; }

# --- tunnel config ----------------------------------------------------------
near_cfg_for() {
    local carriers=$1 portblock=$2
    printf 'tunnel 1 role=near listen=127.0.0.1:%s peer=45.11.1.62:%s n=4 m=4 fec=on mode=grouped w=0 carriers=%s crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=%s nonce=00112233445566778899aabbccddeeff\n' \
        "$NEAR_PORT" "$FAR_PORT" "$carriers" "$portblock"
}

far_cfg_for() {
    local carriers=$1 portblock=$2
    printf 'tunnel 1 role=far listen=0.0.0.0:%s target=127.0.0.1:%s n=4 m=4 fec=on mode=grouped w=0 carriers=%s crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=%s nonce=00112233445566778899aabbccddeeff\n' \
        "$FAR_PORT" "$ECHO_PORT" "$carriers" "$portblock"
}

# --- per-level state --------------------------------------------------------
NEAR_RULE_ADDED=0
NEAR_SYSCTL_SAVED=0
NEAR_MODULE_LOADED=0
FAR_TOUCHED=0
OLD_RMEM=""
OLD_WMEM=""
CLEAN_FAIL=0
CUR_EV=""

cleanup_level() {
    local rc=$?
    trap - EXIT INT TERM HUP
    set +e
    if ((NEAR_MODULE_LOADED)); then
        rrun_near 'rmmod tbtun' || { note "cleanup: rmmod on $NEAR_HOST failed"; CLEAN_FAIL=1; }
    fi
    if ((NEAR_RULE_ADDED)); then
        rrun_near 'ip rule del to 45.11.1.62/32 lookup main pref 100' || { note "cleanup: rule delete on $NEAR_HOST failed"; CLEAN_FAIL=1; }
    fi
    if ((NEAR_SYSCTL_SAVED)); then
        rrun_near "sysctl -q -w net.core.rmem_max=$OLD_RMEM net.core.wmem_max=$OLD_WMEM" || { note "cleanup: sysctl restore on $NEAR_HOST failed"; CLEAN_FAIL=1; }
    fi
    if ((FAR_TOUCHED)); then
        rrun_far "systemctl stop tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
        rrun_far "systemctl reset-failed tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
        if module_loaded_on "$FAR_HOST"; then
            rrun_far 'rmmod tbtun' || { note "cleanup: rmmod on $FAR_HOST failed"; CLEAN_FAIL=1; }
        fi
    fi
    if [[ -n $CUR_EV ]]; then
        {
            printf 'near_module_loaded=%s\n' "$(module_loaded_on "$NEAR_HOST" && echo yes || echo no)"
            printf 'far_module_loaded=%s\n' "$(module_loaded_on "$FAR_HOST" && echo yes || echo no)"
            printf 'near_proc=%s\n' "$(rrun_near 'test -d /proc/net/tbtun && echo present || echo absent')"
            printf 'far_proc=%s\n' "$(rrun_far 'test -d /proc/net/tbtun && echo present || echo absent')"
            printf 'near_listen_%s=%s\n' "$NEAR_PORT" "$(listening_on "$NEAR_HOST" "$NEAR_PORT" && echo yes || echo no)"
            printf 'far_listen_%s=%s\n' "$FAR_PORT" "$(listening_on "$FAR_HOST" "$FAR_PORT" && echo yes || echo no)"
            printf 'far_listen_%s=%s\n' "$ECHO_PORT" "$(listening_on "$FAR_HOST" "$ECHO_PORT" && echo yes || echo no)"
            printf 'near_rule=%s\n' "$(rrun_near 'ip rule show' | grep -F '45.11.1.62 lookup main' || echo absent)"
            printf 'near_sysctl=%s\n' "$(rrun_near 'sysctl -n net.core.rmem_max net.core.wmem_max' | tr '\n' ' ')"
        } >"$CUR_EV/cleanup.txt" 2>&1 || note "cleanup record incomplete"
    fi
    lock_release NEAR
    lock_release FAR
    if ((rc == 0 && CLEAN_FAIL == 1)); then
        rc=$EX_CLEAN
    fi
    set -e
    exit "$rc"
}

# --- health check -----------------------------------------------------------
health_check() { # label -> 0 ok, 11 anomaly
    local label=$1
    local near_load far_load near_route far_route
    near_load=$(load1 "$NEAR_HOST" 2>/dev/null || echo '?')
    far_load=$(load1 "$FAR_HOST" 2>/dev/null || echo '?')
    near_route=$(rrun_near 'ip route get 45.11.1.62' 2>/dev/null || echo '?')
    far_route=$(rrun_far 'ip route get 45.11.1.62' 2>/dev/null || echo '?')
    printf '%s near_load=%s far_load=%s\n' "$label" "$near_load" "$far_load" >&2
    printf '%s near_route=%s\n' "$label" "$near_route" >&2
    printf '%s far_route=%s\n' "$label" "$far_route" >&2
    # Router health: ping the gateway from .65
    local gw_ping
    gw_ping=$(rrun_near 'ping -c 3 -W 2 192.168.123.254 2>&1 | tail -2' 2>/dev/null || echo 'ping failed')
    printf '%s gw_ping=%s\n' "$label" "$gw_ping" >&2
    if [[ $near_load == '?' || $far_load == '?' ]]; then
        note "anomaly: ssh/load failed ($label)"
        return 11
    fi
    if ! printf '%s' "$gw_ping" | grep -q 'received'; then
        note "anomaly: gateway ping failed ($label): $gw_ping"
        return 11
    fi
    if printf '%s' "$gw_ping" | grep -q '100% packet loss\|0 received'; then
        note "anomaly: gateway unreachable ($label): $gw_ping"
        return 11
    fi
    return 0
}

# --- per-level run ----------------------------------------------------------
run_level() {
    local level=$1
    local carriers=${LEVEL_CARRIERS[$level]}
    local procs=${LEVEL_PROCS[$level]}
    local mib=${LEVEL_MIB[$level]}
    local secs=${LEVEL_SECS[$level]}
    local ping=${LEVEL_PING[$level]}

    CUR_EV=$EVROOT/$level
    mkdir -p "$CUR_EV"
    note "level $level: carriers=$carriers procs=$procs mib=$mib secs=$secs ping=$ping"

    # health check before each level
    health_check "pre-$level" || return $EX_ANOMALY

    # far: rewrite config with carriers=N, restart echo
    local far_portblock near_portblock
    far_portblock=$(printf '%d-%d' "$FAR_PORTBASE" $((FAR_PORTBASE + 2 * carriers - 1)))
    near_portblock=$(printf '%d-%d' "$NEAR_PORTBASE" $((NEAR_PORTBASE + 2 * carriers - 1)))

    # far side must be loaded already (deploy_hnd0.sh); rewrite config only
    if ! module_loaded_on "$FAR_HOST"; then
        die "$EX_PRECOND" "tbtun not loaded on $FAR_HOST; run deploy_hnd0.sh first"
    fi
    local far_cfg
    far_cfg=$(far_cfg_for "$carriers" "$far_portblock")
    rrun_far "printf '%s' '$far_cfg' > /proc/net/tbtun/config" || die "$EX_SETUP" "far config write failed"
    # restart echo to clear any stale connections
    rrun_far "systemctl stop tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
    rrun_far "systemctl reset-failed tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
    rrun_far "systemd-run --no-block --unit=tbt-echo-$ECHO_PORT --collect --property=StandardOutput=file:$TBT_SRC_DIR/tbt-echo-$ECHO_PORT.log --property=StandardError=file:$TBT_SRC_DIR/tbt-echo-$ECHO_PORT.log '$TBT_SRC_DIR/tbt_echo' $ECHO_PORT echo" \
        || die "$EX_SETUP" "systemd-run for tbt-echo-$ECHO_PORT failed"
    local i
    for i in $(seq 1 20); do
        listening_on "$FAR_HOST" "$ECHO_PORT" && break
        sleep 0.5
    done
    listening_on "$FAR_HOST" "$ECHO_PORT" || die "$EX_SETUP" "echo target not listening on 127.0.0.1:$ECHO_PORT"
    FAR_TOUCHED=1

    # near: insmod, ports, config
    local near_vm
    near_vm=$(rrun_near "modinfo -F vermagic '$TBT_SRC_DIR/tbtun.ko' 2>/dev/null || true")
    [[ -n $near_vm ]] || near_vm=$(rrun_near "modinfo '$TBT_SRC_DIR/tbtun.ko' | sed -n 's/^vermagic:[[:space:]]*//p'")
    near_vm=${near_vm%% *}
    local near_krel
    near_krel=$(rrun_near 'uname -r')
    [[ $near_vm == "$near_krel" ]] || die "$EX_PRECOND" "module vermagic '$near_vm' != $NEAR_HOST kernel '$near_krel'"

    # ip rule bypass
    local near_rules
    near_rules=$(rrun_near 'ip rule show')
    case $near_rules in
        *'45.11.1.62 lookup main'*)
            note "rule: 45.11.1.62 via main already present; leaving it"
            ;;
        *)
            rrun_near 'ip rule add to 45.11.1.62/32 lookup main pref 100' || die "$EX_SETUP" "ip rule add failed"
            NEAR_RULE_ADDED=1
            ;;
    esac
    local route dev
    route=$(rrun_near 'ip route get 45.11.1.62')
    printf '%s\n' "$route" >"$CUR_EV/route.txt"
    dev=$(printf '%s\n' "$route" | sed -n 's/.* dev \([^ ]*\).*/\1/p' | head -n 1)
    case $dev in
        Meta | tun[0-9]* | utun[0-9]* | wg[0-9]* | tailscale[0-9]*)
            die "$EX_PRECOND" "route to 45.11.1.62 still via $dev: $route" ;;
        '') die "$EX_PRECOND" "no route to 45.11.1.62: $route" ;;
    esac
    note "route: 45.11.1.62 via $dev"

    # sysctl
    OLD_RMEM=$(rrun_near 'sysctl -n net.core.rmem_max')
    OLD_WMEM=$(rrun_near 'sysctl -n net.core.wmem_max')
    NEAR_SYSCTL_SAVED=1
    rrun_near "sysctl -q -w net.core.rmem_max=$SYSCTL_MAX net.core.wmem_max=$SYSCTL_MAX" || die "$EX_SETUP" "sysctl write failed"

    # insmod near
    rrun_near "insmod '$TBT_SRC_DIR/tbtun.ko' max_carriers=256 max_tunnels=1 debug=0" || die "$EX_SETUP" "insmod failed on $NEAR_HOST"
    NEAR_MODULE_LOADED=1
    rrun_near 'test -d /proc/net/tbtun' || die "$EX_SETUP" "/proc/net/tbtun absent after insmod"

    # near ports + config
    rrun_near "printf 'ports $near_portblock\n' > /proc/net/tbtun/ports" || die "$EX_SETUP" "near ports write failed"
    local got_ports
    got_ports=$(rrun_near "grep '^ports ' /proc/net/tbtun/ports")
    [[ $got_ports == "ports $near_portblock" ]] || die "$EX_SETUP" "near ports readback '$got_ports'"

    local near_cfg
    near_cfg=$(near_cfg_for "$carriers" "$near_portblock")
    rrun_near "printf '%s' '$near_cfg' > /proc/net/tbtun/config" || die "$EX_SETUP" "near config write failed"
    local got_cfg
    got_cfg=$(rrun_near 'cat /proc/net/tbtun/config')
    printf '%s\n' "$got_cfg" >"$CUR_EV/config-near.txt"
    rrun_far 'cat /proc/net/tbtun/config' >"$CUR_EV/config-far.txt"

    # build burst tool if needed
    if ! rrun_near "test -f '$TBT_SRC_DIR/tbt_burst'"; then
        if ! rrun_near "cd '$TBT_SRC_DIR' && cc -O2 -Wall -Wextra -o tbt_burst host/tbt_burst.c" >"$CUR_EV/burst-build.log" 2>&1; then
            tail -n 40 "$CUR_EV/burst-build.log" >&2
            die "$EX_BUILD" "tbt_burst build failed on $NEAR_HOST"
        fi
    fi
    if ! rrun_near "test -f '$TBT_SRC_DIR/tbt_ping'"; then
        rrun_near "cd '$TBT_SRC_DIR' && cc -O2 -Wall -Wextra -o tbt_ping host/tbt_ping.c" >"$CUR_EV/ping-build.log" 2>&1 || true
    fi

    # pre snapshot
    note "snapshot: pre-$level"
    snapshot "$NEAR_HOST" "$CUR_EV/pre-near.txt"
    snapshot "$FAR_HOST" "$CUR_EV/pre-far.txt"
    local pre_near_dmesg pre_far_dmesg
    pre_near_dmesg=$(sec_number "$CUR_EV/pre-near.txt" dmesg_count)
    pre_far_dmesg=$(sec_number "$CUR_EV/pre-far.txt" dmesg_count)
    pre_near_dmesg=${pre_near_dmesg:-0}
    pre_far_dmesg=${pre_far_dmesg:-0}

    # join gate
    note "join: waiting up to ${JOIN_TIMEOUT}s for up=$carriers"
    local deadline=$((SECONDS + JOIN_TIMEOUT)) nup fup
    while :; do
        nup=$(live_field "$NEAR_HOST" c3 up || true)
        fup=$(live_field "$FAR_HOST" c3 up || true)
        if [[ $nup == "$carriers" && $fup == "$carriers" ]]; then
            break
        fi
        if ((SECONDS >= deadline)); then
            note "join: near up=${nup:-?} far up=${fup:-?} after ${JOIN_TIMEOUT}s"
            snapshot "$NEAR_HOST" "$CUR_EV/joined-near.txt"
            snapshot "$FAR_HOST" "$CUR_EV/joined-far.txt"
            die "$EX_GATE" "up != $carriers on both ends"
        fi
        sleep 3
    done
    note "join: up=$carriers both ends"

    snapshot "$NEAR_HOST" "$CUR_EV/joined-near.txt"
    snapshot "$FAR_HOST" "$CUR_EV/joined-far.txt"

    # counter gate
    local gate_fail=0
    for end in near far; do
        local f=$CUR_EV/joined-$end.txt
        [[ $(field "$f" c3 up) == "$carriers" ]] || { note "gate: $end up=$(field "$f" c3 up)"; gate_fail=1; }
        while read -r linekey fld; do
            local v
            v=$(field "$f" "$linekey" "$fld")
            [[ $v == 0 ]] || { note "gate: $end $fld=${v:-missing}"; gate_fail=1; }
        done <<'EOF'
c8 http_parse_err
c8 profile_mismatch
c9 fec_drop_window
c9 fec_drop_crypto
c9 fec_short_unknown
EOF
        local cf cr
        cf=$(field "$f" c3 connect_fail)
        cr=$(field "$f" c3 connect_retry)
        [[ $cf == 0 ]] || note "warn: $end connect_fail=$cf"
        [[ $cr == 0 ]] || note "warn: $end connect_retry=$cr"
    done
    ((gate_fail == 0)) || die "$EX_GATE" "counter gate failed; evidence: $CUR_EV"

    # smoke: 37B ping first
    if ((ping == 1)); then
        note "smoke: tbt_ping 37B"
        local ping_rc=0
        rrun_near "cd '$TBT_SRC_DIR' && timeout 20 ./tbt_ping 127.0.0.1 $NEAR_PORT 37 5 0" >"$CUR_EV/ping-37B.txt" 2>&1 || ping_rc=$?
        note "smoke: ping rc=$ping_rc"
        cat "$CUR_EV/ping-37B.txt" >&2
        if ((ping_rc != 0)); then
            die "$EX_RUN" "37B ping failed rc=$ping_rc"
        fi
    fi

    # burst: N concurrent tbt_burst
    local burst_timeout=$((secs * RUN_TIMEOUT_MULT + 120))
    note "burst: $procs x tbt_burst 127.0.0.1 $NEAR_PORT $mib $secs interleaved (timeout ${burst_timeout}s)"
    local burst_rc=0
    rrun_near "cd '$TBT_SRC_DIR' && rm -f burst-*.log; for i in \$(seq 1 $procs); do (timeout $burst_timeout ./tbt_burst 127.0.0.1 $NEAR_PORT $mib $secs interleaved > burst-\$i.log 2>&1; echo \$? > burst-\$i.rc) & done; wait; cat burst-*.rc | sort -u | tr '\n' ' '; echo" >"$CUR_EV/burst-rc.txt" 2>&1 || burst_rc=$?
    # fetch individual logs
    for i in $(seq 1 $procs); do
        rrun_near "cat '$TBT_SRC_DIR/burst-$i.log' 2>/dev/null" >"$CUR_EV/burst-$i.log" 2>&1 || true
        rrun_near "cat '$TBT_SRC_DIR/burst-$i.rc' 2>/dev/null" >"$CUR_EV/burst-$i.rc" 2>&1 || true
    done
    rrun_near "rm -f '$TBT_SRC_DIR'/burst-*.log '$TBT_SRC_DIR'/burst-*.rc" 2>/dev/null || true
    note "burst: rc summary: $(cat "$CUR_EV/burst-rc.txt" 2>/dev/null | tr '\n' ' ')"

    # post snapshot
    note "snapshot: post-$level"
    snapshot "$NEAR_HOST" "$CUR_EV/post-near.txt"
    snapshot "$FAR_HOST" "$CUR_EV/post-far.txt"
    local post_near_dmesg post_far_dmesg
    post_near_dmesg=$(sec_number "$CUR_EV/post-near.txt" dmesg_count)
    post_far_dmesg=$(sec_number "$CUR_EV/post-far.txt" dmesg_count)
    post_near_dmesg=${post_near_dmesg:-0}
    post_far_dmesg=${post_far_dmesg:-0}

    # health check after
    health_check "post-$level" || return $EX_ANOMALY

    # summary
    {
        printf 'level=%s carriers=%s procs=%s mib=%s secs=%s ping=%s\n' "$level" "$carriers" "$procs" "$mib" "$secs" "$ping"
        printf 'near_load1=%s far_load1=%s\n' "$(load1 "$NEAR_HOST")" "$(load1 "$FAR_HOST")"
        printf 'dmesg_delta_near=%s dmesg_delta_far=%s\n' "$((post_near_dmesg - pre_near_dmesg))" "$((post_far_dmesg - pre_far_dmesg))"
        printf '\n'
        for end in near far; do
            local f=$CUR_EV/post-$end.txt
            for fld in up connect_fail connect_retry data_tx tx_ack rx_ack retx_bytes reorder_overflow reorder_max_depth; do
                printf '%s.c3.%s=%s\n' "$end" "$fld" "$(field "$f" c3 "$fld")"
            done
            for fld in http_req http_resp http_parse_err http_status_err profile_mismatch pad_tx drip_tx; do
                printf '%s.c8.%s=%s\n' "$end" "$fld" "$(field "$f" c8 "$fld")"
            done
            for fld in fec_short_tx fec_short_unknown fec_stream_refused fec_drop_window fec_drop_crypto fec_proto_fail; do
                printf '%s.c9.%s=%s\n' "$end" "$fld" "$(field "$f" c9 "$fld")"
            done
            for fld in streams_active n_streams n_pending; do
                printf '%s.streams.%s=%s\n' "$end" "$fld" "$(field "$f" streams "$fld")"
            done
        done
    } >"$CUR_EV/summary.txt" 2>&1 || note "summary incomplete"

    # check burst results
    local all_rc
    all_rc=$(cat "$CUR_EV"/burst-*.rc 2>/dev/null | sort -u | tr '\n' ' ')
    note "burst rc summary: $all_rc"
    if printf '%s' "$all_rc" | grep -qv '^0$'; then
        die "$EX_RUN" "burst returned non-zero: $all_rc"
    fi

    return 0
}

# --- main -------------------------------------------------------------------
# preconditions
if ! rrun_near true >/dev/null 2>&1; then
    die "$EX_PRECOND" "ssh $NEAR_HOST failed"
fi
if ! rrun_far true >/dev/null 2>&1; then
    die "$EX_PRECOND" "ssh $FAR_HOST failed"
fi
ARCH=$(rrun_near 'uname -m')
[[ $ARCH == aarch64 ]] || die "$EX_PRECOND" "$NEAR_HOST is $ARCH, not aarch64"

# locks for whole ladder
lock_acquire "$NEAR_HOST" "$NEAR_LOCK" "t52-arm64 ladder ${LEVELS[*]}" NEAR || die "$EX_LOCK" "lock busy: $NEAR_LOCK on $NEAR_HOST"
lock_acquire "$FAR_HOST" "$FAR_LOCK" "t52-arm64 ladder ${LEVELS[*]}" FAR || die "$EX_LOCK" "lock busy: $FAR_LOCK on $FAR_HOST"

NEAR_LOAD=$(wait_load "$NEAR_HOST" "$LOAD_LIMIT" 0) || exit $?
FAR_LOAD=$(wait_load "$FAR_HOST" "$LOAD_LIMIT" 0) || exit $?
note "load: near=$NEAR_LOAD far=$FAR_LOAD (limit $LOAD_LIMIT)"

# far must be deployed
if ! module_loaded_on "$FAR_HOST"; then
    die "$EX_PRECOND" "tbtun not loaded on $FAR_HOST; run deploy_hnd0.sh b41b259 - first"
fi
FAR_CFG=$(rrun_far 'cat /proc/net/tbtun/config')
for want in 'carriers=256' 'profile=http-kal' 'rx_reorder_bytes=8388608' 'portblock=31000-31511' 'role=far'; do
    case " $FAR_CFG " in *" $want "*) ;; *) die "$EX_PRECOND" "far config lacks $want: $FAR_CFG" ;; esac
done
listening_on "$FAR_HOST" "$FAR_PORT" || die "$EX_PRECOND" "far not listening on $FAR_PORT"
listening_on "$FAR_HOST" "$ECHO_PORT" || die "$EX_PRECOND" "echo not listening on $ECHO_PORT"

# near must be clean
if module_loaded_on "$NEAR_HOST"; then
    die "$EX_PRECOND" "tbtun already loaded on $NEAR_HOST; unload first"
fi
if listening_on "$NEAR_HOST" "$NEAR_PORT"; then
    die "$EX_PRECOND" "port $NEAR_PORT already listens on $NEAR_HOST"
fi

# staged tree on .65
rrun_near "test -s '$TBT_SRC_DIR/tbtun.ko'" || die "$EX_PRECOND" "no module at $TBT_SRC_DIR/tbtun.ko"

# run levels
FAILED=0
for level in "${LEVEL_ORDER[@]}"; do
    if ! run_level "$level"; then
        local rc=$?
        note "level $level failed rc=$rc"
        FAILED=$rc
        break
    fi
done

if ((FAILED != 0)); then
    exit "$FAILED"
fi
exit "$EX_OK"