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

port=$(sed -n 's/.*listen=127\.0\.0\.1:\([0-9][0-9]*\).*/\1/p' /etc/tbtun-090.conf | head -n 1)
[ -n "$port" ] || { echo "install-near: no listen=127.0.0.1:PORT in tbtun.conf" >&2; exit 1; }

wait_port() {
    p=$1
    n=${2:-50}
    while [ "$n" -gt 0 ]; do
        if ss -lnt 2>/dev/null | awk '{print $4}' | grep -q ":$p\$"; then
            return 0
        fi
        sleep 0.2
        n=$((n - 1))
    done
    return 1
}

if [ "$start" -eq 1 ]; then
    # 1. userspace client listens on 19000
    # tbt_client accepts --config FILE only as argv[1]; the file's first
    # non-comment line is the tunnel line (host/tbt_client_main.c:128-170).
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
