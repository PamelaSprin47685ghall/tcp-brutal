#!/usr/bin/env bash
# multiport-accept.sh - real-hardware acceptance for one tunnel, several ports.
#
# WHAT THIS PROVES
#   1. One 256-carrier session (n=4 m=4, RLNC grouped) carries three mappings
#      from one tunnel line: listen=127.0.0.1:9001 plus map=9002:5211,9003:5212.
#      The near side binds :9001, :9002 and :9003; each accepted connection
#      sends its slot's target port in OPEN.
#   2. The far side runs three origins on 127.0.0.1:5210,5211,5212 with
#      distinct deterministic bodies and an X-TBT-Service identity header. A
#      sequential round and a concurrent RLNC multi-stream round both return
#      the sha256 and identity of the mapped origin, so the same carrier set
#      routes every port without cross-delivery.
#   3. The far side is the security boundary. With 9003:5212 removed from the
#      far line while the near line still has it, a request to :9003 is
#      dropped: the far logs the refusal, never dials :5212, creates no far
#      stream, and leaves all 256 carriers up with carrier_lost and
#      carrier_replaced unchanged. :9001 and :9002 still answer correctly.
#
# WHAT IT DOES NOT DO
#   - It does not build, load, unload or reinstall tbtun. Both hosts must
#     already run the 0.5.0 module and be reachable as root over SSH.
#   - It does not test a mixed 0.4.0/0.5.0 pair: that pair fails the first
#     header check by design.
#
# USAGE
#   scripts/qa/multiport-accept.sh [options]
#
#   --near HOST       SSH target for the near side (default root@192.168.123.85)
#   --far HOST        SSH target for the far side (default root@45.11.1.62)
#   --dry-run         print the plan and validate local inputs; touch no host
#   --cleanup         stop the test services and restore the saved config
#   --keep            leave tunnel 1 and the remote files in place
#   --out DIR         evidence directory (also accepted as --evidence)
#   --bytes N         body size per origin (default 1048576)
#   --timeout S       per valid HTTP request timeout (default 120)
#   -h, --help        this text
#
# ENVIRONMENT (all optional)
#   MP_FAR_IP=45.11.1.62         address the near carriers dial
#   MP_NEAR_PORT=9001            near slot-0 listen port
#   MP_MAP1_PORT=9002            near map slot 1 listen port
#   MP_MAP2_PORT=9003            near map slot 2 listen port
#   MP_FAR_CARRIER_PORT=9000     far carrier listen port
#   MP_SVC0_PORT=5210            far origin for slot 0
#   MP_SVC1_PORT=5211            far origin for map slot 1
#   MP_SVC2_PORT=5212            far origin for map slot 2
#   MP_CARRIERS=256              carriers in the one session
#   MP_GROUPS_PER_FLOW=8         required fec_groups_per_flow on both ends
#   MP_BYTES=1048576             body size
#   MP_TIMEOUT=120               valid request timeout
#   MP_REFUSE_TIMEOUT=15         timeout for the refused :9003 request
#   MP_PROBE_SECS=5              wait before probing the refused request
#   MP_WAIT_UP_SECS=90           carrier bring-up timeout
#   MP_DIR=/root/tbtun-multiport remote work directory
#   MP_FAR_LISTEN=0.0.0.0        far carrier bind address
#   MP_PORTBLOCK=30000-30511     near carrier source-port window
#   NEAR_HOST, FAR_HOST, EVIDENCE_ROOT, SSH_TMO  from scripts/qa/lib.sh
#
# EVIDENCE
#   Every phase writes config, stats, request headers/bodies and the far
#   target log under $EV. summary.txt ends with MULTIPORT_RESULT PASS|FAIL.
#
# EXIT CODES
#   0 PASS   2 usage   3 preflight   4 target/setup   6 checks
#
# The script is single shot. It replaces tunnel 1 on both ends and restores
# the saved config on exit unless --keep is given. Load both ends with
# fec_groups_per_flow=8: the 32-group shape admits the primary plus three
# concurrent flows.
set -uo pipefail

PROG=${0##*/}
SELF_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib.sh
. "$SELF_DIR/lib.sh"

CARRIERS=${MP_CARRIERS:-256}
GROUPS_PER_FLOW=${MP_GROUPS_PER_FLOW:-8}
NEAR_PORT=${MP_NEAR_PORT:-9001}
MAP1_PORT=${MP_MAP1_PORT:-9002}
MAP2_PORT=${MP_MAP2_PORT:-9003}
FAR_CARRIER_PORT=${MP_FAR_CARRIER_PORT:-9000}
SVC0_PORT=${MP_SVC0_PORT:-5210}
SVC1_PORT=${MP_SVC1_PORT:-5211}
SVC2_PORT=${MP_SVC2_PORT:-5212}
BODY_BYTES=${MP_BYTES:-1048576}
REQ_TIMEOUT=${MP_TIMEOUT:-120}
REFUSE_TIMEOUT=${MP_REFUSE_TIMEOUT:-15}
PROBE_SECS=${MP_PROBE_SECS:-5}
WAIT_UP_SECS=${MP_WAIT_UP_SECS:-90}
MP_DIR=${MP_DIR:-/root/tbtun-multiport}
NEAR_DIR=$MP_DIR/near
TUNNEL=${TUNNEL:-1}
NONCE=${MP_NONCE:-00112233445566778899aabbccddeeff}
PORTBLOCK=${MP_PORTBLOCK:-30000-30511}
FAR_LISTEN=${MP_FAR_LISTEN:-0.0.0.0}
FAR_ADDR=${MP_FAR_IP:-$FAR_IP}
EXPECT_VERSION=${MP_EXPECT_VERSION:-0.5.0}
MAP_ALL="$MAP1_PORT:$SVC1_PORT,$MAP2_PORT:$SVC2_PORT"
MAP_SAFE="$MAP1_PORT:$SVC1_PORT"

MODE=run
KEEP=0
RESTORE=1
EV_OVERRIDE=${EV:-}
EV=${EV:-}
FAILED=0
CLEANED=0

usage() {
    sed -n '2,/^set -uo pipefail$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while (($#)); do
    case $1 in
        --near) NEAR_HOST=${2:?}; shift 2 ;;
        --far) FAR_HOST=${2:?}; shift 2 ;;
        --dry-run) MODE=dry; shift ;;
        --cleanup) MODE=cleanup; shift ;;
        --keep) KEEP=1; RESTORE=0; shift ;;
        --out | --evidence) EV_OVERRIDE=${2:?}; shift 2 ;;
        --bytes) BODY_BYTES=${2:?}; shift 2 ;;
        --timeout) REQ_TIMEOUT=${2:?}; shift 2 ;;
        -h | --help) usage 0 ;;
        *) printf '%s: unknown argument: %s\n' "$PROG" "$1" >&2; usage 2 ;;
    esac
