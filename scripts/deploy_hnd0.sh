#!/usr/bin/env bash
# deploy_hnd0.sh — tbtun far-side deploy / rollback driver for HND0.
#
# Target box: HND0 45.11.1.62, 2 vCPU / 955 MiB, physical 1G, 256-flow.
# Interface contract: review/goal-v6-interfaces.md §4 and §9.2 (v6 dialect;
# the legacy ABI is the old ARQ stepping stone).
#
# Two ABIs:
#   v6 (default)  fec=on adaptive=on mode=grouped n=4 m=4 w=0 carriers=256
#                 crypto=on crypto_key=-  -> 32 groups x 8 shares.
#                 insmod max_carriers=256 max_tunnels=1.
#                 adaptive=on is the 0.3.0 control law; T5.2 fixed-shape
#                 measurements must pass --adaptive off.
#   legacy        old ARQ keys (n/m/profile/nonce, optional tx_window_bytes),
#                 n+m <= 16. Used for the A-side N=1/4/8 stepping stone.
#                 max_carriers must be >= n+m.
#
# T5.2 profile: the 256/1G run must use profile=http-kal at least (raw stays
# as a control). Use --http-kal --adaptive off --wait-up 120. FEC shares and
# control frames travel in the HTTP chunked body; ChaCha8 encrypts the body
# only.
#
# SC-LDPC: mode=sc-ldpc is accepted by the parser but the data plane still
# returns -EOPNOTSUPP (D integration incomplete). The script refuses it unless
# --allow-unimplemented is given; that flag is for forward-compatible dry-runs,
# not production.
#
# Invariants:
#   - Dry-run is the default. Nothing is loaded and no service is touched
#     unless --execute is given.
#   - Production services (frps, caddy, hysteria-server/hysteria, xray) are
#     never stopped or restarted by deploy. The hysteria unit is detected:
#     hysteria-server first, then hysteria. Their state is snapshotted, and
#     rollback restarts only a service that was active before deploy and is
#     inactive now.
#   - The script owns only: the tbtun module, /proc/net/tbtun/config,
#     /proc/net/tbtun/ports, the transient target unit tbtun-target-<port>,
#     and the experiment ports.
#   - No routing or firewall change is made. Routes are recorded and compared,
#     never written.
#   - execute takes /tmp/tbtun-hnd0.lock (flock -w 1800) and writes a holder
#     line; the lock is the maintenance-window claim.
#   - The target is not started unless --iperf3 or --target-cmd is given.
#     The target command must listen on --target-port unless
#     --no-target-check is given.
#
# Usage:
#   ./deploy_hnd0.sh status
#   ./deploy_hnd0.sh deploy --execute --http-kal --adaptive off --wait-up 120
#   ./deploy_hnd0.sh deploy --execute --abi legacy --m 1
#   ./deploy_hnd0.sh rollback --execute
#   ./deploy_hnd0.sh verify
#
# Options (deploy):
#   --abi v6|legacy        config dialect (default v6)
#   --module PATH          tbtun.ko to load (default ./tbtun.ko)
#   --expect-srcversion HEX  require this srcversion before/after insmod
#   --fec on|off           v6 only (default on)
#   --adaptive on|off      v6 only (default on; T5.2 fixed shape: off)
#   --mode rlnc|grouped|sc-ldpc   v6 only (default grouped)
#   --allow-unimplemented  allow mode=sc-ldpc although no codec is merged
#   --n N                  data shares per group (default 4)
#   --m M                  parity shares per group (default 4; legacy 1)
#   --spares M             alias for --m
#   --w W                  window (default 0 = auto; v6 renders 2*n)
#   --carriers N           carrier count (default 256; legacy n+m)
#   --crypto on|off        v6 only (default on; off when fec=off)
#   --crypto-key HEX|-     64 hex or '-' (default -)
#   --profile P            profile (default raw)
#   --http-kal             shortcut for --profile http-kal (T5.2)
#   --raw                  shortcut for --profile raw
#   --nonce HEX            16 or 32 hex digits
#   --window-bytes W       legacy only: tx_window_bytes (default 0 = auto)
#   --carrier-port P       far listen port (default 9000)
#   --target-port P        target port (default 5201)
#   --target-cmd CMD       target command; must listen on target-port
#   --iperf3               target = iperf3 -s -B 127.0.0.1 -p target-port
#   --no-target            do not start a target (default)
#   --no-target-check      do not require the target listener in verify
#   --portblock LO-HI      global source port block (default 30000-34095)
#   --wait-up SECS         wait up to SECS for all carriers (0 = no wait)
#   --require-up           fail deploy/verify unless up == expected
#   --services "A B C"     override the production service list
#   --app-target-mbps M    recorded T5.2 acceptance floor (default 33.72)
#   --loss-p PPM           recorded T5.2 loss target (default 0)
#   --evidence-dir DIR     runtime evidence copy (/tmp/tbtun-hnd0-evidence)
#   --state-dir DIR        runtime snapshot directory (/tmp/tbtun-hnd0)
#   --execute              actually run; without it everything is dry-run

