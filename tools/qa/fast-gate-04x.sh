#!/usr/bin/env bash
# fast-gate-04x.sh - one-shot fast release gate for an intermediate 0.4.x tag.
#
# The gate owns the test pair for its run. It does not retry a build, a
# deployment, a join, or a transfer. A failure is recorded, evidence is kept,
# and the run stops at that stage.
#
# Stages
#   G0 host build   `git archive <tag>` to both hosts. The far VM builds with
#                   -j1 (its 955 MiB cannot survive a parallel make); the near
#                   board builds with -j8. Checks: modinfo version, srcversion,
#                   vermagic, and the ko sha256. The archive sha256 must be
#                   identical on both hosts and equal to the local one.
#   G1 deploy/join  clean del+rmmod, far->near insmod, the 256-carrier
#                   SC-LDPC config (far 1x256/L8/r0.5, near 8x32/L16/r0.25),
#                   the near pref-100 ip rule, and the far 5210 1 GiB source.
#   G2 transfers    60 s downlink steady, 60 s uplink, and a 1 MiB uplink
#                   completion probe. Checks: HTTP 200, app bytes and
#                   rx_delivered > 0, zero reset/hole/deliver_bad deltas,
#                   streams_active=0 within 15 s, and a clean kernel window.
#   G3 carrier      up + replacing + empty == 256. Default is strict up=256;
#                   `--min-up N` (N < 256) records the missing carriers as
#                   path SYN-loss evidence. The near netwatch unit must be
#                   active and some process must hold /dev/watchdog0. Both
#                   are read-only observations.
#
# Modes
#   --run       full gate (default)
#   --cleanup   remove this tag's sources, ip rule, and modules; no build
#   --dry-run   print the plan; no ssh, no archive, no remote write
#   --help      this text
#
# Options
#   --tag TAG          tag to build (default v0.4.1)
#   --tag-commit SHA    refuse unless TAG resolves to SHA
#   --min-up N          accept up >= N (1..256); default 256
#   --keep              keep the deployment loaded/configured after PASS
#   --evidence DIR      evidence root (default <repo>/)
#
# Environment
#   NEAR_HOST=root@192.168.123.85  FAR_HOST=root@45.11.1.62
#   FAR_IP=45.11.1.62             NEAR_PORT=9001  FAR_PORT=5210
#   FAR_LISTEN_PORT=9000          TUNNEL=1        GATE_DEBUG=1
#   EXPECT_MODULE_VERSION=<tag without v>   empty: skip the version check
#   EXPECT_NEAR_SRCVERSION= EXPECT_FAR_SRCVERSION=   optional pins
#   EXPECT_NEAR_KO_SHA= EXPECT_FAR_KO_SHA=           optional ko pins
#   JOBS_NEAR=8 JOBS_FAR=1 JOIN_TIMEOUT=180 BUILD_TMO=1800 UPLOAD_TMO=900
#   DOWN_SECONDS=60 UP_SECONDS=60 UP_MIB=512
#   NEAR_DEV=eth0 BYPASS_PREF=100
#
# Exit codes
#   0 PASS  2 usage  3 G0  4 G1  5 G2  6 G3/checks  7 cleanup  130 interrupt
#
# The near netwatch unit, /dev/watchdog0, and unrelated services are never
# written. The gate only reads them.
set -uo pipefail

SELF_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib.sh
. "$SELF_DIR/lib.sh"

PROG=${0##*/}
MODE=run
DRY=0
KEEP=${KEEP:-0}
MIN_UP=${MIN_UP:-256}
GATE_TAG=${GATE_TAG:-v0.4.1}
GATE_TAG_COMMIT=${GATE_TAG_COMMIT:-}
JOBS_NEAR=${JOBS_NEAR:-8}
JOBS_FAR=${JOBS_FAR:-1}
JOIN_TIMEOUT=${JOIN_TIMEOUT:-180}
BUILD_TMO=${BUILD_TMO:-1800}
UPLOAD_TMO=${UPLOAD_TMO:-900}
FAR_START_TMO=${FAR_START_TMO:-900}
DOWN_SECONDS=${DOWN_SECONDS:-60}
UP_SECONDS=${UP_SECONDS:-60}
UP_MIB=${UP_MIB:-512}
GATE_DEBUG=${GATE_DEBUG:-1}
BYPASS_PREF=${BYPASS_PREF:-100}
NEAR_DEV=${NEAR_DEV-eth0}
NEAR_PORTBLOCK=${NEAR_PORTBLOCK:-30000-30511}
FAR_PORTBLOCK=${FAR_PORTBLOCK:-31000-31511}
FAR_LISTEN_PORT=${FAR_LISTEN_PORT:-9000}
CARRIERS=${CARRIERS:-256}
EXPECT_BYTES=1073741824
PROBE_BYTES=1048576
TMP=
FAILED=0

TAG_DIR=
TAG_TYPE=
TAG_COMMIT=
TAG_VERSION_HEADER=
EXPECT_VERSION=
ARCHIVE_SHA=
ARCHIVE_SIZE=
FAR_RUN=
NEAR_RUN=
FAR_ACCEPT=
FAR_UP=
NEAR_UP=
FAR_KO=
NEAR_KO=

NEAR_ARCH= FAR_ARCH=
NEAR_KREL= FAR_KREL=
NEAR_VER= FAR_VER=
NEAR_SV= FAR_SV=
NEAR_VMAGIC= FAR_VMAGIC=
NEAR_KO_SHA= FAR_KO_SHA=
NEAR_BUILD_WARN= FAR_BUILD_WARN=

NEAR_DMESG_MARK= FAR_DMESG_MARK=
FAR_CFG= NEAR_CFG=
NONCE=
NEAR_PHYS_DEV=
FAR_SHA=
FAR_SIZE=
NEAR_IP_RULE_ADDED=0
NEAR_IP_RULE_OWNED=0

DOWN_PID= DOWN_HTTP= DOWN_SIZE= DOWN_TIME= DOWN_SPEED=
DOWN_NEAR_RX= DOWN_FAR_SUBMIT=
UP_PID= UP_HTTP= UP_BYTES= UP_TIME= UP_SPEED=
UP_NEAR_SUBMIT= UP_FAR_RX=
PROBE_HTTP= PROBE_UP= PROBE_RC=
DOWN_STREAMS_OK= UP_STREAMS_OK=
DOWN_STREAMS_FINAL= UP_STREAMS_FINAL=
DOWN_WIRE= UP_WIRE=
DOWN_NEAR_CPU= DOWN_FAR_CPU= UP_NEAR_CPU= UP_FAR_CPU=

G0_FAILED=0 G1_FAILED=0 G2_FAILED=0 G3_FAILED=0 CLEANUP_FAILED=0
STAGE_FAIL_BEFORE=0
START_UTC= END_UTC= START_EPOCH=

say() { qa_say "$@"; }
note() { qa_note "$@"; }
mark() { qa_mark "$@"; }

usage() {
    cat <<EOF
usage: $PROG [--run|--cleanup|--dry-run] [--tag TAG] [--tag-commit SHA]
             [--min-up N] [--keep] [--evidence DIR]

One shot. The gate does not retry a build, a deployment, a join, or a transfer.

G0  git archive <tag> -> both hosts; far make -j1, near make -j8;
    check modinfo version/srcversion/vermagic and the ko sha256.
G1  clean rmmod/insmod, 256-carrier SC-LDPC config, near pref-100 rule,
    far 5210 1 GiB source, join gate.
G2  60 s downlink, 60 s uplink, 1 MiB uplink probe; HTTP 200,
    app/rx_delivered > 0, zero reset/hole/deliver_bad, streams_active=0
    within 15 s, clean kernel window.
G3  up + replacing + empty == 256; strict up=256 or --min-up N with
    SYN-loss evidence; netwatch active and a /dev/watchdog0 holder.

Options
  --tag TAG          tag to build (default $GATE_TAG)
  --tag-commit SHA    refuse unless TAG resolves to SHA
  --min-up N          accept up >= N (1..256); default $MIN_UP
  --keep              keep the deployment loaded/configured after PASS
  --evidence DIR      evidence root (default $EVIDENCE_ROOT)

Environment
  NEAR_HOST=$NEAR_HOST  FAR_HOST=$FAR_HOST
  FAR_IP=$FAR_IP  NEAR_PORT=$NEAR_PORT  FAR_PORT=$FAR_PORT
  FAR_LISTEN_PORT=$FAR_LISTEN_PORT  TUNNEL=$TUNNEL  GATE_DEBUG=$GATE_DEBUG
  EXPECT_MODULE_VERSION=  (default: tag without the leading v; empty skips)
  EXPECT_NEAR_SRCVERSION= EXPECT_FAR_SRCVERSION=  (optional pins)
  EXPECT_NEAR_KO_SHA= EXPECT_FAR_KO_SHA=  (optional ko pins)
  JOBS_NEAR=$JOBS_NEAR JOBS_FAR=$JOBS_FAR JOIN_TIMEOUT=$JOIN_TIMEOUT
  BUILD_TMO=$BUILD_TMO UPLOAD_TMO=$UPLOAD_TMO
  DOWN_SECONDS=$DOWN_SECONDS UP_SECONDS=$UP_SECONDS
  UP_MIB=$UP_MIB NEAR_DEV=$NEAR_DEV BYPASS_PREF=$BYPASS_PREF

Exit: 0 PASS, 2 usage, 3 G0, 4 G1, 5 G2, 6 G3/checks, 7 cleanup.
A FAIL keeps the deployment and the evidence. Run --cleanup --tag TAG to
remove it. The netwatch unit and /dev/watchdog0 are never touched.
EOF
}

die() {
    local rc=$1
    shift
    printf '%s: %s\n' "$PROG" "$*" >&2
    exit "$rc"
}

stage_start() { STAGE_FAIL_BEFORE=$FAILED; }
stage_failed() { (( FAILED > STAGE_FAIL_BEFORE )); }

# ---------------------------------------------------------------------------
# ssh and local helpers

gate_ssh() { # host seconds command...
    local host=$1 tmo=$2 target
    shift 2
    case $host in
        near) target=$NEAR_HOST ;;
        far) target=$FAR_HOST ;;
        *) printf '%s: unknown host: %s\n' "$PROG" "$host" >&2; return 2 ;;
    esac
    timeout -k 5 "$tmo" ssh -o BatchMode=yes -o ConnectTimeout=10 \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "$target" "$@"
}

ssh_near() { gate_ssh near "$SSH_TMO" "$@"; }
ssh_far() { gate_ssh far "$SSH_TMO" "$@"; }
ssh_near_long() { local tmo=$1; shift; gate_ssh near "$tmo" "$@"; }
ssh_far_long() { local tmo=$1; shift; gate_ssh far "$tmo" "$@"; }
scp_near() { timeout -k 5 "$SSH_TMO" scp -q -o BatchMode=yes -o ConnectTimeout=10 "$@"; }
scp_far() { timeout -k 5 "$SSH_TMO" scp -q -o BatchMode=yes -o ConnectTimeout=10 "$@"; }

num() { case ${1:-} in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }

delta() { # before after -> difference, or ?
    if num "${1:-}" && num "${2:-}"; then
        printf '%s' "$(( $2 - $1 ))"
    else
        printf '?'
    fi
}

cfg_has() { # file token
    awk -v t="$2" '
        { for (i = 1; i <= NF; i++) if ($i == t) found = 1 }
        END { exit(found ? 0 : 1) }' "$1" 2>/dev/null
}

stat_field() { # stats-file class field [tunnel]
    awk -v c="$2" -v f="$3=" -v t="${4:-$TUNNEL}" '
        $1 == "tunnel" && $2 == t && $3 == c {
            for (i = 1; i <= NF; i++)
                if (index($i, f) == 1) {
                    print substr($i, length(f) + 1)
                    exit
                }
        }' "$1" 2>/dev/null
}

any_field() { # stats-file field [tunnel]
    awk -v f="$2=" -v t="${3:-$TUNNEL}" '
        $1 == "tunnel" && $2 == t {
            for (i = 1; i <= NF; i++)
                if (index($i, f) == 1) {
                    print substr($i, length(f) + 1)
                    exit
                }
        }' "$1" 2>/dev/null
}

kv_line() { # "key=value key=value" key
    awk -v k="$2=" '{ for (i = 1; i <= NF; i++)
        if (index($i, k) == 1) { print substr($i, length(k) + 1); exit } }' <<<"${1:-}"
}

kv_file() { # file key
    awk -v k="$2=" '{ for (i = 1; i <= NF; i++)
        if (index($i, k) == 1) { print substr($i, length(k) + 1); exit } }' "$1" 2>/dev/null
}

sum_counter() { # counters-file key...
    local f=$1
    shift
    if [[ ! -f $f ]]; then
        printf '0\n'
        return
    fi
    awk -F= -v keys="$*" '
        BEGIN { n = split(keys, k, " ") }
        $1 ~ /^\[/ { next }
        { for (i = 1; i <= n; i++) if ($1 == k[i]) s += $2 }
        END { print s + 0 }' "$f" 2>/dev/null
}

snap_stats() { # near|far outfile
    gate_ssh "$1" "$SSH_TMO" "cat '$PROC_ROOT/stats' 2>/dev/null" >"$2" 2>/dev/null || true
}

snap_proc() {
    gate_ssh "$1" "$SSH_TMO" "cat /proc/stat 2>/dev/null" >"$2" 2>/dev/null || true
}

snap_netdev() {
    gate_ssh "$1" "$SSH_TMO" "cat /proc/net/dev 2>/dev/null" >"$2" 2>/dev/null || true
}

streams_active() { # near|far -> value or empty
    gate_ssh "$1" "$SSH_TMO" "cat '$PROC_ROOT/stats' 2>/dev/null" 2>/dev/null |
        awk -v t="$TUNNEL" '
            $1 == "tunnel" && $2 == t && $3 == "streams" {
                for (i = 1; i <= NF; i++)
                    if ($i ~ /^streams_active=/) {
                        sub("streams_active=", "", $i)
                        print $i
                        exit
                    }
            }'
}

netdev_rx() { awk -v d="$2:" '$1 == d { print $2; exit }' "$1" 2>/dev/null; }
netdev_tx() { awk -v d="$2:" '$1 == d { print $10; exit }' "$1" 2>/dev/null; }

cpu_busy() {
    awk 'NR == 1 { b = $2 + $3 + $4 + $7 + $8 + $9; i = $5 + $6
        printf "%s %s\n", b, i }' "$1" 2>/dev/null
}

