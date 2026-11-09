#!/bin/sh
# watchdog-rollback-drill.sh - rehearse the hardware-watchdog exit without
# surprising the board.
#
# This script is a thin, read-only drill around the netwatch rollback script.
# It never opens /dev/watchdog0 (open() itself arms dw_wdt). It reads sysfs
# and /proc only. The default mode changes nothing.
#
# Contract, in one place:
#   * MAGICCLOSE=1: close() without the magic byte 'V' does not stop the
#     timer. Stopping the feeder starts the 44 s countdown. Stop is not off.
#   * There is no ioctl, magic byte, or sysfs knob that disarms an armed
#     dw_wdt here. Reboot is the only supported exit.
#   * nowayout=1 removes the last software exit: 'V'+close and
#     WDIOS_DISABLECARD fail. It does not arm the device by itself; it only
#     makes an armed device impossible to disarm. A dead feeder then means a
#     hard reset. Keep RuntimeWatchdogUSec=0 and no second opener.
#   * sysrq is not compiled (CONFIG_MAGIC_SYSRQ unset). Use systemctl reboot,
#     then reboot -f after 60 s if still up. Keep a serial console.
#   * Never run wdctl on an unarmed board. Never run this during a 1 GiB
#     transfer or a DKMS run.
#
# Modes:
#   (default)             collect evidence, run --status and both dry-runs
#   --verify              after a reboot: prove the unit is gone/unarmed
#   --execute-next-boot   disable the unit for the next boot; no reboot
#   --execute-now         run the rollback for real; the board reboots
#
# The rollback script is auto-detected as rollback-85.sh or rollback-84.sh
# next to this script, then under review/wan-experiment/arm64/netwatch-85
# or netwatch-84. Use --rollback to point elsewhere.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=
ROLLBACK=
BEFORE=
MODE=audit
YES=0
CONFIRM=0
EXPECT_NOWAYOUT=
FAILED=0

usage() {
    cat <<'EOF'
usage: watchdog-rollback-drill.sh [options]

  --rollback PATH        rollback script (default: auto-detect 85, then 84)
  --out DIR              evidence directory (default /var/tmp/tbt-watchdog-drill-<utc>)
  --verify               after reboot: check unit disabled, no holder, new boot
  --before DIR           evidence dir from the pre-reboot drill (for --verify)
  --execute-next-boot    disable the unit for the next boot; no reboot (needs --yes)
  --execute-now          run rollback now; the board reboots (needs --yes and
                         --i-know-this-reboots)
  --yes                  allow a real change
  --i-know-this-reboots  second confirmation for --execute-now
  --expect-nowayout 0|1  fail when the running nowayout differs
  -h, --help             this text

The default mode is read-only. It never stops the feeder and never opens
/dev/watchdog0.
EOF
}

die() { printf 'watchdog-drill: %s\n' "$*" >&2; exit 3; }

while [ "$#" -gt 0 ]; do
    case "$1" in
        --rollback) [ "$#" -ge 2 ] || die "--rollback needs a value"; ROLLBACK=$2; shift 2 ;;
        --out) [ "$#" -ge 2 ] || die "--out needs a value"; OUT=$2; shift 2 ;;
        --before) [ "$#" -ge 2 ] || die "--before needs a value"; BEFORE=$2; shift 2 ;;
        --verify) MODE=verify; shift ;;
        --execute-next-boot) MODE=next-boot; shift ;;
        --execute-now) MODE=now; shift ;;
        --yes) YES=1; shift ;;
        --i-know-this-reboots) CONFIRM=1; shift ;;
        --expect-nowayout) [ "$#" -ge 2 ] || die "--expect-nowayout needs a value"; EXPECT_NOWAYOUT=$2; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

[ "$(id -u)" -eq 0 ] || die "must run as root on the near board"
case "$EXPECT_NOWAYOUT" in
    '' | 0 | 1) ;;
    *) die "--expect-nowayout must be 0 or 1" ;;
esac

find_rollback() {
    for p in \
        "$HERE/rollback-85.sh" \
        "$HERE/rollback-84.sh" \
        "$HERE/../review/wan-experiment/arm64/netwatch-85/rollback-85.sh" \
        "$HERE/../review/wan-experiment/arm64/netwatch-84/rollback-84.sh"; do
        if [ -f "$p" ]; then
            printf '%s\n' "$p"
            return 0
        fi
    done
    return 1
}

if [ -z "$ROLLBACK" ]; then
    if ROLLBACK=$(find_rollback); then
        :
    else
        ROLLBACK=
    fi
