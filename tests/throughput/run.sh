#!/bin/sh
# tests/throughput/run.sh - one command per benchmark scenario, inside one QEMU guest.
#
#   tests/throughput/run.sh <scenario> [options]
#
# Scenarios
#   throughput  control (dd|nc, one shaped 5-tuple) and the tunnel at n=1,2,4,8
#   profile     raw vs http-kal vs http-kal-drip at n=4 m=1
#   loss        prediction under drop_ppm 0/1%/5%/10% on two of four carriers
#   latency     1 KB echo, direct vs tunnel, p50/p95/p99 and the tail
#   memory      guest memory, slab and queue peaks around a 40 MiB transfer
#   scale       n=8 and n=16 against the 16-port block
#   wan         the measured 218 ms / 17 Mbit path, one raw stream
#   all         every scenario above, one boot each
#
# Options
#   --quick          short run for iteration: 8 MiB, 1 s window, 2000 latency
#                    iterations (default is 40 MiB, 2 s, 10000)
#   --mib N          transfer size in MiB
#   --n LIST         carrier counts, comma separated (throughput/scale)
#   --iters N        latency iterations (and --loss-iters for the lossy one)
#   --loss LIST      drop_ppm values (loss)
#   --netem SPEC     netem arguments, '|' for spaces (default delay|20ms|10ms,
#                    latency defaults to delay|1ms|500us; 'none' disables)
#   --window S       rate window in seconds
#   --wan-mbit M     carrier rate for wan (default 17)
#   --wan-delay D    one-way delay in ms for wan, bare or with "ms" (default
#                    109; the carrier round trip is two of them)
#   --rx-reorder N   receiver reorder budget in bytes (default 8 MiB for wan,
#                    2 MiB elsewhere); the old module derives its sender
#                    window from this
#   --tx-window N    pin tx_window_bytes for wan (default 0 = auto)
#   --hole-budget MS receiver hole budget, and the sender's stall bound
#                    (default 1500 for wan, 5000 elsewhere)
#   --tag NAME       extra tag in the result file names
#   --timeout S      wall-clock cap for the guest
#   --allow-tcg      run without KVM (numbers are then not comparable)
#
# Every number comes from counters inside the guest; the run writes
# tests/throughput/results/<stamp>-<scenario>[-tag].csv plus a per-carrier CSV, prints a
# table, and keeps the console log under ~/.cache/tbtun-build/bench/.
#
# The run is serialised with flock: two KVM guests on this box would perturb
# each other, and another agent's tests/qemu run would too. A warning is
# printed (and recorded in the CSV) when a foreign qemu is already running.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
here=$root/tests/throughput
cache=${TBTUN_BUILD:-$HOME/.cache/tbtun-build}
bench_cache=$cache/bench
rel=$(uname -r)
kernel=$cache/dl/vmlinuz-$rel

scenario=
quick=0
mib=
nlist=
iters=
loss_iters=
losslist=
netem=
window=
tag=
timeout_s=
allow_tcg=0
only=
no_control=0
control_mbit=
wan_mbit=
wan_delay=
rx_reorder=
tx_window=
hole_budget=
mib_set=0 n_set=0 iters_set=0 loss_iters_set=0 loss_set=0 netem_set=0 window_set=0 timeout_set=0

usage()
{
    sed -n '2,43p' "$0" | sed 's/^# \{0,1\}//'
}

die()
{
    echo "tests/throughput/run.sh: $*" >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case $1 in
    --quick) quick=1; shift ;;
    --mib) mib=$2; mib_set=1; shift 2 ;;
    --n) nlist=$2; n_set=1; shift 2 ;;
    --iters) iters=$2; iters_set=1; shift 2 ;;
    --loss-iters) loss_iters=$2; loss_iters_set=1; shift 2 ;;
    --loss) losslist=$2; loss_set=1; shift 2 ;;
    --netem) netem=$2; netem_set=1; shift 2 ;;
    --window) window=$2; window_set=1; shift 2 ;;
    --tag) tag=$2; shift 2 ;;
    --timeout) timeout_s=$2; timeout_set=1; shift 2 ;;
    --allow-tcg) allow_tcg=1; shift ;;
    --only) only=$2; shift 2 ;;
    --no-control) no_control=1; shift ;;
    --control-mbit) control_mbit=$2; shift 2 ;;
    --wan-mbit) wan_mbit=$2; shift 2 ;;
    --wan-delay) wan_delay=$2; shift 2 ;;
    --rx-reorder) rx_reorder=$2; shift 2 ;;
    --tx-window) tx_window=$2; shift 2 ;;
    --hole-budget) hole_budget=$2; shift 2 ;;
    -h | --help) usage; exit 0 ;;
    -*) echo "tests/throughput/run.sh: unknown option $1" >&2; exit 2 ;;
    *)
        [ -z "$scenario" ] || die "one scenario per run"
        scenario=$1
        shift
        ;;
    esac