cpu_pct() {
    awk -v b1="$1" -v i1="$2" -v b2="$3" -v i2="$4" 'BEGIN {
        b = b2 - b1; i = i2 - i1; t = b + i
        if (t > 0) printf "%.1f", b * 100 / t
        else printf "0"
    }'
}

host_phys_dev() { # near
    gate_ssh near "$SSH_TMO" "ip route get $FAR_IP 2>/dev/null" 2>/dev/null |
        awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }'
}

check_streams_after() { # check-prefix
    local end v i
    for end in near far; do
        i=0
        v=
        while ((i <= 15)); do
            v=$(streams_active "$end")
            [[ ${v:-?} == 0 ]] && break
            sleep 1
            i=$((i + 1))
        done
        qa_check "$1_${end}_streams_active" "$([[ ${v:-?} == 0 ]]; echo $?)" \
            "streams_active=${v:-?} after ${i}s"
    done
}

wait_streams_clear() { # check-prefix
    local end v i
    for end in near far; do
        i=0
        v=
        while ((i <= 15)); do
            v=$(streams_active "$end")
            [[ ${v:-?} == 0 ]] && break
            sleep 1
            i=$((i + 1))
        done
        qa_check "$1_${end}_clear" "$([[ ${v:-?} == 0 ]]; echo $?)" \
            "streams_active=${v:-?} wait=${i}s"
    done
}

check_kernel_window() { # check-prefix since until
    local prefix=$1 since=$2 until=$3 end f state
    for end in near far; do
        f="$EV/$prefix-kernel-$end.txt"
        qa_kernel_window "$end" "$since" "$until" "$f"
        state=$(qa_kw_state "$f")
        case $state in
            PASS) qa_check "${prefix}_${end}_kernel" 0 "clean source=$(qa_kw_source "$f")" ;;
            FAIL) qa_check "${prefix}_${end}_kernel" 1 "BUG/Oops/WARNING/UBSAN in window source=$(qa_kw_source "$f")" ;;
            *) qa_skip "${prefix}_${end}_kernel" "empty window source=$(qa_kw_source "$f")" ;;
        esac
        gate_ssh "$end" "$SSH_TMO" "dmesg 2>/dev/null | tail -n 600" \
            >"$EV/$prefix-dmesg-$end.txt" 2>/dev/null || true
    done
}

check_delta_zero() { # check-name pre post class field
    local d
    d=$(delta "$(stat_field "$2" "$4" "$5")" "$(stat_field "$3" "$4" "$5")")
    qa_check "$1" "$([[ ${d:-?} == 0 ]]; echo $?)" "$4/$5 delta=${d:-?}"
}

check_any_delta_zero() { # check-name pre post field
    local d
    d=$(delta "$(any_field "$2" "$4")" "$(any_field "$3" "$4")")
    qa_check "$1" "$([[ ${d:-?} == 0 ]]; echo $?)" "$4 delta=${d:-?}"
}

# ---------------------------------------------------------------------------
# argument parsing and derived paths

parse_args() {
    while (($#)); do
        case $1 in
            --run) MODE=run ;;
            --cleanup) MODE=cleanup ;;
            --dry-run) DRY=1 ;;
            --keep) KEEP=1 ;;
            --tag)
                (($# >= 2)) || die 2 "--tag needs a value"
                GATE_TAG=$2
                shift
                ;;
            --tag=*) GATE_TAG=${1#*=} ;;
            --tag-commit)
                (($# >= 2)) || die 2 "--tag-commit needs a value"
                GATE_TAG_COMMIT=$2
                shift
                ;;
            --tag-commit=*) GATE_TAG_COMMIT=${1#*=} ;;
            --min-up)
                (($# >= 2)) || die 2 "--min-up needs a value"
                MIN_UP=$2
                shift
                ;;
            --min-up=*) MIN_UP=${1#*=} ;;
            --evidence)
                (($# >= 2)) || die 2 "--evidence needs a value"
                EVIDENCE_ROOT=$2
                shift
                ;;
            --evidence=*) EVIDENCE_ROOT=${1#*=} ;;
            -h | --help)
                usage
                exit 0
                ;;
            *)
                printf '%s: unknown argument: %s\n' "$PROG" "$1" >&2
                usage >&2
                exit 2
                ;;
        esac
        shift
    done
    num "$MIN_UP" || die 2 "--min-up must be an integer"
    ((MIN_UP >= 1 && MIN_UP <= 256)) || die 2 "--min-up must be 1..256"
    case $GATE_DEBUG in
        0 | 1) ;;
        *) die 2 "GATE_DEBUG must be 0 or 1" ;;
    esac
    case $GATE_TAG in
        '' | *[!A-Za-z0-9._/-]*) die 2 "bad tag: $GATE_TAG" ;;
        *..* | *'//'*) die 2 "bad tag: $GATE_TAG" ;;
    esac
}

derive() {
    TAG_DIR=${GATE_TAG//[^A-Za-z0-9._-]/_}
    local def
    case $GATE_TAG in
        v*) def=${GATE_TAG#v} ;;
        *) def=$GATE_TAG ;;
    esac
    EXPECT_VERSION=${EXPECT_MODULE_VERSION-$def}
    FAR_ROOT=${FAR_ROOT:-/root/tbtun-fastgate}
    NEAR_ROOT=${NEAR_ROOT:-/root/tbtun-fastgate}
    FAR_RUN=$FAR_ROOT/$TAG_DIR
    NEAR_RUN=$NEAR_ROOT/$TAG_DIR
    FAR_ACCEPT=$FAR_RUN/accept-1g
    FAR_UP=$FAR_RUN/uplink
    NEAR_UP=$NEAR_RUN/up.bin
    FAR_KO=$FAR_RUN/src/tbtun.ko
    NEAR_KO=$NEAR_RUN/src/tbtun.ko
}

# ---------------------------------------------------------------------------
# dry run

dry_run() {
    local commit= type=missing
    commit=$(git -C "$QA_REPO" rev-parse --verify --quiet "$GATE_TAG^{commit}" 2>/dev/null || true)
    if [[ -n $commit ]]; then
        type=$(git -C "$QA_REPO" cat-file -t "$GATE_TAG" 2>/dev/null || echo '?')
    fi
    say "dry-run: no ssh, no git archive, no remote write"
    say "  tag:         $GATE_TAG ($type${commit:+, commit $commit})"
    say "  version:     ${EXPECT_VERSION:-<not checked>}"
    say "  hosts:       near=$NEAR_HOST far=$FAR_HOST (far ip $FAR_IP)"
    say "  evidence:    $EVIDENCE_ROOT/fast-gate-04x-$TAG_DIR-<UTC>"
    say "  uploads:     tar $FAR_RUN/src.tar (far) and tar $NEAR_RUN/src.tar (near)"
    say "  G0:          far make -C /lib/modules/\$(uname -r)/build M=\$PWD modules -j$JOBS_FAR"
    say "               near make ... -j$JOBS_NEAR"
    say "               check version=$EXPECT_VERSION srcversion, vermagic, ko sha256"
    say "  G1 near:     $NEAR_CFG_DRY"
    say "  G1 far:      $FAR_CFG_DRY"
    say "               near ip rule: to $FAR_IP/32 lookup main pref $BYPASS_PREF (add only when absent)"
    say "               join gate: up >= $MIN_UP on both ends within ${JOIN_TIMEOUT}s"
    say "               far source: $FAR_PORT 127.0.0.1, 1 GiB, sha256 recorded"
    say "  G2 down:     near curl -m $DOWN_SECONDS http://127.0.0.1:$NEAR_PORT/; http=200, rx_delivered > 0"
    say "  G2 up:       near curl -m $UP_SECONDS POST $UP_MIB MiB -> far $FAR_PORT receiver;"
    say "               then a $PROBE_BYTES-byte POST for HTTP 200 and a complete body"
    say "               zero reset/hole/deliver_bad deltas; streams_active=0 within 15 s"
    say "  G3:          up + replacing + empty == $CARRIERS"
    case $MIN_UP in
        256) say "               strict: up=$CARRIERS on both ends" ;;
        *) say "               --min-up $MIN_UP: require up >= $MIN_UP; record near SYN-SENT and far SYN-RECV evidence" ;;
    esac
    say "               near netwatch active; a process holds /dev/watchdog0 (read-only)"
    if [[ -z $commit ]]; then
        say "  warning: tag $GATE_TAG is missing; the real run refuses at G0"
    fi
    say "  cleanup:     PASS removes sources, the ip rule it added, and both modules;"
    say "               FAIL keeps them. --cleanup performs that removal explicitly."
    say "               --keep leaves the deployment on PASS."
    exit 0
}

NEAR_CFG_DRY=
FAR_CFG_DRY=

dry_configs() {
    local pre post nonce dev
    nonce='<32 hex>'
    pre="fec=on adaptive=off mode=sc-ldpc"
    post="crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=16777216 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000"
    FAR_CFG_DRY="tunnel 1 role=far listen=0.0.0.0:$FAR_LISTEN_PORT target=127.0.0.1:$FAR_PORT n=128 m=128 $pre w=256 carriers=$CARRIERS $post portblock=$FAR_PORTBLOCK L=8 nonce=$nonce"
    dev=
    [[ -n $NEAR_DEV ]] && dev=" dev=$NEAR_DEV"
    NEAR_CFG_DRY="tunnel 1 role=near listen=127.0.0.1:$NEAR_PORT peer=$FAR_IP:$FAR_LISTEN_PORT n=8 m=24 $pre w=128 carriers=$CARRIERS $post portblock=$NEAR_PORTBLOCK L=16 nonce=$nonce$dev"
}

# ---------------------------------------------------------------------------
# G0: host build

g0_archive() {
    local commit hdr
    if ! git -C "$QA_REPO" rev-parse --git-dir >/dev/null 2>&1; then
        qa_check g0_git 1 "$QA_REPO is not a git checkout"
        return 1
    fi
    if ! commit=$(git -C "$QA_REPO" rev-parse --verify --quiet "$GATE_TAG^{commit}" 2>/dev/null); then
        qa_check g0_tag 1 "tag $GATE_TAG does not resolve"
        return 1
    fi
    TAG_COMMIT=$commit
    TAG_TYPE=$(git -C "$QA_REPO" cat-file -t "$GATE_TAG" 2>/dev/null || echo '?')
    qa_check g0_tag 0 "tag=$GATE_TAG type=$TAG_TYPE commit=$commit"
    if [[ -n $GATE_TAG_COMMIT ]]; then
        qa_check g0_tag_commit "$([[ $GATE_TAG_COMMIT == "$commit" ]]; echo $?)" \
            "pin=$GATE_TAG_COMMIT resolved=$commit"
    else
        qa_skip g0_tag_commit "unpinned; set --tag-commit for a release gate"
    fi
    hdr=$(git -C "$QA_REPO" show "$GATE_TAG:src/kernel/tbt_uapi.h" 2>/dev/null |
        awk '
            /^#define TBT_VERSION_MAJOR/ { maj = $3 }
            /^#define TBT_VERSION_MINOR/ { min = $3 }
            /^#define TBT_VERSION_PATCH/ { pat = $3 }
            END {
                if (maj == "" || min == "" || pat == "") exit 1
                printf "%s.%s.%s", maj, min, pat
            }') || hdr=
    TAG_VERSION_HEADER=$hdr
    if [[ -n $EXPECT_VERSION ]]; then
        qa_check g0_version_header "$([[ $hdr == "$EXPECT_VERSION" ]]; echo $?)" \
            "tbt_uapi.h=${hdr:-?} want=$EXPECT_VERSION"
    else
        qa_skip g0_version_header "EXPECT_MODULE_VERSION is empty"
    fi
    if ! git -C "$QA_REPO" archive --format=tar "$GATE_TAG" >"$TMP/src.tar" 2>"$EV/g0-archive.err"; then
        qa_check g0_archive 1 "git archive failed; see g0-archive.err"
        return 1
    fi
    ARCHIVE_SHA=$(sha256sum "$TMP/src.tar" | awk '{print $1}')
    ARCHIVE_SIZE=$(stat -c %s "$TMP/src.tar" 2>/dev/null || echo 0)
    tar -tf "$TMP/src.tar" >"$TMP/src.list" 2>/dev/null || true
    local have_make have_hdr
    awk '$0 == "Makefile" { f = 1 } END { exit(f ? 0 : 1) }' "$TMP/src.list"
    have_make=$?
    awk '$0 == "src/kernel/tbt_uapi.h" { f = 1 } END { exit(f ? 0 : 1) }' "$TMP/src.list"
    have_hdr=$?
    qa_check g0_archive "$([[ $have_make == 0 && $have_hdr == 0 ]]; echo $?)" \
        "bytes=$ARCHIVE_SIZE sha256=$ARCHIVE_SHA"
    ((have_make == 0 && have_hdr == 0))
}

collect_identity() { # host run ko outfile
    local host=$1 run=$2 ko=$3 out=$4
    gate_ssh "$host" "$SSH_TMO" "KO='$ko' RUN='$run' bash -s" >"$out" 2>"$out.err" <<'EOS'
set -u
printf 'host=%s\n' "$(uname -n)"
printf 'arch=%s\n' "$(uname -m)"
printf 'kernel=%s\n' "$(uname -r)"
printf 'ko_path=%s\n' "$KO"
printf 'ko_size=%s\n' "$(stat -c %s "$KO" 2>/dev/null || echo 0)"
printf 'ko_sha256=%s\n' "$(sha256sum "$KO" 2>/dev/null | awk '{print $1}')"
v=$(modinfo -F version "$KO" 2>/dev/null) || v=
[ -n "$v" ] || v=$(modinfo "$KO" 2>/dev/null | sed -n 's/^version:[[:space:]]*//p' | head -n 1)
printf 'version=%s\n' "$v"
s=$(modinfo -F srcversion "$KO" 2>/dev/null) || s=
[ -n "$s" ] || s=$(modinfo "$KO" 2>/dev/null | sed -n 's/^srcversion:[[:space:]]*//p' | head -n 1)
printf 'srcversion=%s\n' "$s"
g=$(modinfo -F vermagic "$KO" 2>/dev/null) || g=
[ -n "$g" ] || g=$(modinfo "$KO" 2>/dev/null | sed -n 's/^vermagic:[[:space:]]*//p' | head -n 1)
printf 'vermagic=%s\n' "$g"
EOS
}

g0_build_host() { # host jobs
    local host=$1 jobs=$2 run ko got rc warn
    case $host in
        near) run=$NEAR_RUN ;;
        far) run=$FAR_RUN ;;
        *) return 2 ;;
    esac
    ko=$run/src/tbtun.ko
    gate_ssh "$host" "$UPLOAD_TMO" "rm -rf '$run' && mkdir -p '$run/src'" || return 1
    gate_ssh "$host" "$UPLOAD_TMO" "cat > '$run/src.tar'" <"$TMP/src.tar" || return 2
    got=$(gate_ssh "$host" "$SSH_TMO" "sha256sum '$run/src.tar' | awk '{print \$1}'" 2>/dev/null) || got=
    printf '%s\n' "$got" >"$EV/g0-$host-archive.sha"
    [[ $got == "$ARCHIVE_SHA" ]] || return 3
    gate_ssh "$host" "$UPLOAD_TMO" "tar -xf '$run/src.tar' -C '$run/src' && rm -f '$run/src.tar'" || return 4
    local cmd="cd '$run/src' && rm -f Module.symvers && make -C /lib/modules/\$(uname -r)/build M=\$PWD modules -j$jobs"
    rc=0
    gate_ssh "$host" "$BUILD_TMO" "$cmd" >"$EV/g0-$host-build.log" 2>&1 || rc=$?
    printf '%s\n' "$rc" >"$EV/g0-$host-build.rc"
    ((rc == 0)) || return 5
    gate_ssh "$host" "$SSH_TMO" "test -s '$ko'" || return 6
    collect_identity "$host" "$run" "$ko" "$EV/g0-$host-identity.txt" || return 7
    warn=$(awk '/warning:/ { n++ } END { print n + 0 }' "$EV/g0-$host-build.log")
    printf 'build_warnings=%s\n' "$warn" >>"$EV/g0-$host-identity.txt"
    return 0
}

