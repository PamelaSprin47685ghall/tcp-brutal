#!/usr/bin/env bash
# build_on_65.sh - build tbtun.ko for the arm64 near host from a local ref.
#
# Runs on the workstation (this checkout). Target: root@192.168.123.65, RK3588
# aarch64, Ubuntu 26.04, kernel 6.1.172, 8 cores. The script builds only; it
# never loads the module, never writes sysctl, routes, or services.
#
# Usage:
#   review/wan-experiment/arm64/build_on_65.sh <ref>
#
# <ref> is any commit-ish in this checkout. The tree is `git archive`d to
# /root/tbtun-t52-arm64-<short-sha>/ on .65 and built there with
#   make -C /lib/modules/$(uname -r)/build M=$PWD modules -j8
# The staged tree is kept: run_t52_65.sh takes the module and the host tools
# from it. Re-running the same ref replaces the tree.
#
# Preconditions, checked rather than assumed:
#   - root ssh to .65 with BatchMode succeeds
#   - .65 reports aarch64 and has /lib/modules/$(uname -r)/build
#   - .65 has make, cc, tar, flock, modinfo, sha256sum
#   - /tmp/tbtun-rk3588.lock is free (waits up to LOCK_WAIT seconds)
#   - .65 1-minute load average is <= LOAD_LIMIT
#
# Environment:
#   NEAR_HOST    ssh target for .65 (default root@192.168.123.65)
#   LOCK_WAIT    seconds to wait for the host lock (default 1800)
#   LOAD_LIMIT   load1 ceiling (default 4)
#   LOAD_SETTLE  seconds to wait for load to fall before refusing (default 0)
#   EVIDENCE_DIR evidence root (default <repo>/review/wan-experiment/arm64)
#
# Evidence: $EVIDENCE_DIR/build-<short-sha>-<UTC>/
#   build.log    make output from .65
#   summary.txt  identity, file(1), vermagic, srcversion, sha256
#
# Exit codes:
#   0 built and recorded
#   2 usage, not a git checkout, or bad ref
#   3 precondition failed (ssh, arch, kernel headers, tools)
#   4 .65 load above LOAD_LIMIT
#   5 lock timeout
#   6 build failed

set -euo pipefail

PROG=${0##*/}
NEAR_HOST=${NEAR_HOST:-root@192.168.123.65}
LOCK_FILE=/tmp/tbtun-rk3588.lock
LOCK_WAIT=${LOCK_WAIT:-1800}
LOCK_HOLD_SECS=${LOCK_HOLD_SECS:-14400}
LOAD_LIMIT=${LOAD_LIMIT:-4}
LOAD_SETTLE=${LOAD_SETTLE:-0}
EVIDENCE_ROOT=${EVIDENCE_DIR:-}
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=4)

EX_OK=0
EX_USAGE=2
EX_PRECOND=3
EX_BUSY=4
EX_LOCK=5
EX_BUILD=6

