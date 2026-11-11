#!/usr/bin/env bash
# alloc-rebalance-accept.sh - real-hardware acceptance for the 0.4.4 dynamic
# FEC group rebalance (FEC_ALLOC 0x36) on the v7 multi-stream data plane.
#
# One run proves the near-side allocator's 1 -> 2 -> 3 -> 4 -> 1 dance:
#   phase 1  stream A starts alone and owns all 32 physical groups;
#   phase 2  B, C and D join while A keeps streaming; each join shrinks the
#            donors on a super-block boundary (16+16, then 11+11+10, then
#            8+8+8+8), every flow stays live and the masks stay disjoint;
#   phase 3  B, C and D close one at a time and A grows back (8+8+8+8 ->
#            11+11+10 -> 16+16 -> 32) while all four bodies are verified.
# Every stream is checked both ways: the near client uploads a random body
# that the far target hashes, and the far target's downlink body is hashed
# by the near client; size + sha256 + rc must agree in both directions. The
# run also requires zero hole/reset/rebalance-failure deltas.
#
# Preconditions
#   * both hosts run the 0.4.4 build whose /proc/net/tbtun/stats stream lines
#     carry fec_groups= and fec_mask= (32 bytes, 64 hex chars);
#   * both ends loaded tbtun with fec_groups_per_flow=0. The tool verifies
#     that parameter; it only reloads the module with --reload:
#       modprobe -r tbtun && modprobe tbtun fec_groups_per_flow=0
#   * near and far can ssh each other's QA account with BatchMode=yes;
#   * the tunnel pair can be rewritten: the run saves both config files and
#     restores them on exit.
#
# Usage: alloc-rebalance-accept.sh [options]
#   --dry-run          print the plan/config/commands; touch no host
#   --cleanup          stop the remote target/clients and restore --setup
#                      config saved in --evidence DIR
#   --no-setup         use the existing tunnel 1 config (still verified)
#   --reload           modprobe -r tbtun and modprobe tbtun
#                      fec_groups_per_flow=0 on both ends (disruptive)
#   --bytes-b N        B/C/D body size (default 67108864)
#   --bytes-c N
#   --bytes-d N
#   --up-bytes N       per-stream request body (near TX, default 4194304)
#   --pace-a BPS       stream A pacing (default 2097152 bytes/s)
#   --timeout S        overall client/body timeout (default 1800)
#   --evidence DIR     reuse an evidence directory
#   --keep             keep the remote work directory
#   -h, --help         this text
#
# Exit: 0 PASS, 2 usage, 3 preflight, 4 target, 5 traffic, 6 checks.
set -uo pipefail

. "$(dirname -- "$0")/lib.sh"

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
PROG=${0##*/}
MODE=run
DRY=0
SETUP=1
KEEP=0
RELOAD=0
EV=${EV:-}
AL_TIMEOUT=${AL_TIMEOUT:-1800}
AL_STAGE_TIMEOUT=${AL_STAGE_TIMEOUT:-120}
AL_BYTES_B=${AL_BYTES_B:-67108864}
AL_BYTES_C=${AL_BYTES_C:-67108864}
AL_BYTES_D=${AL_BYTES_D:-67108864}
AL_UP_BYTES=${AL_UP_BYTES:-4194304}
AL_PACE_A=${AL_PACE_A:-2097152}
AL_LPORT_A=${AL_LPORT_A:-41101}
AL_LPORT_B=${AL_LPORT_B:-41102}
AL_LPORT_C=${AL_LPORT_C:-41103}
AL_LPORT_D=${AL_LPORT_D:-41104}
AL_DIR=${AL_DIR:-/root/tbtun-alloc}
AL_NEAR_CFG=${AL_NEAR_CFG:-}
AL_FAR_CFG=${AL_FAR_CFG:-}
FAILED=0
CLEANED=0
RUN_EXIT=6
A_SID=
B_SID=
C_SID=
D_SID=

usage() {
    sed -n '2,44p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-2}"
}

while (($#)); do
    case $1 in
        --dry-run) DRY=1 ;;
        --cleanup) MODE=cleanup ;;
        --no-setup) SETUP=0 ;;
        --reload) RELOAD=1 ;;
        --keep) KEEP=1 ;;
        --evidence) shift; EV=${1:-} ;;
        --bytes-b) shift; AL_BYTES_B=${1:-} ;;
        --bytes-c) shift; AL_BYTES_C=${1:-} ;;
        --bytes-d) shift; AL_BYTES_D=${1:-} ;;
        --up-bytes) shift; AL_UP_BYTES=${1:-} ;;
        --pace-a) shift; AL_PACE_A=${1:-} ;;
        --timeout) shift; AL_TIMEOUT=${1:-} ;;
        -h|--help) usage 0 ;;
        *) printf '%s: unknown option: %s\n' "$PROG" "$1" >&2; usage ;;
    esac
    shift
