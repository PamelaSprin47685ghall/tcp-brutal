#!/bin/sh
# sclc-bidir.sh - real-machine acceptance for bidirectional 0.4.3 SC-LDPC
# adaptive rate.
#
# WHAT THIS IS
#   0.4.3 opens the full six-point ladder in both directions:
#
#     near TX (downlink, binary 1x256, block 256, L 8)
#       25/30/40/50/60/70% = 64/192, 76/180, 102/154, 128/128, 151/105,
#       178/78, transmitted rate n/256.
#     far TX (uplink, GF(16) 8x32, block 128, L 16)
#       25/30/40/50/60/70% = 8/24, 9/23, 12/20, 16/16, 18/14, 22/10,
#       transmitted rate as the GF(16) profile builds it.
#
#   This script drives both directions with controlled FEC-record loss and
#   checks each one against the same control law:
#
#       r_t = clamp(1 - p - 0.20, 0.25, 0.70)
#
#   and the six-point ladder above. Both ends run adaptive=on by default.
#   The near c13 is the uplink view; the far c13 is the downlink view. Each
#   direction must land on the ladder point its own p selects, and the
#   adaptive-vs-fixed row records the app/wire cost.
#
#   Every run rewrites tunnel 1 on both ends with the SC profiles, waits for
#   up=256, injects `carrier <i> drop_ppm` on the selected end(s), transfers
#   the far source through the tunnel on the near for WARMUP + DURATION
#   seconds, and checks the streamed sha256. A 0.25 s sampler records the
#   near and far c13 lines so the per-run transition count is measured, not
#   inferred from the final shape.
#
#   Injection ends:
#     both (default)  near and far both drop; both directions adapt under
#                     the same loss and each is validated independently.
#     near            only the near TX drops: downlink-only validation.
#     far             only the far TX drops: uplink-only validation.
#   A direction that is not injected keeps p near zero and must move to 70%.
#
#   Modes:
#     adaptive  adaptive=on on both ends; both TX controllers switch.
#     fixed     adaptive=off explicitly; near stays at 128/128 (rate 50)
#               and far stays at 8/24 (rate 25).
#     default   the `adaptive=` key is omitted; the parser defaults it off
#               for mode=3. This is the default-off regression.
#
# USAGE
#   benches/adaptive/sclc-bidir.sh --near root@192.168.123.85 \
#       --far root@45.11.1.62 --near-ip 192.168.123.85 --far-ip 45.11.1.62 \
#       --source /root/accept-1g/accept-2g.bin --quick --tag smoke
#
#   --quick  six representative losses (0,15,25,35,45,55%) x adaptive,fixed
#            + the default-off regression at 0,5%. Covers all six rates in
#            both directions.
#   --full   0,5,10,15,20,25,30,35,40,45,50,55% x adaptive,fixed, repeats,
#            + the default-off regression at 0,5,15,25,35,45,55%.
#   --inject-end both|near|far   where drop_ppm is written (default both).
#   --default-loss LIST   default-off regression points (default 0,5).
#   --no-default          skip the default-off regression.
#   --mode adaptive,fixed restrict the matrix; the default regression is
#                         separate and still runs unless --no-default.
#
# PRECONDITIONS
#   * Both ends run the 0.4.3 build with mode=sc-ldpc and c13.
#   * Both modules were loaded with debug_loss=1 when both ends are injected
#     (the far always needs it for the uplink; the near for the downlink).
#   * The far source file exists (see --source) and the far raw sender
#     listens on 127.0.0.1:5210 (review/wan-experiment/arm64/prod-1g/
#     far-serve.sh --start). A 2 GiB source is recommended so a 60 s
#     window is not cut short by the download finishing.
#   * The near reaches the far on --dev (default eth0); the idempotent
#     pref-100 ip rule is added by the shared harness.
#
# OUTPUT
#   benches/results/<UTC>-sclc-bidir[-tag]/
#     sclc-runs.csv     one row per run: both directions' c13 p/rate/n/m,
#                       CFG shapes, transitions/reversals, bytes, app/wire
#     sclc-runs.jsonl   the same rows as JSON objects
#     verdict.txt       S1..S7 PASS/FAIL/SKIP with the evidence
#     summary.txt       adaptive-vs-fixed app and wire table
#     runs/<name>/      near/far pre/a/b snapshots, near-c13.log,
#                       far-c13.log, sha, size, curl.rc/err, bytes.txt
#
# Runtime effects only: a config rewrite, carrier drop_ppm writes, and the
# near ip rule. Nothing here touches a kernel source tree or a real-machine
# binary. The verify operator runs it.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SCRIPT=sclc-bidir
. "$root/benches/adaptive/lib.sh"

SC_QUICK=1
SC_HAVE_LOSS=0
SC_HAVE_CARRIERS=0
SC_HAVE_MODE=0
SC_DEFAULT_OFF=0
SC_NO_DEFAULT=0
SC_DEFAULT_LOSS=${TBT_SC_DEFAULT_LOSS:-0,5}
SC_SAMPLE_INTERVAL=${TBT_SC_SAMPLE_INTERVAL:-0.25}
SC_SAMPLE_START=sclc_sample_start
SC_SAMPLE_STOP=sclc_sample_stop
SC_NEAR_LOG=
SC_NEAR_PID=
SC_FAR_LOG=
SC_FAR_PID=
INJECT_ENDS=${TBT_INJECT_ENDS:-both}