done

say() { qa_say "$*"; }

# ---------------------------------------------------------------------------
# Plan validation and rendering

validate_settings() {
    local spec name val
    for spec in \
        "near_port=$NEAR_PORT" "map1_port=$MAP1_PORT" "map2_port=$MAP2_PORT" \
        "far_carrier_port=$FAR_CARRIER_PORT" "svc0_port=$SVC0_PORT" \
        "svc1_port=$SVC1_PORT" "svc2_port=$SVC2_PORT"; do
        name=${spec%%=*}
        val=${spec#*=}
        if ! qa_num "$val" || [ "$val" -lt 1 ] || [ "$val" -gt 65535 ]; then
            printf '%s: bad %s=%s\n' "$PROG" "$name" "$val" >&2
            exit 2
        fi
    done
    if [ "$NEAR_PORT" = "$MAP1_PORT" ] || [ "$NEAR_PORT" = "$MAP2_PORT" ] ||
        [ "$MAP1_PORT" = "$MAP2_PORT" ] ||
        [ "$FAR_CARRIER_PORT" = "$SVC0_PORT" ] ||
        [ "$FAR_CARRIER_PORT" = "$SVC1_PORT" ] ||
        [ "$FAR_CARRIER_PORT" = "$SVC2_PORT" ] ||
        [ "$SVC0_PORT" = "$SVC1_PORT" ] ||
        [ "$SVC0_PORT" = "$SVC2_PORT" ] ||
        [ "$SVC1_PORT" = "$SVC2_PORT" ]; then
        printf '%s: duplicate port in the plan\n' "$PROG" >&2
        exit 2
    fi
    if ! qa_num "$BODY_BYTES" || [ "$BODY_BYTES" -lt 1 ]; then
        printf '%s: bad --bytes=%s\n' "$PROG" "$BODY_BYTES" >&2
        exit 2
    fi
    if ! qa_num "$REQ_TIMEOUT" || [ "$REQ_TIMEOUT" -lt 1 ] ||
        ! qa_num "$REFUSE_TIMEOUT" || [ "$REFUSE_TIMEOUT" -lt 1 ] ||
        ! qa_num "$PROBE_SECS" || [ "$PROBE_SECS" -lt 1 ] ||
        [ "$PROBE_SECS" -ge "$REFUSE_TIMEOUT" ] ||
        ! qa_num "$WAIT_UP_SECS" || [ "$WAIT_UP_SECS" -lt 1 ]; then
        printf '%s: bad timeout setting\n' "$PROG" >&2
        exit 2
    fi
    if ! qa_num "$CARRIERS" || [ "$CARRIERS" -lt 8 ] ||
        [ $((CARRIERS % 8)) -ne 0 ]; then
        printf '%s: MP_CARRIERS=%s must be a multiple of n+m=8\n' \
            "$PROG" "$CARRIERS" >&2
        exit 2
    fi
    if ! qa_num "$GROUPS_PER_FLOW" || [ "$GROUPS_PER_FLOW" -lt 1 ]; then
        printf '%s: bad MP_GROUPS_PER_FLOW=%s\n' "$PROG" "$GROUPS_PER_FLOW" >&2
        exit 2
    fi
    case $NONCE in
        *[!0-9a-f]*) printf '%s: nonce must be lowercase hex\n' "$PROG" >&2
            exit 2 ;;
    esac
    if [ "${#NONCE}" != 16 ] && [ "${#NONCE}" != 32 ]; then
        printf '%s: nonce must be 16 or 32 hex digits\n' "$PROG" >&2
        exit 2
    fi
}

near_line() { # map-list
    printf 'tunnel %s role=near profile=http-kal fec=on adaptive=off mode=grouped w=8 n=4 m=4 carriers=%s crypto=off dup=1 keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 portblock=%s listen=127.0.0.1:%s peer=%s:%s nonce=%s%s' \
        "$TUNNEL" "$CARRIERS" "$PORTBLOCK" "$NEAR_PORT" "$FAR_ADDR" \
        "$FAR_CARRIER_PORT" "$NONCE" "${1:+ map=$1}"
}

far_line() { # map-list
    printf 'tunnel %s role=far profile=http-kal fec=on adaptive=off mode=grouped w=8 n=4 m=4 carriers=%s crypto=off dup=1 keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 portblock=0-0 listen=%s:%s target=127.0.0.1:%s nonce=%s%s' \
        "$TUNNEL" "$CARRIERS" "$FAR_LISTEN" "$FAR_CARRIER_PORT" "$SVC0_PORT" \
        "$NONCE" "${1:+ map=$1}"
}