set -euo pipefail

PROG=${0##*/}
CMD=status
MODE=dry-run

ABI=v6
MODULE=./tbtun.ko
EXPECT_SRCVERSION=
FEC=on
ADAPTIVE=on
ADAPTIVE_SET=0
FEC_MODE=grouped
ALLOW_UNIMPLEMENTED=0
N=4
M=4
M_SET=0
W=0
CARRIERS=256
CARRIERS_SET=0
CRYPTO=on
CRYPTO_SET=0
CRYPTO_KEY=-
PROFILE=raw
NONCE=00112233445566778899aabbccddeeff
WINDOW_BYTES=0
CARRIER_PORT=9000
TARGET_PORT=5201
TARGET_KIND=none
TARGET_CMD=
TARGET_CHECK=1
PORTBLOCK=30000-34095
WAIT_UP=0
REQUIRE_UP=0
APP_TARGET_MBPS=33.72
LOSS_PPM=0
EVIDENCE_DIR=/tmp/tbtun-hnd0-evidence
STATE_DIR=/tmp/tbtun-hnd0
LOCK_FILE=/tmp/tbtun-hnd0.lock
LOCK_WAIT=1800
LOCK_HELD=0
DEPLOY_ACTIVE=0
DEPLOY_OK=0
PURPOSE="${PURPOSE:-tbtun HND0 deploy/rollback}"
PROD_SERVICES="${PROD_SERVICES:-}"

usage() {
    sed -n '2,/^set -euo pipefail$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
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
    if [[ $MODE == dry-run ]]; then
        printf '[dry-run] flock -w %s %s (holder: %s)\n' "$LOCK_WAIT" "$LOCK_FILE" "$PURPOSE"
        return 0
    fi
    exec 9>"$LOCK_FILE"
    flock -w "$LOCK_WAIT" 9 || die "lock busy: $LOCK_FILE (do not kill the holder)"
    LOCK_HELD=1
    printf '%s %s %s %s\n' "$$" "$(date -Is)" "$(whoami)" "$PURPOSE" >&9
    printf 'lock: %s holder: %s\n' "$LOCK_FILE" "$PURPOSE"
}

release_lock() {
    if [[ $LOCK_HELD == 1 ]]; then
        flock -u 9 2>/dev/null || true
        LOCK_HELD=0
    fi
}

module_loaded() { lsmod | awk '$1=="tbtun"{found=1} END{exit !found}'; }

listening_on() {
    ss -lntH 2>/dev/null | awk -v p=":$1" '$4 ~ p"$" {found=1} END{exit !found}'
}

service_state() { systemctl is-active "$1" 2>/dev/null || true; }

detect_services() {
    if [[ -n ${PROD_SERVICES:-} ]]; then
        return 0
    fi
    # HND0 evidence has hysteria-server active; the arm64 T5.2 driver's
    # `hysteria` is inactive in its own services-before.txt. Prefer the real
    # unit, then fall back to the other name.
    local base="frps caddy xray" h=""
    if command -v systemctl >/dev/null 2>&1; then
        if systemctl cat hysteria-server.service >/dev/null 2>&1; then
            h=hysteria-server
        elif systemctl cat hysteria.service >/dev/null 2>&1; then
            h=hysteria
        fi
    fi
    if [[ -z $h ]]; then
        h=hysteria-server
        note "hysteria unit not found; assuming hysteria-server"
    fi
    PROD_SERVICES="$base $h"
}

snapshot_value() {
    awk -v k="  $2=" '$0 ~ k {sub(k,""); print; exit}' "$1" 2>/dev/null
}

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
        printf 'abi=%s fec=%s adaptive=%s mode=%s n=%s m=%s w=%s carriers=%s crypto=%s\n' \
            "$ABI" "$FEC" "$ADAPTIVE" "$FEC_MODE" "$N" "$M" "$W" "$CARRIERS" "$CRYPTO"
        printf 'expect_srcversion=%s\n' "${EXPECT_SRCVERSION:-any}"
        printf 'target_kind=%s target_cmd=%s\n' "$TARGET_KIND" "$(target_command || true)"
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
    for p in "$CARRIER_PORT"; do
        if listening_on "$p"; then
            if [[ $MODE == dry-run ]]; then
                note "port $p already has a listener (dry-run continues)"
                printf 'port %s: busy (dry-run)\n' "$p"
            else
                die "port $p already has a listener; refusing to deploy"
            fi
        else
            printf 'port %s: free\n' "$p"
        fi
    done
    if [[ $TARGET_KIND != none ]]; then
        if listening_on "$TARGET_PORT"; then
            if [[ $MODE == dry-run ]]; then
                note "port $TARGET_PORT already has a listener (dry-run continues)"
                printf 'port %s: busy (dry-run)\n' "$TARGET_PORT"
            else
                die "port $TARGET_PORT already has a listener; refusing to deploy"
            fi
        else
            printf 'port %s: free\n' "$TARGET_PORT"
        fi
    fi
}

