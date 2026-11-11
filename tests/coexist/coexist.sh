#!/usr/bin/env bash
#
# coexist.sh - coexistence acceptance for the tbt_cc carrier controller.
#
# WHAT THIS MEASURES
#   A stock TCP flow and the tbtun carriers share one bottleneck (a netem/TBF
#   qdisc on the far side's veth egress). The acceptance number is the stock
#   flow's throughput drop caused by tbtun, not tbtun's own goodput:
#
#     baseline  stock TCP alone          -> B bytes in SECS
#     coexist   stock TCP + tbtun bulk   -> C bytes in SECS
#     drop      (B - C) / B * 100        -> must be <= MAX_DROP_PCT
#
#   The tbtun side runs the real FEC data plane (n=4 m=2, grouped) so the
#   carriers use tbt_cc's pacing from the peer's FEC_FBK estimate. The stock
#   flow uses the system default congestion control and never enters tbtun.
#
# TOPOLOGY
#   netns tbtc-near 10.72.0.1 <--veth--> 10.72.0.2 netns tbtc-far
#   stock TCP:  far sender -> near sink (10.72.0.1:9091), direct, shaped
#   tbtun:      near public 9700 -> far carrier 9701 -> far source :9092
#               (downlink bulk far -> near, the same shaped veth egress)
#
# REQUIREMENTS
#   root, tbtun.ko in the repository root, ip/tc/python3, and the netem or
#   tbf qdisc. Without them the script exits 77 (cannot run here); that is a
#   skip, never a pass.
#
# USAGE
#   sudo tests/coexist/coexist.sh
#   SECS=15 MAX_DROP_PCT=10 ROUNDS=3 sudo -E tests/coexist/coexist.sh
#
# EXIT: 0 pass, 1 fail, 77 cannot run.
set -u

NEAR=tbtc-near
FAR=tbtc-far
NEAR_IP=10.72.0.1
FAR_IP=10.72.0.2
PUB_PORT=9700
CAR_PORT=9701
STOCK_PORT=9091
SRC_PORT=9092
PORTS=43000-43100
PORTBLOCK=43000-43100

SECS=${SECS:-10}
ROUNDS=${ROUNDS:-2}
MAX_DROP_PCT=${MAX_DROP_PCT:-10}
RATE=${RATE:-100mbit}
DELAY=${DELAY:-10ms}

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PY=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/bulk.py
TMP=$(mktemp -d)
PIDS=""
LOADED_MODULE=0
fail=0

say() { printf '%s\n' "$*"; }
bad() { printf 'FAIL %s\n' "$*"; fail=1; }

cleanup() {
    for p in $PIDS; do kill "$p" 2> /dev/null || true; done
    ip netns del "$NEAR" 2> /dev/null || true
    ip netns del "$FAR" 2> /dev/null || true
    if [ "$LOADED_MODULE" -eq 1 ]; then rmmod tbtun 2> /dev/null || true; fi
    rm -rf "$TMP"
}
trap cleanup EXIT

if [ "$(id -u)" -ne 0 ]; then
    say "SKIP: needs root (netns, tc, insmod)."
    exit 77
fi
for c in ip tc python3; do
    command -v "$c" > /dev/null || { say "SKIP: $c missing"; exit 77; }
done

if ! grep -q '^tbtun ' /proc/modules; then
    if [ ! -f "$ROOT/tbtun.ko" ]; then
        say "SKIP: $ROOT/tbtun.ko missing; run make first."
        exit 77
    fi
    insmod "$ROOT/tbtun.ko" || { say "SKIP: insmod failed"; exit 77; }
    LOADED_MODULE=1
fi

say "coexist: stock TCP default cc = $(sysctl -n net.ipv4.tcp_congestion_control 2>/dev/null || echo unknown)"
say "coexist: bottleneck rate=$RATE delay=$DELAY, secs=$SECS rounds=$ROUNDS limit=${MAX_DROP_PCT}%"

ip netns add "$NEAR" || { bad "near netns"; exit 1; }
ip netns add "$FAR" || { bad "far netns"; exit 1; }
ip -n "$NEAR" link add v0 type veth peer name v1 netns "$FAR" || bad "veth"
ip -n "$NEAR" addr add "$NEAR_IP/24" dev v0 || bad "near addr"
ip -n "$FAR" addr add "$FAR_IP/24" dev v1 || bad "far addr"
for ns in "$NEAR" "$FAR"; do
    ip netns exec "$ns" ip link set lo up || bad "$ns lo"
done
ip -n "$NEAR" link set v0 up || bad "near v0"
ip -n "$FAR" link set v1 up || bad "far v1"

