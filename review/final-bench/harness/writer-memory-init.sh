#!/bin/sh
# init - final-bench writer acceptance with a memory ledger.
#
# Runs the adversarial pub.writer cases (100 MiB, then 1 GiB) on a raw n=1
# tunnel, and records MemAvailable before, after, and after a 5 s quiesce.
# A background sampler records the minimum MemAvailable while the writer
# pushes bytes.  adv_probe writer prints the accepted byte count and errno.
set -u

CFG=/proc/net/tbtun/config
PORTS=/proc/net/tbtun/ports
PUB=19100
CAR=19101
TGT=19070

mkdir -p /proc /sys /dev /tmp
mount -t proc proc /proc || { echo WRITER_FATAL mount-proc; poweroff -f; }
mount -t sysfs sysfs /sys 2> /dev/null
mount -t devtmpfs devtmpfs /dev 2> /dev/null || mount -t ramfs ramfs /dev
mount -t tmpfs tmpfs /tmp 2> /dev/null
export PATH=/bin
ip link set lo up 2> /dev/null || ifconfig lo 127.0.0.1 up 2> /dev/null
ifconfig lo 2> /dev/null | grep -q 127.0.0.1 || { echo WRITER_FATAL no-loopback; poweroff -f; }

insmod /tbtun.ko debug=1 || { echo WRITER_FATAL insmod; poweroff -f; }
[ -e "$CFG" ] || { echo WRITER_FATAL no-config; poweroff -f; }
echo 'ports 40000-41023' > "$PORTS" 2> /dev/null

/bin/adv_probe target "$TGT" > /dev/null 2>&1 &
sleep 1
dmesg -c > /dev/null 2>&1 || true
echo 3 > /proc/sys/kernel/printk 2> /dev/null || true

configure()
{
    echo flush > "$CFG" 2> /dev/null
    sleep 1
    echo "tunnel 1 role=far profile=raw listen=127.0.0.1:$CAR target=127.0.0.1:$TGT n=1 m=0" > "$CFG" ||
        { echo WRITER_FATAL far-config; return 1; }
    echo "tunnel 2 role=near profile=raw listen=127.0.0.1:$PUB peer=127.0.0.1:$CAR n=1 m=0" > "$CFG" ||
        { echo WRITER_FATAL near-config; return 1; }
    sleep 1
}

mem_avail()
{
    awk '/^MemAvailable:/ { print $2 }' /proc/meminfo
}

now()
{
    cut -d' ' -f1 /proc/uptime
}

case_run()
{
    _mib=$1
    _tmo=$2
    configure || { echo WRITER_FATAL configure; poweroff -f; }
    _before=$(mem_avail)
    : > /tmp/mem_sample
    ( while :; do mem_avail >> /tmp/mem_sample; sleep 0.2; done ) &
    _sp=$!
    _t0=$(now)
    timeout "$_tmo" /bin/adv_probe writer "$PUB" "$_mib" 0 > /tmp/writer.out 2>&1
    _rc=$?
    _t1=$(now)
    kill "$_sp" 2> /dev/null
    wait "$_sp" 2> /dev/null
    _after=$(mem_avail)
    sleep 5
    _quiesce=$(mem_avail)
    _min=$(sort -n /tmp/mem_sample 2> /dev/null | head -n 1)
    echo "WRITER_CASE mib=$_mib rc=$_rc start=$_t0 end=$_t1 secs=$(awk -v a="$_t0" -v b="$_t1" 'BEGIN { printf "%.2f", b - a }')"
    echo "WRITER_OUT $(cat /tmp/writer.out 2> /dev/null | tr '\n' '|')"
    echo "MEM_AVAIL mib=$_mib before=$_before min=$_min after=$_after quiesce=$_quiesce kB"
    echo "STATS mib=$_mib $(grep '^tunnel ' /proc/net/tbtun/stats 2> /dev/null | head -n 1)"
}

case_run 100 120
case_run 1024 180

# The writer's held bytes are freed by a tunnel flush, not by the closed
# stream alone: sample before and after so the recovery is explicit.
echo "MEM_BEFORE_FINAL_FLUSH $(mem_avail)"
echo flush > "$CFG"
sleep 1
echo "MEM_AFTER_FINAL_FLUSH $(mem_avail)"

echo "TBT_ADV_DONE"
poweroff -f
