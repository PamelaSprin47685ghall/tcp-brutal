#!/usr/bin/env bash
#
# t2_raw_echo.sh - C2 acceptance test: one carrier, raw profile, duplex echo.
#
# PRE-REQUISITE, BY THE OPERATOR
#   The module must already be loaded by root:
#       sudo insmod tbtun.ko
#   This script cannot do that itself: insmod needs CAP_SYS_MODULE in the
#   initial user namespace, which no unshare/namespace trick grants. Run the
#   whole script as root:
#
#       sudo tests/t2_raw_echo.sh
#
#   It builds nothing. Build first with `make check`.
#
# WHAT IT PROVES (DESIGN.md §15 C2, §10.2 T2/T3)
#   - a near tunnel with one raw carrier reaches a far tunnel
#   - the far tunnel connects to a literal target
#   - 64 MiB of random bytes cross and come back byte-identical (sha256)
#   - a second, concurrent stream also completes (duplex, no deadlock)
#
# TOPOLOGY
#   netns tbt2-near 10.71.0.1 <-> 10.71.0.2 netns tbt2-far
#   app in the far netns listens on 127.0.0.1:7070 (echo)
#   client in the near netns connects to 10.71.0.1:9700 (the tunnel)
#
# EXIT CODES: 0 pass, 1 assertion failed, 77 cannot run here.
#
set -u

NEAR=tbt2-near
FAR=tbt2-far
PUB_PORT=9700
CAR_PORT=9701
TARGET_PORT=7070
SIZE_MB=64

fail=0
ok() { printf '  ok   %s\n' "$*"; }
bad() { printf '  FAIL %s\n' "$*"; fail=1; }
say() { printf '%s\n' "$*"; }

cleanup() {
    ip netns del "$NEAR" 2> /dev/null || true
    ip netns del "$FAR" 2> /dev/null || true
}
trap cleanup EXIT

if [ "$(id -u)" -ne 0 ]; then
    say "SKIP: needs root (netns + a module already loaded by root)."
    say "      sudo $0"
    exit 77
fi
if ! grep -q '^tbtun ' /proc/modules; then
    say "SKIP: tbtun is not loaded. Load it first:"
    say "      sudo insmod $PWD/tbtun.ko"
    exit 77
fi
for c in ip python3 sha256sum; do
    command -v "$c" > /dev/null || { say "SKIP: $c missing"; exit 77; }
done

say "t2: building two namespaces"
ip netns add "$NEAR"
ip netns add "$FAR"
ip -n "$NEAR" link add v0 type veth peer name v1 netns "$FAR"
ip -n "$NEAR" addr add 10.71.0.1/24 dev v0
ip -n "$FAR" addr add 10.71.0.2/24 dev v1
ip -n "$NEAR" link set lo up
ip -n "$FAR" link set lo up
ip -n "$NEAR" link set v0 up
ip -n "$FAR" link set v1 up

say "t2: configuring both ends with echo"
ip netns exec "$NEAR" sh -c "echo 'ports 42000-42015' > /proc/net/tbtun/ports" \
    || bad "near: ports directive"
ip netns exec "$FAR" sh -c "echo 'ports 42000-42015' > /proc/net/tbtun/ports" \
    || bad "far: ports directive"

# Both ends declare their own listen address; the far end is what the near end
# dials as peer=, and the near end's public port is what the client uses.
ip netns exec "$FAR" sh -c "echo 'tunnel 1 role=far profile=raw \
    listen=10.71.0.2:$CAR_PORT target=127.0.0.1:$TARGET_PORT n=1 m=0' \
    > /proc/net/tbtun/config" || bad "far: tunnel config"
ip netns exec "$NEAR" sh -c "echo 'tunnel 1 role=near profile=raw \
    listen=10.71.0.1:$PUB_PORT peer=10.71.0.2:$CAR_PORT n=1 m=0' \
    > /proc/net/tbtun/config" || bad "near: tunnel config"

say "  far  config: $(ip netns exec "$FAR" cat /proc/net/tbtun/config)"
say "  near config: $(ip netns exec "$NEAR" cat /proc/net/tbtun/config)"

say "t2: starting the echo target in the far netns"
cat > /tmp/tbt_echo.py <<'PY'
import socket, sys, threading
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(8)
def serve(c):
    with c:
        while True:
            b = c.recv(65536)
            if not b:
                return
            c.sendall(b)
while True:
    c, _ = s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
PY
ip netns exec "$FAR" python3 /tmp/tbt_echo.py "$TARGET_PORT" &
ECHO_PID=$!
sleep 0.5

say "t2: 64 MiB round trip"
head -c $((SIZE_MB * 1024 * 1024)) /dev/urandom > /tmp/tbt_src.bin
SRC_SUM=$(sha256sum /tmp/tbt_src.bin | cut -d' ' -f1)

cat > /tmp/tbt_client.py <<'PY'
import socket, sys, hashlib
host, port = sys.argv[1], int(sys.argv[2])
data = open("/tmp/tbt_src.bin", "rb").read()
s = socket.create_connection((host, port), timeout=30)
sent = 0
got = bytearray()
import threading
def reader():
    while len(got) < len(data):
        b = s.recv(1 << 16)
        if not b:
            break
        got.extend(b)
t = threading.Thread(target=reader, daemon=True); t.start()
s.sendall(data)
t.join(60)
print(hashlib.sha256(bytes(got[:len(data)])).hexdigest())
print(len(got))
PY

OUT=$(ip netns exec "$NEAR" timeout 90 python3 /tmp/tbt_client.py 10.71.0.1 "$PUB_PORT" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/    /'
if [ $RC -ne 0 ]; then
    bad "round trip failed (rc=$RC)"
else
    GOT_SUM=$(echo "$OUT" | head -1)
    if [ "$GOT_SUM" = "$SRC_SUM" ]; then
        ok "64 MiB returned byte-identical"
    else
        bad "sha256 mismatch: sent $SRC_SUM got $GOT_SUM"
    fi
fi

say "t2: two concurrent streams"
PIDS=""
for i in 1 2; do
    ip netns exec "$NEAR" timeout 60 python3 /tmp/tbt_client.py 10.71.0.1 "$PUB_PORT" \
        > /tmp/tbt_c$i.out 2>&1 &
    PIDS="$PIDS $!"
done
for p in $PIDS; do
    wait "$p" || bad "concurrent stream failed"
done
kill "$ECHO_PID" 2> /dev/null

say "t2: tunnel state"
ip netns exec "$NEAR" cat /proc/net/tbtun/tunnels | sed 's/^/    /'
ip netns exec "$NEAR" cat /proc/net/tbtun/stats | sed 's/^/    /'

say ""
if [ $fail -eq 0 ]; then
    say "t2: PASS"
    exit 0
fi
say "t2: FAIL"
exit 1