# Both the stock TCP flow (far -> near) and the tbtun carriers (far -> near)
# leave the far netns through v1. Shape that one egress, or the two flows do
# not share a bottleneck and the measurement is meaningless.
if ! ip netns exec "$FAR" tc qdisc add dev v1 root netem rate "$RATE" delay "$DELAY" limit 2000 2> "$TMP/tc.err"; then
    if ! ip netns exec "$FAR" tc qdisc add dev v1 root tbf rate "$RATE" burst 64kb latency 20ms 2>> "$TMP/tc.err"; then
        say "SKIP: neither netem nor tbf accepted the qdisc: $(cat "$TMP/tc.err")"
        exit 77
    fi
fi

for ns in "$NEAR" "$FAR"; do
    ip netns exec "$ns" sh -c "echo 'ports $PORTS' > /proc/net/tbtun/ports" || bad "$ns: ports"
done

# The far side listens for carriers and dials the local bulk source. The near
# side owns the public port the stock-like client uses.
ip netns exec "$FAR" sh -c "echo 'tunnel 1 role=far profile=raw \
    listen=$FAR_IP:$CAR_PORT target=127.0.0.1:$SRC_PORT n=4 m=2 fec=on \
    mode=grouped carriers=6 crypto=off dup=0' > /proc/net/tbtun/config" ||
    bad "far: tunnel config"
ip netns exec "$NEAR" sh -c "echo 'tunnel 1 role=near profile=raw \
    listen=$NEAR_IP:$PUB_PORT peer=$FAR_IP:$CAR_PORT n=4 m=2 fec=on \
    mode=grouped carriers=6 crypto=off dup=0 portblock=$PORTBLOCK' \
    > /proc/net/tbtun/config" || bad "near: tunnel config"
[ "$fail" -eq 0 ] || exit 1

sleep 3
if command -v ss > /dev/null; then
    n_cc=$(ip netns exec "$NEAR" ss -tin 2>/dev/null | grep -c 'tbt_cc' || true)
    if [ "$n_cc" -eq 0 ]; then
        bad "no carrier socket reports tbt_cc (falls back to BBR?)"
    else
        say "coexist: $n_cc carrier socket(s) report tbt_cc"
    fi
fi

ip netns exec "$NEAR" python3 "$PY" sink "$STOCK_PORT" &
PIDS="$PIDS $!"
sleep 0.3

baseline=""
coexist=""
for r in $(seq 1 "$ROUNDS"); do
    b=$(ip netns exec "$FAR" python3 "$PY" send "$NEAR_IP" "$STOCK_PORT" "$SECS")
    say "COEXIST round=$r phase=baseline stock_bytes=$b"
    baseline="$baseline $b"
done

for r in $(seq 1 "$ROUNDS"); do
    ip netns exec "$FAR" python3 "$PY" src "$SRC_PORT" "$SECS" > "$TMP/src.$r" 2> "$TMP/src.$r.err" &
    src_pid=$!
    PIDS="$PIDS $src_pid"
    sleep 0.3
    ip netns exec "$NEAR" python3 "$PY" recv "$NEAR_IP" "$PUB_PORT" "$SECS" > "$TMP/recv.$r" 2> "$TMP/recv.$r.err" &
    recv_pid=$!
    PIDS="$PIDS $recv_pid"
    c=$(ip netns exec "$FAR" python3 "$PY" send "$NEAR_IP" "$STOCK_PORT" "$SECS")
    wait "$src_pid" 2> /dev/null || true
    wait "$recv_pid" 2> /dev/null || true
    tunnel_bytes=$(cat "$TMP/recv.$r")
    say "COEXIST round=$r phase=coexist stock_bytes=$c tbtun_bytes=$tunnel_bytes"
    if [ "${tunnel_bytes:-0}" -eq 0 ]; then
        bad "round $r: tbtun carried no bytes (see $TMP, now removed)"
    fi
    coexist="$coexist $c"
done

stats=$(python3 - "$MAX_DROP_PCT" $baseline -- $coexist <<'PY'
import statistics, sys
sep = sys.argv.index('--')
limit = float(sys.argv[1])
base = [float(x) for x in sys.argv[2:sep]]
co = [float(x) for x in sys.argv[sep + 1:]]
b = statistics.mean(base)
c = statistics.mean(co)
drop = (b - c) * 100.0 / b if b else 0.0
print(f"{b:.0f} {c:.0f} {drop:.1f} {'PASS' if drop <= limit else 'FAIL'}")
PY
)
set -- $stats
base_mean=${1:-0}
co_mean=${2:-0}
drop_pct=${3:-0}
verdict=${4:-FAIL}

say "COEXIST baseline_bytes=$base_mean coexist_bytes=$co_mean drop_pct=$drop_pct limit_pct=$MAX_DROP_PCT result=$verdict"
[ "$verdict" = "PASS" ] || fail=1

if [ "$fail" -eq 0 ]; then
    say "coexist: PASS"
    exit 0
fi
say "coexist: FAIL"
exit 1