done
[ -n "$scenario" ] || { usage; exit 2; }

case $scenario in
throughput | profile | loss | latency | memory | scale | wan | flush-hang | second-connection | hole-cost) ;;
all) ;;
*) die "unknown scenario $scenario" ;;
esac

[ -f "$kernel" ] || die "no kernel image at $kernel (see tests/qemu/README.md)"
[ -f "$root/tbtun.ko" ] || die "no module at $root/tbtun.ko (run make check)"

if [ ! -w /dev/kvm ] && [ "$allow_tcg" -eq 0 ]; then
    die "/dev/kvm is not writable; pass --allow-tcg only for a smoke run"
fi

# --- scenario defaults ------------------------------------------------------
case $scenario in
throughput)
    [ -n "$mib" ] || mib=40
    # One carrier count per boot: `echo flush` blocks after a large transfer
    # in this module, so a second row in the same guest never completes.
    [ -n "$nlist" ] || nlist=4
    [ -n "$netem" ] || netem='delay|20ms|10ms'
    [ -n "$window" ] || window=2
    [ -n "$timeout_s" ] || timeout_s=480
    ;;
profile)
    [ -n "$mib" ] || mib=40
    # One profile per boot, for the same flush reason; --only selects it.
    [ -n "$only" ] || only=http-kal
    [ -n "$netem" ] || netem='delay|20ms|10ms'
    [ -n "$window" ] || window=2
    [ -n "$timeout_s" ] || timeout_s=360
    ;;
loss)
    [ -n "$mib" ] || mib=40
    [ -n "$losslist" ] || losslist=0,10000,50000,100000
    [ -n "$netem" ] || netem='delay|20ms|10ms'
    [ -n "$window" ] || window=2
    [ -n "$timeout_s" ] || timeout_s=420
    ;;
latency)
    [ -n "$iters" ] || iters=10000
    [ -n "$loss_iters" ] || loss_iters=2000
    [ -n "$netem" ] || netem='delay|1ms|500us'
    [ -n "$timeout_s" ] || timeout_s=360
    ;;
memory)
    [ -n "$mib" ] || mib=40
    [ -n "$netem" ] || netem='delay|20ms|10ms'
    [ -n "$window" ] || window=2
    [ -n "$timeout_s" ] || timeout_s=360
    ;;
scale)
    [ -n "$mib" ] || mib=40
    [ -n "$nlist" ] || nlist=16
    [ -n "$netem" ] || netem='delay|20ms|10ms'
    [ -n "$window" ] || window=2
    [ -n "$timeout_s" ] || timeout_s=360
    ;;
wan)
    [ -n "$mib" ] || mib=16
    [ -n "$nlist" ] || nlist=1
    [ -n "$netem" ] || netem=none
    [ -n "$window" ] || window=2
    [ -n "$timeout_s" ] || timeout_s=420
    ;;
flush-hang)
    [ -n "$mib" ] || mib=8
    [ -n "$netem" ] || netem='delay|20ms|10ms'
    [ -n "$timeout_s" ] || timeout_s=240
    ;;
second-connection)
    [ -n "$netem" ] || netem=none
    [ -n "$timeout_s" ] || timeout_s=120
    ;;
hole-cost)
    [ -n "$iters" ] || iters=2000
    # 1 ms, not 20 ms: the tail must be holes, not the path's own RTT.
    [ -n "$netem" ] || netem='delay|1ms|500us'
    [ -n "$timeout_s" ] || timeout_s=180
    ;;
