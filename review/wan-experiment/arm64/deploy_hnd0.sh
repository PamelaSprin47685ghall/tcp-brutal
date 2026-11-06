#!/usr/bin/env bash
# deploy_hnd0.sh - bring the T5.2 far side up on HND0 (x86_64, 45.11.1.62).
#
# Runs on the workstation. Drives HND0 over ssh/scp. Never touches the near
# host and never touches HND0 production services (frps, caddy, hysteria,
# xray): their state is recorded before and after and never written.
#
# Usage:
#   review/wan-experiment/arm64/deploy_hnd0.sh <src-ref> <ko>
#   review/wan-experiment/arm64/deploy_hnd0.sh teardown
#   review/wan-experiment/arm64/deploy_hnd0.sh status
#
# <src-ref>  git commit-ish to stage on HND0. Use the same revision as the near
#            side: the short-packet epoch and the http-kal profile require both
#            ends to run one revision.
# <ko>       tbtun.ko for HND0's kernel, or `-` to build it on HND0 from the
#            staged tree (the far kernel is 7.0.0-42-generic; a module built
#            anywhere else fails the vermagic check below).
#
# What deploy does, in order:
#   1. takes /tmp/tbtun-hnd0.lock (holder line written) and refuses above load1 4
#   2. records production service state, refuses if tbtun is loaded or if
#      9000/5202 already listen
#   3. stages the tree at /home/tbtun-t52-arm64/ and the module there
#   4. builds tbt_echo, tbt_burst, tbt_ping with HND0's own cc
#   5. insmod tbtun.ko max_carriers=256 max_tunnels=1 debug=0
#   6. writes /proc/net/tbtun/ports = ports 31000-31511
#   7. writes the far config line (w=0; the kernel renders w=8 for n=4) and
#      checks the rendered config token by token
#   8. starts the echo target as transient unit tbt-echo-5202 on 127.0.0.1:5202
#   9. optionally waits for up=256 (--wait-up SECS; needs the near side joining)
#
# On success the far side stays up for run_t52_65.sh, which tears both ends
# down. On any failure, and on INT/TERM, the trap removes the module and stops
# the echo unit. If no run follows, finish with `deploy_hnd0.sh teardown`.
#
# Config source (verbatim, latest evidence):
#   review/wan-experiment/t5.2-256-http-kal/
#     t52-256-rerun-42ed56f-20261106T092730Z/far.cfg
#   The older stop report (09:05Z) carried rx_reorder_bytes=1048576; the
#   42ed56f rerun carries 8388608 and wins.
#
# Environment:
#   FAR_HOST      ssh target (default root@45.11.1.62)
#   FAR_DIR       stage directory (default /home/tbtun-t52-arm64)
#   LOCK_WAIT     seconds to wait for the lock (default 1800)
#   LOAD_LIMIT    load1 ceiling (default 4)
#   LOAD_SETTLE   seconds to wait for load to fall before refusing (default 0)
#   EVIDENCE_DIR  evidence root (default <repo>/review/wan-experiment/arm64)
#
# Evidence: $EVIDENCE_DIR/deploy-hnd0-<UTC>/
#   services-before.txt, services-after.txt, far-build.log, host-tools.log,
#   config.txt (rendered), tunnels.txt, ports.txt, module.txt, summary.txt
#
# Exit codes:
#   0 deployed (or torn down)
#   2 usage or bad ref
#   3 precondition failed (ssh, ports, module loaded, vermagic, tools)
#   4 HND0 load above LOAD_LIMIT
#   5 lock timeout
#   6 module build failed
#   7 insmod, ports, config, or echo unit failed
#   8 up=256 gate failed (--wait-up only)

set -euo pipefail

PROG=${0##*/}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
FAR_DIR=${FAR_DIR:-/home/tbtun-t52-arm64}
LOCK_FILE=/tmp/tbtun-hnd0.lock
LOCK_WAIT=${LOCK_WAIT:-1800}
LOCK_HOLD_SECS=${LOCK_HOLD_SECS:-14400}
LOAD_LIMIT=${LOAD_LIMIT:-4}
LOAD_SETTLE=${LOAD_SETTLE:-0}
EVIDENCE_ROOT=${EVIDENCE_DIR:-}

CARRIERS=256
LISTEN_PORT=9000
ECHO_PORT=5202
PORTBLOCK=31000-31511
PROD_SERVICES="frps caddy hysteria xray"
WAIT_UP=0

# Verbatim from t52-256-rerun-42ed56f-20261106T092730Z/far.cfg.
FAR_CFG='tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5202 n=4 m=4 fec=on mode=grouped w=0 carriers=256 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31511 nonce=00112233445566778899aabbccddeeff'

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=4)

