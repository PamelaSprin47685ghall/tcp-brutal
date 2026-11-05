#!/usr/bin/env bash
# deploy_hnd0.sh — tbtun far-side deploy / rollback driver for HND0.
#
# Target box: HND0 45.11.1.62, 2 vCPU / 955 MiB, physical 1G, 256-flow.
# Final production profile: 256 carriers grouped 32x8, app 500-750 Mbit/s.
# Current w-window module is capped at 16 carriers; a 256-carrier deploy is
# expected to fail at insmod until the FEC v6/grouped branch lands. This
# script never silently shrinks the request: a rejected insmod aborts and
# points at rollback.
#
# Invariants:
#   - Dry-run is the default. Nothing is loaded and no service is touched
#     unless --execute is given.
#   - Production services (frps, caddy, hysteria-server, xray) are never
#     stopped or restarted by deploy. Their state is snapshotted, and rollback
#     restarts only a service that was active before deploy and is inactive
#     now.
#   - The script owns only: the tbtun module, /proc/net/tbtun/config, the
#     transient unit tbtun-iperf3-<target-port>, and the two experiment ports.
#   - No routing or firewall change is made. Routes are recorded and compared,
#     never written.
#
# Usage:
#   ./deploy_hnd0.sh status
#   ./deploy_hnd0.sh deploy --execute --carriers 4 --spares 1
#   ./deploy_hnd0.sh rollback --execute
#   ./deploy_hnd0.sh verify
#
# Options (deploy):
#   --module PATH          tbtun.ko to load (default ./tbtun.ko)
#   --carriers N           active carriers (default 4; production 256)
#   --spares M             spare carriers (default 1)
#   --window-bytes W       tx_window_bytes (default 0 = module auto); the
#                          key name/unit must be confirmed against the fixed
#                          branch README before a non-zero value is used
#   --carrier-port P       far listen port (default 9000)
#   --target-port P        local target port (default 5201)
#   --app-target-mbps M    acceptance floor recorded for T5.2 (default 33.72)
#   --nonce HEX            32 hex digits (default fixed experiment nonce)
#   --state-dir DIR        snapshot directory (default /tmp/tbtun-hnd0)
#   --execute              actually run; without it everything is dry-run

set -euo pipefail

PROG=${0##*/}
CMD=status
MODE=dry-run

MODULE=./tbtun.ko
CARRIERS=4
SPARES=1
WINDOW_BYTES=0
CARRIER_PORT=9000
TARGET_PORT=5201
APP_TARGET_MBPS=33.72
NONCE=00112233445566778899aabbccddeeff
STATE_DIR=/tmp/tbtun-hnd0
PROD_SERVICES="frps caddy hysteria-server xray"

usage() {
    sed -n '2,50p' "$0" | sed 's/^# \{0,1\}//'
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

module_loaded() { lsmod | awk '$1=="tbtun"{found=1} END{exit !found}'; }

listening_on() {
    ss -lntH 2>/dev/null | awk -v p=":$1" '$4 ~ p"$" {found=1} END{exit !found}'
}

service_state() { systemctl is-active "$1" 2>/dev/null || true; }

snapshot() {
    local stamp file
    stamp=$(date +%Y%m%d-%H%M%S)
    file="$STATE_DIR/state-$stamp.txt"
    if [[ $MODE == dry-run ]]; then
        printf '[dry-run] snapshot -> %s\n' "$file"
        return 0
    fi
    mkdir -p "$STATE_DIR"
    {
        printf 'timestamp=%s\n' "$(date -Is)"
        printf 'kernel=%s\n' "$(uname -r)"
        printf 'nproc=%s\n' "$(nproc)"
        printf 'mem_total_kb=%s\n' "$(awk '/MemTotal/{print $2}' /proc/meminfo)"
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
    printf 'snapshot: %s\n' "$file"
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

print_plan() {
    printf 'mode=%s cmd=%s\n' "$MODE" "$CMD"
    printf 'carriers=%s spares=%s window_bytes=%s\n' "$CARRIERS" "$SPARES" "$WINDOW_BYTES"
    printf 'carrier_port=%s target_port=%s app_target_mbps=%s\n' "$CARRIER_PORT" "$TARGET_PORT" "$APP_TARGET_MBPS"
    printf 'module=%s state_dir=%s\n' "$MODULE" "$STATE_DIR"
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
    printf 'snapshots:\n'
    if [[ -L $STATE_DIR/latest ]]; then printf '  latest=%s\n' "$(readlink -f "$STATE_DIR/latest")"; else printf '  none\n'; fi
}

do_deploy() {
    print_plan
    require_root
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
    if [[ $CARRIERS -gt 16 ]]; then
        note "requesting $CARRIERS carriers; the w-window module caps at 16."
        note "insmod is expected to fail until the FEC v6/grouped branch lands."
        note "this script will abort and print the rollback command, not shrink the request."
    fi
    if ! run insmod "$MODULE" max_carriers="$CARRIERS"; then
        note "insmod failed for $CARRIERS carriers; rollback with: $PROG rollback --execute"
        return 1
    fi
    local line="tunnel 1 role=far listen=0.0.0.0:$CARRIER_PORT target=127.0.0.1:$TARGET_PORT n=$CARRIERS m=$SPARES profile=raw nonce=$NONCE portblock=31000-31099"
    if [[ $WINDOW_BYTES -gt 0 ]]; then
        line+=" tx_window_bytes=$WINDOW_BYTES"
    fi
    if ! run_sh "printf '%s\\n' '$line' > /proc/net/tbtun/config"; then
        note "config write failed; rollback with: $PROG rollback --execute"
        return 1
    fi
    run_sh "cat /proc/net/tbtun/config; cat /proc/net/tbtun/tunnels"
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
    if module_loaded; then printf 'module=loaded\n'; else printf 'module=absent\n'; fi
    if [[ -d /proc/net/tbtun ]]; then printf 'proc=present\n'; else printf 'proc=absent\n'; fi
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
    printf 'app_target_mbps=%s (throughput is measured by T5.2, not here)\n' "$APP_TARGET_MBPS"
    return "$fail"
}

while (($#)); do
    case $1 in
        status|deploy|rollback|verify) CMD=$1; shift ;;
        --dry-run) MODE=dry-run; shift ;;
        --execute) MODE=execute; shift ;;
        --module) MODULE=$2; shift 2 ;;
        --carriers) CARRIERS=$2; shift 2 ;;
        --spares) SPARES=$2; shift 2 ;;
        --window-bytes) WINDOW_BYTES=$2; shift 2 ;;
        --carrier-port) CARRIER_PORT=$2; shift 2 ;;
        --target-port) TARGET_PORT=$2; shift 2 ;;
        --app-target-mbps) APP_TARGET_MBPS=$2; shift 2 ;;
        --nonce) NONCE=$2; shift 2 ;;
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
