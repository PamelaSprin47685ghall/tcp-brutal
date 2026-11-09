#!/bin/sh
# lib.sh - shared engine for the 0.3.0 adaptive-FEC acceptance scripts.
#
# Sourced by drop_ppm.sh, netem.sh and matrix.sh. It owns the parts that must
# not drift between them:
#
#   * near/far ssh, the near bypass rule and the join gate. The tokens and the
#     idempotent `ip rule ... pref 100` are the ones benches/short.sh and
#     review/wan-experiment/arm64/prod-1g/run-1g.sh already use on these hosts.
#   * the tunnel config renderer, with `adaptive=on|off`.
#   * snapshots of /proc/net/tbtun/{stats,peers,config} and the counter
#     extraction short.sh uses (c3, c6, c9, c11, peers tx_bytes, /proc/stat).
#   * one run: configure, join, inject, WARMUP s warm-up, DURATION s steady
#     window, stop, verify bytes.
#   * the grid, CSV/JSON rows, medians and the S1..S12 report.
#
# Runtime effects only: a config rewrite, `carrier <i> drop_ppm <ppm>` writes
# and tc qdiscs. Nothing here touches a kernel source tree.
#
# Entry scripts set SCRIPT before sourcing and call:
#   common_arg (arg loop), preflight_hosts, prepare_near, run_grid, aggregate_to
#
# Field names below were read from the 0.3.0 design (review/
# adaptive-fec-0.3.0-design.md) and from the current tbtun procfs:
#   c13 (0.3.0): adaptive epoch n m groups class p_ppm p_win_ppm r_fail_ppm
#                sent miss switches aborts rollbacks pauses pause_ns
#                sample_unknown abandoned state reason
#   c11 (HEAD):  fec_nack_retx_bytes fec_replace_retx_bytes fec_reset_hole ...
#   c6  (HEAD):  holes_unrecoverable rx_max_stall_us
#   c3  (HEAD):  up lost replaced stall_declared ...
# The c13 parser accepts `key=value` and, if the build renders it
# positionally, the order above.

# ---------------------------------------------------------------------------
# defaults (environment wins; command line wins over both)

NEAR=${TBT_NEAR:-root@192.168.123.82}
FAR=${TBT_FAR:-root@45.11.1.62}
NEAR_IP=${TBT_NEAR_IP:-192.168.123.82}
FAR_IP=${TBT_FAR_IP:-45.11.1.62}
NEAR_DEV=${TBT_NEAR_DEV:-eth0}
NEAR_MODULE=${TBT_NEAR_MODULE:-}
NEAR_PORT=${TBT_NEAR_PORT:-9001}
FAR_PORT=${TBT_FAR_PORT:-9000}
TARGET_PORT=${TBT_TARGET_PORT:-5210}
TARGET=127.0.0.1:$TARGET_PORT
URL_PATH=${TBT_URL_PATH:-/accept-1g.bin}
SOURCE=${TBT_SOURCE:-/root/accept-1g/accept-1g.bin}
SOURCE_MIB=${TBT_SOURCE_MIB:-1024}
SOURCE_BYTES=0
NEAR_BLOCK=${TBT_NEAR_BLOCK:-30000-30511}
FAR_BLOCK=${TBT_FAR_BLOCK:-31000-31511}
NONCE=${TBT_NONCE:-00112233445566778899aabbccddeeff}
RX_REORDER=${TBT_RX_REORDER:-8388608}
TX_WINDOW=${TBT_TX_WINDOW:-0}
N=${TBT_N:-4}
M=${TBT_M:-4}

CARRIER_LIST=${TBT_CARRIERS:-256}
LOSS_LIST=${TBT_LOSS:-0,1,3,5,10}
MODE_LIST=${TBT_MODES:-fixed,adaptive}
REPEATS=${TBT_REPEATS:-1}
DURATION=${TBT_DURATION:-60}
WARMUP=${TBT_WARMUP:-10}
JOIN_TIMEOUT=${TBT_JOIN_TIMEOUT:-180}
SSH_TMO=${TBT_SSH_TMO:-10}
SAFETY_SECS=${TBT_SAFETY_SECS:-600}

TAG=${TBT_TAG:-}
OUT=${TBT_OUT:-}
DRY=${TBT_DRY_RUN:-0}
CLEANUP=0
CLEAN_PPM=0
CLEAN_NETEM=0
CLEAN_TUNNEL=0

INJECT_ENDS=${TBT_INJECT_ENDS:-far}   # drop_ppm: near|far|both
NETEM_WHERE=${TBT_NETEM_WHERE:-far}   # netem: near|far|both
NETEM_BURST=${TBT_NETEM_BURST:-0}     # 1: loss with 25% correlation
NETEM_SIMPLE=${TBT_NETEM_SIMPLE:-0}   # 1: root netem, no prio/u32 filter
KILL_AT=${TBT_KILL_AT:-}
KILL_CARRIER=${TBT_KILL_CARRIER:--1}

BYPASS_PREF=${TBT_BYPASS_PREF:-100}
BYPASS_MARK=${TBT_BYPASS_MARK:-/root/.tbt-adaptive-bypass-pref100}
ACTION_FILE=${TBT_ACTION_FILE:-auto}

REMOTE_DIR=/tmp/tbt-adaptive
NETEM_MARK=/tmp/tbt-adaptive-netem
NETEM_DEV_NEAR=
NETEM_DEV_FAR=
CHECK_BYTES=${CHECK_BYTES:-${root:-.}/benches/adaptive/check-bytes.sh}

INJECT_ACTIVE=
TRANSFER_STARTED=0
TRANSFER_FAILS=0
TRANSFER_FAIL_MAX=${TBT_TRANSFER_FAIL_MAX:-3}
STOP=0
TA_TRAP=0

# CSV schema. Keep CSV_HEADER and row_emit in the same order; row_emit counts
# the fields and dies on drift.
CSV_HEADER='stamp,script,inject,loss_pct,ppm,carriers,mode,adaptive,rep,order,window_s,warmup_s,early,app_bytes,app_mbps,curl_bytes,wire_bytes,wire_per_app,retx_bytes,retx_ppm,p_ppm,p_win_ppm,p_delta_pp,sent,miss,switches,aborts,rollbacks,state,reason,near_cpu_pct,far_cpu_pct,hol_max_us,stall_declared,holes_unrecoverable,fec_reset_hole,lost,has_c13,near_sha256,bytes,bytes_ok,curl_rc,rc,r_fail_ppm,pauses,pause_ns,sample_unknown,abandoned,note'
CSV_NF=49

MED_HEADER='inject,loss_pct,carriers,mode,runs,app_mbps_med,app_mbps_min,app_mbps_max,wire_per_app_med,retx_ppm_med,p_ppm_med,p_delta_pp_max,switches_med,near_cpu_med,bytes_ok_all'

# ---------------------------------------------------------------------------
# small helpers

say() { printf '%s\n' "$*"; }
warn() { printf '%s: %s\n' "${SCRIPT:-adaptive}" "$*" >&2; }
die() { printf '%s: %s\n' "${SCRIPT:-adaptive}" "$*" >&2; exit 2; }

note() {
    [ -n "${OUT:-}" ] || { say "$*"; return 0; }
    printf '%s %s\n' "$(date -u +%H:%M:%SZ)" "$*" | tee -a "$OUT/run.log" >&2
}

num() {
    [ -n "${1:-}" ] || { printf '0'; return 0; }
    printf '%s' "$1"
}

csv_safe() { printf '%s' "${1:-}" | tr -d ',\n"'; }

csv_nf() { printf '%s' "$1" | awk -F, '{ print NF }'; }


# ---------------------------------------------------------------------------
# remote access

remote_sh() {
    host=$1
    shift
    ssh -o BatchMode=yes -o ConnectTimeout="$SSH_TMO" "$host" "$@"
}

# Run a script from stdin; arguments are appended to `sh -s --`.
remote_sn() {
    host=$1
    shift
    ssh -o BatchMode=yes -o ConnectTimeout="$SSH_TMO" "$host" "sh -s -- $*"
}

host_peer() {
    case $1 in
    "$NEAR") printf '%s' "$FAR_IP" ;;
    *) printf '%s' "$NEAR_IP" ;;
    esac
}

host_peer_port() {
    case $1 in
    "$NEAR") printf '%s' "$FAR_PORT" ;;
    *) printf '%s' "$NEAR_PORT" ;;
    esac
}

host_is_far() { [ "$1" = "$FAR" ]; }

# ---------------------------------------------------------------------------
# argument parsing: common flags. The caller's loop does:
#   if common_arg "$1" "${2:-}"; then shift "$SHIFT_BY"; continue; fi