EX_OK=0
EX_USAGE=2
EX_PRECOND=3
EX_BUSY=4
EX_LOCK=5
EX_BUILD=6
EX_SETUP=7
EX_GATE=8

note() { printf '%s: %s\n' "$PROG" "$*" >&2; }
die() { local rc=$1; shift; printf '%s: %s\n' "$PROG" "$*" >&2; exit "$rc"; }
usage() { sed -n '2,63p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-$EX_OK}"; }

rrun() { ssh "${SSH_OPTS[@]}" "$FAR_HOST" "$@"; }
rscp() { scp -q "${SSH_OPTS[@]}" "$@"; }

# --- lock -------------------------------------------------------------------
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

wait_load() {
    local host=$1 limit=$2 settle=$3 deadline l1
    deadline=$((SECONDS + settle))
    while :; do
        l1=$(load1 "$host") || return 3
        if awk -v a="$l1" -v b="$limit" 'BEGIN { exit !(a + 0 <= b + 0) }'; then
            printf '%s\n' "$l1"
            return 0
        fi
        if ((SECONDS >= deadline)); then
            note "load: $host load1=$l1 > $limit; refusing"
            return 4
        fi
        note "load: $host load1=$l1 > $limit; waiting up to ${settle}s"
        sleep 5
    done
}

# --- state helpers ----------------------------------------------------------
module_loaded() { rrun 'lsmod | grep -q "^tbtun "'; }
listening_on() { [[ -n $(rrun "ss -lntH 'sport = :$1'") ]]; }
service_state() { rrun "systemctl is-active $1 2>/dev/null || true"; }
cfg_diff() { LC_ALL=C comm -3 <(printf '%s\n' "$1" | tr ' ' '\n' | sort) <(printf '%s\n' "$2" | tr ' ' '\n' | sort); }

expected_cfg() { printf '%s\n' "$FAR_CFG" | sed 's/ w=0 / w=8 /'; }

# --- teardown ---------------------------------------------------------------
teardown_quiet() {
    rrun "systemctl stop tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
    rrun "systemctl reset-failed tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
    if module_loaded 2>/dev/null; then
        rrun 'rmmod tbtun' >/dev/null 2>&1 || note "teardown: rmmod tbtun failed on $FAR_HOST"
    fi
}

do_teardown() {
    lock_acquire "$FAR_HOST" "$LOCK_FILE" "t52-arm64 teardown" || die "$EX_LOCK" "lock busy: $LOCK_FILE on $FAR_HOST"
    rrun "systemctl stop tbt-echo-$ECHO_PORT.service" 2>/dev/null || true
    rrun "systemctl reset-failed tbt-echo-$ECHO_PORT.service" 2>/dev/null || true
    if module_loaded; then
        rrun 'rmmod tbtun'
    else
        printf 'module: already absent\n'
    fi
    local rc=0
    rrun 'test ! -d /proc/net/tbtun' || { note "teardown: /proc/net/tbtun still present"; rc=1; }
    if listening_on "$LISTEN_PORT"; then note "teardown: port $LISTEN_PORT still listening"; rc=1; fi
    if listening_on "$ECHO_PORT"; then note "teardown: port $ECHO_PORT still listening"; rc=1; fi
    printf 'module: absent\nports: %s/%s free\n' "$LISTEN_PORT" "$ECHO_PORT"
    lock_release
    exit "$rc"
}

do_status() {
    printf 'host=%s\n' "$FAR_HOST"
    printf 'kernel=%s\n' "$(rrun 'uname -r')"
    printf 'module=%s\n' "$(module_loaded && echo loaded || echo absent)"
    if rrun 'test -d /proc/net/tbtun'; then
        printf 'proc=present\n'
        printf 'ports=%s\n' "$(rrun 'cat /proc/net/tbtun/ports' 2>/dev/null || true)"
        printf 'config=%s\n' "$(rrun 'cat /proc/net/tbtun/config' 2>/dev/null || true)"
        printf 'tunnels=%s\n' "$(rrun 'cat /proc/net/tbtun/tunnels' 2>/dev/null || true)"
    else
        printf 'proc=absent\n'
    fi
    printf 'listen_%s=%s\n' "$LISTEN_PORT" "$(listening_on "$LISTEN_PORT" && echo yes || echo no)"
    printf 'echo_%s=%s\n' "$ECHO_PORT" "$(listening_on "$ECHO_PORT" && echo yes || echo no)"
    printf 'services:\n'
    local s
    for s in $PROD_SERVICES; do printf '  %s=%s\n' "$s" "$(service_state "$s")"; done
    printf 'lock=%s\n' "$(rrun "cat '$LOCK_FILE' 2>/dev/null || echo absent")"
}

# --- deploy -----------------------------------------------------------------
KEEP_FAR=0
FAR_TOUCHED=0
TMP=""

cleanup() {
    local rc=$?
    trap - EXIT INT TERM HUP
    set +e
    if ((KEEP_FAR == 0 && FAR_TOUCHED == 1)); then
        teardown_quiet
    fi
    lock_release
    [[ -n $TMP ]] && rm -rf "$TMP"
    set -e
    exit "$rc"
}

do_deploy() {
    [[ $# -eq 2 ]] || usage "$EX_USAGE"
    local ref=$1 ko_arg=$2
    local sha short stage stamp ev krel vm got expected missing s

    sha=$(git -C "$REPO" rev-parse --verify --quiet "$ref^{commit}") || die "$EX_USAGE" "not a commit: $ref"
    short=$(git -C "$REPO" rev-parse --short=12 "$sha")
    stage=$FAR_DIR
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    ev=$EVIDENCE_ROOT/deploy-hnd0-$stamp
    mkdir -p "$ev"

    if ! rrun true >/dev/null 2>&1; then
        die "$EX_PRECOND" "ssh $FAR_HOST failed (BatchMode root key required)"
    fi
    missing=$(rrun 'for t in flock ss systemd-run cc insmod modinfo sha256sum tar; do command -v "$t" >/dev/null 2>&1 || printf "%s " "$t"; done')
    [[ -z $missing ]] || die "$EX_PRECOND" "$FAR_HOST lacks: $missing"

    # The lock is the serialization point: every state check and every write
    # below runs under it, so a sibling agent cannot slip in between.
    lock_acquire "$FAR_HOST" "$LOCK_FILE" "t52-arm64 deploy $short" || die "$EX_LOCK" "lock busy: $LOCK_FILE on $FAR_HOST"
    local load1v
    load1v=$(wait_load "$FAR_HOST" "$LOAD_LIMIT" "$LOAD_SETTLE") || exit $?
    note "load: $FAR_HOST load1=$load1v (limit $LOAD_LIMIT)"

    for s in $PROD_SERVICES; do printf '%s=%s\n' "$s" "$(service_state "$s")"; done >"$ev/services-before.txt"
    note "production: $(tr '\n' ' ' <"$ev/services-before.txt")"

    if module_loaded; then
        die "$EX_PRECOND" "tbtun already loaded on $FAR_HOST; run: $PROG teardown"
    fi
    if listening_on "$LISTEN_PORT"; then
        die "$EX_PRECOND" "port $LISTEN_PORT already listens on $FAR_HOST"
    fi
    if listening_on "$ECHO_PORT"; then
        die "$EX_PRECOND" "port $ECHO_PORT already listens on $FAR_HOST"
    fi

    TMP=$(mktemp -d "${TMPDIR:-/tmp}/tbtun-deploy.XXXXXX")
    git -C "$REPO" archive --format=tar "$sha" >"$TMP/src.tar"
    note "stage: $sha -> $FAR_HOST:$stage"
    rrun "rm -rf '$stage' && mkdir -p '$stage'"
    ssh "${SSH_OPTS[@]}" "$FAR_HOST" "tar -x -C '$stage' -f -" <"$TMP/src.tar"
    rrun "test -f '$stage/Makefile'" || die "$EX_PRECOND" "staged tree on $FAR_HOST lacks Makefile"

    if [[ $ko_arg == - ]]; then
        note "module: building on $FAR_HOST"
        if ! rrun "cd '$stage' && rm -f Module.symvers && make -C /lib/modules/\$(uname -r)/build M=\$PWD modules -j2" >"$ev/far-build.log" 2>&1; then
            tail -n 40 "$ev/far-build.log" >&2
            die "$EX_BUILD" "far build failed; log: $ev/far-build.log"
        fi
    else
        [[ -f $ko_arg ]] || die "$EX_USAGE" "no such module: $ko_arg"
        rscp "$ko_arg" "$FAR_HOST:$stage/tbtun.ko"
    fi
    rrun "test -s '$stage/tbtun.ko'" || die "$EX_BUILD" "no module at $stage/tbtun.ko"

    if ! rrun "cd '$stage' && cc -O2 -Wall -Wextra -o tbt_echo benches/src/tbt_echo.c && cc -O2 -Wall -Wextra -o tbt_burst host/tbt_burst.c && cc -O2 -Wall -Wextra -o tbt_ping host/tbt_ping.c" >"$ev/host-tools.log" 2>&1; then
        tail -n 40 "$ev/host-tools.log" >&2
        die "$EX_BUILD" "host tool build failed; log: $ev/host-tools.log"
    fi

    krel=$(rrun 'uname -r')
    vm=$(rrun "modinfo -F vermagic '$stage/tbtun.ko' 2>/dev/null || true")
    [[ -n $vm ]] || vm=$(rrun "modinfo '$stage/tbtun.ko' | sed -n 's/^vermagic:[[:space:]]*//p'")
    vm=${vm%% *}
    [[ $vm == "$krel" ]] || die "$EX_PRECOND" "module vermagic '$vm' does not match $FAR_HOST kernel '$krel'"
    {
        printf 'host=%s\nutc=%s\nref=%s\nsha=%s\nstage=%s\n' "$FAR_HOST" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$ref" "$sha" "$stage"
        printf 'vermagic=%s\nsrcversion=%s\n' "$vm" "$(rrun "modinfo -F srcversion '$stage/tbtun.ko' 2>/dev/null || true")"
        printf 'sha256=%s\n' "$(rrun "sha256sum '$stage/tbtun.ko'" | awk '{print $1}')"
        printf 'load1=%s\n' "$load1v"
    } >"$ev/module.txt" 2>&1 || note "module record incomplete"

    rrun "insmod '$stage/tbtun.ko' max_carriers=$CARRIERS max_tunnels=1 debug=0" || die "$EX_SETUP" "insmod failed on $FAR_HOST"
    FAR_TOUCHED=1
    rrun 'test -d /proc/net/tbtun' || die "$EX_SETUP" "insmod succeeded but /proc/net/tbtun is absent"
    note "module: loaded (max_carriers=$CARRIERS max_tunnels=1 debug=0)"

    rrun "printf 'ports $PORTBLOCK\n' > /proc/net/tbtun/ports" || die "$EX_SETUP" "ports write failed"
    got=$(rrun "grep '^ports ' /proc/net/tbtun/ports")
    [[ $got == "ports $PORTBLOCK" ]] || die "$EX_SETUP" "ports readback '$got' != 'ports $PORTBLOCK'"

    rrun "printf '%s\n' '$FAR_CFG' > /proc/net/tbtun/config" || die "$EX_SETUP" "config write failed"
    got=$(rrun 'cat /proc/net/tbtun/config')
    expected=$(expected_cfg)
    if [[ $got != "$expected" ]]; then
        local diff
        diff=$(cfg_diff "$expected" "$got")
        if [[ -n $diff ]]; then
            printf '%s\n' "$diff" >&2
            die "$EX_SETUP" "rendered config differs from $ev expectation (see diff above)"
        fi
    fi
    printf '%s\n' "$got" >"$ev/config.txt"
    rrun 'cat /proc/net/tbtun/tunnels' >"$ev/tunnels.txt"
    rrun 'cat /proc/net/tbtun/ports' >"$ev/ports.txt"
    grep -q '^1 far listening 0.0.0.0:9000 ' "$ev/tunnels.txt" || die "$EX_SETUP" "far tunnel is not listening on 0.0.0.0:$LISTEN_PORT"

    rrun "systemctl stop tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
    rrun "systemctl reset-failed tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
    rrun "systemd-run --no-block --unit=tbt-echo-$ECHO_PORT --collect --property=StandardOutput=file:$stage/tbt-echo-$ECHO_PORT.log --property=StandardError=file:$stage/tbt-echo-$ECHO_PORT.log '$stage/tbt_echo' $ECHO_PORT echo" \
        || die "$EX_SETUP" "systemd-run for tbt-echo-$ECHO_PORT failed"
    local i
    for i in $(seq 1 20); do
        listening_on "$ECHO_PORT" && break
        sleep 0.5
    done
    listening_on "$ECHO_PORT" || die "$EX_SETUP" "echo target is not listening on 127.0.0.1:$ECHO_PORT"

    if ((WAIT_UP > 0)); then
        local deadline up
        deadline=$((SECONDS + WAIT_UP))
        while :; do
            up=$(rrun "grep -E '^tunnel 1 c3 ' /proc/net/tbtun/stats | grep -oE ' up=[0-9]+' | head -n 1 | cut -d= -f2")
            if [[ $up == "$CARRIERS" ]]; then
                note "up: $up on $FAR_HOST"
                break
            fi
            if ((SECONDS >= deadline)); then
                die "$EX_GATE" "up=$up after ${WAIT_UP}s (expected $CARRIERS); is the near side joining?"
            fi
            sleep 2
        done
    else
        note "up: not waited (no near side yet); run_t52_65.sh gates up=$CARRIERS on both ends"
    fi

    for s in $PROD_SERVICES; do printf '%s=%s\n' "$s" "$(service_state "$s")"; done >"$ev/services-after.txt"
    diff -u "$ev/services-before.txt" "$ev/services-after.txt" >"$ev/services.diff" || note "production service state changed; see $ev/services.diff"

    {
        printf 'far=%s\nconfig=%s\nports=%s\n' "$FAR_HOST" "$(cat "$ev/config.txt")" "$(cat "$ev/ports.txt")"
        printf 'echo=%s\n' "$(rrun "systemctl is-active tbt-echo-$ECHO_PORT.service 2>/dev/null || true")"
        printf 'up=%s\n' "$(rrun "grep -E '^tunnel 1 c3 ' /proc/net/tbtun/stats | grep -oE ' up=[0-9]+' | head -n 1 | cut -d= -f2")"
    } >"$ev/summary.txt" 2>&1 || note "summary incomplete"

    KEEP_FAR=1
    printf '\nfar: %s up (module loaded, %s listening, echo on 127.0.0.1:%s)\n' "$FAR_HOST" "$LISTEN_PORT" "$ECHO_PORT"
    printf 'evidence: %s\n' "$ev"
    printf 'next: review/wan-experiment/arm64/run_t52_65.sh <mib> <seconds> <mode>\n'
    printf 'after the run: run_t52_65.sh tears both ends down; standalone cleanup is %s teardown\n' "$PROG"
}

# --- entry ------------------------------------------------------------------
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1 || die "$EX_USAGE" "not a git checkout: $REPO"
EVIDENCE_ROOT=${EVIDENCE_ROOT:-$REPO/review/wan-experiment/arm64}

CMD=deploy
case ${1:-} in
    teardown | status | deploy)
        CMD=$1
        shift
        ;;
esac

while (($#)); do
    case $1 in
        -h | --help) usage "$EX_OK" ;;
        --wait-up)
            WAIT_UP=${2:-}
            [[ $WAIT_UP =~ ^[0-9]+$ ]] || die "$EX_USAGE" "--wait-up needs seconds"
            shift 2
            ;;
        --evidence-dir)
            EVIDENCE_ROOT=${2:-}
            [[ -n $EVIDENCE_ROOT ]] || die "$EX_USAGE" "--evidence-dir needs a path"
            shift 2
            ;;
        --) shift; break ;;
        -*) die "$EX_USAGE" "unknown option: $1 (try --help)" ;;
        *) break ;;
    esac
done

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

case $CMD in
    status)
        (($# == 0)) || usage "$EX_USAGE"
        KEEP_FAR=1
        do_status
        ;;
    teardown)
        (($# == 0)) || usage "$EX_USAGE"
        KEEP_FAR=1
        do_teardown
        ;;
    deploy) do_deploy "$@" ;;
esac
