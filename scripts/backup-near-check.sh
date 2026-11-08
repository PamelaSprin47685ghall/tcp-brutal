#!/bin/sh
# backup-near-check.sh - read-only readiness check for the 760K standby near.
#
# Target: root@192.168.123.74 (AMD Athlon X4 760K, x86_64).
# Primary near: root@192.168.123.85 (LubanCat-5 V2, RK3588).
#
# This script only reads: ping, host key, BatchMode SSH, uname, /proc/cpuinfo,
# lsmod, /dev/net/tun, systemctl is-active, mihomo config path and one grep,
# ip route get, ip rule show. It never installs a key, edits mihomo, adds a
# route/rule, loads a module, or restarts a service.
#
# --plan prints the operator steps and exits without probing.
#
# Exit: 0 READY, 1 NOT_READY, 3 usage.
set -eu

HOST=${BACKUP_NEAR:-root@192.168.123.74}
FINGERPRINT=SHA256:FC60BmjSUvV2UIsINSyjnrtn18iN5eaBdYilyCcmLpU
T=5
PLAN=0
CHECK_FAILED=0

usage() {
    cat <<'EOF'
usage: backup-near-check.sh [options]

  --host USER@IP       standby near (default root@192.168.123.74)
  --fingerprint FP     expected SSH host key (default: 760K ed25519)
  --timeout SECONDS    SSH connect timeout (default 5)
  --plan               print the operation/rollback plan; do not probe
  -h, --help           this text

Checks: reachability, known host key, passwordless root, x86_64, TUN,
tools, tbtun state, mihomo path for 45.11.1.62.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --host) [ "$#" -ge 2 ] || { printf 'need a value for --host\n' >&2; exit 3; }; HOST=$2; shift 2 ;;
        --fingerprint) [ "$#" -ge 2 ] || { printf 'need a value for --fingerprint\n' >&2; exit 3; }; FINGERPRINT=$2; shift 2 ;;
        --timeout) [ "$#" -ge 2 ] || { printf 'need a value for --timeout\n' >&2; exit 3; }; T=$2; shift 2 ;;
        --plan) PLAN=1; shift ;;
        -h | --help) usage; exit 0 ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; exit 3 ;;
    esac
done

IP=${HOST#*@}
case "$IP" in
    192.168.* | 10.* | 172.1[6-9].* | 172.2[0-9].* | 172.3[0-1].*) ;;
    *) printf 'refusing non-private target: %s\n' "$IP" >&2; exit 3 ;;
esac

plan() {
    cat <<'EOF'
PLAN (no changes are made by this script)
 1. Identity: known_hosts fingerprint, uname -m = x86_64, CPU model has 760.
 2. Root key: if BatchMode 'id -u' is not 0, install the operator key once:
      ssh-copy-id -i ~/.ssh/id_ed25519.pub root@192.168.123.74
    Keep PermitRootLogin prohibit-password; never enable password auth.
 3. mihomo bypass for the far address, in this order:
      a. add at the TOP of the rules list:
           - IP-CIDR,45.11.1.62/32,DIRECT,no-resolve
      b. back up the config, run 'mihomo -t', then restart mihomo.
      c. verify: ip route get 45.11.1.62  -> dev eth0, not Meta/198.18.
      d. if it still shows Meta, add the authoritative host rule:
           ip rule add to 45.11.1.62/32 lookup main pref 100
         and persist it (systemd oneshot or NetworkManager dispatcher).
 4. Only one near may be active. Stop the .85 tunnel first, then load the
    matching x86_64 module on .74 and write role=near to /proc/net/tbtun/config.
 5. Rollback: 'del 1', rmmod, delete the pref-100 rule/marker, restore the
    mihomo config backup, restart mihomo, confirm the original route.
EOF
}

if [ "$PLAN" -eq 1 ]; then
    plan
    exit 0
fi

emit() {
    name=$1
    status=$2
    detail=${3-}
    if [ -n "$detail" ]; then
        printf 'BACKUP_CHECK %s %s (%s)\n' "$name" "$status" "$detail"
    else
        printf 'BACKUP_CHECK %s %s\n' "$name" "$status"
    fi
}
ok() { emit "$1" PASS "${2-}"; }
bad() { emit "$1" FAIL "${2-}"; CHECK_FAILED=$((CHECK_FAILED + 1)); }
note() { emit "$1" WARN "${2-}"; }

ssh_run() {
    ssh -o BatchMode=yes -o ConnectTimeout="$T" \
        -o StrictHostKeyChecking=yes "$HOST" "$@" 2>&1
}

if ping -c 1 -W 1 -n "$IP" >/dev/null 2>&1; then
    ok ping
else
    bad ping "no ICMP reply within 1 s"
fi

KNOWN=
if command -v ssh-keygen >/dev/null 2>&1; then
    KNOWN=$(ssh-keygen -F "$IP" -l 2>/dev/null | awk 'NR == 1 { print $2 }')
fi
if [ -z "$KNOWN" ]; then
    bad host_key_known "not in known_hosts; verify the fingerprint out of band"
