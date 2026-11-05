#!/bin/sh
# init - idle CPU baseline for the final-bench CPU reading.
#
# Configures a raw n=4 tunnel and measures the guest's system+irq+softirq and
# kworker ticks over 20 s with no transfer, so the transfer-time kworker load
# can be separated from a module timer that spins while merely configured.
set -u

CFG=/proc/net/tbtun/config
PORTS=/proc/net/tbtun/ports
PUB=19100
CAR=19101
TGT=19070

mkdir -p /proc /sys /dev /tmp
mount -t proc proc /proc || { echo IDLE_FATAL mount-proc; poweroff -f; }
mount -t sysfs sysfs /sys 2> /dev/null
mount -t devtmpfs devtmpfs /dev 2> /dev/null || mount -t ramfs ramfs /dev
mount -t tmpfs tmpfs /tmp 2> /dev/null
export PATH=/bin
ip link set lo up 2> /dev/null || ifconfig lo 127.0.0.1 up 2> /dev/null

insmod /tbtun.ko debug=0 || { echo IDLE_FATAL insmod; poweroff -f; }
echo 'ports 40000-41023' > "$PORTS" 2> /dev/null
/bin/adv_probe target "$TGT" > /dev/null 2>&1 &
sleep 1
echo 3 > /proc/sys/kernel/printk 2> /dev/null || true

cpu_now()
{
    awk '/^cpu / { busy = $4 + $7 + $8; printf "%d\n", busy }' /proc/stat
}

kw_ticks()
{
    awk '
        {
            c = substr($0, index($0, "(") + 1)
            c = substr(c, 1, index(c, ")") - 1)
            if (c !~ /^kworker/) next
            rest = substr($0, index($0, ")") + 2)
            split(rest, a, " ")
            s += a[12] + a[13]
        }
        END { print s + 0 }' /proc/[0-9]*/stat
}

sample()
{
    _tag=$1
    _b=$(cpu_now)
    _k=$(kw_ticks)
    _t0=$(cut -d' ' -f1 /proc/uptime)
    sleep 20
    _b2=$(cpu_now)
    _k2=$(kw_ticks)
    _t1=$(cut -d' ' -f1 /proc/uptime)
    awk -v tag="$_tag" -v db="$((_b2 - _b))" -v dk="$((_k2 - _k))" \
        -v a="$_t0" -v b="$_t1" 'BEGIN {
            t = b - a; if (t <= 0) t = 1;
            printf "IDLE_CPU tag=%s wall=%.2f busy_pct=%.1f kworker_pct=%.1f\n",
                   tag, t, db / t, dk / t }'
}

sample no_tunnel

echo "tunnel 1 role=far profile=raw listen=127.0.0.1:$CAR target=127.0.0.1:$TGT n=4 m=0" > "$CFG"
echo "tunnel 2 role=near profile=raw listen=127.0.0.1:$PUB peer=127.0.0.1:$CAR n=4 m=0" > "$CFG"
sleep 5
sample configured_idle

echo "TBT_ADV_DONE"
poweroff -f
