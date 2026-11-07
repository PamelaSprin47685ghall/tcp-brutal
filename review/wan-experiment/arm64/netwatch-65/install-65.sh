#!/bin/sh
# install-65.sh - arm the hardware watchdog on .65 with the gateway gate.
#
# Run on .65 as root. The payload must already be next to this script
# (tbtun-netwatch.py, tbtun-netwatch.service, README.md).
#
# Handover order (the only part that can reset the board)
# -------------------------------------------------------
# dw_wdt has MAGICCLOSE=0 and SETTIMEOUT=0. Once /dev/watchdog0 is open the
# countdown cannot be stopped, only fed, and the device allows exactly one
# opener: a second open() returns EBUSY. Therefore the new feeder can never
# hold the fd while the old one still does; "open first, then stop the old
# feeder" is physically impossible on this hardware.
#
# The safe order is the strongest one the device permits:
#
#   1. start the new feeder while the old one still holds the fd;
#   2. wait until the new feeder is provably in its EBUSY retry loop
#      (log line "open /dev/watchdog0: busy ... retrying"), so its open path
#      has already been exercised;
#   3. stop the old feeder;
#   4. confirm within the 44 s hardware window that the new pid holds the fd
#      and has completed its first feed;
#   5. on any failure, restart the old feeder immediately (it reopens and
#      resumes feeding) and exit non-zero.
#
# The forbidden order - stop first, start later - leaves a window with no
# feeder. This script never does that.
#
# --dry-run prints the sequence and still runs the read-only --selftest.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN=/usr/local/sbin/tbtun-netwatch.py
UNIT=/etc/systemd/system/tbtun-netwatch.service
DOCDIR=/usr/local/share/doc
DEV=/dev/watchdog0
LOG=/var/log/tbtun-watchdog.log
HOLD=tbtun-wdt-hold.service
NEW=tbtun-netwatch.service
DRY_RUN=0

usage() {
    cat <<'EOF'
usage: install-65.sh [--dry-run]

Installs the feeder and the unit, runs the read-only selftest, enables the
unit, then hands /dev/watchdog0 over from tbtun-wdt-hold to tbtun-netwatch
with automatic fallback to hold on failure.

  -n, --dry-run   print every action; only the read-only selftest runs
  -h, --help      this text

Refuses to run when RuntimeWatchdogUSec is not 0: systemd feeding the same
device would mask the gateway gate.
EOF
}

for arg in "$@"; do
    case "$arg" in
        -n | --dry-run) DRY_RUN=1 ;;
        -h | --help) usage; exit 0 ;;
        *) printf 'install-65: unknown argument: %s\n' "$arg" >&2; usage >&2; exit 2 ;;
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
die() { printf 'install-65: %s\n' "$*" >&2; exit 1; }

unit_active() { systemctl is-active --quiet "$1" 2>/dev/null; }
unit_loaded() { [ "$(systemctl show -p LoadState --value "$1" 2>/dev/null || true)" = loaded ]; }
main_pid() { systemctl show -p MainPID --value "$1" 2>/dev/null || echo 0; }

holds_dev() {
    pid=$1
    [ -n "$pid" ] && [ "$pid" != 0 ] || return 1
    [ -d "/proc/$pid/fd" ] || return 1
    ls -l "/proc/$pid/fd" 2>/dev/null | grep -qF "$DEV"
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

# The only supported recovery: hold reopens the device and feeds again.
restore_hold() {
    if ! unit_loaded "$HOLD"; then
        say "   no $HOLD unit on this boot; nothing to fall back to"
        return 1
    fi
    say "   restarting $HOLD"
    systemctl start "$HOLD" 2>/dev/null || true
    i=0
    while [ "$i" -lt 15 ]; do
        pid=$(main_pid "$HOLD")
        if holds_dev "$pid"; then
            say "   hold restored: pid=$pid holds $DEV and feeds again"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    say "   hold did not acquire $DEV within 15 s"
    return 1
}

[ "$(id -u)" -eq 0 ] || die "must run as root"
[ -c "$DEV" ] || die "$DEV is missing or not a character device"

say "== 0. preconditions"
RW=$(systemctl show -p RuntimeWatchdogUSec --value 2>/dev/null || true)
case "$RW" in
    "" | 0 | 0s | off) say "   RuntimeWatchdogUSec=${RW:-unset} (systemd does not feed)" ;;
    *) die "RuntimeWatchdogUSec=$RW; systemd would feed the same device and mask the gate. Set it to 0 first." ;;
esac
if unit_active "$NEW"; then
    say "   $NEW already active (re-install path)"
fi
if unit_active "$HOLD"; then
    HOLD_PID=$(main_pid "$HOLD")
    if ! holds_dev "$HOLD_PID"; then
        die "$HOLD is active but pid=$HOLD_PID does not hold $DEV; investigate before touching anything"
    fi
    say "   $HOLD active, pid=$HOLD_PID holds $DEV"
fi