id_get() { sed -n "s/^$2=//p" "$1" 2>/dev/null | head -n 1; }

g0_identity_checks() {
    local end idf arch krel ver sv vm sha size warn want
    for end in near far; do
        idf=$EV/g0-$end-identity.txt
        if [[ ! -s $idf ]]; then
            qa_check "${end}_identity" 1 "missing $idf"
            continue
        fi
        arch=$(id_get "$idf" arch)
        krel=$(id_get "$idf" kernel)
        ver=$(id_get "$idf" version)
        sv=$(id_get "$idf" srcversion)
        vm=$(id_get "$idf" vermagic)
        sha=$(id_get "$idf" ko_sha256)
        size=$(id_get "$idf" ko_size)
        warn=$(id_get "$idf" build_warnings)
        case $end in
            near)
                NEAR_ARCH=$arch NEAR_KREL=$krel NEAR_VER=$ver NEAR_SV=$sv
                NEAR_VMAGIC=$vm NEAR_KO_SHA=$sha NEAR_BUILD_WARN=$warn
                ;;
            far)
                FAR_ARCH=$arch FAR_KREL=$krel FAR_VER=$ver FAR_SV=$sv
                FAR_VMAGIC=$vm FAR_KO_SHA=$sha FAR_BUILD_WARN=$warn
                ;;
        esac
        if [[ -n $EXPECT_VERSION ]]; then
            qa_check "${end}_module_version" "$([[ $ver == "$EXPECT_VERSION" ]]; echo $?)" \
                "version=${ver:-?} want=$EXPECT_VERSION"
        else
            qa_skip "${end}_module_version" "EXPECT_MODULE_VERSION is empty"
        fi
        qa_check "${end}_srcversion" \
            "$(case ${sv:-} in '' | *[!0-9A-Fa-f]*) echo 1 ;; *) echo 0 ;; esac)" \
            "srcversion=${sv:-?}"
        case $vm in
            "$krel "*) qa_check "${end}_vermagic" 0 "vermagic=$vm" ;;
            *) qa_check "${end}_vermagic" 1 "vermagic=${vm:-?} kernel=${krel:-?}" ;;
        esac
        qa_check "${end}_ko_sha256" \
            "$(case ${sha:-} in [0-9a-f][0-9a-f]*) [[ ${#sha} -eq 64 ]] && echo 0 || echo 1 ;; *) echo 1 ;; esac)" \
            "sha256=${sha:-?} size=${size:-?} warnings=${warn:-?}"
        if [[ $end == near ]]; then want=${EXPECT_NEAR_KO_SHA:-}; else want=${EXPECT_FAR_KO_SHA:-}; fi
        if [[ -n $want ]]; then
            qa_check "${end}_ko_sha_pin" "$([[ $sha == "$want" ]]; echo $?)" \
                "sha256=$sha want=$want"
        else
            qa_skip "${end}_ko_sha_pin" "no pin"
        fi
        if [[ $end == near ]]; then want=${EXPECT_NEAR_SRCVERSION:-}; else want=${EXPECT_FAR_SRCVERSION:-}; fi
        if [[ -n $want ]]; then
            qa_check "${end}_srcversion_pin" "$([[ $sv == "$want" ]]; echo $?)" \
                "srcversion=$sv want=$want"
        else
            qa_skip "${end}_srcversion_pin" "no pin"
        fi
        note "$end identity: arch=$arch kernel=$krel version=$ver srcversion=$sv sha=$sha warnings=$warn"
    done
}

g0_run() {
    stage_start
    if ! g0_archive; then
        G0_FAILED=1
        return
    fi
    local rn=0 rf=0 pn pf
    g0_build_host near "$JOBS_NEAR" &
    pn=$!
    g0_build_host far "$JOBS_FAR" &
    pf=$!
    wait "$pn" || rn=$?
    wait "$pf" || rf=$?
    qa_check near_build "$rn" "rc=$rn log=g0-near-build.log"
    qa_check far_build "$rf" "rc=$rf log=g0-far-build.log"
    local end got
    for end in near far; do
        got=$(cat "$EV/g0-$end-archive.sha" 2>/dev/null || true)
        qa_check "${end}_archive_sha" "$([[ $got == "$ARCHIVE_SHA" ]]; echo $?)" \
            "host=${got:-?} local=$ARCHIVE_SHA"
    done
    if ((rn == 0 && rf == 0)); then
        g0_identity_checks
    else
        qa_skip g0_identity "a build failed; identity checks skipped"
    fi
    if stage_failed; then G0_FAILED=1; fi
}

# ---------------------------------------------------------------------------
# G1: preflight, deploy, config, join, far source

near_watchdog() {
    gate_ssh near "$SSH_TMO" 'bash -s' <<'EOS' 2>/dev/null || true
set -u
printf 'ACTIVE=%s\n' "$(systemctl is-active tbtun-netwatch 2>/dev/null || true)"
printf 'MAINPID=%s\n' "$(systemctl show -p MainPID --value tbtun-netwatch 2>/dev/null || true)"
printf 'RUNTIME=%s\n' "$(systemctl show -p RuntimeWatchdogUSec --value tbtun-netwatch 2>/dev/null || true)"
holder=
for fddir in /proc/[0-9]*/fd; do
    if ls -l "$fddir" 2>/dev/null | grep -qF /dev/watchdog0; then
        pid=${fddir#/proc/}
        pid=${pid%/fd}
        holder="pid=$pid($(cat "/proc/$pid/comm" 2>/dev/null || echo '?'))"
        break
    fi
done
printf 'HOLDER=%s\n' "${holder:-none}"
EOS
}

g1_far_port_free() {
    local out busy pid cmd
    out=$(gate_ssh far "$SSH_TMO" "RUN='$FAR_RUN' PORT='$FAR_PORT' bash -s" 2>&1 <<'EOS'
set -u
busy=$(ss -ltnH "sport = :$PORT" 2>/dev/null | wc -l)
printf 'PORT_BUSY=%s\n' "$busy"
if [ "$busy" -eq 0 ]; then
    exit 0
fi
pid=$(ss -ltnpH "sport = :$PORT" 2>/dev/null |
    sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | head -n 1)
printf 'PORT_PID=%s\n' "${pid:-}"
if [ -n "${pid:-}" ]; then
    cmd=$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null || true)
    case "$cmd" in
        *"$RUN"*)
            kill "$pid" 2>/dev/null || true
            i=0
            while [ "$i" -lt 5 ] && kill -0 "$pid" 2>/dev/null; do
                sleep 1
                i=$((i + 1))
            done
            kill -9 "$pid" 2>/dev/null || true
            printf 'PORT_OWNED=1\n'
            ;;
        *) printf 'PORT_OWNED=0\n' ;;
    esac
fi
sleep 1
printf 'PORT_FREE_AFTER=%s\n' "$(ss -ltnH "sport = :$PORT" 2>/dev/null | wc -l)"
EOS
)
    printf '%s\n' "$out" >"$EV/g1-far-port.txt"
    busy=$(sed -n 's/^PORT_BUSY=//p' <<<"$out" | tail -n 1)
    if [[ ${busy:-1} == 0 ]]; then
        qa_check g1_far_port 0 "127.0.0.1:$FAR_PORT free"
        return 0
    fi
    pid=$(sed -n 's/^PORT_OWNED=//p' <<<"$out" | tail -n 1)
    if [[ ${pid:-0} == 1 ]]; then
        local free_after
        free_after=$(sed -n 's/^PORT_FREE_AFTER=//p' <<<"$out" | tail -n 1)
        if [[ ${free_after:-1} == 0 ]]; then
            qa_check g1_far_port 0 "stopped this tag's stale listener on $FAR_PORT"
            return 0
        fi
        qa_check g1_far_port 1 "this tag's listener on $FAR_PORT did not stop"
        return 1
    fi
    qa_check g1_far_port 1 "127.0.0.1:$FAR_PORT is held by a foreign process; stop it first"
    return 1
}

ip_rule_ensure() {
    local out exists added owned dev
    out=$(gate_ssh near "$SSH_TMO" \
        "FAR_IP='$FAR_IP' PREF='$BYPASS_PREF' MARK='$NEAR_RUN/ip-rule-owned' bash -s" \
        2>&1 <<'EOS'
set -u
owned=0
if [ -f "$MARK" ] && grep -qF "to=$FAR_IP/32" "$MARK" 2>/dev/null; then
    owned=1
fi
exists=0
if ip rule show 2>/dev/null | awk -v p="$PREF:" -v ip="$FAR_IP" '
    $1 == p {
        for (i = 2; i < NF; i++)
            if ($i == "to" && ($(i + 1) == ip || $(i + 1) == ip "/32")) found = 1
    }
    END { exit(found ? 0 : 1) }'; then
    exists=1
fi
added=0
if [ "$exists" -eq 0 ]; then
    if ip rule add to "$FAR_IP/32" lookup main pref "$PREF" 2>/dev/null; then
        mkdir -p "$(dirname "$MARK")"
        printf 'pref=%s to=%s/32 lookup=main\n' "$PREF" "$FAR_IP" >"$MARK"
        added=1
        exists=1
    fi
fi
dev=$(ip route get "$FAR_IP" 2>/dev/null |
    awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }')
printf 'RULE_EXISTS=%s\n' "$exists"
printf 'RULE_ADDED=%s\n' "$added"
printf 'RULE_OWNED=%s\n' "$owned"
printf 'ROUTE_DEV=%s\n' "${dev:-none}"
EOS
)
    printf '%s\n' "$out" >"$EV/g1-ip-rule.txt"
    exists=$(sed -n 's/^RULE_EXISTS=//p' <<<"$out" | tail -n 1)
    added=$(sed -n 's/^RULE_ADDED=//p' <<<"$out" | tail -n 1)
    owned=$(sed -n 's/^RULE_OWNED=//p' <<<"$out" | tail -n 1)
    dev=$(sed -n 's/^ROUTE_DEV=//p' <<<"$out" | tail -n 1)
    NEAR_IP_RULE_ADDED=${added:-0}
    NEAR_IP_RULE_OWNED=$([[ ${owned:-0} == 1 || ${added:-0} == 1 ]] && echo 1 || echo 0)
    qa_check g1_ip_rule "$([[ ${exists:-0} == 1 ]]; echo $?)" \
        "pref=$BYPASS_PREF to=$FAR_IP/32 added=${NEAR_IP_RULE_ADDED}"
    case ${dev:-none} in
        '' | none | Meta | tun* | utun* | wg* | tailscale*)
            qa_check g1_route_dev 1 "route dev=${dev:-none}; need a physical device"
            ;;
        *)
            if [[ -n $NEAR_DEV && $dev != "$NEAR_DEV" ]]; then
                qa_check g1_route_dev 1 "route dev=$dev but NEAR_DEV=$NEAR_DEV"
            else
                qa_check g1_route_dev 0 "route dev=$dev"
            fi
            NEAR_PHYS_DEV=$dev
            ;;
    esac
}

clean_unload() { # near|far
    local end=$1 out
    out=$(gate_ssh "$end" 180 "PROC='$PROC_ROOT' bash -s" 2>&1 <<'EOS'
set -u
if [ -e "$PROC/config" ]; then
    timeout 10 sh -c "printf 'del 1\n' > '$PROC/config'" 2>/dev/null || true
fi
if lsmod 2>/dev/null | awk '$1 == "tbtun" { f = 1 } END { exit !f }'; then
    timeout 60 rmmod tbtun 2>/dev/null || true
fi
if lsmod 2>/dev/null | awk '$1 == "tbtun" { f = 1 } END { exit !f }'; then
    echo MODULE_LOADED
else
    echo MODULE_UNLOADED
fi
EOS
)
    printf '%s\n' "$out" >"$EV/g1-unload-$end.txt"
    if grep -qx 'MODULE_UNLOADED' <<<"$out"; then
        qa_check "${end}_unload" 0 "tbtun unloaded"
        return 0
    fi
    gate_ssh "$end" "$SSH_TMO" "ps -eo stat,pid,comm 2>/dev/null | awk '\$1 ~ /^D/'" \
        >"$EV/g1-unload-$end-dstate.txt" 2>/dev/null || true
    qa_check "${end}_unload" 1 "rmmod did not complete; see g1-unload-$end.txt"
    return 1
}