dry_run() {
    validate_settings
    cat <<EOF
DRY-RUN: no host is contacted and no evidence is written.
near:  $NEAR_HOST
far:   $FAR_HOST  (near carriers dial $FAR_ADDR:$FAR_CARRIER_PORT)
tunnel $TUNNEL: $CARRIERS carriers, n=4 m=4, fec=on mode=grouped
  far : $(far_line "$MAP_ALL")
  near: $(near_line "$MAP_ALL")
origins on far 127.0.0.1:
  $SVC0_PORT svc0 -> near :$NEAR_PORT
  $SVC1_PORT svc1 -> near :$MAP1_PORT
  $SVC2_PORT svc2 -> near :$MAP2_PORT
phases:
  1. apply both lines; wait for up=$CARRIERS on both ends
  2. sequential GET on :$NEAR_PORT, :$MAP1_PORT, :$MAP2_PORT; compare sha256
     and X-TBT-Service against the far origins
  3. concurrent GET on the three ports (RLNC multi-stream); compare again
  4. re-apply the far line without $MAP2_PORT:$SVC2_PORT, keep the near map;
     GET :$MAP2_PORT must be dropped with all carriers still up
  5. GET :$NEAR_PORT and :$MAP1_PORT again; the session still routes
requirements: 0.5.0 loaded on both ends; fec_groups_per_flow=$GROUPS_PER_FLOW;
  free ports $NEAR_PORT-$MAP2_PORT on near,
  $FAR_CARRIER_PORT and $SVC0_PORT-$SVC2_PORT on far
EOF
    exit 0
}

# ---------------------------------------------------------------------------
# Remote helpers

cfg_write() { # host line
    printf '%s\n' "$2" | qa_ssh "$1" "cat > '$PROC_ROOT/config'"
}

port_free() { # host port
    qa_ssh "$1" "ss -Hltn 2>/dev/null" 2>/dev/null |
        awk -v p=":$2" '$4 ~ (p "$") { found = 1 } END { exit found ? 1 : 0 }'
}

wait_up() { # host want timeout
    local end=$((SECONDS + $3))
    while ((SECONDS < end)); do
        qa_snap_stats "$1" "$EV/stats-$1-up.txt"
        [ "$(qa_field "$EV/stats-$1-up.txt" c3 up "$TUNNEL")" = "$2" ] &&
            return 0
        sleep 1
    done
    return 1
}

wait_streams_idle() { # host timeout
    local end=$((SECONDS + $2))
    while ((SECONDS < end)); do
        qa_snap_stats "$1" "$EV/stats-$1-idle.txt"
        [ "$(qa_field "$EV/stats-$1-idle.txt" streams streams_active "$TUNNEL")" = 0 ] &&
            return 0
        sleep 1
    done
    return 1
}

wait_remote_file() { # host path timeout
    local end=$((SECONDS + $3))
    while ((SECONDS < end)); do
        qa_ssh "$1" "test -s '$2'" >/dev/null 2>&1 && return 0
        sleep 1
    done
    return 1
}

carrier_state() { # stats-file
    printf 'up=%s lost=%s replaced=%s' \
        "$(qa_field "$1" c3 up "$TUNNEL")" \
        "$(qa_field "$1" c3 lost "$TUNNEL")" \
        "$(qa_field "$1" c3 replaced "$TUNNEL")"
}

carrier_stable() { # before after
    local up1 up2
    up1=$(qa_field "$1" c3 up "$TUNNEL")
    up2=$(qa_field "$2" c3 up "$TUNNEL")
    # A WAN carrier may reconnect while the gates run; up=256 is the health
    # fact. lost/replaced are diagnostics, not a stability predicate.
    qa_num "$up1" && qa_num "$up2" &&
        [ "$up1" = "$CARRIERS" ] && [ "$up2" = "$CARRIERS" ]
}

# ---------------------------------------------------------------------------
# Far origins and near client

start_target() {
    qa_ssh far "mkdir -p '$MP_DIR'" >/dev/null 2>&1 || return 1
    timeout "$SSH_TMO" scp -q -o BatchMode=yes \
        "$SELF_DIR/multiport-target.py" "$FAR_HOST:$MP_DIR/multiport-target.py" ||
        return 1
    qa_ssh far "pkill -f '$MP_DIR/[m]ultiport-target.py' 2>/dev/null || true; \
        rm -f '$MP_DIR/ready' '$MP_DIR/ready.tmp' '$MP_DIR'/sha-*.txt \
        '$MP_DIR'/conns-*.log '$MP_DIR/target.log' '$MP_DIR/target.pid'" \
        >/dev/null 2>&1 || true
    qa_ssh far "nohup $PYTHON '$MP_DIR/multiport-target.py' --dir '$MP_DIR' \
        --bytes '$BODY_BYTES' --ports '$SVC0_PORT,$SVC1_PORT,$SVC2_PORT' \
        >'$MP_DIR/target.log' 2>&1 & echo \$! >'$MP_DIR/target.pid'" \
        >/dev/null 2>&1 || return 1
    local i
    for i in $(seq 1 30); do
        if qa_ssh far "test -s '$MP_DIR/ready' && \
            [ \$(wc -l <'$MP_DIR/ready') -eq 3 ]" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    qa_ssh far "tail -n 50 '$MP_DIR/target.log' 2>/dev/null" \
        >"$EV/target-start.log" 2>/dev/null || true
    return 1
}

deploy_clients() {
    qa_ssh near "mkdir -p '$NEAR_DIR'" >/dev/null 2>&1 || return 1
    qa_ssh near "rm -f '$NEAR_DIR'/rc-* '$NEAR_DIR'/h-* '$NEAR_DIR'/b-* \
        '$NEAR_DIR'/done-*" >/dev/null 2>&1 || true
    cat >"$EV/clients.sh" <<'EOS'
#!/usr/bin/env bash
# clients.sh - near-side GETs through one tunnel's listen ports.
#   usage: clients.sh seq|par DIR TIMEOUT label:port...
set +e
mode=$1
dir=$2
tmo=$3
shift 3
rm -f "$dir/done-$mode"
fetch() { # label port path
    rm -f "$dir/rc-$1" "$dir/h-$1" "$dir/b-$1"
    curl -sS --max-time "$tmo" -D "$dir/h-$1" -o "$dir/b-$1" \
        "http://127.0.0.1:$2$3"
    echo $? >"$dir/rc-$1"
}
case $mode in
    seq)
        for pair in "$@"; do
            fetch "${pair%%:*}" "${pair#*:}" /
        done
        ;;
    par)
        for pair in "$@"; do
            fetch "${pair%%:*}" "${pair#*:}" '/?slow=1' &
            sleep 0.5
        done
        wait
        ;;
    *)
        exit 2
        ;;
