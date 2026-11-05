#!/usr/bin/env bash
# deploy_hnd0.sh — tbtun far-side deploy / rollback driver for HND0.
#
# Target box: HND0 45.11.1.62, 2 vCPU / 955 MiB, physical 1G, 256-flow.
# Interface contract: review/goal-v6-interfaces.md (master ab1d297; ABI
# identical to 7105507).
#
# Two ABIs:
#   v6 (default)  fec=on mode=grouped n=4 m=4 w=0 carriers=256 crypto=on
#                 crypto_key=-  -> 32 groups x 8 shares, 1 share/carrier.
#                 insmod max_carriers=256 max_tunnels=1.
#   legacy        old ARQ keys (n/m/profile/nonce, optional tx_window_bytes),
#                 n+m <= 16. Used for the A-side N=1/4/8 stepping stone.
#
# Invariants:
#   - Dry-run is the default. Nothing is loaded and no service is touched
#     unless --execute is given.
#   - Production services (frps, caddy, hysteria-server, xray) are never
#     stopped or restarted by deploy. Their state is snapshotted, and rollback
#     restarts only a service that was active before deploy and is inactive
#     now.
#   - The script owns only: the tbtun module, /proc/net/tbtun/config,
#     /proc/net/tbtun/ports, the transient unit tbtun-iperf3-<target-port>,
#     and the two experiment ports.
#   - No routing or firewall change is made. Routes are recorded and compared,
#     never written.
#   - execute takes /tmp/tbtun-hnd0.lock (flock -w 1800) and writes a holder
#     line; the lock is the maintenance-window claim required by §7.
#
# Usage:
#   ./deploy_hnd0.sh status
#   ./deploy_hnd0.sh deploy --execute                 # v6 production profile
#   ./deploy_hnd0.sh deploy --execute --abi legacy --carriers 4 --spares 1
#   ./deploy_hnd0.sh rollback --execute
#   ./deploy_hnd0.sh verify
#
# Options (deploy):
#   --abi v6|legacy        config dialect (default v6)
#   --module PATH          tbtun.ko to load (default ./tbtun.ko)
#   --fec on|off           v6 only (default on)
#   --mode rlnc|grouped|sc-ldpc   v6 only (default grouped)
#   --n N                  data shares per group (default 4)
#   --m M                  parity shares per group (default 4; legacy 1)
#   --w W                  window (default 0 = auto; v6 renders 2*n)
#   --carriers N           carrier count (default 256; legacy 4)
#   --crypto on|off        v6 only (default on)
#   --crypto-key HEX|-     64 hex or '-' (default -)
#   --profile P            legacy profile (default raw)
#   --nonce HEX            32 hex digits (default fixed experiment nonce)
#   --window-bytes W       legacy only: tx_window_bytes (default 0 = auto)
#   --carrier-port P       far listen port (default 9000)
#   --target-port P        local target port (default 5201)
#   --portblock LO-HI      global source port block written to
#                          /proc/net/tbtun/ports (default 30000-34095, 4096 ports)
#   --app-target-mbps M    acceptance floor for T5.2 (default 33.72)
#   --loss-p PPM           recorded loss target for T5.2 (default 0)
#   --evidence-dir DIR     runtime evidence copy (default /tmp/tbtun-hnd0-evidence)
#   --state-dir DIR        runtime snapshot directory (default /tmp/tbtun-hnd0)
#   --execute              actually run; without it everything is dry-run

set -euo pipefail