insmod_end() { # end ko staged-version staged-srcversion staged-vermagic
    local end=$1 ko=$2 ver=$3 sv=$4 vm=$5 rc=0
    local lver lsv lvm lmax krel vm_rc vm_src
    gate_ssh "$end" 120 "insmod '$ko' max_carriers=$CARRIERS max_tunnels=1 debug=$GATE_DEBUG" \
        >"$EV/g1-insmod-$end.txt" 2>&1 || rc=$?
    qa_check "${end}_insmod" "$rc" "ko=$ko rc=$rc"
    ((rc == 0)) || return 1
    lver=$(gate_ssh "$end" "$SSH_TMO" "cat /sys/module/tbtun/version 2>/dev/null" 2>/dev/null | tr -d '[:space:]')
    lsv=$(gate_ssh "$end" "$SSH_TMO" "cat /sys/module/tbtun/srcversion 2>/dev/null" 2>/dev/null | tr -d '[:space:]')
    lmax=$(gate_ssh "$end" "$SSH_TMO" "cat /sys/module/tbtun/parameters/max_carriers 2>/dev/null" 2>/dev/null | tr -d '[:space:]')
    lvm=$(gate_ssh "$end" "$SSH_TMO" "cat /sys/module/tbtun/vermagic 2>/dev/null" 2>/dev/null)
    if [[ -n $lvm ]]; then
        vm_rc=$([[ $lvm == "$vm" ]]; echo $?)
        vm_src=sysfs
    else
        # Linux exposes no /sys/module/*/vermagic on most kernels. The ko
        # was insmod'ed (rc=0); accept its built vermagic when it targets
        # this kernel and the loaded srcversion matches that ko.
        krel=$(gate_ssh "$end" "$SSH_TMO" "uname -r" 2>/dev/null | tr -d '[:space:]')
        case $vm in
            "$krel "*) vm_rc=0 ;;
            *) vm_rc=1 ;;
        esac
        [[ -n $sv && $lsv == "$sv" ]] || vm_rc=1
        vm_src="sysfs-absent kernel=${krel:-?}"
    fi
    qa_check "${end}_loaded_version" "$([[ $lver == "$ver" ]]; echo $?)" \
        "loaded=${lver:-?} built=${ver:-?}"
    qa_check "${end}_loaded_srcversion" "$([[ $lsv == "$sv" ]]; echo $?)" \
        "loaded=${lsv:-?} built=${sv:-?}"
    qa_check "${end}_loaded_vermagic" "$vm_rc" \
        "loaded=${lvm:-?} built=${vm:-?} src=$vm_src"
    qa_check "${end}_max_carriers" "$([[ $lmax == "$CARRIERS" ]]; echo $?)" \
        "max_carriers=${lmax:-?}"
    { printf 'version=%s\nsrcversion=%s\nvermagic=%s\nmax_carriers=%s\n' \
        "$lver" "$lsv" "$lvm" "$lmax"; } >"$EV/g1-loaded-$end.txt"
}

