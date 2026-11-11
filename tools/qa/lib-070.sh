#!/usr/bin/env bash
# lib-070.sh - shared plumbing for the 0.7.0 memory and netem acceptance.
#
# Sourced. Defines defaults, near/far ssh wrappers, the 0.7.0 SC-LDPC tunnel
# renderer, the join gate, procfs samplers, netem install/clear with a
# marker-guarded watchdog, and small verdict helpers. It runs nothing at
# source time and owns no mode, verdict, or trap.
#
# The rig is the arm64 WAN pair the other tools/qa tools use:
#   near  $NEAR_HOST  public listen 127.0.0.1:$NEAR_PORT
#   far   $FAR_HOST   carrier listen 0.0.0.0:$FAR_LISTEN
#   /proc/net/tbtun/{config,tunnels,peers,stats}
#
# The tunnel is one 256-carrier SC-LDPC session: far TX is the binary 1x256
# downlink (n=128 m=128 w=256 L=8); near TX is the grouped GF(16) 8x32 uplink
# (n=8 m=24 w=128 L=16). That is the 0.7.0 shape, not a per-run choice.
#
# shellcheck shell=bash

QA070_LIB_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

NEAR_HOST=${NEAR_HOST:-root@192.168.123.85}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
NEAR_IP=${NEAR_IP:-192.168.123.85}
FAR_IP=${FAR_IP:-45.11.1.62}
NEAR_DEV=${NEAR_DEV:-eth0}
FAR_DEV=${FAR_DEV:-eth0}
NEAR_BLOCK=${NEAR_BLOCK:-30000-30511}
FAR_BLOCK=${FAR_BLOCK:-31000-31511}
NEAR_PORT=${NEAR_PORT:-9001}
FAR_LISTEN=${FAR_LISTEN:-9000}
TARGET_PORT=${TARGET_PORT:-5210}
CARRIERS=${CARRIERS:-256}
TUNNEL=${TUNNEL:-1}
NONCE=${NONCE:-00112233445566778899aabbccddeeff}
SSH_TMO=${SSH_TMO:-20}
JOIN_TIMEOUT=${JOIN_TIMEOUT:-180}
EXPECT_VERSION=${EXPECT_VERSION:-}
MIN_CARRIER_PCT=${MIN_CARRIER_PCT:-90}
MIN_ACTIVE_PCT=${MIN_ACTIVE_PCT:-98}
MIN_SS_PCT=${MIN_SS_PCT:-95}
PROC_ROOT=${PROC_ROOT:-/proc/net/tbtun}
REMOTE_BASE=${REMOTE_BASE:-/root/tbt-qa-070}
EVIDENCE_ROOT=${EVIDENCE_ROOT:-}

DRY_RUN=${DRY_RUN:-0}
FAILED=${FAILED:-0}
EV=${EV:-}

# ---------------------------------------------------------------------------
# words

q070_say() { printf '%s\n' "$*"; }

q070_die() {
    printf 'qa-070: %s\n' "$*" >&2
    exit 2
}

q070_warn() { printf 'qa-070: %s\n' "$*" >&2; }

q070_stamp() { date -u +%Y%m%dT%H%M%SZ; }

q070_now() { date -u +%Y-%m-%dT%H:%M:%SZ; }

q070_epoch() { date +%s; }

q070_token() { printf '%s-%s-%s' "$(date +%s)" "$$" "$RANDOM"; }

q070_note() {
    [ -n "$EV" ] || { q070_say "$*"; return 0; }
    printf '%s %s\n' "$(q070_now)" "$*" |
        tee -a "$EV/run.log" >&2
}

q070_check() { # name rc [detail]
    local name=$1 rc=${2:-1} detail=${3:-} line
    case $rc in '' | *[!0-9]*) rc=1 ;; esac
    if [ "$rc" -eq 0 ]; then
        line="CHECK $name PASS${detail:+ ($detail)}"
    else
        line="CHECK $name FAIL${detail:+ ($detail)}"
        FAILED=$((FAILED + 1))
    fi
    q070_say "$line"
    [ -n "$EV" ] && printf '%s\n' "$line" >>"$EV/run.log"
    return 0
}

q070_skip() { # name [detail]
    local line="CHECK $1 SKIP${2:+ ($2)}"
    q070_say "$line"
    [ -n "$EV" ] && printf '%s\n' "$line" >>"$EV/run.log"
    return 0
}

q070_verdict() { # RESULT_NAME
    if [ "$FAILED" -eq 0 ]; then
        printf '%s_RESULT PASS\n' "$1"
        return 0
    fi
    printf '%s_RESULT FAIL (%d checks)\n' "$1" "$FAILED"
    return 1
}

q070_evidence_init() { # name
    local root=${EVIDENCE_ROOT:-$(cd -- "$QA070_LIB_DIR/../.." && pwd)/}
    EV=$root/qa-070-$(q070_stamp)-$1
    mkdir -p "$EV" || return 1
    : >"$EV/run.log"
    printf 'EVIDENCE=%s\n' "$EV"
}

q070_num() { case ${1:-} in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }

# ---------------------------------------------------------------------------
# ssh

q070_ssh() { # host command...
    local host=$1
    shift
    timeout "$SSH_TMO" ssh -o BatchMode=yes -o ConnectTimeout="$SSH_TMO" \
        "$host" "$@"
}

q070_ssh_long() { # seconds host command...
    local secs=$1 host=$2
    shift 2
    timeout "$secs" ssh -o BatchMode=yes -o ConnectTimeout="$SSH_TMO" \
        "$host" "$@"
}

q070_script() { # host [arg...] ; script on stdin
    local host=$1 cmd a
    shift
    cmd="sh -s --"
    for a in "$@"; do
        cmd="$cmd $(printf '%q' "$a")"
    done
    if [ "$DRY_RUN" = 1 ]; then
        printf '+ ssh %s %s <<EOS\n' "$host" "$cmd"
        sed 's/^/  /'
        printf 'EOS\n'
        return 0
    fi
    q070_ssh "$host" "$cmd"
}

q070_run() { # host command
    local host=$1 cmd=$2
    if [ "$DRY_RUN" = 1 ]; then
        printf '+ ssh %s %s\n' "$host" "$cmd"
        return 0
    fi
    q070_ssh "$host" "$cmd"
}