elif [ -z "$FINGERPRINT" ]; then
    note host_key_known "$KNOWN (check disabled)"
elif [ "$KNOWN" = "$FINGERPRINT" ]; then
    ok host_key_known "$KNOWN"
else
    bad host_key_known "$KNOWN != $FINGERPRINT"
fi

if [ "$CHECK_FAILED" -ne 0 ]; then
    printf 'BACKUP_RESULT NOT_READY (fix host identity first)\n'
    exit 1
fi

UID_OUT=$(ssh_run 'id -u' || true)
UID_OUT=$(printf '%s' "$UID_OUT" | tr -d '\r' | tr -d '\n')
if [ "$UID_OUT" = 0 ]; then
    ok root_passwordless "uid=0"
else
    bad root_passwordless "BatchMode ssh failed: ${UID_OUT:-no output}"
fi

ARCH=$(ssh_run 'uname -m' || true)
ARCH=$(printf '%s' "$ARCH" | tr -d '\r' | tr -d '\n')
if [ "$ARCH" = x86_64 ]; then
    ok arch x86_64
else
    bad arch "${ARCH:-unknown} (expected x86_64)"
fi

MODEL=$(ssh_run "grep -m1 'model name' /proc/cpuinfo" || true)
MODEL=$(printf '%s' "$MODEL" | tr -d '\r')
case "$MODEL" in
    *760*) ok cpu_model "$(printf '%s' "$MODEL" | sed 's/^[^:]*: *//')" ;;
    *) note cpu_model "${MODEL:-unknown} (review: expected 760K)" ;;
esac

TUN=$(ssh_run 'test -c /dev/net/tun && echo yes || echo no' || true)
TUN=$(printf '%s' "$TUN" | tr -d '\r\n')
if [ "$TUN" = yes ]; then
    ok tun_device
else
    bad tun_device "/dev/net/tun missing"
fi

TOOLS=$(ssh_run 'for c in python3 curl ip ss nft iptables; do if command -v "$c" >/dev/null 2>&1; then echo "$c=yes"; else echo "$c=no"; fi; done' || true)
MISSING=$(printf '%s\n' "$TOOLS" | awk -F= '$2 == "no" { printf "%s ", $1 }')
if [ -n "$MISSING" ]; then
    note tools "missing: $MISSING"
else
    ok tools
fi

MODS=$(ssh_run 'lsmod' || true)
TBT=$(printf '%s\n' "$MODS" | awk '$1 == "tbtun" { print }')
if [ -n "$TBT" ]; then
    note tbtun_loaded "standby is not clean: $TBT"
else
    ok tbtun_loaded "not loaded"
fi

MIHOMO_ACTIVE=$(ssh_run 'systemctl is-active mihomo 2>/dev/null || true' || true)
MIHOMO_ACTIVE=$(printf '%s' "$MIHOMO_ACTIVE" | tr -d '\r\n')
case "$MIHOMO_ACTIVE" in
    active) ok mihomo_service active ;;
    '' | unknown | inactive | failed) note mihomo_service "${MIHOMO_ACTIVE:-unknown}" ;;
    *) note mihomo_service "$MIHOMO_ACTIVE" ;;
esac

CFG_ALL=$(ssh_run 'for f in /etc/mihomo/config.yaml /root/.config/mihomo/config.yaml /etc/clash/config.yaml /root/.config/clash/config.yaml; do [ -r "$f" ] && echo "$f"; done' || true)
CFG=$(printf '%s\n' "$CFG_ALL" | awk 'NR == 1 { print }')
if [ -z "$CFG" ]; then
    note mihomo_config "config path not found; check: systemctl cat mihomo"
    note mihomo_direct "not checked"
else
    ok mihomo_config "$CFG"
    RULE=$(ssh_run "grep -n 'IP-CIDR,45.11.1.62/32,DIRECT' '$CFG' 2>/dev/null || true")
    if [ -n "$RULE" ]; then
        ok mihomo_direct "$(printf '%s' "$RULE" | tr -d '\r')"
    else
        bad mihomo_direct "IP-CIDR,45.11.1.62/32,DIRECT is absent"
    fi
fi

ROUTE=$(ssh_run 'ip route get 45.11.1.62 2>/dev/null || true')
ROUTE=$(printf '%s' "$ROUTE" | tr '\n' ' ')
case "$ROUTE" in
    *"dev Meta"* | *"198.18."*) bad far_route "mihomo path: $ROUTE" ;;
    *"dev "*) ok far_route "$ROUTE" ;;
    *) note far_route "no route: $ROUTE" ;;
esac

RULES=$(ssh_run 'ip rule show 2>/dev/null || true')
case "$RULES" in
    *45.11.1.62*) ok far_rule "present" ;;
    *) note far_rule "absent; mihomo DIRECT must carry the bypass" ;;
esac

if [ "$CHECK_FAILED" -eq 0 ]; then
    printf 'BACKUP_RESULT READY\n'
    exit 0
fi
printf 'BACKUP_RESULT NOT_READY (%d failed checks)\n' "$CHECK_FAILED"
exit 1