usage() {
    sed -n '2,85p' "$0" | sed 's/^# \{0,1\}//'
    usage_common
}

sclc_die() {
    die "$*"
}

sclc_rate_of() {
    case "$1/$2" in
    22/10) printf '70' ;;
    18/14) printf '60' ;;
    16/16) printf '50' ;;
    12/20) printf '40' ;;
    9/23) printf '30' ;;
    8/24) printf '25' ;;
    *) printf '0' ;;
    esac
}

sclc_down_rate_of() {
    case "$1/$2" in
    178/78) printf '70' ;;
    151/105) printf '60' ;;
    128/128) printf '50' ;;
    102/154) printf '40' ;;
    76/180) printf '30' ;;
    64/192) printf '25' ;;
    *) printf '0' ;;
    esac
}

sclc_target_from_p() {
    awk -v p="$1" 'BEGIN {
        r = 1 - p / 1000000 - 0.20;
        if (r < 0.25) r = 0.25;
        if (r > 0.70) r = 0.70;
        if (r >= 0.695652) print 70;
        else if (r >= 0.592593) print 60;
        else if (r >= 0.50) print 50;
        else if (r >= 0.40) print 40;
        else if (r >= 0.299985) print 30;
        else print 25;
    }'
}

# ---------------------------------------------------------------------------
# tunnel configuration: near TX = binary downlink, far TX = GF(16) uplink.
# These are the initial shapes; adaptive=on lets both controllers move them
# across the full ladder.

configure_pair() {
    cfg_carriers=$1
    cfg_adaptive=$2
    if [ "$cfg_carriers" != 256 ]; then
        sclc_die "SC-LDPC 0.4.3 profiles need carriers=256 (got $cfg_carriers)"
    fi
    remote_sn "$FAR" "$cfg_carriers" "$cfg_adaptive" "$FAR_PORT" "$TARGET" \
        "$RX_REORDER" "$TX_WINDOW" "$FAR_BLOCK" "$NONCE" "$SC_DEFAULT_OFF" <<'EOS' || die "far config write failed"
C=$1; A=$2; PORT=$3; TGT=$4; RX=$5; TW=$6; BLOCK=$7; NONCE=$8; OMIT=$9
AD=
[ "$OMIT" = 0 ] && AD=" adaptive=$A"
printf 'tunnel 1 role=far listen=0.0.0.0:%s target=%s n=8 m=24 fec=on mode=sc-ldpc w=128 L=16 carriers=%s%s crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=%s tx_window_bytes=%s tx_retain_bytes=0 carrier_stall_ms=0 portblock=%s nonce=%s\n' \
    "$PORT" "$TGT" "$C" "$AD" "$RX" "$TW" "$BLOCK" "$NONCE" > /proc/net/tbtun/config
EOS
    remote_sn "$NEAR" "$cfg_carriers" "$cfg_adaptive" "$NEAR_PORT" "$FAR_IP" "$FAR_PORT" \
        "$RX_REORDER" "$TX_WINDOW" "$NEAR_BLOCK" "$NONCE" "$SC_DEFAULT_OFF" "$NEAR_DEV" <<'EOS' || die "near config write failed"
C=$1; A=$2; PORT=$3; PEER=$4; PEERPORT=$5; RX=$6; TW=$7; BLOCK=$8; NONCE=$9; OMIT=${10}; DEV=${11}
AD=
[ "$OMIT" = 0 ] && AD=" adaptive=$A"
DEV_TOKEN=
[ -n "$DEV" ] && DEV_TOKEN=" dev=$DEV"
printf 'tunnel 1 role=near listen=127.0.0.1:%s peer=%s:%s n=128 m=128 fec=on mode=sc-ldpc w=256 carriers=%s%s crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=%s tx_window_bytes=%s tx_retain_bytes=0 carrier_stall_ms=0 portblock=%s nonce=%s%s\n' \
    "$PORT" "$PEER" "$PEERPORT" "$C" "$AD" "$RX" "$TW" "$BLOCK" "$NONCE" "$DEV_TOKEN" > /proc/net/tbtun/config
EOS
    far_cfg=$(remote_sh "$FAR" 'cat /proc/net/tbtun/config' 2>/dev/null || true)
    near_cfg=$(remote_sh "$NEAR" 'cat /proc/net/tbtun/config' 2>/dev/null || true)
    for pair in "far:$far_cfg" "near:$near_cfg"; do
        ph=${pair%%:*}
        pc=${pair#*:}
        case " $pc " in
        *" mode=sc-ldpc "*) ;;
        *) sclc_die "$ph: config read-back lacks mode=sc-ldpc" ;;
        esac
        case " $pc " in
        *" adaptive=$cfg_adaptive "*) ;;
        *) sclc_die "$ph: config read-back lacks adaptive=$cfg_adaptive" ;;
        esac
        case " $pc " in
        *" carriers=$cfg_carriers "*) ;;
        *) sclc_die "$ph: config read-back lacks carriers=$cfg_carriers" ;;
        esac
    done
    case " $far_cfg " in
    *" n=8 m=24 "*) ;;
    *) sclc_die "far: config read-back lacks the uplink n=8 m=24" ;;
    esac
    case " $far_cfg " in
    *" w=128 "*) ;;
    *) sclc_die "far: config read-back lacks the uplink w=128" ;;
    esac
    case " $far_cfg " in
    *" L=16 "*) ;;
    *) sclc_die "far: config read-back lacks the uplink L=16" ;;
    esac
    case " $near_cfg " in
    *" n=128 m=128 "*) ;;
    *) sclc_die "near: config read-back lacks the downlink n=128 m=128" ;;
    esac
    case " $near_cfg " in
    *" w=256 "*) ;;
    *) sclc_die "near: config read-back lacks the downlink w=256" ;;
    esac
}