q070_cat() { # host path
    if [ "$DRY_RUN" = 1 ]; then
        printf '+ ssh %s cat %s\n' "$1" "$2"
        return 0
    fi
    q070_ssh "$1" "cat '$2'" 2>/dev/null || true
}

q070_push() { # host local remote
    local host=$1 src=$2 dst=$3
    if [ "$DRY_RUN" = 1 ]; then
        printf '+ scp %s %s:%s\n' "$src" "$host" "$dst"
        return 0
    fi
    q070_ssh "$host" "mkdir -p '$(dirname "$dst")' && cat > '$dst' && chmod 755 '$dst'" <"$src"
}

q070_file_exists() { # host path
    [ "$DRY_RUN" = 1 ] && return 0
    q070_ssh "$1" "test -e '$2'" >/dev/null 2>&1
}

q070_wait_file() { # host path seconds
    local host=$1 path=$2 secs=$3 i=0
    [ "$DRY_RUN" = 1 ] && return 0
    while [ "$i" -lt "$secs" ]; do
        q070_file_exists "$host" "$path" && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}

q070_kill_pidfile() { # host pidfile
    local host=$1 pf=$2 pid
    if [ "$DRY_RUN" = 1 ]; then
        printf '+ ssh %s kill $(cat %s) 2>/dev/null || true; rm -f %s\n' \
            "$host" "$pf" "$pf"
        return 0
    fi
    pid=$(q070_cat "$host" "$pf" | tr -d '[:space:]')
    q070_num "$pid" || return 0
    q070_run "$host" "kill '$pid' 2>/dev/null || true; rm -f '$pf'"
}

# ---------------------------------------------------------------------------
# preflight

q070_tools() { # host tool...
    local host=$1
    shift
    local missing
    [ "$DRY_RUN" = 1 ] && return 0
    missing=$(q070_ssh "$host" "for t in $*; do command -v \"\$t\" >/dev/null 2>&1 || printf '%s ' \"\$t\"; done" 2>/dev/null || true)
    [ -z "$missing" ] || { q070_warn "$host is missing:$missing"; return 1; }
}

q070_require_rig() { # tools_near tools_far
    local near_tools=$1 far_tools=$2 ver
    if [ "$DRY_RUN" = 1 ]; then
        q070_say "  near $NEAR_HOST ($NEAR_IP) far $FAR_HOST ($FAR_IP)"
        return 0
    fi
    q070_ssh "$NEAR_HOST" true >/dev/null 2>&1 ||
        q070_die "near unreachable: $NEAR_HOST"
    q070_ssh "$FAR_HOST" true >/dev/null 2>&1 ||
        q070_die "far unreachable: $FAR_HOST"
    q070_ssh "$NEAR_HOST" "test -d '$PROC_ROOT'" >/dev/null 2>&1 ||
        q070_die "$NEAR_HOST: $PROC_ROOT missing; load the 0.7.0 tbtun first"
    q070_ssh "$FAR_HOST" "test -d '$PROC_ROOT'" >/dev/null 2>&1 ||
        q070_die "$FAR_HOST: $PROC_ROOT missing; load the 0.7.0 tbtun first"
    q070_tools "$NEAR_HOST" $near_tools || q070_die "near tool check failed"
    q070_tools "$FAR_HOST" $far_tools || q070_die "far tool check failed"
    if [ -n "$EXPECT_VERSION" ]; then
        for h in "$NEAR_HOST" "$FAR_HOST"; do
            ver=$(q070_ssh "$h" 'v=$(cat /sys/module/tbtun/version 2>/dev/null || true); [ -n "$v" ] || v=$(modinfo -F version tbtun 2>/dev/null || true); printf "%s" "$v"' 2>/dev/null | tr -d '[:space:]')
            [ "$ver" = "$EXPECT_VERSION" ] ||
                q070_warn "$h module version=$ver want=$EXPECT_VERSION"
        done
    fi
    q070_say "  near $NEAR_HOST ($NEAR_IP) far $FAR_HOST ($FAR_IP)"
}

# ---------------------------------------------------------------------------
# tunnel configuration and join

q070_config_line() { # role
    local role=$1
    case $role in
    near)
        printf 'tunnel %s role=near listen=127.0.0.1:%s peer=%s:%s n=8 m=24 fec=on adaptive=off mode=sc-ldpc w=128 L=16 carriers=%s crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=16777216 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=%s dev=%s nonce=%s' \
            "$TUNNEL" "$NEAR_PORT" "$FAR_IP" "$FAR_LISTEN" "$CARRIERS" \
            "$NEAR_BLOCK" "$NEAR_DEV" "$NONCE"
        ;;
    far)
        printf 'tunnel %s role=far listen=0.0.0.0:%s target=127.0.0.1:%s n=128 m=128 fec=on adaptive=off mode=sc-ldpc w=256 L=8 carriers=%s crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=16777216 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=%s nonce=%s' \
            "$TUNNEL" "$FAR_LISTEN" "$TARGET_PORT" "$CARRIERS" \
            "$FAR_BLOCK" "$NONCE"
        ;;
    *) q070_die "role must be near or far" ;;
    esac
}

q070_write_config() { # host line
    local host=$1 line=$2
    if [ "$DRY_RUN" = 1 ]; then
        printf '+ ssh %s cat > %s <<< %s\n' "$host" "$PROC_ROOT/config" "$line"
        return 0
    fi
    printf '%s\n' "$line" |
        q070_ssh "$host" "cat > '$PROC_ROOT/config'" || return 1
}

q070_configure() {
    local line host role cfg
    for role in far near; do
        case $role in
        near) host=$NEAR_HOST ;;
        far) host=$FAR_HOST ;;
        esac
        line=$(q070_config_line "$role")
        q070_write_config "$host" "$line" ||
            q070_die "$role config write failed"
        if [ "$DRY_RUN" = 1 ]; then
            q070_say "  $role config: $line"
            continue
        fi
        cfg=$(q070_cat "$host" /proc/net/tbtun/config)
        case " $cfg " in
        *" mode=sc-ldpc "*) ;;
        *) q070_die "$role read-back lacks mode=sc-ldpc: $cfg" ;;
        esac
        case " $cfg " in
        *" carriers=$CARRIERS "*) ;;
        *) q070_die "$role read-back lacks carriers=$CARRIERS: $cfg" ;;
        esac
    done
}

