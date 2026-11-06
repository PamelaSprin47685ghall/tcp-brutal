#!/usr/bin/env bash
# run_t52_65.sh - T5.2 run on the arm64 near host (.65) against HND0.
#
# Runs on the workstation. Drives .65 (near) and HND0 (far) over ssh. The far
# side must already be up from deploy_hnd0.sh; this script brings the near side
# up, gates the join, runs tbt_burst, collects both ends, and tears both ends
# down.
#
# Usage:
#   review/wan-experiment/arm64/run_t52_65.sh <mib> <seconds> <mode>
#
#   <mib>      MiB each way, passed to tbt_burst
#   <seconds>  tbt_burst socket timeout, passed through
#   <mode>     strict | interleaved. tbt_burst's own contract: strict at 8 MiB
#              and above deadlocks unless both applications raise their socket
#              buffers; this script does not preload a buffer shim, so use
#              interleaved for large transfers over the WAN.
#
# Order of operations:
#   1. take /tmp/tbtun-rk3588.lock on .65 and /tmp/tbtun-hnd0.lock on HND0
#   2. refuse if either host has load1 > 4
#   3. check both ends are in the expected state, build tbt_burst on .65,
#      pre-snapshot both ends
#   4. .65: ip rule to 45.11.1.62 via main (pref 100), rmem_max/wmem_max
#      67108864, insmod max_carriers=256 max_tunnels=1 debug=0,
#      ports 30000-30511, near config (w=0 renders w=8)
#   5. wait for up=256 on both ends, then gate the counters
#   6. tbt_burst 127.0.0.1 9001 <mib> <seconds> <mode>
#   7. post-snapshot both ends and write summary.txt
#   8. trap: rmmod both ends, del the rule if this script added it, restore
#      sysctl, stop the far echo unit, release both locks
#
# Gate (hard): up=256 on both ends, http_parse_err=0, profile_mismatch=0,
# fec_drop_window=0, fec_drop_crypto=0, fec_short_unknown=0 on both ends.
# connect_fail/connect_retry are recorded and warned about, not gated: the
# 09:05Z run joined with connect_fail=1 and up fell to 255, which the up gate
# already catches.
#
# Config source (verbatim, latest evidence):
#   review/wan-experiment/t5.2-256-http-kal/
#     t52-256-rerun-42ed56f-20261106T092730Z/near.cfg
#   rx_reorder_bytes=8388608 is the T5.2 requirement; the 09:05Z stop report
#   still carried 1048576 and is superseded.
#
# Environment:
#   NEAR_HOST      ssh target for .65 (default root@192.168.123.65)
#   FAR_HOST       ssh target for HND0 (default root@45.11.1.62)
#   TBT_SRC_DIR    staged tree on .65 (default: newest /root/tbtun-t52-arm64-*
#                  that holds a tbtun.ko)
#   LOCK_WAIT      seconds to wait for each lock (default 1800)
#   LOAD_LIMIT     load1 ceiling on either host (default 4)
#   LOAD_SETTLE    seconds to wait for load to fall before refusing (default 0)
#   JOIN_TIMEOUT   seconds to wait for up=256 (default 300)
#   EVIDENCE_DIR   evidence root (default <repo>/review/wan-experiment/arm64)
#
# Evidence: $EVIDENCE_DIR/run-<UTC>/
#   pre-near.txt, pre-far.txt, joined-near.txt, joined-far.txt,
#   post-near.txt, post-far.txt   (one sectioned dump each: config, ports,
#   tunnels, peers, stats, dmesg, /proc/stat, free -m, /proc/net/dev,
#   ss -ltnp, ss -s, ss -tin, ip rule, ip route get, sysctl)
#   burst.txt, burst-build.log, config.txt, summary.txt, cleanup.txt
#
# Exit codes:
#   0 run completed and both ends cleaned
#   2 usage
#   3 precondition failed (ssh, far not deployed, near tools, route via proxy)
#   4 load above LOAD_LIMIT
#   5 lock timeout
#   6 tbt_burst build failed
#   7 near setup failed (rule, sysctl, insmod, ports, config)
#   8 join or counter gate failed
#   9 tbt_burst returned non-zero
#  10 run finished but cleanup was incomplete