esac
if [ "$quick" -eq 1 ]; then
    case $scenario in
    throughput | profile | loss | memory | scale | wan)
        # A quick run is still a real measurement, just a smaller volume and
        # a shorter rate window. An explicit --mib/--window wins.
        [ "$mib_set" -eq 1 ] || mib=8
        [ "$window_set" -eq 1 ] || window=1
        ;;
    latency)
        [ "$iters_set" -eq 1 ] || iters=2000
        [ "$loss_iters_set" -eq 1 ] || loss_iters=500
        ;;
    esac
    [ "$timeout_set" -eq 1 ] || timeout_s=180
fi
[ -n "$iters" ] || iters=10000
[ -n "$loss_iters" ] || loss_iters=2000
[ -n "$losslist" ] || losslist=0,10000,50000,100000
[ -n "$nlist" ] || nlist=1,2,4,8
[ -n "$wan_mbit" ] || wan_mbit=17
[ -n "$wan_delay" ] || wan_delay=109
# --wan-delay accepts the unit the row prints ("109ms") as well as a bare
# number; everything downstream does integer arithmetic on it.
case $wan_delay in
*ms) wan_delay=${wan_delay%ms} ;;
esac
case $wan_delay in
'' | *[!0-9]*) die "--wan-delay needs milliseconds: 109 or 109ms" ;;
esac
[ -n "$tx_window" ] || tx_window=0

stamp=$(date +%Y%m%dT%H%M%S)
mkdir -p "$here/bin" "$here/results" "$bench_cache"

# --- lock and contention check ---------------------------------------------
exec 9> "$bench_cache/lock"
if ! flock -n 9; then
    echo "tests/throughput/run.sh: another bench run holds the lock; waiting"
    flock 9
fi
others=$(pgrep -c -f qemu-system-x86_64 2> /dev/null) || others=0
[ -n "$others" ] || others=0
if [ "${others:-0}" -gt 0 ]; then
    echo "tests/throughput/run.sh: WARNING: $others foreign qemu-system process(es) running; numbers may be perturbed" >&2
fi

# --- helpers ----------------------------------------------------------------
free_port_triple()
{
    base=$1
    for _try in 0 10 20 30 40; do
        p=$((base + _try))
        if ! ss -ltn 2> /dev/null | awk '{print $4}' | grep -qE "[:.]$p\$"; then
            if ! ss -ltn 2> /dev/null | awk '{print $4}' | grep -qE "[:.]$((p + 1))\$"; then
                if ! ss -ltn 2> /dev/null | awk '{print $4}' | grep -qE "[:.]$((p + 2))\$"; then
                    echo "$p"
                    return 0
                fi
            fi
        fi
    done
    return 1
}

csv_header='stamp,scenario,row,git_rev,git_dirty,ko_sha256,src_sha256,src_status,kernel,qemu_sha256,n,m,profile,mib,drop_ppm,iters,bytes,control_mbit,tunnel_mbit,ratio,goodput_mbit,app_mbit,wall_s,wire_bytes,app_bytes,wire_per_app,frames,frames_s,cpu_sys_pct,cpu_softirq_pct,cpu_total_pct,kworker_pct,retx_ppm,retx_hole,retx_slow,retx_pathloss,dup_bytes_ppm,fb_delay_us,rto_us,fbk_tx,dup_tx,inflate_ppm,holes_declared,holes_repaired,holes_unrecoverable,rx_max_stall_us,reorder_max_depth,carriers_up,txq_max,rxq_max,ooo_bytes_max,retain_bytes_max,spec_bytes_max,mem_before_kb,mem_peak_kb,mem_after_kb,proc_rss_before_kb,proc_rss_peak_kb,slab_before_kb,slab_peak_kb,slab_after_kb,slab_top,lat_min_us,lat_p50_us,lat_p90_us,lat_p95_us,lat_p99_us,lat_p999_us,lat_max_us,lat_mean_us,lat_complete,lat_over5ms,lat_over20ms,lat_hist,lat_outliers,lat_tail_n,lat_tail_min_us,lat_tail_p50_us,lat_tail_p90_us,lat_tail_p99_us,lat_tail_max_us,dropped_by_ppm,connect_fail,alloc_fail,carrier_lost,carrier_replaced,qemu_rss_peak_kb,host_qemu_others,config,verdict,flush_s,hung,wchan,stack,while_active,errno_active,ms_active,after_close,errno_after,ms_after,rc,strict,notes'
# WAN fields, appended so the columns above keep their positions for the
# result files already on disk. `tx_window`, the car_* aggregates, the
# pump/buffer keys and rtt_mean_us/rttvar_us stay empty when the module does
# not print them; rtt_min_us and n_carriers come from the row-end peers
# snapshot, which every module prints.
csv_header="$csv_header"',wan_mbit,wan_delay_us,rx_mbit,inflight_sum,tx_window,tx_ack,car_delivery_mbps,car_srtt_ms,car_cwnd_kb,car_snd_wnd_kb,win_block,pump_hw_armed,pump_hw_fired,car_sndbuf,car_rcvbuf,rtt_min_us,rtt_mean_us,rtt_max_us,rttvar_us,n_carriers,sum_delivery_mbps,mean_bdp_kb,coverage,k_eff,inflight_over_window,hole_budget_ms,carrier_stall_ms,wan_ingress,bdp_bytes,k_x100,rx_reorder_bytes'
carrier_header='stamp,scenario,row,carrier,sport,state,profile,bytes,frames,dropped,retx_ppm,inflight,delivery_rate_bps,cwnd_bytes,snd_wnd,rtt_us,wscale,rcv_wnd,window_clamp,rmem_alloc'