done

for n in "$AL_BYTES_B" "$AL_BYTES_C" "$AL_BYTES_D" "$AL_UP_BYTES" \
         "$AL_TIMEOUT" \
         "$AL_STAGE_TIMEOUT" "$AL_PACE_A"; do
    qa_num "$n" || { printf 'bad numeric option: %s\n' "$n" >&2; exit 2; }
done
(( AL_BYTES_B > 0 && AL_BYTES_C > 0 && AL_BYTES_D > 0 &&
   AL_UP_BYTES > 0 && AL_TIMEOUT > 0 )) ||
    { printf 'body/timeout sizes must be positive\n' >&2; exit 2; }

field() { qa_field "$1" "$2" "$3" "${4:-$TUNNEL}"; }

cfg_has() { grep -qw -- "$2" "$1" 2>/dev/null; }

param_of() {
    qa_ssh "$1" "cat /sys/module/tbtun/parameters/fec_groups_per_flow 2>/dev/null" |
        tr -d '[:space:]'
}

snap() { # near|far file
    qa_snap_stats "$1" "$2"
}

active_streams() {
    awk -v t="$TUNNEL" '$1=="tunnel" && $2==t && $3=="streams" {
        for (i=1;i<=NF;i++) if ($i ~ /^streams_active=/) {
            split($i,a,"="); print a[2]; exit } }' "$1" 2>/dev/null
}

# One line per live flow: "sid groups mask". The mask is the stream row's
# fec_mask (32 bytes as 64 hex chars), copied from the session flow table by
# tbt_procfs.c. Only state=1 (OPEN) rows count: GAP/REORDER/HOLE/DEAD is a
# failed stream and must never be hidden by the census.
rows_in_file() {
    awk -v t="$TUNNEL" '
        $1=="tunnel" && $2==t && $3=="stream" {
            sid=""; g=""; m=""; st="";
            for (i=4;i<=NF;i++) {
                p=index($i,"="); if (!p) continue;
                k=substr($i,1,p-1); v=substr($i,p+1);
                if (k=="sid") sid=v;
                else if (k=="state") st=v;
                else if (k=="fec_groups") g=v;
                else if (k=="fec_mask") m=v;
            }
            if (g != "" && g+0 > 0 && st != "" && st+0 == 1)
                print sid, g, m;
        }' "$1" 2>/dev/null
}

row_count() { rows_in_file "$1" | awk 'NF { n++ } END { print n+0 }'; }
row_sum() { rows_in_file "$1" | awk 'NF { s += $2 } END { print s+0 }'; }
row_masks() { rows_in_file "$1" | awk 'NF { print $3 }'; }
row_sids() { rows_in_file "$1" | awk 'NF { print $1 }' | sort; }
row_groups_sorted() {
    rows_in_file "$1" | awk 'NF { print $2 }' | sort -n | tr '\n' ' '
}

rows_masks_ok() {
    row_masks "$1" | awk 'length($0) != 64 { bad=1 } END { exit bad ? 1 : 0 }'
}