# ---------------------------------------------------------------------------
# c13 sampler: 0.25 s near/far lines, started after join and stopped by the
# run grid (or by the trap). The remote loop stops by itself after SAFETY_SECS
# if this script dies without a cleanup.

sclc_sample_one() {
    so_host=$1
    so_file=$2
    so_pid=$3
    remote_sn "$so_host" "$so_file" "$so_pid" "$SAFETY_SECS" "$SC_SAMPLE_INTERVAL" <<'EOS' || return 1
F=$1; P=$2; LIMIT=$3; IV=$4
: > "$F"
END=$(( $(date +%s) + LIMIT ))
nohup sh -c 'F=$1; END=$2; IV=$3; while [ "$(date +%s)" -lt "$END" ]; do printf "%s " "$(date +%s.%N 2>/dev/null || date +%s)"; awk "/ c13 /{print; exit}" /proc/net/tbtun/stats; sleep "$IV"; done' sh "$F" "$END" "$IV" >/dev/null 2>&1 &
echo $! > "$P"
EOS
}

sclc_sample_start() {
    SC_NEAR_LOG=/tmp/tbt-sclc-$R_name-near.log
    SC_NEAR_PID=/tmp/tbt-sclc-$R_name-near.pid
    SC_FAR_LOG=/tmp/tbt-sclc-$R_name-far.log
    SC_FAR_PID=/tmp/tbt-sclc-$R_name-far.pid
    sclc_sample_one "$NEAR" "$SC_NEAR_LOG" "$SC_NEAR_PID" || \
        warn "near c13 sampler did not start"
    sclc_sample_one "$FAR" "$SC_FAR_LOG" "$SC_FAR_PID" || \
        warn "far c13 sampler did not start"
}

sclc_sample_stop_one() {
    ss_host=$1
    ss_pid=$2
    ss_log=$3
    ss_out=$4
    if [ -n "$ss_pid" ]; then
        remote_sh "$ss_host" "P='$ss_pid'; [ -f \"\$P\" ] && { kill \$(cat \"\$P\") 2>/dev/null || true; rm -f \"\$P\"; }; true" >/dev/null 2>&1 || true
    fi
    if [ -n "$ss_log" ] && [ -n "$ss_out" ]; then
        remote_sh "$ss_host" "cat '$ss_log' 2>/dev/null" >"$ss_out" 2>/dev/null || true
        remote_sh "$ss_host" "rm -f '$ss_log'" >/dev/null 2>&1 || true
    fi
}

sclc_sample_stop() {
    sclc_sample_stop_one "$NEAR" "$SC_NEAR_PID" "$SC_NEAR_LOG" \
        "${R_dir:-/tmp}/near-c13.log"
    sclc_sample_stop_one "$FAR" "$SC_FAR_PID" "$SC_FAR_LOG" \
        "${R_dir:-/tmp}/far-c13.log"
    SC_NEAR_LOG=
    SC_NEAR_PID=
    SC_FAR_LOG=
    SC_FAR_PID=
}

# ---------------------------------------------------------------------------
# analysis helpers

config_line() {
    awk '/^@@config/{p=1;next} /^@@/{p=0} p && NF {print; exit}' "$1" 2>/dev/null || true
}

config_shape() {
    cl=$(config_line "$1")
    printf '%s\n' "$cl" | tr ' ' '\n' | awk -F= '
        $1 == "mode" || $1 == "n" || $1 == "m" || $1 == "w" ||
        $1 == "L" || $1 == "adaptive" || $1 == "carriers" {
            printf "%s=%s ", $1, $2
        }'
}

c13_series_stats() {
    awk -v dir="$2" '
    / c13 / {
        n=""; m="";
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^n=/) n = substr($i, 3);
            if ($i ~ /^m=/) m = substr($i, 3);
        }
        if (n == "" || m == "") next;
        rate = 0;
        if (dir == "down") {
            if (n == 178 && m == 78) rate = 70;
            else if (n == 151 && m == 105) rate = 60;
            else if (n == 128 && m == 128) rate = 50;
            else if (n == 102 && m == 154) rate = 40;
            else if (n == 76 && m == 180) rate = 30;
            else if (n == 64 && m == 192) rate = 25;
        } else {
            if (n == 22 && m == 10) rate = 70;
            else if (n == 18 && m == 14) rate = 60;
            else if (n == 16 && m == 16) rate = 50;
            else if (n == 12 && m == 20) rate = 40;
            else if (n == 9 && m == 23) rate = 30;
            else if (n == 8 && m == 24) rate = 25;
        }
        if (!rate) next;
        samples++;
        if (prev && rate != prev) {
            changes++;
            d = rate - prev;
            if (pd && d * pd < 0) rev++;
            pd = d;
        }
        prev = rate;
    }
    END { printf "%d %d %d\n", changes, rev, samples }
    ' "$1" 2>/dev/null || printf '0 0 0\n'
}