common_arg() {
    SHIFT_BY=0
    case $1 in
    --near) NEAR=$2; SHIFT_BY=2 ;;
    --far) FAR=$2; SHIFT_BY=2 ;;
    --near-ip) NEAR_IP=$2; SHIFT_BY=2 ;;
    --far-ip) FAR_IP=$2; SHIFT_BY=2 ;;
    --near-module) NEAR_MODULE=$2; SHIFT_BY=2 ;;
    --dev) NEAR_DEV=$2; SHIFT_BY=2 ;;
    --near-port) NEAR_PORT=$2; SHIFT_BY=2 ;;
    --far-port) FAR_PORT=$2; SHIFT_BY=2 ;;
    --source) SOURCE=$2; SHIFT_BY=2 ;;
    --source-mib) SOURCE_MIB=$2; SHIFT_BY=2 ;;
    --url-path) URL_PATH=$2; SHIFT_BY=2 ;;
    --carriers) CARRIER_LIST=$2; SHIFT_BY=2 ;;
    --loss) LOSS_LIST=$2; SHIFT_BY=2 ;;
    --mode) MODE_LIST=$2; SHIFT_BY=2 ;;
    --repeats) REPEATS=$2; SHIFT_BY=2 ;;
    --duration) DURATION=$2; SHIFT_BY=2 ;;
    --warmup) WARMUP=$2; SHIFT_BY=2 ;;
    --join-timeout) JOIN_TIMEOUT=$2; SHIFT_BY=2 ;;
    --ssh-timeout) SSH_TMO=$2; SHIFT_BY=2 ;;
    --safety) SAFETY_SECS=$2; SHIFT_BY=2 ;;
    --tag) TAG=$2; SHIFT_BY=2 ;;
    --out) OUT=$2; SHIFT_BY=2 ;;
    --kill-at) KILL_AT=$2; SHIFT_BY=2 ;;
    --kill-carrier) KILL_CARRIER=$2; SHIFT_BY=2 ;;
    --cleanup) CLEANUP=1; SHIFT_BY=1 ;;
    --dry-run) DRY=1; SHIFT_BY=1 ;;
    -h | --help) usage_common; exit 0 ;;
    *) return 1 ;;
    esac
    return 0
}

usage_common() {
    cat <<'EOF'
common options:
  --near HOST --far HOST --near-ip ADDR --far-ip ADDR
  --near-module KO      insmod once when /proc/net/tbtun is absent
  --dev eth0            near carrier device (dev= token)
  --near-port 9001 --far-port 9000
  --source PATH         far source file (default /root/accept-1g/accept-1g.bin)
  --source-mib 1024     size hint in the far-serve.sh error message
  --url-path /accept-1g.bin
  --carriers 32,64,256  --loss 0,1,3,5,10  --mode fixed,adaptive
  --repeats 1 --duration 60 --warmup 10 --join-timeout 180
  --kill-at SEC --kill-carrier N   carrier down N seconds into the steady window
  --tag NAME --out DIR --dry-run --cleanup
EOF
}

# ---------------------------------------------------------------------------
# preflight

preflight_hosts() {
    say "== preflight"
    remote_sh "$NEAR" true >/dev/null 2>&1 || die "near unreachable: $NEAR"
    remote_sh "$FAR" true >/dev/null 2>&1 || die "far unreachable: $FAR"
    for h in "$NEAR" "$FAR"; do
        remote_sh "$h" 'test -d /proc/net/tbtun' >/dev/null 2>&1 ||
            die "$h: /proc/net/tbtun missing; load the 0.3.0 module first"
    done
    miss=$(remote_sh "$NEAR" 'for t in curl sha256sum awk tee wc mkfifo stat setsid; do command -v $t >/dev/null || printf "%s " $t; done' 2>/dev/null || true)
    [ -z "$miss" ] || warn "near is missing: $miss (setsid is only needed for the netem watchdog)"
    miss=$(remote_sh "$FAR" 'for t in curl sha256sum head stat awk; do command -v $t >/dev/null || printf "%s " $t; done' 2>/dev/null || true)
    [ -z "$miss" ] || die "far is missing: $miss"

    src_size=$(remote_sh "$FAR" "stat -c %s '$SOURCE' 2>/dev/null || echo 0")
    [ "${src_size:-0}" -gt 0 ] ||
        die "far source missing: $SOURCE (run far-serve.sh --start with SIZE_MIB=$SOURCE_MIB)"
    SOURCE_BYTES=$src_size
    cl=$(remote_sh "$FAR" "curl -fsSI -m 5 'http://127.0.0.1:$TARGET_PORT$URL_PATH' 2>/dev/null | awk 'tolower(\$1)==\"content-length:\" {print \$2}' | tr -d '\r'" || true)
    [ -n "$cl" ] ||
        die "far raw server not answering HEAD on 127.0.0.1:$TARGET_PORT$URL_PATH (far-serve.sh --start)"
    [ "$cl" = "$SOURCE_BYTES" ] ||
        die "far HEAD Content-Length=$cl but $SOURCE is $SOURCE_BYTES bytes"
    say "   source $SOURCE ($SOURCE_BYTES bytes)"
    say "   near $NEAR ($NEAR_IP) far $FAR ($FAR_IP)"
}

preflight_ppm() {
    case $INJECT_ENDS in
    near | far | both) ;;
    *) die "--inject-end must be near|far|both" ;;
    esac
    for hh in $(inject_hosts); do
        af=$(action_file "$hh") || die "$hh: no writable action file (/proc/net/tbtun/carriers or peers)"
        dl=$(remote_sh "$hh" 'cat /sys/module/tbtun/parameters/debug_loss 2>/dev/null || echo missing')
        case $dl in
        1 | Y | y) ;;
        *) die "$hh: debug_loss=$dl; drop_ppm needs insmod tbtun debug_loss=1" ;;
        esac
        say "   drop_ppm target $hh action=$af"
    done
}

inject_hosts() {
    case $INJECT_ENDS in
    near) printf '%s' "$NEAR" ;;
    far) printf '%s' "$FAR" ;;
    both) printf '%s %s' "$NEAR" "$FAR" ;;
    *) die "--inject-end must be near|far|both" ;;
    esac
}

max_carriers() {
    printf '%s' "$CARRIER_LIST" | awk -F, '{ m = 0; for (i = 1; i <= NF; i++) if ($i + 0 > m) m = $i + 0; if (m < 1) m = 256; print m }'
}

preflight_netem() {
    case $NETEM_WHERE in
    near | far | both) ;;
    *) die "--netem-where must be near|far|both" ;;
    esac
    for h in $(netem_hosts); do
        root=$(remote_sh "$h" 'id -u' 2>/dev/null || echo '?')
        [ "$root" = 0 ] || die "$h: netem needs root (id -u=$root)"
        remote_sh "$h" 'command -v tc >/dev/null' || die "$h: tc missing (iproute2)"
        dev=$(netem_dev "$h")
        [ -n "$dev" ] || die "$h: cannot detect the carrier egress device; pass --dev"
        remote_sh "$h" "tc qdisc show dev '$dev'" >/dev/null 2>&1 ||
            die "$h: tc cannot query dev $dev"
        say "   netem target $h dev=$dev"
    done
    [ "$NETEM_SIMPLE" = 1 ] &&
        warn "--simple puts loss on root and can drop the ssh session; the watchdog still cleans up"
}

action_file() {
    af_h=$1
    af=$ACTION_FILE
    if [ "$af" = auto ]; then
        af=$(remote_sh "$af_h" 'for f in /proc/net/tbtun/carriers /proc/net/tbtun/peers; do [ -w "$f" ] && { printf "%s" "$f"; exit 0; }; done; exit 1' 2>/dev/null || true)
        [ -n "$af" ] || return 1
    fi
    printf '%s' "$af"
}

# Add the far bypass rule only when missing; record that this harness added it
# so --cleanup never deletes mihomo's own rule. Same pref and lookup as
# short.sh and run-1g.sh.
prepare_near() {
    remote_sn "$NEAR" "${NEAR_MODULE:--}" "$FAR_IP" "$BYPASS_PREF" "$BYPASS_MARK" "$NEAR_DEV" <<'EOS' || die "near preparation failed"
KO=$1; FAR=$2; PREF=$3; MARK=$4; DEV=$5
[ "$KO" = - ] && KO=
if [ -n "$KO" ] && [ ! -d /proc/net/tbtun ]; then
    insmod "$KO" || exit 3
fi
[ -d /proc/net/tbtun ] || { echo "tbt-adaptive: tbtun not loaded on near" >&2; exit 3; }
# `ip rule show` prints "100:\tfrom all to <ip>/32 lookup main"; match the
# pref and the target, not a sentence that never appears.
if ! ip rule show 2>/dev/null | awk -v p="$PREF:" -v ip="$FAR" '
    $1 == p { for (i = 2; i < NF; i++) if ($i == "to" && ($(i + 1) == ip || $(i + 1) == ip "/32")) f = 1 }
    END { exit(f ? 0 : 1) }'; then
    if ip rule add to "$FAR/32" lookup main pref "$PREF" 2>/dev/null; then
        printf 'pref=%s to=%s/32\n' "$PREF" "$FAR" > "$MARK" 2>/dev/null || true
    fi
fi
if [ -n "$DEV" ]; then
    route=$(ip route get "$FAR" 2>/dev/null | head -1)
    case $route in
    *" dev $DEV "*) ;;
    *) echo "tbt-adaptive: near route to $FAR is not dev $DEV: $route" >&2; exit 3 ;;
    esac
fi
EOS
}