mask_overlap() { # hex-a hex-b -> true when any bit is shared
    local i a b
    [[ ${#1} -eq 64 && ${#2} -eq 64 ]] || return 1
    for i in 0 8 16 24 32 40 48 56; do
        a=${1:$i:8}
        b=${2:$i:8}
        (( (16#$a & 16#$b) != 0 )) && return 0
    done
    return 1
}

rows_overlap() {
    local masks=() m i j
    while IFS= read -r m; do
        [[ -n $m ]] && masks+=("$m")
    done < <(row_masks "$1")
    for ((i = 0; i < ${#masks[@]}; i++)); do
        for ((j = i + 1; j < ${#masks[@]}; j++)); do
            mask_overlap "${masks[i]}" "${masks[j]}" && return 0
        done
    done
    return 1
}

desc_file() {
    printf 'active=%s groups="%s" sum=%s overlap=%s' \
        "$(active_streams "$1")" "$(row_groups_sorted "$1")" \
        "$(row_sum "$1")" "$(rows_overlap "$1" && echo yes || echo no)"
}

stage_ok() { # file want expected-sorted-groups
    local f=$1 want=$2 expect=$3
    [[ $(active_streams "$f") == "$want" ]] || return 1
    ((want == 0)) && return 0
    [[ $(row_count "$f") == "$want" ]] || return 1
    [[ $(row_sum "$f") == 32 ]] || return 1
    rows_masks_ok "$f" || return 1
    rows_overlap "$f" && return 1
    [[ $(row_groups_sorted "$f") == "$expect" ]] || return 1
    return 0
}

wait_stage() { # name want expected seconds
    local name=$1 want=$2 expect=$3 secs=$4 i
    for i in $(seq 1 "$secs"); do
        snap near "$EV/near-$name-stats.txt"
        snap far "$EV/far-$name-stats.txt"
        if stage_ok "$EV/near-$name-stats.txt" "$want" "$expect" &&
            stage_ok "$EV/far-$name-stats.txt" "$want" "$expect"; then
            return 0
        fi
        sleep 1
    done
    return 1
}

stage_check() { # name want expected
    local name=$1 want=$2 expect=$3
    if wait_stage "$name" "$want" "$expect" "$AL_STAGE_TIMEOUT"; then
        qa_check "$name" 0 \
            "near[$(desc_file "$EV/near-$name-stats.txt")] far[$(desc_file "$EV/far-$name-stats.txt")]"
        return 0
    fi
    qa_check "$name" 1 \
        "near[$(desc_file "$EV/near-$name-stats.txt")] far[$(desc_file "$EV/far-$name-stats.txt")]"
    return 1
}

sid_of_new() { # before-stats after-stats -> the one sid that appeared
    comm -13 <(row_sids "$1") <(row_sids "$2") | head -n 1
}

sid_present() { # file sid
    row_sids "$1" | grep -qx -- "$2"
}

check_sid() { # name file sid want(0|1)
    if [[ -z $3 ]]; then
        qa_check "$1" 1 "sid is empty"
        return
    fi
    if [[ $4 == 1 ]]; then
        qa_check "$1" "$(sid_present "$2" "$3"; echo $?)" "sid=$3"
    else
        qa_check "$1" "$(! sid_present "$2" "$3"; echo $?)" "sid=$3 absent"
    fi
}

delta_check() { # name pre post class field
    local d
    d=$(qa_delta "$(field "$2" "$4" "$5")" "$(field "$3" "$4" "$5")")
    qa_check "$1" "$([[ ${d:-?} == 0 ]]; echo $?)" \
        "$4 $5 delta=${d:-?}"
}

delta_ge() { # name pre post class field min
    local d
    d=$(qa_delta "$(field "$2" "$4" "$5")" "$(field "$3" "$4" "$5")")
    qa_check "$1" "$(qa_num "${d:-}" && (( d >= $6 )); echo $?)" \
        "$4 $5 delta=${d:-?} min=$6"
}

check_zero_health() { # tag pre-stage post-stage
    local tag=$1 pre=$2 post=$3 host
    for host in near far; do
        delta_check "${tag}_${host}_holes_declared" \
            "$EV/$host-$pre-stats.txt" "$EV/$host-$post-stats.txt" \
            c6 holes_declared
        delta_check "${tag}_${host}_holes_unrecoverable" \
            "$EV/$host-$pre-stats.txt" "$EV/$host-$post-stats.txt" \
            c6 holes_unrecoverable
        delta_check "${tag}_${host}_reset_tcp_failed" \
            "$EV/$host-$pre-stats.txt" "$EV/$host-$post-stats.txt" \
            c11 fec_reset_tcp_failed
        delta_check "${tag}_${host}_reset_hole" \
            "$EV/$host-$pre-stats.txt" "$EV/$host-$post-stats.txt" \
            c11 fec_reset_hole
        delta_check "${tag}_${host}_drop_sid" \
            "$EV/$host-$pre-stats.txt" "$EV/$host-$post-stats.txt" \
            c9 fec_drop_sid
        delta_check "${tag}_${host}_stream_refused" \
            "$EV/$host-$pre-stats.txt" "$EV/$host-$post-stats.txt" \
            c9 fec_stream_refused
        delta_check "${tag}_${host}_deliver_bad" \
            "$EV/$host-$pre-stats.txt" "$EV/$host-$post-stats.txt" \
            c10 fec_deliver_bad
        delta_check "${tag}_${host}_fin_bad" \
            "$EV/$host-$pre-stats.txt" "$EV/$host-$post-stats.txt" \
            c10 fec_fin_bad
        delta_check "${tag}_${host}_rebalance_bad" \
            "$EV/$host-$pre-stats.txt" "$EV/$host-$post-stats.txt" \
            rebalance bad
        delta_check "${tag}_${host}_rebalance_overlap" \
            "$EV/$host-$pre-stats.txt" "$EV/$host-$post-stats.txt" \
            rebalance overlap
    done
}

cleanup() {
    ((CLEANED)) && return
    CLEANED=1
    qa_ssh near "pkill -f '$AL_DIR/[r]un-one.sh' 2>/dev/null || true; \
        pkill -f '[c]url.*$AL_DIR/out-' 2>/dev/null || true" >/dev/null 2>&1 || true
    qa_ssh far "pkill -f '$AL_DIR/[a]lloc-rebalance-target.py' 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    sleep 1
    if ((SETUP)); then
        if [[ -n $EV && -s $EV/config-near-before.txt ]]; then
            qa_ssh near "cat '$EV/config-near-before.txt' > '$PROC_ROOT/config'" \
                >/dev/null 2>&1 || true
        fi
        if [[ -n $EV && -s $EV/config-far-before.txt ]]; then
            qa_ssh far "cat '$EV/config-far-before.txt' > '$PROC_ROOT/config'" \
                >/dev/null 2>&1 || true
        fi
    fi
    if ((!KEEP)); then
        qa_ssh near "rm -rf '$AL_DIR'" >/dev/null 2>&1 || true
        qa_ssh far "rm -rf '$AL_DIR'" >/dev/null 2>&1 || true
    fi
}

dry_run() {
    qa_say "DRY-RUN alloc-rebalance-accept (no host is touched)"
    qa_say "  near=$NEAR_HOST far=$FAR_HOST tunnel=$TUNNEL"
    qa_say "  target: python3 $HERE/alloc-rebalance-target.py --port $FAR_PORT"
    qa_say "          --outdir $AL_DIR --bytes-b $AL_BYTES_B --bytes-c $AL_BYTES_C --bytes-d $AL_BYTES_D"
    qa_say "          --pace-a $AL_PACE_A --hold-timeout $AL_TIMEOUT"
    qa_say "  upload: $AL_UP_BYTES bytes per stream (near TX, hashed by the target)"
    qa_say "  module: both ends need fec_groups_per_flow=0"
    qa_say "          modprobe -r tbtun && modprobe tbtun fec_groups_per_flow=0"
    qa_say "  config: mode=grouped n=4 m=4 carriers=256 (32 groups), setup=$SETUP"
    qa_say "  phase 1: A alone -> groups=32"
    qa_say "  phase 2: A+B -> 16+16; +C -> 10+11+11; +D -> 8+8+8+8"
    qa_say "  phase 3: release B -> 10+11+11; release C -> 16+16;"
    qa_say "           release D -> 32; release A -> 0"
    qa_say "  checks: 4x up/down rc+sha256+bytes, disjoint masks,"
    qa_say "          zero hole/reset/bad"
    exit 0
}

target_ok() {
    qa_ssh far "command -v python3 >/dev/null 2>&1" >/dev/null 2>&1
}

start_target() {
    qa_ssh far "mkdir -p '$AL_DIR'" >/dev/null 2>&1 || return 1
    scp -q "$HERE/alloc-rebalance-target.py" "$FAR_HOST:$AL_DIR/" || return 1
    qa_ssh far "pkill -f '$AL_DIR/[a]lloc-rebalance-target.py' 2>/dev/null || true; \
        rm -f '$AL_DIR'/release-* '$AL_DIR'/sent-* '$AL_DIR'/sha-*.txt \
              '$AL_DIR'/bytes-*.txt '$AL_DIR/target.log'" >/dev/null 2>&1 || true
    qa_ssh far "nohup python3 '$AL_DIR/alloc-rebalance-target.py' \
        --host 127.0.0.1 --port '$FAR_PORT' --outdir '$AL_DIR' \
        --bytes-b '$AL_BYTES_B' --bytes-c '$AL_BYTES_C' --bytes-d '$AL_BYTES_D' \
        --pace-a '$AL_PACE_A' --hold-timeout '$AL_TIMEOUT' \
        >'$AL_DIR/target.log' 2>&1 & echo \$! >'$AL_DIR/target.pid'" \
        >/dev/null 2>&1 || return 1
    local i
    for i in $(seq 1 20); do
        if qa_ssh far "grep -q 'LISTEN' '$AL_DIR/target.log' 2>/dev/null" \
            >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    qa_ssh far "cat '$AL_DIR/target.log' 2>/dev/null" >"$EV/target-start.log" 2>/dev/null || true
    return 1
}

client_script() {
    qa_ssh near "mkdir -p '$AL_DIR'" >/dev/null 2>&1 || return 1
    qa_ssh near "cat > '$AL_DIR/run-one.sh'" >/dev/null 2>&1 <<EOS
#!/usr/bin/env bash
set +e
DIR='$AL_DIR'
URL='http://127.0.0.1:$NEAR_PORT'
TMO='$AL_TIMEOUT'
label=\$1
port=\$2
rm -f "\$DIR/rc-\$label"
curl -sS --max-time "\$TMO" --local-port "\$port" \
    -H 'Expect:' --data-binary "@\$DIR/up-\$label.bin" \
    -o "\$DIR/out-\$label.bin" "\$URL/stream/\$label" \
    >"\$DIR/curl-\$label.log" 2>&1
echo \$? >"\$DIR/rc-\$label"
EOS
}

gen_uploads() {
    qa_ssh near "mkdir -p '$AL_DIR'; for l in a b c d; do \
        dd if=/dev/urandom of='$AL_DIR/up-'\$l'.bin' \
            bs='$AL_UP_BYTES' count=1 status=none || exit 1; done" \
        >/dev/null 2>&1
}

start_client() { # label port
    qa_ssh near "nohup bash '$AL_DIR/run-one.sh' '$1' '$2' \
        >/dev/null 2>&1 & echo \$! >'$AL_DIR/client-$1.pid'" \
        >/dev/null 2>&1
}

wait_client() { # label
    local i
    for i in $(seq 1 "$AL_TIMEOUT"); do
        if qa_ssh near "test -s '$AL_DIR/rc-$1'" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

client_rc() {
    qa_ssh near "cat '$AL_DIR/rc-$1' 2>/dev/null" | tr -d '[:space:]'
}

release() {
    qa_ssh far "touch '$AL_DIR/release-$1'" >/dev/null 2>&1
}

wait_sent() { # label
    local i
    for i in $(seq 1 "$AL_STAGE_TIMEOUT"); do
        qa_ssh far "test -s '$AL_DIR/sent-$1'" >/dev/null 2>&1 && return 0
        sleep 1
    done
    return 1
}

preflight() {
    if [[ -z $EV ]]; then
        qa_evidence_init alloc >/dev/null || {
            printf 'cannot create evidence dir\n' >&2
            exit 3
        }
    else
        mkdir -p "$EV" || {
            printf 'cannot create evidence dir %s\n' "$EV" >&2
            exit 3
        }
    fi
    qa_say "EVIDENCE=$EV"
    [[ -f $HERE/alloc-rebalance-target.py ]] ||
        qa_check target_helper 1 "$HERE/alloc-rebalance-target.py missing"

    for host in near far; do
        qa_check "${host}_reachable" \
            "$(qa_ssh "$host" true >/dev/null 2>&1; echo $?)" "$host"
        qa_check "${host}_module" \
            "$(qa_ssh "$host" 'lsmod | grep -q "^tbtun "' >/dev/null 2>&1; echo $?)" \
            tbtun
        qa_snap_config "$host" "$EV/config-$host-before.txt" 2>/dev/null || true
    done

    if ((RELOAD)); then
        for host in near far; do
            qa_ssh "$host" "modprobe -r tbtun 2>/dev/null; \
                modprobe tbtun fec_groups_per_flow=0" >/dev/null 2>&1 ||
                qa_check "${host}_reload" 1 "modprobe tbtun fec_groups_per_flow=0"
        done
    fi
    local p_near p_far
    p_near=$(param_of near)
    p_far=$(param_of far)
    qa_check near_param0 "$([[ $p_near == 0 ]]; echo $?)" \
        "near=$p_near want=0 (see --reload)"
    qa_check far_param0 "$([[ $p_far == 0 ]]; echo $?)" \
        "far=$p_far want=0 (see --reload)"
    qa_check far_python "$(target_ok; echo $?)" python3

    if ((SETUP && FAILED == 0)); then
        if [[ -z $AL_NEAR_CFG ]]; then
            AL_NEAR_CFG="tunnel $TUNNEL role=near listen=127.0.0.1:$NEAR_PORT peer=$FAR_IP:9000 n=4 m=4 fec=on mode=grouped w=8 carriers=256 crypto=off dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 portblock=30000-30511 nonce=00112233445566778899aabbccddeeff adaptive=off"
        fi
        if [[ -z $AL_FAR_CFG ]]; then
            AL_FAR_CFG="tunnel $TUNNEL role=far listen=0.0.0.0:9000 target=127.0.0.1:$FAR_PORT n=4 m=4 fec=on mode=grouped w=8 carriers=256 crypto=off dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 portblock=31000-31511 nonce=00112233445566778899aabbccddeeff adaptive=off"
        fi
        qa_ssh near "cat > '$PROC_ROOT/config'" <<<"$AL_NEAR_CFG" || true
        qa_ssh far "cat > '$PROC_ROOT/config'" <<<"$AL_FAR_CFG" || true
        local i
        for i in $(seq 1 20); do
            qa_snap_stats near "$EV/near-setup-stats.txt"
            qa_snap_stats far "$EV/far-setup-stats.txt"
            if [[ $(field "$EV/near-setup-stats.txt" c3 up) == 256 &&
                  $(field "$EV/far-setup-stats.txt" c3 up) == 256 ]]; then
                break
            fi
            sleep 1
        done
    fi

    qa_snap_config near "$EV/near-config.txt"
    qa_snap_config far "$EV/far-config.txt"
    for host in near far; do
        qa_check "${host}_cfg_fec" \
            "$(cfg_has "$EV/$host-config.txt" 'fec=on'; echo $?)" fec=on
        qa_check "${host}_cfg_mode" \
            "$(cfg_has "$EV/$host-config.txt" 'mode=grouped'; echo $?)" mode=grouped
        qa_check "${host}_cfg_shape" \
            "$(cfg_has "$EV/$host-config.txt" 'n=4' &&
              cfg_has "$EV/$host-config.txt" 'm=4'; echo $?)" n=4,m=4
        qa_check "${host}_cfg_carriers" \
            "$(cfg_has "$EV/$host-config.txt" 'carriers=256'; echo $?)" carriers=256
    done

    sleep 2
    snap near "$EV/near-base-stats.txt"
    snap far "$EV/far-base-stats.txt"
    for host in near far; do
        qa_check "${host}_up256" \
            "$([[ $(field "$EV/$host-base-stats.txt" c3 up) == 256 ]]; echo $?)" \
            "up=$(field "$EV/$host-base-stats.txt" c3 up) want=256"
        qa_check "${host}_idle" \
            "$([[ $(active_streams "$EV/$host-base-stats.txt") == 0 ]]; echo $?)" \
            "streams_active=0"
        # data_init publishes the primary flow before any stream: with
        # fec_groups_per_flow=0 it owns every group (used == total).
        qa_check "${host}_idle_groups" \
            "$([[ $(field "$EV/$host-base-stats.txt" rebalance used) == \
                  $(field "$EV/$host-base-stats.txt" rebalance total) ]]; echo $?)" \
            "used=$(field "$EV/$host-base-stats.txt" rebalance used)/$(field "$EV/$host-base-stats.txt" rebalance total) (primary flow)"
        qa_check "${host}_has_alloc_fields" \
            "$(awk -v t="$TUNNEL" '$1=="tunnel" && $2==t && $3=="rebalance" {
                for (i=1;i<=NF;i++) if ($i ~ /^allocator=/) f=1;
                } END { exit f ? 0 : 1 }' "$EV/$host-base-stats.txt"; echo $?)" \
            "rebalance line"
        qa_check "${host}_groups_total32" \
            "$([[ $(field "$EV/$host-base-stats.txt" rebalance total) == 32 ]]; echo $?)" \
            "total=$(field "$EV/$host-base-stats.txt" rebalance total)"
    done
    qa_check near_is_allocator \
        "$([[ $(field "$EV/near-base-stats.txt" rebalance allocator) == 1 ]]; echo $?)" \
        "allocator=$(field "$EV/near-base-stats.txt" rebalance allocator)"
    qa_check far_is_follower \
        "$([[ $(field "$EV/far-base-stats.txt" rebalance allocator) == 0 ]]; echo $?)" \
        "allocator=$(field "$EV/far-base-stats.txt" rebalance allocator)"
}

run_alloc() {
    start_target || {
        qa_check target_start 1 "see $EV/target-start.log"
        RUN_EXIT=4
        return
    }
    qa_check target_start 0 "python3 $AL_DIR/alloc-rebalance-target.py"
    client_script || {
        qa_check client_script 1 "$AL_DIR/run-one.sh"
        RUN_EXIT=5
        return
    }
    gen_uploads || {
        qa_check uploads 1 "dd on near"
        RUN_EXIT=5
        return
    }
    local label usz
    for label in a b c d; do
        usz=$(qa_ssh near "stat -c %s '$AL_DIR/up-$label.bin' 2>/dev/null" |
            tr -d '[:space:]')
        qa_check "upload_${label}_size" \
            "$([[ $usz == "$AL_UP_BYTES" ]]; echo $?)" \
            "size=${usz:-?} want=$AL_UP_BYTES"
    done

    start_client a "$AL_LPORT_A"
    stage_check phase1 1 "32 " || { RUN_EXIT=5; return; }
    qa_check near_groups_exposed \
        "$(grep -q 'fec_groups=' "$EV/near-phase1-stats.txt"; echo $?)" \
        "stream row carries fec_groups/fec_mask"
    qa_check far_groups_exposed \
        "$(grep -q 'fec_groups=' "$EV/far-phase1-stats.txt"; echo $?)" \
        "stream row carries fec_groups/fec_mask"
    A_SID=$(sid_of_new /dev/null "$EV/near-phase1-stats.txt")
    check_sid near_a_sid "$EV/near-phase1-stats.txt" "$A_SID" 1

    start_client b "$AL_LPORT_B"
    stage_check phase2a 2 "16 16 " || { RUN_EXIT=5; return; }
    B_SID=$(sid_of_new "$EV/near-phase1-stats.txt" "$EV/near-phase2a-stats.txt")
    check_sid near_b_sid "$EV/near-phase2a-stats.txt" "$B_SID" 1
    check_sid near_a_still "$EV/near-phase2a-stats.txt" "$A_SID" 1

    start_client c "$AL_LPORT_C"
    stage_check phase2b 3 "10 11 11 " || { RUN_EXIT=5; return; }
    C_SID=$(sid_of_new "$EV/near-phase2a-stats.txt" "$EV/near-phase2b-stats.txt")
    check_sid near_c_sid "$EV/near-phase2b-stats.txt" "$C_SID" 1
    check_sid near_a_still2 "$EV/near-phase2b-stats.txt" "$A_SID" 1
    check_sid near_b_still "$EV/near-phase2b-stats.txt" "$B_SID" 1

    start_client d "$AL_LPORT_D"
    stage_check phase2c 4 "8 8 8 8 " || { RUN_EXIT=5; return; }
    D_SID=$(sid_of_new "$EV/near-phase2b-stats.txt" "$EV/near-phase2c-stats.txt")
    check_sid near_d_sid "$EV/near-phase2c-stats.txt" "$D_SID" 1
    for sid in "$A_SID" "$B_SID" "$C_SID"; do
        check_sid "near_survivor_$sid" "$EV/near-phase2c-stats.txt" "$sid" 1
    done
    qa_check all_four_distinct \
        "$([[ -n $A_SID && -n $B_SID && -n $C_SID && -n $D_SID &&
              $(printf '%s\n' "$A_SID" "$B_SID" "$C_SID" "$D_SID" |
                sort -u | wc -l) == 4 ]]; echo $?)" \
        "a=$A_SID b=$B_SID c=$C_SID d=$D_SID"

    qa_check near_rebalance_used32 \
        "$([[ $(field "$EV/near-phase2c-stats.txt" rebalance used) == 32 ]]; echo $?)" \
        "used=$(field "$EV/near-phase2c-stats.txt" rebalance used)"
    qa_check far_rebalance_used32 \
        "$([[ $(field "$EV/far-phase2c-stats.txt" rebalance used) == 32 ]]; echo $?)" \
        "used=$(field "$EV/far-phase2c-stats.txt" rebalance used)"
    delta_ge near_rebalance_begin3 "$EV/near-base-stats.txt" \
        "$EV/near-phase2c-stats.txt" rebalance begin 3
    check_zero_health "p2" base phase2c
    delta_check p2_near_reset_user "$EV/near-base-stats.txt" \
        "$EV/near-phase2c-stats.txt" c11 fec_reset_user
    delta_check p2_far_reset_user "$EV/far-base-stats.txt" \
        "$EV/far-phase2c-stats.txt" c11 fec_reset_user

    for label in b c d; do
        if wait_sent "$label"; then
            qa_check "sent_$label" 0 "body complete, held open"
        else
            qa_check "sent_$label" 1 "target did not finish the body"
        fi
    done

    release b
    wait_client b || qa_check client_b_done 1 "timeout"
    qa_check client_b_rc "$([[ $(client_rc b) == 0 ]]; echo $?)" \
        "rc=$(client_rc b)"
    stage_check phase3a 3 "10 11 11 " || { RUN_EXIT=5; return; }
    check_sid b_gone "$EV/near-phase3a-stats.txt" "$B_SID" 0
    for sid in "$A_SID" "$C_SID" "$D_SID"; do
        check_sid "survivor_$sid" "$EV/near-phase3a-stats.txt" "$sid" 1
    done

    release c
    wait_client c || qa_check client_c_done 1 "timeout"
    qa_check client_c_rc "$([[ $(client_rc c) == 0 ]]; echo $?)" \
        "rc=$(client_rc c)"
    stage_check phase3b 2 "16 16 " || { RUN_EXIT=5; return; }
    check_sid c_gone "$EV/near-phase3b-stats.txt" "$C_SID" 0
    check_sid a_survives_c "$EV/near-phase3b-stats.txt" "$A_SID" 1
    check_sid d_survives_c "$EV/near-phase3b-stats.txt" "$D_SID" 1

    release d
    wait_client d || qa_check client_d_done 1 "timeout"
    qa_check client_d_rc "$([[ $(client_rc d) == 0 ]]; echo $?)" \
        "rc=$(client_rc d)"
    stage_check phase3c 1 "32 " || { RUN_EXIT=5; return; }
    check_sid only_a "$EV/near-phase3c-stats.txt" "$A_SID" 1

    release a
    wait_client a || qa_check client_a_done 1 "timeout"
    qa_check client_a_rc "$([[ $(client_rc a) == 0 ]]; echo $?)" \
        "rc=$(client_rc a)"
    stage_check phase3d 0 "" || { RUN_EXIT=5; return; }
    qa_snap_stats near "$EV/near-final-stats.txt"
    qa_snap_stats far "$EV/far-final-stats.txt"
    for host in near far; do
        qa_check "${host}_final_idle" \
            "$([[ $(active_streams "$EV/$host-final-stats.txt") == 0 ]]; echo $?)" \
            "streams_active=0"
        qa_check "${host}_final_used_total" \
            "$([[ $(field "$EV/$host-final-stats.txt" rebalance used) == \
                  $(field "$EV/$host-final-stats.txt" rebalance total) ]]; echo $?)" \
            "used=$(field "$EV/$host-final-stats.txt" rebalance used)/$(field "$EV/$host-final-stats.txt" rebalance total) (primary flow)"
    done
    delta_ge near_rebalance_begin6 "$EV/near-base-stats.txt" \
        "$EV/near-final-stats.txt" rebalance begin 6
    check_zero_health "final" base final

    verify_bodies
}

verify_bodies() {
    qa_ssh near "cat '$AL_DIR'/rc-* 2>/dev/null; \
        stat -c '%n %s' '$AL_DIR'/out-*.bin 2>/dev/null; \
        sha256sum '$AL_DIR'/out-*.bin 2>/dev/null" >"$EV/near-result.txt" 2>/dev/null || true
    qa_ssh far "cat '$AL_DIR'/sha-*.txt '$AL_DIR'/bytes-*.txt 2>/dev/null; \
        tail -n 40 '$AL_DIR/target.log'" >"$EV/far-result.txt" 2>/dev/null || true

    local label rc size nsha fsha fbytes usha_n usha_f ubytes_f
    local shas=()
    : >"$EV/body-checks.txt"
    for label in a b c d; do
        rc=$(client_rc "$label")
        size=$(qa_ssh near "stat -c %s '$AL_DIR/out-$label.bin' 2>/dev/null" |
            tr -d '[:space:]')
        nsha=$(qa_ssh near \
            "sha256sum '$AL_DIR/out-$label.bin' 2>/dev/null | awk '{print \$1}'" |
            tr -d '[:space:]')
        fsha=$(qa_ssh far "cat '$AL_DIR/sha-$label.txt' 2>/dev/null" |
            tr -d '[:space:]')
        fbytes=$(qa_ssh far "cat '$AL_DIR/bytes-$label.txt' 2>/dev/null" |
            tr -d '[:space:]')
        usha_n=$(qa_ssh near \
            "sha256sum '$AL_DIR/up-$label.bin' 2>/dev/null | awk '{print \$1}'" |
            tr -d '[:space:]')
        usha_f=$(qa_ssh far "cat '$AL_DIR/up-sha-$label.txt' 2>/dev/null" |
            tr -d '[:space:]')
        ubytes_f=$(qa_ssh far "cat '$AL_DIR/up-bytes-$label.txt' 2>/dev/null" |
            tr -d '[:space:]')
        printf '%s rc=%s size=%s near_sha=%s far_sha=%s far_bytes=%s\n' \
            "$label" "${rc:-?}" "${size:-?}" "${nsha:-?}" "${fsha:-?}" \
            "${fbytes:-?}" >>"$EV/body-checks.txt"
        printf '%s up_near_sha=%s up_far_sha=%s up_bytes=%s\n' \
            "$label" "${usha_n:-?}" "${usha_f:-?}" "${ubytes_f:-?}" \
            >>"$EV/body-checks.txt"
        qa_check "body_${label}_rc" "$([[ $rc == 0 ]]; echo $?)" "rc=${rc:-?}"
        qa_check "body_${label}_bytes" \
            "$([[ -n $size && $size == "$fbytes" ]]; echo $?)" \
            "near=${size:-?} far=${fbytes:-?}"
        qa_check "body_${label}_sha" \
            "$([[ -n $nsha && $nsha == "$fsha" ]]; echo $?)" \
            "near=${nsha:-?} far=${fsha:-?}"
        qa_check "body_${label}_up_bytes" \
            "$([[ $ubytes_f == "$AL_UP_BYTES" ]]; echo $?)" \
            "far=${ubytes_f:-?} want=$AL_UP_BYTES"
        qa_check "body_${label}_up_sha" \
            "$([[ -n $usha_n && $usha_n == "$usha_f" ]]; echo $?)" \
            "near=${usha_n:-?} far=${usha_f:-?}"
        [[ -n $nsha ]] && shas+=("$nsha")
    done
    qa_check bodies_distinct \
        "$([[ ${#shas[@]} == 4 &&
              $(printf '%s\n' "${shas[@]}" | sort -u | wc -l) == 4 ]]; echo $?)" \
        "${#shas[@]} distinct hashes"
}

if ((DRY)); then
    dry_run
fi

if [[ $MODE == cleanup ]]; then
    trap - EXIT
    cleanup
    qa_say "CLEANUP done${EV:+ evidence=$EV}"
    exit 0
fi

trap cleanup EXIT
preflight
if ((FAILED)); then
    qa_say "PRECHECK FAIL checks=$FAILED evidence=$EV"
    exit 3
fi

run_alloc

if ((FAILED)); then
    qa_say "RESULT FAIL checks=$FAILED evidence=$EV"
    exit "$RUN_EXIT"
fi
qa_say "RESULT PASS evidence=$EV"
exit 0