# ---------------------------------------------------------------------------
# one row

sclc_collect() {
    col_dir=$1
    col_mode=$2
    col_loss=$3
    col_rep=$4
    col_n=$(c13_field "$col_dir/near-b.txt" n)
    col_m=$(c13_field "$col_dir/near-b.txt" m)
    col_p=$(num "$(c13_field "$col_dir/near-b.txt" p_ppm)")
    col_pwin=$(num "$(c13_field "$col_dir/near-b.txt" p_win_ppm)")
    col_epoch=$(num "$(c13_field "$col_dir/near-b.txt" epoch)")
    col_state=$(csv_safe "$(c13_field "$col_dir/near-b.txt" state)")
    col_reason=$(csv_safe "$(c13_field "$col_dir/near-b.txt" reason)")
    col_sent=$(num "$(c13_field "$col_dir/near-b.txt" sent)")
    col_miss=$(num "$(c13_field "$col_dir/near-b.txt" miss)")
    col_far_n=$(c13_field "$col_dir/far-b.txt" n)
    col_far_m=$(c13_field "$col_dir/far-b.txt" m)
    col_far_p=$(num "$(c13_field "$col_dir/far-b.txt" p_ppm)")
    col_far_pwin=$(num "$(c13_field "$col_dir/far-b.txt" p_win_ppm)")
    col_far_r=$(num "$(c13_field "$col_dir/far-b.txt" r_fail_ppm)")
    col_far_state=$(csv_safe "$(c13_field "$col_dir/far-b.txt" state)")
    col_far_reason=$(csv_safe "$(c13_field "$col_dir/far-b.txt" reason)")
    col_rate=$(sclc_rate_of "$col_n" "$col_m")
    col_expected=$(sclc_target_from_p "$col_p")
    col_perr=$(awk -v p="$col_p" -v l="$col_loss" 'BEGIN { printf "%.2f", p / 10000 - l }')
    col_far_rate=$(sclc_down_rate_of "$col_far_n" "$col_far_m")
    col_far_expected=$(sclc_target_from_p "$col_far_p")
    col_far_perr=$(awk -v p="$col_far_p" -v l="$col_loss" 'BEGIN { printf "%.2f", p / 10000 - l }')
    col_seq=$(c13_series_stats "$col_dir/near-c13.log" up)
    col_trans=$(printf '%s\n' "$col_seq" | awk '{ print $1 }')
    col_rev=$(printf '%s\n' "$col_seq" | awk '{ print $2 }')
    col_samples=$(printf '%s\n' "$col_seq" | awk '{ print $3 }')
    col_down_seq=$(c13_series_stats "$col_dir/far-c13.log" down)
    col_down_trans=$(printf '%s\n' "$col_down_seq" | awk '{ print $1 }')
    col_down_rev=$(printf '%s\n' "$col_down_seq" | awk '{ print $2 }')
    col_down_samples=$(printf '%s\n' "$col_down_seq" | awk '{ print $3 }')
    col_up=$(num "$(stat_field "$col_dir/near-b.txt" 'tunnel 1 c3 ' up)")
    col_cfg_near=$(csv_safe "$(config_shape "$col_dir/near-pre.txt")")
    col_cfg_far=$(csv_safe "$(config_shape "$col_dir/far-pre.txt")")
    col_sha=$(csv_safe "${R_sha:-}")
    col_curl=$(num "${R_curl_rc:-0}")
    col_bytes_ok=$(num "${R_bytes_ok:-0}")
    col_rc=$(num "${R_rc:-0}")
    col_note=$(csv_safe "${R_note:-}")
    [ -n "$col_state" ] || col_state=-
    [ -n "$col_reason" ] || col_reason=-
    [ -n "$col_far_state" ] || col_far_state=-
    [ -n "$col_far_reason" ] || col_far_reason=-
    [ -n "$col_n" ] || col_n=0
    [ -n "$col_m" ] || col_m=0
    [ -n "$col_far_n" ] || col_far_n=0
    [ -n "$col_far_m" ] || col_far_m=0
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$(date -u +%Y%m%dT%H%M%SZ)" "$col_mode" "$col_loss" \
        "$((col_loss * 10000))" "$R_carriers" "$col_cfg_near" "$col_cfg_far" \
        "$col_p" "$col_pwin" "$col_perr" "$col_rate" "$col_expected" \
        "$col_n" "$col_m" "$col_epoch" "$col_trans" "$col_rev" \
        "$col_samples" "$col_state" "$col_reason" "$col_far_n" "$col_far_m" \
        "$col_far_r" "$col_sent" "$col_miss" "${R_app_mbps:-0}" \
        "${R_wire_per_app:-0}" "${R_retx_ppm:-0}" "$col_up" \
        "${R_holes:-0}" "${R_reset:-0}" "${R_lost:-0}" "${R_stall:-0}" \
        "$col_bytes_ok" "$col_rc" "$col_sha" "${R_bytes:-0}" \
        "${R_near_cpu:-0}" "${R_far_cpu:-0}" "$col_note" \
        "$col_far_p" "$col_far_pwin" "$col_far_perr" "$col_far_rate" \
        "$col_far_expected" "$col_down_trans" "$col_down_rev" \
        "$col_down_samples" "$col_far_state" "$col_far_reason" \
        >>"$OUT/sclc-runs.csv"
    printf '{"stamp":"%s","mode":"%s","loss_pct":%s,"carriers":%s,' \
        "$(date -u +%Y%m%dT%H%M%SZ)" "$col_mode" "$col_loss" "$R_carriers" \
        >>"$OUT/sclc-runs.jsonl"
    printf '"p_ppm":%s,"p_win_ppm":%s,"p_err_pp":%s,"rate_pct":%s,' \
        "$col_p" "$col_pwin" "$col_perr" "$col_rate" \
        >>"$OUT/sclc-runs.jsonl"
    printf '"expected_rate":%s,"n":%s,"m":%s,"epoch":%s,' \
        "$col_expected" "$col_n" "$col_m" "$col_epoch" \
        >>"$OUT/sclc-runs.jsonl"
    printf '"transitions":%s,"reversals":%s,"c13_samples":%s,' \
        "$col_trans" "$col_rev" "$col_samples" \
        >>"$OUT/sclc-runs.jsonl"
    printf '"far_n":%s,"far_m":%s,"far_r_fail_ppm":%s,' \
        "$col_far_n" "$col_far_m" "$col_far_r" \
        >>"$OUT/sclc-runs.jsonl"
    printf '"down_p_ppm":%s,"down_p_win_ppm":%s,"down_p_err_pp":%s,' \
        "$col_far_p" "$col_far_pwin" "$col_far_perr" \
        >>"$OUT/sclc-runs.jsonl"
    printf '"down_rate_pct":%s,"down_expected_rate":%s,' \
        "$col_far_rate" "$col_far_expected" >>"$OUT/sclc-runs.jsonl"
    printf '"down_transitions":%s,"down_reversals":%s,"down_c13_samples":%s,' \
        "$col_down_trans" "$col_down_rev" "$col_down_samples" \
        >>"$OUT/sclc-runs.jsonl"
    printf '"down_state":"%s","down_reason":"%s",' \
        "$col_far_state" "$col_far_reason" >>"$OUT/sclc-runs.jsonl"
    printf '"app_mbps":%s,"wire_per_app":%s,"retx_ppm":%s,' \
        "${R_app_mbps:-0}" "${R_wire_per_app:-0}" "${R_retx_ppm:-0}" \
        >>"$OUT/sclc-runs.jsonl"
    printf '"up":%s,"holes":%s,"reset":%s,"lost":%s,"stall":%s,' \
        "$col_up" "${R_holes:-0}" "${R_reset:-0}" "${R_lost:-0}" \
        "${R_stall:-0}" >>"$OUT/sclc-runs.jsonl"
    printf '"bytes_ok":%s,"rc":%s,"sha":"%s","bytes":%s}\n' \
        "$col_bytes_ok" "$col_rc" "$col_sha" "${R_bytes:-0}" \
        >>"$OUT/sclc-runs.jsonl"
}