build_bins()
{
    cc=${HOSTCC:-cc}
    "$cc" -std=gnu99 -O2 -Wall -Wextra -Werror -o "$here/bin/tbt_lat" "$here/src/tbt_lat.c"
    "$cc" -std=gnu99 -O2 -Wall -Wextra -Werror -o "$here/bin/tbt_echo" "$here/src/tbt_echo.c"
    "$cc" -std=gnu99 -O2 -Wall -Wextra -Werror -o "$here/bin/tbt_conn" "$here/src/tbt_conn.c"
    "$cc" -std=gnu99 -O2 -Wall -Wextra -Werror -o "$here/bin/tbt_burst" "$root/host/tbt_burst.c"
}

net_assets()
{
    mkdir -p "$bench_cache/net"
    for m in failover net_failover virtio_net sch_htb sch_netem cls_u32 sch_tbf \
        act_gact act_mirred ifb sch_ingress; do
        src=$(find "/usr/lib/modules/$rel/kernel" -name "$m.ko.zst" 2> /dev/null | head -1)
        [ -n "$src" ] || die "no $m module for $rel under /usr/lib/modules"
        dst=$bench_cache/net/$m.ko
        [ -f "$dst" ] || zstd -dc "$src" > "$dst" || die "cannot decompress $src"
        printf ' --asset %s' "$dst"
    done
}

print_table()
{
    _csv=$1
    shift
    awk -F, -v cols="$*" '
        BEGIN {
            n = split(cols, c, " ")
            line = sprintf("%-26s", c[1])
            for (i = 2; i <= n; i++) line = line sprintf("%14s", c[i])
            print line
            print ""
        }
        NR == 1 { for (i = 1; i <= NF; i++) h[$i] = i; next }
        {
            if (n == 0) next
            line = ""
            for (i = 1; i <= n; i++) {
                v = $(h[c[i]])
                if (i == 1) line = sprintf("%-26s", v)
                else line = line sprintf("%14s", v)
            }
            print line
        }' "$_csv"
}