make_configs() {
    local pre post dev=
    NONCE=$(od -An -N16 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')
    [[ ${#NONCE} -eq 32 ]] || NONCE=$(printf '%032x' "$(date +%s)")
    [[ -n $NEAR_DEV ]] && dev=" dev=$NEAR_DEV"
    pre="fec=on adaptive=off mode=sc-ldpc"
    post="crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=16777216 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000"
    FAR_CFG="tunnel 1 role=far listen=0.0.0.0:$FAR_LISTEN_PORT target=127.0.0.1:$FAR_PORT n=128 m=128 $pre w=256 carriers=$CARRIERS $post portblock=$FAR_PORTBLOCK L=8 nonce=$NONCE"
    NEAR_CFG="tunnel 1 role=near listen=127.0.0.1:$NEAR_PORT peer=$FAR_IP:$FAR_LISTEN_PORT n=8 m=24 $pre w=128 carriers=$CARRIERS $post portblock=$NEAR_PORTBLOCK L=16 nonce=$NONCE$dev"
}

write_config() { # end config outfile
    local end=$1 cfg=$2 out=$3 rc=0
    printf '%s\n' "$cfg" | gate_ssh "$end" "$SSH_TMO" "cat > '$PROC_ROOT/config'" || rc=$?
    gate_ssh "$end" "$SSH_TMO" "cat '$PROC_ROOT/config' 2>/dev/null" >"$out" 2>/dev/null || true
    qa_check "${end}_config_write" "$rc" "rc=$rc"
    ((rc == 0))
}

check_config_tokens() { # end file token...
    local end=$1 file=$2
    shift 2
    local tok
    for tok in "$@"; do
        qa_check "${end}_cfg_${tok%%=*}" "$(cfg_has "$file" "$tok"; echo $?)" "$tok"
    done
}

join_wait() {
    local i=0 misses=0 nu fu nn nrep nemp frep femp
    : >"$EV/g1-join-probes.txt"
    while ((i < JOIN_TIMEOUT)); do
        snap_stats near "$EV/g1-join-near.tmp"
        snap_stats far "$EV/g1-join-far.tmp"
        nn=$(stat_field "$EV/g1-join-near.tmp" c3 n)
        nu=$(stat_field "$EV/g1-join-near.tmp" c3 up)
        nrep=$(stat_field "$EV/g1-join-near.tmp" c3 replacing)
        nemp=$(stat_field "$EV/g1-join-near.tmp" c3 empty)
        fu=$(stat_field "$EV/g1-join-far.tmp" c3 up)
        frep=$(stat_field "$EV/g1-join-far.tmp" c3 replacing)
        femp=$(stat_field "$EV/g1-join-far.tmp" c3 empty)
        printf '%s near_up=%s near_replacing=%s near_empty=%s far_up=%s far_replacing=%s far_empty=%s\n' \
            "$(date -u +%H:%M:%SZ)" "${nu:-?}" "${nrep:-?}" "${nemp:-?}" \
            "${fu:-?}" "${frep:-?}" "${femp:-?}" >>"$EV/g1-join-probes.txt"
        if [[ -z ${nu:-} && -z ${fu:-} ]]; then
            misses=$((misses + 1))
            ((misses >= 5)) && break
        else
            misses=0
        fi
        if num "${nu:-}" && num "${nrep:-}" && num "${nemp:-}" &&
            num "${fu:-}" && num "${frep:-}" && num "${femp:-}" &&
            num "${nn:-}" && ((nn > 0)) &&
            ((nu >= MIN_UP && fu >= MIN_UP)) &&
            ((nu + nrep + nemp == nn)) &&
            ((fu + frep + femp == nn)); then
            break
        fi
        sleep 1
        i=$((i + 1))
    done
    JOIN_NEAR_UP=${nu:-?} JOIN_NEAR_REP=${nrep:-?} JOIN_NEAR_EMPTY=${nemp:-?}
    JOIN_FAR_UP=${fu:-?} JOIN_FAR_REP=${frep:-?} JOIN_FAR_EMPTY=${femp:-?}
    JOIN_N=${nn:-?} JOIN_SECONDS=$i
    qa_check g1_near_join "$([[ ${nu:-0} -ge $MIN_UP ]]; echo $?)" \
        "up=${nu:-?} min=$MIN_UP after ${i}s"
    qa_check g1_far_join "$([[ ${fu:-0} -ge $MIN_UP ]]; echo $?)" \
        "up=${fu:-?} min=$MIN_UP after ${i}s"
    local ninv=1 finv=1
    (( ${nn:-0} > 0 && ${nu:-0} + ${nrep:-0} + ${nemp:-0} == ${nn:-0} )) && ninv=0
    (( ${nn:-0} > 0 && ${fu:-0} + ${frep:-0} + ${femp:-0} == ${nn:-0} )) && finv=0
    qa_check g1_near_invariant "$ninv" \
        "n=${nn:-?} up=${nu:-?} replacing=${nrep:-?} empty=${nemp:-?}"
    qa_check g1_far_invariant "$finv" \
        "n=${nn:-?} up=${fu:-?} replacing=${frep:-?} empty=${femp:-?}"
    local ne fe
    ne=$([[ -s $EV/g1-join-near.tmp ]]; echo $?)
    fe=$([[ -s $EV/g1-join-far.tmp ]]; echo $?)
    qa_check g1_near_stats "$ne" "stats snapshot readable"
    qa_check g1_far_stats "$fe" "stats snapshot readable"
}

listener_check() { # end port
    local n
    n=$(gate_ssh "$1" "$SSH_TMO" "ss -ltnH 'sport = :$2' 2>/dev/null | wc -l" 2>/dev/null | tr -d '[:space:]')
    qa_check "${1}_listen_$2" "$([[ ${n:-0} -gt 0 ]]; echo $?)" "listeners=$n"
}

peer_shape() { # end mark tokens
    local end=$1 mark=$2 tokens=$3 f ok=0 tok
    f=$EV/g1-peer-shape-$end.txt
    gate_ssh "$end" "$SSH_TMO" \
        "dmesg 2>/dev/null | tail -n +$((mark + 1)) | grep -F 'fec rx shape from peer' | tail -n 1" \
        >"$f" 2>/dev/null || true
    if [[ ! -s $f ]]; then
        qa_skip "${end}_peer_shape" "not logged after dmesg mark=$mark"
        return
    fi
    for tok in $tokens; do
        cfg_has "$f" "$tok" || ok=1
    done
    qa_check "${end}_peer_shape" "$ok" "$(head -n 1 "$f")"
}

g1_preflight() {
    local before=$FAILED
    if ! ssh_near true >/dev/null 2>&1; then
        qa_check near_ssh 1 "unreachable $NEAR_HOST"
    else
        qa_check near_ssh 0 "reachable $NEAR_HOST"
    fi
    if ! ssh_far true >/dev/null 2>&1; then
        qa_check far_ssh 1 "unreachable $FAR_HOST"
    else
        qa_check far_ssh 0 "reachable $FAR_HOST"
    fi
    local wd active holder
    wd=$(near_watchdog)
    printf '%s\n' "$wd" >"$EV/g1-watchdog-pre.txt"
    active=$(sed -n 's/^ACTIVE=//p' <<<"$wd" | tail -n 1)
    holder=$(sed -n 's/^HOLDER=//p' <<<"$wd" | tail -n 1)
    qa_check near_watchdog_pre_active "$([[ $active == active ]]; echo $?)" "state=${active:-?}"
    qa_check near_watchdog_pre_holder "$([[ -n $holder && $holder != none ]]; echo $?)" "${holder:-none}"
    local na fa
    na=$(gate_ssh near "$SSH_TMO" 'for t in curl dd sha256sum awk ss stat ip; do command -v $t >/dev/null || echo missing:$t; done' 2>/dev/null)
    fa=$(gate_ssh far "$SSH_TMO" 'for t in python3 dd sha256sum curl ss awk stat; do command -v $t >/dev/null || echo missing:$t; done' 2>/dev/null)
    qa_check near_tools "$([[ -z $na ]]; echo $?)" "${na:-curl dd sha256sum awk ss stat ip}"
    qa_check far_tools "$([[ -z $fa ]]; echo $?)" "${fa:-python3 dd sha256sum curl ss awk stat}"
    local nk fk nmem
    nk=$(gate_ssh near "$SSH_TMO" "df -P -k /root | awk 'NR==2{print \$4}'" 2>/dev/null | tr -d '[:space:]')
    fk=$(gate_ssh far "$SSH_TMO" "df -P -k /root | awk 'NR==2{print \$4}'" 2>/dev/null | tr -d '[:space:]')
    qa_check near_disk "$([[ ${nk:-0} -ge 1572864 ]]; echo $?)" "free=${nk:-?}KiB need=1572864KiB"
    qa_check far_disk "$([[ ${fk:-0} -ge 1572864 ]]; echo $?)" "free=${fk:-?}KiB need=1572864KiB"
    nmem=$(gate_ssh far "$SSH_TMO" "awk '/^MemAvailable:/{print \$2}' /proc/meminfo" 2>/dev/null | tr -d '[:space:]')
    qa_check far_mem "$([[ ${nmem:-0} -ge 131072 ]]; echo $?)" "MemAvailable=${nmem:-?}KiB; far builds with -j$JOBS_FAR"
    FAR_PHYS_DEV=$(gate_ssh far "$SSH_TMO" "ip route get 1.1.1.1 2>/dev/null" 2>/dev/null |
        awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }')
    g1_far_port_free || true
    local end sa
    for end in near far; do
        sa=$(gate_ssh "$end" "$SSH_TMO" "awk -v t='$TUNNEL' '\$1==\"tunnel\" && \$2==t && \$3==\"streams\" {
            for (i=1;i<=NF;i++) if (\$i ~ /^streams_active=/) { sub(\"streams_active=\",\"\",\$i); print \$i; exit } }' '$PROC_ROOT/stats' 2>/dev/null" 2>/dev/null || true)
        if [[ -z $sa ]]; then
            qa_skip "${end}_streams_pre" "no live session"
        else
            qa_check "${end}_streams_pre" "$([[ $sa == 0 ]]; echo $?)" "streams_active=$sa"
        fi
    done
    ((FAILED == before))
}

g1_far_source() {
    local out rc=0
    gate_ssh far "$SSH_TMO" "mkdir -p '$FAR_RUN/helpers'" || return 1
    scp_far "$QA_REPO/" \
        "$FAR_HOST:$FAR_RUN/helpers/far-serve.sh" || return 1
    scp_far "$QA_REPO/" \
        "$FAR_HOST:$FAR_RUN/helpers/far-raw-serve.py" || return 1
    out=$(gate_ssh far "$FAR_START_TMO" \
        "FAR_DIR='$FAR_ACCEPT' RAW_PY='$FAR_RUN/helpers/far-raw-serve.py' PORT='$FAR_PORT' sh '$FAR_RUN/helpers/far-serve.sh' --start" 2>&1) || rc=$?
    printf '%s\n' "$out" >"$EV/g1-far-serve.log"
    qa_check g1_far_source "$rc" "rc=$rc"
    qa_check g1_far_source_result "$(grep -q '^FAR_RESULT PASS' <<<"$out"; echo $?)" \
        "see g1-far-serve.log"
    FAR_SHA=$(sed -n 's/^FAR_SHA256=//p' <<<"$out" | tail -n 1)
    FAR_SIZE=$(sed -n 's/^FAR_SIZE=//p' <<<"$out" | tail -n 1)
    qa_check g1_far_size "$([[ ${FAR_SIZE:-0} == "$EXPECT_BYTES" ]]; echo $?)" \
        "size=${FAR_SIZE:-?} want=$EXPECT_BYTES"
    qa_check g1_far_sha "$([[ -n $FAR_SHA ]]; echo $?)" "sha256=${FAR_SHA:-?}"
    local code
    code=$(gate_ssh far "$SSH_TMO" "curl -sS -o /dev/null -w '%{http_code}' -I -m 5 http://127.0.0.1:$FAR_PORT/ 2>/dev/null" 2>/dev/null || true)
    qa_check g1_far_http "$([[ $code == 200 ]]; echo $?)" "loopback http=$code"
    ((rc == 0))
}

g1_run() {
    stage_start
    if ((G0_FAILED)); then
        G1_FAILED=1
        qa_skip g1 "G0 failed; no deployment"
        return
    fi
    if ! g1_preflight; then
        G1_FAILED=1
        capture_diag g1-preflight || true
        return
    fi
    ip_rule_ensure
    if stage_failed; then
        G1_FAILED=1
        return
    fi
    if ! clean_unload far; then
        G1_FAILED=1
        return
    fi
    if ! clean_unload near; then
        G1_FAILED=1
        return
    fi
    NEAR_DMESG_MARK=$(gate_ssh near "$SSH_TMO" "dmesg 2>/dev/null | wc -l" 2>/dev/null | tr -d '[:space:]')
    FAR_DMESG_MARK=$(gate_ssh far "$SSH_TMO" "dmesg 2>/dev/null | wc -l" 2>/dev/null | tr -d '[:space:]')
    case ${NEAR_DMESG_MARK:-} in '' | *[!0-9]*) NEAR_DMESG_MARK=0 ;; esac
    case ${FAR_DMESG_MARK:-} in '' | *[!0-9]*) FAR_DMESG_MARK=0 ;; esac
    if ! insmod_end far "$FAR_KO" "$FAR_VER" "$FAR_SV" "$FAR_VMAGIC"; then
        G1_FAILED=1
        return
    fi
    if ! insmod_end near "$NEAR_KO" "$NEAR_VER" "$NEAR_SV" "$NEAR_VMAGIC"; then
        G1_FAILED=1
        return
    fi
    make_configs
    if ! write_config far "$FAR_CFG" "$EV/g1-config-far.txt"; then
        G1_FAILED=1
        return
    fi
    if ! write_config near "$NEAR_CFG" "$EV/g1-config-near.txt"; then
        G1_FAILED=1
        return
    fi
    check_config_tokens far "$EV/g1-config-far.txt" \
        "role=far" "mode=sc-ldpc" "carriers=$CARRIERS" "n=128" "m=128" "w=256" \
        "L=8" "adaptive=off" "fec=on" "crypto=on" "profile=http-kal" \
        "target=127.0.0.1:$FAR_PORT" "portblock=$FAR_PORTBLOCK"
    check_config_tokens near "$EV/g1-config-near.txt" \
        "role=near" "mode=sc-ldpc" "carriers=$CARRIERS" "n=8" "m=24" "w=128" \
        "L=16" "adaptive=off" "fec=on" "crypto=on" "profile=http-kal" \
        "listen=127.0.0.1:$NEAR_PORT" "peer=$FAR_IP:$FAR_LISTEN_PORT" \
        "portblock=$NEAR_PORTBLOCK"
    if [[ -n $NEAR_DEV ]]; then
        qa_check near_cfg_dev "$(cfg_has "$EV/g1-config-near.txt" "dev=$NEAR_DEV"; echo $?)" "dev=$NEAR_DEV"
    else
        qa_skip near_cfg_dev "NEAR_DEV is empty"
    fi
    listener_check near "$NEAR_PORT"
    listener_check far "$FAR_LISTEN_PORT"
    join_wait
    peer_shape near "$NEAR_DMESG_MARK" "n=128 m=128 groups=1 slots=8 block=256 L=8 rate=500"
    peer_shape far "$FAR_DMESG_MARK" "n=8 m=24 groups=8 slots=8 block=128 L=16 rate=64"
    local ne fe
    ne=$(any_field "$EV/g1-join-near.tmp" rx_errs)
    fe=$(any_field "$EV/g1-join-far.tmp" rx_errs)
    qa_check g1_near_rx_errs "$([[ ${ne:-?} == 0 ]]; echo $?)" "rx_errs=${ne:-?}"
    qa_check g1_far_rx_errs "$([[ ${fe:-?} == 0 ]]; echo $?)" "rx_errs=${fe:-?}"
    if stage_failed; then
        G1_FAILED=1
        capture_diag g1 || true
        return
    fi
    if ! g1_far_source; then
        G1_FAILED=1
        capture_diag g1 || true
        return
    fi
    if stage_failed; then G1_FAILED=1; fi
}

capture_diag() { # stage
    local stage=$1 end
    for end in near far; do
        gate_ssh "$end" "$SSH_TMO" "bash -s" >"$EV/diag-$stage-$end.txt" 2>&1 <<EOS || true
printf '=== date ===\n'; date -u +%Y-%m-%dT%H:%M:%SZ
printf '=== uname ===\n'; uname -a
printf '=== module ===\n'; lsmod | grep -E '^tbtun' || true
printf '=== config ===\n'; cat '$PROC_ROOT/config' 2>/dev/null || true
printf '=== stats ===\n'; cat '$PROC_ROOT/stats' 2>/dev/null || true
printf '=== tunnels ===\n'; cat '$PROC_ROOT/tunnels' 2>/dev/null || true
printf '=== ss ===\n'; ss -ltnp 2>/dev/null || true
printf '=== dmesg ===\n'; dmesg 2>/dev/null | tail -n 400 || true
printf '=== ps ===\n'; ps -eo stat,pid,comm 2>/dev/null || true
EOS
    done
    note "diagnostics captured for stage=$stage"
}

# ---------------------------------------------------------------------------
# G2: transfers

snap_all() { # prefix
    local p=$1 end
    for end in near far; do
        snap_stats "$end" "$EV/$p-$end-stats.txt"
        snap_proc "$end" "$EV/$p-$end-procstat.txt"
        snap_netdev "$end" "$EV/$p-$end-netdev.txt"
    done
}

start_bg_curl() { # script-path
    local i remote
    remote=/tmp/fastgate-$(basename -- "$1")
    scp_near "$1" "$NEAR_HOST:$remote" || return 1
    gate_ssh near "$SSH_TMO" "rm -f /tmp/fastgate.pid /tmp/fastgate.out; setsid sh '$remote' </dev/null >/tmp/fastgate.out 2>&1 & echo started" >/dev/null || return 1
    BG_PID=
    for i in {1..5}; do
        sleep 1
        BG_PID=$(gate_ssh near "$SSH_TMO" "cat /tmp/fastgate.pid 2>/dev/null" 2>/dev/null | tr -d '[:space:]')
        [[ -n $BG_PID ]] && break
    done
    [[ -n $BG_PID ]]
}

stop_bg_curl() {
    local pid=${BG_PID:-}
    gate_ssh near "$SSH_TMO" "p='$pid'; if [ -n \"\$p\" ]; then kill \"\$p\" 2>/dev/null || true; sleep 1; kill -9 \"\$p\" 2>/dev/null || true; fi; true" >/dev/null 2>&1 || true
}

g2_down_run() {
    local before=$FAILED kw0 kw1
    say "== G2a: ${DOWN_SECONDS}s downlink (far 1 GiB -> near curl)"
    mark g2-down-start
    wait_streams_clear g2_down
    snap_all g2-down-a
    cat >"$TMP/down.sh" <<EOF
#!/bin/sh
echo \$\$ > /tmp/fastgate.pid
exec curl -sS -m $DOWN_SECONDS -o /dev/null -w 'http=%{http_code} size=%{size_download} ttfb=%{time_starttransfer} time=%{time_total} speed=%{speed_download}\n' http://127.0.0.1:$NEAR_PORT/
EOF
    DOWN_PID=
    kw0=$(date +%s)
    if ! start_bg_curl "$TMP/down.sh"; then
        qa_check g2_down_start 1 "curl did not start"
        G2_FAILED=1
        return
    fi
    DOWN_PID=$BG_PID
    qa_check g2_down_start 0 "pid=$DOWN_PID"
    sleep "$((DOWN_SECONDS + 3))"
    stop_bg_curl
    gate_ssh near "$SSH_TMO" "cat /tmp/fastgate.out 2>/dev/null" >"$EV/g2-down-curl.txt" 2>/dev/null || true
    snap_all g2-down-b
    kw1=$(date +%s)
    DOWN_HTTP=$(kv_file "$EV/g2-down-curl.txt" http)
    DOWN_SIZE=$(kv_file "$EV/g2-down-curl.txt" size)
    DOWN_TIME=$(kv_file "$EV/g2-down-curl.txt" time)
    DOWN_SPEED=$(kv_file "$EV/g2-down-curl.txt" speed)
    DOWN_NEAR_RX=$(delta "$(any_field "$EV/g2-down-a-near-stats.txt" rx_delivered)" \
        "$(any_field "$EV/g2-down-b-near-stats.txt" rx_delivered)")
    DOWN_FAR_SUBMIT=$(delta "$(any_field "$EV/g2-down-a-far-stats.txt" submit_bytes)" \
        "$(any_field "$EV/g2-down-b-far-stats.txt" submit_bytes)")
    if [[ -n $NEAR_PHYS_DEV ]]; then
        DOWN_WIRE=$(delta "$(netdev_rx "$EV/g2-down-a-near-netdev.txt" "$NEAR_PHYS_DEV")" \
            "$(netdev_rx "$EV/g2-down-b-near-netdev.txt" "$NEAR_PHYS_DEV")")
    fi
    local nb ni nb2 ni2 fb fi fb2 fi2
    read -r nb ni < <(cpu_busy "$EV/g2-down-a-near-procstat.txt") || true
    read -r nb2 ni2 < <(cpu_busy "$EV/g2-down-b-near-procstat.txt") || true
    DOWN_NEAR_CPU=$(cpu_pct "${nb:-0}" "${ni:-0}" "${nb2:-0}" "${ni2:-0}")
    read -r fb fi < <(cpu_busy "$EV/g2-down-a-far-procstat.txt") || true
    read -r fb2 fi2 < <(cpu_busy "$EV/g2-down-b-far-procstat.txt") || true
    DOWN_FAR_CPU=$(cpu_pct "${fb:-0}" "${fi:-0}" "${fb2:-0}" "${fi2:-0}")
    g2_report g2-down
    qa_check g2_down_http "$([[ ${DOWN_HTTP:-000} == 200 ]]; echo $?)" \
        "http=${DOWN_HTTP:-000} (headers arrive before the ${DOWN_SECONDS}s cut)"
    qa_check g2_down_size "$([[ ${DOWN_SIZE:-0} -gt 0 ]]; echo $?)" "size_download=${DOWN_SIZE:-?}"
    qa_check g2_down_far_submit "$([[ ${DOWN_FAR_SUBMIT:-0} -gt 0 ]]; echo $?)" \
        "far submit_bytes=${DOWN_FAR_SUBMIT:-?}"
    qa_check g2_down_near_rx "$([[ ${DOWN_NEAR_RX:-0} -gt 0 ]]; echo $?)" \
        "near rx_delivered=${DOWN_NEAR_RX:-?}"
    for end in near far; do
        check_delta_zero "g2_down_${end}_stream_reset_tcp" \
            "$EV/g2-down-a-$end-stats.txt" "$EV/g2-down-b-$end-stats.txt" c3 stream_reset_tcp_failed
        check_delta_zero "g2_down_${end}_stream_reset_user" \
            "$EV/g2-down-a-$end-stats.txt" "$EV/g2-down-b-$end-stats.txt" c3 stream_reset_user
        check_delta_zero "g2_down_${end}_fec_reset_tcp" \
            "$EV/g2-down-a-$end-stats.txt" "$EV/g2-down-b-$end-stats.txt" c11 fec_reset_tcp_failed
        # The 60 s cut is an application teardown (curl -m). It is expected to
        # kill the FEC stream; only the fault reasons must stay zero.
        qa_skip "g2_down_${end}_fec_reset_user" \
            "application teardown delta=$(delta "$(stat_field "$EV/g2-down-a-$end-stats.txt" c11 fec_reset_user)" "$(stat_field "$EV/g2-down-b-$end-stats.txt" c11 fec_reset_user)")"
        check_delta_zero "g2_down_${end}_fec_reset_hole" \
            "$EV/g2-down-a-$end-stats.txt" "$EV/g2-down-b-$end-stats.txt" c11 fec_reset_hole
        check_delta_zero "g2_down_${end}_holes_unrecoverable" \
            "$EV/g2-down-a-$end-stats.txt" "$EV/g2-down-b-$end-stats.txt" c6 holes_unrecoverable
        check_delta_zero "g2_down_${end}_deliver_bad" \
            "$EV/g2-down-a-$end-stats.txt" "$EV/g2-down-b-$end-stats.txt" c10 fec_deliver_bad
        check_any_delta_zero "g2_down_${end}_rx_errs" \
            "$EV/g2-down-a-$end-stats.txt" "$EV/g2-down-b-$end-stats.txt" rx_errs
    done
    check_streams_after g2_down
    DOWN_STREAMS_FINAL="$(streams_active near)/$(streams_active far)"
    check_kernel_window g2_down "$kw0" "$kw1"
    if ((FAILED > before)); then
        G2_FAILED=1
        capture_diag g2-down || true
    fi
    mark g2-down-done
}

g2_up_run() {
    local before=$FAILED kw0 kw1 i
    say "== G2b: ${UP_SECONDS}s uplink (near ${UP_MIB} MiB -> far receiver) + ${PROBE_BYTES}-byte probe"
    mark g2-up-start
    if ! stop_far_source; then
        qa_check g2_up_far_port 1 "127.0.0.1:$FAR_PORT did not free after stopping far-serve"
        G2_FAILED=1
        return
    fi
    qa_check g2_up_far_port 0 "127.0.0.1:$FAR_PORT free"
    scp_far "$QA_REPO/" \
        "$FAR_HOST:$FAR_RUN/helpers/far-uplink-recv.py" || {
        qa_check g2_up_receiver 1 "upload far-uplink-recv.py"
        G2_FAILED=1
        return
    }
    if ! start_uplink_receiver; then
        qa_check g2_up_receiver 1 "receiver did not answer HEAD 200"
        G2_FAILED=1
        return
    fi
    qa_check g2_up_receiver 0 "HEAD 200 on 127.0.0.1:$FAR_PORT"
    local sz
    sz=$(gate_ssh near "$SSH_TMO" "stat -c %s '$NEAR_UP' 2>/dev/null || echo 0" 2>/dev/null | tr -d '[:space:]')
    if [[ ${sz:-0} != "$((UP_MIB * 1024 * 1024))" ]]; then
        gate_ssh near 300 "mkdir -p '$NEAR_RUN' && dd if=/dev/zero of='$NEAR_UP' bs=1M count=$UP_MIB status=none" \
            >"$EV/g2-up-dd.log" 2>&1 || true
    fi
    sz=$(gate_ssh near "$SSH_TMO" "stat -c %s '$NEAR_UP' 2>/dev/null || echo 0" 2>/dev/null | tr -d '[:space:]')
    qa_check g2_up_source "$([[ ${sz:-0} == "$((UP_MIB * 1024 * 1024))" ]]; echo $?)" \
        "near source size=${sz:-0}"
    wait_streams_clear g2_up
    snap_all g2-up-a
    cat >"$TMP/up.sh" <<EOF
#!/bin/sh
echo \$\$ > /tmp/fastgate.pid
exec curl -sS -m $UP_SECONDS -H 'Expect:' --data-binary @$NEAR_UP -o /dev/null -w 'http=%{http_code} up=%{size_upload} time=%{time_total} speed=%{speed_upload}\n' http://127.0.0.1:$NEAR_PORT/
EOF
    kw0=$(date +%s)
    if ! start_bg_curl "$TMP/up.sh"; then
        qa_check g2_up_start 1 "curl did not start"
        G2_FAILED=1
        return
    fi
    UP_PID=$BG_PID
    qa_check g2_up_start 0 "pid=$UP_PID"
    sleep "$((UP_SECONDS + 3))"
    stop_bg_curl
    gate_ssh near "$SSH_TMO" "cat /tmp/fastgate.out 2>/dev/null" >"$EV/g2-up-curl.txt" 2>/dev/null || true
    snap_all g2-up-b
    UP_HTTP=$(kv_file "$EV/g2-up-curl.txt" http)
    UP_BYTES=$(kv_file "$EV/g2-up-curl.txt" up)
    UP_TIME=$(kv_file "$EV/g2-up-curl.txt" time)
    UP_SPEED=$(kv_file "$EV/g2-up-curl.txt" speed)
    UP_NEAR_SUBMIT=$(delta "$(any_field "$EV/g2-up-a-near-stats.txt" submit_bytes)" \
        "$(any_field "$EV/g2-up-b-near-stats.txt" submit_bytes)")
    UP_FAR_RX=$(delta "$(any_field "$EV/g2-up-a-far-stats.txt" rx_delivered)" \
        "$(any_field "$EV/g2-up-b-far-stats.txt" rx_delivered)")
    if [[ -n $FAR_PHYS_DEV ]]; then
        UP_WIRE=$(delta "$(netdev_tx "$EV/g2-up-a-far-netdev.txt" "$FAR_PHYS_DEV")" \
            "$(netdev_tx "$EV/g2-up-b-far-netdev.txt" "$FAR_PHYS_DEV")")
    fi
    local nb ni nb2 ni2 fb fi fb2 fi2
    read -r nb ni < <(cpu_busy "$EV/g2-up-a-near-procstat.txt") || true
    read -r nb2 ni2 < <(cpu_busy "$EV/g2-up-b-near-procstat.txt") || true
    UP_NEAR_CPU=$(cpu_pct "${nb:-0}" "${ni:-0}" "${nb2:-0}" "${ni2:-0}")
    read -r fb fi < <(cpu_busy "$EV/g2-up-a-far-procstat.txt") || true
    read -r fb2 fi2 < <(cpu_busy "$EV/g2-up-b-far-procstat.txt") || true
    UP_FAR_CPU=$(cpu_pct "${fb:-0}" "${fi:-0}" "${fb2:-0}" "${fi2:-0}")
    local recv_line recv
    recv_line=
    for i in {1..10}; do
        recv_line=$(gate_ssh far "$SSH_TMO" "grep 'POST' '$FAR_UP/recv.log' 2>/dev/null | tail -n 1" 2>/dev/null || true)
        [[ -n $recv_line ]] && break
        sleep 1
    done
    recv=$(kv_line "$recv_line" recv)
    printf '%s\n' "$recv_line" >"$EV/g2-up-recv-cut.txt"
    g2_report g2-up
    qa_check g2_up_near_submit "$([[ ${UP_NEAR_SUBMIT:-0} -gt 0 ]]; echo $?)" \
        "near submit_bytes=${UP_NEAR_SUBMIT:-?}"
    qa_check g2_up_far_rx "$([[ ${UP_FAR_RX:-0} -gt 0 ]]; echo $?)" \
        "far rx_delivered=${UP_FAR_RX:-?}"
    qa_check g2_up_recv "$([[ ${recv:-0} -gt 0 ]]; echo $?)" \
        "far receiver recv=${recv:-?} (cut by -m $UP_SECONDS; http=${UP_HTTP:-000})"
    for end in near far; do
        check_delta_zero "g2_up_${end}_stream_reset_tcp" \
            "$EV/g2-up-a-$end-stats.txt" "$EV/g2-up-b-$end-stats.txt" c3 stream_reset_tcp_failed
        check_delta_zero "g2_up_${end}_stream_reset_user" \
            "$EV/g2-up-a-$end-stats.txt" "$EV/g2-up-b-$end-stats.txt" c3 stream_reset_user
        check_delta_zero "g2_up_${end}_fec_reset_tcp" \
            "$EV/g2-up-a-$end-stats.txt" "$EV/g2-up-b-$end-stats.txt" c11 fec_reset_tcp_failed
        # Same as g2_down: the ${UP_SECONDS}s cut is an application teardown.
        qa_skip "g2_up_${end}_fec_reset_user" \
            "application teardown delta=$(delta "$(stat_field "$EV/g2-up-a-$end-stats.txt" c11 fec_reset_user)" "$(stat_field "$EV/g2-up-b-$end-stats.txt" c11 fec_reset_user)")"
        check_delta_zero "g2_up_${end}_fec_reset_hole" \
            "$EV/g2-up-a-$end-stats.txt" "$EV/g2-up-b-$end-stats.txt" c11 fec_reset_hole
        check_delta_zero "g2_up_${end}_holes_unrecoverable" \
            "$EV/g2-up-a-$end-stats.txt" "$EV/g2-up-b-$end-stats.txt" c6 holes_unrecoverable
        check_delta_zero "g2_up_${end}_deliver_bad" \
            "$EV/g2-up-a-$end-stats.txt" "$EV/g2-up-b-$end-stats.txt" c10 fec_deliver_bad
        check_any_delta_zero "g2_up_${end}_rx_errs" \
            "$EV/g2-up-a-$end-stats.txt" "$EV/g2-up-b-$end-stats.txt" rx_errs
    done
    local streams_before=$FAILED
    check_streams_after g2_up
    UP_STREAMS_FINAL="$(streams_active near)/$(streams_active far)"
    if ((FAILED > streams_before)); then
        UP_STREAMS_OK=0
        qa_skip g2_probe "the ${UP_SECONDS}s stream did not clear within 15 s; probe not attempted"
        check_kernel_window g2_up "$kw0" "$(date +%s)"
        capture_diag g2-up || true
        G2_FAILED=1
        return
    fi
    local probe_rc=0 probe_line probe_recv
    gate_ssh near "$SSH_TMO" "dd if=/dev/zero of='$NEAR_RUN/probe.bin' bs=1M count=1 status=none" \
        >/dev/null 2>&1 || true
    cat >"$TMP/probe.sh" <<EOF
#!/bin/sh
exec curl -sS -m 30 -H 'Expect:' --data-binary @$NEAR_RUN/probe.bin -o /dev/null -w 'http=%{http_code} up=%{size_upload} time=%{time_total} speed=%{speed_upload}\n' http://127.0.0.1:$NEAR_PORT/
EOF
    scp_near "$TMP/probe.sh" "$NEAR_HOST:/tmp/fastgate-probe.sh" >/dev/null 2>&1 || true
    probe_line=$(ssh_near_long 120 "sh /tmp/fastgate-probe.sh" 2>&1) || probe_rc=$?
    printf '%s\n' "$probe_line" >"$EV/g2-probe-curl.txt"
    PROBE_HTTP=$(kv_line "$probe_line" http)
    PROBE_UP=$(kv_line "$probe_line" up)
    PROBE_RC=$probe_rc
    qa_check g2_probe_rc "$probe_rc" "curl rc=$probe_rc"
    qa_check g2_probe_http "$([[ ${PROBE_HTTP:-000} == 200 ]]; echo $?)" "http=${PROBE_HTTP:-000}"
    qa_check g2_probe_bytes "$([[ ${PROBE_UP:-0} == "$PROBE_WANT" ]]; echo $?)" \
        "size_upload=${PROBE_UP:-?} want=$PROBE_WANT"
    probe_recv=
    for i in {1..10}; do
        probe_recv=$(gate_ssh far "$SSH_TMO" "grep 'POST' '$FAR_UP/recv.log' 2>/dev/null | tail -n 1" 2>/dev/null || true)
        [[ $probe_recv == *recv=$PROBE_WANT*result=ok* ]] && break
        sleep 1
    done
    printf '%s\n' "$probe_recv" >"$EV/g2-probe-recv.txt"
    qa_check g2_probe_recv "$([[ $probe_recv == *recv=$PROBE_WANT*result=ok* ]]; echo $?)" \
        "${probe_recv:-no receiver line}"
    check_streams_after g2_probe
    check_kernel_window g2_up "$kw0" "$(date +%s)"
    if ((FAILED > before)); then
        G2_FAILED=1
        capture_diag g2-up || true
    fi
    mark g2-up-done
}

PROBE_WANT=$PROBE_BYTES

stop_far_source() {
    gate_ssh far 120 "FAR_DIR='$FAR_ACCEPT' PORT='$FAR_PORT' sh '$FAR_RUN/helpers/far-serve.sh' --stop" \
        >"$EV/g2-far-stop.txt" 2>&1 || true
    local i busy
    for i in {1..10}; do
        busy=$(gate_ssh far "$SSH_TMO" "ss -ltnH 'sport = :$FAR_PORT' 2>/dev/null | wc -l" 2>/dev/null | tr -d '[:space:]')
        [[ ${busy:-1} == 0 ]] && return 0
        sleep 1
    done
    return 1
}

start_uplink_receiver() {
    gate_ssh far "$SSH_TMO" "mkdir -p '$FAR_UP'; rm -f '$FAR_UP/recv.pid' '$FAR_UP/up.bin' '$FAR_UP/up.bin.part' '$FAR_UP/recv.log'; setsid python3 '$FAR_RUN/helpers/far-uplink-recv.py' --bind 127.0.0.1 --port $FAR_PORT --out '$FAR_UP/up.bin' --log '$FAR_UP/recv.log' --pidfile '$FAR_UP/recv.pid' --max-bytes 2147483648 </dev/null >'$FAR_UP/recv.err' 2>&1 & echo started" \
        >/dev/null || return 1
    local i code
    for i in {1..20}; do
        code=$(gate_ssh far "$SSH_TMO" "curl -sS -o /dev/null -w '%{http_code}' -I -m 3 http://127.0.0.1:$FAR_PORT/ 2>/dev/null" 2>/dev/null || true)
        [[ $code == 200 ]] && return 0
        sleep 1
    done
    return 1
}

g2_report() { # prefix
    local p=$1 end cls fld spec d
    local specs=(
        "c3 stream_reset_tcp_failed"
        "c3 stream_reset_user"
        "c3 connect_fail"
        "c3 join_bad"
        "c3 alloc_fail"
        "c3 retx_bytes"
        "c6 holes_declared"
        "c6 holes_repaired"
        "c6 holes_unrecoverable"
        "c9 fec_proto_fail"
        "c9 fec_hdr_crc_fail"
        "c9 fec_payload_crc_fail"
        "c10 fec_deliver_bad"
        "c10 fec_decode_blocks"
        "c11 fec_reset_tcp_failed"
        "c11 fec_reset_user"
        "c11 fec_reset_hole"
        "c11 fec_nack_drop"
        "c11 fec_replay_miss_never_stored"
    )
    {
        printf 'app_near_rx_delivered=%s\n' "$(delta "$(any_field "$EV/$p-a-near-stats.txt" rx_delivered)" "$(any_field "$EV/$p-b-near-stats.txt" rx_delivered)")"
        printf 'app_far_submit_bytes=%s\n' "$(delta "$(any_field "$EV/$p-a-far-stats.txt" submit_bytes)" "$(any_field "$EV/$p-b-far-stats.txt" submit_bytes)")"
        printf 'near_cpu_busy_pct=%s\n' "$([[ $p == g2-down ]] && echo "$DOWN_NEAR_CPU" || echo "$UP_NEAR_CPU")"
        printf 'far_cpu_busy_pct=%s\n' "$([[ $p == g2-down ]] && echo "$DOWN_FAR_CPU" || echo "$UP_FAR_CPU")"
        for end in near far; do
            printf '[%s]\n' "$end"
            for spec in "${specs[@]}"; do
                cls=${spec%% *}
                fld=${spec#* }
                d=$(delta "$(stat_field "$EV/$p-a-$end-stats.txt" "$cls" "$fld")" \
                    "$(stat_field "$EV/$p-b-$end-stats.txt" "$cls" "$fld")")
                printf '%s=%s\n' "$fld" "$d"
            done
            printf 'rx_errs=%s\n' "$(delta "$(any_field "$EV/$p-a-$end-stats.txt" rx_errs)" "$(any_field "$EV/$p-b-$end-stats.txt" rx_errs)")"
            printf 'up_post=%s\n' "$(stat_field "$EV/$p-b-$end-stats.txt" c3 up)"
            printf 'replacing_post=%s\n' "$(stat_field "$EV/$p-b-$end-stats.txt" c3 replacing)"
            printf 'empty_post=%s\n' "$(stat_field "$EV/$p-b-$end-stats.txt" c3 empty)"
            printf 'streams_active_post=%s\n' "$(stat_field "$EV/$p-b-$end-stats.txt" streams streams_active)"
        done
    } >"$EV/$p-counters.txt"
}

g2_run() {
    stage_start
    if ((G0_FAILED || G1_FAILED)); then
        qa_skip g2 "G1 failed; no traffic"
        return
    fi
    g2_down_run
    if ((G2_FAILED)); then
        qa_skip g2_up "downlink failed; uplink not attempted"
        return
    fi
    g2_up_run
    if stage_failed; then G2_FAILED=1; fi
}

# ---------------------------------------------------------------------------
# G3: carrier and watchdog

capture_syn_evidence() { # shortfall
    local short=$1 near_n far_n near_ports
    {
        printf 'shortfall=%s\n' "$short"
        printf '=== near ss syn-sent to %s ===\n' "$FAR_IP"
        gate_ssh near "$SSH_TMO" "ss -Htn state syn-sent dst '$FAR_IP' 2>/dev/null || true"
        printf '=== far ss syn-recv ===\n'
        gate_ssh far "$SSH_TMO" "ss -Htn state syn-recv 2>/dev/null || true"
    } >"$EV/g3-syn.txt" 2>&1
    near_n=$(sed -n '/=== near ss syn-sent/,/=== far ss syn-recv/p' "$EV/g3-syn.txt" |
        awk 'NF >= 4 { n++ } END { print n + 0 }')
    far_n=$(sed -n '/=== far ss syn-recv/,$p' "$EV/g3-syn.txt" |
        awk 'NF >= 4 { n++ } END { print n + 0 }')
    near_ports=$(awk '
        /=== near ss syn-sent/ { in_near = 1; next }
        /=== far ss syn-recv/ { in_near = 0 }
        in_near && NF >= 4 {
            lp = $3
            sub(/.*:/, "", lp)
            if (lp ~ /^[0-9]+$/ && lp >= 30000 && lp <= 30511) n++
        }
        END { print n + 0 }' "$EV/g3-syn.txt")
    printf 'near_syn_sent=%s\nnear_ports_in_block=%s\nfar_syn_recv=%s\n' \
        "$near_n" "$near_ports" "$far_n" >>"$EV/g3-syn.txt"
    if ((near_n + far_n > 0)); then
        qa_check g3_syn_evidence 0 \
            "shortfall=$short near_syn_sent=$near_n (ports $near_ports) far_syn_recv=$far_n"
    else
        qa_skip g3_syn_evidence \
            "shortfall=$short but no SYN-SENT/SYN-RECV lines are visible; recorded in g3-syn.txt"
    fi
}

g3_run() {
    stage_start
    local end f n up rep emp lost cf cr inv_ok up_ok
    for end in near far; do
        f=$EV/g3-$end-stats.txt
        snap_stats "$end" "$f"
        if [[ ! -s $f ]]; then
            qa_check "${end}_g3_stats" 1 "no stats snapshot"
            continue
        fi
        n=$(stat_field "$f" c3 n)
        up=$(stat_field "$f" c3 up)
        rep=$(stat_field "$f" c3 replacing)
        emp=$(stat_field "$f" c3 empty)
        lost=$(stat_field "$f" c3 lost)
        cf=$(stat_field "$f" c3 connect_fail)
        cr=$(stat_field "$f" c3 connect_retry)
        inv_ok=1
        (( ${n:-0} == CARRIERS && ${up:-0} + ${rep:-0} + ${emp:-0} == ${n:-0} )) && inv_ok=0
        qa_check "${end}_carrier_invariant" "$inv_ok" \
            "n=${n:-?} up=${up:-?} replacing=${rep:-?} empty=${emp:-?}"
        if ((MIN_UP == CARRIERS)); then
            up_ok=1
            (( ${up:-0} == CARRIERS )) && up_ok=0
            qa_check "${end}_carrier_up" "$up_ok" "up=${up:-?} strict=$CARRIERS"
        else
            up_ok=1
            (( ${up:-0} >= MIN_UP )) && up_ok=0
            qa_check "${end}_carrier_up" "$up_ok" "up=${up:-?} min=$MIN_UP"
        fi
        {
            printf 'end=%s\nn=%s\nup=%s\nreplacing=%s\nempty=%s\nlost=%s\nconnect_fail=%s\nconnect_retry=%s\nport_alloc_fail=%s\nalloc_fail=%s\n' \
                "$end" "${n:-?}" "${up:-?}" "${rep:-?}" "${emp:-?}" "${lost:-?}" \
                "${cf:-?}" "${cr:-?}" \
                "$(stat_field "$f" c3 port_alloc_fail)" "$(stat_field "$f" c3 alloc_fail)"
            printf 'rx_proto=%s\nrx_errs=%s\nstreams_active=%s\n' \
                "$(any_field "$f" rx_proto)" "$(any_field "$f" rx_errs)" \
                "$(stat_field "$f" streams streams_active)"
        } >"$EV/g3-carrier-$end.txt"
        if [[ $end == near ]]; then G3_NEAR_UP=$up; G3_NEAR_REP=$rep; G3_NEAR_EMPTY=$emp; G3_NEAR_N=$n; fi
        if [[ $end == far ]]; then G3_FAR_UP=$up; G3_FAR_REP=$rep; G3_FAR_EMPTY=$emp; G3_FAR_N=$n; fi
    done
    local short=0
    num "${G3_NEAR_UP:-}" && short=$((CARRIERS - G3_NEAR_UP))
    num "${G3_FAR_UP:-}" && ((CARRIERS - G3_FAR_UP > short)) && short=$((CARRIERS - G3_FAR_UP))
    if ((short > 0)); then
        capture_syn_evidence "$short"
    else
        qa_skip g3_syn_evidence "no shortfall"
    fi
    local wd active holder mainpid runtime
    wd=$(near_watchdog)
    printf '%s\n' "$wd" >"$EV/g3-watchdog.txt"
    gate_ssh near "$SSH_TMO" "tail -n 20 /var/log/tbtun-watchdog.log 2>/dev/null" \
        >>"$EV/g3-watchdog.txt" 2>/dev/null || true
    active=$(sed -n 's/^ACTIVE=//p' <<<"$wd" | tail -n 1)
    holder=$(sed -n 's/^HOLDER=//p' <<<"$wd" | tail -n 1)
    mainpid=$(sed -n 's/^MAINPID=//p' <<<"$wd" | tail -n 1)
    runtime=$(sed -n 's/^RUNTIME=//p' <<<"$wd" | tail -n 1)
    qa_check near_netwatch_active "$([[ $active == active ]]; echo $?)" \
        "state=${active:-?} MainPID=${mainpid:-?} RuntimeWatchdogUSec=${runtime:-?}"
    qa_check near_watchdog_holder "$([[ -n $holder && $holder != none ]]; echo $?)" "${holder:-none}"
    for end in near far; do
        f=$EV/g3-$end-stats.txt
        [[ -s $f ]] || continue
        local sa rp re
        sa=$(stat_field "$f" streams streams_active)
        rp=$(any_field "$f" rx_proto)
        re=$(any_field "$f" rx_errs)
        qa_check "${end}_final_streams_active" "$([[ ${sa:-?} == 0 ]]; echo $?)" "streams_active=${sa:-?}"
        qa_check "${end}_final_rx_proto" "$([[ ${rp:-?} == 0 ]]; echo $?)" "rx_proto=${rp:-?}"
        qa_check "${end}_final_rx_errs" "$([[ ${re:-?} == 0 ]]; echo $?)" "rx_errs=${re:-?}"
    done
    check_kernel_window g3 "$START_EPOCH" "$(date +%s)"
    if stage_failed; then
        G3_FAILED=1
        capture_diag g3 || true
    fi
    mark g3-done
}

# ---------------------------------------------------------------------------
# cleanup

cleanup_all() {
    stage_start
    say "== cleanup"
    local out=$EV/cleanup.txt owned near_out far_out
    : >"$out"
    owned=$(gate_ssh near "$SSH_TMO" "test -f '$NEAR_RUN/ip-rule-owned' && echo yes || echo no" 2>/dev/null || true)
    near_out=$(gate_ssh near "$SSH_TMO" \
        "NEAR_RUN='$NEAR_RUN' NEAR_UP='$NEAR_UP' PROC='$PROC_ROOT' FAR_IP='$FAR_IP' PREF='$BYPASS_PREF' bash -s" \
        2>&1 <<'EOS'
set -u
pkill -f '/tmp/fastgate-down.sh' 2>/dev/null || true
pkill -f '/tmp/fastgate-up.sh' 2>/dev/null || true
pkill -f '/tmp/fastgate-probe.sh' 2>/dev/null || true
rm -f /tmp/fastgate-down.sh /tmp/fastgate-up.sh /tmp/fastgate-probe.sh \
    /tmp/fastgate.pid /tmp/fastgate.out
if [ -f "$NEAR_RUN/ip-rule-owned" ]; then
    ip rule del pref "$PREF" to "$FAR_IP/32" lookup main 2>/dev/null ||
        ip rule del pref "$PREF" to "$FAR_IP/32" 2>/dev/null || true
    rm -f "$NEAR_RUN/ip-rule-owned"
    echo "IP_RULE_REMOVED=1"
else
    echo "IP_RULE_REMOVED=0"
fi
if [ -e "$PROC/config" ]; then
    timeout 10 sh -c "printf 'del 1\n' > '$PROC/config'" 2>/dev/null || true
fi
if lsmod 2>/dev/null | awk '$1 == "tbtun" { f = 1 } END { exit !f }'; then
    timeout 60 rmmod tbtun 2>/dev/null || true
fi
rm -f "$NEAR_UP" "$NEAR_RUN/probe.bin" 2>/dev/null || true
if lsmod 2>/dev/null | awk '$1 == "tbtun" { f = 1 } END { exit !f }'; then
    echo MODULE_LOADED
else
    echo MODULE_UNLOADED
fi
EOS
)
    far_out=$(gate_ssh far "$SSH_TMO" \
        "FAR_RUN='$FAR_RUN' FAR_ACCEPT='$FAR_ACCEPT' FAR_UP='$FAR_UP' FAR_PORT='$FAR_PORT' PROC='$PROC_ROOT' bash -s" \
        2>&1 <<'EOS'
set -u
if [ -f "$FAR_RUN/helpers/far-serve.sh" ]; then
    FAR_DIR="$FAR_ACCEPT" PORT="$FAR_PORT" sh "$FAR_RUN/helpers/far-serve.sh" --stop 2>/dev/null || true
fi
if [ -f "$FAR_UP/recv.pid" ]; then
    p=$(cat "$FAR_UP/recv.pid" 2>/dev/null || true)
    if [ -n "${p:-}" ]; then
        kill "$p" 2>/dev/null || true
        i=0
        while [ "$i" -lt 5 ] && kill -0 "$p" 2>/dev/null; do
            sleep 1
            i=$((i + 1))
        done
        kill -9 "$p" 2>/dev/null || true
    fi
fi
pkill -f "$FAR_RUN/helpers/far-uplink-recv.py" 2>/dev/null || true
if [ -e "$PROC/config" ]; then
    timeout 10 sh -c "printf 'del 1\n' > '$PROC/config'" 2>/dev/null || true
fi
if lsmod 2>/dev/null | awk '$1 == "tbtun" { f = 1 } END { exit !f }'; then
    timeout 60 rmmod tbtun 2>/dev/null || true
fi
rm -rf "$FAR_ACCEPT" "$FAR_UP" 2>/dev/null || true
if lsmod 2>/dev/null | awk '$1 == "tbtun" { f = 1 } END { exit !f }'; then
    echo MODULE_LOADED
else
    echo MODULE_UNLOADED
fi
EOS
)
    {
        printf '=== near ===\n%s\n' "$near_out"
        printf '=== far ===\n%s\n' "$far_out"
    } >>"$out" 2>&1
    qa_check cleanup_near_module "$(grep -qx 'MODULE_UNLOADED' <<<"$near_out"; echo $?)" "tbtun unloaded"
    qa_check cleanup_far_module "$(grep -qx 'MODULE_UNLOADED' <<<"$far_out"; echo $?)" "tbtun unloaded"
    local busy
    busy=$(gate_ssh far "$SSH_TMO" "ss -ltnH 'sport = :$FAR_PORT' 2>/dev/null | wc -l" 2>/dev/null | tr -d '[:space:]')
    qa_check cleanup_far_port "$([[ ${busy:-1} == 0 ]]; echo $?)" "127.0.0.1:$FAR_PORT listeners=${busy:-?}"
    if [[ $owned == yes ]]; then
        local rule
        rule=$(gate_ssh near "$SSH_TMO" "ip rule show 2>/dev/null | awk -v p='$BYPASS_PREF:' -v ip='$FAR_IP' '\$1==p { for (i=2;i<NF;i++) if (\$i==\"to\" && (\$(i+1)==ip || \$(i+1)==ip \"/32\")) f=1 } END { print f?1:0 }'" 2>/dev/null | tr -d '[:space:]')
        qa_check cleanup_near_rule "$([[ ${rule:-1} == 0 ]]; echo $?)" \
            "pref $BYPASS_PREF to $FAR_IP/32 present=${rule:-?}"
    else
        qa_skip cleanup_near_rule "the rule was not owned by this tag"
    fi
    local nf ff
    nf=$(gate_ssh near "$SSH_TMO" "test -e '$NEAR_UP' -o -e '$NEAR_RUN/probe.bin'; echo \$?" 2>/dev/null | tr -d '[:space:]')
    ff=$(gate_ssh far "$SSH_TMO" "test -e '$FAR_ACCEPT' -o -e '$FAR_UP'; echo \$?" 2>/dev/null | tr -d '[:space:]')
    qa_check cleanup_near_files "$([[ ${nf:-0} == 1 ]]; echo $?)" "near source files removed"
    qa_check cleanup_far_files "$([[ ${ff:-0} == 1 ]]; echo $?)" "far source files removed"
    if stage_failed; then CLEANUP_FAILED=1; fi
}

# ---------------------------------------------------------------------------
# SUMMARY

write_summary() {
    local verdict=$1 pass fail skip
    local dresets dholes dbad uresets uholes ubad
    pass=$(awk '$1 == "CHECK" && $3 == "PASS" { n++ } END { print n + 0 }' "$EV/run.log")
    fail=$(awk '$1 == "CHECK" && $3 == "FAIL" { n++ } END { print n + 0 }' "$EV/run.log")
    skip=$(awk '$1 == "CHECK" && $3 == "SKIP" { n++ } END { print n + 0 }' "$EV/run.log")
    dresets=$(sum_counter "$EV/g2-down-counters.txt" fec_reset_tcp_failed fec_reset_user fec_reset_hole)
    dholes=$(sum_counter "$EV/g2-down-counters.txt" holes_unrecoverable)
    dbad=$(sum_counter "$EV/g2-down-counters.txt" fec_deliver_bad)
    uresets=$(sum_counter "$EV/g2-up-counters.txt" fec_reset_tcp_failed fec_reset_user fec_reset_hole)
    uholes=$(sum_counter "$EV/g2-up-counters.txt" holes_unrecoverable)
    ubad=$(sum_counter "$EV/g2-up-counters.txt" fec_deliver_bad)
    {
        printf '# tbtun 0.4.x fast gate — %s\n\n' "$verdict"
        printf -- '- tag: `%s` (%s, commit `%s`)\n' "$GATE_TAG" "${TAG_TYPE:-?}" "${TAG_COMMIT:-?}"
        printf -- '- tag pin: %s\n' "${GATE_TAG_COMMIT:-unpinned}"
        printf -- '- expected module version: `%s`\n' "${EXPECT_VERSION:-<not checked>}"
        printf -- '- archive: `git archive %s`, %s bytes, sha256 `%s`\n' \
            "$GATE_TAG" "${ARCHIVE_SIZE:-?}" "${ARCHIVE_SHA:-?}"
        printf -- '- start: %s\n- end: %s\n- evidence: `%s`\n\n' \
            "$START_UTC" "$END_UTC" "$EV"
        printf 'FAST_GATE_RESULT %s\n\n' "$verdict"
        printf 'Checks: %s PASS, %s FAIL, %s SKIP\n\n' "$pass" "$fail" "$skip"
        printf '## G0 host build\n\n'
        printf '| end | host | arch | kernel | version | srcversion | vermagic | ko sha256 | warnings |\n'
        printf '|---|---|---|---|---|---|---|---|---|\n'
        printf '| near | %s | %s | %s | %s | %s | %s | %s | %s |\n' \
            "$NEAR_HOST" "${NEAR_ARCH:-?}" "${NEAR_KREL:-?}" "${NEAR_VER:-?}" \
            "${NEAR_SV:-?}" "${NEAR_VMAGIC:-?}" "${NEAR_KO_SHA:-?}" "${NEAR_BUILD_WARN:-?}"
        printf '| far | %s | %s | %s | %s | %s | %s | %s | %s |\n\n' \
            "$FAR_HOST" "${FAR_ARCH:-?}" "${FAR_KREL:-?}" "${FAR_VER:-?}" \
            "${FAR_SV:-?}" "${FAR_VMAGIC:-?}" "${FAR_KO_SHA:-?}" "${FAR_BUILD_WARN:-?}"
        printf '## G1 deploy and join\n\n'
        printf '```\nfar:  %s\nnear: %s\n```\n\n' "${FAR_CFG:-?}" "${NEAR_CFG:-?}"
        printf '| end | n | up | replacing | empty | lost | connect_fail | connect_retry |\n'
        printf '|---|---|---|---|---|---|---|---|\n'
        printf '| near | %s | %s | %s | %s | %s | %s | %s |\n' \
            "${G3_NEAR_N:-${JOIN_N:-?}}" "${G3_NEAR_UP:-${JOIN_NEAR_UP:-?}}" \
            "${G3_NEAR_REP:-${JOIN_NEAR_REP:-?}}" "${G3_NEAR_EMPTY:-${JOIN_NEAR_EMPTY:-?}}" \
            "$(sed -n 's/^lost=//p' "$EV/g3-carrier-near.txt" 2>/dev/null | head -n 1)" \
            "$(sed -n 's/^connect_fail=//p' "$EV/g3-carrier-near.txt" 2>/dev/null | head -n 1)" \
            "$(sed -n 's/^connect_retry=//p' "$EV/g3-carrier-near.txt" 2>/dev/null | head -n 1)"
        printf '| far | %s | %s | %s | %s | %s | %s | %s |\n\n' \
            "${G3_FAR_N:-${JOIN_N:-?}}" "${G3_FAR_UP:-${JOIN_FAR_UP:-?}}" \
            "${G3_FAR_REP:-${JOIN_FAR_REP:-?}}" "${G3_FAR_EMPTY:-${JOIN_FAR_EMPTY:-?}}" \
            "$(sed -n 's/^lost=//p' "$EV/g3-carrier-far.txt" 2>/dev/null | head -n 1)" \
            "$(sed -n 's/^connect_fail=//p' "$EV/g3-carrier-far.txt" 2>/dev/null | head -n 1)" \
            "$(sed -n 's/^connect_retry=//p' "$EV/g3-carrier-far.txt" 2>/dev/null | head -n 1)"
        printf -- '- near ip rule: added=%s owned=%s; `to %s/32 lookup main pref %s`\n' \
            "$NEAR_IP_RULE_ADDED" "$NEAR_IP_RULE_OWNED" "$FAR_IP" "$BYPASS_PREF"
        printf -- '- far source: size=%s sha256=%s\n' "${FAR_SIZE:-?}" "${FAR_SHA:-?}"
        printf -- '- join waited %s s\n\n' "${JOIN_SECONDS:-?}"
        printf '## G2 transfers\n\n'
        printf '| direction | http | app bytes | wire bytes | time s | speed B/s | reset/holes/deliver_bad | streams_active after 15 s |\n'
        printf '|---|---|---|---|---|---|---|---|\n'
        printf '| down | %s | far submit=%s, near rx_delivered=%s | %s | %s | %s | %s | %s |\n' \
            "${DOWN_HTTP:-000}" "${DOWN_FAR_SUBMIT:-?}" "${DOWN_NEAR_RX:-?}" \
            "${DOWN_WIRE:-?}" "${DOWN_TIME:-?}" "${DOWN_SPEED:-?}" \
            "resets=$dresets holes=$dholes deliver_bad=$dbad" \
            "${DOWN_STREAMS_FINAL:-?}"
        printf '| up (60 s) | %s (cut) | near submit=%s, far rx_delivered=%s | %s | %s | %s | %s | %s |\n' \
            "${UP_HTTP:-000}" "${UP_NEAR_SUBMIT:-?}" "${UP_FAR_RX:-?}" \
            "${UP_WIRE:-?}" "${UP_TIME:-?}" "${UP_SPEED:-?}" \
            "resets=$uresets holes=$uholes deliver_bad=$ubad" \
            "${UP_STREAMS_FINAL:-?}"
        printf '| up probe | %s | size_upload=%s rc=%s | | | | | %s |\n\n' \
            "${PROBE_HTTP:-000}" "${PROBE_UP:-?}" "${PROBE_RC:-?}" \
            "$(stat_field "$EV/g3-near-stats.txt" streams streams_active 2>/dev/null)/$(stat_field "$EV/g3-far-stats.txt" streams streams_active 2>/dev/null)"
        printf 'Counter deltas:\n\n```\n'
        cat "$EV/g2-down-counters.txt" 2>/dev/null || true
        printf '\n'
        cat "$EV/g2-up-counters.txt" 2>/dev/null || true
        printf '```\n\n'
        printf '## G3 carrier and watchdog\n\n'
        printf -- '- criterion: %s\n' "$([[ $MIN_UP == 256 ]] && echo "strict up=$CARRIERS" || echo "up >= $MIN_UP (explicit --min-up)")"
        printf -- '- near shortfall evidence: `%s`\n' "$([[ -f $EV/g3-syn.txt ]] && echo g3-syn.txt || echo none)"
        printf -- '- netwatch: `%s`; holder: `%s`\n\n' \
            "$(sed -n 's/^ACTIVE=//p' "$EV/g3-watchdog.txt" 2>/dev/null | head -n 1)" \
            "$(sed -n 's/^HOLDER=//p' "$EV/g3-watchdog.txt" 2>/dev/null | head -n 1)"
        printf '## Checks\n\n```\n'
        awk '$1 == "CHECK" { print }' "$EV/run.log" 2>/dev/null || true
        printf '```\n\n'
        printf '## Failures\n\n```\n'
        awk '$1 == "CHECK" && $3 == "FAIL" { print }' "$EV/run.log" 2>/dev/null || true
        printf '```\n\n'
        printf '## Cleanup\n\n'
        if ((KEEP)); then
            printf 'skipped (--keep)\n'
        elif [[ -f $EV/cleanup.txt ]]; then
            printf 'see `cleanup.txt`\n'
        elif ((CLEANUP_FAILED)); then
            printf 'FAIL; see `cleanup.txt`\n'
        else
            printf 'not run (FAIL keeps the deployment); use `%s --cleanup --tag %s`\n' \
                "$PROG" "$GATE_TAG"
        fi
        printf '\n## Next\n\n'
        if [[ $verdict == PASS ]]; then
            printf 'The gate passed. Record this SUMMARY with the release evidence.\n'
        else
            printf 'Fix the first failure. Do not rerun blindly: read the stage log,\n'
            printf 'then rerun once. `%s --cleanup --tag %s` removes the deployment.\n' \
                "$PROG" "$GATE_TAG"
        fi
    } >"$EV/SUMMARY.md"
}

# ---------------------------------------------------------------------------
# modes

run_gate() {
    local evname="fast-gate-04x-$TAG_DIR"
    qa_evidence_init "$evname" >/dev/null || die 3 "cannot create evidence directory"
    : >"$EV/run.log"
    : >"$EV/timeline.txt"
    START_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    START_EPOCH=$(date +%s)
    say "fast gate $GATE_TAG; evidence: $EV"
    mark run-start "tag=$GATE_TAG"
    g0_run
    g1_run
    g2_run
    g3_run
    local verdict=PASS
    if ((FAILED > 0)); then verdict=FAIL; fi
    if [[ $verdict == PASS && $KEEP == 0 ]]; then
        cleanup_all
        if ((FAILED > 0)); then
            verdict=FAIL
        fi
    elif [[ $verdict == FAIL ]]; then
        qa_skip cleanup "deployment kept after FAIL; run $PROG --cleanup --tag $GATE_TAG"
    else
        qa_skip cleanup "kept by --keep"
    fi
    END_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    if [[ $verdict == PASS ]]; then
        note "verdict PASS ($(awk '$1=="CHECK" && $3=="PASS"{n++} END{print n+0}' "$EV/run.log") checks)"
    else
        note "verdict FAIL; evidence kept in $EV"
    fi
    write_summary "$verdict"
    say "SUMMARY: $EV/SUMMARY.md"
    say "FAST_GATE_RESULT $verdict"
    if [[ $verdict == PASS ]]; then
        exit 0
    fi
    if ((CLEANUP_FAILED)); then
        exit 7
    fi
    if ((G0_FAILED)); then
        exit 3
    fi
    if ((G1_FAILED)); then
        exit 4
    fi
    if ((G2_FAILED)); then
        exit 5
    fi
    exit 6
}

run_cleanup_mode() {
    local evname="fast-gate-04x-$TAG_DIR-cleanup"
    qa_evidence_init "$evname" >/dev/null || die 7 "cannot create evidence directory"
    : >"$EV/run.log"
    : >"$EV/timeline.txt"
    START_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    START_EPOCH=$(date +%s)
    say "fast gate cleanup $GATE_TAG; evidence: $EV"
    cleanup_all
    END_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    local verdict=PASS
    ((FAILED > 0)) && verdict=FAIL
    write_summary "$verdict"
    say "SUMMARY: $EV/SUMMARY.md"
    say "FAST_GATE_RESULT $verdict"
    [[ $verdict == PASS ]] && exit 0
    exit 7
}

parse_args "$@"
derive
dry_configs
if ((DRY)); then
    if [[ $MODE == cleanup ]]; then
        say "dry-run cleanup: stop this tag's far sources and receiver, stop near curls,"
        say "  remove the owned pref-$BYPASS_PREF rule to $FAR_IP/32, del+rmmod both ends,"
        say "  remove $FAR_ACCEPT, $FAR_UP, and $NEAR_UP; no ssh"
        exit 0
    fi
    dry_run
fi
TMP=$(mktemp -d "${TMPDIR:-/tmp}/tbtun-fastgate.XXXXXX") || die 2 "cannot create a temporary directory"
cleanup_tmp() { [[ -n ${TMP:-} ]] && rm -rf "$TMP"; }
trap cleanup_tmp EXIT
trap 'printf "%s: interrupted; evidence is kept\n" "$PROG" >&2; exit 130' INT TERM
if [[ $MODE == cleanup ]]; then
    run_cleanup_mode
fi
run_gate
