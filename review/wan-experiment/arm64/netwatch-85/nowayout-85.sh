#!/bin/sh
# nowayout-85.sh - optional dw_wdt.nowayout=1 cmdline step for .85.
#
# Current facts (2026-11-08):
#   CONFIG_WATCHDOG_NOWAYOUT=n, no dw_wdt.nowayout=1 on the cmdline, so
#   nowayout=0 at runtime. The driver advertises MAGICCLOSE (options 0x8180),
#   so a close() without the magic byte 'V' does NOT stop the watchdog. The
#   feeder never writes 'V' and never calls WDIOS_DISABLECARD, which is the
#   property we actually need. nowayout=1 is defense in depth: it also makes
#   'V' + close and WDIOS_DISABLECARD fail.
#
# The built-in driver does not expose /sys/module/dw_wdt, and fw_printenv
# cannot read the U-Boot environment on this board. The supported cmdline
# source is the uEnv file loaded by /boot/firmware/boot.scr:
#   /boot/firmware/ubuntuEnv.txt -> uEnv/uEnv-lubancat-5-v2.txt
# This script edits the bootargs= line of that file. It does NOT reboot.
#
# Modes
#   --status    (default) read-only: cmdline, uEnv line, sysfs, config
#   --prepare   back up the uEnv file and append dw_wdt.nowayout=1
#   --verify    after a reboot: check the running cmdline and sysfs
#   --rollback  restore the newest .bak-* backup of the uEnv file
#
# Requires a reboot to take effect. Keep a serial console available: a broken
# bootargs line can prevent the board from booting. The backup is the way
# back. Do not run this while the watchdog feeder is being installed unless
# a reboot window is approved.
set -eu

UENV_LINK=/boot/firmware/ubuntuEnv.txt
UENV=$(readlink -f "$UENV_LINK" 2>/dev/null || true)
MODE=status

usage() {
    cat <<'EOF'
usage: nowayout-85.sh [--status | --prepare | --verify | --rollback]

  --status    read-only report (default)
  --prepare   back up the uEnv file and append dw_wdt.nowayout=1 to bootargs
  --verify    after reboot: check /proc/cmdline and the module parameter
  --rollback  restore the newest uEnv .bak-* backup
  -h, --help  this text

This changes the bootloader-visible kernel cmdline and needs a reboot.
It does not reboot by itself.
EOF
}

for arg in "$@"; do
    case "$arg" in
        --status) MODE=status ;;
        --prepare) MODE=prepare ;;
        --verify) MODE=verify ;;
        --rollback) MODE=rollback ;;
        -h | --help) usage; exit 0 ;;
        *) printf 'nowayout-85: unknown argument: %s\n' "$arg" >&2; usage >&2; exit 2 ;;
    esac
done

say() { printf '%s\n' "$*"; }

cmdline_nowayout() {
    if grep -qE '(^| )dw_wdt\.nowayout=1( |$)' /proc/cmdline; then
        echo "1"
    elif grep -qE '(^| )dw_wdt\.nowayout=0( |$)' /proc/cmdline; then
        echo "0"
    else
        echo "absent"
    fi
}

sysfs_nowayout() {
    p=/sys/module/dw_wdt/parameters/nowayout
    if [ -r "$p" ]; then
        cat "$p"
    else
        echo "absent"
    fi
}

uEnv_bootargs() {
    [ -n "$UENV" ] && [ -f "$UENV" ] || return 1
    grep -m1 '^bootargs=' "$UENV" || true
}

if [ "$MODE" = status ]; then
    say "== nowayout status (read-only)"
    say "running cmdline dw_wdt.nowayout: $(cmdline_nowayout)"
    say "sysfs /sys/module/dw_wdt/parameters/nowayout: $(sysfs_nowayout)"
    say "uEnv link: $UENV_LINK"
    say "uEnv file: ${UENV:-unresolved}"
    say "uEnv bootargs: $(uEnv_bootargs || echo '(unreadable)')"
    cfg=/boot/config-$(uname -r)
    if [ -r "$cfg" ]; then
        say "kernel config: $(grep -E '^#? ?CONFIG_WATCHDOG_NOWAYOUT' "$cfg" | head -1)"
    else
        say "kernel config: $cfg unreadable"
    fi
    say
    say "MAGICCLOSE is advertised by dw_wdt (options 0x8180). The feeder never"
    say "writes 'V', so close() does not stop the watchdog even with nowayout=0."
    exit 0
fi

if [ "$MODE" = verify ]; then
    say "== nowayout verification (read-only)"
    say "running cmdline dw_wdt.nowayout: $(cmdline_nowayout)"
    say "sysfs nowayout: $(sysfs_nowayout)"
    if dmesg 2>/dev/null | grep -qi 'unknown kernel command line'; then
        say "dmesg reports unknown kernel command line parameters:"
        dmesg 2>/dev/null | grep -i 'unknown kernel command line' | tail -3
    fi
    case "$(cmdline_nowayout)" in
        1) say "RESULT: cmdline token present. If sysfs is absent, the driver" \
                "did not expose it; confirm MAGICCLOSE behavior in the feeder log." ;;
        0 | absent) say "RESULT: nowayout token NOT active. The edit did not take effect" \
                         "or was not applied." ;;
    esac
    exit 0
fi

[ "$(id -u)" -eq 0 ] || { say "must run as root"; exit 1; }
[ -n "$UENV" ] && [ -f "$UENV" ] || { say "cannot resolve/read $UENV_LINK"; exit 1; }

if [ "$MODE" = rollback ]; then
    BACKUP=$(ls -1t "$UENV".bak-* 2>/dev/null | head -1 || true)
    [ -n "$BACKUP" ] || { say "no $UENV.bak-* backup found"; exit 1; }
    say "== restore $BACKUP -> $UENV"
    cp -a "$BACKUP" "$UENV"
    say "restored. Reboot to apply. (No automatic reboot.)"
    exit 0
fi

# MODE=prepare
if uEnv_bootargs | grep -q 'dw_wdt\.nowayout=1'; then
    say "== already present: $(uEnv_bootargs)"
    say "nothing changed. Reboot is still required if the running cmdline lacks it."
    exit 0
fi

BACKUP="$UENV.bak-$(date +%Y%m%dT%H%M%S)"
cp -a "$UENV" "$BACKUP"
say "== backup: $BACKUP"

TMP="$UENV.tmp.$$"
awk '
    /^bootargs=/ && !done {
        print $0 " dw_wdt.nowayout=1"
        done = 1
        next
    }
    { print }
' "$UENV" > "$TMP"
[ -s "$TMP" ] || { rm -f "$TMP"; say "generated file is empty; aborting"; exit 1; }
grep -q '^bootargs=.*dw_wdt\.nowayout=1' "$TMP" || {
    rm -f "$TMP"
    say "generated file does not contain the token; aborting"
    exit 1
}
chmod 0644 "$TMP"
chown root:root "$TMP"
mv "$TMP" "$UENV"
say "== new line: $(uEnv_bootargs)"
say
say "Reboot required. Before rebooting, confirm the rest of bootargs is"
say "unchanged (diff against $BACKUP). If the board fails to boot, restore"
say "the backup from a rescue shell or by mounting the boot partition:"
say "  cp -a $BACKUP $UENV"
say "This script does not reboot."
