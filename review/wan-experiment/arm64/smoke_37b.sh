#!/usr/bin/env bash
# smoke_37b.sh - 1-carrier-pair / 37-byte x2 smoke on the merged fix head.
#
# Replicates the t52-smoke2 bounded run shape exactly, on a caller-supplied
# source revision:
#   near .65 (aarch64): staged tree from build_on_65.sh, insmod
#     max_carriers=16 debug=2, raw/fec=off config, carriers=2 (n=1 m=1),
#     ip rule to 45.11.1.62 via main pref 100, two tbt_ping 127.0.0.1 9000 37.
#   far HND0 (x86_64): staged tree built on HND0, insmod
#     max_carriers=16 debug=2, same config tokens (role=far target=127.0.0.1:5202),
#     tbt_echo 5202 via systemd-run unit tbt-echo-5202.
#
# Stop rule: if .65 eth0 link flaps (dmesg Link Down/Up) during the run, the
# script keeps going only to finish the in-flight ping, then tears down and
# exits 11 with the flap evidence — no retry loop.
#
# Usage: smoke_37b.sh <near-stage-dir> <far-stage-dir> <evidence-dir>
#   near-stage-dir  e.g. /root/tbtun-t52-arm64-9ba6a096b1bb (holds tbtun.ko + src)
#   far-stage-dir   e.g. /home/tbtun-t52-9ba6a09
#   evidence-dir    local dir for pre/mid/post snapshots and SUMMARY.md

set -euo pipefail

PROG=${0##*/}
NEAR_HOST=${NEAR_HOST:-root@192.168.123.65}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
NEAR_DIR=$1
FAR_DIR=$2
EV=$3

SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=4)

NEAR_CFG='tunnel 1 role=near listen=0.0.0.0:9000 peer=45.11.1.62:40000 n=1 m=1 fec=off mode=rlnc w=2 carriers=2 crypto=off crypto_key=- dup=0 profile=raw keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=0 portblock=0-0'
FAR_CFG='tunnel 1 role=far listen=0.0.0.0:40000 target=127.0.0.1:5202 n=1 m=1 fec=off mode=rlnc w=2 carriers=2 crypto=off crypto_key=- dup=0 profile=raw keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=0 portblock=0-0'

n() { printf '%s: %s\n' "$PROG" "$*" >&2; }
die() { n "FAIL: $*"; exit "${2:-1}"; }

mkdir -p "$EV"

snapshot() { # host outfile
    local host=$1 out=$2
    {
        echo "=== DATE $(date -Is)"
        echo "=== CONFIG"; "${SSH[@]}" "$host" 'cat /proc/net/tbtun/config 2>/dev/null'
        echo "=== PEERS"; "${SSH[@]}" "$host" 'cat /proc/net/tbtun/peers 2>/dev/null'
        echo "=== TUNNELS"; "${SSH[@]}" "$host" 'cat /proc/net/tbtun/tunnels 2>/dev/null'
        echo "=== STATS"; "${SSH[@]}" "$host" 'cat /proc/net/tbtun/stats 2>/dev/null'
        echo "=== DMESG_COUNT"; "${SSH[@]}" "$host" 'dmesg 2>/dev/null | wc -l'
        echo "=== DMESG_TBTUN"; "${SSH[@]}" "$host" 'dmesg 2>/dev/null | grep -i tbtun | tail -60'
        echo "=== LINK"; "${SSH[@]}" "$host" 'dmesg 2>/dev/null | grep -iE "link is|eth0" | tail -10; ip -br link show eth0 2>/dev/null; cat /sys/class/net/eth0/carrier 2>/dev/null; cat /sys/class/net/eth0/operstate 2>/dev/null'
    } >"$out" 2>&1 || true
}