PROG=${0##*/}
CMD=status
MODE=dry-run

ABI=v6
MODULE=./tbtun.ko
FEC=on
FEC_MODE=grouped
N=4
M=4
W=0
CARRIERS=256
CRYPTO=on
CRYPTO_KEY=-
PROFILE=raw
NONCE=00112233445566778899aabbccddeeff
WINDOW_BYTES=0
CARRIER_PORT=9000
TARGET_PORT=5201
PORTBLOCK=30000-34095
APP_TARGET_MBPS=33.72
LOSS_PPM=0
EVIDENCE_DIR=/tmp/tbtun-hnd0-evidence
STATE_DIR=/tmp/tbtun-hnd0
LOCK_FILE=/tmp/tbtun-hnd0.lock
LOCK_WAIT=1800
PURPOSE="${PURPOSE:-tbtun HND0 deploy/rollback}"
PROD_SERVICES="frps caddy hysteria-server xray"

usage() {
    sed -n '2,62p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 1; }
note() { printf '%s: %s\n' "$PROG" "$*" >&2; }

run() {
    if [[ $MODE == dry-run ]]; then
        printf '[dry-run] '
        printf '%q ' "$@"
        printf '\n'
    else
        printf '+ '; printf '%q ' "$@"; printf '\n'
        "$@"
    fi
}

run_sh() {
    if [[ $MODE == dry-run ]]; then
        printf '[dry-run] sh -c %s\n' "$1"
    else
        printf '+ sh -c %s\n' "$1"
        sh -c "$1"
    fi
}

require_root() {
    [[ $MODE == dry-run ]] && return 0
    [[ $(id -u) -eq 0 ]] || die "deploy/rollback must run as root"
}

take_lock() {
    [[ $MODE == dry-run ]] && { printf '[dry-run] flock -w %s %s (holder: %s)\n' "$LOCK_WAIT" "$LOCK_FILE" "$PURPOSE"; return 0; }
    exec 9>"$LOCK_FILE"
    flock -w "$LOCK_WAIT" 9 || die "lock busy: $LOCK_FILE (do not kill the holder)"
    printf '%s %s %s %s\n' "$$" "$(date -Is)" "$(whoami)" "$PURPOSE" >&9
    trap 'flock -u 9' EXIT
    printf 'lock: %s holder: %s\n' "$LOCK_FILE" "$PURPOSE"
}

module_loaded() { lsmod | awk '$1=="tbtun"{found=1} END{exit !found}'; }

listening_on() {
    ss -lntH 2>/dev/null | awk -v p=":$1" '$4 ~ p"$" {found=1} END{exit !found}'
}

service_state() { systemctl is-active "$1" 2>/dev/null || true; }

# Returns 0 when the reserved ranges fully cover [lo,hi], 1 otherwise.
# ip_local_reserved_ports means "reserved from ephemeral allocation", so
# coverage is desirable; a gap is the risk.
portblock_fully_reserved() {
    local res lo hi tok a b
    res=$(sysctl -n net.ipv4.ip_local_reserved_ports 2>/dev/null || true)
    [[ -z $res ]] && return 1
    lo=${PORTBLOCK%-*}
    hi=${PORTBLOCK#*-}
    local -a ranges=()
    for tok in ${res//,/ }; do
        a=${tok%-*}
        b=${tok#*-}
        [[ $tok == *-* ]] || b=$a
        [[ $a =~ ^[0-9]+$ && $b =~ ^[0-9]+$ ]] || continue
        ranges+=("$a $b")
    done
    (( ${#ranges[@]} )) || return 1
    local sorted cur_lo=-1 cur_hi=-1
    sorted=$(printf '%s\n' "${ranges[@]}" | sort -n -k1,1)
    while read -r a b; do
        [[ -z $a ]] && continue
        if (( cur_lo < 0 )); then
            cur_lo=$a; cur_hi=$b
        elif (( a <= cur_hi + 1 )); then
            (( b > cur_hi )) && cur_hi=$b
        else
            (( cur_lo <= lo && cur_hi >= hi )) && return 0
            cur_lo=$a; cur_hi=$b
        fi
    done <<< "$sorted"
    (( cur_lo >= 0 && cur_lo <= lo && cur_hi >= hi )) && return 0
    return 1
}

# Warn only; never change sysctl.
portblock_reserved_warn() {
    if ! portblock_fully_reserved; then
        printf 'warning: net.ipv4.ip_local_reserved_ports does not fully cover portblock=%s (ephemeral ports may collide)\n' \
            "$PORTBLOCK" >&2
    fi
}

snapshot() {
    local stamp file
    stamp=$(date +%Y%m%d-%H%M%S)
    file="$STATE_DIR/state-$stamp.txt"
    if [[ $MODE == dry-run ]]; then
        printf '[dry-run] snapshot -> %s (copy to %s/)\n' "$file" "$EVIDENCE_DIR"
        return 0
    fi
    mkdir -p "$STATE_DIR"
    {
        printf 'timestamp=%s\n' "$(date -Is)"
        printf 'kernel=%s\n' "$(uname -r)"
        printf 'nproc=%s\n' "$(nproc)"
        printf 'mem_total_kb=%s\n' "$(awk '/MemTotal/{print $2}' /proc/meminfo)"
        printf 'abi=%s fec=%s mode=%s n=%s m=%s w=%s carriers=%s crypto=%s\n' \
            "$ABI" "$FEC" "$FEC_MODE" "$N" "$M" "$W" "$CARRIERS" "$CRYPTO"
        printf 'app_target_mbps=%s loss_ppm=%s\n' "$APP_TARGET_MBPS" "$LOSS_PPM"
        if module_loaded; then printf 'module=loaded\n'; else printf 'module=absent\n'; fi
        printf 'services:\n'
        for s in $PROD_SERVICES; do printf '  %s=%s\n' "$s" "$(service_state "$s")"; done
        printf 'listeners:\n'
        ss -lntH | awk '{print "  "$4}'
        printf 'routes:\n'
        ip route show | sed 's/^/  /'
        printf 'rules:\n'
        ip rule show | sed 's/^/  /'
    } > "$file"
    ln -sf "$file" "$STATE_DIR/latest"
    if [[ -n $EVIDENCE_DIR ]]; then
        mkdir -p "$EVIDENCE_DIR"
        cp -f "$file" "$EVIDENCE_DIR/"
        printf 'snapshot: %s (evidence copy in %s/)\n' "$file" "$EVIDENCE_DIR"
    else
        printf 'snapshot: %s\n' "$file"
    fi
}

latest_snapshot() {
    [[ -L $STATE_DIR/latest ]] || return 1
    readlink -f "$STATE_DIR/latest"
}

check_ports_free() {
    local p
    for p in "$CARRIER_PORT" "$TARGET_PORT"; do
        if listening_on "$p"; then
            die "port $p already has a listener; refusing to deploy"
        fi
        printf 'port %s: free\n' "$p"
    done
}

validate_profile() {
    case $ABI in
        v6|legacy) ;;
        *) die "--abi must be v6 or legacy" ;;
    esac
    case $FEC in on|off) ;; *) die "--fec must be on or off" ;; esac
    case $FEC_MODE in rlnc|grouped|sc-ldpc) ;; *) die "--mode must be rlnc, grouped, or sc-ldpc" ;; esac
    case $CRYPTO in on|off) ;; *) die "--crypto must be on or off" ;; esac
    [[ $N =~ ^[0-9]+$ ]] || die "--n must be decimal"
    [[ $M =~ ^[0-9]+$ ]] || die "--m must be decimal"
    [[ $W =~ ^[0-9]+$ ]] || die "--w must be decimal"
    [[ $CARRIERS =~ ^[0-9]+$ ]] || die "--carriers must be decimal"
    if [[ $ABI == v6 ]]; then
        (( CARRIERS >= 1 && CARRIERS <= 256 )) || die "--carriers must be 1..256 for v6"
        (( N >= 1 && N <= 255 )) || die "--n must be 1..255"
        if [[ $FEC == on ]]; then
            (( M >= 1 && M <= 255 )) || die "--m must be 1..255 when fec=on"
        else
            (( M >= 0 && M <= 255 )) || die "--m must be 0..255 when fec=off"
        fi
        (( N + M <= 256 )) || die "n+m must be <= 256"
        (( CARRIERS % (N + M) == 0 )) || die "carriers must divide by (n+m) for 1 share/carrier"
        if [[ $FEC == on ]]; then
            (( CARRIERS >= N + M )) || die "fec=on requires carriers >= n+m"
        fi
        if [[ $CRYPTO == on && $FEC == off ]]; then
            die "crypto=on requires fec=on"
        fi
        if [[ $FEC_MODE == sc-ldpc ]]; then
            (( W == 0 || W == 256 || W == 512 )) || die "--w for sc-ldpc must be 0, 256, or 512"
        else
            (( W == 0 || (W >= N && W <= 1024) )) || die "--w must be 0 or n..1024"
        fi
        if [[ $CRYPTO_KEY != - ]]; then
            [[ $CRYPTO_KEY =~ ^[0-9a-fA-F]{64}$ ]] || die "--crypto-key must be 64 hex digits or -"
        fi
    else
        (( N + M <= 16 )) || die "legacy ARQ supports n+m <= 16 and max_carriers <= 16"
        (( CARRIERS <= 16 )) || die "legacy ARQ supports n+m <= 16 and max_carriers <= 16"
    fi
    [[ $PORTBLOCK =~ ^[0-9]+-[0-9]+$ ]] || die "--portblock must be LO-HI"
}