cleanup_bypass() {
    remote_sh "$NEAR" "MARK='$BYPASS_MARK' FAR='$FAR_IP' PREF='$BYPASS_PREF' sh -s" <<'EOS' || true
set -u
[ -f "$MARK" ] || { echo "near bypass: nothing added by this harness"; exit 0; }
grep -qF "to=$FAR/32" "$MARK" 2>/dev/null || { echo "near bypass: marker is for another target"; exit 0; }
if ip rule show 2>/dev/null | awk -v p="$PREF:" -v ip="$FAR" '
    $1 == p { for (i = 2; i < NF; i++) if ($i == "to" && ($(i + 1) == ip || $(i + 1) == ip "/32")) f = 1 }
    END { exit(f ? 0 : 1) }'; then
    ip rule del pref "$PREF" to "$FAR/32" lookup main 2>/dev/null ||
        ip rule del pref "$PREF" to "$FAR/32" 2>/dev/null || true
    echo "near bypass: removed pref $PREF to $FAR/32 (added by this harness)"
else
    echo "near bypass: marker present, rule already gone"
fi
rm -f "$MARK"
EOS
}

# ---------------------------------------------------------------------------
# tunnel configuration (tokens identical to benches/short.sh, plus adaptive=)

configure_pair() {
    cfg_carriers=$1
    cfg_adaptive=$2
    remote_sn "$FAR" "$cfg_carriers" "$cfg_adaptive" "$FAR_PORT" "$TARGET" "$N" "$M" \
        "$RX_REORDER" "$TX_WINDOW" "$FAR_BLOCK" "$NONCE" <<'EOS' || die "far config write failed"
C=$1; A=$2; PORT=$3; TGT=$4; N=$5; M=$6; RX=$7; TW=$8; BLOCK=$9; NONCE=${10}
printf 'tunnel 1 role=far listen=0.0.0.0:%s target=%s n=%s m=%s fec=on mode=grouped w=8 carriers=%s adaptive=%s crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=%s tx_window_bytes=%s tx_retain_bytes=0 carrier_stall_ms=0 portblock=%s nonce=%s\n' \
    "$PORT" "$TGT" "$N" "$M" "$C" "$A" "$RX" "$TW" "$BLOCK" "$NONCE" > /proc/net/tbtun/config
EOS
    remote_sn "$NEAR" "$cfg_carriers" "$cfg_adaptive" "$NEAR_PORT" "$FAR_IP" "$FAR_PORT" \
        "$N" "$M" "$RX_REORDER" "$TX_WINDOW" "$NEAR_BLOCK" "$NONCE" "$NEAR_DEV" <<'EOS' || die "near config write failed"
C=$1; A=$2; PORT=$3; PEER=$4; PEERPORT=$5; N=$6; M=$7; RX=$8; TW=$9; BLOCK=${10}; NONCE=${11}; DEV=${12}
DEV_TOKEN=
[ -n "$DEV" ] && DEV_TOKEN=" dev=$DEV"
printf 'tunnel 1 role=near listen=127.0.0.1:%s peer=%s:%s n=%s m=%s fec=on mode=grouped w=8 carriers=%s adaptive=%s crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=%s tx_window_bytes=%s tx_retain_bytes=0 carrier_stall_ms=0 portblock=%s nonce=%s%s\n' \
    "$PORT" "$PEER" "$PEERPORT" "$N" "$M" "$C" "$A" "$RX" "$TW" "$BLOCK" "$NONCE" "$DEV_TOKEN" > /proc/net/tbtun/config
EOS
    # The read-back proves the build understands adaptive=; a build without
    # WP9 drops the key (or rejects the write) and every later number would
    # be a fixed-shape number labelled adaptive.
    for h in "$FAR" "$NEAR"; do
        cfg=$(remote_sh "$h" 'cat /proc/net/tbtun/config' 2>/dev/null || true)
        case " $cfg " in
        *" adaptive=$cfg_adaptive "*) ;;
        *) die "$h: config read-back lacks adaptive=$cfg_adaptive; build does not implement WP9" ;;
        esac
        case " $cfg " in
        *" carriers=$cfg_carriers "*) ;;
        *) die "$h: config read-back lacks carriers=$cfg_carriers" ;;
        esac
    done
}

wait_join() {
    jn=$1
    ji=0
    jnu=0
    jfu=0
    while [ "$ji" -lt "$JOIN_TIMEOUT" ]; do
        jnu=$(remote_sh "$NEAR" "awk '\$1==\"tunnel\" && \$2==1 && \$3==\"c3\" { for (i=1;i<=NF;i++) if (\$i ~ /^up=/) { sub(/^up=/,\"\",\$i); print \$i; exit } }' /proc/net/tbtun/stats 2>/dev/null" 2>/dev/null || true)
        jfu=$(remote_sh "$FAR" "awk '\$1==\"tunnel\" && \$2==1 && \$3==\"c3\" { for (i=1;i<=NF;i++) if (\$i ~ /^up=/) { sub(/^up=/,\"\",\$i); print \$i; exit } }' /proc/net/tbtun/stats 2>/dev/null" 2>/dev/null || true)
        if [ "${jnu:-0}" = "$jn" ] && [ "${jfu:-0}" = "$jn" ]; then
            return 0
        fi
        ji=$((ji + 1))
        sleep 1
    done
    warn "join timeout near=${jnu:-0} far=${jfu:-0} want=$jn"
    return 1
}

# ---------------------------------------------------------------------------
# snapshots and counters

snapshot() {
    remote_sh "$1" '
        echo "@@stats"; cat /proc/net/tbtun/stats 2>/dev/null || true
        echo "@@peers"; cat /proc/net/tbtun/peers 2>/dev/null || true
        echo "@@config"; cat /proc/net/tbtun/config 2>/dev/null || true
        echo "@@cpu"; head -1 /proc/stat 2>/dev/null || true
        echo "@@clk"; getconf CLK_TCK 2>/dev/null || echo 100
        echo "@@uptime"; cut -d" " -f1 /proc/uptime 2>/dev/null || true
    ' >"$2"
}

field() {
    printf '%s\n' "${1:-}" | tr ' ' '\n' | awk -F= -v k="$2" '$1 == k { print $2; exit }'
}

stat_line() {
    awk -v p="$2" 'index($0, p) == 1 { print; exit }' "$1" 2>/dev/null || true
}

stat_field() {
    sf_line=$(stat_line "$1" "$2")
    field "$sf_line" "$3"
}

peers_sum() {
    awk -v c="$3" '
        /^@@peers/ { p = 1; next }
        /^@@/ { p = 0 }
        p && NF >= 14 { s += $c }
        END { printf "%.0f", s }' "$1" 2>/dev/null || printf '0'
}

# Wire bytes from the sender. `reconcile live_bytes + retired_bytes` survives
# a carrier replacement (S12); peers only lists live slots.
wire_total() {
    wt_live=$(stat_field "$1" 'tunnel 1 reconcile ' live_bytes)
    wt_ret=$(stat_field "$1" 'tunnel 1 reconcile ' retired_bytes)
    if [ -n "$wt_live" ] && [ -n "$wt_ret" ]; then
        printf '%s' "$((wt_live + wt_ret))"
    else
        peers_sum "$1" 14
    fi
}

rx_field() {
    awk -v f="$2=" '
        / rx_ready_calls=/ {
            for (i = 1; i <= NF; i++)
                if (index($i, f) == 1) { print substr($i, length(f) + 1); exit }
        }' "$1" 2>/dev/null || true
}

submit_field() {
    awk -v f="$2=" '
        / capture_runs=/ {
            for (i = 1; i <= NF; i++)
                if (index($i, f) == 1) { print substr($i, length(f) + 1); exit }
        }' "$1" 2>/dev/null || true
}

c13_index() {
    case $1 in
    epoch) printf '1' ;;
    n) printf '2' ;;
    m) printf '3' ;;
    groups) printf '4' ;;
    class) printf '5' ;;
    p_ppm) printf '6' ;;
    p_win_ppm) printf '7' ;;
    r_fail_ppm) printf '8' ;;
    sent) printf '9' ;;
    miss) printf '10' ;;
    switches) printf '11' ;;
    aborts) printf '12' ;;
    rollbacks) printf '13' ;;
    pauses) printf '14' ;;
    pause_ns) printf '15' ;;
    sample_unknown) printf '16' ;;
    abandoned) printf '17' ;;
    state) printf '18' ;;
    reason) printf '19' ;;
    *) printf '0' ;;
    esac
}

c13_field() {
    c13_line=$(awk '/ c13 /{ print; exit }' "$1" 2>/dev/null || true)
    [ -n "$c13_line" ] || { printf ''; return 0; }
    for c13_tok in $c13_line; do
        case $c13_tok in
        "$2="*) printf '%s' "${c13_tok#"$2="}"; return 0 ;;
        esac
    done
    c13_i=$(c13_index "$2")
    [ "$c13_i" -gt 0 ] || { printf ''; return 0; }
    printf '%s\n' "$c13_line" | awk -v i="$c13_i" '{
        for (j = 1; j <= NF; j++)
            if ($j == "c13") {
                k = j + 1
                if ($(k) == "adaptive") k++
                print $(k + i - 1)
                exit
            }
    }'
}

cpu_pct() {
    cb=$1
    ca=$2
    ce=$3
    lb=$(printf '%s\n' "$cb" | awk '/^cpu /{print;exit}')
    la=$(printf '%s\n' "$ca" | awk '/^cpu /{print;exit}')
    hz=$(printf '%s\n' "$ca" | awk '/^@@clk/{getline; print; exit}')
    [ -n "$hz" ] || hz=100
    awk -v b="$lb" -v a="$la" -v e="$ce" -v hz="$hz" 'BEGIN {
        nb = split(b, x, " "); na = split(a, y, " ")
        if (nb < 8 || na < 8) { print "0.0"; exit }
        busy_b = x[2] + x[3] + x[4] + x[7] + x[8]
        busy_a = y[2] + y[3] + y[4] + y[7] + y[8]
        if (e <= 0) { print "0.0"; exit }
        printf "%.1f", (busy_a - busy_b) / (e * hz) * 100
    }'
}

