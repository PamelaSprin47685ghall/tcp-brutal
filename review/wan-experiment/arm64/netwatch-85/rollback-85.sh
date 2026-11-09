#!/bin/sh
# rollback-85.sh - leave the .85 gateway-gated hardware watchdog behind.
#
# Read this before running it.
#
# There is no ioctl, magic byte or sysfs knob in this deployment that turns an
# armed dw_wdt off. The feeder never writes 'V' and never closes the fd on
# purpose. When the process exits, the descriptor closes without 'V' and,
# because dw_wdt advertises MAGICCLOSE, the hardware keeps counting. Stopping
# the unit therefore starts the 44 s countdown; it is not an off switch.
#
# Modes
#   --status           read-only report: holder, unit state, uptime, budget,
#                      device capability evidence. Changes nothing.
#
#   --next-boot-only   disable the unit, keep the current feeder running.
#                      This boot stays protected; the next boot is unarmed.
#
#   (default) now      disable, sync, and reboot. The reboot is the only
#                      supported way to leave the watchdog behind. If the
#                      device is not armed and no feeder is active, no reboot
#                      is needed and the script exits without one.
#
#   --dry-run          print every action; change nothing.
set -eu

UNIT=/etc/systemd/system/tbtun-netwatch.service
BIN=/usr/local/sbin/tbtun-netwatch.py
CONF=/etc/tbtun-netwatch.conf
LOGROTATE=/etc/logrotate.d/tbtun-netwatch
DEV=/dev/watchdog0
LOG=/var/log/tbtun-watchdog.log
STATE=/var/lib/tbtun-netwatch
NEW=tbtun-netwatch.service
MODE=now
DRY_RUN=0

usage() {
    cat <<'EOF'
usage: rollback-85.sh [--status | --next-boot-only | now] [--dry-run]

  --status          read-only report; changes nothing
  --next-boot-only  disable the unit; keep this boot's feeder running
  now               disable, sync, reboot (default). No reboot if unarmed.
  --dry-run         print every action; change nothing
  -h, --help        this text
EOF
}

for arg in "$@"; do
    case "$arg" in
        --status) MODE=status ;;
        --next-boot-only) MODE=next-boot-only ;;
        now | --now) MODE=now ;;
        -n | --dry-run) DRY_RUN=1 ;;
        -h | --help) usage; exit 0 ;;
        *) printf 'rollback-85: unknown argument: %s\n' "$arg" >&2; usage >&2; exit 2 ;;
    esac
done

say() { printf '%s\n' "$*"; }
run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '   + %s\n' "$*"
    else
        "$@"
    fi
}

[ "$(id -u)" -eq 0 ] || { say "must run as root"; exit 1; }

unit_active() { systemctl is-active --quiet "$1" 2>/dev/null; }
main_pid() { systemctl show -p MainPID --value "$1" 2>/dev/null || echo 0; }
holds_dev() {
    pid=$1
    [ -n "$pid" ] && [ "$pid" != 0 ] || return 1
    [ -d "/proc/$pid/fd" ] || return 1
    ls -l "/proc/$pid/fd" 2>/dev/null | grep -qE '/dev/watchdog0?($| )'
}
holders() {
    for fddir in /proc/[0-9]*/fd; do
        pid=${fddir#/proc/}
        pid=${pid%/fd}
        if ls -l "$fddir" 2>/dev/null | grep -qE '/dev/watchdog0?($| )'; then
            comm=$(cat "/proc/$pid/comm" 2>/dev/null || echo '?')
            printf 'pid=%s(%s) ' "$pid" "$comm"
        fi
    done
}

if [ "$MODE" = status ]; then
    say "== watchdog state (read-only)"
    say "device: $(ls -l "$DEV" 2>/dev/null || echo missing)"
    say "holders: $(holders)"
    say "$NEW: active=$(systemctl is-active "$NEW" 2>/dev/null || true) pid=$(main_pid "$NEW")"
    say "RuntimeWatchdogUSec=$(systemctl show -p RuntimeWatchdogUSec --value 2>/dev/null || echo unknown)"
    say "uptime=$(cut -d' ' -f1 /proc/uptime) s"
    if [ -f "$STATE/armed-log" ]; then
        ARMED_N=$(wc -l < "$STATE/armed-log")
    else
        ARMED_N=0
    fi
    say "armed-log entries: $ARMED_N"
    say "boot-count: $(cat "$STATE/boot-count" 2>/dev/null || echo 0)"
    say "-- device capability evidence:"
    tail -n 3 "$STATE/device-info.log" 2>/dev/null || say "(no $STATE/device-info.log)"
    say "-- log tail:"
    tail -n 8 "$LOG" 2>/dev/null || say "(no $LOG)"
    exit 0
fi

if [ "$MODE" = next-boot-only ]; then
    say "== 1. disable first, so a reset cannot re-arm us"
    run systemctl disable "$NEW" 2>/dev/null || true
    say "== 2. leaving the current feeder running"
    say "   active=$(systemctl is-active "$NEW" 2>/dev/null || true) pid=$(main_pid "$NEW")"
    say
    say "This boot stays protected; the next boot comes up unarmed."
    say "Files stay in place. After the next reboot, collect evidence first,"
    say "then remove them:"
    say "  rm -f $UNIT $BIN $CONF $LOGROTATE"
    say "  rm -rf $STATE $LOG"
    say "  systemctl daemon-reload"
    if [ "$DRY_RUN" -eq 1 ]; then
        say "dry-run complete; nothing was changed"
    fi
    exit 0
fi

# MODE=now
say "== 1. disable first, so a reset cannot re-arm us"
run systemctl disable "$NEW" 2>/dev/null || true

ARMED=0
if unit_active "$NEW"; then
    ARMED=1
fi
if [ -n "$(holders)" ]; then
    ARMED=1
fi

if [ "$ARMED" -eq 0 ]; then
    say "== 2. device is not armed (no active feeder, no holder)"
    say "   no reboot is needed. Files are disabled and stay in place."
    if [ "$DRY_RUN" -eq 1 ]; then
        say "dry-run complete; nothing was changed"
    fi
    exit 0
fi

say "== 2. sync and reboot"
say "   MAGICCLOSE=1 means the feeder cannot be stopped without a reset:"
say "   a plain close without 'V' leaves the hardware counting."
say "   The reboot is the rollback."
if [ "$DRY_RUN" -eq 1 ]; then
    say "   + sync"
    say "   + systemctl reboot --no-block"
    say "   + (60 s later, if still up) reboot -f"
    say "dry-run complete; nothing was changed"
    exit 0
fi

sync
systemctl reboot --no-block 2>/dev/null || true
say "   reboot requested; waiting 60 s for it to take effect"
say "   if the board is still up after 60 s, this script runs 'reboot -f'."
say "   sysrq is not compiled (CONFIG_MAGIC_SYSRQ unset); do not use it."
sleep 60
say "   still up after 60 s: forcing reboot"
reboot -f
