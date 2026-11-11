#!/bin/sh
# deploy/090/install-near.sh
# Near side only: mihomo + tbt_client. Never installs or loads tbtun.ko.
#
#   ./install-near.sh            install, then start tbt_client and mihomo in
#                                the required order (client first)
#   ./install-near.sh --no-start install files only
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
start=1
case "${1:-}" in
    --no-start) start=0 ;;
    --start|"") start=1 ;;
    *) echo "usage: $0 [--no-start]" >&2; exit 2 ;;
esac

if [ "$(id -u)" -ne 0 ]; then
    echo "install-near: run as root (writes /usr/local, /etc)" >&2
    exit 1
fi

for f in mihomo tbt_client config.yaml tbtun.conf; do
    [ -f "$here/$f" ] || { echo "install-near: missing $here/$f" >&2; exit 1; }
done

mkdir -p /usr/local/bin /etc/mihomo-090 /var/log/tbtun-090 /run/tbtun-090
install -m 0755 "$here/mihomo"     /usr/local/bin/mihomo
install -m 0755 "$here/tbt_client" /usr/local/bin/tbt_client
install -m 0644 "$here/config.yaml" /etc/mihomo-090/config.yaml
install -m 0644 "$here/tbtun.conf"  /etc/tbtun-090.conf

# Optional systemd units. Minimal images (Alpine, OpenWrt) have no systemd;
# the pid-file start below stays the supported smoke path there.
if [ -d "$here/systemd" ] && [ -d /run/systemd/system ] && \
   command -v systemctl >/dev/null 2>&1; then
    for u in "$here"/systemd/*.service; do
        [ -f "$u" ] || continue
        install -m 0644 "$u" /etc/systemd/system/
    done
    systemctl daemon-reload
    echo "install-near: systemd units installed"
    echo "install-near: enable with: systemctl enable --now tbt-client-090 mihomo-090-near"
fi

# A systemd-managed deployment must not race pid-file instances of the same
# two binaries. Checked only when --start would spawn them.
systemd_owns() {
    [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1 && \
        systemctl is-active --quiet "$1" 2>/dev/null
}

port=$(sed -n 's/.*listen=127\.0\.0\.1:\([0-9][0-9]*\).*/\1/p' /etc/tbtun-090.conf | head -n 1)
[ -n "$port" ] || { echo "install-near: no listen=127.0.0.1:PORT in tbtun.conf" >&2; exit 1; }

# Is one TCP port in state LISTEN? Prefer ss; minimal images ship without
# iproute2, so fall back to the kernel's own table: /proc/net/tcp field 2 is
# HEXADDR:HEXPORT and field 4 is the state, where 0A means LISTEN.
port_is_listening() {
    p=$1
    if command -v ss >/dev/null 2>&1; then
        ss -lnt 2>/dev/null | awk -v suffix=":$p" \
            'length($4) >= length(suffix) && substr($4, length($4) - length(suffix) + 1) == suffix { found = 1 }
             END { exit found ? 0 : 1 }'
        return $?
    fi
    hex=$(printf '%04X' "$p")
    for f in /proc/net/tcp /proc/net/tcp6; do
        [ -r "$f" ] || continue
        awk -v suffix=":$hex" \
            '$4 == "0A" && length($2) >= length(suffix) && substr($2, length($2) - length(suffix) + 1) == suffix { found = 1 }
             END { exit found ? 0 : 1 }' "$f" && return 0
    done
    return 1
}

wait_port() {
    p=$1
    n=${2:-50}
    while [ "$n" -gt 0 ]; do
        if port_is_listening "$p"; then
            return 0
        fi
        sleep 0.2
        n=$((n - 1))
    done
    return 1
}

if [ "$start" -eq 1 ]; then
    if systemd_owns tbt-client-090 || systemd_owns mihomo-090-near; then
        echo "install-near: systemd owns this deployment; not starting pid-file instances"
        echo "install-near: roll out with: systemctl restart tbt-client-090 mihomo-090-near"
        exit 0
    fi
    # 1. userspace client listens on 19000
    # tbt_client accepts --config FILE only as argv[1]; the file's first
    # non-comment line is the tunnel line (src/client/tbt_client_main.c:128-170).
    if [ -f /run/tbtun-090/tbt_client.pid ] && \
       kill -0 "$(cat /run/tbtun-090/tbt_client.pid)" 2>/dev/null; then
        echo "install-near: tbt_client already running"
    else
        nohup /usr/local/bin/tbt_client --config /etc/tbtun-090.conf \
            >>/var/log/tbtun-090/tbt_client.log 2>&1 &
        echo $! > /run/tbtun-090/tbt_client.pid
    fi
    if ! wait_port "$port" 50; then
        echo "install-near: tbt_client did not listen on $port" >&2
        tail -n 50 /var/log/tbtun-090/tbt_client.log >&2 || true
        exit 1
    fi

    # 2. mihomo may now dial 127.0.0.1:$port
    if [ -f /run/tbtun-090/mihomo.pid ] && \
       kill -0 "$(cat /run/tbtun-090/mihomo.pid)" 2>/dev/null; then
        echo "install-near: mihomo already running"
    else
        nohup /usr/local/bin/mihomo -d /etc/mihomo-090 -f /etc/mihomo-090/config.yaml \
            >>/var/log/tbtun-090/mihomo.log 2>&1 &
        echo $! > /run/tbtun-090/mihomo.pid
    fi

    mixed=$(sed -n 's/^mixed-port:[[:space:]]*\([0-9][0-9]*\).*/\1/p' /etc/mihomo-090/config.yaml | head -n 1)
    if [ -n "$mixed" ] && wait_port "$mixed" 50; then
        echo "install-near: ready (tbt_client :$port, mihomo :$mixed)"
    else
        echo "install-near: mihomo did not open mixed-port $mixed; see /var/log/tbtun-090/mihomo.log" >&2
        exit 1
    fi
else
    echo "installed. start order:"
    echo "  tbt_client --config /etc/tbtun-090.conf   # must listen first"
    echo "  mihomo -d /etc/mihomo-090 -f /etc/mihomo-090/config.yaml"
fi