# ---------------------------------------------------------------------------
# drop_ppm injection: one write per carrier. tbt_peers_write() parses a single
# line per write(), so a batch would be rejected; the loop is the API.

inject_ppm_apply() {
    ipa_ppm=$1
    ipa_n=$2
    INJECT_ACTIVE=ppm
    for ipa_h in $(inject_hosts); do
        ipa_af=$(action_file "$ipa_h") || return 1
        remote_sn "$ipa_h" "$ipa_af" "$ipa_n" "$ipa_ppm" <<'EOS' || return 1
F=$1; N=$2; PPM=$3
i=0
while [ "$i" -lt "$N" ]; do
    printf 'carrier %d drop_ppm %d\n' "$i" "$PPM" > "$F" || exit 4
    i=$((i + 1))
done
EOS
    done
    INJECT_ACTIVE=ppm
    return 0
}

# Clear on both ends: a previous run may have targeted either end.
inject_ppm_clear() {
    ipc_n=${1:-$(max_carriers)}
    ipc_saved=$INJECT_ENDS
    INJECT_ENDS=both
    inject_ppm_apply 0 "$ipc_n" >/dev/null 2>&1 || true
    INJECT_ENDS=$ipc_saved
    INJECT_ACTIVE=
}

# ---------------------------------------------------------------------------
# netem injection: egress loss isolated to the carrier 5-tuple.
# near egress matches dport 9000, far egress matches sport 9000, so ssh is
# never in the lossy band. --simple falls back to root netem (ssh included).

netem_hosts() {
    case $NETEM_WHERE in
    near) printf '%s' "$NEAR" ;;
    far) printf '%s' "$FAR" ;;
    both) printf '%s %s' "$NEAR" "$FAR" ;;
    *) die "--netem-where must be near|far|both" ;;
    esac
}

netem_dev() {
    h=$1
    case $h in
    "$NEAR") [ -n "$NETEM_DEV_NEAR" ] && { printf '%s' "$NETEM_DEV_NEAR"; return 0; } ;;
    "$FAR") [ -n "$NETEM_DEV_FAR" ] && { printf '%s' "$NETEM_DEV_FAR"; return 0; } ;;
    esac
    dev=$(remote_sn "$h" "$(host_peer "$h")" <<'EOS' 2>/dev/null || true
P=$1
ip route get "$P" 2>/dev/null | awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }'
EOS
)
    case $h in
    "$NEAR") NETEM_DEV_NEAR=$dev ;;
    "$FAR") NETEM_DEV_FAR=$dev ;;
    esac
    printf '%s' "$dev"
}

netem_match() {
    h=$1
    if host_is_far "$h"; then printf 'sport'; else printf 'dport'; fi
}

netem_apply_one() {
    nh=$1
    npp=$2
    ndev=$(netem_dev "$nh")
    nmatch=$(netem_match "$nh")
    [ -n "$ndev" ] || return 1
    ncorr=
    [ "$NETEM_BURST" = 1 ] && ncorr=25%
    if [ "$NETEM_SIMPLE" = 1 ]; then
        nmatch=simple
    fi
    remote_sn "$nh" "$ndev" "$npp" "$ncorr" "$nmatch" "$FAR_PORT" "$NETEM_MARK.$ndev" "$SAFETY_SECS" <<'EOS' || return 1
DEV=$1; PCT=$2; CORR=$3; MATCH=$4; PORT=$5; MARK=$6; SAFETY=$7
TOKEN=$$
if [ "$MATCH" = simple ]; then
    tc qdisc add dev "$DEV" root netem loss "$PCT"% $CORR || exit 4
else
    tc qdisc add dev "$DEV" root handle 1: prio bands 4 || exit 4
    tc qdisc add dev "$DEV" parent 1:4 handle 40: netem loss "$PCT"% $CORR ||
        { tc qdisc del dev "$DEV" root 2>/dev/null; exit 4; }
    tc filter add dev "$DEV" parent 1: protocol ip prio 1 u32 match ip "$MATCH" "$PORT" 0xffff flowid 1:4 ||
        { tc qdisc del dev "$DEV" root 2>/dev/null; exit 4; }
fi
printf '%s\n' "$TOKEN" > "$MARK" || { tc qdisc del dev "$DEV" root 2>/dev/null; exit 4; }
# Watchdog: if this script dies with ssh gone, remove only the qdisc whose
# marker still carries this token. A later run overwrites the marker with a
# new token, so a stale watchdog can never delete a newer qdisc.
if command -v setsid >/dev/null 2>&1; then
    setsid sh -c 'sleep "$1"; [ "$(cat "$2" 2>/dev/null)" = "$3" ] && { tc qdisc del dev "$4" root 2>/dev/null; rm -f "$2"; }' sh "$SAFETY" "$MARK" "$TOKEN" "$DEV" </dev/null >/dev/null 2>&1 &
else
    nohup sh -c 'sleep "$1"; [ "$(cat "$2" 2>/dev/null)" = "$3" ] && { tc qdisc del dev "$4" root 2>/dev/null; rm -f "$2"; }' sh "$SAFETY" "$MARK" "$TOKEN" "$DEV" </dev/null >/dev/null 2>&1 &
fi
EOS
}

netem_apply() {
    pct=$1
    INJECT_ACTIVE=netem
    for h in $(netem_hosts); do
        netem_apply_one "$h" "$pct" || return 1
    done
    return 0
}

netem_clear_one() {
    nh=$1
    ndev=$(netem_dev "$nh")
    [ -n "$ndev" ] || return 0
    remote_sh "$nh" "MARK='$NETEM_MARK.$ndev' DEV='$ndev' sh -s" <<'EOS' >/dev/null 2>&1 || true
[ -f "$MARK" ] || exit 0
tc qdisc del dev "$DEV" root 2>/dev/null || true
rm -f "$MARK"
EOS
}

netem_clear() {
    for h in "$NEAR" "$FAR"; do
        netem_clear_one "$h"
    done
    INJECT_ACTIVE=
}

inject_apply() {
    ia_inject=$1
    ia_loss=$2
    case $ia_inject in
    ppm) inject_ppm_apply "$((ia_loss * 10000))" "$R_carriers" ;;
    netem) netem_apply "$ia_loss" ;;
    *) die "unknown injector $ia_inject" ;;
    esac
}

inject_clear() {
    ic_inject=${1:-}
    ic_n=${2:-$(max_carriers)}
    case $ic_inject in
    ppm) inject_ppm_clear "$ic_n" ;;
    netem) netem_clear ;;
    esac
}

# ---------------------------------------------------------------------------
# transfer: stream the far body through the tunnel on near, hash it on the fly
# (no multi-GiB file on near), and count the bytes. The process ends on its
# own via --max-time; transfer_stop only waits.

transfer_start() {
    remote_sn "$NEAR" "$REMOTE_DIR" "http://127.0.0.1:$NEAR_PORT$URL_PATH" "$((WARMUP + DURATION + 2))" <<'EOS' || return 1
D=$1; URL=$2; TOTAL=$3
rm -rf "$D"
mkdir -p "$D" || exit 4
mkfifo "$D/fifo" || exit 4
# `sha256sum < fifo` prints "HEX  -"; store only the digest so the CSV and
# check-bytes compare one field.
sha256sum < "$D/fifo" 2>/dev/null | awk '{print $1}' > "$D/sha" &
# curl -f: an HTTP error is a failed run, not an error body hashed as data.
( curl -fsS --max-time "$TOTAL" "$URL" 2>"$D/curl.err"; echo $? > "$D/curl.rc" ) |
    tee "$D/fifo" | wc -c > "$D/size" &
printf '%s\n' "$!" > "$D/pid"
EOS
    TRANSFER_STARTED=1
    TRANSFER_FAILS=0
}

# curl writes curl.rc when it exits; the hash/count may still be flushing.
# Polling the file is exact, while kill -0 can stay true for a zombie.
# A failed ssh is not proof the transfer ended: only a definite answer from
# near does that. One lost ssh packet during a lossy warm-up must not set
# early=1, so a run ends only after TRANSFER_FAIL_MAX consecutive failures.
transfer_alive() {
    ta_state=$(remote_sh "$NEAR" \
        "if [ -f '$REMOTE_DIR/curl.rc' ]; then echo ended; else echo alive; fi" \
        2>/dev/null || true)
    case $ta_state in
    alive)
        TRANSFER_FAILS=0
        return 0
        ;;
    ended)
        TRANSFER_FAILS=0
        return 1
        ;;
    esac
    TRANSFER_FAILS=$((TRANSFER_FAILS + 1))
    warn "near liveness probe failed ($TRANSFER_FAILS/$TRANSFER_FAIL_MAX)"
    if [ "$TRANSFER_FAILS" -lt "$TRANSFER_FAIL_MAX" ]; then
        return 0
    fi
    TRANSFER_FAILS=0
    return 1
}