esac
touch "$dir/done-$mode"
EOS
    qa_ssh near "cat > '$NEAR_DIR/clients.sh'" <"$EV/clients.sh" || return 1
}

run_clients() { # mode pairs...
    local mode=$1
    shift
    qa_ssh near "bash '$NEAR_DIR/clients.sh' '$mode' '$NEAR_DIR' \
        '$REQ_TIMEOUT' $*" 2>"$EV/clients-$mode.err"
}

start_refused_request() {
    qa_ssh near "rm -f '$NEAR_DIR/rc-refuse' '$NEAR_DIR/b-refuse' \
        '$NEAR_DIR/h-refuse'; nohup bash '$NEAR_DIR/clients.sh' seq \
        '$NEAR_DIR' '$REFUSE_TIMEOUT' 'refuse:$MAP2_PORT' \
        >'$NEAR_DIR/refuse.log' 2>&1 &" >/dev/null 2>&1
}

collect_result() { # label
    local l=$1
    qa_ssh near "cat '$NEAR_DIR/rc-$l' 2>/dev/null" \
        >"$EV/rc-$l.txt" 2>/dev/null || true
    qa_ssh near "cat '$NEAR_DIR/h-$l' 2>/dev/null" \
        >"$EV/hdr-$l.txt" 2>/dev/null || true
    qa_ssh near "{ stat -c %s '$NEAR_DIR/b-$l' 2>/dev/null || echo 0; \
        sha256sum '$NEAR_DIR/b-$l' 2>/dev/null | cut -d' ' -f1; }" \
        >"$EV/sum-$l.txt" 2>/dev/null || true
}

r_rc() { [ -f "$EV/rc-$1.txt" ] && tr -d '[:space:]' <"$EV/rc-$1.txt"; }
r_size() { sed -n '1p' "$EV/sum-$1.txt" 2>/dev/null | tr -d '[:space:]'; }
r_sha() { sed -n '2p' "$EV/sum-$1.txt" 2>/dev/null | tr -d '[:space:]'; }

conn_count() { # service
    qa_ssh far "if [ -f '$MP_DIR/conns-$1.log' ]; then \
        wc -l <'$MP_DIR/conns-$1.log'; else echo 0; fi" 2>/dev/null |
        tr -d '[:space:]'
}

refused_warn_count() {
    local n
    n=$(qa_ssh far "dmesg 2>/dev/null | \
        grep -c 'refused OPEN for unconfigured target port $SVC2_PORT'" \
        2>/dev/null | tr -d '[:space:]')
    if qa_num "$n"; then printf '%s' "$n"; else printf 'NA'; fi
}

# ---------------------------------------------------------------------------
# Checks on one collected response

check_response() { # label service origin-port
    local label=$1 svc=$2 port=$3 want sha
    want=$(sed -n '1p' "$EV/sha-far-$svc.txt" 2>/dev/null)
    sha=$(r_sha "$label")
    qa_check "resp_${label}_rc" \
        "$([ "$(r_rc "$label")" = 0 ]; echo $?)" \
        "curl rc=$(r_rc "$label")"
    qa_check "resp_${label}_size" \
        "$([ "$(r_size "$label")" = "$BODY_BYTES" ]; echo $?)" \
        "size=$(r_size "$label") want=$BODY_BYTES"
    qa_check "resp_${label}_sha256" \
        "$([ -n "$want" ] && [ "$sha" = "$want" ]; echo $?)" \
        "near=${sha:-?} far=${want:-?}"
    qa_check "resp_${label}_service" \
        "$(grep -qi "^X-TBT-Service: $svc" "$EV/hdr-$label.txt" 2>/dev/null; echo $?)" \
        "$svc"
    qa_check "resp_${label}_origin_port" \
        "$(grep -qi "^X-TBT-Port: $port" "$EV/hdr-$label.txt" 2>/dev/null; echo $?)" \
        ":$port"
}

check_distinct() { # labelA labelB labelC phase
    local a b c
    a=$(r_sha "$1")
    b=$(r_sha "$2")
    c=$(r_sha "$3")
    qa_check "distinct_$4" \
        "$([ -n "$a" ] && [ -n "$b" ] && [ -n "$c" ] &&
            [ "$a" != "$b" ] && [ "$a" != "$c" ] && [ "$b" != "$c" ]; echo $?)" \
        "three different bodies"
}

check_conn_delta() { # service before phase
    local now
    now=$(conn_count "$1")
    qa_check "conn_$1_$3" \
        "$(qa_num "$2" && qa_num "$now" && [ "$now" = "$(( $2 + 1 ))" ]; echo $?)" \
        "$2 -> ${now:-?}"
}

# ---------------------------------------------------------------------------
# Preflight

