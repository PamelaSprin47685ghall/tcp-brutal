#!/usr/bin/env bash
# smoke_37b_evidence.sh - bounded 1-carrier-pair / 37-byte x2 smoke with the
# evidence a stalled direction needs:
#   - far-side one-shot carrier stall dump (debug>=2, one per generation),
#   - /proc/net/tcp rows for 9000/40000/40001/5202 on both ends
#     (state, tx_queue:rx_queue),
#   - fbk_tx/fbk_rx, rx_reads/rx_read_bytes/rx_delivered, tx_ack/rx_ack,
#   - TXSTALLKILL and hole lines from dmesg,
#   - a live sample of the same counters every 500 ms across both pings.
#
# Shape (same as smoke_37b.sh):
#   near .65 (aarch64): staged tree, insmod max_carriers=16 debug=2,
#     raw/fec=off config, carriers=2 (n=1 m=1), ip rule to 45.11.1.62 via
#     main pref 100, two tbt_ping 127.0.0.1 9000 37.
#   far HND0 (x86_64): staged tree, insmod max_carriers=16 debug=2, same
#     config tokens (role=far target=127.0.0.1:5202), tbt_echo 5202 via
#     systemd-run unit tbt-echo-5202.
#
# Single owner: takes /tmp/tbtun-smoke-owner.lock (flock -n) and refuses to run
# beside another smoke. Stop rule: a .65 eth0 Link Down after run start keeps
# the in-flight ping only, then tears down and exits 11. No retry loop.
#
# Usage: smoke_37b_evidence.sh <near-stage-dir> <far-stage-dir> <evidence-dir>

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

# --- single owner -----------------------------------------------------------
exec 9>/tmp/tbtun-smoke-owner.lock
flock -n 9 || die "another smoke owns /tmp/tbtun-smoke-owner.lock" 9

mkdir -p "$EV"
: >"$EV/SUMMARY.txt"

# Ports worth watching in /proc/net/tcp: 9000 (near public listen), 40000
# (far carrier listen), 40001 (second carrier slot if the config opens one),
# 5202 (far target echo). Hex, case-insensitive because the kernel prints caps.
TCP_GREP=':(2328|9C40|9C41|1452) '

snapshot() { # host outfile
    local host=$1 out=$2
    {
        echo "=== DATE $(date -Is)"
        echo "=== CONFIG"; "${SSH[@]}" "$host" 'cat /proc/net/tbtun/config 2>/dev/null'
        echo "=== PEERS"; "${SSH[@]}" "$host" 'cat /proc/net/tbtun/peers 2>/dev/null'
        echo "=== TUNNELS"; "${SSH[@]}" "$host" 'cat /proc/net/tbtun/tunnels 2>/dev/null'
        echo "=== STATS"; "${SSH[@]}" "$host" 'cat /proc/net/tbtun/stats 2>/dev/null'
        echo "=== STATS_KEYS"; "${SSH[@]}" "$host" "grep -E 'fbk_|rx_read|rx_delivered|tx_ack|rx_ack|lost=|join_dup|stall_declared|up=' /proc/net/tbtun/stats 2>/dev/null"
        echo "=== TCP_ALL"; "${SSH[@]}" "$host" 'cat /proc/net/tcp 2>/dev/null'
        echo "=== TCP_KEYS"; "${SSH[@]}" "$host" "grep -iE '$TCP_GREP' /proc/net/tcp 2>/dev/null"
        echo "=== SS_KEYS"; "${SSH[@]}" "$host" "ss -tanp 2>/dev/null | grep -E ':(9000|40000|40001|5202)\\b'"
        echo "=== DMESG_COUNT"; "${SSH[@]}" "$host" 'dmesg 2>/dev/null | wc -l'
        echo "=== DMESG_TBTUN"; "${SSH[@]}" "$host" 'dmesg 2>/dev/null | grep -iE "tbtun|TXSTALLKILL|hole |carrier stall" | tail -200'
        echo "=== LINK"; "${SSH[@]}" "$host" 'dmesg 2>/dev/null | grep -iE "link is|eth0" | tail -10; ip -br link show eth0 2>/dev/null; cat /sys/class/net/eth0/carrier 2>/dev/null; cat /sys/class/net/eth0/operstate 2>/dev/null'
    } >"$out" 2>&1 || true
}