sclc_header() {
    printf '%s\n' \
        'stamp,mode,loss_pct,ppm,carriers,cfg_near,cfg_far,p_ppm,p_win_ppm,p_err_pp,rate_pct,expected_rate,n,m,epoch,transitions,reversals,c13_samples,state,reason,far_n,far_m,far_r_fail_ppm,sent,miss,app_mbps,wire_per_app,retx_ppm,up,holes,reset,lost,stall,bytes_ok,rc,sha,bytes,near_cpu,far_cpu,note,down_p_ppm,down_p_win_ppm,down_p_err_pp,down_rate_pct,down_expected_rate,down_transitions,down_reversals,down_c13_samples,down_state,down_reason' \
        >"$OUT/sclc-runs.csv"
}

# ---------------------------------------------------------------------------
# grid

sclc_run_grid() {
    mkdir -p "$OUT"
    : >"$OUT/run.log"
    : >"$OUT/sclc-runs.jsonl"
    csv_init
    sclc_header
    note "SC-LDPC 0.4.3 bidir: loss=$LOSS_LIST modes=$MODE_LIST inject=$INJECT_ENDS repeats=$REPEATS window=${WARMUP}+${DURATION}s"
    note "initial shapes: near downlink 128/128 w=256; far uplink 8/24 w=128 L=16; carriers=256"
    grid_rc=0
    rep=1
    while [ "$rep" -le "$REPEATS" ]; do
        for loss in $(printf '%s' "$LOSS_LIST" | tr ',' ' '); do
            for mode in $(printf '%s' "$MODE_LIST" | tr ',' ' '); do
                case $mode in
                adaptive | fixed) ;;
                *) continue ;;
                esac
                SC_DEFAULT_OFF=0
                if ! run_one ppm "$loss" "$R_carriers" "$mode" "$rep" 0; then
                    grid_rc=1
                fi
                sclc_sample_stop
                sclc_collect "$R_dir" "$mode" "$loss" "$rep"
                if [ "${STOP:-0}" = 1 ]; then
                    note "stop: hard failure in $mode loss=$loss"
                    return 1
                fi
            done
        done
        if [ "$SC_NO_DEFAULT" = 0 ]; then
            for loss in $(printf '%s' "$SC_DEFAULT_LOSS" | tr ',' ' '); do
                SC_DEFAULT_OFF=1
                if ! run_one ppm "$loss" "$R_carriers" default "$rep" 0; then
                    grid_rc=1
                fi
                sclc_sample_stop
                sclc_collect "$R_dir" "default" "$loss" "$rep"
                if [ "${STOP:-0}" = 1 ]; then
                    note "stop: hard failure in default loss=$loss"
                    return 1
                fi
            done
        fi
        rep=$((rep + 1))
    done
    return "$grid_rc"
}

