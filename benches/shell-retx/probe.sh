#!/bin/sh
# Borrowed-shell retransmission: decisive probe.
#
# Two questions, in order:
#   1. Can one 1079-byte FEC chunk be made to occupy exactly one skb?
#      The module enforces TCP_MAXSEG=chunk, sk_gso_max_size=chunk,
#      sk_gso_max_segs=1 and MSG_EOR on every carrier write. Its counters
#      check the skb that owns each just-written chunk:
#        writes  chunk-sized writes checked
#        exact   len == chunk and the skb is not GSO
#        merged  the chunk was appended to a larger skb
#        gso     the skb is a TSO skb
#        noskb   the owning skb was not found
#      OFF is the control; ON must report exact == writes and 0 merged/gso.
#   2. If that holds, does rewriting the payload of a LOST skb with a record
#      from the FEC FIFO stay byte-exact under loss and reorder?
#
# The host lock is held for the whole run; it is released after at most
# 1800 s of waiting, so this probe cannot camp on the shared host.
set -u

ROOT=/root/shell-retx
KO=$ROOT/tbtun.ko
HERE=$ROOT/benches/shell-retx
TMP=$ROOT/tmp/probe
OUT=$ROOT/benches/shell-retx/results
LOCK=/root/tbt-testhost.lock
SIZE=${1:-16777216}
TGT_PORT=19620
FAR_PORT=19610
NEAR_PORT=19600
PB_LO=19650
PB_HI=19699
NS=tbtapp

mkdir -p "$TMP" "$OUT"
export TMPDIR=$TMP
SRV_PID=
SS_PID=

say() { printf '%s\n' "$*"; }
die() { say "FATAL: $*"; exit 1; }

LOCKED=0
exec 9>"$LOCK"
flock -w 1800 9 || die "testhost lock timeout"
LOCKED=1

cleanup_net() {
    [ -n "${SS_PID:-}" ] && kill "$SS_PID" 2> /dev/null
    SS_PID=
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2> /dev/null
    SRV_PID=
    timeout 15 sh -c 'echo flush > /proc/net/tbtun/config' 2> /dev/null || true
    sleep 1
    rmmod tbtun 2> /dev/null || true
    tc qdisc del dev lo root 2> /dev/null || true
    ip netns del "$NS" 2> /dev/null || true
    ip link del sa0 2> /dev/null || true
}
cleanup() { [ "$LOCKED" = 1 ] || return; cleanup_net; rm -f "$TMP"/out.bin; }
trap cleanup EXIT INT TERM