# Wait for curl and both pipeline tails to finish. curl.rc appearing is not
# enough: wc -c and sha256sum write their files just after it. sha is written
# only at EOF, so a non-empty sha means the hash is complete.
transfer_stop() {
    [ "${TRANSFER_STARTED:-0}" = 1 ] || return 0
    ts_i=0
    while [ "$ts_i" -lt 15 ]; do
        ts_ok=$(remote_sh "$NEAR" "[ -f '$REMOTE_DIR/curl.rc' ] && [ -s '$REMOTE_DIR/sha' ] && [ -f '$REMOTE_DIR/size' ] && echo yes" 2>/dev/null || true)
        [ "$ts_ok" = yes ] && { TRANSFER_STARTED=0; return 0; }
        sleep 1
        ts_i=$((ts_i + 1))
    done
    remote_sh "$NEAR" "kill -TERM \$(cat '$REMOTE_DIR/pid') 2>/dev/null || true" >/dev/null 2>&1 || true
    sleep 2
    TRANSFER_STARTED=0
    return 0
}

transfer_collect() {
    tc_dir=$1
    remote_sh "$NEAR" "cat '$REMOTE_DIR/size' 2>/dev/null" >"$tc_dir/size" 2>/dev/null || true
    remote_sh "$NEAR" "cat '$REMOTE_DIR/sha' 2>/dev/null" >"$tc_dir/sha" 2>/dev/null || true
    remote_sh "$NEAR" "cat '$REMOTE_DIR/curl.rc' 2>/dev/null" >"$tc_dir/curl.rc" 2>/dev/null || true
    remote_sh "$NEAR" "cat '$REMOTE_DIR/curl.err' 2>/dev/null" >"$tc_dir/curl.err" 2>/dev/null || true
}

transfer_cleanup() {
    remote_sh "$NEAR" "rm -rf '$REMOTE_DIR'" >/dev/null 2>&1 || true
    TRANSFER_STARTED=0
}

kill_carrier_once() {
    [ -n "$KILL_AT" ] || return 0
    [ "$KILL_CARRIER" -ge 0 ] || return 0
    kc_host=$(printf '%s' "$INJECT_ENDS" | awk '{ print $1 }')
    case $kc_host in
    both) kc_host=$FAR ;;
    near) kc_host=$NEAR ;;
    *) kc_host=$FAR ;;
    esac
    kc_af=$(action_file "$kc_host")
    remote_sh "$kc_host" "printf 'carrier $KILL_CARRIER down\n' > '$kc_af'" >/dev/null 2>&1 || true
    note "kill carrier $KILL_CARRIER on $kc_host"
}

# ---------------------------------------------------------------------------
# one run

run_one() {
    R_inject=$1
    R_loss=$2
    R_carriers=$3
    R_mode=$4
    R_rep=$5
    R_order=$6
    R_ppm=$((R_loss * 10000))
    R_adaptive=off
    [ "$R_mode" = adaptive ] && R_adaptive=on
    R_name=$(printf '%s.p%02d.c%03d.%s.r%d' "$R_inject" "$R_loss" "$R_carriers" "$R_mode" "$R_rep")
    R_dir=$OUT/runs/$R_name
    mkdir -p "$R_dir"
    note "run $R_name"

    R_rc=0
    R_early=0
    R_window=0
    R_killed=0
    R_note=
    if [ "$R_inject" = netem ] && [ "$NETEM_BURST" = 1 ]; then R_note=burst; fi
    if [ -n "$KILL_AT" ]; then
        R_note="${R_note:+$R_note+}kill=$KILL_CARRIER@$KILL_AT"
    fi

    configure_pair "$R_carriers" "$R_adaptive"
    if ! wait_join "$R_carriers"; then
        R_rc=1
    fi
    if [ "$R_rc" -eq 0 ]; then
        # Optional entry-script hook: start a c13 sampler after join and
        # before the first transfer. Empty for the 0.3.0 scripts.
        if [ -n "${SC_SAMPLE_START:-}" ]; then
            "$SC_SAMPLE_START"
        fi
        snapshot "$NEAR" "$R_dir/near-pre.txt"
        snapshot "$FAR" "$R_dir/far-pre.txt"
        if ! inject_apply "$R_inject" "$R_loss"; then
            R_rc=1
        fi
    fi
    if [ "$R_rc" -eq 0 ]; then
        if ! transfer_start; then
            R_rc=1
        fi
    fi
    if [ "$R_rc" -eq 0 ]; then
        t0=$(date +%s)
        warm_early=0
        R_i=0
        while [ "$R_i" -lt "$WARMUP" ]; do
            sleep 1
            R_i=$((R_i + 1))
            if ! transfer_alive; then
                R_early=1
                warm_early=1
                break
            fi
        done
        tA=$(date +%s)
        if [ "$warm_early" -eq 0 ]; then
            snapshot "$NEAR" "$R_dir/near-a.txt"
            snapshot "$FAR" "$R_dir/far-a.txt"
        else
            cp "$R_dir/near-pre.txt" "$R_dir/near-a.txt"
            cp "$R_dir/far-pre.txt" "$R_dir/far-a.txt"
        fi
        if [ "$warm_early" -eq 0 ]; then
            R_i=0
            while [ "$R_i" -lt "$DURATION" ]; do
                sleep 1
                R_i=$((R_i + 1))
                if [ -n "$KILL_AT" ] && [ "$R_i" -eq "$KILL_AT" ] && [ "$R_killed" = 0 ]; then
                    kill_carrier_once
                    R_killed=1
                fi
                if ! transfer_alive; then
                    R_early=1
                    break
                fi
            done
        fi
        tB=$(date +%s)
        snapshot "$NEAR" "$R_dir/near-b.txt"
        snapshot "$FAR" "$R_dir/far-b.txt"
        if [ "$warm_early" -eq 1 ]; then
            # near-a is a copy of near-pre here, so the app delta spans the
            # whole transfer; the window is the real elapsed time.
            R_note="${R_note:+$R_note+}warm_early"
            R_window=$((tB - t0))
        else
            R_window=$((tB - tA))
        fi
        [ "$R_window" -gt 0 ] || R_window=1
        transfer_stop
        transfer_collect "$R_dir"
    fi
    inject_clear "$R_inject" "$R_carriers" || true
    transfer_cleanup

    collect_row
    emit_row
    if [ "$STOP" = 1 ]; then
        return 1
    fi
    return 0
}