fi

if [ -z "$OUT" ]; then
    OUT=/var/tmp/tbt-watchdog-drill-$(date -u +%Y%m%dT%H%M%SZ)
fi
mkdir -p "$OUT" || die "cannot create $OUT"

say() {
    printf '%s\n' "$*" | tee -a "$OUT/summary.txt"
}
pass() {
    detail=${2-}
    if [ -n "$detail" ]; then say "DRILL_CHECK $1 PASS ($detail)"; else say "DRILL_CHECK $1 PASS"; fi
}
fail() {
    detail=${2-}
    if [ -n "$detail" ]; then say "DRILL_CHECK $1 FAIL ($detail)"; else say "DRILL_CHECK $1 FAIL"; fi
    FAILED=$((FAILED + 1))
}
skip() {
    detail=${2-}
    if [ -n "$detail" ]; then say "DRILL_CHECK $1 SKIP ($detail)"; else say "DRILL_CHECK $1 SKIP"; fi
}
warn() { say "DRILL_WARN $*"; }

capture_cmd() {
    name=$1
    cmd=$2
    if ! sh -c "$cmd" >"$OUT/$name.txt" 2>&1; then
        printf '(command failed)\n' >>"$OUT/$name.txt"
    fi
    return 0
}

capture() {
    name=$1
    shift
    if ! "$@" >"$OUT/$name.txt" 2>&1; then
        printf '(command failed)\n' >>"$OUT/$name.txt"
    fi
    return 0
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

holds_dev() {
    pid=$1
    [ -n "$pid" ] && [ "$pid" != 0 ] || return 1
    [ -d "/proc/$pid/fd" ] || return 1
    ls -l "/proc/$pid/fd" 2>/dev/null | grep -qE '/dev/watchdog0?($| )'
}

unit_prop() {
    systemctl show -p "$1" --value "$2" 2>/dev/null || true
}

run_view() {
    tag=$1
    shift
    log="$OUT/rollback-$tag.log"
    set +e
    sh "$ROLLBACK" "$@" >"$log" 2>&1
    rc=$?
    set -e
    cat "$log"
    cat "$log" >>"$OUT/summary.txt"
    return "$rc"
}

say "== watchdog rollback drill"
say "time_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
say "out=$OUT"
say "rollback=${ROLLBACK:-not found}"
say
say "== contract"
say "  /dev/watchdog0 is not a read-only device: open() arms it."
say "  MAGICCLOSE=1: stopping the feeder starts the 44 s countdown."
say "  Reboot is the only supported exit; stop is not an off switch."
say "  nowayout=1 disables 'V'+close and WDIOS_DISABLECARD; a dead feeder"
say "  then means a hard reset. sysrq is not compiled. Use a serial console."
say

if [ "$MODE" = verify ]; then
    :
elif [ -z "$ROLLBACK" ]; then
    fail rollback_present "no rollback-85.sh/rollback-84.sh found; use --rollback"
else
    pass rollback_present "$ROLLBACK"
fi

if [ -c /dev/watchdog0 ]; then
    pass device_node "$(ls -l /dev/watchdog0 2>/dev/null | tr -s ' ')"
else
    fail device_node "/dev/watchdog0 is missing or not a character device"
fi

say "== evidence"
capture hostname hostname
capture uname uname -a
capture_cmd uptime 'cat /proc/uptime'
capture_cmd boot_id 'cat /proc/sys/kernel/random/boot_id'
capture_cmd cmdline 'cat /proc/cmdline'
capture systemd_watchdog systemctl show -p RuntimeWatchdogUSec --value
capture unit_netwatch systemctl show tbtun-netwatch.service
capture unit_hold systemctl show tbtun-wdt-hold.service
capture unit_files systemctl list-unit-files --no-pager --no-legend 'tbtun*'
capture_cmd process_list "pgrep -a -f 'tbtun-netwatch|tbtun-wdt-hold' || true"

: >"$OUT/watchdog-sysfs.txt"
for a in state timeout nowayout options identity bootstatus status timeleft pretimeout governor; do
    f=/sys/class/watchdog/watchdog0/$a
    if [ -r "$f" ]; then
        printf '%s: %s\n' "$a" "$(cat "$f")" >>"$OUT/watchdog-sysfs.txt"
    else
        printf '%s: (absent)\n' "$a" >>"$OUT/watchdog-sysfs.txt"
    fi
done
cat "$OUT/watchdog-sysfs.txt"

UENV_LINK=/boot/firmware/ubuntuEnv.txt
if [ -e "$UENV_LINK" ]; then
    UENV=$(readlink -f "$UENV_LINK" 2>/dev/null || true)
    say "uEnv=$UENV"
    if [ -n "$UENV" ] && [ -r "$UENV" ]; then
        grep -n '^bootargs=' "$UENV" | tee -a "$OUT/summary.txt" || true
    else
        warn "uEnv target is unreadable"
    fi
else
    warn "$UENV_LINK is absent"
fi

UNIT_ACTIVE=$(systemctl is-active tbtun-netwatch.service 2>/dev/null || true)
UNIT_ENABLED=$(systemctl is-enabled tbtun-netwatch.service 2>/dev/null || true)
MAIN_PID=$(unit_prop MainPID tbtun-netwatch.service)
case "$MAIN_PID" in '' | *[!0-9]*) MAIN_PID=0 ;; esac

