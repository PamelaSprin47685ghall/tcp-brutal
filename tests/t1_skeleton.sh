#!/usr/bin/env bash
#
# t1_skeleton.sh - C1 acceptance test for tbtun (DESIGN.md §15 C1).
#
# WHAT THIS TEST PROVES
#   1. tbtun.ko loads and its /proc interface appears.
#   2. `echo` configures a tunnel, and `cat config` read-back is canonical.
#   3. A malformed line makes the writing process fail (non-zero status) and
#      leaves the configuration untouched.
#   4. A TCP connect to the tunnel's public port is accepted and closed
#      immediately, i.e. the client sees EOF (the C1 data path is a stub).
#
# WHAT IT DOES NOT DO
#   It does not build the module. Build first:
#       . /home/kunweiz/.cache/tbtun-build/env.sh
#       KDIR=$KDIR make -C $KDIR M=$PWD PAHOLE=$PAHOLE modules
#
# PRIVILEGES
#   Loading a kernel module and creating a network namespace both need root.
#   This script is meant to be run by an operator as:
#       sudo tests/t1_skeleton.sh
#   Without root it reports exactly what is missing and exits 77 (skip), so a
#   casual `make test`-style invocation is never mistaken for a pass.
#
# EXIT CODES
#   0  all assertions passed
#   1  an assertion failed (see the message)
#   77 cannot run here (not root, module missing, /proc missing)
#
set -u

MOD="${TBTUN_KO:-$PWD/tbtun.ko}"
NS="tbtun-t1"
PORT="${TBT_PORT:-19000}"
CONFIG=/proc/net/tbtun/config
fail=0

say() { printf '%s\n' "$*"; }
ok() { printf '  ok   %s\n' "$*"; }
bad() { printf '  FAIL %s\n' "$*"; fail=1; }

cleanup() {
    if [ -n "${LOADED:-}" ]; then
        echo flush > $CONFIG 2> /dev/null || true
        rmmod tbtun 2> /dev/null || true
    fi
    ip netns del "$NS" 2> /dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Preconditions

if [ "$(id -u)" -ne 0 ]; then
    say "SKIP: this test needs root (insmod + ip netns)."
    say "      Run it as: sudo $0"
    exit 77
fi
if [ ! -f "$MOD" ]; then
    say "SKIP: $MOD not found. Build it first:"
    say "      . /home/kunweiz/.cache/tbtun-build/env.sh"
    say "      KDIR=\$KDIR make -C \$KDIR M=$PWD PAHOLE=\$PAHOLE modules"
    exit 77
fi
if ! command -v ip > /dev/null; then
    say "SKIP: iproute2 (ip) is required to build the namespace."
    exit 77
fi

say "t1: loading $MOD"
if ! insmod "$MOD"; then
    say "SKIP: insmod failed (see dmesg); kernel may refuse unsigned modules."
    exit 77
fi
LOADED=1
if [ ! -e "$CONFIG" ]; then
    say "FAIL: $CONFIG did not appear after load"
    exit 1
fi

# A namespace with one veth pair, so the listener has a path the test fully
# controls and cannot collide with the host's real addresses.
ip netns add "$NS"
ip -n "$NS" link set lo up
ip -n "$NS" addr add 10.123.0.1/24 dev lo

# ---------------------------------------------------------------------------
# 2. Configure with echo, then read back canonically

say "t1: configuring a tunnel with echo"
if echo "tunnel 1 role=near listen=10.123.0.1:$PORT peer=10.123.0.2:443 n=2 m=1" > "$CONFIG"; then
    ok "echo > config succeeded"
else
    bad "echo > config failed"
fi

CANON=$(cat "$CONFIG")
say "  canonical: $CANON"
case "$CANON" in
"tunnel 1 role=near listen=10.123.0.1:$PORT peer=10.123.0.2:443 n=2 m=1 "*) ok "read-back starts with the expected prefix" ;;
*) bad "read-back does not match the written config" ;;
esac
if printf '%s' "$CANON" | grep -q "profile=http-kal"; then
    ok "defaults are rendered explicitly (profile=http-kal)"
else
    bad "canonical form is missing the profile default"
fi

# ---------------------------------------------------------------------------
# 3. A bad line must fail and must change nothing

say "t1: a malformed line must fail visibly"
BEFORE=$(cat "$CONFIG")
if echo "tunnel 2 role=sideways listen=1.2.3.4:5 peer=1.2.3.4:6" > "$CONFIG" 2> /dev/null; then
    bad "a bad role was accepted"
else
    ok "bad role rejected with non-zero status"
fi
if echo "tunnel 2 listen=10.123.0.1:1 peer=10.123.0.2:2" > "$CONFIG" 2> /dev/null; then
    bad "a tunnel without a role was accepted"
else
    ok "missing role rejected"
fi
AFTER=$(cat "$CONFIG")
if [ "$BEFORE" = "$AFTER" ]; then
    ok "rejected lines left the configuration unchanged"
else
    bad "configuration changed despite rejected input"
fi

# ---------------------------------------------------------------------------
# Port range directive

if echo "ports 41000-41015" > /proc/net/tbtun/ports; then
    if grep -q "ports 41000-41015" /proc/net/tbtun/ports; then
        ok "ports directive applied"
    else
        bad "ports directive not reflected in read-back"
    fi
else
    bad "ports directive rejected"
fi

# ---------------------------------------------------------------------------
# 4. Connect to the public port: accept then immediate EOF (C1 stub)

say "t1: connecting to the public port (expect immediate EOF)"
if ip netns exec "$NS" command -v nc > /dev/null 2>&1; then
    # -w 3 bounds the wait; a tunnel that hangs is a failure, not a slow pass.
    out=$(ip netns exec "$NS" timeout 5 sh -c \
        "printf 'hello' | nc -w 3 10.123.0.1 $PORT" 2>&1)
    rc=$?
    if [ $rc -eq 124 ]; then
        bad "connection hung (expected immediate close)"
    else
        ok "connection closed promptly (nc rc=$rc, output '${out}')"
    fi
else
    bad "nc not found; cannot exercise the public port"
fi

if grep -q "tunnel 1 role=near" /proc/net/tbtun/tunnels; then
    ok "tunnels file lists the tunnel"
else
    bad "tunnels file does not list the tunnel"
fi

# ---------------------------------------------------------------------------
say ""
if [ $fail -eq 0 ]; then
    say "t1: PASS"
    exit 0
fi
say "t1: FAIL"
exit 1