collect_row() {
    R_app=0
    R_wire=0
    R_retx=0
    R_p_ppm=0
    R_p_win=0
    R_p_delta=0
    R_sent=0
    R_miss=0
    R_switches=0
    R_aborts=0
    R_rollbacks=0
    R_state=-
    R_reason=-
    R_r_fail=0
    R_pauses=0
    R_pause_ns=0
    R_sample_unknown=0
    R_abandoned=0
    R_hol=0
    R_stall=0
    R_holes=0
    R_reset=0
    R_lost=0
    R_has_c13=0
    R_sha=
    R_bytes=0
    R_bytes_ok=0
    R_curl_rc=
    R_curl_bytes=0
    R_app_mbps=0
    R_wire_per_app=0
    R_retx_ppm=0
    R_near_cpu=0
    R_far_cpu=0

    if [ -f "$R_dir/near-b.txt" ]; then
        a_app=$(rx_field "$R_dir/near-a.txt" rx_delivered)
        b_app=$(rx_field "$R_dir/near-b.txt" rx_delivered)
        R_app=$(( $(num "$b_app") - $(num "$a_app") ))
        [ "$R_app" -ge 0 ] || R_app=0
        if [ "$R_app" -eq 0 ]; then
            a_app=$(submit_field "$R_dir/far-a.txt" submit_bytes)
            b_app=$(submit_field "$R_dir/far-b.txt" submit_bytes)
            R_app=$(( $(num "$b_app") - $(num "$a_app") ))
            [ "$R_app" -ge 0 ] || R_app=0
        fi

        a_w=$(wire_total "$R_dir/far-a.txt")
        b_w=$(wire_total "$R_dir/far-b.txt")
        R_wire=$((b_w - a_w))
        [ "$R_wire" -ge 0 ] || R_wire=0

        R_retx=$(( $(num "$(stat_field "$R_dir/far-b.txt" 'tunnel 1 c11 ' fec_nack_retx_bytes)") - $(num "$(stat_field "$R_dir/far-a.txt" 'tunnel 1 c11 ' fec_nack_retx_bytes)") ))
        R_retx=$(( R_retx + $(num "$(stat_field "$R_dir/far-b.txt" 'tunnel 1 c11 ' fec_replace_retx_bytes)") - $(num "$(stat_field "$R_dir/far-a.txt" 'tunnel 1 c11 ' fec_replace_retx_bytes)") ))
        [ "$R_retx" -ge 0 ] || R_retx=0

        R_holes=$(( $(num "$(stat_field "$R_dir/near-b.txt" 'tunnel 1 c6 ' holes_unrecoverable)") - $(num "$(stat_field "$R_dir/near-a.txt" 'tunnel 1 c6 ' holes_unrecoverable)") ))
        [ "$R_holes" -ge 0 ] || R_holes=0
        R_reset=$(( $(num "$(stat_field "$R_dir/near-b.txt" 'tunnel 1 c11 ' fec_reset_hole)") - $(num "$(stat_field "$R_dir/near-a.txt" 'tunnel 1 c11 ' fec_reset_hole)") ))
        [ "$R_reset" -ge 0 ] || R_reset=0
        R_lost=$(( $(num "$(stat_field "$R_dir/far-b.txt" 'tunnel 1 c3 ' lost)") - $(num "$(stat_field "$R_dir/far-a.txt" 'tunnel 1 c3 ' lost)") ))
        [ "$R_lost" -ge 0 ] || R_lost=0
        R_stall=$(( $(num "$(stat_field "$R_dir/near-b.txt" 'tunnel 1 c3 ' stall_declared)") - $(num "$(stat_field "$R_dir/near-a.txt" 'tunnel 1 c3 ' stall_declared)") ))
        [ "$R_stall" -ge 0 ] || R_stall=0
        R_hol=$(num "$(stat_field "$R_dir/near-b.txt" 'tunnel 1 c6 ' rx_max_stall_us)")

        if [ -n "$(c13_field "$R_dir/far-b.txt" epoch)" ]; then
            R_has_c13=1
            R_p_ppm=$(num "$(c13_field "$R_dir/far-b.txt" p_ppm)")
            R_p_win=$(num "$(c13_field "$R_dir/far-b.txt" p_win_ppm)")
            R_sent=$(num "$(c13_field "$R_dir/far-b.txt" sent)")
            R_miss=$(num "$(c13_field "$R_dir/far-b.txt" miss)")
            R_r_fail=$(num "$(c13_field "$R_dir/far-b.txt" r_fail_ppm)")
            R_switches=$(( $(num "$(c13_field "$R_dir/far-b.txt" switches)") - $(num "$(c13_field "$R_dir/far-a.txt" switches)") ))
            [ "$R_switches" -ge 0 ] || R_switches=0
            R_aborts=$(( $(num "$(c13_field "$R_dir/far-b.txt" aborts)") - $(num "$(c13_field "$R_dir/far-a.txt" aborts)") ))
            [ "$R_aborts" -ge 0 ] || R_aborts=0
            R_rollbacks=$(( $(num "$(c13_field "$R_dir/far-b.txt" rollbacks)") - $(num "$(c13_field "$R_dir/far-a.txt" rollbacks)") ))
            [ "$R_rollbacks" -ge 0 ] || R_rollbacks=0
            R_pauses=$(( $(num "$(c13_field "$R_dir/far-b.txt" pauses)") - $(num "$(c13_field "$R_dir/far-a.txt" pauses)") ))
            [ "$R_pauses" -ge 0 ] || R_pauses=0
            R_pause_ns=$(( $(num "$(c13_field "$R_dir/far-b.txt" pause_ns)") - $(num "$(c13_field "$R_dir/far-a.txt" pause_ns)") ))
            [ "$R_pause_ns" -ge 0 ] || R_pause_ns=0
            R_sample_unknown=$(( $(num "$(c13_field "$R_dir/far-b.txt" sample_unknown)") - $(num "$(c13_field "$R_dir/far-a.txt" sample_unknown)") ))
            [ "$R_sample_unknown" -ge 0 ] || R_sample_unknown=0
            R_abandoned=$(( $(num "$(c13_field "$R_dir/far-b.txt" abandoned)") - $(num "$(c13_field "$R_dir/far-a.txt" abandoned)") ))
            [ "$R_abandoned" -ge 0 ] || R_abandoned=0
            R_state=$(csv_safe "$(c13_field "$R_dir/far-b.txt" state)")
            R_reason=$(csv_safe "$(c13_field "$R_dir/far-b.txt" reason)")
            [ -n "$R_state" ] || R_state=-
            [ -n "$R_reason" ] || R_reason=-
        fi

        R_near_cpu=$(cpu_pct "$(cat "$R_dir/near-a.txt")" "$(cat "$R_dir/near-b.txt")" "$R_window")
        R_far_cpu=$(cpu_pct "$(cat "$R_dir/far-a.txt")" "$(cat "$R_dir/far-b.txt")" "$R_window")
    fi

    if [ "$R_has_c13" = 1 ]; then
        R_p_delta=$(awk -v p="$R_p_ppm" -v q="$R_ppm" 'BEGIN { printf "%.2f", (p - q) / 10000 }')
    fi
    R_app_mbps=$(awk -v b="$R_app" -v s="$R_window" 'BEGIN { printf "%.3f", (s > 0 ? b / s / 1e6 : 0) }')
    R_wire_per_app=$(awk -v w="$R_wire" -v a="$R_app" 'BEGIN { printf "%.4f", (a > 0 ? w / a : 0) }')
    R_retx_ppm=$(awk -v r="$R_retx" -v a="$R_app" 'BEGIN { printf "%d", (a > 0 ? r * 1000000 / a : 0) }')

    R_sha=
    R_bytes=0
    R_curl_rc=
    if [ -f "$R_dir/sha" ]; then R_sha=$(awk 'NR == 1 { print $1 }' "$R_dir/sha"); fi
    if [ -f "$R_dir/size" ]; then R_bytes=$(num "$(tr -d '\n ' <"$R_dir/size")"); fi
    if [ -f "$R_dir/curl.rc" ]; then R_curl_rc=$(tr -d '\n ' <"$R_dir/curl.rc"); fi
    R_curl_bytes=$R_bytes
    R_checked=0
    if [ -n "$R_sha" ] && [ "$R_bytes" -gt 0 ]; then
        R_checked=1
        far_sha=$(remote_sh "$FAR" "head -c $R_bytes '$SOURCE' 2>/dev/null | sha256sum | awk '{print \$1}'" 2>/dev/null || true)
        if [ "$R_bytes" -le "$SOURCE_BYTES" ]; then
            if [ -x "$CHECK_BYTES" ]; then
                if "$CHECK_BYTES" --sha "$R_sha" --expect "$far_sha" --bytes "$R_bytes" \
                    --label "$R_name" >"$R_dir/bytes.txt" 2>&1; then
                    R_bytes_ok=1
                fi
            elif [ "$R_sha" = "$far_sha" ]; then
                R_bytes_ok=1
            fi
        fi
        if [ "$R_curl_rc" = 0 ] && [ "$R_bytes" != "$SOURCE_BYTES" ]; then
            R_bytes_ok=0
        fi
    fi
    case ${R_curl_rc:-0} in
    0 | 28 | 143) ;;
    '') ;;
    *)
        R_rc=1
        R_bytes_ok=0
        STOP=1
        warn "$R_name: curl rc=$R_curl_rc (not 0/28/143)"
        ;;
    esac
    if [ "$R_checked" = 1 ] && [ "$R_bytes_ok" != 1 ]; then
        STOP=1
        warn "$R_name: byte check failed (near sha=${R_sha:0:16}.. far=${far_sha:0:16}.. bytes=$R_bytes)"
    fi
}

row_add() {
    if [ -z "${R_ROW:-}" ]; then
        R_ROW=$1
    else
        R_ROW=$R_ROW,$1
    fi
}

emit_row() {
    R_ROW=
    row_add "$(date -u +%Y%m%dT%H%M%SZ)"
    row_add "$SCRIPT"
    row_add "$R_inject"
    row_add "$R_loss"
    row_add "$R_ppm"
    row_add "$R_carriers"
    row_add "$R_mode"
    row_add "$R_adaptive"
    row_add "$R_rep"
    row_add "$R_order"
    row_add "$R_window"
    row_add "$WARMUP"
    row_add "$R_early"
    row_add "$R_app"
    row_add "$R_app_mbps"
    row_add "$R_curl_bytes"
    row_add "$R_wire"
    row_add "$R_wire_per_app"
    row_add "$R_retx"
    row_add "$R_retx_ppm"
    row_add "$R_p_ppm"
    row_add "$R_p_win"
    row_add "$R_p_delta"
    row_add "$R_sent"
    row_add "$R_miss"
    row_add "$R_switches"
    row_add "$R_aborts"
    row_add "$R_rollbacks"
    row_add "$R_state"
    row_add "$R_reason"
    row_add "$R_near_cpu"
    row_add "$R_far_cpu"
    row_add "$R_hol"
    row_add "$R_stall"
    row_add "$R_holes"
    row_add "$R_reset"
    row_add "$R_lost"
    row_add "$R_has_c13"
    row_add "${R_sha:--}"
    row_add "$R_bytes"
    row_add "$R_bytes_ok"
    row_add "${R_curl_rc:--}"
    row_add "$R_rc"
    row_add "$R_r_fail"
    row_add "$R_pauses"
    row_add "$R_pause_ns"
    row_add "$R_sample_unknown"
    row_add "$R_abandoned"
    row_add "${R_note:--}"

    nf=$(csv_nf "$R_ROW")
    [ "$nf" = "$CSV_NF" ] || die "internal: row has $nf fields, header has $CSV_NF: $R_ROW"
    printf '%s\n' "$R_ROW" >>"$OUT/runs.csv"
}

# ---------------------------------------------------------------------------
# grid

csv_init() {
    printf '%s\n' "$CSV_HEADER" >"$OUT/runs.csv"
}