q070_stats_key() { # host key [class]
    local host=$1 key=$2 cls=${3:-}
    if [ "$DRY_RUN" = 1 ]; then
        printf 'NA'
        return 0
    fi
    q070_cat "$host" "$PROC_ROOT/stats" | awk -v k="$key=" -v c="$cls" -v id="$TUNNEL" '
        $1 == "tunnel" && $2 == id && (c == "" || $3 == c) {
            for (i = 1; i <= NF; i++)
                if (index($i, k) == 1) {
                    print substr($i, length(k) + 1)
                    exit
                }
        }'
}

q070_join() {
    local deadline upn upf n st
    deadline=$((SECONDS + JOIN_TIMEOUT))
    while :; do
        upn=$(q070_stats_key "$NEAR_HOST" up c3)
        upf=$(q070_stats_key "$FAR_HOST" up c3)
        n=$(q070_stats_key "$NEAR_HOST" n c3)
        if [ "${upn:-x}" = "$CARRIERS" ] && [ "${upf:-x}" = "$CARRIERS" ] &&
            [ "${n:-x}" = "$CARRIERS" ]; then
            q070_say "  joined: up=$upn/$CARRIERS"
            return 0
        fi
        if [ "$SECONDS" -ge "$deadline" ]; then
            q070_warn "join timeout: near up=${upn:-?} far up=${upf:-?} n=${n:-?}"
            return 1
        fi
        st=$(q070_cat "$NEAR_HOST" "$PROC_ROOT/tunnels" | awk -v id="$TUNNEL" '$1 == id { print $3 }')
        [ "$st" = listening ] || q070_warn "near tunnel $TUNNEL state=${st:-?}"
        sleep 2
    done
}

q070_del_tunnel() {
    local host
    for host in "$NEAR_HOST" "$FAR_HOST"; do
        q070_run "$host" "printf 'del $TUNNEL\\n' > '$PROC_ROOT/config' 2>/dev/null || true"
    done
}

# ---------------------------------------------------------------------------
# procfs sampling

q070_sample_line() { # host -> "mem=.. slab=.. ... groups=.."
    local host=$1 out
    if [ "$DRY_RUN" = 1 ]; then
        printf 'mem=NA slab=NA cached=NA k8=NA k16=NA k32=NA k64=NA k128=NA oom=NA outq=NA outq_bytes=NA drop_window=NA rx_delivered=NA submit_bytes=NA n=NA m=NA groups=NA x_slots=NA x_replay_k=NA x_credit=NA x_arena=NA\n'
        return 0
    fi
    out=$(q070_script "$host" "$TUNNEL" <<'EOS'
ID=$1
awk '
  /^MemAvailable:/ { printf "mem=%s ", $2 }
  /^Slab:/         { printf "slab=%s ", $2 }
  /^Cached:/       { printf "cached=%s\n", $2 }
' /proc/meminfo
for k in 8 16 32 64 128; do
    v=$(awk -v n="kmalloc-${k}k" '$1 == n { print $3; exit }' /proc/slabinfo)
    printf 'k%s=%s\n' "$k" "${v:-NA}"
done
awk '/^oom_kill / { print "oom=" $2; exit }' /proc/vmstat
awk -v id="$ID" '$1 == "tunnel" && $2 == id { print }' /proc/net/tbtun/stats |
awk '
  {
    cls = $3
    for (i = 1; i <= NF; i++) {
      split($i, kv, "=")
      if (kv[1] == "fec_outq_depth") oq = kv[2]
      else if (kv[1] == "fec_outq_bytes") ob = kv[2]
      else if (kv[1] == "fec_drop_window") dw = kv[2]
      else if (kv[1] == "rx_delivered") rx = kv[2]
      else if (kv[1] == "submit_bytes") sb = kv[2]
      else if (kv[1] == "slots") xs = kv[2]
      else if (kv[1] == "replay_k") xr = kv[2]
      else if (kv[1] == "credit") xc = kv[2]
      else if (kv[1] == "arena") xa = kv[2]
      else if (cls == "c13" && kv[1] == "n") cn = kv[2]
      else if (cls == "c13" && kv[1] == "m") cm = kv[2]
      else if (cls == "c13" && kv[1] == "groups") cg = kv[2]
    }
  }
  END {
    printf "outq=%s outq_bytes=%s drop_window=%s rx_delivered=%s submit_bytes=%s n=%s m=%s groups=%s x_slots=%s x_replay_k=%s x_credit=%s x_arena=%s\n",
           (oq == "" ? "NA" : oq), (ob == "" ? "NA" : ob),
           (dw == "" ? "NA" : dw), (rx == "" ? "NA" : rx),
           (sb == "" ? "NA" : sb), (cn == "" ? "NA" : cn),
           (cm == "" ? "NA" : cm), (cg == "" ? "NA" : cg),
           (xs == "" ? "NA" : xs), (xr == "" ? "NA" : xr),
           (xc == "" ? "NA" : xc), (xa == "" ? "NA" : xa)
  }'
EOS
)
    printf '%s\n' "$out" | tr '\n' ' ' | sed 's/  */ /g; s/ $//'
}

q070_kv() { # key "k=v k=v"
    if [[ $2 =~ (^|[[:space:]])$1=([^[:space:]]+) ]]; then
        printf '%s' "${BASH_REMATCH[2]}"
    else
        printf 'NA'
    fi
}

q070_ring_k() { # groups shares
    local g=$1 sh=$2 k=32
    while [ "$k" -gt 1 ]; do
        [ $((k * g * sh * (8192 + 48))) -le $((64 * 1024 * 1024)) ] && break
        k=$((k - 1))
    done
    printf '%s' "$k"
}

