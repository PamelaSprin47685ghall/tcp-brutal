#!/bin/sh
# install-85.sh - install (and optionally arm) the .85 hardware watchdog feeder.
#
# Run on 192.168.123.85 as root. The payload must be next to this script:
#   tbtun-netwatch.py  tbtun-netwatch.service  netwatch-85.conf
#   tbtun-netwatch.logrotate
#
# .85 has no previous feeder: no process holds /dev/watchdog0, the bootloader
# does not arm it, and systemd does not feed it (RuntimeWatchdogUSec=0). The
# handover is therefore "no feeder -> this process". The only vulnerable
# interval is open() (which arms the device) to the first feed, milliseconds.
#
# Modes
#   --install-only (default)  copy payload and daemon-reload. No enable, no
#                             start. The device stays untouched.
#   --enable                  install, then enable for the next boot. No start.
#   --arm                     install, enable, start now, verify fd and first
#                             feed. This is the only mode that arms the device.
#   --dry-run                 print every action; only the read-only selftest
#                             runs (it probes the gateway 5 times, no device
#                             open).
#
# The selftest never opens /dev/watchdog0: on dw_wdt open() itself arms the
# watchdog, so a "probe" that opens it would be a write, not a read.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN=/usr/local/sbin/tbtun-netwatch.py
UNIT=/etc/systemd/system/tbtun-netwatch.service
CONF=/etc/tbtun-netwatch.conf
LOGROTATE=/etc/logrotate.d/tbtun-netwatch
DEV=/dev/watchdog0
LOG=/var/log/tbtun-watchdog.log
STATE=/var/lib/tbtun-netwatch
NEW=tbtun-netwatch.service
MODE=install-only
DRY_RUN=0

usage() {
    cat <<'EOF'
usage: install-85.sh [--install-only | --enable | --arm] [--dry-run]

  --install-only  copy payload, daemon-reload; no enable, no start (default)
  --enable        install, then enable the unit for the next boot; no start
  --arm           install, enable, start now, verify fd + first feed
  --dry-run       print every action; only the read-only selftest runs
  -h, --help      this text

Refuses to run when RuntimeWatchdogUSec is not 0, when another process
already holds /dev/watchdog0, or when the selftest fails.
EOF
}

for arg in "$@"; do
    case "$arg" in
        --install-only) MODE=install-only ;;
        --enable) MODE=enable ;;
        --arm) MODE=arm ;;
        -n | --dry-run) DRY_RUN=1 ;;
        -h | --help) usage; exit 0 ;;
        *) printf 'install-85: unknown argument: %s\n' "$arg" >&2; usage >&2; exit 2 ;;
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
die() { printf 'install-85: %s\n' "$*" >&2; exit 1; }

unit_active() { systemctl is-active --quiet "$1" 2>/dev/null; }
main_pid() { systemctl show -p MainPID --value "$1" 2>/dev/null || echo 0; }

# Match both device nodes: /dev/watchdog and /dev/watchdog0 refer to the same
# dw_wdt instance, and either fd keeps a second opener out with EBUSY.
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
wait_pid() {
    limit=$1
    i=0
    while [ "$i" -lt "$limit" ]; do
        pid=$(main_pid "$NEW")
        if [ -n "$pid" ] && [ "$pid" != 0 ]; then
            printf '%s\n' "$pid"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}
wait_fd() {
    pid=$1
    limit=$2
    i=0
    while [ "$i" -lt "$limit" ]; do
        if holds_dev "$pid"; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}
log_since() {
    base=$1
    [ -f "$LOG" ] || return 1
    tail -n "+$((base + 1))" "$LOG" 2>/dev/null
}

[ "$(id -u)" -eq 0 ] || die "must run as root"
[ -c "$DEV" ] || die "$DEV is missing or not a character device"
for f in tbtun-netwatch.py tbtun-netwatch.service netwatch-85.conf \
         tbtun-netwatch.logrotate; do
    [ -f "$HERE/$f" ] || die "missing payload file: $HERE/$f"
done

say "== 0. preconditions"
RW=$(systemctl show -p RuntimeWatchdogUSec --value 2>/dev/null || true)
case "$RW" in
    "" | 0 | 0s | off) say "   RuntimeWatchdogUSec=${RW:-unset} (systemd does not feed)" ;;
    *) die "RuntimeWatchdogUSec=$RW; systemd would feed the same device and mask the gate. Set it to 0 first." ;;
esac
say "   $DEV: $(ls -l "$DEV" 2>/dev/null | sed 's/  */ /g')"
say "   tbtun-sampler: $(systemctl is-active tbtun-sampler.service 2>/dev/null || true) (not touched)"
say "   tbtun-kmsg-relay: $(systemctl is-active tbtun-kmsg-relay.service 2>/dev/null || true) (not touched)"

EXISTING=$(holders)
PID=$(main_pid "$NEW")
if [ -n "$EXISTING" ]; then
    if unit_active "$NEW" && holds_dev "$PID"; then
        say "   $NEW already holds $DEV (pid=$PID)"
    else
        die "another process holds $DEV: $EXISTING; refusing to become a second opener"
    fi
else
    say "   no process holds $DEV (watchdog not armed)"
fi

say "== 1. selftest (read-only; never opens $DEV)"
SELFTEST_OUT=$(python3 "$HERE/tbtun-netwatch.py" --selftest 2>&1) || true
printf '%s\n' "$SELFTEST_OUT"
printf '%s\n' "$SELFTEST_OUT" | grep -q '^SELFTEST_RESULT PASS' \
    || die "selftest failed; nothing installed, device untouched"

