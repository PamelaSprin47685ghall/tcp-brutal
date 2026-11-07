#!/bin/sh
# rollback-65.sh - leave the gateway-gated hardware watchdog behind on .65.
#
# Read this before running it.
#
# dw_wdt on this board has no .set_options, MAGICCLOSE=0 and SETTIMEOUT=0.
# There is no ioctl, no magic byte and no sysfs knob that disarms an open
# watchdog. Stopping the feeder therefore does not switch the watchdog off;
# it lets the 44 s countdown run out and resets the board. `systemctl stop`
# is not an off switch, and this script never claims it is.
#
# Modes
# -----
#   --status           read-only report: who holds /dev/watchdog0, unit
#                      states, uptime, reset budget. Changes nothing.
#
#   --to-hold          hand the device back to tbtun-wdt-hold: start hold,
#                      confirm its fd, then stop the netwatch feeder. Aborts
#                      before stopping netwatch if hold cannot take over.
#
#   --next-boot-only   disable the unit, keep the current feeder running.
#                      This boot stays protected; the next boot is unarmed.
#                      No reset now. Use it mid-acceptance.
#
#   (default) now      disable the unit, sync, and reboot. The reboot is the
#                      only supported way to leave the watchdog behind; the
#                      hardware resets within 44 s even if the reboot path
#                      itself stalls, and `reboot -f` is the fallback after
#                      60 s. sysrq is not compiled into this kernel.
set -eu

MODE=now
DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --status) MODE=status ;;
        --to-hold) MODE=hold ;;
        --next-boot-only) MODE=next-boot-only ;;
        now | --now) MODE=now ;;
        -n | --dry-run) DRY_RUN=1 ;;
        -h | --help)
            sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            printf 'rollback-65: unknown argument: %s\n' "$arg" >&2
            exit 2
            ;;
    esac
done