preflight() {
    local host ver groups p busy=0

    if [ -n "$EV_OVERRIDE" ]; then
        EV=$EV_OVERRIDE
        mkdir -p "$EV" || { say "cannot create $EV"; exit 3; }
        : >"$EV/run.log"
        : >"$EV/timeline.txt"
    else
        qa_evidence_init multiport || { say "cannot create evidence dir"; exit 3; }
    fi
    say "EVIDENCE=$EV"

    if ! qa_ssh near true >/dev/null 2>&1; then
        qa_check near_ssh 1 "$NEAR_HOST unreachable"
        exit 3
    fi
    qa_check near_ssh 0 "$NEAR_HOST"
    if ! qa_ssh far true >/dev/null 2>&1; then
        qa_check far_ssh 1 "$FAR_HOST unreachable"
        exit 3
    fi
    qa_check far_ssh 0 "$FAR_HOST"

    for host in near far; do
        qa_check "${host}_root" \
            "$([ "$(qa_ssh "$host" 'id -u' 2>/dev/null)" = 0 ]; echo $?)" root
        qa_check "${host}_proc" \
            "$(qa_ssh "$host" "test -e '$PROC_ROOT/config'; echo \$?" 2>/dev/null)" \
            "$PROC_ROOT/config"
        qa_check "${host}_module" \
            "$(qa_ssh "$host" 'lsmod | grep -q "^tbtun "' >/dev/null 2>&1; echo $?)" \
            tbtun
        ver=$(qa_ssh "$host" "cat /sys/module/tbtun/version 2>/dev/null" |
            tr -d '[:space:]')
        qa_check "${host}_version" "$([ "$ver" = "$EXPECT_VERSION" ]; echo $?)" \
            "${ver:-?} want $EXPECT_VERSION"
        groups=$(qa_ssh "$host" \
            "cat /sys/module/tbtun/parameters/fec_groups_per_flow 2>/dev/null" |
            tr -d '[:space:]')
        qa_check "${host}_groups_per_flow" \
            "$([ "$groups" = "$GROUPS_PER_FLOW" ]; echo $?)" \
            "${groups:-?} want $GROUPS_PER_FLOW"
        qa_ssh "$host" 'uname -r; cat /sys/module/tbtun/srcversion 2>/dev/null' \
            >"$EV/$host-identity.txt" 2>/dev/null || true
    done

    qa_check near_curl \
        "$(qa_ssh near 'command -v curl >/dev/null 2>&1; echo $?')" curl
    qa_check near_sha256sum \
        "$(qa_ssh near 'command -v sha256sum >/dev/null 2>&1; echo $?')" \
        sha256sum
    qa_check far_python \
        "$(qa_ssh far "command -v $PYTHON >/dev/null 2>&1; echo \$?")" "$PYTHON"

    for p in "$NEAR_PORT" "$MAP1_PORT" "$MAP2_PORT"; do
        port_free near "$p" || { busy=1; qa_check "near_port_$p" 1 busy; }
    done
    for p in "$FAR_CARRIER_PORT" "$SVC0_PORT" "$SVC1_PORT" "$SVC2_PORT"; do
        port_free far "$p" || { busy=1; qa_check "far_port_$p" 1 busy; }
    done
    [ "$busy" = 0 ] &&
        qa_check ports_free 0 "near $NEAR_PORT-$MAP2_PORT far $SVC0_PORT-$SVC2_PORT"

    if [ "$FAILED" -ne 0 ]; then
        say "preflight failed: 0.5.0, fec_groups_per_flow=$GROUPS_PER_FLOW,"
        say "a busy port, or a missing command. A leftover test tunnel is"
        say "cleared by: $PROG --cleanup. Nothing was changed."
        exit 3
    fi

    if ! qa_ssh near "cat '$PROC_ROOT/config'" >"$EV/near-config-saved.txt"; then
        qa_check near_config_saved 1 "cannot read $PROC_ROOT/config"
        exit 3
    fi
    if ! qa_ssh far "cat '$PROC_ROOT/config'" >"$EV/far-config-saved.txt"; then
        qa_check far_config_saved 1 "cannot read $PROC_ROOT/config"
        exit 3
    fi
    qa_check config_saved 0 "near/far saved before the test"

    trap cleanup EXIT
    trap 'cleanup; exit 130' INT TERM
}

# ---------------------------------------------------------------------------
# Cleanup and restore

restore_host() { # host saved-file
    [ -f "$2" ] || return 1
    printf 'flush\n' | qa_ssh "$1" "cat > '$PROC_ROOT/config'" \
        >/dev/null 2>&1 || return 1
    if [ -s "$2" ]; then
        qa_ssh "$1" "cat > '$PROC_ROOT/config'" <"$2" >/dev/null 2>&1 ||
            return 1
    fi
    return 0
}

stop_remote() {
    qa_ssh near "pkill -f '$NEAR_DIR/[c]lients.sh' 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    qa_ssh far "pkill -f '$MP_DIR/[m]ultiport-target.py' 2>/dev/null || true" \
        >/dev/null 2>&1 || true
}

cleanup() {
    [ "$CLEANED" = 0 ] || return 0
    CLEANED=1
    trap - EXIT INT TERM
    stop_remote
    if [ "$KEEP" = 1 ]; then
        say "KEEP=1: tunnel $TUNNEL and $MP_DIR left (services stopped)"
        [ -n "${EV:-}" ] && say "  later: $PROG --cleanup --out $EV"
        return 0
    fi
    local ok=0
    if [ "$RESTORE" = 1 ] && [ -n "${EV:-}" ]; then
        restore_host near "$EV/near-config-saved.txt" ||
            { say "WARN: near config restore failed"; ok=1; }
        restore_host far "$EV/far-config-saved.txt" ||
            { say "WARN: far config restore failed"; ok=1; }
    else
        say "no saved config in evidence; remote configuration untouched"
        ok=1
    fi
    qa_ssh near "rm -rf '$MP_DIR'" >/dev/null 2>&1 || true
    qa_ssh far "rm -rf '$MP_DIR'" >/dev/null 2>&1 || true
    [ "$ok" = 0 ] && say "cleanup: saved config restored"
}

latest_evidence() {
    local d
    for d in $(ls -1dt "$EVIDENCE_ROOT"/multiport-* 2>/dev/null); do
        if [ -f "$d/near-config-saved.txt" ]; then
            printf '%s' "$d"
            return 0
        fi
    done
    return 1
}

# ---------------------------------------------------------------------------
# Phase 1: one tunnel, three mappings, 256 carriers