# --- one boot ---------------------------------------------------------------
run_one()
{
    _scen=$1
    _mib=$2
    _nlist=$3
    _iters=$4
    _lossiters=$5
    _losslist=$6
    _netem=$7
    _window=$8
    _timeout=$9
    shift 9
    _tag=${1:-}

    printf 'scenario=%s\nmib=%s\nnlist=%s\nprofile=raw\ndup=0\n' \
        "$_scen" "$_mib" "$_nlist" > "$bench_cache/bench.txt"
    printf 'iters=%s\nloss_iters=%s\nhole_iters=%s\nloss=%s\nbytes=1024\nnetem=%s\n' \
        "$_iters" "$_lossiters" "$_iters" "$_losslist" "$_netem" >> "$bench_cache/bench.txt"
    case $_scen in
    wan) _rx=${rx_reorder:-8388608}; _hb=${hole_budget:-1500} ;;
    *) _rx=${rx_reorder:-2097152}; _hb=${hole_budget:-5000} ;;
    esac
    printf 'rx_reorder=%s\nhole_budget=%s\nwindow=%s\nwan_mbit=%s\nwan_delay=%s\ntx_window=%s\n' \
        "$_rx" "$_hb" "$_window" "$wan_mbit" "$wan_delay" "$tx_window" >> "$bench_cache/bench.txt"
    printf 'profiles=%s\ncontrol=%s\ncontrol_mbit=%s\n' "${only:-raw,http-kal,http-kal-drip}" \
        "$((1 - no_control))" "${control_mbit:-0}" >> "$bench_cache/bench.txt"

    ports=$(free_port_triple "${TBT_BENCH_PORT_BASE:-29100}") ||
        die "no free host port triple for hostfwd"
    car_port=$ports
    echo_port=$((ports + 1))
    sink_port=$((ports + 2))
    printf 'car=%s\necho=%s\nsink=%s\n' "$car_port" "$echo_port" "$sink_port" > "$bench_cache/hostfwd.txt"

    initrd=$bench_cache/initramfs.cpio.gz
    tc_arg=
    [ -n "$tc_bin" ] && tc_arg="--bin $tc_bin"
    # The wan scenario needs the host iproute2: busybox ip cannot create an
    # ifb device. It is copied to /bin/ip, replacing the busybox applet link.
    ip_arg=
    case $_scen in
    wan)
        [ -n "$ip_bin" ] && ip_arg="--bin $ip_bin"
        ;;
    esac
    # shellcheck disable=SC2046,SC2086
    "$here/qemu/mkinitramfs.sh" --ko "$root/tbtun.ko" --out "$initrd" \
        --init "$here/qemu/init" --endpoint "$here/bin/tbt_burst" \
        --bin "$here/bin/tbt_lat" --bin "$here/bin/tbt_echo" --bin "$here/bin/tbt_conn" \
        $tc_arg $ip_arg \
        --asset "$bench_cache/bench.txt" --asset "$bench_cache/hostfwd.txt" \
        $(net_assets) > /dev/null || die "initramfs build failed"

    console=$bench_cache/console-$stamp-$_scen$_tag.log
    qlog=$bench_cache/qemu-$stamp-$_scen$_tag.log
    rssfile=$bench_cache/rss-$stamp-$_scen$_tag
    rm -f "$rssfile" "$console"
    accel=kvm
    [ -w /dev/kvm ] || accel=tcg

    case $_scen in
    wan)
        echo "tests/throughput/run.sh: scenario=$_scen mib=$_mib n=$_nlist wan=${wan_mbit}mbit/${wan_delay}ms window=${_window}s accel=$accel"
        ;;
    *)
        echo "tests/throughput/run.sh: scenario=$_scen mib=$_mib n=$_nlist netem=$_netem window=${_window}s accel=$accel"
        ;;
    esac
    set -- qemu-system-x86_64 \
        -accel "$accel" -cpu max -smp "${TBT_BENCH_SMP:-2}" -m 512 \
        -kernel "$kernel" -initrd "$initrd" \
        -append "console=ttyS0 panic=-1 loglevel=4 tsc=unstable" \
        -nographic -no-reboot -nodefaults \
        -netdev "user,id=n0,hostfwd=tcp::$car_port-:19101,hostfwd=tcp::$echo_port-:19102,hostfwd=tcp::$sink_port-:19103" \
        -device virtio-net-pci,netdev=n0 \
        -display none -monitor none -serial "file:$console"
    qemu_sha256=$(printf '%s\n' "$@" | sha256sum | cut -c1-16)
    "$@" > "$qlog" 2>&1 &
    qpid=$!

    (
        exec 9>&- # a child that outlives this shell must not hold the run lock
        _peak=0
        while kill -0 "$qpid" 2> /dev/null; do
            _r=$(awk '/^VmRSS:/ { print $2 }' "/proc/$qpid/status" 2> /dev/null || true)
            if [ -n "$_r" ] && [ "$_r" -gt "$_peak" ]; then
                _peak=$_r
                echo "$_peak" > "$rssfile"
            fi
            sleep 0.2
        done
    ) > /dev/null 2>&1 &
    rsspid=$!
    # stdout/stderr to /dev/null: an orphaned `sleep` that inherits the
    # caller's pipe would keep a `| tail` waiting for the full timeout.
    ( exec 9>&-; sleep "$_timeout"; kill -9 "$qpid" 2> /dev/null ) > /dev/null 2>&1 &
    wdpid=$!
    # The guest prints BENCH_DONE after the last row; cleanup (flush, poweroff)
    # is best effort and may block on a stuck stream, so stop qemu as soon as
    # the marker is on the console instead of waiting for the timeout.
    while kill -0 "$qpid" 2> /dev/null; do
        if grep -qE '^BENCH_(DONE|FATAL) ' "$console" 2> /dev/null; then
            sleep 0.3
            kill "$qpid" 2> /dev/null || true
            break
        fi
        sleep 0.25
    done
    set +e
    wait "$qpid"
    _rc=$?
    set -e
    kill "$wdpid" 2> /dev/null || true
    wait "$wdpid" 2> /dev/null || true
    wait "$rsspid" 2> /dev/null || true
    qemu_rss=$(cat "$rssfile" 2> /dev/null || echo 0)
    : "${qemu_rss:=0}"

    if ! grep -q '^BENCH_DONE ' "$console"; then
        echo "tests/throughput/run.sh: guest did not finish (qemu rc=$_rc); console tail:" >&2
        tail -n 40 "$console" | sed 's/^/    /' >&2
        return 1
    fi

    out=$here/results/$stamp-$_scen${_tag:+-$_tag}.csv
    cars=$here/results/$stamp-$_scen${_tag:+-$_tag}-carriers.csv
    {
        printf '%s\n' "$csv_header"
        awk -v cols="$csv_header" -v stamp="$stamp" -v scen="$_scen" \
            -v rss="$qemu_rss" -v others="$others" -v grev="$git_rev" \
            -v gdirty="$git_dirty" -v kosha="$ko_sha256" -v kern="$kernel_name" \
            -v qsha="$qemu_sha256" -v srcsha="$src_sha256" -v srcstat="$src_status" '
            BEGIN { n = split(cols, c, ","); for (i = 1; i <= n; i++) idx[c[i]] = i }
            { gsub(/\r/, "") }
            /^BENCHCFG/ { cfg = $0; sub(/^BENCHCFG[| ]?/, "", cfg); next }
            /^BENCHROW / {
                delete v
                for (i = 2; i <= NF; i++) { split($i, a, "="); v[a[1]] = a[2] }
                v["stamp"] = stamp; v["scenario"] = scen
                v["qemu_rss_peak_kb"] = rss; v["host_qemu_others"] = others
                v["git_rev"] = grev; v["git_dirty"] = gdirty
                v["ko_sha256"] = kosha; v["kernel"] = kern
                v["qemu_sha256"] = qsha; v["config"] = cfg
                v["src_sha256"] = srcsha; v["src_status"] = srcstat
                out = ""
                for (i = 1; i <= n; i++) {
                    x = v[c[i]]
                    if (x == "") x = ""
                    gsub(/,/, ";", x)
                    out = out (i > 1 ? "," : "") x
                }
                print out
            }' "$console"
    } > "$out"
    {
        printf '%s\n' "$carrier_header"
        awk -v stamp="$stamp" -v scen="$_scen" '
            { gsub(/\r/, "") }
            /^BENCHCAR / {
                delete v
                for (i = 2; i <= NF; i++) { split($i, a, "="); v[a[1]] = a[2] }
                printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", stamp, scen,
                       v["row"], v["carrier"], v["sport"], v["state"], v["profile"],
                       v["bytes"], v["frames"], v["dropped"], v["retx_ppm"], v["inflight"],
                       v["delivery_rate_bps"], v["cwnd_bytes"], v["snd_wnd"], v["rtt_us"],
                       v["wscale"], v["rcv_wnd"], v["window_clamp"], v["rmem_alloc"]
            }' "$console"
    } > "$cars"

    echo
    echo "== $_scen  ($out)"
    case $_scen in
    throughput)
        print_table "$out" row control_mbit tunnel_mbit ratio goodput_mbit wire_per_app frames_s cpu_total_pct kworker_pct rc strict
        ;;
    profile)
        print_table "$out" row goodput_mbit frames_s wire_per_app cpu_sys_pct cpu_softirq_pct cpu_total_pct kworker_pct rc
        ;;
    loss)
        print_table "$out" row drop_ppm goodput_mbit retx_ppm holes_declared holes_repaired holes_unrecoverable rx_max_stall_us reorder_max_depth rc strict
        ;;
    latency)
        print_table "$out" row iters lat_p50_us lat_p90_us lat_p95_us lat_p99_us lat_max_us lat_mean_us lat_complete rc
        ;;
    memory)
        print_table "$out" row mem_before_kb mem_peak_kb mem_after_kb proc_rss_peak_kb slab_before_kb slab_peak_kb slab_after_kb txq_max rxq_max ooo_bytes_max retain_bytes_max spec_bytes_max rc
        awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) h[$i] = i; next } { print "   slab delta: " $(h["slab_top"]) }' "$out"
        ;;
    scale)
        print_table "$out" row goodput_mbit carriers_up connect_fail alloc_fail carrier_lost carrier_replaced rc strict
        ;;
    wan)
        print_table "$out" row rx_mbit goodput_mbit rtt_mean_us car_snd_wnd_kb bdp_bytes k_x100 tx_window rx_reorder_bytes inflight_sum wire_per_app ooo_bytes_max reorder_max_depth txq_max rxq_max tx_ack retx_ppm carrier_lost holes_unrecoverable coverage inflight_over_window wan_ingress rc strict
        ;;
    flush-hang)
        print_table "$out" row hung flush_s verdict wchan stack
        ;;
    second-connection)
        print_table "$out" row while_active errno_active ms_active after_close errno_after ms_after verdict rc
        ;;
    hole-cost)
        print_table "$out" row iters lat_p50_us lat_p99_us lat_max_us lat_tail_n lat_tail_min_us lat_tail_p50_us lat_tail_p90_us lat_tail_p99_us lat_tail_max_us dropped_by_ppm holes_declared holes_repaired holes_unrecoverable rc
        ;;
    esac
    echo "   carriers: $cars"
    echo "   console:  $console"
    echo "   qemu peak RSS: ${qemu_rss} kB"
    _ret=0
    case $_scen in
    flush-hang | second-connection)
        if awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) h[$i] = i; next }
                    $(h["verdict"]) == "FAIL" { f = 1 }
                    END { exit f ? 0 : 1 }' "$out"; then
            echo "tests/throughput/run.sh: $_scen reports the known defect (verdict=FAIL)"
            _ret=1
        fi
        ;;
    esac
    return $_ret
}

