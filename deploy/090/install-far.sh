#!/bin/sh
# deploy/090/install-far.sh
# Far side only: mihomo + tbtun.ko. Never installs tbt_client.
#
#   ./install-far.sh            install, start mihomo, then modprobe and write
#                               the tunnel line (mihomo must bind 19001 first)
#   ./install-far.sh --no-start install files only
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
start=1
case "${1:-}" in
    --no-start) start=0 ;;
    --start|"") start=1 ;;
    *) echo "usage: $0 [--no-start]" >&2; exit 2 ;;
esac

if [ "$(id -u)" -ne 0 ]; then
    echo "install-far: run as root (writes /usr/local, /lib/modules, /proc)" >&2
    exit 1
fi

for f in mihomo tbtun.ko config.yaml tbtun.conf; do
    [ -f "$here/$f" ] || { echo "install-far: missing $here/$f" >&2; exit 1; }
done

mkdir -p /usr/local/bin /etc/mihomo-090 /var/log/tbtun-090 /run/tbtun-090
mkdir -p "/lib/modules/$(uname -r)/extra"
install -m 0755 "$here/mihomo" /usr/local/bin/mihomo
install -m 0644 "$here/tbtun.ko" "/lib/modules/$(uname -r)/extra/tbtun.ko"
depmod -a
install -m 0644 "$here/config.yaml" /etc/mihomo-090/config.yaml
install -m 0644 "$here/tbtun.conf"  /etc/tbtun-090.conf

vless=$(sed -n 's/^[[:space:]]*port:[[:space:]]*\([0-9][0-9]*\).*/\1/p' /etc/mihomo-090/config.yaml | head -n 1)
[ -n "$vless" ] || { echo "install-far: no port in config.yaml" >&2; exit 1; }

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
    # 1. mihomo binds the VLESS inbound first
    if [ -f /run/tbtun-090/mihomo.pid ] && \
       kill -0 "$(cat /run/tbtun-090/mihomo.pid)" 2>/dev/null; then
        echo "install-far: mihomo already running"
    else
        nohup /usr/local/bin/mihomo -d /etc/mihomo-090 -f /etc/mihomo-090/config.yaml \
            >>/var/log/tbtun-090/mihomo.log 2>&1 &
        echo $! > /run/tbtun-090/mihomo.pid
    fi
    if ! wait_port "$vless" 50; then
        echo "install-far: mihomo did not bind 127.0.0.1:$vless" >&2
        tail -n 50 /var/log/tbtun-090/mihomo.log >&2 || true
        exit 1
    fi

    # 2. module: modprobe uses the depmod entry just installed; insmod is a fallback
    modprobe tbtun 2>/dev/null || insmod "/lib/modules/$(uname -r)/extra/tbtun.ko"
    [ -d /proc/net/tbtun ] || { echo "install-far: /proc/net/tbtun missing after load" >&2; exit 1; }

    # 3. apply the tunnel line
    cat /etc/tbtun-090.conf > /proc/net/tbtun/config
    echo "install-far: ready (VLESS 127.0.0.1:$vless, carriers on the listen= port)"
    echo "verify: cat /proc/net/tbtun/config"
    echo "verify: lsmod | grep tbtun"
else
    echo "installed. start order:"
    echo "  mihomo -d /etc/mihomo-090 -f /etc/mihomo-090/config.yaml  # bind 19001 first"
    echo "  modprobe tbtun"
    echo "  cat /etc/tbtun-090.conf > /proc/net/tbtun/config"
fi
