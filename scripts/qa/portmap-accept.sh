#!/usr/bin/env bash
# portmap-accept.sh - real-hardware acceptance for per-tunnel portblock.
#
# WHAT THIS PROVES
#   1. Two near tunnels with different portblock= windows draw every carrier
#      source port from their own window, with no duplicate source port and no
#      duplicate (sport, peer) 5-tuple across the tunnels.
#   2. A near tunnel whose window overlaps another live tunnel's window gets
#      no port: it reports port_exhaust and stays empty instead of stealing a
#      live port. The shared absolute bitmap is what makes this a conflict test,
#      not a bind-race test.
#   3. portblock=0-0 falls back to the global /proc/net/tbtun/ports window.
#   4. Writing /ports changes the window for new allocations while live
#      carriers keep their ports; writing the old window back still skips those
#      live bits, so the next tunnel gets the next free ports.
#   5. A far tunnel records portblock= but never allocates a carrier source
#      port: far port_exhaust/port_alloc_fail stay 0, and the far peers view
#      shows the near's source ports in the near windows, not the far block.
#
# WHAT IT DOES NOT DO
#   - It does not build, load, unload, or reinstall tbtun. Both hosts must
#     already run the 0.4.0 module and be reachable as root over SSH.
#   - It does not send application traffic. Carriers come up and stay up; the
#     far target is a literal 127.0.0.1:1 that is never dialled.
#   - It does not change net.ipv4.ip_local_reserved_ports. It records the
#     sysctl and warns when a test window is not reserved (--strict-reserved
#     turns that warning into a failed check).
#
# USAGE
#   scripts/qa/portmap-accept.sh [options]
#
#   --near HOST        SSH target for the near side (default $NEAR_HOST)
#   --far HOST         SSH target for the far side (default $FAR_HOST)
#   --carriers N       carriers per tunnel, 1..16 (default 4)
#   --keep             leave the test tunnels configured; default restores the
#                      saved config and port block on exit
#   --strict-reserved  fail when a test window is not covered by
#                      net.ipv4.ip_local_reserved_ports
#   --out DIR          evidence directory (default
#                      $EVIDENCE_ROOT/portmap-<UTC>)
#   -h, --help         this text
#
# ENVIRONMENT (all optional)
#   PB1=30000-30511            first near window
#   PB2=30600-30799            second near window
#   GLOBAL1=40000-40031        first global fallback window
#   GLOBAL2=40100-40131        second global fallback window
#   FAR_PB=31000-31015         far-only recorded window
#   NEAR_LISTEN=0.0.0.0        near public listen address
#   FAR_LISTEN=0.0.0.0         far carrier listen address
#   FAR_IP=45.11.1.62          address near carriers dial
#   NEAR_PUB_BASE=19100        first near public listen port
#   FAR_CAR_BASE=29100         first far carrier listen port
#   TARGET=127.0.0.1:1         far target (never dialled)
#   NEAR_DEV=                  optional near dev= interface
#   WAIT_UP_SECS=30            carrier bring-up timeout
#   WAIT_EXHAUST_SECS=20       port_exhaust observation timeout
#   STRICT_RESERVED=0          1 = fail unreserved windows
#   KEEP=0                     1 = leave the test configuration in place
#   EV=/path                   evidence directory; skips qa_evidence_init
#   NEAR_HOST, FAR_HOST, EVIDENCE_ROOT, SSH_TMO  from scripts/qa/lib.sh
#
# EVIDENCE
#   Every phase writes the near/far config, ports view, peers view, stats and
#   ss output under $EV. summary.txt ends with PORTMAP_RESULT PASS|FAIL.
#
# EXIT CODES
#   0 PASS   1 FAIL   2 usage   3 preflight   77 cannot run here (skip)
#
# The script is single shot. It restores the saved configuration on any exit
# unless --keep is given. Run it only on the test pair: it replaces tunnels
# 1..6 and the global port block for the duration of the run.
# A /ports view that was "default" is restored as an explicit write of the
# same block; the kernel has no way to clear the configured flag.
set -uo pipefail

PROG=${0##*/}
SELF_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib.sh
. "$SELF_DIR/lib.sh"

PB1=${PB1:-30000-30511}
PB2=${PB2:-30600-30799}
GLOBAL1=${GLOBAL1:-40000-40031}
GLOBAL2=${GLOBAL2:-40100-40131}
FAR_PB=${FAR_PB:-31000-31015}
CARRIERS=${CARRIERS:-4}
NEAR_LISTEN=${NEAR_LISTEN:-0.0.0.0}
FAR_LISTEN=${FAR_LISTEN:-0.0.0.0}
FAR_IP=${FAR_IP:-45.11.1.62}
NEAR_PUB_BASE=${NEAR_PUB_BASE:-19100}
FAR_CAR_BASE=${FAR_CAR_BASE:-29100}
TARGET=${TARGET:-127.0.0.1:1}
NEAR_DEV=${NEAR_DEV:-}
WAIT_UP_SECS=${WAIT_UP_SECS:-30}
WAIT_EXHAUST_SECS=${WAIT_EXHAUST_SECS:-20}
STRICT_RESERVED=${STRICT_RESERVED:-0}
KEEP=${KEEP:-0}
OUT_OVERRIDE=${EV:-}
EV=${EV:-}

FAILED=0
CLEANED=0
LOADED_NEAR=0
LOADED_FAR=0
SAVED_NEAR_CFG=
SAVED_FAR_CFG=
SAVED_NEAR_PORTS=
SAVED_FAR_PORTS=
EXH_LO=
EXH_HI=
T4_OLD=

usage() {
    sed -n '2,/^set -uo pipefail$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while (($#)); do
    case $1 in
        --near) NEAR_HOST=${2:?}; shift 2 ;;
        --far) FAR_HOST=${2:?}; shift 2 ;;
        --carriers) CARRIERS=${2:?}; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --strict-reserved) STRICT_RESERVED=1; shift ;;
        --out) OUT_OVERRIDE=${2:?}; shift 2 ;;
        -h | --help) usage 0 ;;
        *) echo "$PROG: unknown argument: $1" >&2; usage 2 ;;
    esac