set -euo pipefail

PROG=${0##*/}
NEAR_HOST=${NEAR_HOST:-root@192.168.123.65}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
NEAR_LOCK=/tmp/tbtun-rk3588.lock
FAR_LOCK=/tmp/tbtun-hnd0.lock
LOCK_WAIT=${LOCK_WAIT:-1800}
LOCK_HOLD_SECS=${LOCK_HOLD_SECS:-14400}
LOAD_LIMIT=${LOAD_LIMIT:-4}
LOAD_SETTLE=${LOAD_SETTLE:-0}
JOIN_TIMEOUT=${JOIN_TIMEOUT:-300}
EVIDENCE_ROOT=${EVIDENCE_DIR:-}
TBT_SRC_DIR=${TBT_SRC_DIR:-}

CARRIERS=256
BURST_PORT=9001
FAR_PORT=9000
ECHO_PORT=5202
NEAR_PORTBLOCK=30000-30511
SYSCTL_MAX=67108864

# Verbatim from t52-256-rerun-42ed56f-20261106T092730Z/near.cfg.
NEAR_CFG='tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=4 m=4 fec=on mode=grouped w=0 carriers=256 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30511 nonce=00112233445566778899aabbccddeeff'

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

note() { printf '%s: %s\n' "$PROG" "$*" >&2; }
die() { local rc=$1; shift; printf '%s: %s\n' "$PROG" "$*" >&2; exit "$rc"; }
usage() { sed -n '2,73p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-$EX_OK}"; }

case ${1:-} in -h | --help) usage "$EX_OK" ;; esac
[[ $# -eq 3 ]] || usage "$EX_USAGE"
MIB=$1
SECS=$2
MODE=$3
[[ $MIB =~ ^[1-9][0-9]*$ ]] || die "$EX_USAGE" "<mib> must be a positive integer"
[[ $SECS =~ ^[1-9][0-9]*$ ]] || die "$EX_USAGE" "<seconds> must be a positive integer"
case $MODE in
    strict | interleaved) ;;
    *) die "$EX_USAGE" "<mode> must be strict or interleaved (tbt_burst's only modes)" ;;
esac

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1 || die "$EX_USAGE" "not a git checkout: $REPO"
EVIDENCE_ROOT=${EVIDENCE_ROOT:-$REPO/review/wan-experiment/arm64}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
EV=$EVIDENCE_ROOT/run-$STAMP
mkdir -p "$EV"

rrun_near() { ssh "${SSH_OPTS[@]}" "$NEAR_HOST" "$@"; }
rrun_far() { ssh "${SSH_OPTS[@]}" "$FAR_HOST" "$@"; }
rrun_on() { ssh "${SSH_OPTS[@]}" "$1" "$2"; }
rscp_near() { scp -q "${SSH_OPTS[@]}" "$@"; }

# --- locks ------------------------------------------------------------------
NEAR_LOCK_PID=""
NEAR_LOCK_TMP=""
FAR_LOCK_PID=""
FAR_LOCK_TMP=""

lock_acquire() { # host lockfile purpose varname-prefix
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

lock_release() { # varname-prefix
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
    local host=$1 limit=$2 settle=$3 deadline l1
    deadline=$((SECONDS + settle))
    while :; do
        l1=$(load1 "$host") || return 3
        if awk -v a="$l1" -v b="$limit" 'BEGIN { exit !(a + 0 <= b + 0) }'; then
            printf '%s\n' "$l1"
            return 0
        fi
        if ((SECONDS >= deadline)); then
            note "load: $host load1=$l1 > $limit; refusing (wait for the bench to finish)"
            return 4
        fi
        note "load: $host load1=$l1 > $limit; waiting up to ${settle}s"
        sleep 5
    done
}

# --- snapshot ---------------------------------------------------------------
snapshot() { # host outfile
    ssh "${SSH_OPTS[@]}" "$1" 'bash -s' >"$2" 2>&1 <<'EOS'
set -u
say() { printf '\n===%s===\n' "$1"; }
run() { say "$1"; shift; "$@" 2>&1; printf '[rc=%d]\n' "$?"; }
run uname uname -a
run date date -u +%Y-%m-%dT%H:%M:%SZ
run loadavg cat /proc/loadavg
run nproc nproc
run lsmod sh -c 'lsmod | grep -E "^tbtun" || true'
run proc_ls sh -c 'ls -l /proc/net/tbtun 2>&1 || true'
run config sh -c 'cat /proc/net/tbtun/config 2>&1 || true'
run ports sh -c 'cat /proc/net/tbtun/ports 2>&1 || true'
run tunnels sh -c 'cat /proc/net/tbtun/tunnels 2>&1 || true'
run peers sh -c 'cat /proc/net/tbtun/peers 2>&1; printf "[lines] "; wc -l < /proc/net/tbtun/peers 2>/dev/null || echo 0'
run stats sh -c 'cat /proc/net/tbtun/stats 2>&1 || true'
run dmesg sh -c 'dmesg 2>&1 | tail -n 200'
run dmesg_count sh -c 'dmesg 2>/dev/null | wc -l'
run procstat cat /proc/stat
run freem free -m
run netdev cat /proc/net/dev
run ss_listen sh -c 'ss -ltnp 2>&1 || true'
run ss_summary sh -c 'ss -s 2>&1 || true'
run ss_tcpinfo sh -c "ss -tin '( sport = :9000 or dport = :9000 or sport = :9001 or dport = :9001 or sport = :5202 or dport = :5202 )' 2>&1 | head -n 1200 || true"
run iprule sh -c 'ip rule show 2>&1 || true'
run iproute sh -c 'ip route get 45.11.1.62 2>&1 || true'
run sysctl sh -c 'sysctl net.core.rmem_max net.core.wmem_max 2>&1 || true'
EOS
}

sec() { awk -v s="===$2===" '$0 == s { f = 1; next } f && /^===/ { exit } f' "$1"; }

# First non-empty number in a section. The section body ends with the `run`
# wrapper's `[rc=N]` line, so a plain `tr -dc 0-9` would concatenate that N.
sec_number() {
    awk -v s="===$2===" '
        $0 == s { f = 1; next }
        f && /^===/ { exit }
        f && !done {
            v = $0; gsub(/[^0-9]/, "", v)
            if (v != "") { print v; done = 1 }
        }' "$1"
}

# One awk pass per lookup: no grep|head pipeline, so a closed pipe can never
# turn a successful read into a SIGPIPE status under `set -o pipefail`.
stat_field_text() { # stats-text line-key field-name
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

cfg_diff() { LC_ALL=C comm -3 <(printf '%s\n' "$1" | tr ' ' '\n' | sort) <(printf '%s\n' "$2" | tr ' ' '\n' | sort); }
expected_cfg() { printf '%s\n' "$NEAR_CFG" | sed 's/ w=0 / w=8 /'; }

module_loaded_on() { ssh "${SSH_OPTS[@]}" "$1" 'lsmod | grep -q "^tbtun "'; }
listening_on() { [[ -n $(rrun_on "$1" "ss -lntH 'sport = :$2'") ]]; }

# --- state flags and cleanup ------------------------------------------------
NEAR_RULE_ADDED=0
NEAR_SYSCTL_SAVED=0
NEAR_MODULE_LOADED=0
FAR_TOUCHED=0
OLD_RMEM=""
OLD_WMEM=""
CLEAN_FAIL=0

cleanup() {
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
    {
        printf 'near_module_loaded=%s\n' "$(module_loaded_on "$NEAR_HOST" && echo yes || echo no)"
        printf 'far_module_loaded=%s\n' "$(module_loaded_on "$FAR_HOST" && echo yes || echo no)"
        printf 'near_proc=%s\n' "$(rrun_near 'test -d /proc/net/tbtun && echo present || echo absent')"
        printf 'far_proc=%s\n' "$(rrun_far 'test -d /proc/net/tbtun && echo present || echo absent')"
        printf 'near_listen_%s=%s\n' "$BURST_PORT" "$(listening_on "$NEAR_HOST" "$BURST_PORT" && echo yes || echo no)"
        printf 'far_listen_%s=%s\n' "$FAR_PORT" "$(listening_on "$FAR_HOST" "$FAR_PORT" && echo yes || echo no)"
        printf 'far_listen_%s=%s\n' "$ECHO_PORT" "$(listening_on "$FAR_HOST" "$ECHO_PORT" && echo yes || echo no)"
        printf 'near_rule=%s\n' "$(rrun_near 'ip rule show' | grep -F '45.11.1.62 lookup main' || echo absent)"
        printf 'near_sysctl=%s\n' "$(rrun_near 'sysctl -n net.core.rmem_max net.core.wmem_max' | tr '\n' ' ')"
    } >"$EV/cleanup.txt" 2>&1 || note "cleanup record incomplete"
    lock_release NEAR
    lock_release FAR
    if ((rc == 0 && CLEAN_FAIL == 1)); then
        rc=$EX_CLEAN
    fi
    set -e
    exit "$rc"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# --- preconditions ----------------------------------------------------------
if ! rrun_near true >/dev/null 2>&1; then
    die "$EX_PRECOND" "ssh $NEAR_HOST failed (BatchMode root key required)"
fi
if ! rrun_far true >/dev/null 2>&1; then
    die "$EX_PRECOND" "ssh $FAR_HOST failed (BatchMode root key required)"
fi
ARCH=$(rrun_near 'uname -m')
[[ $ARCH == aarch64 ]] || die "$EX_PRECOND" "$NEAR_HOST is $ARCH, not aarch64"
NEAR_KREL=$(rrun_near 'uname -r')
MISSING=$(rrun_near 'for t in cc flock ss ip sysctl modinfo sha256sum; do command -v "$t" >/dev/null 2>&1 || printf "%s " "$t"; done')
[[ -z $MISSING ]] || die "$EX_PRECOND" "$NEAR_HOST lacks: $MISSING"

# --- locks and load ---------------------------------------------------------
# Both locks come before every state check and every write, so a sibling agent
# cannot change either end between the check and the act.
lock_acquire "$NEAR_HOST" "$NEAR_LOCK" "t52-arm64 run ${MIB}MiB ${SECS}s $MODE" NEAR || die "$EX_LOCK" "lock busy: $NEAR_LOCK on $NEAR_HOST"
lock_acquire "$FAR_HOST" "$FAR_LOCK" "t52-arm64 run ${MIB}MiB ${SECS}s $MODE" FAR || die "$EX_LOCK" "lock busy: $FAR_LOCK on $FAR_HOST"
NEAR_LOAD=$(wait_load "$NEAR_HOST" "$LOAD_LIMIT" "$LOAD_SETTLE") || exit $?
FAR_LOAD=$(wait_load "$FAR_HOST" "$LOAD_LIMIT" "$LOAD_SETTLE") || exit $?
note "load: near=$NEAR_LOAD far=$FAR_LOAD (limit $LOAD_LIMIT)"

# --- both ends must be in the expected state (under the locks) --------------
if module_loaded_on "$NEAR_HOST"; then
    die "$EX_PRECOND" "tbtun already loaded on $NEAR_HOST; unload it first (rmmod tbtun)"
fi
if listening_on "$NEAR_HOST" "$BURST_PORT"; then
    die "$EX_PRECOND" "port $BURST_PORT already listens on $NEAR_HOST"
fi
if ! module_loaded_on "$FAR_HOST"; then
    die "$EX_PRECOND" "tbtun is not loaded on $FAR_HOST; run deploy_hnd0.sh first"
fi
listening_on "$FAR_HOST" "$FAR_PORT" || die "$EX_PRECOND" "far side is not listening on $FAR_PORT"
listening_on "$FAR_HOST" "$ECHO_PORT" || die "$EX_PRECOND" "echo target is not listening on $ECHO_PORT; run deploy_hnd0.sh first"
FAR_CFG=$(rrun_far 'cat /proc/net/tbtun/config')
for want in 'carriers=256' 'profile=http-kal' 'rx_reorder_bytes=8388608' 'portblock=31000-31511' 'role=far'; do
    case " $FAR_CFG " in *" $want "*) ;; *) die "$EX_PRECOND" "far config lacks $want: $FAR_CFG" ;; esac
done
FAR_PORTS=$(rrun_far "grep '^ports ' /proc/net/tbtun/ports")
[[ $FAR_PORTS == 'ports 31000-31511' ]] || die "$EX_PRECOND" "far ports '$FAR_PORTS' != 'ports 31000-31511'"
FAR_TOUCHED=1

# --- staged tree ------------------------------------------------------------
if [[ -z $TBT_SRC_DIR ]]; then
    TBT_SRC_DIR=$(rrun_near 'bash -s' <<'EOS'
for d in /root/tbtun-t52-arm64-*; do
    [ -s "$d/tbtun.ko" ] && printf '%s\n' "$d"
done | tail -n 1
EOS
)
fi
[[ -n $TBT_SRC_DIR ]] || die "$EX_PRECOND" "no staged tree on $NEAR_HOST; run build_on_65.sh <ref> first"
rrun_near "test -s '$TBT_SRC_DIR/tbtun.ko'" || die "$EX_PRECOND" "no module at $TBT_SRC_DIR/tbtun.ko"
NEAR_VM=$(rrun_near "modinfo -F vermagic '$TBT_SRC_DIR/tbtun.ko' 2>/dev/null || true")
[[ -n $NEAR_VM ]] || NEAR_VM=$(rrun_near "modinfo '$TBT_SRC_DIR/tbtun.ko' | sed -n 's/^vermagic:[[:space:]]*//p'")
NEAR_VM=${NEAR_VM%% *}
[[ $NEAR_VM == "$NEAR_KREL" ]] || die "$EX_PRECOND" "module vermagic '$NEAR_VM' != $NEAR_HOST kernel '$NEAR_KREL'"

# --- burst tool -------------------------------------------------------------
if ! rrun_near "test -f '$TBT_SRC_DIR/host/tbt_burst.c'"; then
    rscp_near "$REPO/host/tbt_burst.c" "$NEAR_HOST:$TBT_SRC_DIR/host/tbt_burst.c"
fi
if ! rrun_near "cd '$TBT_SRC_DIR' && cc -O2 -Wall -Wextra -o tbt_burst host/tbt_burst.c" >"$EV/burst-build.log" 2>&1; then
    tail -n 40 "$EV/burst-build.log" >&2
    die "$EX_BUILD" "tbt_burst build failed on $NEAR_HOST; log: $EV/burst-build.log"
fi
BURST_SHA=$(rrun_near "sha256sum '$TBT_SRC_DIR/tbt_burst' '$TBT_SRC_DIR/host/tbt_burst.c'") \
    || die "$EX_BUILD" "cannot hash the burst tool on $NEAR_HOST"
note "burst: $TBT_SRC_DIR/tbt_burst ready"

# --- pre snapshot -----------------------------------------------------------
note "snapshot: pre"
snapshot "$NEAR_HOST" "$EV/pre-near.txt"
snapshot "$FAR_HOST" "$EV/pre-far.txt"
PRE_NEAR_DMESG=$(sec_number "$EV/pre-near.txt" dmesg_count)
PRE_FAR_DMESG=$(sec_number "$EV/pre-far.txt" dmesg_count)
PRE_NEAR_DMESG=${PRE_NEAR_DMESG:-0}
PRE_FAR_DMESG=${PRE_FAR_DMESG:-0}

# --- near setup -------------------------------------------------------------
NEAR_RULES=$(rrun_near 'ip rule show')
case $NEAR_RULES in
    *'45.11.1.62 lookup main'*)
        note "rule: 45.11.1.62 via main already present; leaving it in place at cleanup"
        ;;
    *)
        rrun_near 'ip rule add to 45.11.1.62/32 lookup main pref 100' || die "$EX_SETUP" "ip rule add failed on $NEAR_HOST"
        NEAR_RULE_ADDED=1
        ;;
