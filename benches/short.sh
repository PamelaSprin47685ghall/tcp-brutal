#!/bin/sh
# short.sh - the real-host short benchmark: one 5 s download per shape.
#
#   benches/short.sh --near root@192.168.123.85 --far root@45.11.1.62 \
#                    --near-ip 192.168.123.85 --far-ip 45.11.1.62 \
#                    --carriers 32,64,256 --duration 5 \
#                    [--near-module /root/tbtun-fix27/tbtun.ko] \
#                    [--url http://127.0.0.1:9001/accept-1g.bin] \
#                    [--baseline|--compare]
#
# WHAT IT MEASURES
#   The far host runs a raw HTTP sender on 127.0.0.1:5210 (far-raw-serve.py);
#   the near host runs one curl through the public listener for `duration`
#   seconds. One public TCP flow, no iperf3, no second connection. The shape
#   is the production one: n=4 m=4 fec=on mode=grouped w=8 crypto=on
#   profile=http-kal, nonce=00112233445566778899aabbccddeeff.
#
#   For every carrier count it overwrites tunnel 1 on both ends (no `flush`:
#   flush blocks after a transfer, a known module behaviour), waits for
#   `up=N` on both, takes a snapshot, runs the curl, takes another snapshot,
#   and diffs the counters. The far raw sender and the loaded modules are
#   never restarted.
#
# OUTPUT
#   benches/results/<UTC stamp>-remote[-tag]/
#     remote.csv         name,iterations,ns_per_op,mb_per_s,checksum
#     remote-detail.csv  app/wire, wire/app, retx, NACK/replay, HOL,
#                        flush_skip, hol_wait, stalls, CPU, proto errors
#     <name>.curl        the curl -w line
#
#   --baseline copies remote.csv to benches/baseline/remote.csv.
#   --compare joins that baseline with this run (scripts/microbench.sh
#   compare, same verdicts).
#
#   A failed shape is recorded with rc != 0 and the sweep continues; the
#   script exits non-zero at the end. Nothing is retried.
#
# SAFETY
#   near must be idle: the 1 GiB acceptance owns it otherwise. The hardware
#   watchdog feeder is never touched. If /proc/net/tbtun is missing and
#   --near-module names a .ko, it is insmod'ed once (never rmmod'ed). The
#   near ip rule to the far address (pref 100, lookup main) is added only if
#   missing, because mihomo otherwise sends the carriers through Meta.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
results=$root/benches/results
baseline_dir=$root/benches/baseline

near=${TBT_NEAR:-}
far=${TBT_FAR:-}
near_ip=${TBT_NEAR_IP:-}
far_ip=${TBT_FAR_IP:-}
carriers_list=32,64,256
duration=5
url=http://127.0.0.1:9001/accept-1g.bin
near_module=
dev=eth0
near_port=9001
far_port=9000
target=127.0.0.1:5210
near_block=30000-30511
far_block=31000-31511
rx_reorder=8388608
tx_window=0
nonce=00112233445566778899aabbccddeeff
tag=
baseline_flag=0
compare_flag=0

usage()
{
    sed -n '2,42p' "$0" | sed 's/^# \{0,1\}//'
}

die()
{
    echo "short.sh: $*" >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case $1 in
    --near) near=$2; shift 2 ;;
    --far) far=$2; shift 2 ;;
    --near-ip) near_ip=$2; shift 2 ;;
    --far-ip) far_ip=$2; shift 2 ;;
    --carriers) carriers_list=$2; shift 2 ;;
    --duration) duration=$2; shift 2 ;;
    --url) url=$2; shift 2 ;;
    --near-module) near_module=$2; shift 2 ;;
    --dev) dev=$2; shift 2 ;;
    --near-port) near_port=$2; shift 2 ;;
    --far-port) far_port=$2; shift 2 ;;
    --target) target=$2; shift 2 ;;
    --near-block) near_block=$2; shift 2 ;;
    --far-block) far_block=$2; shift 2 ;;
    --rx-reorder) rx_reorder=$2; shift 2 ;;
    --tx-window) tx_window=$2; shift 2 ;;
    --nonce) nonce=$2; shift 2 ;;
    --baseline) baseline_flag=1; shift ;;
    --compare) compare_flag=1; shift ;;
    --tag) tag=$2; shift 2 ;;
    -h | --help) usage; exit 0 ;;
    *) die "unknown option $1" ;;
    esac
done

[ -n "$near" ] || die "--near HOST is required (or TBT_NEAR)"
[ -n "$far" ] || die "--far HOST is required (or TBT_FAR)"
[ -n "$near_ip" ] || die "--near-ip ADDR is required"
[ -n "$far_ip" ] || die "--far-ip ADDR is required"