phase_one_tunnel() {
    say "phase 1: one $CARRIERS-carrier tunnel with three mappings"
    if ! cfg_write far "$(far_line "$MAP_ALL")" 2>"$EV/cfg-far.err"; then
        qa_check config_far 1 "$(tr '\n' ' ' <"$EV/cfg-far.err" 2>/dev/null)"
        say "far refused the line; both ends must run the 0.5.0 map= parser"
        return 1
    fi
    sleep 1
    if ! cfg_write near "$(near_line "$MAP_ALL")" 2>"$EV/cfg-near.err"; then
        qa_check config_near 1 "$(tr '\n' ' ' <"$EV/cfg-near.err" 2>/dev/null)"
        return 1
    fi
    qa_check config_far 0 "target=127.0.0.1:$SVC0_PORT map=$MAP_ALL"
    qa_check config_near 0 "listen=127.0.0.1:$NEAR_PORT map=$MAP_ALL"

    qa_check near_up \
        "$(wait_up near "$CARRIERS" "$WAIT_UP_SECS"; echo $?)" \
        "up=$CARRIERS within ${WAIT_UP_SECS}s"
    qa_check far_up \
        "$(wait_up far "$CARRIERS" "$WAIT_UP_SECS"; echo $?)" \
        "up=$CARRIERS within ${WAIT_UP_SECS}s"

    qa_snap_config near "$EV/near-config-phase1.txt"
    qa_snap_config far "$EV/far-config-phase1.txt"
    qa_snap_stats near "$EV/near-stats-phase1.txt"
    qa_snap_stats far "$EV/far-stats-phase1.txt"
    qa_check near_cfg_map \
        "$(grep -qF "map=$MAP_ALL" "$EV/near-config-phase1.txt"; echo $?)" \
        "$MAP_ALL"
    qa_check far_cfg_map \
        "$(grep -qF "map=$MAP_ALL" "$EV/far-config-phase1.txt"; echo $?)" \
        "$MAP_ALL"
    qa_check near_carriers \
        "$([ "$(qa_field "$EV/near-stats-phase1.txt" c3 n "$TUNNEL")" = \
            "$CARRIERS" ]; echo $?)" "n=$CARRIERS"
    qa_check far_carriers \
        "$([ "$(qa_field "$EV/far-stats-phase1.txt" c3 n "$TUNNEL")" = \
            "$CARRIERS" ]; echo $?)" "n=$CARRIERS"
    qa_check near_up_exact \
        "$([ "$(qa_field "$EV/near-stats-phase1.txt" c3 up "$TUNNEL")" = \
            "$CARRIERS" ]; echo $?)" "$(carrier_state "$EV/near-stats-phase1.txt")"
    qa_check far_up_exact \
        "$([ "$(qa_field "$EV/far-stats-phase1.txt" c3 up "$TUNNEL")" = \
            "$CARRIERS" ]; echo $?)" "$(carrier_state "$EV/far-stats-phase1.txt")"
    return 0
}

# ---------------------------------------------------------------------------
# Phase 2: sequential requests

phase_sequential() {
    local c0 c1 c2 pair
    say "phase 2: sequential GET on :$NEAR_PORT, :$MAP1_PORT, :$MAP2_PORT"
    c0=$(conn_count svc0)
    c1=$(conn_count svc1)
    c2=$(conn_count svc2)
    # One request at a time with a drain between: the next stream then takes
    # the lowest free groups (0..7). A far session that has never bound a
    # stream keeps its primary on exactly that bitmap, and a different first
    # bitmap would be refused with -EPROTO.
    for pair in "s0:$NEAR_PORT" "s1:$MAP1_PORT" "s2:$MAP2_PORT"; do
        qa_check "clients_seq_${pair%%:*}" \
            "$(run_clients seq "$pair"; echo $?)" "$pair"
        wait_streams_idle near 15
        sleep 0.5
    done
    collect_result s0
    collect_result s1
    collect_result s2
    check_response s0 svc0 "$SVC0_PORT"
    check_response s1 svc1 "$SVC1_PORT"
    check_response s2 svc2 "$SVC2_PORT"
    check_distinct s0 s1 s2 seq
    check_conn_delta svc0 "$c0" seq
    check_conn_delta svc1 "$c1" seq
    check_conn_delta svc2 "$c2" seq
    qa_snap_stats near "$EV/near-stats-seq.txt"
    qa_snap_stats far "$EV/far-stats-seq.txt"
}

# ---------------------------------------------------------------------------
# Phase 3: concurrent RLNC multi-stream requests

phase_concurrent() {
    local job rc c0 c1 c2 max_near=0 max_far=0 n f i
    say "phase 3: concurrent GET (RLNC multi-stream) on the three ports"
    wait_streams_idle near 15
    c0=$(conn_count svc0)
    c1=$(conn_count svc1)
    c2=$(conn_count svc2)
    qa_ssh near "rm -f '$NEAR_DIR/done-par'" >/dev/null 2>&1 || true
    run_clients par "c0:$NEAR_PORT" "c1:$MAP1_PORT" "c2:$MAP2_PORT" &
    job=$!
    for i in $(seq 1 120); do
        qa_snap_stats near "$EV/stats-near-par.txt"
        qa_snap_stats far "$EV/stats-far-par.txt"
        n=$(qa_field "$EV/stats-near-par.txt" streams streams_active "$TUNNEL")
        f=$(qa_field "$EV/stats-far-par.txt" streams streams_active "$TUNNEL")
        qa_num "$n" || n=0
        qa_num "$f" || f=0
        if [ "$n" -gt "$max_near" ]; then
            max_near=$n
            cp -f "$EV/stats-near-par.txt" "$EV/near-stats-par-max.txt"
            cp -f "$EV/stats-far-par.txt" "$EV/far-stats-par-max.txt"
        fi
        [ "$f" -gt "$max_far" ] && max_far=$f
        if qa_ssh near "test -f '$NEAR_DIR/done-par'" >/dev/null 2>&1; then
            break
        fi
        sleep 0.25
    done
    wait "$job"
    rc=$?
    qa_check clients_par "$rc" "three concurrent GETs"
    qa_check near_streams_par "$([ "$max_near" -ge 3 ]; echo $?)" \
        "max streams_active=$max_near"
    qa_check far_streams_par "$([ "$max_far" -ge 3 ]; echo $?)" \
        "max streams_active=$max_far"
    collect_result c0
    collect_result c1
    collect_result c2
    check_response c0 svc0 "$SVC0_PORT"
    check_response c1 svc1 "$SVC1_PORT"
    check_response c2 svc2 "$SVC2_PORT"
    check_distinct c0 c1 c2 parallel
    check_conn_delta svc0 "$c0" parallel
    check_conn_delta svc1 "$c1" parallel
    check_conn_delta svc2 "$c2" parallel
    qa_snap_stats near "$EV/near-stats-par-after.txt"
    qa_snap_stats far "$EV/far-stats-par-after.txt"
}