esac
ROUTE=$(rrun_near 'ip route get 45.11.1.62')
printf '%s\n' "$ROUTE" >"$EV/route.txt"
DEV=$(printf '%s\n' "$ROUTE" | sed -n 's/.* dev \([^ ]*\).*/\1/p' | head -n 1)
case $DEV in
    Meta | tun[0-9]* | utun[0-9]* | wg[0-9]* | tailscale[0-9]*)
        die "$EX_PRECOND" "route to 45.11.1.62 still leaves via $DEV: $ROUTE" ;;
    '') die "$EX_PRECOND" "no route to 45.11.1.62: $ROUTE" ;;
esac
note "route: 45.11.1.62 via $DEV"

OLD_RMEM=$(rrun_near 'sysctl -n net.core.rmem_max')
OLD_WMEM=$(rrun_near 'sysctl -n net.core.wmem_max')
NEAR_SYSCTL_SAVED=1
rrun_near "sysctl -q -w net.core.rmem_max=$SYSCTL_MAX net.core.wmem_max=$SYSCTL_MAX" || die "$EX_SETUP" "sysctl write failed on $NEAR_HOST"
note "sysctl: rmem_max/wmem_max=$SYSCTL_MAX (was $OLD_RMEM/$OLD_WMEM)"

rrun_near "insmod '$TBT_SRC_DIR/tbtun.ko' max_carriers=$CARRIERS max_tunnels=1 debug=0" || die "$EX_SETUP" "insmod failed on $NEAR_HOST"
NEAR_MODULE_LOADED=1
rrun_near 'test -d /proc/net/tbtun' || die "$EX_SETUP" "insmod succeeded but /proc/net/tbtun is absent"