# ---------------------------------------------------------------------------
# verdict

SC_V_FILE=
SC_V_FAIL=0

sclc_v() {
    sclc_v_name=$1
    sclc_v_status=$2
    sclc_v_detail=$3
    printf '%s: %s %s\n' "$sclc_v_name" "$sclc_v_status" "$sclc_v_detail" >>"$SC_V_FILE"
    [ "$sclc_v_status" = FAIL ] && SC_V_FAIL=1
    return 0
}

sclc_verdict() {
    vf=$OUT/verdict.txt
    sf=$OUT/summary.txt
    : >"$vf"
    : >"$sf"
    SC_V_FILE=$vf
    SC_V_FAIL=0

    awk -F, -v ends="$INJECT_ENDS" '
    NR == 1 { next }
    {
        rows++;
        if ($34 != 1 || $35 != 0 || $30 != 0 || $31 != 0 || $32 != 0 ||
            $33 != 0 || $29 != $5)
            bad_integrity++;
        if ($2 == "adaptive") {
            arows++;
            if (ends != "near") {
                if ($3 + 0 > 0) {
                    pe = $10 < 0 ? -$10 : $10;
                    if (pe > 1.5) bad_p++;
                } else if ($8 + 0 > 5000) bad_p0++;
            } else if ($8 + 0 > 5000) bad_p0++;
            if (ends != "far") {
                if ($3 + 0 > 0) {
                    pe = $43 < 0 ? -$43 : $43;
                    if (pe > 1.5) bad_p++;
                } else if ($41 + 0 > 5000) bad_p0++;
            } else if ($41 + 0 > 5000) bad_p0++;
            if ($11 != $12) bad_rate++;
            if ($44 != $45) bad_rate++;
            if ($16 > 3 || $17 > 1) bad_osc++;
            if ($46 > 3 || $47 > 1) bad_osc++;
            if ($18 < 1 || $48 < 1) bad_sample++;
            seen_up[$11] = 1;
            seen_down[$44] = 1;
            if ($6 !~ /n=128 m=128/) bad_down_cfg++;
            if ($7 !~ /n=8 m=24/) bad_up_cfg++;
        } else {
            frows++;
            if ($11 != 25) bad_fixed_rate++;
            if ($44 != 50) bad_fixed_rate++;
            if ($16 != 0 || $46 != 0) bad_fixed_osc++;
            if ($6 !~ /adaptive=off/ || $7 !~ /adaptive=off/) bad_off_cfg++;
        }
        if ($2 == "default" &&
            ($6 !~ /adaptive=off/ || $7 !~ /adaptive=off/))
            bad_default_cfg++;
    }
    END {
        printf "rows=%d adaptive=%d fixed/default=%d integrity_bad=%d p_bad=%d p0_bad=%d rate_bad=%d osc_bad=%d sample_bad=%d fixed_rate_bad=%d fixed_osc_bad=%d down_cfg_bad=%d up_cfg_bad=%d off_cfg_bad=%d default_cfg_bad=%d rates_up=%d%d%d%d%d%d rates_down=%d%d%d%d%d%d\n",
            rows, arows, frows, bad_integrity, bad_p, bad_p0, bad_rate,
            bad_osc, bad_sample, bad_fixed_rate, bad_fixed_osc,
            bad_down_cfg, bad_up_cfg, bad_off_cfg, bad_default_cfg,
            (seen_up[25] ? 1 : 0), (seen_up[30] ? 1 : 0),
            (seen_up[40] ? 1 : 0), (seen_up[50] ? 1 : 0),
            (seen_up[60] ? 1 : 0), (seen_up[70] ? 1 : 0),
            (seen_down[25] ? 1 : 0), (seen_down[30] ? 1 : 0),
            (seen_down[40] ? 1 : 0), (seen_down[50] ? 1 : 0),
            (seen_down[60] ? 1 : 0), (seen_down[70] ? 1 : 0);
    }' "$OUT/sclc-runs.csv" >"$OUT/sclc-verdict.dat"

    if [ -s "$OUT/sclc-verdict.dat" ]; then
        read -r sclc_verdict_line <"$OUT/sclc-verdict.dat"
    else
        sclc_verdict_line="rows=0 adaptive=0 fixed/default=0 integrity_bad=0 p_bad=0 p0_bad=0 rate_bad=0 osc_bad=0 sample_bad=0 fixed_rate_bad=0 fixed_osc_bad=0 down_cfg_bad=0 up_cfg_bad=0 off_cfg_bad=0 default_cfg_bad=0 rates_up=000000 rates_down=000000"
    fi
    sclc_rows=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="rows"{print $2}')
    sclc_arows=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="adaptive"{print $2}')
    sclc_frows=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="fixed/default"{print $2}')
    sclc_integrity=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="integrity_bad"{print $2}')
    sclc_pbad=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="p_bad"{print $2}')
    sclc_p0bad=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="p0_bad"{print $2}')
    sclc_ratebad=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="rate_bad"{print $2}')
    sclc_oscbad=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="osc_bad"{print $2}')
    sclc_samplebad=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="sample_bad"{print $2}')
    sclc_fixedrate=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="fixed_rate_bad"{print $2}')
    sclc_fixedosc=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="fixed_osc_bad"{print $2}')
    sclc_downcfg=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="down_cfg_bad"{print $2}')
    sclc_upcfg=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="up_cfg_bad"{print $2}')
    sclc_offcfg=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="off_cfg_bad"{print $2}')
    sclc_defcfg=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="default_cfg_bad"{print $2}')
    sclc_rates_up=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="rates_up"{print $2}')
    sclc_rates_down=$(printf '%s\n' "$sclc_verdict_line" | tr ' ' '\n' | awk -F= '$1=="rates_down"{print $2}')

    if [ "$sclc_rows" -gt 0 ] && [ "$sclc_integrity" = 0 ]; then
        sclc_v S1 PASS "bytes/lost/stall/holes/reset/up clean on $sclc_rows runs"
    else
        sclc_v S1 FAIL "integrity_bad=$sclc_integrity rows=$sclc_rows"
    fi
    if [ "${sclc_arows:-0}" = 0 ]; then
        sclc_v S2 SKIP "no adaptive rows"
    elif [ "$sclc_pbad" = 0 ] && [ "$sclc_p0bad" = 0 ]; then
        sclc_v S2 PASS "injected direction p_est within 1.5 pp (loss>0) and <=0.5% (loss 0), both directions"
    else
        sclc_v S2 FAIL "p_bad=$sclc_pbad p0_bad=$sclc_p0bad"
    fi
    if [ "${sclc_arows:-0}" = 0 ]; then
        sclc_v S3 SKIP "no adaptive rows"
    elif [ "$sclc_ratebad" = 0 ] && [ "$sclc_downcfg" = 0 ] && [ "$sclc_upcfg" = 0 ]; then
        sclc_v S3 PASS "both final rates match their p-selected ladder point; initial configs are 128/128 down and 8/24 up"
    else
        sclc_v S3 FAIL "rate_bad=$sclc_ratebad down_cfg_bad=$sclc_downcfg up_cfg_bad=$sclc_upcfg"
    fi
    if [ "${sclc_arows:-0}" = 0 ]; then
        sclc_v S4 SKIP "no adaptive rows"
    elif [ "$sclc_oscbad" = 0 ] && [ "$sclc_samplebad" = 0 ]; then
        sclc_v S4 PASS "both directions transitions<=3, reversals<=1, c13 sampled"
    else
        sclc_v S4 FAIL "osc_bad=$sclc_oscbad sample_bad=$sclc_samplebad"
    fi
    if [ "${sclc_arows:-0}" = 0 ]; then
        sclc_v S5 SKIP "no adaptive rows"
    elif [ "$SC_HAVE_LOSS" = 1 ]; then
        sclc_v S5 SKIP "custom --loss may not cover the six points; up=$sclc_rates_up down=$sclc_rates_down"
    elif [ "$INJECT_ENDS" = both ] && [ "$sclc_rates_up" = 111111 ] && [ "$sclc_rates_down" = 111111 ]; then
        sclc_v S5 PASS "both directions observed all six rates (25,30,40,50,60,70)"
    elif [ "$INJECT_ENDS" = near ] && [ "$sclc_rates_down" = 111111 ]; then
        sclc_v S5 PASS "downlink observed all six rates (uplink not injected: up=$sclc_rates_up)"
    elif [ "$INJECT_ENDS" = far ] && [ "$sclc_rates_up" = 111111 ]; then
        sclc_v S5 PASS "uplink observed all six rates (downlink not injected: down=$sclc_rates_down)"
    else
        sclc_v S5 FAIL "six-rate coverage incomplete: up=$sclc_rates_up down=$sclc_rates_down ends=$INJECT_ENDS"
    fi
    if [ "${sclc_frows:-0}" = 0 ]; then
        sclc_v S6 SKIP "no fixed/default rows"
    elif [ "$sclc_fixedrate" = 0 ] && [ "$sclc_fixedosc" = 0 ] && [ "$sclc_offcfg" = 0 ] && [ "$sclc_defcfg" = 0 ]; then
        sclc_v S6 PASS "fixed/default stay at down 128/128 and up 8/24, no switches, adaptive=off"
    else
        sclc_v S6 FAIL "fixed_rate_bad=$sclc_fixedrate fixed_osc_bad=$sclc_fixedosc off_cfg_bad=$sclc_offcfg default_cfg_bad=$sclc_defcfg"
    fi

    {
        printf 'adaptive-vs-fixed (app MB/s, wire/app, final up/down rate)\n'
        awk -F, '
        NR == 1 { next }
        $2 == "adaptive" { a[$3] = $26; aw[$3] = $27; ar[$3] = $11 "/" $44 }
        $2 == "fixed" { f[$3] = $26; fw[$3] = $27; fr[$3] = $11 "/" $44 }
        END {
            for (l in a)
                printf "loss=%s%% adaptive=%.3f/%.4f rates=%s fixed=%.3f/%.4f rates=%s\n",
                    l, a[l], aw[l], ar[l], f[l], fw[l], fr[l];
        }' "$OUT/sclc-runs.csv" | sort -t= -k2 -n
    } >"$sf"
    sclc_v S7 RECORD "adaptive-vs-fixed app/wire and final up/down rates in summary.txt"

    if [ "$SC_V_FAIL" = 0 ]; then
        printf 'S_RESULT: PASS\n' >>"$vf"
        return 0
    fi
    printf 'S_RESULT: FAIL\n' >>"$vf"
    return 1
}

