#!/bin/sh
# Borrowed-shell retransmission: netem sweep.
#
# For each module mode (shell_retx=0/1) and each netem condition, run one
# byte-exact file transfer through a near/far pair on loopback carriers, then
# print the shell and FEC counters. App and target TCP run over a veth pair,
# so netem on lo shapes only the carrier path.
#
# The test host lock is held for the whole sweep.
set -u

ROOT=/root/shell-retx
KO=$ROOT/tbtun.ko
HERE=$ROOT/benches/shell-retx
TMP=$ROOT/tmp/sweep
OUT=$ROOT/benches/shell-retx/results
LOCK=/root/tbt-testhost.lock
SIZE=${1:-8388608}
NETEMS=${2:-"none loss1 loss5 loss20 reorder10"}
TGT_PORT=19620
FAR_PORT=19610
NEAR_PORT=19600
PB_LO=19650
PB_HI=19699
NS=tbtapp

mkdir -p "$TMP" "$OUT"
export TMPDIR=$TMP
SRV_PID=
exec 9>"$LOCK"
flock -w 3600 9 || { echo "LOCK_TIMEOUT"; exit 9; }

say() { printf '%s\n' "$*"; }
die() { say "FATAL: $*"; exit 1; }

cleanup_net() {
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2> /dev/null
    SRV_PID=
    timeout 15 sh -c 'echo flush > /proc/net/tbtun/config' 2> /dev/null || true
    sleep 1
    rmmod tbtun 2> /dev/null || true
    tc qdisc del dev lo root 2> /dev/null || true
    ip netns del "$NS" 2> /dev/null || true
    ip link del sa0 2> /dev/null || true
}
cleanup() { cleanup_net; rm -f "$TMP"/out.bin; }
trap cleanup EXIT INT TERM

# Extract one key=value from the shell line of a tunnel.
shell_kv() {
    tunnel=$1
    key=$2
    awk -v t="$tunnel" -v k="$key" '$1=="tunnel" && $2==t && $3=="shell" {
        for (i=4;i<=NF;i++) if (index($i,k"=")==1) { sub("^"k"=","",$i); print $i; exit }
    }' /proc/net/tbtun/stats 2> /dev/null | tail -1
}