# --- main -------------------------------------------------------------------
build_bins
ko_sha256=$(sha256sum "$root/tbtun.ko" | awk '{print $1}')
git_rev=$(git -C "$root" rev-parse --short HEAD 2> /dev/null || echo none)
git_dirty=0
if [ -n "$(git -C "$root" status --porcelain --untracked-files=no 2> /dev/null)" ]; then
    git_dirty=1
fi
# tbtun/ is not tracked by this repository, so `git status` cannot identify
# the module sources; a hash of the sources does, and src_status says whether
# git knows them at all.
src_sha256=$(find "$root/tbtun" -maxdepth 1 \( -name '*.c' -o -name '*.h' \) |
    sort | xargs sha256sum | sha256sum | cut -c1-16)
src_status=clean
case $(git -C "$root" status --porcelain -- tbtun 2> /dev/null | head -1) in
'') ;;
??*) src_status=untracked ;;
*) src_status=modified ;;
esac
kernel_name=$(basename "$kernel")
tc_bin=
[ -x /usr/bin/tc ] && tc_bin=/usr/bin/tc
[ -n "$tc_bin" ] || die "no /usr/bin/tc (the guest's busybox tc has no u32 filters)"
ip_bin=
[ -x /usr/bin/ip ] && ip_bin=/usr/bin/ip
case $scenario in
wan | all)
    [ -n "$ip_bin" ] || die "no /usr/bin/ip (the guest's busybox ip cannot create an ifb device)"
    ;;
esac

if [ "$scenario" = all ]; then
    set -- throughput profile loss latency memory scale wan
    for s in "$@"; do
        run_one "$s" "$mib" "$nlist" "$iters" "$loss_iters" "$losslist" "$netem" "$window" "$timeout_s" "$tag"
        echo
    done
else
    run_one "$scenario" "$mib" "$nlist" "$iters" "$loss_iters" "$losslist" "$netem" "$window" "$timeout_s" "$tag"
fi