# ---------------------------------------------------------------------------
# dry run

sclc_dry_run() {
    say "dry-run: $SCRIPT (no ssh, no writes)"
    say "  near=$NEAR ($NEAR_IP) far=$FAR ($FAR_IP)"
    say "  near dev=$NEAR_DEV ports near=$NEAR_PORT far=$FAR_PORT target=$TARGET"
    say "  near config: tunnel 1 role=near ... n=128 m=128 fec=on mode=sc-ldpc w=256 carriers=256 adaptive=<on|off>"
    say "  far  config: tunnel 1 role=far  ... n=8 m=24 fec=on mode=sc-ldpc w=128 L=16 carriers=256 adaptive=<on|off>"
    say "  uplink ladder (far TX):  22/10=70 18/14=60 16/16=50 12/20=40 9/23=30 8/24=25"
    say "  downlink ladder (near TX): 178/78=70 151/105=60 128/128=50 102/154=40 76/180=30 64/192=25"
    say "  adaptive runs: each final rate must match 1-p-0.20 on its own ladder"
    say "  loss list $LOSS_LIST % (quick=$SC_QUICK) modes=$MODE_LIST"
    say "  default-off regression losses=$SC_DEFAULT_LOSS (skip=$SC_NO_DEFAULT)"
    say "  window ${WARMUP}s warm-up + ${DURATION}s steady, repeats=$REPEATS"
    say "  inject: ends=$INJECT_ENDS carrier <i> drop_ppm <loss*10000>; debug_loss=1 on each injected end"
    say "  sample: near/far c13 every ${SC_SAMPLE_INTERVAL}s after join"
    say "  transfer: curl -fsS --max-time $((WARMUP + DURATION + 2)) http://127.0.0.1:$NEAR_PORT$URL_PATH"
    say "  checks: S1 bytes/lost/stall/holes/reset/up, S2 p_est per direction, S3 rate/config per"
    say "          direction, S4 oscillation per direction, S5 six rates per direction,"
    say "          S6 fixed/default, S7 adaptive-vs-fixed record"
    say "  output: ${OUT:-<results>/<UTC>-sclc-bidir[-tag]}"
}