q070_arena_slots() { # groups shares
    local per=$(( $1 * $2 * 8192 )) slots
    [ "$per" -gt 0 ] || { printf 'NA'; return 0; }
    slots=$(( (64 * 1024 * 1024) / per ))
    [ "$slots" -lt 4 ] && slots=4
    [ "$slots" -gt 256 ] && slots=256
    printf '%s' "$slots"
}

q070_credit() { # groups n k slots
    local per_block=$(( $1 * $2 )) k=$3 slots=$4 blocks limit
    [ "$per_block" -gt 0 ] || { printf '1'; return 0; }
    [ "$slots" -gt 0 ] && [ "$k" -gt "$slots" ] && k=$slots
    [ "$k" -gt 32 ] && k=32
    if [ "$k" -gt 2 ]; then blocks=$((k - 2)); else blocks=1; fi
    limit=$(( 2048 / per_block ))
    [ "$blocks" -gt "$limit" ] && blocks=$limit
    [ "$blocks" -gt 0 ] || blocks=1
    printf '%s' $((blocks * per_block))
}

q070_peers() { # host -> peers file contents
    q070_cat "$1" "$PROC_ROOT/peers"
}

# ---------------------------------------------------------------------------
# per-carrier ss -ti: host carries 256 BBR sockets, one per carrier.

q070_ss_ti() { # host near|far block -> "local remote cwnd pacing_bps delivery_bps"
    local host=$1 side=$2 block=$3 lo hi
    lo=${block%-*}
    hi=${block#*-}
    if [ "$DRY_RUN" = 1 ]; then
        return 0
    fi
    q070_ssh "$host" "ss -Htin state established 2>/dev/null" |
        awk -v lo="$lo" -v hi="$hi" -v side="$side" '
        function rate(v, u) {
            if (u == "Kbps") return v * 1000
            if (u == "Mbps") return v * 1000000
            if (u == "Gbps") return v * 1000000000
            if (u == "Tbps") return v * 1000000000000
            return v
        }
        /^[^ \t]/ {
            lp = $4; rp = $5
            sub(/.*:/, "", lp); sub(/.*:/, "", rp)
            llp = lp + 0; rrp = rp + 0
            if ((side == "near" && llp >= lo && llp <= hi) ||
                (side == "far" && rrp >= lo && rrp <= hi)) {
                keep = 1
            } else {
                keep = 0
            }
            next
        }
        keep {
            cwnd = ""; pr = ""; dr = ""
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^cwnd:/) { cwnd = $i; sub(/^cwnd:/, "", cwnd) }
                if ($i == "pacing_rate") { pr = $(i + 1) }
                if ($i == "delivery_rate") { dr = $(i + 1) }
            }
            if (cwnd != "") {
                pv = 0; dv = 0
                if (pr ~ /^[0-9.]+[KMG]bps$/) {
                    u = pr; sub(/^[0-9.]+/, "", u)
                    pv = pr; sub(/[KMG]bps$/, "", pv)
                    pv = rate(pv + 0, u)
                }
                if (dr ~ /^[0-9.]+[KMG]bps$/) {
                    u = dr; sub(/^[0-9.]+/, "", u)
                    dv = dr; sub(/[KMG]bps$/, "", dv)
                    dv = rate(dv + 0, u)
                }
                printf "%s %s %s %s %s\n", lp, rp, cwnd, pv, dv
            }
        }'
}

q070_carrier_ports() { # near block count
    local block=$1 count=$2 lo hi
    lo=${block%-*}
    hi=${block#*-}
    [ "$DRY_RUN" = 1 ] && return 0
    q070_ssh "$NEAR_HOST" "ss -Htn state established dst $FAR_IP 2>/dev/null" |
        awk -v lo="$lo" -v hi="$hi" '
            { lp = $4; sub(/.*:/, "", lp); lp += 0
              if (lp >= lo && lp <= hi) print lp }' |
        sort -n -u | head -n "$count"
}

# ---------------------------------------------------------------------------
# netem: marker-guarded, never deletes a qdisc this harness did not install.

q070_qdisc_kind() { # host dev
    q070_ssh "$1" "tc qdisc show dev '$2'" 2>/dev/null |
        awk '{ for (i = 1; i <= NF; i++) if ($i == "qdisc") { print $(i + 1); exit } }'
}

q070_qdisc_root_ok() { # host dev
    local kind
    kind=$(q070_qdisc_kind "$1" "$2")
    case $kind in
    noqueue | pfifo_fast | fq_codel | mq) return 0 ;;
    '') return 1 ;;
    *) q070_warn "$1 dev $2 root qdisc is '$kind'; run --cleanup or restore the default first"; return 1 ;;
    esac
}

q070_netem_root_apply() { # host dev delay loss marker safety
    local host=$1 dev=$2 delay=$3 loss=$4 marker=$5 safety=$6 tok
    tok=$(q070_token)
    if [ "$DRY_RUN" = 1 ]; then
        printf '+ ssh %s tc qdisc replace dev %s root netem delay %s loss %s (watchdog %ss)\n' \
            "$host" "$dev" "$delay" "$loss" "$safety"
        return 0
    fi
    if ! q070_script "$host" "$dev" "$delay" "$loss" "$marker" "$safety" "$tok" <<'EOS'
DEV=$1; DELAY=$2; LOSS=$3; MARK=$4; SAFETY=$5; TOKEN=$6
tc qdisc replace dev "$DEV" root netem delay "$DELAY" loss "$LOSS" || exit 4
printf '%s\n' "$TOKEN" > "$MARK" || { tc qdisc del dev "$DEV" root 2>/dev/null; exit 4; }
setsid sh -c 'sleep '"$SAFETY"'; [ "$(cat '"$MARK"' 2>/dev/null)" = "'"$TOKEN"'" ] &&
    { rm -f '"$MARK"'; tc qdisc del dev '"$DEV"' root 2>/dev/null; }' \
    </dev/null >/dev/null 2>&1 &
EOS
    then
        q070_warn "netem root apply failed on $host $dev"
        return 1
    fi
}

q070_netem_root_clear() { # host dev marker
    local host=$1 dev=$2 marker=$3
    if [ "$DRY_RUN" = 1 ]; then
        printf '+ ssh %s tc qdisc del dev %s root (marker %s)\n' "$host" "$dev" "$marker"
        return 0
    fi
    q070_script "$host" "$dev" "$marker" <<'EOS' >/dev/null 2>&1 || true
DEV=$1; MARK=$2
[ -f "$MARK" ] || exit 0
rm -f "$MARK"
tc qdisc del dev "$DEV" root 2>/dev/null || true
EOS
}