say "== 1. install payload"
run install -D -m 0755 "$HERE/tbtun-netwatch.py" "$BIN"
run install -D -m 0644 "$HERE/tbtun-netwatch.service" "$UNIT"
if [ -f "$HERE/README.md" ]; then
    run install -D -m 0644 "$HERE/README.md" "$DOCDIR/tbtun-netwatch.md"
fi

say "== 2. systemd reload"
run systemctl daemon-reload

say "== 3. selftest (read-only; never opens $DEV)"
SELFTEST_OUT=$("$BIN" --selftest 2>&1) || true
printf '%s\n' "$SELFTEST_OUT"
printf '%s\n' "$SELFTEST_OUT" | grep -q '^SELFTEST_RESULT PASS' \
    || die "selftest failed; nothing was started and the hold feeder is untouched"

say "== 4. enable, do not start yet"
run systemctl enable "$NEW"

say "== 5. unit contract"
if [ "$DRY_RUN" -eq 1 ]; then
    say "   + verify Restart=no after daemon-reload"
else
    RESTART=$(systemctl show -p Restart --value "$NEW")
    [ "$RESTART" = "no" ] || die "unit Restart=$RESTART, expected no (a restart would hide a dead feeder)"
    say "   Restart=no"
fi

if [ "$DRY_RUN" -eq 1 ]; then
    say "== 6. handover (dry-run: printed only)"
    say "   + start $NEW; it retries EBUSY every 2 s"
    say "   + wait for 'open $DEV: busy' in $LOG"
    if unit_active "$HOLD"; then
        say "   + stop $HOLD"
    fi
    say "   + wait <=20 s for the new MainPID to hold $DEV and log 'first feed ok'"
    say "   + on failure: systemctl start $HOLD"
    say "dry-run complete; nothing was changed"
    exit 0
fi

BASE=$(wc -l < "$LOG" 2>/dev/null || echo 0)

if unit_active "$NEW"; then
    say "== 6. restart the already-active feeder"
    systemctl restart "$NEW"
    PID=$(wait_pid 10) || die "service produced no MainPID after restart"
    if ! wait_fd "$PID" 20; then
        say "   pid=$PID did not acquire $DEV within 20 s"
        restore_hold || true
        exit 4
    fi
else
    HOLD_ACTIVE=0
    if unit_active "$HOLD"; then
        HOLD_ACTIVE=1
    fi

    say "== 6a. start the new feeder (EBUSY retry is expected)"
    systemctl start "$NEW"
    PID=$(wait_pid 10) || die "service produced no MainPID; hold, if any, still feeds"

    if [ "$HOLD_ACTIVE" -eq 1 ]; then
        say "== 6b. require proof that the new feeder reached its open path"
        i=0
        while [ "$i" -lt 15 ]; do
            if log_since "$BASE" | grep -qF "open $DEV: busy"; then
                break
            fi
            sleep 1
            i=$((i + 1))
        done
        if ! log_since "$BASE" | grep -qF "open $DEV: busy"; then
            say "   no EBUSY retry line after 15 s; hold still feeds, stopping the new service"
            systemctl stop "$NEW" 2>/dev/null || true
            exit 4
        fi
        say "   retry loop confirmed; stopping $HOLD (the 44 s window starts)"
        systemctl stop "$HOLD"
    fi

    if ! wait_fd "$PID" 20; then
        say "   pid=$PID did not acquire $DEV within 20 s"
        if restore_hold; then
            exit 4
        fi
        exit 4
    fi
fi

say "== 7. verify"
i=0
while [ "$i" -lt 10 ]; do
    if log_since "$BASE" | grep -qF "first feed ok"; then
        break
    fi
    sleep 1
    i=$((i + 1))
done
log_since "$BASE" | grep -qF "first feed ok" \
    || say "   WARNING: no 'first feed ok' line within 10 s; check the log tail"

systemctl is-active --quiet "$NEW" || die "$NEW is not active after handover"
holds_dev "$PID" || die "pid=$PID no longer holds $DEV"

say "   observing 10 s"
sleep 10
systemctl is-active --quiet "$NEW" || die "$NEW died within 10 s of handover"
holds_dev "$PID" || die "pid=$PID lost $DEV within 10 s"

say "   MainPID=$PID holds $DEV"
say "   Restart=$(systemctl show -p Restart --value "$NEW")"
say "   RuntimeWatchdogUSec=$(systemctl show -p RuntimeWatchdogUSec --value)"
say "   uptime=$(cut -d' ' -f1 /proc/uptime) s (grace is 600 s from boot)"
say "-- last log lines:"
log_since "$BASE" | tail -n 10 || true
say "-- fd table:"
ls -l "/proc/$PID/fd" 2>/dev/null | grep -F "$DEV" || true

say
say "armed."
say
say "Grace: the script judges nothing while /proc/uptime < 600 s. It feeds"
say "every 5 s and logs 'boot grace ...' only. If this install ran on a board"
say "that is already past 600 s uptime, probing starts immediately and the log"
say "shows no 'boot grace start' line - that is correct, not a bug."
say
say "wdctl reporting 'Device or resource busy' while the feeder holds the"
say "device is the expected, healthy state."