say "== 2. install payload"
run install -D -m 0755 "$HERE/tbtun-netwatch.py" "$BIN"
run install -D -m 0644 "$HERE/tbtun-netwatch.service" "$UNIT"
run install -D -m 0644 "$HERE/netwatch-85.conf" "$CONF"
run install -D -m 0644 "$HERE/tbtun-netwatch.logrotate" "$LOGROTATE"
run install -d -m 0755 "$STATE"
run systemctl daemon-reload

if [ "$DRY_RUN" -eq 0 ]; then
    RESTART=$(systemctl show -p Restart --value "$NEW")
    [ "$RESTART" = "no" ] || die "unit Restart=$RESTART, expected no (a restart would hide a dead feeder)"
    say "   Restart=no"
fi

if [ "$MODE" = "install-only" ]; then
    say "== 3. installed; unit is NOT enabled and NOT started"
    say "   the device is untouched. Next step after approval:"
    say "   $0 --arm"
    if [ "$DRY_RUN" -eq 1 ]; then
        say "dry-run complete; nothing was changed"
    fi
    exit 0
fi

if [ "$MODE" = "enable" ]; then
    say "== 3. enable for the next boot"
    run systemctl enable "$NEW"
    say "   enabled; not started. The current boot stays unarmed."
    if [ "$DRY_RUN" -eq 1 ]; then
        say "dry-run complete; nothing was changed"
    fi
    exit 0
fi

# MODE=arm
if [ "$DRY_RUN" -eq 1 ]; then
    say "== 3. arm (dry-run: printed only)"
    say "   + systemctl enable $NEW"
    say "   + systemctl start $NEW"
    say "   + wait <=10 s for MainPID"
    say "   + wait <=20 s for /proc/<pid>/fd -> $DEV"
    say "   + wait <=10 s for 'first feed ok' in $LOG"
    say "   + observe 10 s, then require active + fd"
    say "dry-run complete; nothing was changed"
    exit 0
fi

say "== 3. enable"
systemctl enable "$NEW" >/dev/null

ALREADY_ACTIVE=0
if unit_active "$NEW"; then
    say "== 4. $NEW is already active; NOT restarting"
    say "   a restart would close the only fd. With MAGICCLOSE=1 and no 'V'"
    say "   write the hardware keeps counting, and nobody would feed it."
    PID=$(main_pid "$NEW")
    holds_dev "$PID" || die "$NEW active but pid=$PID does not hold $DEV"
    ALREADY_ACTIVE=1
    BASE=0
else
    if [ -f "$LOG" ]; then
        BASE=$(wc -l < "$LOG")
    else
        BASE=0
    fi
    say "== 4. start $NEW (this arms $DEV)"
    systemctl start "$NEW" || die "systemctl start failed; if open() succeeded the device is armed and counting down. Check 'journalctl -u $NEW' and $LOG immediately."
    PID=$(wait_pid 10) || { systemctl status "$NEW" --no-pager -l 2>&1 | tail -20; die "no MainPID; device not armed"; }
fi

say "== 5. verify fd (the device allows one opener)"
if ! wait_fd "$PID" 20; then
    say "   pid=$PID did not acquire $DEV within 20 s"
    if unit_active "$NEW"; then
        # open() never succeeded, so no countdown is running. Stopping is safe.
        say "   stopping $NEW; open() did not arm the device"
        systemctl stop "$NEW" 2>/dev/null || true
    fi
    log_since "$BASE" | tail -n 20 || true
    exit 4
fi
say "   pid=$PID holds $DEV"

say "== 6. verify first feed and capability report"
if [ "$ALREADY_ACTIVE" -eq 1 ]; then
    say "   already active: first feed was logged by the running process"
    EVIDENCE=$(tail -n 30 "$LOG" 2>/dev/null || true)
else
    i=0
    while [ "$i" -lt 10 ]; do
        if log_since "$BASE" | grep -qF 'first feed ok'; then
            break
        fi
        sleep 1
        i=$((i + 1))
    done
    if log_since "$BASE" | grep -qF 'first feed ok'; then
        say "   first feed ok"
    else
        say "   WARNING: no 'first feed ok' within 10 s; keeping the service running"
        say "   (stopping it would start the 44 s countdown). Inspect $LOG."
    fi
    EVIDENCE=$(log_since "$BASE" 2>/dev/null || true)
fi

printf '%s\n' "$EVIDENCE" | grep -E 'opened |bootstatus=' || true
TIMEOUT_LINE=$(printf '%s\n' "$EVIDENCE" | grep -m1 -o 'timeout=[0-9]*s' || true)
case "$TIMEOUT_LINE" in
    timeout=44s) say "   hardware timeout matches the design (44 s)" ;;
    "") say "   WARNING: could not read the hardware timeout from the log" ;;
    *) say "   WARNING: $TIMEOUT_LINE, expected timeout=44s; inspect before leaving unattended" ;;
esac

say "== 7. observe 10 s"
sleep 10
systemctl is-active --quiet "$NEW" || die "$NEW died within 10 s; the device may be unarmed or counting down"
holds_dev "$PID" || die "pid=$PID lost $DEV within 10 s"

say "   MainPID=$PID holds $DEV"
say "   Restart=$(systemctl show -p Restart --value "$NEW")"
say "   RuntimeWatchdogUSec=$(systemctl show -p RuntimeWatchdogUSec --value)"
UP=$(cut -d' ' -f1 /proc/uptime)
say "   uptime=${UP}s"
if awk "BEGIN{exit !($UP < 600)}"; then
    say "   boot grace active: no probe, no failure count until uptime 600 s"
else
    say "   uptime already past 600 s: probing starts now (first verdict at"
    say "   3 consecutive failures, 90 s, then <=44 s to reset)"
fi
say "-- last log lines:"
log_since "$BASE" | tail -n 12 || true

say
say "armed. Stop is not an off switch: use rollback-85.sh, which reboots."
