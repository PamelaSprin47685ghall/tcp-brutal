#!/bin/sh
# local-dstate-check.sh - read-only D-state and tbtun teardown check.
#
# The failure this looks for:
#   * a process in D state writing /proc/net/tbtun/config
#   * a process in D state running 'rmmod tbtun'
#   * /sys/module/tbtun/refcnt below zero
#   * the module loaded while /proc/net/tbtun is already gone
#
# That combination has no userspace cleanup. A second rmmod, a second config
# write, or kill -9 only adds more D-state tasks. The supported exit is a
# reboot after sync. This script never unloads, kills, writes /proc, or
# reboots. It only reads /proc, /sys, ps, dmesg and journal.
#
# --collect DIR writes evidence below DIR (the only write).
#
# Exit: 0 OK, 2 REBOOT_REQUIRED, 3 REVIEW, 1 error.
set -eu

OUT=
FAILED=0
TBT_SIG=0
D_FOUND=0
TBT_D=0

usage() {
    cat <<'EOF'
usage: local-dstate-check.sh [--collect DIR]

  --collect DIR   write ps/stack/dmesg evidence below DIR
  -h, --help      this text

Read-only. Exit 2 means the tbtun teardown is wedged and a reboot is the only
clean exit; exit 3 means D-state tasks exist outside the tbtun signature.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --collect) [ "$#" -ge 2 ] || { printf 'need a value for --collect\n' >&2; exit 3; }; OUT=$2; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; exit 3 ;;
    esac
done

if [ -n "$OUT" ]; then
    mkdir -p "$OUT" || { printf 'cannot create %s\n' "$OUT" >&2; exit 1; }
fi

TMP=$(mktemp -d 2>/dev/null) || { printf 'mktemp failed\n' >&2; exit 1; }
trap 'rm -rf "$TMP"' 0 1 2 15

say() {
    if [ -n "$OUT" ]; then
        printf '%s\n' "$*" | tee -a "$OUT/summary.txt"
    else
        printf '%s\n' "$*"
    fi
}
pass() {
    detail=${2-}
    if [ -n "$detail" ]; then say "DSTATE_CHECK $1 PASS ($detail)"; else say "DSTATE_CHECK $1 PASS"; fi
}
fail() {
    detail=${2-}
    if [ -n "$detail" ]; then say "DSTATE_CHECK $1 FAIL ($detail)"; else say "DSTATE_CHECK $1 FAIL"; fi
    FAILED=$((FAILED + 1))
}
skip() {
    detail=${2-}
    if [ -n "$detail" ]; then say "DSTATE_CHECK $1 SKIP ($detail)"; else say "DSTATE_CHECK $1 SKIP"; fi
}
info() { say "DSTATE_INFO $*"; }

say "== local D-state check"
say "time_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
say "host=$(hostname 2>/dev/null || echo unknown)"
say "kernel=$(uname -r)"
say "uptime_s=$(awk '{print int($1)}' /proc/uptime)"

ps -eo pid,ppid,stat,wchan:32,comm,args --sort=pid >"$TMP/ps.txt" 2>/dev/null \
    || { fail ps "ps failed"; exit 1; }
awk 'NR > 1 && $3 ~ /^D/ { print }' "$TMP/ps.txt" >"$TMP/dstate.txt"
D_FOUND=$(wc -l <"$TMP/dstate.txt")

: >"$TMP/d_pids"
if [ "$D_FOUND" -eq 0 ]; then
    pass dstate "no D-state tasks"
else
    fail dstate "$D_FOUND D-state task(s)"
fi

while read -r pid ppid stat wchan comm args; do
    [ -n "${pid:-}" ] || continue
    printf '%s\n' "$pid" >>"$TMP/d_pids"
    say "DSTATE_TASK pid=$pid ppid=$ppid stat=$stat wchan=$wchan comm=$comm args=$args"
    case "$args" in
        *"/proc/net/tbtun"* | *"rmmod tbtun"*)
            TBT_D=$((TBT_D + 1))
            TBT_SIG=1
            ;;
    esac
    case "$comm" in
        rmmod)
            case "$args" in
                *tbtun*) TBT_D=$((TBT_D + 1)); TBT_SIG=1 ;;
            esac
            ;;
    esac
done <"$TMP/dstate.txt"