config_line() {
    local line
    if [[ $ABI == v6 ]]; then
        line="tunnel 1 role=far listen=0.0.0.0:$CARRIER_PORT target=127.0.0.1:$TARGET_PORT"
        line+=" n=$N m=$M fec=$FEC mode=$FEC_MODE w=$W carriers=$CARRIERS crypto=$CRYPTO crypto_key=$CRYPTO_KEY"
        line+=" profile=$PROFILE keepalive=5000 drip_ms=1000 carrier_stall_ms=15000"
        line+=" portblock=$PORTBLOCK nonce=$NONCE"
    else
        line="tunnel 1 role=far listen=0.0.0.0:$CARRIER_PORT target=127.0.0.1:$TARGET_PORT"
        line+=" n=$N m=$M profile=$PROFILE"
        line+=" keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000"
        line+=" hole_budget_ms=30000 rx_reorder_bytes=262144 tx_retain_bytes=0 carrier_stall_ms=15000"
        line+=" portblock=$PORTBLOCK nonce=$NONCE"
        if (( WINDOW_BYTES > 0 )); then
            line+=" tx_window_bytes=$WINDOW_BYTES"
        fi
    fi
    printf '%s\n' "$line"
}

print_plan() {
    printf 'mode=%s cmd=%s abi=%s\n' "$MODE" "$CMD" "$ABI"
    if [[ $ABI == v6 ]]; then
        printf 'fec=%s mode=%s n=%s m=%s w=%s carriers=%s crypto=%s crypto_key=%s\n' \
            "$FEC" "$FEC_MODE" "$N" "$M" "$W" "$CARRIERS" "$CRYPTO" "$CRYPTO_KEY"
    else
        printf 'n=%s m=%s profile=%s window_bytes=%s\n' "$N" "$M" "$PROFILE" "$WINDOW_BYTES"
    fi
    printf 'carrier_port=%s target_port=%s portblock=%s app_target_mbps=%s loss_ppm=%s\n' \
        "$CARRIER_PORT" "$TARGET_PORT" "$PORTBLOCK" "$APP_TARGET_MBPS" "$LOSS_PPM"
    printf 'module=%s state_dir=%s evidence_dir=%s lock=%s\n' \
        "$MODULE" "$STATE_DIR" "$EVIDENCE_DIR" "$LOCK_FILE"
}

