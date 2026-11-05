#!/bin/sh
# init - final-bench flush acceptance: two completed 40 MiB transfers, then
# `echo flush > /proc/net/tbtun/config` must return in under one second.
#
# The topology is the adversarial one (loopback only, unshaped): far tunnel
# 19101 -> target 19070, near tunnel public port 19100.  The transfer is
# host/tbt_burst's strict send-then-read phase, the pattern that reproduced
# the old flush hang.
set -u

CFG=/proc/net/tbtun/config
PORTS=/proc/net/tbtun/ports
PUB=19100
CAR=19101
TGT=19070
MIB=40

mkdir -p /proc /sys /dev /tmp
mount -t proc proc /proc || { echo FLUSH_FATAL mount-proc; poweroff -f; }
mount -t sysfs sysfs /sys 2> /dev/null
mount -t devtmpfs devtmpfs /dev 2> /dev/null || mount -t ramfs ramfs /dev
mount -t tmpfs tmpfs /tmp 2> /dev/null
export PATH=/bin
ip link set lo up 2> /dev/null || ifconfig lo 127.0.0.1 up 2> /dev/null
ifconfig lo 2> /dev/null | grep -q 127.0.0.1 || { echo FLUSH_FATAL no-loopback; poweroff -f; }

insmod /tbtun.ko debug=1 || { echo FLUSH_FATAL insmod; poweroff -f; }
[ -e "$CFG" ] || { echo FLUSH_FATAL no-config; poweroff -f; }
echo 'ports 40000-41023' > "$PORTS" 2> /dev/null

/bin/adv_probe target "$TGT" > /dev/null 2>&1 &
sleep 1
dmesg -c > /dev/null 2>&1 || true
echo 3 > /proc/sys/kernel/printk 2> /dev/null || true

configure()
{
    echo flush > "$CFG" 2> /dev/null
    sleep 1
    echo "tunnel 1 role=far profile=raw listen=127.0.0.1:$CAR target=127.0.0.1:$TGT n=4 m=0" > "$CFG" ||
        { echo FLUSH_FATAL far-config; return 1; }
    echo "tunnel 2 role=near profile=raw listen=127.0.0.1:$PUB peer=127.0.0.1:$CAR n=4 m=0" > "$CFG" ||
        { echo FLUSH_FATAL near-config; return 1; }
    sleep 1
}

now()
{
    cut -d' ' -f1 /proc/uptime
}

configure || { echo FLUSH_FATAL configure; poweroff -f; }

i=1
while [ "$i" -le 2 ]; do
    echo "XFER${i}_START uptime=$(now)"
    /bin/tbt_burst 127.0.0.1 "$PUB" "$MIB" 120
    echo "XFER${i}_END rc=$? uptime=$(now)"
    i=$((i + 1))
done

echo "FLUSH_CONFIG_BEFORE $(cat "$CFG" | tr '\n' ';')"
_t0=$(now)
( echo flush > "$CFG" ) &
_fp=$!
_i=0
while kill -0 "$_fp" 2> /dev/null && [ "$_i" -lt 100 ]; do
    sleep 0.05
    _i=$((_i + 1))
done
_t1=$(now)
_hung=0
kill -0 "$_fp" 2> /dev/null && _hung=1
_secs=$(awk -v a="$_t0" -v b="$_t1" 'BEGIN { printf "%.2f", b - a }')
echo "FLUSH_RESULT hung=$_hung start=$_t0 end=$_t1 secs=$_secs"
echo "FLUSH_CONFIG_AFTER $(cat "$CFG" 2> /dev/null | tr '\n' ';')"

if [ "$_hung" -eq 1 ]; then
    _pid=$(cat /proc/net/tbtun/stats 2> /dev/null | head -n 1)
    echo "FLUSH_BLOCKED wchan=$(cat "/proc/$_fp/wchan" 2> /dev/null) syscall=$(cat "/proc/$_fp/syscall" 2> /dev/null | tr ' ' '_')"
    kill -9 "$_fp" 2> /dev/null
    echo "FLUSH_VERDICT FAIL"
else
    echo "FLUSH_VERDICT PASS"
fi

echo "TBT_ADV_DONE"
poweroff -f