if [ -d /sys/module/tbtun ]; then
    REF=$(cat /sys/module/tbtun/refcnt 2>/dev/null || echo unknown)
    HOLD=$(ls -A /sys/module/tbtun/holders 2>/dev/null || true)
    info "tbtun_loaded=yes refcnt=$REF holders=${HOLD:-none}"
    case "$REF" in
        -*) fail tbtun_refcnt "$REF (use count is corrupted)"; TBT_SIG=1 ;;
        unknown) skip tbtun_refcnt "unreadable" ;;
        0) pass tbtun_refcnt 0 ;;
        *) fail tbtun_refcnt "$REF (expected 0 for a clean module)"; TBT_SIG=1 ;;
    esac

    if [ -e /proc/net/tbtun ]; then
        pass tbtun_proc_entry
    else
        case "$REF" in
            0) skip tbtun_proc_entry "module loaded, refcnt 0, proc entry absent" ;;
            *) fail tbtun_proc_entry "module loaded, refcnt=$REF, /proc/net/tbtun absent"; TBT_SIG=1 ;;
        esac
    fi
else
    info "tbtun_loaded=no"
    pass tbtun_refcnt "not loaded"
fi

if [ -n "$OUT" ]; then
    cp "$TMP/ps.txt" "$OUT/ps.txt" 2>/dev/null || true
    cp "$TMP/dstate.txt" "$OUT/dstate.txt" 2>/dev/null || true
fi

: >"$TMP/stacks.txt"
if [ -s "$TMP/d_pids" ]; then
    while read -r pid; do
        printf '=== %s ===\n' "$pid" >>"$TMP/stacks.txt"
        if [ -r "/proc/$pid/stack" ] && cat "/proc/$pid/stack" >>"$TMP/stacks.txt" 2>/dev/null; then
            :
        else
            printf '(stack unreadable; run as root for kernel stacks)\n' >>"$TMP/stacks.txt"
        fi
    done <"$TMP/d_pids"
fi
if [ -n "$OUT" ]; then
    cp "$TMP/stacks.txt" "$OUT/stacks.txt" 2>/dev/null || true
fi

if [ -n "$OUT" ]; then
    {
        printf 'refcnt=%s\n' "$(cat /sys/module/tbtun/refcnt 2>/dev/null || echo absent)"
        printf 'holders=%s\n' "$(ls -A /sys/module/tbtun/holders 2>/dev/null || echo absent)"
        printf 'proc_net_tbtun=%s\n' "$([ -e /proc/net/tbtun ] && echo present || echo absent)"
    } >"$OUT/tbtun.txt" 2>/dev/null || true
fi

if dmesg >"$TMP/dmesg.txt" 2>/dev/null; then
    grep -E 'hung_task|tbtun|proc_entry_rundown' "$TMP/dmesg.txt" 2>/dev/null \
        | tail -n 20 >"$TMP/dmesg-hit.txt" || true
else
    : >"$TMP/dmesg-hit.txt"
    skip dmesg "restricted; try as root"
fi
if [ -s "$TMP/dmesg-hit.txt" ]; then
    say "DSTATE_DMESG_BEGIN"
    while read -r line; do say "DSTATE_DMESG $line"; done <"$TMP/dmesg-hit.txt"
    say "DSTATE_DMESG_END"
    if [ -n "$OUT" ]; then cp "$TMP/dmesg-hit.txt" "$OUT/dmesg.txt" 2>/dev/null || true; fi
fi

case "$TBT_SIG" in
    1) VERDICT=REBOOT_REQUIRED ;;
    0)
        if [ "$D_FOUND" -gt 0 ]; then
            VERDICT=REVIEW
        else
            VERDICT=OK
        fi
        ;;
esac

say "DSTATE_SUMMARY dstate=$D_FOUND tbtun_dstate=$TBT_D failed_checks=$FAILED"
say "DSTATE_VERDICT $VERDICT"

case "$VERDICT" in
    REBOOT_REQUIRED)
        say "DSTATE_REASON tbtun teardown is wedged; refcnt/proc/rmmod evidence above"
        say "DSTATE_NEXT sync; sync; sync"
        say "DSTATE_NEXT systemctl reboot"
        say "DSTATE_NEXT # if still up after 120 s, from a serial console: reboot -f"
        say "DSTATE_NEXT # do not run rmmod again, do not write /proc/net/tbtun/config"
        say "DSTATE_NEXT # kill -9 cannot reap a D-state task and adds no value"
        if [ -n "$OUT" ]; then
            say "DSTATE_EVIDENCE $OUT"
        fi
        exit 2
        ;;
    REVIEW)
        say "DSTATE_REASON D-state tasks exist outside the tbtun signature"
        say "DSTATE_NEXT inspect each DSTATE_TASK pid; storage/network owners"
        say "DSTATE_NEXT decide per subsystem; this script does not kill or reboot"
        if [ -n "$OUT" ]; then
            say "DSTATE_EVIDENCE $OUT"
        fi
        exit 3
        ;;
    *)
        say "DSTATE_NEXT none"
        if [ -n "$OUT" ]; then
            say "DSTATE_EVIDENCE $OUT"
        fi
        exit 0
        ;;
esac