run_grid() {
    grid_inject=$1
    [ "$DRY" = 1 ] && { dry_run_plan "$grid_inject"; return 0; }
    if [ -n "$KILL_AT" ]; then
        [ "$KILL_CARRIER" -ge 0 ] && [ "$KILL_CARRIER" -le 255 ] ||
            die "--kill-at needs --kill-carrier 0..255"
    fi
    mkdir -p "$OUT"
    : >"$OUT/run.log"
    csv_init
    note "grid $grid_inject: loss=$LOSS_LIST carriers=$CARRIER_LIST modes=$MODE_LIST repeats=$REPEATS window=${WARMUP}+${DURATION}s"
    grid_mode_list=$(printf '%s' "$MODE_LIST" | tr ',' ' ')
    grid_rep=1
    grid_runs=0
    while [ "$grid_rep" -le "$REPEATS" ]; do
        for grid_c in $(printf '%s' "$CARRIER_LIST" | tr ',' ' '); do
            for grid_l in $(printf '%s' "$LOSS_LIST" | tr ',' ' '); do
                if [ $((grid_rep % 2)) -eq 0 ]; then
                    grid_modes="adaptive fixed"
                else
                    grid_modes="fixed adaptive"
                fi
                grid_ord=0
                for grid_m in $grid_modes; do
                    case " $grid_mode_list " in
                    *" $grid_m "*) ;;
                    *) continue ;;
                    esac
                    grid_ord=$((grid_ord + 1))
                    grid_runs=$((grid_runs + 1))
                    if ! run_one "$grid_inject" "$grid_l" "$grid_c" "$grid_m" "$grid_rep" "$grid_ord"; then
                        note "stop: byte check failed at run $grid_runs"
                        return 1
                    fi
                done
            done
        done
        grid_rep=$((grid_rep + 1))
    done
    note "grid done: $grid_runs runs -> $OUT/runs.csv"
    return 0
}

# ---------------------------------------------------------------------------
# dry-run

dry_run_plan() {
    plan_inject=$1
    plan_runs=0
    for plan_c in $(printf '%s' "$CARRIER_LIST" | tr ',' ' '); do
        for plan_l in $(printf '%s' "$LOSS_LIST" | tr ',' ' '); do
            for plan_m in $(printf '%s' "$MODE_LIST" | tr ',' ' '); do
                plan_runs=$((plan_runs + REPEATS))
            done
        done
    done
    say "dry-run: ${SCRIPT:-adaptive} inject=$plan_inject"
    say "  near=$NEAR ($NEAR_IP) far=$FAR ($FAR_IP)"
    say "  near dev=$NEAR_DEV  ports near=$NEAR_PORT far=$FAR_PORT target=$TARGET"
    say "  shape n=$N m=$M w=8 carriers=$CARRIER_LIST adaptive=$MODE_LIST"
    say "  window ${WARMUP}s warm-up + ${DURATION}s steady, repeats=$REPEATS, $plan_runs runs"
    say "  loss list $LOSS_LIST %"
    say "  far config: tunnel 1 role=far listen=0.0.0.0:$FAR_PORT target=$TARGET n=$N m=$M fec=on mode=grouped w=8 carriers=<C> adaptive=<on|off> crypto=on ... portblock=$FAR_BLOCK nonce=$NONCE"
    say "  near config: tunnel 1 role=near listen=127.0.0.1:$NEAR_PORT peer=$FAR_IP:$FAR_PORT n=$N m=$M fec=on mode=grouped w=8 carriers=<C> adaptive=<on|off> crypto=on ... dev=$NEAR_DEV"
    case $plan_inject in
    ppm)
        say "  inject: for i in 0..<C-1>: printf 'carrier %d drop_ppm <loss*10000>' > /proc/net/tbtun/{carriers|peers}"
        say "          ends=$INJECT_ENDS (debug_loss=1 required)"
        ;;
    netem)
        say "  inject on $(netem_hosts): tc qdisc add dev <dev> root handle 1: prio bands 4"
        say "          tc qdisc add dev <dev> parent 1:4 handle 40: netem loss <loss>%"
        say "          tc filter add dev <dev> parent 1: protocol ip prio 1 u32 match ip <sport|dport> $FAR_PORT 0xffff flowid 1:4"
        say "          (--simple: root netem, ssh included; watchdog ${SAFETY_SECS}s)"
        ;;
    esac
    say "  transfer: curl -fsS --max-time $((WARMUP + DURATION + 2)) http://127.0.0.1:$NEAR_PORT$URL_PATH"
    say "            streamed through tee|wc -c and sha256sum on near; far prefix hash: head -c N $SOURCE | sha256sum"
    say "  output: ${OUT:-<results>/<UTC>-adaptive-$SCRIPT}"
    case $plan_inject in
    ppm) say "  cleanup: carrier <i> drop_ppm 0; printf 'del 1' > /proc/net/tbtun/config; ip rule del pref $BYPASS_PREF to $FAR_IP/32" ;;
    netem) say "  cleanup: tc qdisc del dev <dev> root (marker-guarded); printf 'del 1' > /proc/net/tbtun/config; ip rule del pref $BYPASS_PREF to $FAR_IP/32" ;;
    esac
    say "  no ssh, no writes (dry-run)"
}

# ---------------------------------------------------------------------------
# results: merge, medians, verdict, JSON

aggregate_to() {
    agg_out=$1
    shift
    mkdir -p "$OUT"
    agg_tmp=$agg_out.tmp
    agg_first=$1
    [ -f "$agg_first" ] || { warn "no runs.csv: $agg_first"; return 1; }
    {
        head -1 "$agg_first"
        for agg_f in "$@"; do
            [ -f "$agg_f" ] && tail -n +2 "$agg_f"
        done
    } >"$agg_tmp"
    mv "$agg_tmp" "$agg_out"
    medians_from "$agg_out" >"$OUT/medians.csv"
    json_runs "$agg_out" >"$OUT/runs.jsonl"
    verdict_from "$agg_out" "$OUT/medians.csv" >"$OUT/verdict.txt"
    json_summary "$agg_out" "$OUT/medians.csv" "$OUT/verdict.txt" >"$OUT/summary.json"
    say "== results"
    say "   $agg_out"
    say "   $OUT/medians.csv"
    say "   $OUT/verdict.txt"
    say "   $OUT/summary.json"
}

medians_from() {
    printf '%s\n' "$MED_HEADER"
    awk -F, -v OFS=, '
        NR == 1 { next }
        {
            k = $3 SUBSEP $4 SUBSEP $6 SUBSEP $7
            if (!(k in seen)) { seen[k] = 1; order[++n] = k }
            app[k, ++cnt[k]] = $15 + 0
            wire[k, cnt[k]] = $18 + 0
            retx[k, cnt[k]] = $20 + 0
            p[k, cnt[k]] = $21 + 0
            pd[k, cnt[k]] = $23 + 0
            sw[k, cnt[k]] = $26 + 0
            cpu[k, cnt[k]] = $31 + 0
            ok[k, cnt[k]] = $41 + 0
        }
        function medof(a, m,   i, j, v) {
            for (i = 2; i <= m; i++) {
                v = a[i]
                for (j = i - 1; j >= 1 && a[j] > v; j--) a[j + 1] = a[j]
                a[j + 1] = v
            }
            if (m % 2) return a[(m + 1) / 2]
            return (a[m / 2] + a[m / 2 + 1]) / 2
        }
        END {
            for (i = 1; i <= n; i++) {
                k = order[i]
                split(k, key, SUBSEP)
                m = cnt[k]
                amin = app[k, 1]; amax = app[k, 1]
                okall = 1; pdmax = 0
                for (j = 1; j <= m; j++) {
                    if (app[k, j] < amin) amin = app[k, j]
                    if (app[k, j] > amax) amax = app[k, j]
                    if (ok[k, j] != 1) okall = 0
                    d = pd[k, j]; if (d < 0) d = -d
                    if (d > pdmax) pdmax = d
                }
                for (j = 1; j <= m; j++) v[j] = app[k, j]
                am = medof(v, m)
                for (j = 1; j <= m; j++) v[j] = wire[k, j]
                wm = medof(v, m)
                for (j = 1; j <= m; j++) v[j] = retx[k, j]
                rm = medof(v, m)
                for (j = 1; j <= m; j++) v[j] = p[k, j]
                pm = medof(v, m)
                for (j = 1; j <= m; j++) v[j] = sw[k, j]
                sm = medof(v, m)
                for (j = 1; j <= m; j++) v[j] = cpu[k, j]
                cm = medof(v, m)
                printf "%s,%s,%s,%s,%d,%.3f,%.3f,%.3f,%.4f,%d,%d,%.2f,%d,%.1f,%d\n",
                    key[1], key[2], key[3], key[4], m,
                    am, amin, amax, wm, rm, pm, pdmax, sm, cm, okall
            }
        }' "$1"
}