do_status() {
    print_plan
    printf 'module: '
    if module_loaded; then printf 'loaded\n'; else printf 'absent\n'; fi
    printf 'proc: '
    if [[ -d /proc/net/tbtun ]]; then printf 'present\n'; else printf 'absent\n'; fi
    printf 'services:\n'
    for s in $PROD_SERVICES; do printf '  %s=%s\n' "$s" "$(service_state "$s")"; done
    printf 'ports:\n'
    for p in "$CARRIER_PORT" "$TARGET_PORT"; do
        if listening_on "$p"; then printf '  %s=listening\n' "$p"; else printf '  %s=free\n' "$p"; fi
    done
    if module_loaded; then
        printf 'ports_block=%s\n' "$(cat /proc/net/tbtun/ports 2>/dev/null | awk '$1=="ports"{print $2; exit}')"
    fi
    portblock_reserved_warn
    printf 'snapshots:\n'
    if [[ -L $STATE_DIR/latest ]]; then printf '  latest=%s\n' "$(readlink -f "$STATE_DIR/latest")"; else printf '  none\n'; fi
    printf 'lock:\n'
    if [[ -e $LOCK_FILE ]]; then printf '  %s present\n' "$LOCK_FILE"; else printf '  %s absent\n' "$LOCK_FILE"; fi
}