# ---------------------------------------------------------------------------
# Phase 4: the far side refuses a revoked mapping

phase_security() {
    local c2_before c2_during c2_after warn_before warn_after
    local active_n active_f up_n up_f base
    local c0 c1 now
    say "phase 4: far revokes $MAP2_PORT:$SVC2_PORT; the OPEN must be dropped"

    qa_check near_drained "$(wait_streams_idle near 30; echo $?)" \
        "streams_active=0"
    qa_check far_drained "$(wait_streams_idle far 30; echo $?)" \
        "streams_active=0"

    if ! cfg_write far "$(far_line "$MAP_SAFE")" 2>"$EV/cfg-far-revoked.err"; then
        qa_check far_cfg_revoked 1 \
            "$(tr '\n' ' ' <"$EV/cfg-far-revoked.err" 2>/dev/null)"
        return
    fi
    qa_snap_config far "$EV/far-config-revoked.txt"
    if grep -qF "map=$MAP_SAFE" "$EV/far-config-revoked.txt" &&
        ! grep -qF "$MAP2_PORT:$SVC2_PORT" "$EV/far-config-revoked.txt"; then
        qa_check far_cfg_revoked 0 "map=$MAP_SAFE, no :$SVC2_PORT"
    else
        qa_check far_cfg_revoked 1 \
            "$(grep '^tunnel' "$EV/far-config-revoked.txt" 2>/dev/null)"
    fi
    qa_snap_config near "$EV/near-config-revoked.txt"
    qa_check near_cfg_keeps \
        "$(grep -qF "map=$MAP_ALL" "$EV/near-config-revoked.txt"; echo $?)" \
        "$MAP_ALL"

    qa_check far_up_revoked \
        "$(wait_up far "$CARRIERS" "$WAIT_UP_SECS"; echo $?)" \
        "up=$CARRIERS after the far restart"
    qa_check near_up_revoked \
        "$(wait_up near "$CARRIERS" "$WAIT_UP_SECS"; echo $?)" \
        "up=$CARRIERS after the far restart"
    sleep 3
    qa_snap_stats near "$EV/near-stats-refuse-base.txt"
    qa_snap_stats far "$EV/far-stats-refuse-base.txt"
    base=$EV/far-stats-refuse-base.txt
    qa_check refuse_base_up \
        "$([ "$(qa_field "$EV/near-stats-refuse-base.txt" c3 up "$TUNNEL")" = \
            "$CARRIERS" ] && \
          [ "$(qa_field "$base" c3 up "$TUNNEL")" = "$CARRIERS" ]; echo $?)" \
        "near $(carrier_state "$EV/near-stats-refuse-base.txt") far $(carrier_state "$base")"

    c2_before=$(conn_count svc2)
    warn_before=$(refused_warn_count)
    qa_check refuse_conns_base "$(qa_num "$c2_before"; echo $?)" \
        "svc2 conns=$c2_before"

    start_refused_request
    sleep "$PROBE_SECS"
    qa_snap_stats near "$EV/near-stats-refuse-probe.txt"
    qa_snap_stats far "$EV/far-stats-refuse-probe.txt"
    c2_during=$(conn_count svc2)
    active_n=$(qa_field "$EV/near-stats-refuse-probe.txt" streams \
        streams_active "$TUNNEL")
    active_f=$(qa_field "$EV/far-stats-refuse-probe.txt" streams \
        streams_active "$TUNNEL")
    up_n=$(qa_field "$EV/near-stats-refuse-probe.txt" c3 up "$TUNNEL")
    up_f=$(qa_field "$EV/far-stats-refuse-probe.txt" c3 up "$TUNNEL")
    qa_ssh far "ss -Htn 'dport = :$SVC2_PORT' 2>/dev/null" \
        >"$EV/far-ss-refuse.txt" 2>/dev/null || true

    qa_check refuse_curl_exit \
        "$(wait_remote_file near "$NEAR_DIR/rc-refuse" $((REFUSE_TIMEOUT + 20)); echo $?)" \
        "refused request finished within $((REFUSE_TIMEOUT + 20))s"
    collect_result refuse
    qa_check refuse_rc "$([ -n "$(r_rc refuse)" ] && [ "$(r_rc refuse)" != 0 ]; echo $?)" \
        "curl rc=$(r_rc refuse)"
    qa_check refuse_no_body "$([ "$(r_size refuse)" = 0 ]; echo $?)" \
        "body bytes=$(r_size refuse)"

    qa_snap_stats near "$EV/near-stats-refuse-after.txt"
    qa_snap_stats far "$EV/far-stats-refuse-after.txt"
    c2_after=$(conn_count svc2)
    warn_after=$(refused_warn_count)
    if qa_num "$warn_before" && qa_num "$warn_after"; then
        qa_check far_refused_log \
            "$([ "$warn_after" -gt "$warn_before" ]; echo $?)" \
            "dmesg warnings $warn_before -> $warn_after"
    else
        qa_skip far_refused_log "dmesg unreadable"
    fi
    qa_check not_dialled \
        "$([ "$c2_before" = "$c2_during" ] && [ "$c2_before" = "$c2_after" ]; echo $?)" \
        "svc2 conns $c2_before -> $c2_during -> $c2_after"
    qa_check far_no_stream "$([ "${active_f:-x}" = 0 ]; echo $?)" \
        "far streams_active=${active_f:-?}"
    qa_check near_stream_waits \
        "$(qa_num "$active_n" && [ "$active_n" -ge 1 ]; echo $?)" \
        "near streams_active=${active_n:-?}"
    qa_check refuse_up_near "$([ "$up_n" = "$CARRIERS" ]; echo $?)" \
        "up=${up_n:-?}/$CARRIERS"
    qa_check refuse_up_far "$([ "$up_f" = "$CARRIERS" ]; echo $?)" \
        "up=${up_f:-?}/$CARRIERS"
    qa_check far_ss_no_dial "$([ ! -s "$EV/far-ss-refuse.txt" ]; echo $?)" \
        "no established :$SVC2_PORT connection"
    qa_check refuse_near_carriers \
        "$(carrier_stable "$EV/near-stats-refuse-base.txt" \
            "$EV/near-stats-refuse-after.txt"; echo $?)" \
        "$(carrier_state "$EV/near-stats-refuse-base.txt") -> $(carrier_state "$EV/near-stats-refuse-after.txt")"
    qa_check refuse_far_carriers \
        "$(carrier_stable "$base" "$EV/far-stats-refuse-after.txt"; echo $?)" \
        "$(carrier_state "$base") -> $(carrier_state "$EV/far-stats-refuse-after.txt")"

    qa_check near_drained_after_refuse \
        "$(wait_streams_idle near 30; echo $?)" "streams_active=0"
    sleep 0.5

    c0=$(conn_count svc0)
    c1=$(conn_count svc1)
    qa_check clients_recheck_r0 \
        "$(run_clients seq "r0:$NEAR_PORT"; echo $?)" \
        "GET r0 after the refusal"
    wait_streams_idle near 15
    qa_check clients_recheck_r1 \
        "$(run_clients seq "r1:$MAP1_PORT"; echo $?)" \
        "GET r1 after the refusal"
    wait_streams_idle near 15
    collect_result r0
    collect_result r1
    check_response r0 svc0 "$SVC0_PORT"
    check_response r1 svc1 "$SVC1_PORT"
    now=$(conn_count svc0)
    qa_check conn_svc0_recheck \
        "$(qa_num "$c0" && [ "$now" = "$((c0 + 1))" ]; echo $?)" \
        "$c0 -> ${now:-?}"
    now=$(conn_count svc1)
    qa_check conn_svc1_recheck \
        "$(qa_num "$c1" && [ "$now" = "$((c1 + 1))" ]; echo $?)" \
        "$c1 -> ${now:-?}"

    qa_snap_stats near "$EV/near-stats-final.txt"
    qa_snap_stats far "$EV/far-stats-final.txt"
    qa_check final_near_carriers \
        "$(carrier_stable "$EV/near-stats-refuse-base.txt" \
            "$EV/near-stats-final.txt"; echo $?)" \
        "$(carrier_state "$EV/near-stats-final.txt")"
    qa_check final_far_carriers \
        "$(carrier_stable "$base" "$EV/far-stats-final.txt"; echo $?)" \
        "$(carrier_state "$EV/far-stats-final.txt")"
}