UNIT=/etc/systemd/system/tbtun-netwatch.service
BIN=/usr/local/sbin/tbtun-netwatch.py
DOC=/usr/local/share/doc/tbtun-netwatch.md
DEV=/dev/watchdog0
LOG=/var/log/tbtun-watchdog.log
NEW=tbtun-netwatch.service
HOLD=tbtun-wdt-hold.service

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
unit_loaded() { [ "$(systemctl show -p LoadState --value "$1" 2>/dev/null || true)" = loaded ]; }
main_pid() { systemctl show -p MainPID --value "$1" 2>/dev/null || echo 0; }
holds_dev() {
    pid=$1
    [ -n "$pid" ] && [ "$pid" != 0 ] || return 1
    [ -d "/proc/$pid/fd" ] || return 1
    ls -l "/proc/$pid/fd" 2>/dev/null | grep -qF "$DEV"
}
holders() {
    for fddir in /proc/[0-9]*/fd; do
        pid=${fddir#/proc/}
        pid=${pid%/fd}
        if ls -l "$fddir" 2>/dev/null | grep -qF "$DEV"; then
            comm=$(cat "/proc/$pid/comm" 2>/dev/null || echo '?')
            printf 'pid=%s(%s) ' "$pid" "$comm"
        fi
    done
}
# Re-read MainPID each second: systemd may not have set it on the first poll,
# and the whole point is to confirm the *current* process owns the fd.
wait_unit_fd() {
    unit=$1
    limit=$2
    i=0
    while [ "$i" -lt "$limit" ]; do
        pid=$(main_pid "$unit")
        if holds_dev "$pid"; then
            printf '%s\n' "$pid"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

if [ "$MODE" = status ]; then
    say "== watchdog state (read-only)"
    say "device: $(ls -l "$DEV" 2>/dev/null || echo missing)"
    say "holders: $(holders)"
    say "$NEW: active=$(systemctl is-active "$NEW" 2>/dev/null || true) pid=$(main_pid "$NEW")"
    say "$HOLD: active=$(systemctl is-active "$HOLD" 2>/dev/null || true) pid=$(main_pid "$HOLD") loaded=$(systemctl show -p LoadState --value "$HOLD" 2>/dev/null || echo unknown)"
    say "RuntimeWatchdogUSec=$(systemctl show -p RuntimeWatchdogUSec --value 2>/dev/null || echo unknown)"
    say "uptime=$(cut -d' ' -f1 /proc/uptime) s"
    say "armed-log entries: $(wc -l < /var/lib/tbtun-netwatch/armed-log 2>/dev/null || echo 0)"
    say "boot-count: $(cat /var/lib/tbtun-netwatch/boot-count 2>/dev/null || echo 0)"
    say "-- log tail:"
    tail -n 8 "$LOG" 2>/dev/null || say "(no $LOG)"
    exit 0
fi

case "$MODE" in
next-boot-only)
    say "== 1. disable first, so a reset cannot re-arm us"
    run systemctl disable "$NEW" 2>/dev/null || true
    say "== 2. leaving the current feeder running"
    say "   active=$(systemctl is-active "$NEW" 2>/dev/null || true) pid=$(main_pid "$NEW")"
    say
    say "This boot stays protected; the next boot comes up unarmed."
    say "Files stay in place. Remove them once .65 has rebooted:"
    say "  rm -f $UNIT $BIN $DOC"
    say "  rm -rf /var/lib/tbtun-netwatch /var/log/tbtun-watchdog.log"
    say "  systemctl daemon-reload"
    exit 0
    ;;
hold)
    say "== 1. require the hold unit on this boot"
    if ! unit_loaded "$HOLD"; then
        say "   $HOLD is not loaded. It is transient and does not survive a"
        say "   reboot. Recreate it first (see netwatch-65/README.md section 10):"
        say "     systemd-run --unit=tbtun-wdt-hold --property=Restart=always \\"
        say "       /bin/sh -c 'while :; do printf 1 > $DEV; sleep 5; done'"
        exit 2
    fi

    # The device allows one opener, so hold cannot acquire the fd while
    # netwatch still holds it: it hits EBUSY on every attempt. The strongest
    # order the hardware permits is "hold already trying -> stop netwatch ->
    # confirm hold's fd inside the 44 s window -> on failure restart netwatch".
    say "== 2. start hold (it retries EBUSY until netwatch releases the fd)"
    run systemctl start "$HOLD" 2>/dev/null || true
    if [ "$DRY_RUN" -eq 1 ]; then
        say "   + systemctl stop $NEW"
        say "   + wait <=15 s for hold to hold $DEV"
        say "   + on failure: systemctl start $NEW"
        say "   + systemctl disable $NEW"
        say "dry-run complete; nothing was changed"
        exit 0
    fi
    sleep 1
    HOLD_PID=$(main_pid "$HOLD")
    if [ -z "$HOLD_PID" ] || [ "$HOLD_PID" = 0 ]; then
        say "   hold did not start; nothing changed"
        exit 3
    fi
    say "   hold pid=$HOLD_PID is running and retrying"

    say "== 3. stop the netwatch feeder (the 44 s window starts)"
    systemctl stop "$NEW" 2>/dev/null || true

    if ! HOLD_PID=$(wait_unit_fd "$HOLD" 15); then
        say "   hold did not acquire $DEV within 15 s; restarting $NEW"
        systemctl start "$NEW" 2>/dev/null || true
        if NEW_PID=$(wait_unit_fd "$NEW" 15); then
            say "   $NEW pid=$NEW_PID holds $DEV again; rollback aborted"
            exit 4
        fi
        say "   CRITICAL: no feeder holds $DEV; the board resets in <=44 s"
        exit 4
    fi
    say "   hold pid=$HOLD_PID holds $DEV"

    say "== 4. hold owns the fd; disabling $NEW"
    run systemctl disable "$NEW" 2>/dev/null || true
    say "   holders now: $(holders)"
    say
    say "Handover done: hold feeds this boot. $NEW is disabled."
    say "Files stay in place; remove them after the next reboot:"
    say "  rm -f $UNIT $BIN $DOC"
    say "  rm -rf /var/lib/tbtun-netwatch /var/log/tbtun-watchdog.log"
    say "  systemctl daemon-reload"
    exit 0
    ;;
now) ;;
esac

say "== 1. disable first, so a reset cannot re-arm us"
run systemctl disable "$NEW" 2>/dev/null || true

say "== 2. sync and reboot"
say "   MAGICCLOSE=0 means the feeder cannot be stopped without a reset."
say "   The reboot is the rollback. The 44 s hardware countdown is already"
say "   running only if the feeder stops before the reboot takes over."
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