rrun_near "printf 'ports $NEAR_PORTBLOCK\n' > /proc/net/tbtun/ports" || die "$EX_SETUP" "near ports write failed"
GOT_PORTS=$(rrun_near "grep '^ports ' /proc/net/tbtun/ports")
[[ $GOT_PORTS == "ports $NEAR_PORTBLOCK" ]] || die "$EX_SETUP" "near ports readback '$GOT_PORTS'"

rrun_near "printf '%s\n' '$NEAR_CFG' > /proc/net/tbtun/config" || die "$EX_SETUP" "near config write failed"
GOT_CFG=$(rrun_near 'cat /proc/net/tbtun/config')
EXPECTED_CFG=$(expected_cfg)
if [[ $GOT_CFG != "$EXPECTED_CFG" ]]; then
    DIFF=$(cfg_diff "$EXPECTED_CFG" "$GOT_CFG")
    if [[ -n $DIFF ]]; then
        printf '%s\n' "$DIFF" >&2
        die "$EX_SETUP" "rendered near config differs (diff above)"
    fi
fi
printf '%s\n' "$GOT_CFG" >"$EV/config.txt"
note "near: config applied (w=0 rendered w=8)"

# --- join -------------------------------------------------------------------
note "join: waiting up to ${JOIN_TIMEOUT}s for up=$CARRIERS on both ends"
JOIN_DEADLINE=$((SECONDS + JOIN_TIMEOUT))
NUP=""
FUP=""
while :; do
    NUP=$(live_field "$NEAR_HOST" c3 up || true)
    FUP=$(live_field "$FAR_HOST" c3 up || true)
    if [[ $NUP == "$CARRIERS" && $FUP == "$CARRIERS" ]]; then
        break
    fi
    if ((SECONDS >= JOIN_DEADLINE)); then
        note "join: near up=${NUP:-?} far up=${FUP:-?} after ${JOIN_TIMEOUT}s"
        snapshot "$NEAR_HOST" "$EV/joined-near.txt"
        snapshot "$FAR_HOST" "$EV/joined-far.txt"
        die "$EX_GATE" "up != $CARRIERS on both ends"
    fi
    sleep 3