# ---------------------------------------------------------------------------
# Final snapshot and summary

final_checks() {
    qa_snap_config near "$EV/near-config-final.txt"
    qa_snap_config far "$EV/far-config-final.txt"
    qa_check final_near_map \
        "$(grep -qF "map=$MAP_ALL" "$EV/near-config-final.txt"; echo $?)" \
        "$MAP_ALL"
    qa_check final_far_map \
        "$(grep -qF "map=$MAP_SAFE" "$EV/far-config-final.txt"; echo $?)" \
        "$MAP_SAFE"
    {
        echo "MULTIPORT_RESULT $([ "$FAILED" -eq 0 ] && echo PASS || echo FAIL)"
        echo "date=$(qa_now)"
        echo "near=$NEAR_HOST far=$FAR_HOST far_ip=$FAR_ADDR"
        echo "tunnel=$TUNNEL carriers=$CARRIERS n=4 m=4 mode=grouped"
        echo "map=$MAP_ALL revoked=$MAP2_PORT:$SVC2_PORT"
        echo "origins=$SVC0_PORT,svc0 $SVC1_PORT,svc1 $SVC2_PORT,svc2"
        echo "bytes=$BODY_BYTES"
        echo "checks_failed=$FAILED"
        echo "--- checks ---"
        cat "$EV/run.log" 2>/dev/null
        echo "--- far connections ---"
        for svc in svc0 svc1 svc2; do
            echo "$svc $(conn_count "$svc")"
        done
        echo "--- near config ---"
        cat "$EV/near-config-final.txt" 2>/dev/null
        echo "--- far config ---"
        cat "$EV/far-config-final.txt" 2>/dev/null
        echo "--- near carriers ---"
        carrier_state "$EV/near-stats-final.txt"
        echo
        echo "--- far carriers ---"
        carrier_state "$EV/far-stats-final.txt"
        echo
    } >"$EV/summary.txt"
}

# ---------------------------------------------------------------------------
# Main

validate_settings

if [ "$MODE" = dry ]; then
    dry_run
fi

if [ "$MODE" = cleanup ]; then
    EV=${EV_OVERRIDE:-$(latest_evidence || true)}
    if [ -z "$EV" ]; then
        say "no evidence directory with a saved config found"
    else
        say "using evidence $EV"
    fi
    cleanup
    exit 0
fi

preflight

if ! start_target; then
    qa_check target_start 1 "see $EV/target-start.log"
    exit 4
fi
qa_snap_stats far "$EV/far-stats-target.txt"
qa_ssh far "cat '$MP_DIR/target.log' 2>/dev/null" \
    >"$EV/far-target.log" 2>/dev/null || true

if ! deploy_clients; then
    qa_check clients_deploy 1 "$NEAR_DIR/clients.sh"
    exit 4
fi

all_sha=0
for svc in svc0 svc1 svc2; do
    qa_ssh far "cat '$MP_DIR/sha-$svc.txt' 2>/dev/null" \
        >"$EV/sha-far-$svc.txt" 2>/dev/null || true
    grep -Eq '^[0-9a-f]{64}$' "$EV/sha-far-$svc.txt" || {
        qa_check "target_sha_$svc" 1 "missing or malformed"
        all_sha=1
    }
done
[ "${all_sha:-0}" = 0 ] && qa_check target_hashes 0 "three sha256 files"
[ "${all_sha:-0}" = 0 ] || exit 4

phase_one_tunnel || exit 4
phase_sequential
phase_concurrent
phase_security
final_checks

if [ "$FAILED" -eq 0 ]; then
    say "MULTIPORT_RESULT PASS"
    say "evidence: $EV"
    exit 0
fi
say "MULTIPORT_RESULT FAIL ($FAILED checks)"
say "evidence: $EV"
exit 6
