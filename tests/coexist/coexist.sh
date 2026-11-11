#!/usr/bin/env bash
#
# coexist.sh - OPTIONAL coexistence diagnostic for the tbt_cc controller.
#
# NOT AN ACCEPTANCE GATE. The deployment target is metered dedicated
# bandwidth, where contention with third-party traffic on one bottleneck is
# not a design case. No coexistence measurement is part of the release
# criteria and none was performed. This script is not run by `make check`,
# cannot block a release, and runs only when invoked deliberately as
# `make coexist-diag` with TBT_COEXIST_ALLOW_LO=1.
#
# WHAT IT MEASURES (when deliberately run)
#   A stock TCP flow and the tbtun carriers share one bottleneck. The
#   diagnostic number is the stock flow's throughput drop caused by tbtun,
#   not tbtun's own goodput:
#
#     baseline  stock TCP alone          -> B bytes in SECS
#     coexist   stock TCP + tbtun bulk   -> C bytes in SECS
#     drop      (B - C) / B * 100        -> reported, never a pass/fail gate
#
#   The bottleneck is a netem/TBF qdisc on init_net's `lo`, because
#   /proc/net/tbtun is registered in init_net only and cannot be configured
#   from a child netns. Both the stock TCP flow and the tbtun carriers use
#   127.0.0.x, so both cross the same qdisc. That also means the qdisc shapes
#   every loopback flow on the host: run it only on a dedicated test host.
#
# TOPOLOGY (all in init_net)
#   stock TCP:  sender -> 127.0.0.1:9091 sink
#   tbtun:      near 127.0.0.1:9700 -> carrier 127.0.0.2:9701 (far) ->
#               far source 127.0.0.1:9092; the downlink bulk crosses lo
#
# LIMITATION
#   A loopback qdisc is a host-local bottleneck, not a WAN path. The result
#   is useful only as a relative diagnostic on a quiet, dedicated host. This
#   is why the number is not part of acceptance.
#
# REQUIREMENTS
#   root, tbtun.ko in the repository root, ip/tc/python3, and the netem or
#   tbf qdisc. Without them the script exits 77 (cannot run here); that is a
#   skip, never a pass.
#
# USAGE
#   TBT_COEXIST_ALLOW_LO=1 sudo -E make coexist-diag
#   SECS=15 ROUNDS=3 TBT_COEXIST_ALLOW_LO=1 sudo -E make coexist-diag
#
# EXIT: 0 completed, 1 diagnostic mismatch, 77 cannot run.
set -u

NEAR_IP=127.0.0.1
FAR_IP=127.0.0.2
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
bad() { printf 'DIAG-MISMATCH %s\n' "$*"; fail=1; }

cleanup() {
    for p in $PIDS; do kill "$p" 2> /dev/null || true; done
    tc qdisc del dev lo root 2> /dev/null || true
    if [ "$LOADED_MODULE" -eq 1 ]; then rmmod tbtun 2> /dev/null || true; fi
    rm -rf "$TMP"
}
trap cleanup EXIT

if [ "${TBT_COEXIST_ALLOW_LO:-0}" != "1" ]; then
    say "SKIP: optional diagnostic. It shapes init_net loopback and is not an"
    say "      acceptance gate. Run it deliberately on a dedicated host:"
    say "      TBT_COEXIST_ALLOW_LO=1 sudo -E make coexist-diag"
    exit 77
fi
if [ "$(id -u)" -ne 0 ]; then
    say "SKIP: needs root (tc, insmod)."
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

say "coexist-diag: stock TCP default cc = $(sysctl -n net.ipv4.tcp_congestion_control 2>/dev/null || echo unknown)"
say "coexist-diag: loopback bottleneck rate=$RATE delay=$DELAY, secs=$SECS rounds=$ROUNDS"
say "coexist-diag: optional diagnostic only; not an acceptance gate"