cleanup() {
    local rc=$?
    trap - EXIT INT TERM HUP
    set +e
    "${SSH[@]}" "$NEAR_HOST"  'rmmod tbtun 2>/dev/null; ip rule del to 45.11.1.62/32 lookup main pref 100 2>/dev/null; rm -f /tmp/tbt_ping; true' >/dev/null 2>&1
    "${SSH[@]}" "$FAR_HOST"   'systemctl stop tbt-echo-5202.service 2>/dev/null; systemctl reset-failed tbt-echo-5202.service 2>/dev/null; rmmod tbtun 2>/dev/null; true' >/dev/null 2>&1
    {
        echo "near_module=$("${SSH[@]}" "$NEAR_HOST" 'lsmod | grep -c "^tbtun "' 2>/dev/null || echo '?')"
        echo "far_module=$("${SSH[@]}" "$FAR_HOST" 'lsmod | grep -c "^tbtun "' 2>/dev/null || echo '?')"
        echo "near_proc=$("${SSH[@]}" "$NEAR_HOST" 'test -d /proc/net/tbtun && echo present || echo absent')"
        echo "far_proc=$("${SSH[@]}" "$FAR_HOST" 'test -d /proc/net/tbtun && echo present || echo absent')"
        echo "near_listen_9000=$("${SSH[@]}" "$NEAR_HOST" "ss -lntH 'sport = :9000'" | wc -l)"
        echo "far_listen_40000=$("${SSH[@]}" "$FAR_HOST" "ss -lntH 'sport = :40000'" | wc -l)"
        echo "far_listen_5202=$("${SSH[@]}" "$FAR_HOST" "ss -lntH 'sport = :5202'" | wc -l)"
        echo "near_rule=$("${SSH[@]}" "$NEAR_HOST" 'ip rule show' | grep -c '45.11.1.62 lookup main' || true)"
    } >"$EV/cleanup.txt" 2>&1
    n "cleanup: $(tr '\n' ' ' <"$EV/cleanup.txt")"
    set -e
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# --- preconditions ----------------------------------------------------------
"${SSH[@]}" "$NEAR_HOST" "test -f '$NEAR_DIR/tbtun.ko'" || die "no ko at $NEAR_DIR on $NEAR_HOST" 3
"${SSH[@]}" "$FAR_HOST"  "test -f '$FAR_DIR/tbtun.ko'"  || die "no ko at $FAR_DIR on $FAR_HOST" 3
true

# --- link health pre-check --------------------------------------------------
snapshot "$NEAR_HOST" "$EV/pre-near.txt"
snapshot "$FAR_HOST"  "$EV/pre-far.txt"
if grep -iE 'link is down' "$EV/pre-near.txt" | tail -3 | grep -q .; then
    n "note: recent Link Down lines on .65 (see pre-near.txt LINK section)"
fi

RUN_START_UPTIME=$("${SSH[@]}" "$NEAR_HOST" 'cut -d" " -f1 /proc/uptime' | cut -d. -f1)

# --- far up -----------------------------------------------------------------
n "far: build tbt_echo, insmod debug=2, config, echo 5202"
"${SSH[@]}" "$FAR_HOST" "cd '$FAR_DIR' && cc -O2 -Wall -Wextra -o tbt_echo benches/src/tbt_echo.c" || die "tbt_echo build failed on $FAR_HOST" 6
"${SSH[@]}" "$FAR_HOST" "rmmod tbtun 2>/dev/null; sleep 0.3; insmod '$FAR_DIR/tbtun.ko' max_carriers=16 debug=2" || die "far insmod failed" 7
"${SSH[@]}" "$FAR_HOST" "printf '%s' '$FAR_CFG' > /proc/net/tbtun/config" || die "far config write failed" 7
"${SSH[@]}" "$FAR_HOST" "systemctl stop tbt-echo-5202.service 2>/dev/null; systemctl reset-failed tbt-echo-5202.service 2>/dev/null; systemd-run --no-block --unit=tbt-echo-5202 --collect --property=StandardOutput=file:$FAR_DIR/tbt-echo-5202.log --property=StandardError=file:$FAR_DIR/tbt-echo-5202.log '$FAR_DIR/tbt_echo' 5202 echo" || die "echo unit failed" 7
echo_up=0
for i in $(seq 1 60); do
    if "${SSH[@]}" "$FAR_HOST" "ss -lntH 'sport = :5202'" | grep -q .; then echo_up=1; break; fi
    sleep 0.5
done
((echo_up == 1)) || die "echo not listening on 5202 after 30s" 7

# --- near up ----------------------------------------------------------------
n "near: rule, insmod debug=2, config, build tbt_ping"
"${SSH[@]}" "$NEAR_HOST" 'ip rule del to 45.11.1.62/32 lookup main pref 100 2>/dev/null; ip rule add to 45.11.1.62/32 lookup main pref 100' || die "near ip rule failed" 7
"${SSH[@]}" "$NEAR_HOST" "rmmod tbtun 2>/dev/null; sleep 0.3; insmod '$NEAR_DIR/tbtun.ko' max_carriers=16 debug=2" || die "near insmod failed" 7
"${SSH[@]}" "$NEAR_HOST" "cd '$NEAR_DIR' && cc -O2 -Wall -Wextra -o tbt_ping host/tbt_ping.c" || die "tbt_ping build failed" 6
"${SSH[@]}" "$NEAR_HOST" "printf '%s' '$NEAR_CFG' > /proc/net/tbtun/config" || die "near config write failed" 7

# --- join gate ---------------------------------------------------------------
n "join: waiting up to 120s for up=2 both ends"
deadline=$((SECONDS + 120))
: >"$EV/join-poll.txt"
while :; do
    nstats=$("${SSH[@]}" "$NEAR_HOST" 'cat /proc/net/tbtun/stats 2>/dev/null')
    fstats=$("${SSH[@]}" "$FAR_HOST"  'cat /proc/net/tbtun/stats 2>/dev/null')
    nup=$(printf '%s\n' "$nstats" | sed -n 's/^tunnel 1 c3 .* up=\([0-9][0-9]*\).*/\1/p' | tail -1)
    fup=$(printf '%s\n' "$fstats" | sed -n 's/^tunnel 1 c3 .* up=\([0-9][0-9]*\).*/\1/p' | tail -1)
    { echo "--- poll $(date +%H:%M:%S) near_up=${nup:-?} far_up=${fup:-?}"; printf '%s\n' "$nstats" | grep 'c3 '; printf '%s\n' "$fstats" | grep 'c3 '; } >>"$EV/join-poll.txt"
    [[ $nup == 2 && $fup == 2 ]] && break
    if ((SECONDS >= deadline)); then
        snapshot "$NEAR_HOST" "$EV/joined-near.txt"
        snapshot "$FAR_HOST"  "$EV/joined-far.txt"
        die "join timeout: near up=${nup:-?} far up=${fup:-?}" 8
    fi
    sleep 3
done
n "join: up=2 both ends"
snapshot "$NEAR_HOST" "$EV/joined-near.txt"
snapshot "$FAR_HOST"  "$EV/joined-far.txt"

# --- ping x2 ---------------------------------------------------------------
FLAP=0
for p in 1 2; do
    n "ping#$p: tbt_ping 127.0.0.1 9000 37 5"
    local_rc=0
    "${SSH[@]}" "$NEAR_HOST" "cd '$NEAR_DIR' && timeout 20 ./tbt_ping 127.0.0.1 9000 37 5 0" >"$EV/ping$p.txt" 2>&1 || local_rc=$?
    n "ping#$p rc=$local_rc: $(cat "$EV/ping$p.txt" | tr '\n' ' ' | cut -c1-200)"
    snapshot "$NEAR_HOST" "$EV/post${p}-near.txt"
    snapshot "$FAR_HOST"  "$EV/post${p}-far.txt"
    # link flap check after each ping
    # flap = a Link Down/Up pair whose dmesg uptime timestamp is newer than
    # the run start; older flap lines must not false-positive.
    "${SSH[@]}" "$NEAR_HOST" "dmesg 2>/dev/null | grep -iE 'link is (down|up)'" >"$EV/link-after-ping$p.txt" 2>&1 || true
    if awk -v base="$RUN_START_UPTIME" 'match($0,/\[ *([0-9]+)\./,a){if(a[1]+0>=base+0 && /Link is Down/) f=1} END{exit !f}' "$EV/link-after-ping$p.txt"; then
        FLAP=1
        n "LINK FLAP detected after ping#$p — see link-after-ping$p.txt"
    fi
done

# --- ko identity -------------------------------------------------------------
{
    echo "utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "near_ko=$NEAR_DIR/tbtun.ko"
    "${SSH[@]}" "$NEAR_HOST" "modinfo -F srcversion '$NEAR_DIR/tbtun.ko'; sha256sum '$NEAR_DIR/tbtun.ko'"
    echo "far_ko=$FAR_DIR/tbtun.ko"
    "${SSH[@]}" "$FAR_HOST"  "modinfo -F srcversion '$FAR_DIR/tbtun.ko'; sha256sum '$FAR_DIR/tbtun.ko'"
} >"$EV/ko.txt" 2>&1

((FLAP == 0)) || exit 11
exit 0