json_runs() {
    awk -F, '
        NR == 1 { next }
        {
            printf "{\"stamp\":\"%s\",\"script\":\"%s\",\"inject\":\"%s\",\"loss_pct\":%s,\"ppm\":%s,\"carriers\":%s,\"mode\":\"%s\",\"adaptive\":\"%s\",\"rep\":%s,\"window_s\":%s,\"early\":%s,\"app_bytes\":%s,\"app_mbps\":%s,\"curl_bytes\":%s,\"wire_bytes\":%s,\"wire_per_app\":%s,\"retx_bytes\":%s,\"retx_ppm\":%s,\"p_ppm\":%s,\"p_win_ppm\":%s,\"p_delta_pp\":%s,\"sent\":%s,\"miss\":%s,\"switches\":%s,\"aborts\":%s,\"rollbacks\":%s,\"state\":\"%s\",\"reason\":\"%s\",\"near_cpu_pct\":%s,\"far_cpu_pct\":%s,\"hol_max_us\":%s,\"holes_unrecoverable\":%s,\"fec_reset_hole\":%s,\"lost\":%s,\"has_c13\":%s,\"bytes\":%s,\"bytes_ok\":%s,\"curl_rc\":\"%s\",\"rc\":%s,\"r_fail_ppm\":%s,\"pauses\":%s,\"pause_ns\":%s,\"sample_unknown\":%s,\"abandoned\":%s,\"note\":\"%s\"}\n",
                $1, $2, $3, $4, $5, $6, $7, $8, $9, $11, $13, $14, $15, $16, $17, $18, $19, $20, $21, $22, $23, $24, $25, $26, $27, $28, $29, $30, $31, $32, $33, $35, $36, $37, $38, $40, $41, $42, $43, $44, $45, $46, $47, $48, $49
        }' "$1"
}

verdict_from() {
    awk -F, '
        NR == FNR {
            if (FNR == 1) next
            key = $1 SUBSEP $2 SUBSEP $3 SUBSEP $4
            app[key] = $6 + 0
            sw[key] = $13 + 0
            cpu[key] = $14 + 0
            ok[key] = $15 + 0
            next
        }
        FNR == 1 { next }
        {
            runs++
            if ($41 + 0 != 1) s1bad++
            if ($35 + 0 > 0) s1hole++
            if ($36 + 0 > 0) s1reset++
            if ($43 + 0 != 0) s1rc++
            if ($26 + 0 > maxsw) maxsw = $26 + 0
            if ($33 + 0 > maxhol) maxhol = $33 + 0
            if ($45 + 0 > 0) {
                pm = ($46 + 0 / $45 + 0) / 1000000
                if (pm > maxpause) maxpause = pm
            }
            if ($37 + 0 > 0) anylost = 1
            if ($3 == "drop_ppm" && $4 + 0 > 0) {
                hasp = 1
                d = $23 + 0; if (d < 0) d = -d
                if (d > pdmax) pdmax = d
                if ($38 + 0 != 1) pmissing = 1
            }
            if ($3 == "netem" && $4 + 0 == 5 && $6 + 0 == 256 && $49 ~ /burst/) {
                hasburst = 1
                bk = "burst" SUBSEP $7
                bsum[bk] += $15 + 0; bcnt[bk]++
            }
            if ($49 ~ /kill=/) {
                haskill = 1
                if ($41 + 0 != 1) killbad = 1
            }
        }
        function v(name, state, detail) { printf "%s %s %s\n", name, state, detail }
        function ratio(a, b) { return (b > 0) ? a / b : 0 }
        END {
            v("S1", (runs > 0 && s1bad == 0 && s1hole == 0 && s1reset == 0 && s1rc == 0) ? "PASS" : "FAIL",
              "runs=" runs " bytes_bad=" s1bad + 0 " holes=" s1hole + 0 " reset_hole=" s1reset + 0 " rc_bad=" s1rc + 0)
            if (hasp && !pmissing)
                v("S2", (pdmax <= 1.5) ? "PASS" : "FAIL", "max |p_est-loss|=" pdmax " pp (limit 1.5, drop_ppm)")
            else if (hasp)
                v("S2", "SKIP", "c13 p_ppm missing (build predates WP9)")
            else
                v("S2", "SKIP", "no drop_ppm runs with loss>0")

            split("0 1.60 1 1.40 3 1.20 5 0.98 10 0.97", lim, " ")
            for (i = 1; i <= 5; i++) {
                L = lim[2 * i - 1]; need = lim[2 * i]
                f = app["drop_ppm" SUBSEP L SUBSEP 256 SUBSEP "fixed"]
                a = app["drop_ppm" SUBSEP L SUBSEP 256 SUBSEP "adaptive"]
                if (f > 0 && a > 0)
                    v("S" (i + 2), (ratio(a, f) >= need) ? "PASS" : "FAIL",
                      "loss=" L "% adaptive/fixed=" sprintf("%.3f", ratio(a, f)) " need>=" need)
                else
                    v("S" (i + 2), "SKIP", "loss=" L "% 256-carrier pair missing")
            }
            v("S8", (runs > 0 && maxsw <= 3 && maxhol <= 500000 && maxpause <= 500 && !anylost) ? "PASS" : "FAIL",
              "max switches/run=" maxsw " max rx stall=" maxhol "us mean switch pause=" sprintf("%.1f", maxpause) "ms carrier_lost=" (anylost ? 1 : 0))
            f = cpu["drop_ppm" SUBSEP 5 SUBSEP 256 SUBSEP "fixed"]
            a = cpu["drop_ppm" SUBSEP 5 SUBSEP 256 SUBSEP "adaptive"]
            if (f > 0 && a > 0)
                v("S9", (ratio(a, f) <= 1.05) ? "PASS" : "FAIL",
                  "near cpu adaptive/fixed=" sprintf("%.3f", ratio(a, f)) " need<=1.05")
            else
                v("S9", "SKIP", "256-carrier 5% CPU pair missing")
            v("S10", (runs > 0 && s1bad == 0) ? "PASS" : "FAIL", "32/64-carrier rows share S1; see runs.csv")
            if (hasburst) {
                f = bsum["burst" SUBSEP "fixed"] / (bcnt["burst" SUBSEP "fixed"] ? bcnt["burst" SUBSEP "fixed"] : 1)
                a = bsum["burst" SUBSEP "adaptive"] / (bcnt["burst" SUBSEP "adaptive"] ? bcnt["burst" SUBSEP "adaptive"] : 1)
                v("S11", (f > 0 && a > 0 && ratio(a, f) >= 0.95) ? "PASS" : "FAIL",
                  "netem burst 5% adaptive/fixed=" sprintf("%.3f", ratio(a, f)) " need>=0.95")
            } else
                v("S11", "SKIP", "no netem burst rows (run netem.sh --burst --loss 5)")
            if (haskill)
                v("S12", (killbad ? "FAIL" : "PASS"), "carrier kill rows present; bytes_ok=" (killbad ? 0 : 1))
            else
                v("S12", "SKIP", "no --kill-at rows")
        }' "$2" "$1"
}

json_summary() {
    js_runs=$(awk 'END { print NR - 1 }' "$1")
    {
        printf '{"generated":"%s","runs":%s,"medians":[' "$(date -u +%Y%m%dT%H%M%SZ)" "$js_runs"
        awk -F, 'NR > 1 {
            if (NR > 2) printf ","
            printf "{\"inject\":\"%s\",\"loss_pct\":%s,\"carriers\":%s,\"mode\":\"%s\",\"runs\":%s,\"app_mbps_med\":%s,\"wire_per_app_med\":%s,\"retx_ppm_med\":%s,\"p_ppm_med\":%s,\"p_delta_pp_max\":%s,\"switches_med\":%s,\"near_cpu_med\":%s,\"bytes_ok_all\":%s}",
                $1, $2, $3, $4, $5, $6, $9, $10, $11, $12, $13, $14, $15
        }' "$2"
        printf '],"verdict":['
        awk '{ gsub(/"/, ""); if (NR > 1) printf ","; printf "{\"id\":\"%s\",\"state\":\"%s\",\"detail\":\"%s\"}", $1, $2, substr($0, index($0, $3)) }' "$3"
        printf ']}\n'
    }
}

# ---------------------------------------------------------------------------
# cleanup

do_cleanup() {
    say "== cleanup"
    if [ -n "${SC_SAMPLE_STOP:-}" ]; then
        "$SC_SAMPLE_STOP" >/dev/null 2>&1 || true
    fi
    for h in "$NEAR" "$FAR"; do
        remote_sh "$h" true >/dev/null 2>&1 ||
            warn "$h unreachable; its qdisc/drop_ppm/config was not cleaned"
    done
    if [ "$CLEAN_NETEM" = 1 ]; then
        netem_clear
        say "   netem qdiscs removed (marker-guarded)"
    fi
    if [ "$CLEAN_PPM" = 1 ]; then
        inject_ppm_clear
        say "   carrier drop_ppm reset to 0"
    fi
    if [ "$CLEAN_TUNNEL" = 1 ]; then
        for h in "$NEAR" "$FAR"; do
            remote_sh "$h" "printf 'del 1\n' > /proc/net/tbtun/config 2>/dev/null || true" >/dev/null 2>&1 || true
        done
        say "   tunnel 1 deleted on both ends"
    fi
    cleanup_bypass
    transfer_cleanup
    say "   done"
}

ta_cleanup_trap() {
    st=$?
    trap - EXIT INT TERM
    [ "$TA_TRAP" = 1 ] || exit "$st"
    [ "$DRY" = 1 ] && exit "$st"
    if [ -n "${SC_SAMPLE_STOP:-}" ]; then
        "$SC_SAMPLE_STOP" >/dev/null 2>&1 || true
    fi
    transfer_stop >/dev/null 2>&1 || true
    case ${INJECT_ACTIVE:-} in
    ppm) inject_ppm_clear >/dev/null 2>&1 || true ;;
    netem) netem_clear >/dev/null 2>&1 || true ;;
    esac
    exit "$st"
}

install_trap() {
    TA_TRAP=1
    trap ta_cleanup_trap EXIT INT TERM
}