# netem first; tbf is the fallback for a kernel without sch_netem.
if ! tc qdisc replace dev lo root netem rate "$RATE" delay "$DELAY" limit 2000 2> "$TMP/tc.err"; then
    if ! tc qdisc replace dev lo root tbf rate "$RATE" burst 64kb latency 20ms 2>> "$TMP/tc.err"; then
        say "SKIP: neither netem nor tbf accepted the lo qdisc: $(cat "$TMP/tc.err")"
        exit 77
    fi
fi

sh -c "echo 'ports $PORTS' > /proc/net/tbtun/ports" || bad "ports directive"

# The far end listens for carriers on 127.0.0.2 and dials the local source.
# The near end owns the public port on 127.0.0.1.
sh -c "echo 'tunnel 1 role=far profile=raw \
    listen=$FAR_IP:$CAR_PORT target=127.0.0.1:$SRC_PORT n=4 m=2 fec=on \
    mode=grouped carriers=6 crypto=off dup=0' > /proc/net/tbtun/config" ||
    bad "far: tunnel config"
sh -c "echo 'tunnel 1 role=near profile=raw \
    listen=$NEAR_IP:$PUB_PORT peer=$FAR_IP:$CAR_PORT n=4 m=2 fec=on \
    mode=grouped carriers=6 crypto=off dup=0 portblock=$PORTBLOCK' \
    > /proc/net/tbtun/config" || bad "near: tunnel config"
[ "$fail" -eq 0 ] || exit 1

sleep 3
if command -v ss > /dev/null; then
    n_cc=$(ss -tin 2>/dev/null | grep -c 'tbt_cc' || true)
    if [ "$n_cc" -eq 0 ]; then
        bad "no socket reports tbt_cc (falls back to BBR?)"
    else
        say "coexist-diag: $n_cc socket(s) report tbt_cc"
    fi
fi

python3 "$PY" sink "$STOCK_PORT" &
PIDS="$PIDS $!"
sleep 0.3

baseline=""
coexist=""
for r in $(seq 1 "$ROUNDS"); do
    b=$(python3 "$PY" send "$NEAR_IP" "$STOCK_PORT" "$SECS")
    say "COEXIST_DIAG round=$r phase=baseline stock_bytes=$b"
    baseline="$baseline $b"
done

for r in $(seq 1 "$ROUNDS"); do
    python3 "$PY" src "$SRC_PORT" "$SECS" > "$TMP/src.$r" 2> "$TMP/src.$r.err" &
    src_pid=$!
    PIDS="$PIDS $src_pid"
    sleep 0.3
    python3 "$PY" recv "$NEAR_IP" "$PUB_PORT" "$SECS" > "$TMP/recv.$r" 2> "$TMP/recv.$r.err" &
    recv_pid=$!
    PIDS="$PIDS $recv_pid"
    c=$(python3 "$PY" send "$NEAR_IP" "$STOCK_PORT" "$SECS")
    wait "$src_pid" 2> /dev/null || true
    wait "$recv_pid" 2> /dev/null || true
    tunnel_bytes=$(cat "$TMP/recv.$r")
    say "COEXIST_DIAG round=$r phase=coexist stock_bytes=$c tbtun_bytes=$tunnel_bytes"
    if [ "${tunnel_bytes:-0}" -eq 0 ]; then
        bad "round $r: tbtun carried no bytes"
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
print(f"{b:.0f} {c:.0f} {drop:.1f} {'WITHIN' if drop <= limit else 'ABOVE'}")
PY
)
set -- $stats
base_mean=${1:-0}
co_mean=${2:-0}
drop_pct=${3:-0}
verdict=${4:-ABOVE}

say "COEXIST_DIAG baseline_bytes=$base_mean coexist_bytes=$co_mean drop_pct=$drop_pct limit_pct=$MAX_DROP_PCT relation=$verdict"
say "coexist-diag: diagnostic only; $verdict the ${MAX_DROP_PCT}% reference, no gate"

if [ "$fail" -eq 0 ]; then
    say "coexist-diag: completed"
    exit 0
fi
say "coexist-diag: diagnostic mismatch"
exit 1