do_deploy() {
    print_plan
    validate_profile
    require_root
    take_lock
    if [[ ! -f $MODULE ]]; then
        if [[ $MODE == dry-run ]]; then
            note "module missing: $MODULE (dry-run continues; build before --execute)"
        else
            die "module not found: $MODULE (build it first)"
        fi
    fi
    if module_loaded; then
        die "tbtun already loaded; run rollback first"
    fi
    check_ports_free
    snapshot
    if [[ $ABI == v6 ]]; then
        note "v6: carriers=$CARRIERS n=$N m=$M -> groups=$((CARRIERS / (N + M))) groups, 1 share/carrier"
        note "v6: insmod max_carriers=$CARRIERS max_tunnels=1 (128 MiB budget, §3.3)"
        if (( CARRIERS > 16 )); then
            note "256-carrier deploy requires the FEC v6 branch; a module without fec= keys will reject the config line"
        fi
    else
        note "legacy ARQ: n+m=$((N + M)) <= 16; no fec=/mode=/carriers= keys are written"
    fi
    if [[ $ABI == v6 ]]; then
        if ! run insmod "$MODULE" max_carriers="$CARRIERS" max_tunnels=1; then
            note "insmod failed for $CARRIERS carriers; rollback with: $PROG rollback --execute"
            return 1
        fi
    else
        if ! run insmod "$MODULE" max_carriers="$CARRIERS"; then
            note "insmod failed for $CARRIERS carriers; rollback with: $PROG rollback --execute"
            return 1
        fi
    fi
    if ! run_sh "printf 'ports %s\\n' '$PORTBLOCK' > /proc/net/tbtun/ports"; then
        note "ports block write failed; rollback with: $PROG rollback --execute"
        return 1
    fi
    local line
    line=$(config_line)
    if ! run_sh "printf '%s\\n' '$line' > /proc/net/tbtun/config"; then
        note "config write failed; rollback with: $PROG rollback --execute"
        return 1
    fi
    run_sh "cat /proc/net/tbtun/config; cat /proc/net/tbtun/tunnels; cat /proc/net/tbtun/ports"
    run systemd-run --unit="tbtun-iperf3-$TARGET_PORT" \
        --property=StandardOutput=file:"$STATE_DIR/iperf3-$TARGET_PORT.log" \
        --property=StandardError=file:"$STATE_DIR/iperf3-$TARGET_PORT.log" \
        /usr/bin/iperf3 -s -B 127.0.0.1 -p "$TARGET_PORT"
    run_sh "ss -lntH | awk '\$4 ~ \":$CARRIER_PORT\$\" || \$4 ~ \":$TARGET_PORT\$\"'"
    note "deploy requested; verify with: $PROG verify"
}

do_rollback() {
    print_plan
    require_root
    take_lock
    # Transient units do not appear in list-unit-files; stop is idempotent.
    run_sh "systemctl stop 'tbtun-iperf3-$TARGET_PORT.service' 2>/dev/null || true"
    run_sh "systemctl reset-failed 'tbtun-iperf3-$TARGET_PORT.service' 2>/dev/null || true"
    if module_loaded; then
        run rmmod tbtun
    else
        printf 'module: already absent\n'
    fi
    run_sh "test ! -d /proc/net/tbtun"
    run_sh "! ss -lntH | awk '\$4 ~ \":$CARRIER_PORT\$\" || \$4 ~ \":$TARGET_PORT\$\"' | grep -q ."
    local snap
    if snap=$(latest_snapshot); then
        printf 'restoring production services against snapshot: %s\n' "$snap"
        local s before now
        for s in $PROD_SERVICES; do
            before=$(awk -v k="  $s=" '$0 ~ k {sub(k,""); print; exit}' "$snap")
            now=$(service_state "$s")
            printf '  %s: before=%s now=%s\n' "$s" "${before:-unknown}" "$now"
            if [[ $before == active && $now != active ]]; then
                run systemctl start "$s"
            fi
        done
    else
        note "no snapshot found; checking services as-is"
        for s in $PROD_SERVICES; do printf '  %s=%s\n' "$s" "$(service_state "$s")"; done
    fi
    printf 'routes (unchanged by this script):\n'
    ip route show
    note "rollback requested; verify with: $PROG verify"
}