say "== checks"
if systemctl cat tbtun-netwatch.service >/dev/null 2>&1; then
    pass unit_loaded
else
    skip unit_loaded "tbtun-netwatch.service is not installed"
fi

if [ "$UNIT_ACTIVE" = active ]; then
    if holds_dev "$MAIN_PID"; then
        pass feeder_holder "pid=$MAIN_PID holds /dev/watchdog0"
    else
        fail feeder_holder "unit active but pid=$MAIN_PID does not hold the device"
    fi
else
    skip feeder_holder "unit is ${UNIT_ACTIVE:-unknown}"
fi

RESTART=$(unit_prop Restart tbtun-netwatch.service)
if [ -z "$RESTART" ]; then
    skip restart_no "unit not loaded"
elif [ "$RESTART" = no ]; then
    pass restart_no
else
    fail restart_no "Restart=$RESTART; a restart would hide a dead feeder"
fi

RW=$(systemctl show -p RuntimeWatchdogUSec --value 2>/dev/null || true)
case "$RW" in
    0 | 0s | '') pass runtime_watchdog_usec "RuntimeWatchdogUSec=${RW:-unset}" ;;
    *) fail runtime_watchdog_usec "RuntimeWatchdogUSec=$RW; systemd would mask the gate" ;;
esac

CMDLINE=$(cat /proc/cmdline 2>/dev/null || true)
case " $CMDLINE " in
    *" dw_wdt.nowayout=1 "*) NW_CMDLINE=1 ;;
    *" dw_wdt.nowayout=0 "*) NW_CMDLINE=0 ;;
    *) NW_CMDLINE=absent ;;
esac
NW_SYSFS=$(cat /sys/module/dw_wdt/parameters/nowayout 2>/dev/null || echo absent)
say "DRILL_INFO nowayout cmdline=$NW_CMDLINE sysfs=$NW_SYSFS"
if [ -n "$EXPECT_NOWAYOUT" ]; then
    if [ "$NW_CMDLINE" = "$EXPECT_NOWAYOUT" ]; then
        pass nowayout_expected "$EXPECT_NOWAYOUT"
    else
        fail nowayout_expected "cmdline=$NW_CMDLINE expected=$EXPECT_NOWAYOUT"
    fi
fi
if [ "$NW_CMDLINE" = 1 ] || [ "$NW_SYSFS" = 1 ]; then
    warn "nowayout=1 is active: 'V'+close and WDIOS_DISABLECARD cannot disarm"
    warn "an armed device; a dead feeder is a hard reset. This does not arm"
    warn "the device by itself; RuntimeWatchdogUSec must stay 0."
fi

OPTS=$(cat /sys/class/watchdog/watchdog0/options 2>/dev/null || true)
case "$OPTS" in
    *0x8180* | *8180*) pass magicclose "options=$OPTS (MAGICCLOSE=1)" ;;
    '') skip magicclose "options attribute absent" ;;
    *) warn "options=$OPTS; deployment evidence expects 0x8180 (MAGICCLOSE=1)" ;;
esac

HOLD_STATE=$(systemctl is-enabled tbtun-wdt-hold.service 2>/dev/null || true)
HOLD_ACTIVE=$(systemctl is-active tbtun-wdt-hold.service 2>/dev/null || true)
case "$HOLD_STATE" in
    '' | not-found) ;;
    *)
        warn "tbtun-wdt-hold.service exists: enabled=$HOLD_STATE active=$HOLD_ACTIVE"
        warn "a --next-boot-only rollback must disable this unit too, or the"
        warn "next boot can be armed by a second feeder."
        ;;
esac