done
note "join: up=$CARRIERS both ends"

snapshot "$NEAR_HOST" "$EV/joined-near.txt"
snapshot "$FAR_HOST" "$EV/joined-far.txt"

GATE_FAIL=0
for end in near far; do
    f=$EV/joined-$end.txt
    [[ $(field "$f" c3 up) == "$CARRIERS" ]] || { note "gate: $end up=$(field "$f" c3 up)"; GATE_FAIL=1; }
    while read -r linekey fld; do
        v=$(field "$f" "$linekey" "$fld")
        [[ $v == 0 ]] || { note "gate: $end $fld=${v:-missing}"; GATE_FAIL=1; }
    done <<'EOF'
c8 http_parse_err
c8 profile_mismatch
c9 fec_drop_window
c9 fec_drop_crypto
c9 fec_short_unknown
EOF
    cf=$(field "$f" c3 connect_fail)
    cr=$(field "$f" c3 connect_retry)
    [[ $cf == 0 ]] || note "warn: $end connect_fail=$cf (recorded, not gated)"
    [[ $cr == 0 ]] || note "warn: $end connect_retry=$cr (recorded, not gated)"
done
((GATE_FAIL == 0)) || die "$EX_GATE" "counter gate failed; evidence: $EV"

# --- burst ------------------------------------------------------------------
BURST_TIMEOUT=$((SECS * 3 + 120))
note "burst: tbt_burst 127.0.0.1 $BURST_PORT $MIB $SECS $MODE (timeout ${BURST_TIMEOUT}s)"
set +e
rrun_near "cd '$TBT_SRC_DIR' && start=\$(date +%s%N); timeout $BURST_TIMEOUT ./tbt_burst 127.0.0.1 $BURST_PORT $MIB $SECS $MODE; rc=\$?; end=\$(date +%s%N); printf 'burst_rc=%d wall_ms=%d\n' \"\$rc\" \"\$(( (end - start) / 1000000 ))\"; exit \$rc" >"$EV/burst.txt" 2>&1
BURST_RC=$?
set -e
WALL_MS=$(sed -n 's/^burst_rc=[0-9]* wall_ms=\([0-9]*\)$/\1/p' "$EV/burst.txt" | tail -n 1)
note "burst: rc=$BURST_RC wall_ms=${WALL_MS:-?}"