do_verify() {
    print_plan
    local fail=0
    if module_loaded; then
        printf 'module=loaded\n'
        local got
        got=$(cat /proc/net/tbtun/ports 2>/dev/null | awk '$1=="ports"{print $2; exit}')
        if [[ $got == "$PORTBLOCK" ]]; then
            printf 'ports_block=%s (matches)\n' "$got"
        else
            printf 'ports_block=%s (expected %s)\n' "${got:-missing}" "$PORTBLOCK"
            fail=1
        fi
    else
        printf 'module=absent\n'
    fi
    if [[ -d /proc/net/tbtun ]]; then printf 'proc=present\n'; else printf 'proc=absent\n'; fi
    portblock_reserved_warn
    for s in $PROD_SERVICES; do
        local st; st=$(service_state "$s")
        printf 'service %s=%s\n' "$s" "$st"
        [[ $st == active ]] || fail=1
    done
    for p in "$CARRIER_PORT" "$TARGET_PORT"; do
        if listening_on "$p"; then printf 'port %s=listening\n' "$p"; else printf 'port %s=free\n' "$p"; fi
    done
    if [[ -L $STATE_DIR/latest ]]; then
        printf 'snapshot=%s\n' "$(readlink -f "$STATE_DIR/latest")"
    else
        printf 'snapshot=none\n'
    fi
    printf 'app_target_mbps=%s loss_ppm=%s (throughput is measured by T5.2, not here)\n' "$APP_TARGET_MBPS" "$LOSS_PPM"
    return "$fail"
}

while (($#)); do
    case $1 in
        status|deploy|rollback|verify) CMD=$1; shift ;;
        --dry-run) MODE=dry-run; shift ;;
        --execute) MODE=execute; shift ;;
        --abi) ABI=$2; shift 2 ;;
        --module) MODULE=$2; shift 2 ;;
        --fec) FEC=$2; shift 2 ;;
        --mode) FEC_MODE=$2; shift 2 ;;
        --n) N=$2; shift 2 ;;
        --m) M=$2; shift 2 ;;
        --w) W=$2; shift 2 ;;
        --carriers) CARRIERS=$2; shift 2 ;;
        --crypto) CRYPTO=$2; shift 2 ;;
        --crypto-key) CRYPTO_KEY=$2; shift 2 ;;
        --profile) PROFILE=$2; shift 2 ;;
        --nonce) NONCE=$2; shift 2 ;;
        --window-bytes) WINDOW_BYTES=$2; shift 2 ;;
        --carrier-port) CARRIER_PORT=$2; shift 2 ;;
        --target-port) TARGET_PORT=$2; shift 2 ;;
        --portblock) PORTBLOCK=$2; shift 2 ;;
        --app-target-mbps) APP_TARGET_MBPS=$2; shift 2 ;;
        --loss-p) LOSS_PPM=$2; shift 2 ;;
        --evidence-dir) EVIDENCE_DIR=$2; shift 2 ;;
        --state-dir) STATE_DIR=$2; shift 2 ;;
        -h|--help) usage 0 ;;
        *) die "unknown argument: $1 (try --help)" ;;
    esac
done

case $CMD in
    status) do_status ;;
    deploy) do_deploy ;;
    rollback) do_rollback ;;
    verify) do_verify ;;
esac