q070_netem_ports_apply() { # host dev "p1 p2 ..." delay loss marker safety
    local host=$1 dev=$2 ports=$3 delay=$4 loss=$5 marker=$6 safety=$7 tok
    tok=$(q070_token)
    if [ "$DRY_RUN" = 1 ]; then
        printf '+ ssh %s tc qdisc add dev %s root handle 700: prio bands 4; netem delay %s loss %s on 700:4; u32 sport filters: %s\n' \
            "$host" "$dev" "$delay" "$loss" "$ports"
        return 0
    fi
    if ! q070_script "$host" "$dev" "$ports" "$delay" "$loss" "$marker" "$safety" "$tok" <<'EOS'
DEV=$1; PORTS=$2; DELAY=$3; LOSS=$4; MARK=$5; SAFETY=$6; TOKEN=$7
tc qdisc del dev "$DEV" root handle 700: 2>/dev/null || true
tc qdisc add dev "$DEV" root handle 700: prio bands 4 || exit 4
tc qdisc add dev "$DEV" parent 700:4 handle 740: netem delay "$DELAY" loss "$LOSS" ||
    { tc qdisc del dev "$DEV" root 2>/dev/null; exit 4; }
for p in $PORTS; do
    tc filter add dev "$DEV" parent 700: protocol ip prio 1 u32 \
        match ip sport "$p" 0xffff flowid 700:4 ||
        { tc qdisc del dev "$DEV" root 2>/dev/null; exit 4; }
done
printf '%s\n' "$TOKEN" > "$MARK" || { tc qdisc del dev "$DEV" root 2>/dev/null; exit 4; }
setsid sh -c 'sleep '"$SAFETY"'; [ "$(cat '"$MARK"' 2>/dev/null)" = "'"$TOKEN"'" ] &&
    { rm -f '"$MARK"'; tc qdisc del dev '"$DEV"' root handle 700: 2>/dev/null; }' \
    </dev/null >/dev/null 2>&1 &
EOS
    then
        q070_warn "netem port apply failed on $host $dev"
        return 1
    fi
}

q070_netem_ports_clear() { # host dev marker
    local host=$1 dev=$2 marker=$3
    if [ "$DRY_RUN" = 1 ]; then
        printf '+ ssh %s tc qdisc del dev %s root handle 700: (marker %s)\n' "$host" "$dev" "$marker"
        return 0
    fi
    q070_script "$host" "$dev" "$marker" <<'EOS' >/dev/null 2>&1 || true
DEV=$1; MARK=$2
[ -f "$MARK" ] || exit 0
rm -f "$MARK"
tc qdisc del dev "$DEV" root handle 700: 2>/dev/null || true
EOS
}

# ---------------------------------------------------------------------------
# argument parsing shared by the three scripts. The caller's loop is:
#   if q070_common_arg "$1" "${2:-}"; then shift "$SHIFT_BY"; continue; fi

q070_common_arg() {
    SHIFT_BY=0
    case $1 in
    --near) NEAR_HOST=$2; SHIFT_BY=2 ;;
    --far) FAR_HOST=$2; SHIFT_BY=2 ;;
    --near-ip) NEAR_IP=$2; SHIFT_BY=2 ;;
    --far-ip) FAR_IP=$2; SHIFT_BY=2 ;;
    --near-dev) NEAR_DEV=$2; SHIFT_BY=2 ;;
    --far-dev) FAR_DEV=$2; SHIFT_BY=2 ;;
    --near-block) NEAR_BLOCK=$2; SHIFT_BY=2 ;;
    --far-block) FAR_BLOCK=$2; SHIFT_BY=2 ;;
    --near-port) NEAR_PORT=$2; SHIFT_BY=2 ;;
    --far-listen) FAR_LISTEN=$2; SHIFT_BY=2 ;;
    --target-port) TARGET_PORT=$2; SHIFT_BY=2 ;;
    --carriers) CARRIERS=$2; SHIFT_BY=2 ;;
    --tunnel) TUNNEL=$2; SHIFT_BY=2 ;;
    --ssh-timeout) SSH_TMO=$2; SHIFT_BY=2 ;;
    --join-timeout) JOIN_TIMEOUT=$2; SHIFT_BY=2 ;;
    --expect-version) EXPECT_VERSION=$2; SHIFT_BY=2 ;;
    --evidence) EVIDENCE_ROOT=$2; SHIFT_BY=2 ;;
    *) return 1 ;;
    esac
    return 0
}

# ---------------------------------------------------------------------------
# netem acceptance engine. A case is one tunnel at a fixed RTT and loss,
# moving a fixed byte volume in one or both directions. The two scenario
# scripts set the numbers; this function owns the sequence and the verdict.
#
#   q070_netem_case NAME RTT_MS SECONDS TARGET_BPS both|dl LOSS MARK_NEAR MARK_FAR
#
# Caller contract: EV is set (q070_evidence_init), MODE is run, DRY_RUN is 0
# or 1, FAILED is 0. On FAILED != 0 the verdict is FAIL.
#
# Remote layout: $REMOTE_BASE/NAME/ holds bidir-io.py, target.pid, and
# results/server-{dl,ul}.txt. Each insertion is marker-guarded; a watchdog
# removes a netem root qdisc if this harness disappears.

q070_result_line() { # file role
    [ -f "$1" ] || return 1
    awk -v r="$2" 'index($0, "role=" r " ") == 1 { l = $0 } END { print l }' "$1"
}

q070_result_field() { # line key
    printf '%s\n' "$1" | tr ' ' '\n' |
        awk -F= -v k="$2" '$1 == k { print substr($0, index($0, "=") + 1); exit }'
}