remote_sh()
{
    host=$1
    shift
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" "sh -s -- $*"
}

# Add the far bypass rule only when missing. mihomo owns the default route;
# without pref 100 the carrier dials leave through Meta and never join.
prepare_near()
{
    remote_sh "$near" "${near_module:--}" "$far_ip" <<'EOS'
KO=$1; FAR=$2
[ "$KO" = - ] && KO=
if [ -n "$KO" ] && [ ! -d /proc/net/tbtun ]; then
    insmod "$KO"
fi
[ -d /proc/net/tbtun ] || { echo "short.sh: tbtun not loaded on near" >&2; exit 3; }
if ! ip rule show | grep -q "to $FAR/32 lookup main pref 100"; then
    ip rule add to "$FAR/32" lookup main pref 100
fi
route=$(ip route get "$FAR" 2>/dev/null | head -1)
case $route in
*" dev eth0 "*) ;;
*) echo "short.sh: near route to $FAR is not eth0: $route" >&2; exit 3 ;;
esac
EOS
}

snapshot()
{
    remote_sh "$1" <<'EOS'
echo '@@stats'
cat /proc/net/tbtun/stats
echo '@@peers'
cat /proc/net/tbtun/peers
echo '@@cpu'
head -1 /proc/stat
echo '@@clk'
getconf CLK_TCK 2>/dev/null || echo 100
EOS
}

configure_far()
{
    # fix28's far rejects peer= (EINVAL); it anchors by the first HELLO
    # (TOFU) as before. Do not add the key back without checking the parser.
    remote_sh "$far" "$1" "$far_port" "$target" "$far_block" "$nonce" "$rx_reorder" "$tx_window" <<'EOS'
N=$1; PORT=$2; TARGET=$3; BLOCK=$4; NONCE=$5; RX=$6; TW=$7
printf 'tunnel 1 role=far listen=0.0.0.0:%s target=%s n=4 m=4 fec=on mode=grouped w=8 carriers=%s crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=%s tx_window_bytes=%s tx_retain_bytes=0 carrier_stall_ms=0 portblock=%s nonce=%s\n' \
    "$PORT" "$TARGET" "$N" "$RX" "$TW" "$BLOCK" "$NONCE" > /proc/net/tbtun/config
EOS
}

configure_near()
{
    remote_sh "$near" "$1" "$near_port" "$far_ip" "$far_port" "$near_block" "$nonce" "$dev" "$rx_reorder" "$tx_window" <<'EOS'
N=$1; PORT=$2; PEER=$3; PEERPORT=$4; BLOCK=$5; NONCE=$6; DEV=$7; RX=$8; TW=$9
printf 'tunnel 1 role=near listen=127.0.0.1:%s peer=%s:%s n=4 m=4 fec=on mode=grouped w=8 carriers=%s crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=%s tx_window_bytes=%s tx_retain_bytes=0 carrier_stall_ms=0 portblock=%s nonce=%s dev=%s\n' \
    "$PORT" "$PEER" "$PEERPORT" "$N" "$RX" "$TW" "$BLOCK" "$NONCE" "$DEV" > /proc/net/tbtun/config
EOS
}

# Wait until both ends report up=N. The FEC join dials N carriers; 256 takes
# longer than 32, so the bound is generous but finite.
wait_join()
{
    n=$1
    i=0
    while [ "$i" -lt 36 ]; do
        nu=$(remote_sh "$near" <<'EOS'
grep 'tunnel 1 c3 ' /proc/net/tbtun/stats | sed 's/.* up=\([0-9]*\) .*/\1/'
EOS
)
        fu=$(remote_sh "$far" <<'EOS'
grep 'tunnel 1 c3 ' /proc/net/tbtun/stats | sed 's/.* up=\([0-9]*\) .*/\1/'
EOS
)
        if [ "${nu:-0}" = "$n" ] && [ "${fu:-0}" = "$n" ]; then
            return 0
        fi
        i=$((i + 1))
        sleep 5
    done
    echo "short.sh: join timeout near=${nu:-0} far=${fu:-0} want=$n" >&2
    return 1
}

# One field of a `key=value` line.
field()
{
    printf '%s\n' "$1" | tr ' ' '\n' | awk -F= -v k="$2" '$1 == k { print $2; exit }'
}

stat_line()
{
    printf '%s\n' "$1" | awk -v p="$2" 'index($0, p) == 1 { print; exit }'
}

num()
{
    [ -n "${1:-}" ] || { echo 0; return; }
    echo "$1"
}