# --- post snapshot ----------------------------------------------------------
note "snapshot: post"
snapshot "$NEAR_HOST" "$EV/post-near.txt"
snapshot "$FAR_HOST" "$EV/post-far.txt"
POST_NEAR_DMESG=$(sec_number "$EV/post-near.txt" dmesg_count)
POST_FAR_DMESG=$(sec_number "$EV/post-far.txt" dmesg_count)
POST_NEAR_DMESG=${POST_NEAR_DMESG:-0}
POST_FAR_DMESG=${POST_FAR_DMESG:-0}

{
    printf 'utc=%s\nmib=%s seconds=%s mode=%s\n' "$STAMP" "$MIB" "$SECS" "$MODE"
    printf 'near_host=%s far_host=%s near_src=%s\n' "$NEAR_HOST" "$FAR_HOST" "$TBT_SRC_DIR"
    printf 'burst_rc=%s wall_ms=%s\n' "$BURST_RC" "${WALL_MS:-?}"
    printf 'near_load1=%s far_load1=%s\n' "$NEAR_LOAD" "$FAR_LOAD"
    printf 'dmesg_delta_near=%s dmesg_delta_far=%s\n' "$((POST_NEAR_DMESG - PRE_NEAR_DMESG))" "$((POST_FAR_DMESG - PRE_FAR_DMESG))"
    printf '\n'
    for end in near far; do
        f=$EV/post-$end.txt
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
    printf '\nburst_sha256=\n%s\n' "$BURST_SHA"
} >"$EV/summary.txt" 2>&1 || note "summary incomplete"

printf '\nrun: %s MiB, %s s, %s -> rc=%s wall_ms=%s\n' "$MIB" "$SECS" "$MODE" "$BURST_RC" "${WALL_MS:-?}"
printf 'evidence: %s\n' "$EV"
if ((BURST_RC != 0)); then
    exit "$EX_RUN"
fi
exit "$EX_OK"