one_case() {
    mode=$1
    netem=$2
    tag="mode${mode}-${netem}"
    case_dir=$OUT/sweep-$tag
    mkdir -p "$case_dir"

    cleanup_net
    say "== $tag size=$SIZE"

    [ -f "$TMP/src.bin" ] || head -c "$SIZE" /dev/urandom > "$TMP/src.bin"
    want_sha=$(sha256sum "$TMP/src.bin" | cut -d' ' -f1)

    ip netns add "$NS" || die "netns add"
    ip netns exec "$NS" ip link set lo up
    ip link add sa0 type veth peer name sa1 || die "veth"
    ip link set sa1 netns "$NS"
    ip addr add 10.202.0.1/24 dev sa0
    ip link set sa0 up
    ip netns exec "$NS" ip addr add 10.202.0.2/24 dev sa1
    ip netns exec "$NS" ip link set sa1 up

    insmod "$KO" shell_retx="$mode" || die "insmod"

    ip netns exec "$NS" python3 "$HERE/serve.py" 10.202.0.2 "$TGT_PORT" "$TMP/src.bin" \
        > "$case_dir/serve.log" 2>&1 &
    SRV_PID=$!

    echo "tunnel 1 role=far listen=127.0.0.1:$FAR_PORT target=10.202.0.2:$TGT_PORT n=4 m=1 fec=on mode=grouped w=8 carriers=5 profile=http-kal crypto=off adaptive=off" \
        > /proc/net/tbtun/config || die "far config"
    echo "tunnel 2 role=near listen=10.202.0.1:$NEAR_PORT peer=127.0.0.1:$FAR_PORT n=4 m=1 fec=on mode=grouped w=8 carriers=5 profile=http-kal crypto=off adaptive=off dev=lo portblock=$PB_LO-$PB_HI" \
        > /proc/net/tbtun/config || die "near config"

    i=0
    while [ "$i" -lt 60 ]; do
        up1=$(awk '$1=="tunnel" && $2==1 && $3=="c3" {for(j=1;j<=NF;j++) if($j ~ /^up=/){sub(/^up=/,"",$j);print $j}}' /proc/net/tbtun/stats 2> /dev/null | tail -1)
        up2=$(awk '$1=="tunnel" && $2==2 && $3=="c3" {for(j=1;j<=NF;j++) if($j ~ /^up=/){sub(/^up=/,"",$j);print $j}}' /proc/net/tbtun/stats 2> /dev/null | tail -1)
        [ "${up1:-0}" = 5 ] && [ "${up2:-0}" = 5 ] && break
        sleep 0.5
        i=$((i + 1))
    done
    [ "${up1:-0}" = 5 ] && [ "${up2:-0}" = 5 ] || die "$tag carriers up=$up1/$up2"

    case $netem in
    none) ;;
    loss1) tc qdisc replace dev lo root netem loss 1% || die "loss1" ;;
    loss5) tc qdisc replace dev lo root netem loss 5% || die "loss5" ;;
    loss20) tc qdisc replace dev lo root netem loss 20% || die "loss20" ;;
    reorder10) tc qdisc replace dev lo root netem delay 5ms 2ms reorder 10% 50% || die "reorder10" ;;
    *) die "netem $netem" ;;
    esac

    t0=$(date +%s.%N)
    rc=0
    timeout 300 ip netns exec "$NS" python3 "$HERE/client.py" 10.202.0.1 "$NEAR_PORT" "$SIZE" "$TMP/out.bin" \
        > "$case_dir/client.log" 2>&1 || rc=$?
    t1=$(date +%s.%N)
    secs=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')
    mbps=$(awk -v s="$SIZE" -v t="$secs" 'BEGIN{ if (t>0) printf "%.2f", s*8/t/1000000; else print "0" }')

    got_bytes=0
    got_sha=none
    exact=0
    [ -f "$TMP/out.bin" ] && got_bytes=$(stat -c%s "$TMP/out.bin")
    [ -f "$TMP/out.bin" ] && got_sha=$(sha256sum "$TMP/out.bin" | cut -d' ' -f1)
    [ "$rc" = 0 ] && [ "$got_bytes" = "$SIZE" ] && [ "$got_sha" = "$want_sha" ] && exact=1

    grep -E ' shell | c11 ' /proc/net/tbtun/stats > "$case_dir/stats.txt" 2>&1 || true
    cat /proc/net/tbtun/peers > "$case_dir/peers.txt" 2>&1 || true
    dmesg | tail -120 > "$case_dir/dmesg.txt" 2>&1 || true

    subst=$(shell_kv 2 subst)
    ack=$(shell_kv 2 ack)
    dsack=$(shell_kv 2 dsack)
    dsack_rq=$(shell_kv 2 dsack_rq)
    retx_same=$(shell_kv 2 retx_same)
    timeout=$(shell_kv 2 timeout)
    carrier_lost=$(shell_kv 2 carrier_lost)
    stranded=$(shell_kv 2 stranded)
    no_rec=$(shell_kv 2 no_rec)
    skip_busy=$(shell_kv 2 skip_busy)
    skip_prefix=$(shell_kv 2 skip_prefix)
    selfcheck=$(shell_kv 2 selfcheck)
    csum_none=$(shell_kv 2 csum_none)
    exact_chunks=$(shell_kv 2 exact)
    merged=$(shell_kv 2 merged)
    crc_fail=$(awk '$1=="tunnel" && $2==1 && $3=="c11" {for(i=1;i<=NF;i++) if(index($i,"fec_payload_crc_fail=")==1){sub(/^fec_payload_crc_fail=/,"",$i);print $i;exit}}' /proc/net/tbtun/stats 2> /dev/null | tail -1)
    proto_fail=$(awk '$1=="tunnel" && $2==1 && $3=="c11" {for(i=1;i<=NF;i++) if(index($i,"fec_proto_fail=")==1){sub(/^fec_proto_fail=/,"",$i);print $i;exit}}' /proc/net/tbtun/stats 2> /dev/null | tail -1)

    say "RESULT mode=$mode netem=$netem ok=$exact bytes=$got_bytes/$SIZE secs=$secs mbps=$mbps"
    say "  subst=${subst:-0} ack=${ack:-0} dsack=${dsack:-0} dsack_rq=${dsack_rq:-0} retx_same=${retx_same:-0} timeout=${timeout:-0} carrier_lost=${carrier_lost:-0} stranded=${stranded:-0}"
    say "  no_rec=${no_rec:-0} skip_busy=${skip_busy:-0} skip_prefix=${skip_prefix:-0} selfcheck=${selfcheck:-0} csum_none=${csum_none:-0} chunks_exact=${exact_chunks:-0} chunks_merged=${merged:-0} crc_fail=${crc_fail:-0} proto_fail=${proto_fail:-0}"

    cleanup_net
}

say "sweep start $(date -Is) kernel=$(uname -r) size=$SIZE"
for mode in 0 1; do
    for netem in $NETEMS; do
        one_case "$mode" "$netem"
    done
done
say "sweep done $(date -Is)"