done

say() { qa_say "$*"; }

# ---------------------------------------------------------------------------
# Small helpers

range_parse() { # "lo-hi" -> R_LO R_HI
    case $1 in
        *-*) ;;
        *) return 1 ;;
    esac
    R_LO=${1%%-*}
    R_HI=${1#*-}
    qa_num "$R_LO" && qa_num "$R_HI" || return 1
    R_LO=$((10#$R_LO))
    R_HI=$((10#$R_HI))
    ((R_LO >= 1 && R_LO <= R_HI && R_HI <= 65535))
}

range_size() { # "lo-hi" -> count
    range_parse "$1" || return 1
    echo $((R_HI - R_LO + 1))
}

port_in() { # port lo hi
    local p=$((10#$1)) lo=$((10#$2)) hi=$((10#$3))
    ((p >= lo && p <= hi))
}

remote_write() { # host path: stdin
    local host=$1 path=$2
    qa_ssh "$host" "cat > '$path'"
}

cfg_write() { # host line
    printf '%s\n' "$2" | remote_write "$1" "$PROC_ROOT/config"
}

ports_write() { # host "lo-hi"
    printf 'ports %s\n' "$2" | remote_write "$1" "$PROC_ROOT/ports"
}

del_tunnel() { # host id
    printf 'del %s\n' "$2" | remote_write "$1" "$PROC_ROOT/config" 2>/dev/null || true
}

flush_host() {
    printf 'flush\n' | remote_write "$1" "$PROC_ROOT/config" 2>/dev/null || true
}

snap_ports() { # host outfile
    qa_ssh "$1" "cat '$PROC_ROOT/ports'" >"$2" 2>/dev/null || true
}

snap_peers() { # host outfile
    qa_ssh "$1" "cat '$PROC_ROOT/peers'" >"$2" 2>/dev/null || true
}

ports_directive() { # ports-view file -> lo-hi
    [ -n "${1:-}" ] && [ -f "$1" ] || return 0
    sed -n 's/^ports //p' "$1" 2>/dev/null | tail -1
}

ports_configured() { # ports-view file -> 0 configured, 1 default
    grep -q '^# default' "$1" 2>/dev/null
}

peer_up_count() { # peers file id
    awk -v id="$2" '$1 !~ /^#/ && $1 == id && $8 == "up" { n++ } END { print n + 0 }' "$1"
}

peer_sports() { # peers file id
    awk -v id="$2" '$1 !~ /^#/ && $1 == id && $8 == "up" { print $3 }' "$1" |
        sort -n
}

peer_all_sports() { # peers file
    awk '$1 !~ /^#/ && $8 == "up" && $3 != 0 { print $3 }' "$1" | sort -n
}

peer_tuples() { # peers file -> "sport peer" for up rows
    awk '$1 !~ /^#/ && $8 == "up" { print $3, $4 }' "$1" | sort
}

check_window() { # peers file id lo-hi label
    local file=$1 id=$2 range=$3 label=$4 lo hi bad=0 n=0 s
    range_parse "$range" || { qa_check "$label" 1 "bad range $range"; return; }
    lo=$R_LO hi=$R_HI
    while read -r s; do
        [ -n "$s" ] || continue
        n=$((n + 1))
        if ! port_in "$s" "$lo" "$hi"; then
            bad=1
        fi
    done < <(peer_sports "$file" "$id")
    qa_check "$label" "$([ "$bad" -eq 0 ] && [ "$n" -gt 0 ]; echo $?)" \
        "id=$id ports=$n window=$range"
}

check_no_dup_sports() { # peers file label
    local dup
    dup=$(peer_all_sports "$1" | uniq -d | tr '\n' ',' | sed 's/,$//')
    qa_check "$2" "$([ -z "$dup" ]; echo $?)" "duplicates=${dup:-none}"
}

check_no_dup_tuples() { # peers file label
    local dup
    dup=$(peer_tuples "$1" | uniq -d | tr '\n' ';' | sed 's/;$//')
    qa_check "$2" "$([ -z "$dup" ]; echo $?)" "duplicates=${dup:-none}"
}

check_no_overlap() { # fileA idA fileB idB label
    local common
    common=$(comm -12 <(peer_sports "$1" "$2") <(peer_sports "$3" "$4") |
        tr '\n' ',' | sed 's/,$//')
    qa_check "$5" "$([ -z "$common" ]; echo $?)" "shared=${common:-none}"
}

# Join a far peers snapshot to the near one by (id, carrier). A far row whose
# peer port differs from the near's current source port for the same carrier
# belongs to a connection the near has already replaced; it is reported as
# stale_row and never judged, because the far keeps the accepted socket until
# that TCP closes.
align_far_near() { # far-file near-file id -> "MATCH|STALE id carrier far_port near_sport"
    awk -v want="$3" '
        NR == FNR {
            if ($1 !~ /^#/ && $8 == "up")
                near[$1 " " $2] = $3
            next
        }
        $1 !~ /^#/ && $1 == want && $8 == "up" {
            p = $4; sub(/^.*:/, "", p); p += 0
            k = $1 " " $2
            if (k in near && near[k] + 0 == p)
                print "MATCH", $1, $2, p, near[k]
            else if (k in near)
                print "STALE", $1, $2, p, near[k]
            else
                print "STALE", $1, $2, p, "none"
        }' "$2" "$1"
}

# Assert the far's peer port for every current carrier: the (id, carrier) row
# must exist in the near snapshot, and a matching row must name a port inside
# the tunnel's window. Stale rows are counted, printed, and skipped.
far_window_check() { # label far-file near-file "id:window ..."
    local label=$1 farf=$2 nearf=$3 spec=$4 item id pb out kind fid c p ns
    local bad=0 checked=0 stale=0
    for item in $spec; do
        id=${item%%:*} pb=${item#*:}
        out=$(align_far_near "$farf" "$nearf" "$id")
        while read -r kind fid c p ns; do
            [ -n "${kind:-}" ] || continue
            if [ "$kind" = STALE ]; then
                stale=$((stale + 1))
                qa_note "INFO stale_row tunnel=$fid carrier=$c far_peer_port=$p near_sport=$ns (not asserted)"
                continue
            fi
            checked=$((checked + 1))
            range_parse "$pb"
            if [ "$p" -lt "$R_LO" ] || [ "$p" -gt "$R_HI" ]; then
                bad=1
                qa_note "INFO far_out_of_window tunnel=$fid carrier=$c far_peer_port=$p window=$pb"
            fi
        done <<<"$out"
    done
    qa_check "$label" "$bad" "checked=$checked stale_skipped=$stale"
}

wait_up() { # host id expected timeout
    local host=$1 id=$2 want=$3 tmo=$4 end up
    end=$((SECONDS + tmo))
    while ((SECONDS < end)); do
        qa_snap_stats "$host" "$EV/stats-$host-now.txt"
        up=$(qa_field "$EV/stats-$host-now.txt" c3 up "$id")
        if [ -n "$up" ] && [ "$up" = "$want" ]; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_exhaust() { # host id timeout -> 0 when port_exhaust > 0 and up == 0
    local host=$1 id=$2 tmo=$3 end pe up
    end=$((SECONDS + tmo))
    while ((SECONDS < end)); do
        qa_snap_stats "$host" "$EV/stats-$host-now.txt"
        pe=$(qa_field "$EV/stats-$host-now.txt" c3 port_exhaust "$id")
        up=$(qa_field "$EV/stats-$host-now.txt" c3 up "$id")
        if qa_num "$pe" && [ "$pe" -gt 0 ] && [ "$up" = 0 ]; then
            return 0
        fi
        sleep 1
    done
    return 1
}

port_free() { # host port -> 0 free/unknown, 1 busy
    local host=$1 port=$2 file="$EV/ss-$1.txt"
    qa_ssh "$host" "ss -ltn 2>/dev/null" >"$file" || return 0
    [ -s "$file" ] || return 0
    awk -v p=":$port" '$4 ~ (p "$") { found = 1 } END { exit found ? 1 : 0 }' "$file"
}

reserved_covers() { # list lo hi
    local list=$1 lo=$2 hi=$3
    list=$(printf '%s' "$list" | tr -d ' ')
    awk -v list="$list" -v lo="$lo" -v hi="$hi" 'BEGIN {
        n = split(list, a, ",")
        for (i = 1; i <= n; i++) {
            if (a[i] ~ /^[0-9]+-[0-9]+$/) {
                split(a[i], r, "-")
                if (lo >= r[1] + 0 && hi <= r[2] + 0) exit 0
            } else if (a[i] ~ /^[0-9]+$/ && lo == hi && lo == a[i] + 0) {
                exit 0
            }
        }
        exit 1
    }'
}

check_reserved() { # name range list
    local name=$1 range=$2 list=$3 lo hi
    range_parse "$range" || return
    lo=$R_LO hi=$R_HI
    if reserved_covers "$list" "$lo" "$hi"; then
        qa_check "$name" 0 "$range"
    elif [ "$STRICT_RESERVED" = 1 ]; then
        qa_check "$name" 1 "$range not covered by ip_local_reserved_ports"
    else
        qa_skip "$name" "$range not reserved (warning only; --strict-reserved to fail)"
    fi
}

# ---------------------------------------------------------------------------
# Config lines

nonce_for() { printf '%032x' "$1"; }

near_line() { # id pub farport portblock
    local dev=""
    [ -n "$NEAR_DEV" ] && dev=" dev=$NEAR_DEV"
    printf 'tunnel %s role=near profile=raw fec=off crypto=off listen=%s:%s peer=%s:%s n=%s m=0 portblock=%s nonce=%s%s' \
        "$1" "$NEAR_LISTEN" "$2" "$FAR_IP" "$3" "$CARRIERS" "$4" "$(nonce_for "$1")" "$dev"
}

far_line() { # id farport portblock
    printf 'tunnel %s role=far profile=raw fec=off crypto=off listen=%s:%s target=%s n=%s m=0 portblock=%s nonce=%s' \
        "$1" "$FAR_LISTEN" "$2" "$TARGET" "$CARRIERS" "$3" "$(nonce_for "$1")"
}

# ---------------------------------------------------------------------------
# Cleanup

restore_config() { # host saved
    local host=$1 saved=$2
    # Remove every test tunnel first. Writing the saved lines alone would
    # replace matching ids but leave the extra test ids running.
    flush_host "$host"
    if [ -s "$saved" ]; then
        remote_write "$host" "$PROC_ROOT/config" <"$saved" 2>/dev/null || true
    fi
}

restore_ports() { # host saved-view
    local host=$1 saved=$2 line
    line=$(ports_directive "$saved")
    [ -n "$line" ] || return 0
    ports_write "$host" "$line" 2>/dev/null || true
}

cleanup() {
    [ "$CLEANED" = 0 ] || return 0
    CLEANED=1
    trap - EXIT INT TERM
    if [ "$KEEP" = 1 ]; then
        local line
        line=$(ports_directive "$SAVED_NEAR_PORTS")
        say "KEEP=1: test tunnels left in place."
        say "  restore near: ssh $NEAR_HOST 'cat > $PROC_ROOT/config' < $EV/near-config-saved.txt"
        say "  restore far:  ssh $FAR_HOST 'cat > $PROC_ROOT/config' < $EV/far-config-saved.txt"
        [ -n "$line" ] && say "  restore near ports: ssh $NEAR_HOST \"echo 'ports $line' > $PROC_ROOT/ports\""
        return 0
    fi
    if [ "$LOADED_NEAR" = 1 ]; then
        restore_ports near "$SAVED_NEAR_PORTS"
        restore_config near "$SAVED_NEAR_CFG"
    fi
    if [ "$LOADED_FAR" = 1 ]; then
        restore_config far "$SAVED_FAR_CFG"
        restore_ports far "$SAVED_FAR_PORTS"
    fi
    say "cleanup: saved config/ports restored"
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

# ---------------------------------------------------------------------------
# Preflight

preflight() {
    local host u ports_list p busy=0 range

    if [ -z "$OUT_OVERRIDE" ]; then
        qa_evidence_init portmap || { say "cannot create evidence dir"; exit 3; }
    else
        EV=$OUT_OVERRIDE
        mkdir -p "$EV" || exit 3
        : >"$EV/run.log"
        : >"$EV/timeline.txt"
    fi
    say "EVIDENCE=$EV"

    if ! qa_ssh near true 2>/dev/null; then
        qa_check near_ssh 1 unreachable
        exit 3
    fi
    qa_check near_ssh 0 "$NEAR_HOST"
    if ! qa_ssh far true 2>/dev/null; then
        qa_check far_ssh 1 unreachable
        exit 3
    fi
    qa_check far_ssh 0 "$FAR_HOST"

    for host in near far; do
        u=$(qa_ssh "$host" 'id -u' 2>/dev/null)
        qa_check "${host}_root" "$([ "$u" = 0 ]; echo $?)" "uid=${u:-?}"
        qa_check "${host}_proc" \
            "$(qa_ssh "$host" "test -e '$PROC_ROOT/config'; echo \$?" 2>/dev/null)" \
            "$PROC_ROOT/config"
        qa_check "${host}_module" \
            "$(qa_ssh "$host" "lsmod | grep -q '^tbtun '; echo \$?" 2>/dev/null)" \
            tbtun
        qa_ssh "$host" "uname -r; modinfo -F version tbtun 2>/dev/null" \
            >"$EV/$host-version.txt" 2>/dev/null || true
        qa_ssh "$host" "sysctl -n net.ipv4.ip_local_reserved_ports 2>/dev/null" \
            >"$EV/$host-reserved.txt" 2>/dev/null || true
        qa_ssh "$host" "ss -ltn 2>/dev/null" >"$EV/ss-$host.txt" 2>/dev/null || true
    done
    if [ "$FAILED" -ne 0 ]; then
        say "preflight failed: root, $PROC_ROOT, or tbtun missing"
        exit 3
    fi
    LOADED_NEAR=1
    LOADED_FAR=1

    qa_snap_config near "$EV/near-config-saved.txt"
    qa_snap_config far "$EV/far-config-saved.txt"
    snap_ports near "$EV/near-ports-saved.txt"
    snap_ports far "$EV/far-ports-saved.txt"
    SAVED_NEAR_CFG=$EV/near-config-saved.txt
    SAVED_FAR_CFG=$EV/far-config-saved.txt
    SAVED_NEAR_PORTS=$EV/near-ports-saved.txt
    SAVED_FAR_PORTS=$EV/far-ports-saved.txt

    for range in "$PB1" "$PB2" "$GLOBAL1" "$GLOBAL2"; do
        if ! range_parse "$range"; then
            qa_check range_format 1 "$range"
            exit 3
        fi
        if [ "$(range_size "$range")" -lt "$CARRIERS" ]; then
            qa_check "range_$range" 1 "smaller than $CARRIERS carriers"
            exit 3
        fi
    done
    if ! range_parse "$FAR_PB"; then
        qa_check range_format 1 "$FAR_PB"
        exit 3
    fi
    if ! qa_num "$CARRIERS" || [ "$CARRIERS" -lt 1 ] || [ "$CARRIERS" -gt 16 ]; then
        qa_check carriers_range 1 "$CARRIERS outside 1..16"
        exit 3
    fi
    qa_check carriers_range 0 "$CARRIERS (legacy ARQ 1..16)"
    for p in "$NEAR_PUB_BASE" "$FAR_CAR_BASE"; do
        if ! qa_num "$p" || [ "$p" -lt 1 ] || [ "$p" -gt 65530 ]; then
            qa_check port_base 1 "$p outside 1..65530"
            exit 3
        fi
    done

    ports_list=$(cat "$EV/near-reserved.txt" 2>/dev/null)
    check_reserved reserved_pb1 "$PB1" "$ports_list"
    check_reserved reserved_pb2 "$PB2" "$ports_list"
    check_reserved reserved_global1 "$GLOBAL1" "$ports_list"
    check_reserved reserved_global2 "$GLOBAL2" "$ports_list"

    for p in $(seq "$NEAR_PUB_BASE" $((NEAR_PUB_BASE + 5))); do
        port_free near "$p" || { busy=1; qa_check "near_port_free_$p" 1 busy; }
    done
    for p in $(seq "$FAR_CAR_BASE" $((FAR_CAR_BASE + 5))); do
        port_free far "$p" || { busy=1; qa_check "far_port_free_$p" 1 busy; }
    done
    [ "$busy" = 0 ] && qa_check listen_ports_free 0 "$NEAR_PUB_BASE..+5 / $FAR_CAR_BASE..+5"
    if [ "$busy" = 1 ]; then
        say "choose free bases with NEAR_PUB_BASE=/FAR_CAR_BASE= or stop the listener"
        exit 3
    fi

    if [ -z "$(ports_directive "$EV/near-ports-saved.txt")" ]; then
        qa_check near_ports_view 1 "no ports directive"
        exit 3
    fi
    qa_check near_ports_view 0 "$(ports_directive "$EV/near-ports-saved.txt")"
    if [ -z "$(ports_directive "$EV/far-ports-saved.txt")" ]; then
        qa_check far_ports_view 1 "no ports directive"
        exit 3
    fi
    qa_check far_ports_view 0 "$(ports_directive "$EV/far-ports-saved.txt")"
}

# ---------------------------------------------------------------------------
# Phase 1: two tunnels, two windows

phase_two_tunnels() {
    local id farport pub
    say "phase 1: two near tunnels with distinct portblock windows"

    # Far listeners first, so a near dial always has somewhere to land.
    for id in 1 2; do
        farport=$((FAR_CAR_BASE + id - 1))
        case $id in
            1) cfg_write far "$(far_line "$id" "$farport" "$PB1")" ;;
            2) cfg_write far "$(far_line "$id" "$farport" "$PB2")" ;;
        esac
    done
    sleep 1
    for id in 1 2; do
        pub=$((NEAR_PUB_BASE + id - 1))
        farport=$((FAR_CAR_BASE + id - 1))
        case $id in
            1) cfg_write near "$(near_line "$id" "$pub" "$farport" "$PB1")" ;;
            2) cfg_write near "$(near_line "$id" "$pub" "$farport" "$PB2")" ;;
        esac
    done

    for id in 1 2; do
        qa_check "near_up_$id" "$(wait_up near "$id" "$CARRIERS" "$WAIT_UP_SECS"; echo $?)" \
            "up=$CARRIERS"
        qa_check "far_up_$id" "$(wait_up far "$id" "$CARRIERS" "$WAIT_UP_SECS"; echo $?)" \
            "up=$CARRIERS"
    done

    qa_snap_config near "$EV/near-config-phase1.txt"
    qa_snap_config far "$EV/far-config-phase1.txt"
    qa_snap_stats near "$EV/near-stats-phase1.txt"
    qa_snap_stats far "$EV/far-stats-phase1.txt"
    snap_peers near "$EV/near-peers-phase1.txt"
    snap_peers far "$EV/far-peers-phase1.txt"
    snap_ports near "$EV/near-ports-phase1.txt"
    snap_ports far "$EV/far-ports-phase1.txt"

    check_window "$EV/near-peers-phase1.txt" 1 "$PB1" near_t1_window
    check_window "$EV/near-peers-phase1.txt" 2 "$PB2" near_t2_window
    qa_check near_cfg_t1_portblock \
        "$(grep -Eq "^tunnel 1 .* portblock=$PB1( |\$)" "$EV/near-config-phase1.txt"; echo $?)" \
        "$PB1"
    qa_check near_cfg_t2_portblock \
        "$(grep -Eq "^tunnel 2 .* portblock=$PB2( |\$)" "$EV/near-config-phase1.txt"; echo $?)" \
        "$PB2"
    qa_check near_t1_count \
        "$([ "$(peer_up_count "$EV/near-peers-phase1.txt" 1)" = "$CARRIERS" ]; echo $?)" \
        "$(peer_up_count "$EV/near-peers-phase1.txt" 1)/$CARRIERS"
    qa_check near_t2_count \
        "$([ "$(peer_up_count "$EV/near-peers-phase1.txt" 2)" = "$CARRIERS" ]; echo $?)" \
        "$(peer_up_count "$EV/near-peers-phase1.txt" 2)/$CARRIERS"
    check_no_dup_sports "$EV/near-peers-phase1.txt" near_no_dup_sports_p1
    check_no_dup_tuples "$EV/near-peers-phase1.txt" near_no_dup_tuples_p1

    # The near peer column must be the far carrier listener of its own tunnel.
    qa_check near_peer_ports \
        "$(awk -v c1="$((FAR_CAR_BASE))" -v c2="$((FAR_CAR_BASE + 1))" \
            '$1 !~ /^#/ && $8 == "up" {
                p = $4; sub(/^.*:/, "", p)
                if (($1 == 1 && p != c1) || ($1 == 2 && p != c2)) bad = 1
            } END { exit bad ? 1 : 0 }' "$EV/near-peers-phase1.txt"; echo $?)" \
        "far listen $FAR_CAR_BASE/$((FAR_CAR_BASE + 1))"

    # Far records the key and never allocates a carrier source port.
    qa_check far_cfg_t1_portblock \
        "$(grep -Eq "^tunnel 1 .* portblock=$PB1( |\$)" "$EV/far-config-phase1.txt"; echo $?)" \
        "$PB1"
    qa_check far_cfg_t2_portblock \
        "$(grep -Eq "^tunnel 2 .* portblock=$PB2( |\$)" "$EV/far-config-phase1.txt"; echo $?)" \
        "$PB2"
    for id in 1 2; do
        qa_check "far_t${id}_no_port_alloc" \
            "$([ "$(qa_field "$EV/far-stats-phase1.txt" c3 port_exhaust "$id")" = 0 ] &&
               [ "$(qa_field "$EV/far-stats-phase1.txt" c3 port_alloc_fail "$id")" = 0 ]; echo $?)" \
            "port_exhaust=0 port_alloc_fail=0"
    done

    # The far peers view names the near's source port in the peer column. Only
    # carriers the near still holds are asserted; a far row that names a
    # replaced connection is reported as stale_row instead of failing.
    far_window_check far_sees_near_ports \
        "$EV/far-peers-phase1.txt" "$EV/near-peers-phase1.txt" "1:$PB1 2:$PB2"
}

# ---------------------------------------------------------------------------
# Phase 2: overlapping window exhausts instead of stealing

phase_exhaustion() {
    local sports n lo hi id=3 farport pub
    say "phase 2: overlapping window reports port_exhaust"

    sports=$(peer_sports "$EV/near-peers-phase1.txt" 1)
    n=$(printf '%s\n' "$sports" | grep -c .)
    if [ "$n" != "$CARRIERS" ]; then
        qa_skip exhaust_overlap "tunnel 1 has $n/$CARRIERS up ports"
        return
    fi
    lo=$(printf '%s\n' "$sports" | head -1)
    hi=$(printf '%s\n' "$sports" | tail -1)
    if [ $((10#$hi - 10#$lo + 1)) != "$CARRIERS" ]; then
        qa_skip exhaust_overlap "tunnel 1 ports are not contiguous: $lo..$hi"
        return
    fi
    EXH_LO=$lo
    EXH_HI=$hi
    say "  tunnel 1 holds $lo..$hi; tunnel 3 will ask for the same $CARRIERS ports"

    farport=$((FAR_CAR_BASE + id - 1))
    pub=$((NEAR_PUB_BASE + id - 1))
    cfg_write far "$(far_line "$id" "$farport" "$PB1")"
    sleep 1
    cfg_write near "$(near_line "$id" "$pub" "$farport" "$EXH_LO-$EXH_HI")"

    qa_check exhaust_seen "$(wait_exhaust near "$id" "$WAIT_EXHAUST_SECS"; echo $?)" \
        "port_exhaust>0 and up=0 within ${WAIT_EXHAUST_SECS}s"
    qa_snap_stats near "$EV/near-stats-phase2.txt"
    qa_snap_stats far "$EV/far-stats-phase2.txt"
    snap_peers near "$EV/near-peers-phase2.txt"
    qa_snap_config near "$EV/near-config-phase2.txt"

    qa_check exhaust_up_zero \
        "$([ "$(qa_field "$EV/near-stats-phase2.txt" c3 up 3)" = 0 ]; echo $?)" \
        "up=$(qa_field "$EV/near-stats-phase2.txt" c3 up 3)"
    qa_check exhaust_empty \
        "$([ "$(qa_field "$EV/near-stats-phase2.txt" c3 empty 3)" = "$CARRIERS" ]; echo $?)" \
        "empty=$(qa_field "$EV/near-stats-phase2.txt" c3 empty 3)/$CARRIERS"
    qa_check exhaust_alloc_fail_zero \
        "$([ "$(qa_field "$EV/near-stats-phase2.txt" c3 port_alloc_fail 3)" = 0 ]; echo $?)" \
        "ENOSPC, not ENOMEM"
    qa_check exhaust_no_up_rows \
        "$([ "$(peer_up_count "$EV/near-peers-phase2.txt" 3)" = 0 ]; echo $?)" \
        "no carrier received a port"

    # The far drops tunnel 3 together with its near peer, so its config and
    # peers must be captured here; phase 5 reads these files.
    qa_snap_config far "$EV/far-config-phase2.txt"
    snap_peers far "$EV/far-peers-phase2.txt"

    # Remove the exhausted near tunnel. Keep far 3 configured: it proves the
    # far side records portblock= even when no carrier ever arrives.
    del_tunnel near "$id"
}

# ---------------------------------------------------------------------------
# Phase 3: 0-0 falls back to the global block

phase_global_fallback() {
    local id=4 farport pub
    say "phase 3: portblock=0-0 uses the global /ports block"

    ports_write near "$GLOBAL1"
    snap_ports near "$EV/near-ports-global1.txt"
    qa_check ports_global1 \
        "$([ "$(ports_directive "$EV/near-ports-global1.txt")" = "$GLOBAL1" ]; echo $?)" \
        "ports $(ports_directive "$EV/near-ports-global1.txt")"
    qa_check ports_global1_configured \
        "$(ports_configured "$EV/near-ports-global1.txt"; [ $? -eq 1 ]; echo $?)" configured

    farport=$((FAR_CAR_BASE + id - 1))
    pub=$((NEAR_PUB_BASE + id - 1))
    cfg_write far "$(far_line "$id" "$farport" "0-0")"
    sleep 1
    cfg_write near "$(near_line "$id" "$pub" "$farport" "0-0")"
    qa_check global_t4_up "$(wait_up near "$id" "$CARRIERS" "$WAIT_UP_SECS"; echo $?)" \
        "up=$CARRIERS"

    qa_snap_config near "$EV/near-config-phase3.txt"
    qa_snap_stats near "$EV/near-stats-phase3.txt"
    snap_peers near "$EV/near-peers-phase3.txt"
    snap_ports near "$EV/near-ports-phase3.txt"

    check_window "$EV/near-peers-phase3.txt" "$id" "$GLOBAL1" global_t4_window
    qa_check global_t4_cfg \
        "$(grep -Eq "^tunnel $id .* portblock=0-0( |\$)" "$EV/near-config-phase3.txt"; echo $?)" \
        "portblock=0-0"
    check_no_dup_sports "$EV/near-peers-phase3.txt" near_no_dup_sports_p3
    check_no_overlap "$EV/near-peers-phase1.txt" 1 "$EV/near-peers-phase3.txt" "$id" \
        global_t4_no_pb1_overlap
    check_no_overlap "$EV/near-peers-phase1.txt" 2 "$EV/near-peers-phase3.txt" "$id" \
        global_t4_no_pb2_overlap
}

# ---------------------------------------------------------------------------
# Phase 4: /ports changes affect new allocations; live bits stay

phase_ports_change() {
    local id=5 farport pub sports old skip_bad
    say "phase 4: /ports changes new allocations and keeps live bits"

    old=$(peer_sports "$EV/near-peers-phase3.txt" 4)
    T4_OLD=$old
    qa_check phase4_t4_ports \
        "$([ -n "$old" ]; echo $?)" "tunnel 4 holds $(printf '%s' "$old" | tr '\n' ',')"

    # Move the global window away from tunnel 4's live ports.
    ports_write near "$GLOBAL2"
    snap_ports near "$EV/near-ports-global2.txt"
    qa_check ports_global2 \
        "$([ "$(ports_directive "$EV/near-ports-global2.txt")" = "$GLOBAL2" ]; echo $?)" \
        "ports $(ports_directive "$EV/near-ports-global2.txt")"
    snap_peers near "$EV/near-peers-after-global2.txt"

    # Live carriers keep their ports; a replaced carrier would use the new
    # window, so accept that but require the original four to still be present
    # when the carrier set did not churn.
    qa_check live_ports_kept \
        "$(for p in $old; do
               grep -qE "^[0-9]+ [0-9]+ $p " "$EV/near-peers-after-global2.txt" || exit 1
           done; echo $?)" \
        "tunnel 4 keeps $(printf '%s' "$old" | tr '\n' ',')"
    qa_check t4_still_up \
        "$([ "$(peer_up_count "$EV/near-peers-after-global2.txt" 4)" = "$CARRIERS" ]; echo $?)" \
        "up=$(peer_up_count "$EV/near-peers-after-global2.txt" 4)/$CARRIERS"

    # A new 0-0 tunnel must use the new window.
    farport=$((FAR_CAR_BASE + id - 1))
    pub=$((NEAR_PUB_BASE + id - 1))
    cfg_write far "$(far_line "$id" "$farport" "$FAR_PB")"
    sleep 1
    cfg_write near "$(near_line "$id" "$pub" "$farport" "0-0")"
    qa_check global2_t5_up "$(wait_up near "$id" "$CARRIERS" "$WAIT_UP_SECS"; echo $?)" \
        "up=$CARRIERS"
    qa_snap_stats near "$EV/near-stats-phase4.txt"
    snap_peers near "$EV/near-peers-phase4.txt"
    check_window "$EV/near-peers-phase4.txt" "$id" "$GLOBAL2" global2_t5_window
    check_no_overlap "$EV/near-peers-phase4.txt" 4 "$EV/near-peers-phase4.txt" "$id" \
        global2_t5_no_t4_overlap

    # Put the window back. Tunnel 4's bits are still live, so tunnel 6 must
    # skip them and take the next free ports in the same window.
    ports_write near "$GLOBAL1"
    snap_ports near "$EV/near-ports-global1-again.txt"
    qa_check ports_global1_again \
        "$([ "$(ports_directive "$EV/near-ports-global1-again.txt")" = "$GLOBAL1" ]; echo $?)" \
        "ports $(ports_directive "$EV/near-ports-global1-again.txt")"

    id=6
    farport=$((FAR_CAR_BASE + id - 1))
    pub=$((NEAR_PUB_BASE + id - 1))
    cfg_write far "$(far_line "$id" "$farport" "$FAR_PB")"
    sleep 1
    cfg_write near "$(near_line "$id" "$pub" "$farport" "0-0")"
    qa_check global1_t6_up "$(wait_up near "$id" "$CARRIERS" "$WAIT_UP_SECS"; echo $?)" \
        "up=$CARRIERS"
    qa_snap_stats near "$EV/near-stats-phase4b.txt"
    snap_peers near "$EV/near-peers-phase4b.txt"
    check_window "$EV/near-peers-phase4b.txt" "$id" "$GLOBAL1" global1_t6_window

    # Tunnel 6 must not reuse tunnel 4's still-live bits.
    sports=$(peer_sports "$EV/near-peers-phase4b.txt" 6)
    skip_bad=0
    for p in $old; do
        printf '%s\n' "$sports" | grep -qx "$p" && skip_bad=1
    done
    qa_check global1_t6_skips_live "$skip_bad" \
        "tunnel 6 avoids tunnel 4's live ports (t4=$(printf '%s' "$old" | tr '\n' ',') t6=$(printf '%s' "$sports" | tr '\n' ','))"
    check_no_overlap "$EV/near-peers-phase4b.txt" 4 "$EV/near-peers-phase4b.txt" "$id" \
        global1_t6_no_t4_overlap
    check_no_dup_sports "$EV/near-peers-phase4b.txt" near_no_dup_sports_p4
}

# ---------------------------------------------------------------------------
# Phase 5: far records, never allocates

phase_far_record() {
    local id p pb
    say "phase 5: far records portblock= and never allocates"

    qa_snap_config far "$EV/far-config-final.txt"
    qa_snap_stats far "$EV/far-stats-final.txt"
    snap_peers far "$EV/far-peers-final.txt"
    snap_ports far "$EV/far-ports-final.txt"

    qa_check far_cfg_t3_portblock \
        "$(grep -Eq "^tunnel 3 .* portblock=$PB1( |\$)" "$EV/far-config-phase2.txt"; echo $?)" \
        "$PB1 (recorded; phase-2 snapshot, tunnel 3 was deleted on near)"
    qa_check far_cfg_t4_portblock \
        "$(grep -Eq "^tunnel 4 .* portblock=0-0( |\$)" "$EV/far-config-final.txt"; echo $?)" \
        "0-0"
    qa_check far_cfg_t5_portblock \
        "$(grep -Eq "^tunnel 5 .* portblock=$FAR_PB( |\$)" "$EV/far-config-final.txt"; echo $?)" \
        "$FAR_PB"
    qa_check far_cfg_t6_portblock \
        "$(grep -Eq "^tunnel 6 .* portblock=$FAR_PB( |\$)" "$EV/far-config-final.txt"; echo $?)" \
        "$FAR_PB"

    qa_check far_t3_no_alloc_phase2 \
        "$([ "$(qa_field "$EV/far-stats-phase2.txt" c3 port_exhaust 3)" = 0 ] &&
           [ "$(qa_field "$EV/far-stats-phase2.txt" c3 port_alloc_fail 3)" = 0 ]; echo $?)" \
        "port_exhaust=0 port_alloc_fail=0 (phase-2 snapshot)"
    for id in 1 2 4 5 6; do
        qa_check "far_t${id}_no_alloc_final" \
            "$([ "$(qa_field "$EV/far-stats-final.txt" c3 port_exhaust "$id")" = 0 ] &&
               [ "$(qa_field "$EV/far-stats-final.txt" c3 port_alloc_fail "$id")" = 0 ]; echo $?)" \
            "port_exhaust=0 port_alloc_fail=0"
    done

    # Far peers for the global-fallback tunnels: the near source port in the
    # peer column is in GLOBAL1/GLOBAL2, never in FAR_PB. Rows are joined to
    # the near snapshot by (id, carrier).
    far_window_check far_t4_sees_near_window \
        "$EV/far-peers-final.txt" "$EV/near-peers-phase4b.txt" "4:$GLOBAL1"
    far_window_check far_t5_sees_near_window \
        "$EV/far-peers-final.txt" "$EV/near-peers-phase4b.txt" "5:$GLOBAL2"
    far_window_check far_t6_sees_near_window \
        "$EV/far-peers-final.txt" "$EV/near-peers-phase4b.txt" "6:$GLOBAL1"
}

# ---------------------------------------------------------------------------
# Final cross-checks and summary

final_checks() {
    local all dup tuples
    snap_peers near "$EV/near-peers-final.txt"
    qa_snap_stats near "$EV/near-stats-final.txt"
    qa_snap_config near "$EV/near-config-final.txt"
    snap_ports near "$EV/near-ports-final.txt"
    qa_ssh near "ss -tnp 2>/dev/null" >"$EV/near-ss-final.txt" 2>/dev/null || true
    qa_ssh far "ss -tnp 2>/dev/null" >"$EV/far-ss-final.txt" 2>/dev/null || true

    check_no_dup_sports "$EV/near-peers-final.txt" final_no_dup_sports
    check_no_dup_tuples "$EV/near-peers-final.txt" final_no_dup_tuples
    check_window "$EV/near-peers-final.txt" 1 "$PB1" final_t1_window
    check_window "$EV/near-peers-final.txt" 2 "$PB2" final_t2_window
    check_window "$EV/near-peers-final.txt" 4 "$GLOBAL1" final_t4_window
    check_window "$EV/near-peers-final.txt" 5 "$GLOBAL2" final_t5_window
    check_window "$EV/near-peers-final.txt" 6 "$GLOBAL1" final_t6_window

    qa_check near_ports_final \
        "$([ -n "$(ports_directive "$EV/near-ports-final.txt")" ]; echo $?)" \
        "$(ports_directive "$EV/near-ports-final.txt")"
    qa_check far_ports_final \
        "$([ -n "$(ports_directive "$EV/far-ports-final.txt")" ]; echo $?)" \
        "$(ports_directive "$EV/far-ports-final.txt")"

    all=$(peer_all_sports "$EV/near-peers-final.txt")
    dup=$(printf '%s\n' "$all" | uniq -d | tr '\n' ',' | sed 's/,$//')
    qa_check final_bitmap_unique 0 "sports=$(printf '%s' "$all" | tr '\n' ',') duplicates=${dup:-none}"

    {
        echo "PORTMAP_RESULT $([ "$FAILED" -eq 0 ] && echo PASS || echo FAIL)"
        echo "date=$(qa_now)"
        echo "near=$NEAR_HOST far=$FAR_HOST far_ip=$FAR_IP"
        echo "pb1=$PB1 pb2=$PB2 global1=$GLOBAL1 global2=$GLOBAL2 far_pb=$FAR_PB carriers=$CARRIERS"
        echo "exhaust_window=${EXH_LO:-?}-${EXH_HI:-?}"
        echo "near_ports=$(ports_directive "$EV/near-ports-final.txt")"
        echo "far_ports=$(ports_directive "$EV/far-ports-final.txt")"
        echo "--- checks ---"
        cat "$EV/run.log"
        echo "--- near peers (sport peer state) ---"
        awk '$1 !~ /^#/ { print $1, $2, $3, $4, $8 }' "$EV/near-peers-final.txt"
        echo "--- far peers (sport peer state) ---"
        awk '$1 !~ /^#/ { print $1, $2, $3, $4, $8 }' "$EV/far-peers-final.txt"
    } >"$EV/summary.txt"
}

# ---------------------------------------------------------------------------
# Main

preflight
phase_two_tunnels
phase_exhaustion
phase_global_fallback
phase_ports_change
phase_far_record
final_checks

if [ "$FAILED" -eq 0 ]; then
    say "PORTMAP_RESULT PASS"
    say "evidence: $EV"
    exit 0
fi
say "PORTMAP_RESULT FAIL ($FAILED checks)"
say "evidence: $EV"
exit 1