peers_sum()
{
    snap=$1
    col=$2
    printf '%s\n' "$snap" | awk -v c="$col" '
        /^@@peers/ { p = 1; next }
        /^@@/ { p = 0 }
        p && NF >= 14 { s += $c }
        END { printf "%.0f", s }'
}

cpu_pct()
{
    b=$1
    a=$2
    e=$3
    lb=$(printf '%s\n' "$b" | awk '/^cpu /{print;exit}')
    la=$(printf '%s\n' "$a" | awk '/^cpu /{print;exit}')
    hz=$(printf '%s\n' "$a" | awk '/^@@clk/{getline; print; exit}')
    [ -n "$hz" ] || hz=100
    awk -v b="$lb" -v a="$la" -v e="$e" -v hz="$hz" 'BEGIN {
        nb = split(b, x, " "); na = split(a, y, " ")
        if (nb < 8 || na < 8) { print "0.0"; exit }
        busy_b = x[2] + x[3] + x[4] + x[7] + x[8]
        busy_a = y[2] + y[3] + y[4] + y[7] + y[8]
        printf "%.1f", (busy_a - busy_b) / (e * hz) * 100
    }'
}

stamp=$(date -u +%Y%m%dT%H%M%SZ)
if [ -n "$tag" ]; then
    dir=$results/$stamp-remote-$tag
else
    dir=$results/$stamp-remote
fi
mkdir -p "$dir"
main=$dir/remote.csv
detail=$dir/remote-detail.csv
printf 'name,iterations,ns_per_op,mb_per_s,checksum\n' >"$main"
printf '%s\n' 'name,carriers,app_bytes,wire_bytes,wire_per_app,retx_bytes,retx_ppm,nack_tx,nack_rx,replay_hit,replay_miss,hol_max_us,hol_wait,flush_skip,stall_declared,near_cpu_pct,far_cpu_pct,near_proto_fail,far_proto_fail,elapsed_s,rc' >"$detail"

prepare_near || die "near preparation failed"

echo "short.sh: near=$near ($near_ip) far=$far ($far_ip), ${duration}s per shape" >&2
failures=0