# ---------------------------------------------------------------------------
# main

while [ $# -gt 0 ]; do
    case $1 in
    -h | --help) usage; exit 0 ;;
    --quick) SC_QUICK=1; shift; continue ;;
    --full) SC_QUICK=0; shift; continue ;;
    --default-loss) SC_DEFAULT_LOSS=$2; shift 2; continue ;;
    --no-default) SC_NO_DEFAULT=1; shift; continue ;;
    --inject-end) INJECT_ENDS=$2; shift 2; continue ;;
    --loss) SC_HAVE_LOSS=1 ;;
    --carriers) SC_HAVE_CARRIERS=1 ;;
    --mode) SC_HAVE_MODE=1 ;;
    esac
    if common_arg "$1" "${2:-}"; then
        shift "$SHIFT_BY"
        continue
    fi
    die "unknown option $1 (try --help)"
done

if [ "$SC_HAVE_LOSS" = 0 ]; then
    if [ "$SC_QUICK" = 1 ]; then
        LOSS_LIST=0,15,25,35,45,55
    else
        LOSS_LIST=0,5,10,15,20,25,30,35,40,45,50,55
    fi
fi
if [ "$SC_HAVE_CARRIERS" = 1 ] && [ "$CARRIER_LIST" != 256 ]; then
    sclc_die "SC-LDPC 0.4.3 profiles need --carriers 256 (got $CARRIER_LIST)"
fi
case $INJECT_ENDS in
near | far | both) ;;
*) sclc_die "--inject-end must be near|far|both (got $INJECT_ENDS)" ;;
esac
CARRIER_LIST=256
R_carriers=256
if [ "$SC_HAVE_MODE" = 0 ]; then
    MODE_LIST=adaptive,fixed
fi

if [ -z "$OUT" ]; then
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    OUT=$root/benches/results/$stamp-sclc-bidir${TAG:+-$TAG}
fi

if [ "$DRY" = 1 ]; then
    sclc_dry_run
    exit 0
fi

if [ "$CLEANUP" = 1 ]; then
    CLEAN_PPM=1
    CLEAN_TUNNEL=1
    do_cleanup
    exit 0
fi

prepare_near
preflight_hosts
preflight_ppm
install_trap

rc=0
sclc_run_grid || rc=1
sclc_verdict || rc=1

if [ "$rc" -eq 0 ]; then
    say "$SCRIPT: PASS -> $OUT (sclc-runs.csv, verdict.txt, summary.txt)"
else
    say "$SCRIPT: FAIL -> $OUT (see verdict.txt)" >&2
fi
exit "$rc"