STATE=/var/lib/tbtun-netwatch
if [ -f "$STATE/armed-log" ]; then
    ARMED_N=$(wc -l <"$STATE/armed-log")
else
    ARMED_N=0
fi
BOOT_N=$(cat "$STATE/boot-count" 2>/dev/null || echo 0)
UP=$(awk '{print int($1)}' /proc/uptime 2>/dev/null || echo 0)
say "DRILL_INFO uptime=${UP}s armed_log=$ARMED_N boot_count=$BOOT_N"
if [ "$UP" -lt 600 ]; then
    say "DRILL_INFO boot grace active (<600 s): the prober is not judging yet"
fi

if [ "$MODE" = verify ]; then
    say "== verify after reboot"
    if [ -z "$BEFORE" ]; then
        for d in $(ls -1dt /var/tmp/tbt-watchdog-drill-* 2>/dev/null); do
            case "$d" in
                "$OUT") continue ;;
            esac
            if [ -f "$d/boot_id.txt" ]; then
                BEFORE=$d
                break
            fi
        done
    fi
    if [ -n "$BEFORE" ] && [ -d "$BEFORE" ]; then
        OLD_BOOT=$(cat "$BEFORE/boot_id.txt" 2>/dev/null || true)
        NEW_BOOT=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || true)
        if [ -n "$OLD_BOOT" ] && [ -n "$NEW_BOOT" ] && [ "$OLD_BOOT" != "$NEW_BOOT" ]; then
            pass fresh_boot "boot_id changed"
        else
            fail fresh_boot "boot_id did not change; this is the same boot"
        fi
        say "before=$BEFORE"
    else
        fail fresh_boot "no --before evidence directory"
    fi

    EN=$(systemctl is-enabled tbtun-netwatch.service 2>/dev/null || true)
    case "$EN" in
        disabled | masked | not-found) pass unit_disabled "$EN" ;;
        *) fail unit_disabled "is-enabled=$EN" ;;
    esac
    ACT=$(systemctl is-active tbtun-netwatch.service 2>/dev/null || true)
    case "$ACT" in
        active | activating | deactivating) fail unit_inactive "$ACT" ;;
        *) pass unit_inactive "${ACT:-unknown}" ;;
    esac
    H=$(holders)
    if [ -z "$H" ]; then
        pass no_holder
    else
        fail no_holder "$H"
    fi
    RW2=$(systemctl show -p RuntimeWatchdogUSec --value 2>/dev/null || true)
    case "$RW2" in
        0 | 0s | '') pass runtime_watchdog_usec_after ;;
        *) fail runtime_watchdog_usec_after "$RW2" ;;
    esac

    if [ "$FAILED" -eq 0 ]; then
        say "DRILL_RESULT PASS"
        exit 0
    fi
    say "DRILL_RESULT FAIL"
    exit 2
fi

if [ -n "$ROLLBACK" ]; then
    say "== 1. rollback --status (read-only)"
    if run_view status --status; then pass rollback_status; else fail rollback_status; fi

    say "== 2. next-boot-only dry-run"
    if run_view nextboot-dry --next-boot-only --dry-run; then pass nextboot_dry; else fail nextboot_dry; fi

    say "== 3. now dry-run"
    if run_view now-dry now --dry-run; then pass now_dry; else fail now_dry; fi
fi

if [ "$MODE" = next-boot ]; then
    [ "$YES" -eq 1 ] || die "--execute-next-boot requires --yes"
    [ -n "$ROLLBACK" ] || die "no rollback script"
    say "== 4. disable for the next boot (this boot stays armed)"
    if run_view nextboot-exec --next-boot-only; then
        pass nextboot_exec
    else
        fail nextboot_exec
    fi
fi

if [ "$MODE" = now ]; then
    [ "$YES" -eq 1 ] && [ "$CONFIRM" -eq 1 ] \
        || die "--execute-now requires --yes --i-know-this-reboots"
    [ -n "$ROLLBACK" ] || die "no rollback script"
    say "== 4. rollback now; handing control to the rollback script"
    say "DRILL_WARN the board will reboot; this shell may not return"
    run_view now-exec now || fail now_exec
    say "DRILL_RESULT REBOOT_REQUESTED_OR_UNARMED"
    if [ "$FAILED" -eq 0 ]; then
        exit 0
    fi
    exit 2
fi

if [ "$FAILED" -eq 0 ]; then
    say "DRILL_RESULT AUDIT_OK"
    exit 0
fi
say "DRILL_RESULT AUDIT_FAIL"
exit 2