for n in $(printf '%s' "$carriers_list" | tr ',' ' '); do
    name=remote.n$n.fec-on.grouped.http-kal.crypto-on
    echo "  $name" >&2
    rc=0

    configure_far "$n" || rc=1
    configure_near "$n" || rc=1
    if [ "$rc" -eq 0 ]; then
        wait_join "$n" || rc=1
    fi
    if [ "$rc" -ne 0 ]; then
        printf '%s,%s,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,%s\n' \
            "$name" "$n" "$rc" >>"$detail"
        failures=$((failures + 1))
        continue
    fi

    nb=$(snapshot "$near")
    fb=$(snapshot "$far")
    t0=$(date +%s.%N)
    curl_out=$(remote_sh "$near" "$duration" "$url" <<'EOS'
D=$1; U=$2
out=$(curl -sS --max-time "$D" -o /dev/null -w '%{size_download} %{time_total}' "$U")
rc=$?
printf '%s %s\n' "$out" "$rc"
EOS
)
    t1=$(date +%s.%N)
    na=$(snapshot "$near")
    fa=$(snapshot "$far")

    app_bytes=$(printf '%s\n' "$curl_out" | awk 'NF >= 3 { printf "%.0f", $1 }')
    [ -n "$app_bytes" ] || app_bytes=0
    elapsed=$(printf '%s\n' "$curl_out" | awk 'NF >= 3 { print $2 }')
    [ -n "$elapsed" ] || elapsed=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.6f", b - a }')
    [ "$(awk -v e="$elapsed" 'BEGIN { print (e > 0.001) }')" = 1 ] || elapsed=0.001
    curl_rc=$(printf '%s\n' "$curl_out" | awk 'NF >= 3 { print $3 }')
    case ${curl_rc:-0} in
    0 | 28) ;;
    *) rc=1 ;;
    esac

    wire_b=$(peers_sum "$fb" 14)
    wire_a=$(peers_sum "$fa" 14)
    wire_bytes=$((wire_a - wire_b))
    [ "$wire_bytes" -ge 0 ] || wire_bytes=0

    c3b=$(stat_line "$fb" 'tunnel 1 c3 ')
    c3a=$(stat_line "$fa" 'tunnel 1 c3 ')
    c11b=$(stat_line "$fb" 'tunnel 1 c11 ')
    c11a=$(stat_line "$fa" 'tunnel 1 c11 ')
    nc11b=$(stat_line "$nb" 'tunnel 1 c11 ')
    nc11a=$(stat_line "$na" 'tunnel 1 c11 ')
    nc6a=$(stat_line "$na" 'tunnel 1 c6 ')
    nc9b=$(stat_line "$nb" 'tunnel 1 c9 ')
    nc9a=$(stat_line "$na" 'tunnel 1 c9 ')
    fc9b=$(stat_line "$fb" 'tunnel 1 c9 ')
    fc9a=$(stat_line "$fa" 'tunnel 1 c9 ')

    # FEC replay bytes are the repair traffic that matters here; c3's
    # retx_bytes covers the legacy ARQ path and stays zero on a FEC session.
    retx_bytes=$(( $(num "$(field "$c11a" fec_nack_retx_bytes)") - $(num "$(field "$c11b" fec_nack_retx_bytes)") ))
    retx_bytes=$(( retx_bytes + $(num "$(field "$c11a" fec_replace_retx_bytes)") - $(num "$(field "$c11b" fec_replace_retx_bytes)") ))
    retx_ppm=$(awk -v r="$retx_bytes" -v a="$app_bytes" 'BEGIN { printf "%d", a ? r * 1000000 / a : 0 }')
    stall=$(( $(num "$(field "$c3a" stall_declared)") - $(num "$(field "$c3b" stall_declared)") ))
    nack_tx=$(( $(num "$(field "$nc11a" fec_nack_tx)") - $(num "$(field "$nc11b" fec_nack_tx)") ))
    nack_rx=$(( $(num "$(field "$c11a" fec_nack_rx)") - $(num "$(field "$c11b" fec_nack_rx)") ))
    replay_hit=$(( $(num "$(field "$c11a" fec_replay_hit)") - $(num "$(field "$c11b" fec_replay_hit)") ))
    replay_miss=$(( $(num "$(field "$c11a" fec_replay_miss)") - $(num "$(field "$c11b" fec_replay_miss)") ))
    hol_wait=$(( $(num "$(field "$c11a" fec_hol_wait)") - $(num "$(field "$c11b" fec_hol_wait)") ))
    flush_skip=$(( $(num "$(field "$c11a" fec_flush_skip)") - $(num "$(field "$c11b" fec_flush_skip)") ))
    hol_max=$(field "$nc6a" rx_max_stall_us)
    near_proto=$(( $(num "$(field "$nc9a" fec_proto_fail)") - $(num "$(field "$nc9b" fec_proto_fail)") ))
    far_proto=$(( $(num "$(field "$fc9a" fec_proto_fail)") - $(num "$(field "$fc9b" fec_proto_fail)") ))

    near_cpu=$(cpu_pct "$nb" "$na" "$elapsed")
    far_cpu=$(cpu_pct "$fb" "$fa" "$elapsed")
    wire_per_app=$(awk -v w="$wire_bytes" -v a="$app_bytes" 'BEGIN { printf "%.4f", a ? w / a : 0 }')
    mbps=$(awk -v a="$app_bytes" -v e="$elapsed" 'BEGIN { printf "%.3f", a / e / 1e6 }')
    blocks=$(( app_bytes > 0 ? (app_bytes + 1048575) / 1048576 : 0 ))
    [ "$blocks" -gt 0 ] || blocks=1
    ns_per_op=$(awk -v e="$elapsed" -v b="$blocks" 'BEGIN { printf "%.3f", e * 1e9 / b }')
    checksum=$(printf '%016x' "$app_bytes")

    printf '%s,%s,%s,%s,%s\n' "$name" "$blocks" "$ns_per_op" "$mbps" "$checksum" >>"$main"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$name" "$n" "$app_bytes" "$wire_bytes" "$wire_per_app" \
        "$retx_bytes" "${retx_ppm:-0}" "$nack_tx" "$nack_rx" "$replay_hit" \
        "$replay_miss" "${hol_max:-0}" "$hol_wait" "$flush_skip" "$stall" \
        "$near_cpu" "$far_cpu" "$near_proto" "$far_proto" "$elapsed" "$rc" \
        >>"$detail"
    printf '%s\n' "$curl_out" >"$dir/$name.curl"
done

if [ "$baseline_flag" = 1 ]; then
    mkdir -p "$baseline_dir"
    cp "$main" "$baseline_dir/remote.csv"
    echo "short.sh: baseline -> $baseline_dir/remote.csv" >&2
fi
if [ "$compare_flag" = 1 ]; then
    if [ -f "$baseline_dir/remote.csv" ]; then
        "$root/scripts/microbench.sh" compare "$baseline_dir/remote.csv" "$main" || true
    else
        echo "short.sh: --compare skipped, no $baseline_dir/remote.csv" >&2
    fi
fi

echo "short.sh: wrote $main" >&2
echo "short.sh: wrote $detail" >&2
[ "$failures" -eq 0 ] || die "$failures shapes failed (see $detail)"