one_case() {
    mode=$1
    netem=$2
    tag="mode${mode}-${netem}"
    case_dir=$OUT/probe-$tag
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

    [ -f "$KO" ] || die "missing $KO"
    insmod "$KO" shell_retx="$mode" || die "insmod"

    ip netns exec "$NS" python3 "$HERE/serve.py" 10.202.0.2 "$TGT_PORT" "$TMP/src.bin" \
        > "$case_dir/serve.log" 2>&1 &
    SRV_PID=$!

    echo "tunnel 1 role=far listen=127.0.0.1:$FAR_PORT target=10.202.0.2:$TGT_PORT n=4 m=1 fec=on mode=grouped w=8 carriers=5 profile=http-kal crypto=off adaptive=off" \
        > /proc/net/tbtun/config || die "far config"
    echo "tunnel 2 role=near listen=10.202.0.1:$NEAR_PORT peer=127.0.0.1:$FAR_PORT n=4 m=1 fec=on mode=grouped w=8 carriers=5 profile=http-kal crypto=off adaptive=off dev=lo portblock=$PB_LO-$PB_HI" \
        > /proc/net/tbtun/config || die "near config"

    i=0
    up1=0
    up2=0
    while [ "$i" -lt 60 ]; do
        up1=$(awk '$1=="tunnel" && $2==1 && $3=="c3" {for(j=1;j<=NF;j++) if($j ~ /^up=/){sub(/^up=/,"",$j);print $j}}' /proc/net/tbtun/stats 2> /dev/null | tail -1)
        up2=$(awk '$1=="tunnel" && $2==2 && $3=="c3" {for(j=1;j<=NF;j++) if($j ~ /^up=/){sub(/^up=/,"",$j);print $j}}' /proc/net/tbtun/stats 2> /dev/null | tail -1)
        [ "${up1:-0}" = 5 ] && [ "${up2:-0}" = 5 ] && break
        sleep 0.5
        i=$((i + 1))
    done
    say "   up far=${up1:-?} near=${up2:-?} after $((i / 2))s"
    if [ "${up1:-0}" != 5 ] || [ "${up2:-0}" != 5 ]; then
        cat /proc/net/tbtun/stats > "$case_dir/stats.txt" 2>&1 || true
        dmesg | tail -80 > "$case_dir/dmesg.txt" 2>&1 || true
        die "$tag: carriers did not come up"
    fi

    case $netem in
    none) ;;
    loss5) tc qdisc replace dev lo root netem loss 5% || die "netem loss5" ;;
    reorder10) tc qdisc replace dev lo root netem delay 5ms 2ms reorder 10% 50% || die "netem reorder10" ;;
    *) die "unknown netem $netem" ;;
    esac

    # One socket-table sample while the transfer runs: MSS, cwnd, pacing and
    # any GSO counters the kernel exposes. The decisive skb-boundary evidence
    # is the module's exact/merged counter above, not this sample.
    ( sleep 1; ss -tin > "$case_dir/ss.txt" 2>&1 ) &
    SS_PID=$!

    t0=$(date +%s.%N)
    rc=0
    timeout 240 ip netns exec "$NS" python3 "$HERE/client.py" 10.202.0.1 "$NEAR_PORT" "$SIZE" "$TMP/out.bin" \
        > "$case_dir/client.log" 2>&1 || rc=$?
    t1=$(date +%s.%N)
    secs=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')
    mbps=$(awk -v s="$SIZE" -v t="$secs" 'BEGIN{ if (t>0) printf "%.2f", s*8/t/1000000; else print "0" }')
    wait "$SS_PID" 2> /dev/null
    SS_PID=

    got_bytes=0
    got_sha=none
    [ -f "$TMP/out.bin" ] && got_bytes=$(stat -c%s "$TMP/out.bin")
    [ -f "$TMP/out.bin" ] && got_sha=$(sha256sum "$TMP/out.bin" | cut -d' ' -f1)
    if [ "$rc" = 0 ] && [ "$got_bytes" = "$SIZE" ] && [ "$got_sha" = "$want_sha" ]; then
        exact=1
    else
        exact=0
    fi

    grep -E ' shell | c11 ' /proc/net/tbtun/stats > "$case_dir/stats.txt" 2>&1 || true
    cat /proc/net/tbtun/peers > "$case_dir/peers.txt" 2>&1 || true
    dmesg | tail -120 > "$case_dir/dmesg.txt" 2>&1 || true

    near_shell=$(awk '$1=="tunnel" && $2==2 && $3=="shell" {sub(/^shell[ \t]*/,"");print}' /proc/net/tbtun/stats 2> /dev/null | tail -1)
    far_shell=$(awk '$1=="tunnel" && $2==1 && $3=="shell" {sub(/^shell[ \t]*/,"");print}' /proc/net/tbtun/stats 2> /dev/null | tail -1)
    say "   rc=$rc bytes=$got_bytes/$SIZE sha_ok=$exact seconds=$secs mbps=$mbps"
    say "   near $near_shell"
    say "   far  $far_shell"

    cleanup_net
}

say "probe start $(date -Is) kernel=$(uname -r) size=$SIZE"
# The decisive pair: control then enforcement.
one_case 0 none
one_case 1 none
# Only if the premise held does the substitution run matter.
one_case 0 loss5
one_case 1 loss5
one_case 0 reorder10
one_case 1 reorder10
say "probe done $(date -Is)"