q070_fetch_result() { # host path role seconds
    local host=$1 path=$2 role=$3 tmo=$4 i=0 line
    while [ "$i" -lt "$tmo" ]; do
        line=$(q070_cat "$host" "$path" |
            awk -v r="$role" 'index($0, "role=" r " ") == 1 { l = $0 } END { print l }')
        if [ -n "$line" ]; then
            printf '%s\n' "$line"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

q070_peer_snapshot() { # host file
    q070_peers "$1" >"$2"
}

q070_peer_deltas() { # before after elapsed
    awk -v id="$TUNNEL" -v el="$3" '
        FNR == NR { if ($1 == id) b[$2] = $14 + 0; next }
        $1 == id {
            d = ($14 + 0) - b[$2]
            printf "%d,%d,%.0f\n", $2, d, (el > 0 ? d / el : 0)
        }' "$1" "$2"
}

q070_rate_median() { # deltas-file
    awk -F, '{ print $3 }' "$1" | sort -n |
        awk '{ a[NR] = $1 + 0 }
             END {
                 if (!NR) { print "NA"; exit }
                 if (NR % 2) printf "%.0f\n", a[(NR + 1) / 2]
                 else printf "%.0f\n", (a[NR / 2] + a[NR / 2 + 1]) / 2
             }'
}

q070_netem_plan() { # name rtt seconds target dirs loss
    local name=$1 rtt=$2 secs=$3 target=$4 dirs=$5 loss=$6
    local bytes=$((target * secs)) per=$((target / CARRIERS))
    q070_say "dry-run: $name"
    q070_say "  rig: near $NEAR_HOST ($NEAR_IP) far $FAR_HOST ($FAR_IP)"
    q070_say "  shape: near TX n=8 m=24 L=16, far TX n=128 m=128 L=8, carriers=$CARRIERS"
    q070_say "  netem: both ends root netem delay $((rtt / 2))ms loss $loss (RTT ${rtt}ms)"
    q070_say "  volume: $bytes B per direction ($((bytes / 1000000)) MB), target $((target / 1000000)) MB/s"
    q070_say "           per carrier target $((per / 1000)) kB/s, directions=$dirs"
    q070_say "  sample: ss -ti cwnd/pacing_rate mid-run, peers tx_bytes before/after"
    q070_say "  pass: sha256+bytes match per direction; app >= $((target / 1000000)) MB/s;"
    q070_say "        per-carrier median >= $MIN_CARRIER_PCT% of target; active >= $MIN_ACTIVE_PCT%;"
    q070_say "        ss rows >= $MIN_SS_PCT% of $CARRIERS per end"
    q070_say "  no ssh, no writes (dry-run)"
}

q070_netem_case() { # name rtt_ms seconds target_bps both|dl loss mark_near mark_far
    local name=$1 rtt=$2 secs=$3 target=$4 dirs=$5 loss=$6 mn=$7 mf=$8
    local delay=$((rtt / 2))ms
    local bytes=$((target * secs))
    local per=$((target / CARRIERS))
    local dn=$REMOTE_BASE/$name df=$REMOTE_BASE/$name
    local tmo=$((secs * 2 + 120))
    local ss_at=$((secs / 3))
    local pids=()
    local rc_dl=0 rc_ul=0
    local pre_n=$EV/peers-near-pre.txt pre_f=$EV/peers-far-pre.txt
    local post_n=$EV/peers-near-post.txt post_f=$EV/peers-far-post.txt
    local c_line c_bytes c_sha c_secs c_bps c_trunc
    local s_line s_bytes s_sha
    local med active total ssrows ssneed wire wire_bps ratio sha_ok
    local host upn upf

    [ "$ss_at" -ge 10 ] || ss_at=10

    printf 'role,carrier,tx_bytes,rate_bps\n' >"$EV/carriers.csv"
    printf 'sample,host,local_port,remote_port,cwnd,pacing_rate_bps,delivery_rate_bps\n' \
        >"$EV/ss-ti.csv"
    printf 'role,seconds,bytes,app_bps,wire_bps,wire_per_app,sha_ok,carrier_median_bps,carriers_active,ss_rows\n' \
        >"$EV/netem-runs.csv"

    q070_note "preflight $name"
    q070_require_rig \
        "tc ss python3 sha256sum awk sed setsid sort sleep" \
        "tc ss python3 sha256sum awk sed setsid sort sleep"
    q070_qdisc_root_ok "$NEAR_HOST" "$NEAR_DEV" ||
        q070_die "$NEAR_DEV root qdisc is not restorable by this script"
    q070_qdisc_root_ok "$FAR_HOST" "$FAR_DEV" ||
        q070_die "$FAR_DEV root qdisc is not restorable by this script"

    q070_note "configure tunnel $TUNNEL"
    q070_configure
    q070_join || q070_die "carriers did not join"

    q070_run "$NEAR_HOST" "mkdir -p '$dn'"
    q070_run "$FAR_HOST" "mkdir -p '$df/results' && rm -f '$df/results/server-dl.txt' '$df/results/server-ul.txt' '$df/ready'"
    q070_push "$NEAR_HOST" "$QA070_LIB_DIR/bidir-io.py" "$dn/bidir-io.py" ||
        q070_die "cannot push bidir-io.py to near"
    q070_push "$FAR_HOST" "$QA070_LIB_DIR/bidir-io.py" "$df/bidir-io.py" ||
        q070_die "cannot push bidir-io.py to far"

    q070_note "start far target"
    if ! q070_script "$FAR_HOST" "$df" "$TARGET_PORT" <<'EOS'
D=$1; P=$2
cd "$D" || exit 3
rm -f ready
setsid nohup python3 "$D/bidir-io.py" serve --bind 127.0.0.1 --port "$P" \
    --result-dir "$D/results" --ready-file "$D/ready" \
    </dev/null >"$D/serve.log" 2>&1 &
echo $! > target.pid
EOS
    then
        q070_die "far target failed to start"
    fi
    q070_wait_file "$FAR_HOST" "$df/ready" 20 ||
        q070_die "far target did not become ready"

    q070_peer_snapshot "$NEAR_HOST" "$pre_n"
    q070_peer_snapshot "$FAR_HOST" "$pre_f"

    q070_note "netem: root delay $delay loss $loss on both ends"
    q070_netem_root_apply "$NEAR_HOST" "$NEAR_DEV" "$delay" "$loss" "$mn" "$tmo" ||
        q070_die "netem apply failed on near"
    q070_netem_root_apply "$FAR_HOST" "$FAR_DEV" "$delay" "$loss" "$mf" "$tmo" ||
        q070_die "netem apply failed on far"

    q070_note "start clients: $bytes B per direction, $((target / 1000000)) MB/s target"
    pids=()
    q070_ssh_long "$tmo" "$NEAR_HOST" \
        "timeout $tmo python3 '$dn/bidir-io.py' client --host 127.0.0.1 --port $NEAR_PORT --role dl --bytes $bytes" \
        >"$EV/near-dl.txt" 2>"$EV/near-dl.err" </dev/null &
    pids+=($!)
    if [ "$dirs" = both ]; then
        q070_ssh_long "$tmo" "$NEAR_HOST" \
            "timeout $tmo python3 '$dn/bidir-io.py' client --host 127.0.0.1 --port $NEAR_PORT --role ul --bytes $bytes" \
            >"$EV/near-ul.txt" 2>"$EV/near-ul.err" </dev/null &
        pids+=($!)
    fi

    sleep "$ss_at"
    for host in near far; do
        if [ "$host" = near ]; then
            q070_ss_ti "$NEAR_HOST" near "$NEAR_BLOCK"
        else
            q070_ss_ti "$FAR_HOST" far "$NEAR_BLOCK"
        fi | awk -v h="$host" -v s="mid" '{ print s "," h "," $0 }' >>"$EV/ss-ti.csv"
    done

    wait "${pids[0]}" || rc_dl=$?
    if [ "${#pids[@]}" -gt 1 ]; then
        wait "${pids[1]}" || rc_ul=$?
    fi

    for host in near far; do
        if [ "$host" = near ]; then
            q070_ss_ti "$NEAR_HOST" near "$NEAR_BLOCK"
        else
            q070_ss_ti "$FAR_HOST" far "$NEAR_BLOCK"
        fi | awk -v h="$host" -v s="end" '{ print s "," h "," $0 }' >>"$EV/ss-ti.csv"
    done

    q070_peer_snapshot "$NEAR_HOST" "$post_n"
    q070_peer_snapshot "$FAR_HOST" "$post_f"

    q070_note "clear netem"
    q070_netem_root_clear "$NEAR_HOST" "$NEAR_DEV" "$mn"
    q070_netem_root_clear "$FAR_HOST" "$FAR_DEV" "$mf"
    q070_kill_pidfile "$FAR_HOST" "$df/target.pid"

    q070_check "dl_client_rc" "$rc_dl" "timeout=$tmo role=dl"
    c_line=$(q070_result_line "$EV/near-dl.txt" dl)
    c_bytes=$(q070_result_field "$c_line" bytes)
    c_sha=$(q070_result_field "$c_line" sha256)
    c_secs=$(q070_result_field "$c_line" secs)
    c_bps=$(q070_result_field "$c_line" bps)
    c_trunc=$(q070_result_field "$c_line" truncated)
    s_line=$(q070_fetch_result "$FAR_HOST" "$df/results/server-dl.txt" dl 10)
    s_bytes=$(q070_result_field "$s_line" bytes)
    s_sha=$(q070_result_field "$s_line" sha256)

    q070_check dl_bytes \
        "$([ "${c_bytes:-x}" = "$bytes" ] && [ "${s_bytes:-y}" = "$bytes" ]; echo $?)" \
        "client=${c_bytes:-?} server=${s_bytes:-?} want=$bytes"
    q070_check dl_sha \
        "$([ -n "${c_sha:-}" ] && [ "$c_sha" != "-" ] && [ "${c_sha:-x}" = "${s_sha:-y}" ]; echo $?)" \
        "client=${c_sha:-?} server=${s_sha:-?}"
    q070_check dl_complete "$([ "${c_trunc:-1}" = 0 ]; echo $?)" "truncated=${c_trunc:-?}"
    q070_check dl_rate \
        "$(q070_num "$c_bps" && [ "$c_bps" -ge "$target" ]; echo $?)" \
        "app=${c_bps:-?}B/s want>=$target"

    q070_peer_deltas "$pre_f" "$post_f" "${c_secs:-1}" >"$EV/deltas-dl.csv"
    awk -v r=dl -F, '{ printf "%s,%s,%s,%s\n", r, $1, $2, $3 }' \
        "$EV/deltas-dl.csv" >>"$EV/carriers.csv"
    active=$(awk -F, '$3 > 0 { n++ } END { print n + 0 }' "$EV/deltas-dl.csv")
    total=$(awk 'END { print NR + 0 }' "$EV/deltas-dl.csv")
    med=$(q070_rate_median "$EV/deltas-dl.csv")
    q070_check dl_carrier_median \
        "$(q070_num "$med" && [ $((med * 100)) -ge $((per * MIN_CARRIER_PCT)) ]; echo $?)" \
        "median=${med}B/s want>=$((per * MIN_CARRIER_PCT / 100))"
    q070_check dl_carrier_active \
        "$([ "$total" -gt 0 ] && [ $((active * 100)) -ge $((total * MIN_ACTIVE_PCT)) ]; echo $?)" \
        "active=$active/$total want>=$MIN_ACTIVE_PCT%"
    wire=$(awk -F, '{ s += $2 } END { printf "%.0f", s }' "$EV/deltas-dl.csv")
    wire_bps=$(awk -v w="$wire" -v s="${c_secs:-1}" 'BEGIN { printf "%.0f", (s > 0 ? w / s : 0) }')
    ratio=$(awk -v w="$wire_bps" -v a="${c_bps:-1}" 'BEGIN { printf "%.3f", (a > 0 ? w / a : 0) }')
    sha_ok=$([ -n "${c_sha:-}" ] && [ "$c_sha" = "${s_sha:-}" ]; echo $?)
    printf 'dl,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${c_secs:-0}" "${c_bytes:-0}" "${c_bps:-0}" "$wire_bps" "$ratio" \
        "$sha_ok" "${med:-0}" "$active" "$total" >>"$EV/netem-runs.csv"
    q070_say "  dl: app=$(( ${c_bps:-0} / 1000 ))kB/s wire=$((wire_bps / 1000))kB/s wire/app=$ratio"

    if [ "$dirs" = both ]; then
        q070_check "ul_client_rc" "$rc_ul" "timeout=$tmo role=ul"
        c_line=$(q070_result_line "$EV/near-ul.txt" ul)
        c_bytes=$(q070_result_field "$c_line" bytes)
        c_sha=$(q070_result_field "$c_line" sha256)
        c_secs=$(q070_result_field "$c_line" secs)
        c_bps=$(q070_result_field "$c_line" bps)
        c_trunc=$(q070_result_field "$c_line" truncated)
        s_line=$(q070_fetch_result "$FAR_HOST" "$df/results/server-ul.txt" ul 10)
        s_bytes=$(q070_result_field "$s_line" bytes)
        s_sha=$(q070_result_field "$s_line" sha256)

        q070_check ul_bytes \
            "$([ "${c_bytes:-x}" = "$bytes" ] && [ "${s_bytes:-y}" = "$bytes" ]; echo $?)" \
            "client=${c_bytes:-?} server=${s_bytes:-?} want=$bytes"
        q070_check ul_sha \
            "$([ -n "${c_sha:-}" ] && [ "$c_sha" != "-" ] && [ "${c_sha:-x}" = "${s_sha:-y}" ]; echo $?)" \
            "client=${c_sha:-?} server=${s_sha:-?}"
        q070_check ul_complete "$([ "${c_trunc:-1}" = 0 ]; echo $?)" "truncated=${c_trunc:-?}"
        q070_check ul_rate \
            "$(q070_num "$c_bps" && [ "$c_bps" -ge "$target" ]; echo $?)" \
            "app=${c_bps:-?}B/s want>=$target"

        q070_peer_deltas "$pre_n" "$post_n" "${c_secs:-1}" >"$EV/deltas-ul.csv"
        awk -v r=ul -F, '{ printf "%s,%s,%s,%s\n", r, $1, $2, $3 }' \
            "$EV/deltas-ul.csv" >>"$EV/carriers.csv"
        active=$(awk -F, '$3 > 0 { n++ } END { print n + 0 }' "$EV/deltas-ul.csv")
        total=$(awk 'END { print NR + 0 }' "$EV/deltas-ul.csv")
        med=$(q070_rate_median "$EV/deltas-ul.csv")
        q070_check ul_carrier_median \
            "$(q070_num "$med" && [ $((med * 100)) -ge $((per * MIN_CARRIER_PCT)) ]; echo $?)" \
            "median=${med}B/s want>=$((per * MIN_CARRIER_PCT / 100))"
        q070_check ul_carrier_active \
            "$([ "$total" -gt 0 ] && [ $((active * 100)) -ge $((total * MIN_ACTIVE_PCT)) ]; echo $?)" \
            "active=$active/$total want>=$MIN_ACTIVE_PCT%"
        wire=$(awk -F, '{ s += $2 } END { printf "%.0f", s }' "$EV/deltas-ul.csv")
        wire_bps=$(awk -v w="$wire" -v s="${c_secs:-1}" 'BEGIN { printf "%.0f", (s > 0 ? w / s : 0) }')
        ratio=$(awk -v w="$wire_bps" -v a="${c_bps:-1}" 'BEGIN { printf "%.3f", (a > 0 ? w / a : 0) }')
        sha_ok=$([ -n "${c_sha:-}" ] && [ "$c_sha" = "${s_sha:-}" ]; echo $?)
        printf 'ul,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "${c_secs:-0}" "${c_bytes:-0}" "${c_bps:-0}" "$wire_bps" "$ratio" \
            "$sha_ok" "${med:-0}" "$active" "$total" >>"$EV/netem-runs.csv"
        q070_say "  ul: app=$(( ${c_bps:-0} / 1000 ))kB/s wire=$((wire_bps / 1000))kB/s wire/app=$ratio"
    fi

    ssneed=$((CARRIERS * MIN_SS_PCT / 100))
    for host in near far; do
        if [ "$host" = far ] || [ "$dirs" = both ]; then
            ssrows=$(awk -F, -v h="$host" \
                '$1 == "mid" && $2 == h && $5 + 0 > 0 && $6 + 0 > 0 { n++ }
                 END { print n + 0 }' "$EV/ss-ti.csv")
        else
            ssrows=$(awk -F, -v h="$host" \
                '$1 == "mid" && $2 == h && $5 + 0 > 0 { n++ }
                 END { print n + 0 }' "$EV/ss-ti.csv")
        fi
        q070_check "ss_$host" "$([ "$ssrows" -ge "$ssneed" ]; echo $?)" \
            "mid rows=$ssrows want>=$ssneed"
    done

    upn=$(q070_stats_key "$NEAR_HOST" up c3)
    upf=$(q070_stats_key "$FAR_HOST" up c3)
    q070_check carriers_near "$([ "${upn:-0}" = "$CARRIERS" ]; echo $?)" "up=$upn want=$CARRIERS"
    q070_check carriers_far "$([ "${upf:-0}" = "$CARRIERS" ]; echo $?)" "up=$upf want=$CARRIERS"

    {
        printf 'case=%s rtt=%sms loss=%s seconds=%s target_bps=%s dirs=%s\n' \
            "$name" "$rtt" "$loss" "$secs" "$target" "$dirs"
        cat "$EV/netem-runs.csv"
    } >"$EV/netem-verdict.txt"
    q070_verdict NETEM | tee -a "$EV/netem-verdict.txt"
}

q070_netem_abort() { # name mark_near mark_far
    local name=$1 mn=$2 mf=$3 dn=$REMOTE_BASE/$1 df=$REMOTE_BASE/$1
    q070_netem_root_clear "$NEAR_HOST" "$NEAR_DEV" "$mn"
    q070_netem_root_clear "$FAR_HOST" "$FAR_DEV" "$mf"
    q070_run "$NEAR_HOST" "pkill -f '$dn/bidir-io.py client' 2>/dev/null || true"
    q070_kill_pidfile "$FAR_HOST" "$df/target.pid"
}

q070_netem_cleanup() { # name mark_near mark_far
    q070_netem_abort "$1" "$2" "$3"
    q070_del_tunnel
}