note() { printf '%s: %s\n' "$PROG" "$*" >&2; }
die() { local rc=$1; shift; printf '%s: %s\n' "$PROG" "$*" >&2; exit "$rc"; }
usage() { sed -n '2,41p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-$EX_OK}"; }

[[ $# -eq 1 ]] || usage "$EX_USAGE"
case $1 in -h | --help) usage "$EX_OK" ;; esac
REF=$1

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1 || die "$EX_USAGE" "not a git checkout: $REPO"
SHA=$(git -C "$REPO" rev-parse --verify --quiet "$REF^{commit}") || die "$EX_USAGE" "not a commit: $REF"
SHORT=$(git -C "$REPO" rev-parse --short=12 "$SHA")
STAGE=/root/tbtun-t52-arm64-$SHORT
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
EVIDENCE_ROOT=${EVIDENCE_ROOT:-$REPO/review/wan-experiment/arm64}
EV=$EVIDENCE_ROOT/build-$SHORT-$STAMP
mkdir -p "$EV"

rrun() { ssh "${SSH_OPTS[@]}" "$NEAR_HOST" "$@"; }

# --- lock -------------------------------------------------------------------
# The lock is held by an ssh session that keeps fd 9 flock(1)ed and then sleeps.
# It must not block on stdin: a script run with stdin on /dev/null would give
# the remote reader EOF at once and drop the lock. Killing the local ssh ends
# the remote session, which closes fd 9 and drops the lock; LOCK_HOLD_SECS is
# the fail-open bound if that path ever fails. The holder line is written into
# the lock file so a human can see who is running.
LOCK_PID=""
LOCK_TMP=""

lock_acquire() {
    local host=$1 lock=$2 purpose=$3 reply
    case $purpose in *"'"*) die "$EX_USAGE" "lock purpose must not contain a single quote" ;; esac
    LOCK_TMP=$(mktemp -d "${TMPDIR:-/tmp}/tbtun-lock.XXXXXX")
    mkfifo "$LOCK_TMP/fifo"
    ssh "${SSH_OPTS[@]}" "$host" \
        "exec 9>>'$lock' || { echo 'cannot open lock file' >&2; exit 3; }
         flock -w $LOCK_WAIT 9 || { echo 'lock busy (another holder)' >&2; exit 4; }
         printf '%s %s %s %s\n' \$\$ \"\$(date -Is)\" \"\$(id -un)\" '$purpose' > '$lock'
         echo LOCKED
         sleep $LOCK_HOLD_SECS 9>&-" >"$LOCK_TMP/fifo" 2>"$LOCK_TMP/err" &
    LOCK_PID=$!
    if ! read -r reply <"$LOCK_TMP/fifo"; then
        note "lock: cannot acquire $lock on $host: $(tr '\n' ' ' <"$LOCK_TMP/err" | cut -c1-300)"
        kill "$LOCK_PID" 2>/dev/null || true
        wait "$LOCK_PID" 2>/dev/null || true
        LOCK_PID=""
        rm -rf "$LOCK_TMP"
        LOCK_TMP=""
        return 1
    fi
    if [[ $reply != LOCKED ]]; then
        note "lock: unexpected reply from $host: $reply"
        kill "$LOCK_PID" 2>/dev/null || true
        wait "$LOCK_PID" 2>/dev/null || true
        LOCK_PID=""
        rm -rf "$LOCK_TMP"
        LOCK_TMP=""
        return 1
    fi
    note "lock: $lock held (holder line written)"
}

lock_release() {
    if [[ -n $LOCK_PID ]]; then
        kill "$LOCK_PID" 2>/dev/null || true
        wait "$LOCK_PID" 2>/dev/null || true
        LOCK_PID=""
    fi
    if [[ -n $LOCK_TMP ]]; then
        rm -rf "$LOCK_TMP"
        LOCK_TMP=""
    fi
}

# --- load -------------------------------------------------------------------
load1() { ssh "${SSH_OPTS[@]}" "$1" "cut -d' ' -f1 /proc/loadavg"; }

wait_load() { # host limit settle -> prints load1, returns 0; 3 on ssh failure; 4 when too high
    local host=$1 limit=$2 settle=$3 deadline l1
    deadline=$((SECONDS + settle))
    while :; do
        l1=$(load1 "$host") || return 3
        if awk -v a="$l1" -v b="$limit" 'BEGIN { exit !(a + 0 <= b + 0) }'; then
            printf '%s\n' "$l1"
            return 0
        fi
        if ((SECONDS >= deadline)); then
            note "load: $host load1=$l1 > $limit; refusing (wait for the bench to finish)"
            return 4
        fi
        note "load: $host load1=$l1 > $limit; waiting up to ${settle}s"
        sleep 5
    done
}

TMP=""
cleanup() {
    local rc=$?
    trap - EXIT INT TERM HUP
    set +e
    lock_release
    [[ -n $TMP ]] && rm -rf "$TMP"
    set -e
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# --- preconditions ----------------------------------------------------------
if ! rrun true >/dev/null 2>&1; then
    die "$EX_PRECOND" "ssh $NEAR_HOST failed (BatchMode root key required)"
fi
ARCH=$(rrun 'uname -m')
[[ $ARCH == aarch64 ]] || die "$EX_PRECOND" "$NEAR_HOST is $ARCH, not aarch64"
KREL=$(rrun 'uname -r')
if ! rrun "test -d /lib/modules/$KREL/build"; then
    die "$EX_PRECOND" "$NEAR_HOST has no /lib/modules/$KREL/build (install the matching kernel headers first)"
fi
MISSING=$(rrun 'for t in make cc tar flock modinfo sha256sum; do command -v "$t" >/dev/null 2>&1 || printf "%s " "$t"; done')
[[ -z $MISSING ]] || die "$EX_PRECOND" "$NEAR_HOST lacks: $MISSING"

# --- lock + load ------------------------------------------------------------
lock_acquire "$NEAR_HOST" "$LOCK_FILE" "t52-arm64 build $SHORT" || die "$EX_LOCK" "lock busy: $LOCK_FILE on $NEAR_HOST"
LOAD1=$(wait_load "$NEAR_HOST" "$LOAD_LIMIT" "$LOAD_SETTLE") || exit $?
note "load: $NEAR_HOST load1=$LOAD1 (limit $LOAD_LIMIT)"

# --- stage ------------------------------------------------------------------
TMP=$(mktemp -d "${TMPDIR:-/tmp}/tbtun-build.XXXXXX")
git -C "$REPO" archive --format=tar "$SHA" >"$TMP/src.tar"
SRC_SHA=$(sha256sum "$TMP/src.tar" | awk '{print $1}')
note "stage: $SHA -> $NEAR_HOST:$STAGE (archive sha256 $SRC_SHA)"
rrun "rm -rf '$STAGE' && mkdir -p '$STAGE'"
ssh "${SSH_OPTS[@]}" "$NEAR_HOST" "tar -x -C '$STAGE' -f -" <"$TMP/src.tar"
rrun "test -f '$STAGE/Makefile'" || die "$EX_PRECOND" "staged tree on $NEAR_HOST lacks Makefile"

# --- build ------------------------------------------------------------------
note "build: make -C /lib/modules/$KREL/build M=\$PWD modules -j8"
if ! rrun "cd '$STAGE' && rm -f Module.symvers && make -C /lib/modules/$KREL/build M=\$PWD modules -j8" >"$EV/build.log" 2>&1; then
    tail -n 40 "$EV/build.log" >&2
    die "$EX_BUILD" "build failed on $NEAR_HOST; log: $EV/build.log"
fi
rrun "test -s '$STAGE/tbtun.ko'" || die "$EX_BUILD" "build produced no $STAGE/tbtun.ko"

# --- record -----------------------------------------------------------------
{
    printf 'ref=%s\nsha=%s\nshort=%s\nhost=%s\nstage=%s\n' "$REF" "$SHA" "$SHORT" "$NEAR_HOST" "$STAGE"
    printf 'utc=%s\narchive_sha256=%s\nload1=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$SRC_SHA" "$LOAD1"
    printf '\n== uname ==\n'
    rrun 'uname -a'
    printf '\n== nproc ==\n'
    rrun 'nproc'
    printf '\n== cc ==\n'
    rrun 'cc --version | head -n 1'
    printf '\n== make ==\n'
    rrun 'make --version | head -n 1'
    printf '\n== ko ==\n'
    rrun "ls -l '$STAGE/tbtun.ko'"
    printf '\n== file ==\n'
    rrun "file '$STAGE/tbtun.ko' 2>/dev/null || echo 'file(1) missing'"
    printf '\n== vermagic ==\n'
    rrun "v=\$(modinfo -F vermagic '$STAGE/tbtun.ko' 2>/dev/null); [ -n \"\$v\" ] || v=\$(modinfo '$STAGE/tbtun.ko' | sed -n 's/^vermagic:[[:space:]]*//p'); printf '%s\n' \"\$v\""
    printf '\n== srcversion ==\n'
    rrun "v=\$(modinfo -F srcversion '$STAGE/tbtun.ko' 2>/dev/null); [ -n \"\$v\" ] || v=\$(modinfo '$STAGE/tbtun.ko' | sed -n 's/^srcversion:[[:space:]]*//p'); printf '%s\n' \"\$v\""
    printf '\n== sha256 ==\n'
    rrun "sha256sum '$STAGE/tbtun.ko'"
} >"$EV/summary.txt" 2>&1 || note "summary capture incomplete: $EV/summary.txt"

KO_SHA=$(rrun "sha256sum '$STAGE/tbtun.ko'" | awk '{print $1}')
printf '\nmodule: %s:%s\nsha256: %s\nevidence: %s\n' "$NEAR_HOST" "$STAGE/tbtun.ko" "$KO_SHA" "$EV"
printf 'next: review/wan-experiment/arm64/deploy_hnd0.sh %s -\n' "$SHA"
