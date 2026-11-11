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

# Optional systemd units. Minimal images (Alpine, OpenWrt) have no systemd;
# the pid-file start below stays the supported smoke path there.
if [ -d "$here/systemd" ] && [ -d /run/systemd/system ] && \
   command -v systemctl >/dev/null 2>&1; then
    for u in "$here"/systemd/*.service; do
        [ -f "$u" ] || continue
        install -m 0644 "$u" /etc/systemd/system/
    done
    systemctl daemon-reload
    echo "install-far: systemd units installed"
    echo "install-far: enable with: systemctl enable --now mihomo-090-far tbtun-090-far"
fi

# A systemd-managed deployment must not race pid-file instances of the same
# two components. Checked only when --start would spawn them.
systemd_owns() {
    [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1 && \
        systemctl is-active --quiet "$1" 2>/dev/null
}

vless=$(sed -n 's/^[[:space:]]*port:[[:space:]]*\([0-9][0-9]*\).*/\1/p' /etc/mihomo-090/config.yaml | head -n 1)
[ -n "$vless" ] || { echo "install-far: no port in config.yaml" >&2; exit 1; }

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
    if systemd_owns mihomo-090-far || systemd_owns tbtun-090-far; then
        echo "install-far: systemd owns this deployment; not starting pid-file instances"
        echo "install-far: roll out with: systemctl restart mihomo-090-far tbtun-090-far"
        exit 0
    fi
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
    # BBR is a soft requirement: tbt_car_set_bbr() falls back to the kernel
    # default when the algorithm is missing, but every carrier then runs the
    # default CC and long-fat throughput drops. Load it before the module so no
    # carrier exists without the choice.
    if ! modprobe tcp_bbr 2>/dev/null; then
        avail=$(sysctl -n net.ipv4.tcp_available_congestion_control 2>/dev/null || echo unknown)
        echo "install-far: WARNING: tcp_bbr is unavailable on this kernel" >&2
        echo "install-far: WARNING: available congestion control: $avail" >&2
        echo "install-far: WARNING: carriers will run the default CC; build a kernel with CONFIG_TCP_CONG_BBR=m/y" >&2
    fi
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