# Live sampler: one ssh per end, sampling the counters and the socket table
# every 500 ms for $1 seconds. Runs in the background; callers `wait`.
live_sampler() { # host outfile seconds
    local host=$1 out=$2 secs=$3
    "${SSH[@]}" "$host" "end=\$(( \$(date +%s) + $secs )); while [ \$(date +%s) -lt \$end ]; do
        printf '### %s\n' \"\$(date +%H:%M:%S.%3N)\"
        grep -E 'fbk_|rx_read|rx_delivered|tx_ack|rx_ack|lost=|join_dup|stall_declared|up=' /proc/net/tbtun/stats 2>/dev/null
        grep -iE '$TCP_GREP' /proc/net/tcp 2>/dev/null
        sleep 0.5
    done" >"$out" 2>&1 || true
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

# Pollution guard: another smoke that rmmod/insmods either end invalidates the
# run (a rebuilt near session no longer matches the far's recorded nonce, so
# every later HELLO is join_bad). Count unload events before the pings and
# compare after them.
NEAR_UNLOADS_BASE=$("${SSH[@]}" "$NEAR_HOST" "dmesg 2>/dev/null | grep -c 'tbtun: unloaded'" || true)
FAR_UNLOADS_BASE=$("${SSH[@]}" "$FAR_HOST"  "dmesg 2>/dev/null | grep -c 'tbtun: unloaded'" || true)
n "guard: unload base near=${NEAR_UNLOADS_BASE:-?} far=${FAR_UNLOADS_BASE:-?}"

# --- live sampling across both pings ---------------------------------------
SAMPLES_SECS=40
: >"$EV/live-near.txt"; : >"$EV/live-far.txt"
live_sampler "$NEAR_HOST" "$EV/live-near.txt" "$SAMPLES_SECS" & NPID=$!
live_sampler "$FAR_HOST"  "$EV/live-far.txt"  "$SAMPLES_SECS" & FPID=$!
n "live: sampling both ends every 500 ms for ${SAMPLES_SECS}s"

# --- ping x2 ---------------------------------------------------------------
FLAP=0
for p in 1 2; do
    n "ping#$p: tbt_ping 127.0.0.1 9000 37 5"
    local_rc=0
    "${SSH[@]}" "$NEAR_HOST" "cd '$NEAR_DIR' && timeout 20 ./tbt_ping 127.0.0.1 9000 37 5 0" >"$EV/ping$p.txt" 2>&1 || local_rc=$?
    n "ping#$p rc=$local_rc: $(cat "$EV/ping$p.txt" | tr '\n' ' ' | cut -c1-200)"
    snapshot "$NEAR_HOST" "$EV/post${p}-near.txt"
    snapshot "$FAR_HOST"  "$EV/post${p}-far.txt"
    "${SSH[@]}" "$NEAR_HOST" "dmesg 2>/dev/null | grep -iE 'link is (down|up)'" >"$EV/link-after-ping$p.txt" 2>&1 || true
    if awk -v base="$RUN_START_UPTIME" 'match($0,/\[ *([0-9]+)\./,a){if(a[1]+0>=base+0 && /Link is Down/) f=1} END{exit !f}' "$EV/link-after-ping$p.txt"; then
        FLAP=1
        n "LINK FLAP detected after ping#$p — see link-after-ping$p.txt"
    fi
done
wait "$NPID" "$FPID" 2>/dev/null || true

NEAR_UNLOADS_END=$("${SSH[@]}" "$NEAR_HOST" "dmesg 2>/dev/null | grep -c 'tbtun: unloaded'" || true)
FAR_UNLOADS_END=$("${SSH[@]}" "$FAR_HOST"  "dmesg 2>/dev/null | grep -c 'tbtun: unloaded'" || true)
POLLUTED=0
if [[ ${NEAR_UNLOADS_END:-0} -gt ${NEAR_UNLOADS_BASE:-0} || ${FAR_UNLOADS_END:-0} -gt ${FAR_UNLOADS_BASE:-0} ]]; then
    POLLUTED=1
    {
        echo "POLLUTED=1"
        echo "near_unloads base=${NEAR_UNLOADS_BASE:-?} end=${NEAR_UNLOADS_END:-?}"
        echo "far_unloads base=${FAR_UNLOADS_BASE:-?} end=${FAR_UNLOADS_END:-?}"
    } >"$EV/POLLUTED.txt"
    n "POLLUTED: another owner rmmod/insmoded an end during the run"
fi

# --- focused stall/hole/TXSTALLKILL extract --------------------------------
"${SSH[@]}" "$NEAR_HOST" "dmesg 2>/dev/null" >"$EV/dmesg-near-full.txt" 2>&1 || true
"${SSH[@]}" "$FAR_HOST"  "dmesg 2>/dev/null" >"$EV/dmesg-far-full.txt" 2>&1 || true
for f in "$EV/dmesg-near-full.txt" "$EV/dmesg-far-full.txt"; do
    grep -iE 'TXSTALLKILL|RXHOLEKILL|carrier stall|hole (gatefail|depthfail|timefail)|carrier (DIALED|ACCEPTED)|link is (down|up)' "$f" >"${f%.txt}-focus.txt" 2>/dev/null || true
done

# --- ko identity -------------------------------------------------------------
{
    echo "utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "near_ko=$NEAR_DIR/tbtun.ko"
    "${SSH[@]}" "$NEAR_HOST" "modinfo -F srcversion '$NEAR_DIR/tbtun.ko'; sha256sum '$NEAR_DIR/tbtun.ko'"
    echo "far_ko=$FAR_DIR/tbtun.ko"
    "${SSH[@]}" "$FAR_HOST"  "modinfo -F srcversion '$FAR_DIR/tbtun.ko'; sha256sum '$FAR_DIR/tbtun.ko'"
} >"$EV/ko.txt" 2>&1

# --- summary ----------------------------------------------------------------
{
    echo "run_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "head=$(cd "$(dirname "$0")/../../.." && git rev-parse HEAD 2>/dev/null)"
    echo "near_dir=$NEAR_DIR far_dir=$FAR_DIR"
    echo "--- pings"; cat "$EV/ping1.txt" "$EV/ping2.txt" 2>/dev/null
    echo "--- near stats keys"; grep -E 'fbk_|rx_read|rx_delivered|tx_ack|rx_ack|lost=|join_dup|stall_declared|up=' "$EV/post2-near.txt" 2>/dev/null | tail -6
    echo "--- far stats keys";  grep -E 'fbk_|rx_read|rx_delivered|tx_ack|rx_ack|lost=|join_dup|stall_declared|up=' "$EV/post2-far.txt" 2>/dev/null | tail -6
    echo "--- TXSTALLKILL"; grep -c 'TXSTALLKILL' "$EV/dmesg-near-full.txt" "$EV/dmesg-far-full.txt" 2>/dev/null || true
    echo "--- hole lines"; grep -cE 'hole (gatefail|depthfail|timefail)' "$EV/dmesg-near-full.txt" "$EV/dmesg-far-full.txt" 2>/dev/null || true
    echo "--- carrier stall dumps"; grep -c 'carrier stall' "$EV/dmesg-near-full.txt" "$EV/dmesg-far-full.txt" 2>/dev/null || true
    echo "--- focus (near)"; cat "$EV/dmesg-near-full-focus.txt" 2>/dev/null | tail -40
    echo "--- focus (far)"; cat "$EV/dmesg-far-full-focus.txt" 2>/dev/null | tail -40
    echo "--- cleanup"; cat "$EV/cleanup.txt" 2>/dev/null
} >"$EV/SUMMARY.txt" 2>&1

((FLAP == 0)) || exit 11
((POLLUTED == 0)) || exit 12
exit 0