normalize_args() {
    if [[ $ABI == legacy ]]; then
        (( M_SET == 1 )) || M=1
        (( CARRIERS_SET == 1 )) || CARRIERS=$((N + M))
        ADAPTIVE=off
        CRYPTO=off
    else
        if [[ $FEC == off && $ADAPTIVE_SET == 0 ]]; then ADAPTIVE=off; fi
        if [[ $FEC == off && $CRYPTO_SET == 0 ]]; then CRYPTO=off; fi
    fi
}

validate_profile() {
    normalize_args
    case $ABI in
        v6|legacy) ;;
        *) die "--abi must be v6 or legacy" ;;
    esac
    case $FEC in on|off) ;; *) die "--fec must be on or off" ;; esac
    case $ADAPTIVE in on|off) ;; *) die "--adaptive must be on or off" ;; esac
    case $FEC_MODE in rlnc|grouped|sc-ldpc) ;; *) die "--mode must be rlnc, grouped, or sc-ldpc" ;; esac
    case $CRYPTO in on|off) ;; *) die "--crypto must be on or off" ;; esac
    case $PROFILE in raw|http-kal|http-kal-drip) ;; *) die "--profile must be raw, http-kal, or http-kal-drip" ;; esac
    [[ $N =~ ^[0-9]+$ ]] || die "--n must be decimal"
    [[ $M =~ ^[0-9]+$ ]] || die "--m must be decimal"
    [[ $W =~ ^[0-9]+$ ]] || die "--w must be decimal"
    [[ $CARRIERS =~ ^[0-9]+$ ]] || die "--carriers must be decimal"
    [[ $CARRIER_PORT =~ ^[0-9]+$ ]] || die "--carrier-port must be decimal"
    (( CARRIER_PORT >= 1 && CARRIER_PORT <= 65535 )) || die "--carrier-port must be 1..65535"
    [[ $TARGET_PORT =~ ^[0-9]+$ ]] || die "--target-port must be decimal"
    (( TARGET_PORT >= 1 && TARGET_PORT <= 65535 )) || die "--target-port must be 1..65535"
    [[ $WAIT_UP =~ ^[0-9]+$ ]] || die "--wait-up must be decimal seconds"
    [[ $REQUIRE_UP =~ ^[01]$ ]] || die "--require-up is a flag"
    if [[ -n $EXPECT_SRCVERSION ]]; then
        EXPECT_SRCVERSION=${EXPECT_SRCVERSION^^}
        [[ $EXPECT_SRCVERSION =~ ^[0-9A-F]+$ ]] || die "--expect-srcversion must be hex"
    fi
    [[ $NONCE =~ ^[0-9a-fA-F]{16}([0-9a-fA-F]{16})?$ ]] || die "--nonce must be 16 or 32 hex digits"
    [[ $PORTBLOCK =~ ^[0-9]+-[0-9]+$ ]] || die "--portblock must be LO-HI"
    (( 10#${PORTBLOCK%-*} <= 10#${PORTBLOCK#*-} )) || die "--portblock LO must be <= HI"
    if [[ $TARGET_KIND == cmd && -z $TARGET_CMD ]]; then
        die "--target-cmd requires a command"
    fi
    if [[ $ABI == v6 ]]; then
        if [[ $FEC_MODE == sc-ldpc && $ALLOW_UNIMPLEMENTED != 1 ]]; then
            die "mode=sc-ldpc is not usable in this tree (data plane returns -EOPNOTSUPP; D integration incomplete); pass --allow-unimplemented only for forward-compatible dry-runs"
        fi
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
        else
            (( N + M <= 16 )) || die "fec=off uses the legacy ARQ plane: n+m must be <= 16"
        fi
        if [[ $CRYPTO == on && $FEC == off ]]; then
            die "crypto=on requires fec=on"
        fi
        if [[ $FEC == off && $ADAPTIVE == on ]]; then
            die "adaptive=on requires fec=on"
        fi
        if [[ $FEC_MODE == rlnc ]]; then
            (( CARRIERS == N + M )) || die "--mode rlnc requires carriers == n+m"
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
        if [[ $FEC_MODE != grouped ]]; then
            die "--mode is v6 only"
        fi
        (( N + M <= 16 )) || die "legacy ARQ supports n+m <= 16 and max_carriers <= 16"
        (( CARRIERS >= N + M )) || die "--carriers must be >= n+m for legacy (max_carriers is the module cap)"
        (( CARRIERS <= 16 )) || die "legacy ARQ supports n+m <= 16 and max_carriers <= 16"
    fi
}

config_line() {
    local line
    if [[ $ABI == v6 ]]; then
        line="tunnel 1 role=far listen=0.0.0.0:$CARRIER_PORT target=127.0.0.1:$TARGET_PORT"
        line+=" n=$N m=$M fec=$FEC adaptive=$ADAPTIVE mode=$FEC_MODE w=$W carriers=$CARRIERS crypto=$CRYPTO crypto_key=$CRYPTO_KEY"
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

expected_w() {
    if (( W > 0 )); then
        printf '%s' "$W"
    elif [[ $FEC_MODE == sc-ldpc ]]; then
        printf '256'
    else
        printf '%s' "$((2 * N))"
    fi
}

expected_carriers() {
    if (( CARRIERS > 0 )); then
        printf '%s' "$CARRIERS"
    else
        printf '%s' "$((N + M))"
    fi
}

expected_up() {
    if [[ $ABI == v6 && $FEC == on ]]; then
        printf '%s' "$CARRIERS"
    else
        printf '%s' "$((N + M))"
    fi
}

module_srcversion() {
    local sv=""
    if command -v modinfo >/dev/null 2>&1; then
        sv=$(modinfo -F srcversion "$MODULE" 2>/dev/null || true)
        if [[ -z $sv ]]; then
            sv=$(modinfo "$MODULE" 2>/dev/null | sed -n 's/^srcversion:[[:space:]]*//p' | head -n 1)
        fi
    fi
    printf '%s' "$sv"
}

loaded_srcversion() { cat /sys/module/tbtun/srcversion 2>/dev/null || true; }

check_srcversion_pre() {
    if [[ -z $EXPECT_SRCVERSION ]]; then
        return 0
    fi
    local got
    got=$(module_srcversion)
    if [[ -z $got ]]; then
        note "cannot read module srcversion; post-insmod check only"
        return 0
    fi
    if [[ $got != "$EXPECT_SRCVERSION" ]]; then
        note "module srcversion '$got' != expected '$EXPECT_SRCVERSION'"
        return 1
    fi
    printf 'srcversion: %s (module matches)\n' "$got"
}

check_srcversion_post() {
    if [[ $MODE == dry-run ]]; then
        printf '[dry-run] srcversion post-check (expected %s)\n' "${EXPECT_SRCVERSION:-any}"
        return 0
    fi
    local got want
    got=$(loaded_srcversion)
    if [[ -z $got ]]; then
        note "loaded srcversion is missing"
        return 1
    fi
    if [[ -n $EXPECT_SRCVERSION && $got != "$EXPECT_SRCVERSION" ]]; then
        note "loaded srcversion '$got' != expected '$EXPECT_SRCVERSION'"
        return 1
    fi
    want=$(module_srcversion)
    if [[ -n $want && $got != "$want" ]]; then
        note "loaded srcversion '$got' != module '$want'"
        return 1
    fi
    printf 'srcversion: %s (loaded)\n' "$got"
}

check_ports_readback() {
    if [[ $MODE == dry-run ]]; then
        printf '[dry-run] ports readback: ports %s\n' "$PORTBLOCK"
        return 0
    fi
    local got
    got=$(awk '$1=="ports"{print $2; exit}' /proc/net/tbtun/ports 2>/dev/null || true)
    if [[ $got != "$PORTBLOCK" ]]; then
        note "ports readback '$got' != 'ports $PORTBLOCK'"
        return 1
    fi
    printf 'ports: %s (matches)\n' "$got"
}

# Compare the keys this script writes against the canonical render. Defaults
# the kernel adds for keys we did not write are allowed; every key we did
# write must be present with the canonical value (w and carriers resolved).
check_config_readback() {
    if [[ $MODE == dry-run ]]; then
        printf '[dry-run] config line: %s\n' "$(config_line)"
        return 0
    fi
    local got
    got=$(cat /proc/net/tbtun/config 2>/dev/null || true)
    if [[ -z $got ]]; then
        note "config readback is empty"
        return 1
    fi
    local -A want=()
    want[role]=far
    want[listen]="0.0.0.0:$CARRIER_PORT"
    want[target]="127.0.0.1:$TARGET_PORT"
    want[n]=$N
    want[m]=$M
    want[profile]=$PROFILE
    want[keepalive]=5000
    want[drip_ms]=1000
    want[carrier_stall_ms]=15000
    want[portblock]=$PORTBLOCK
    local nonce_exp=${NONCE,,}
    if (( ${#nonce_exp} == 16 )); then
        nonce_exp="0000000000000000$nonce_exp"
    fi
    want[nonce]=$nonce_exp
    if [[ $ABI == v6 ]]; then
        want[fec]=$FEC
        want[adaptive]=$ADAPTIVE
        want[mode]=$FEC_MODE
        want[w]=$(expected_w)
        want[carriers]=$(expected_carriers)
        want[crypto]=$CRYPTO
        want[crypto_key]=${CRYPTO_KEY,,}
    else
        want[fec]=off
        want[adaptive]=off
        want[mode]=rlnc
        want[w]=$((2 * N))
        want[carriers]=$((N + M))
        want[crypto]=off
        want[crypto_key]=-
        want[replay]=8388608
        want[tunnel_rate]=0
        want[max_inflate]=2.000
        want[hole_budget_ms]=30000
        want[rx_reorder_bytes]=262144
        want[tx_retain_bytes]=0
        if (( WINDOW_BYTES > 0 )); then
            want[tx_window_bytes]=$WINDOW_BYTES
        fi
    fi
    local tok key val seen=0
    for tok in $got; do
        [[ $tok == *=* ]] || continue
        key=${tok%%=*}
        val=${tok#*=}
        if [[ -n ${want[$key]+x} ]]; then
            if [[ ${want[$key]} != "$val" ]]; then
                note "config $key='$val' != expected '${want[$key]}'"
                return 1
            fi
            unset "want[$key]"
            seen=$((seen + 1))
        fi
    done
    if (( ${#want[@]} )); then
        note "config readback is missing keys: ${!want[*]}"
        return 1
    fi
    printf 'config: readback OK (%d tokens checked)\n' "$seen"
}

check_tunnels() {
    if [[ $MODE == dry-run ]]; then
        printf '[dry-run] tunnel 1 far listening 0.0.0.0:%s\n' "$CARRIER_PORT"
        return 0
    fi
    if ! grep -q "^1 far listening 0.0.0.0:$CARRIER_PORT " /proc/net/tbtun/tunnels 2>/dev/null; then
        note "tunnel 1 is not listening on 0.0.0.0:$CARRIER_PORT"
        return 1
    fi
    printf 'tunnel: far listening on 0.0.0.0:%s\n' "$CARRIER_PORT"
}

read_up() {
    awk '/^tunnel 1 c3 /{for(i=1;i<=NF;i++) if($i ~ /^up=/){sub("up=","",$i); print $i; exit}}' \
        /proc/net/tbtun/stats 2>/dev/null || true
}

wait_up() {
    local want up deadline
    want=$(expected_up)
    if [[ $MODE == dry-run ]]; then
        printf '[dry-run] wait up=%s (timeout %ss)\n' "$want" "$WAIT_UP"
        return 0
    fi
    up=$(read_up)
    printf 'carriers: up=%s expected=%s\n' "${up:-?}" "$want"
    if (( WAIT_UP <= 0 )); then
        if (( REQUIRE_UP == 1 )) && [[ ${up:-0} -lt $want ]]; then
            note "carrier-up check failed: up=${up:-0} expected=$want"
            return 1
        fi
        if [[ -z $up || ${up:-0} -eq 0 ]]; then
            note "no carriers are up yet (near side not joined?)"
        fi
        return 0
    fi
    deadline=$((SECONDS + WAIT_UP))
    while (( SECONDS < deadline )); do
        up=$(read_up)
        if [[ -n $up ]] && (( up >= want )); then
            printf 'carriers: up=%s/%s\n' "$up" "$want"
            return 0
        fi
        sleep 1
    done
    note "carrier-up timeout: up=${up:-?} expected=$want"
    return 1
}

target_unit() { printf 'tbtun-target-%s' "$TARGET_PORT"; }

target_command() {
    case $TARGET_KIND in
        none) return 0 ;;
        iperf3) printf 'iperf3 -s -B 127.0.0.1 -p %s' "$TARGET_PORT" ;;
        cmd) printf '%s' "$TARGET_CMD" ;;
        *) return 1 ;;
    esac
}

start_target() {
    local cmd unit
    cmd=$(target_command || true)
    if [[ -z $cmd ]]; then
        note "target: none (no target started)"
        return 0
    fi
    unit=$(target_unit)
    if [[ $MODE == dry-run ]]; then
        printf '[dry-run] systemd-run --unit=%s --collect /bin/sh -lc %q\n' "$unit" "$cmd"
        return 0
    fi
    systemctl stop "$unit.service" >/dev/null 2>&1 || true
    systemctl reset-failed "$unit.service" >/dev/null 2>&1 || true
    systemd-run --unit="$unit" --collect \
        --property=StandardOutput=file:"$STATE_DIR/target-$TARGET_PORT.log" \
        --property=StandardError=file:"$STATE_DIR/target-$TARGET_PORT.log" \
        /bin/sh -lc "$cmd" || return 1
    printf 'target: %s (unit %s)\n' "$cmd" "$unit"
}

stop_target() {
    local unit
    unit=$(target_unit)
    systemctl stop "$unit.service" 2>/dev/null || true
    systemctl reset-failed "$unit.service" 2>/dev/null || true
    # Backward-compatible cleanup for the old iperf3 unit name.
    systemctl stop "tbtun-iperf3-$TARGET_PORT.service" 2>/dev/null || true
    systemctl reset-failed "tbtun-iperf3-$TARGET_PORT.service" 2>/dev/null || true
}

print_plan() {
    normalize_args
    detect_services
    printf 'mode=%s cmd=%s abi=%s\n' "$MODE" "$CMD" "$ABI"
    if [[ $ABI == v6 ]]; then
        printf 'fec=%s adaptive=%s mode=%s n=%s m=%s w=%s carriers=%s crypto=%s crypto_key=%s profile=%s\n' \
            "$FEC" "$ADAPTIVE" "$FEC_MODE" "$N" "$M" "$W" "$CARRIERS" "$CRYPTO" "$CRYPTO_KEY" "$PROFILE"
    else
        printf 'n=%s m=%s max_carriers=%s profile=%s window_bytes=%s\n' \
            "$N" "$M" "$CARRIERS" "$PROFILE" "$WINDOW_BYTES"
    fi
    printf 'carrier_port=%s target_port=%s portblock=%s app_target_mbps=%s loss_ppm=%s\n' \
        "$CARRIER_PORT" "$TARGET_PORT" "$PORTBLOCK" "$APP_TARGET_MBPS" "$LOSS_PPM"
    printf 'target=%s cmd=%s wait_up=%s require_up=%s expect_srcversion=%s\n' \
        "$TARGET_KIND" "$(target_command || true)" "$WAIT_UP" "$REQUIRE_UP" "${EXPECT_SRCVERSION:-any}"
    printf 'module=%s state_dir=%s evidence_dir=%s lock=%s\n' \
        "$MODULE" "$STATE_DIR" "$EVIDENCE_DIR" "$LOCK_FILE"
    printf 'services=%s\n' "$PROD_SERVICES"
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
        printf 'ports_block=%s\n' "$(cat /proc/net/tbtun/ports 2>/dev/null | awk '$1=="ports"{print $2; exit}' || true)"
        printf 'up=%s expected=%s\n' "$(read_up)" "$(expected_up)"
    fi
    portblock_reserved_warn
    printf 'snapshots:\n'
    if [[ -L $STATE_DIR/latest ]]; then printf '  latest=%s\n' "$(readlink -f "$STATE_DIR/latest")"; else printf '  none\n'; fi
    printf 'lock:\n'
    if [[ -e $LOCK_FILE ]]; then printf '  %s present\n' "$LOCK_FILE"; else printf '  %s absent\n' "$LOCK_FILE"; fi
}

do_deploy() {
    validate_profile
    print_plan
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
        if [[ $MODE == dry-run ]]; then
            note "tbtun already loaded (dry-run continues)"
        else
            die "tbtun already loaded; run rollback first"
        fi
    fi
    check_ports_free
    snapshot
    if ! check_srcversion_pre; then
        return 1
    fi
    if [[ $ABI == v6 ]]; then
        note "v6: carriers=$CARRIERS n=$N m=$M -> groups=$((CARRIERS / (N + M))) groups, 1 share/carrier"
        note "v6: insmod max_carriers=$CARRIERS max_tunnels=1 (128 MiB budget, §3.3)"
        note "v6: adaptive=$ADAPTIVE (T5.2 fixed shape needs --adaptive off)"
        if (( CARRIERS > 16 )); then
            note "256-carrier deploy requires the FEC v6 branch; a module without fec= keys will reject the config line"
        fi
        if ! run insmod "$MODULE" max_carriers="$CARRIERS" max_tunnels=1; then
            note "insmod failed for $CARRIERS carriers; rollback with: $PROG rollback --execute"
            return 1
        fi
    else
        note "legacy ARQ: n+m=$((N + M)) <= 16; max_carriers=$CARRIERS; no fec=/mode=/carriers= keys are written"
        if ! run insmod "$MODULE" max_carriers="$CARRIERS"; then
            note "insmod failed for $CARRIERS carriers; rollback with: $PROG rollback --execute"
            return 1
        fi
    fi
    if [[ $MODE == execute ]]; then
        DEPLOY_ACTIVE=1
    fi
    if ! check_srcversion_post; then
        return 1
    fi
    if ! run_sh "printf 'ports %s\\n' '$PORTBLOCK' > /proc/net/tbtun/ports"; then
        note "ports block write failed; rollback with: $PROG rollback --execute"
        return 1
    fi
    if ! check_ports_readback; then
        return 1
    fi
    local line
    line=$(config_line)
    if ! run_sh "printf '%s\\n' '$line' > /proc/net/tbtun/config"; then
        note "config write failed; rollback with: $PROG rollback --execute"
        return 1
    fi
    if ! check_config_readback; then
        return 1
    fi
    if ! check_tunnels; then
        return 1
    fi
    if ! start_target; then
        note "target start failed; rollback with: $PROG rollback --execute"
        return 1
    fi
    if ! wait_up; then
        note "carrier-up check failed; rollback with: $PROG rollback --execute"
        return 1
    fi
    note "deploy requested; verify with: $PROG verify"
    DEPLOY_OK=1
}

do_rollback() {
    print_plan
    require_root
    take_lock
    if [[ $MODE == dry-run ]]; then
        printf '[dry-run] systemctl stop %s.service\n' "$(target_unit)"
        printf '[dry-run] systemctl stop tbtun-iperf3-%s.service\n' "$TARGET_PORT"
    else
        stop_target
    fi
    if module_loaded; then
        if ! run rmmod tbtun; then
            note "rmmod tbtun failed"
        fi
    else
        printf 'module: already absent\n'
    fi
    if ! run_sh "test ! -d /proc/net/tbtun"; then
        note "/proc/net/tbtun is still present"
    fi
    if ! run_sh "! ss -lntH | awk '\$4 ~ \":$CARRIER_PORT\$\" || \$4 ~ \":$TARGET_PORT\$\"' | grep -q ."; then
        note "a carrier or target port is still listening"
    fi
    local snap
    if snap=$(latest_snapshot); then
        printf 'restoring production services against snapshot: %s\n' "$snap"
        local s before now
        for s in $PROD_SERVICES; do
            before=$(snapshot_value "$snap" "$s")
            now=$(service_state "$s")
            printf '  %s: before=%s now=%s\n' "$s" "${before:-unknown}" "$now"
            if [[ $before == active && $now != active ]]; then
                if ! run systemctl start "$s"; then
                    note "failed to start $s"
                fi
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
        got=$(cat /proc/net/tbtun/ports 2>/dev/null | awk '$1=="ports"{print $2; exit}' || true)
        if [[ $got == "$PORTBLOCK" ]]; then
            printf 'ports_block=%s (matches)\n' "$got"
        else
            printf 'ports_block=%s (expected %s)\n' "${got:-missing}" "$PORTBLOCK"
            fail=1
        fi
        printf 'config=%s\n' "$(cat /proc/net/tbtun/config 2>/dev/null || true)"
        local up want
        up=$(read_up)
        want=$(expected_up)
        printf 'carriers: up=%s expected=%s\n' "${up:-?}" "$want"
        if [[ -z $up ]] || (( up < want )); then
            fail=1
        fi
    else
        printf 'module=absent\n'
        note "module absent: verify only checked rollback state; it does not prove a deploy"
    fi
    if [[ -d /proc/net/tbtun ]]; then printf 'proc=present\n'; else printf 'proc=absent\n'; fi
    portblock_reserved_warn
    local snap
    if snap=$(latest_snapshot); then
        printf 'snapshot=%s\n' "$snap"
        local s before now
        for s in $PROD_SERVICES; do
            before=$(snapshot_value "$snap" "$s")
            now=$(service_state "$s")
            printf 'service %s=%s (before=%s)\n' "$s" "$now" "${before:-unknown}"
            if [[ $before == active && $now != active ]]; then
                fail=1
            fi
        done
    else
        for s in $PROD_SERVICES; do
            local st
            st=$(service_state "$s")
            printf 'service %s=%s\n' "$s" "$st"
            [[ $st == active ]] || fail=1
        done
    fi
    for p in "$CARRIER_PORT" "$TARGET_PORT"; do
        if listening_on "$p"; then printf 'port %s=listening\n' "$p"; else printf 'port %s=free\n' "$p"; fi
    done
    if [[ $TARGET_KIND != none && $TARGET_CHECK == 1 ]]; then
        if ! listening_on "$TARGET_PORT"; then
            printf 'target %s=not-listening\n' "$TARGET_PORT"
            fail=1
        fi
    fi
    printf 'app_target_mbps=%s loss_ppm=%s (throughput is measured by T5.2, not here)\n' "$APP_TARGET_MBPS" "$LOSS_PPM"
    return "$fail"
}

on_exit() {
    local rc=$?
    trap - EXIT
    if [[ $MODE == execute && $DEPLOY_ACTIVE == 1 && $DEPLOY_OK != 1 ]]; then
        set +e
        note "deploy failed (rc=$rc); cleaning module/target"
        stop_target
        if module_loaded; then
            rmmod tbtun
        fi
        note "cleaned; production services were not restored; run: $PROG rollback --execute"
        rc=1
    fi
    release_lock
    exit "$rc"
}

while (($#)); do
    case $1 in
        status|deploy|rollback|verify) CMD=$1; shift ;;
        --dry-run) MODE=dry-run; shift ;;
        --execute) MODE=execute; shift ;;
        --abi) ABI=$2; shift 2 ;;
        --module) MODULE=$2; shift 2 ;;
        --expect-srcversion) EXPECT_SRCVERSION=$2; shift 2 ;;
        --fec) FEC=$2; shift 2 ;;
        --adaptive) ADAPTIVE=$2; ADAPTIVE_SET=1; shift 2 ;;
        --mode) FEC_MODE=$2; shift 2 ;;
        --allow-unimplemented) ALLOW_UNIMPLEMENTED=1; shift ;;
        --n) N=$2; shift 2 ;;
        --m) M=$2; M_SET=1; shift 2 ;;
        --spares) M=$2; M_SET=1; shift 2 ;;
        --w) W=$2; shift 2 ;;
        --carriers) CARRIERS=$2; CARRIERS_SET=1; shift 2 ;;
        --crypto) CRYPTO=$2; CRYPTO_SET=1; shift 2 ;;
        --crypto-key) CRYPTO_KEY=$2; shift 2 ;;
        --profile) PROFILE=$2; shift 2 ;;
        --http-kal) PROFILE=http-kal; shift ;;
        --raw) PROFILE=raw; shift ;;
        --nonce) NONCE=$2; shift 2 ;;
        --window-bytes) WINDOW_BYTES=$2; shift 2 ;;
        --carrier-port) CARRIER_PORT=$2; shift 2 ;;
        --target-port) TARGET_PORT=$2; shift 2 ;;
        --target-cmd) TARGET_KIND=cmd; TARGET_CMD=$2; shift 2 ;;
        --iperf3) TARGET_KIND=iperf3; shift ;;
        --no-target) TARGET_KIND=none; TARGET_CMD=; shift ;;
        --no-target-check) TARGET_CHECK=0; shift ;;
        --portblock) PORTBLOCK=$2; shift 2 ;;
        --wait-up) WAIT_UP=$2; shift 2 ;;
        --require-up) REQUIRE_UP=1; shift ;;
        --services) PROD_SERVICES=$2; shift 2 ;;
        --app-target-mbps) APP_TARGET_MBPS=$2; shift 2 ;;
        --loss-p) LOSS_PPM=$2; shift 2 ;;
        --evidence-dir) EVIDENCE_DIR=$2; shift 2 ;;
        --state-dir) STATE_DIR=$2; shift 2 ;;
        -h|--help) usage 0 ;;
        *) die "unknown argument: $1 (try --help)" ;;
    esac
done

trap on_exit EXIT
detect_services

case $CMD in
    status) do_status ;;
    deploy) do_deploy ;;
    rollback) do_rollback ;;
    verify) do_verify ;;
esac
