#!/usr/bin/env bash
# mem-512m.sh - 0.7.0 memory acceptance under 256 carriers and a 512 MiB cap.
#
# WHAT IT PROVES
#   One SC-LDPC tunnel (far TX binary 1x256, near TX grouped GF16 8x32, 256
#   carriers) carries two application streams while a far-side stressor asks
#   for 256 MiB and 32 of the 256 carriers run under `tc netem delay 100ms
#   loss 1%`. For 300 s the script samples both ends once a second:
#     /proc/meminfo      MemAvailable, Slab, Cached
#     /proc/slabinfo     kmalloc-8k/16k/32k/64k/128k
#     /proc/net/tbtun/stats
#                        fec_outq_depth/bytes, fec_drop_window, rx_delivered,
#                        submit_bytes, c13 n/m/groups (shape)
#   and derives the per-end FEC memory bounds from that shape:
#     ring_k and replay_bytes (15 MiB TX ring), rx_slots and rx_arena (16 MiB
#     RX arena), credit (K-2 blocks capped at 1024 packets).
#
# PASS
#   * both ends lose at most --limit-mib (default 512) of MemAvailable over
#     the injection window;
#   * neither end increments /proc/vmstat oom_kill, and stress-ng is still
#     running at the end (the stress was real);
#   * fec_drop_window does not move on either end;
#   * rx_delivered advances in every 5 s window on both ends;
#   * carriers stay up (c3 up == --carriers) and the CSV holds one row per
#     second per end.
#
# USAGE
#   scripts/qa/mem-512m.sh --run            # needs both ends preloaded
#   scripts/qa/mem-512m.sh --dry-run        # print the plan, no ssh
#   scripts/qa/mem-512m.sh --cleanup        # qdisc, stress, traffic, tunnel
#
#   --duration S       sample seconds (default 300)
#   --stress-bytes B   stress-ng --vm-bytes (default 256M)
#   --netem-carriers N carriers put under netem (default 32)
#   --limit-mib M      MemAvailable drop bound (default 512)
#   --chunk-mib M      application chunk per direction (default 2048)
#   --near/--far/--near-dev/--near-ip/--far-ip/--near-port/--far-listen
#   --target-port/--carriers/--tunnel/--evidence/--expect-version
#
# PRECONDITIONS
#   Both ends run the 0.7.0 tbtun (mode=sc-ldpc) and accept the config in
#   lib-070.sh; tc, ss, python3, sha256sum, setsid on both; stress-ng on far;
#   root ssh; the near egress root qdisc is a default one. The script writes
#   tunnel 1 on both ends and may replace the near root qdisc; --cleanup puts
#   both back (the root qdisc becomes the kernel default).
#
# OUTPUT
#   <repo>/review/wan-experiment/arm64/qa-070-<stamp>-mem-512m/
#     mem.csv baseline and 1 s rows, sample.log, mem-verdict.txt
#   stdout: CHECK lines, MEM_RESULT PASS|FAIL
# Exit: 0 PASS, 2 usage/preflight, 3 setup, 4 checks, 130 interrupt.
set -uo pipefail

SELF=$(cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib-070.sh
. "$SELF/lib-070.sh"

SCRIPT=mem-512m
MODE=run
DURATION=${DURATION:-300}
STRESS_BYTES=${STRESS_BYTES:-256M}
NETEM_CARRIERS=${NETEM_CARRIERS:-32}
NETEM_DELAY=${NETEM_DELAY:-100ms}
NETEM_LOSS=${NETEM_LOSS:-1%}
LIMIT_MIB=${LIMIT_MIB:-512}
CHUNK_MIB=${CHUNK_MIB:-2048}
SAMPLE_SECS=1
REPLAY_BUDGET=$((15 * 1024 * 1024))
RX_BUDGET=$((16 * 1024 * 1024))

DIR_NEAR=$REMOTE_BASE/$SCRIPT
DIR_FAR=$REMOTE_BASE/$SCRIPT
MARK_NEAR=/tmp/tbt-070-$SCRIPT-netem.mark
TARGET_PID=$DIR_FAR/target.pid
STRESS_PID=$DIR_FAR/stress.pid
STOP_NEAR=$DIR_NEAR/stop
CLEANED=0

usage() {
    sed -n '2,48p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

while [ $# -gt 0 ]; do
    case $1 in
    -h | --help) usage ;;
    --run) MODE=run; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    --cleanup) MODE=cleanup; shift ;;
    --duration) DURATION=$2; shift 2 ;;
    --stress-bytes) STRESS_BYTES=$2; shift 2 ;;
    --netem-carriers) NETEM_CARRIERS=$2; shift 2 ;;
    --limit-mib) LIMIT_MIB=$2; shift 2 ;;
    --chunk-mib) CHUNK_MIB=$2; shift 2 ;;
    *)
        if q070_common_arg "$1" "${2:-}"; then
            shift "$SHIFT_BY"
        else
            q070_die "unknown option $1 (try --help)"
        fi
        ;;
    esac
done

q070_num "$DURATION" || q070_die "--duration must be a number"
q070_num "$LIMIT_MIB" || q070_die "--limit-mib must be a number"
q070_num "$CHUNK_MIB" || q070_die "--chunk-mib must be a number"
[ "$DURATION" -ge 30 ] || q070_die "--duration must be at least 30 s"
[ "$CHUNK_MIB" -ge 64 ] || q070_die "--chunk-mib must be at least 64"

# ---------------------------------------------------------------------------
# injection and cleanup

clean_stress() { q070_kill_pidfile "$FAR_HOST" "$STRESS_PID"; }

clean_traffic() {
    q070_run "$NEAR_HOST" "touch '$STOP_NEAR' 2>/dev/null || true"
    q070_run "$NEAR_HOST" "pkill -f '$DIR_NEAR/bidir-io.py client' 2>/dev/null || true; rm -f '$STOP_NEAR'"
    q070_kill_pidfile "$FAR_HOST" "$TARGET_PID"
}

clean_injection() {
    [ "$CLEANED" = 1 ] && return 0
    CLEANED=1
    q070_netem_ports_clear "$NEAR_HOST" "$NEAR_DEV" "$MARK_NEAR"
    clean_stress
    clean_traffic
}

clean_all() { clean_injection; q070_del_tunnel; }

on_exit() {
    local rc=$?
    [ "$MODE" = run ] && clean_injection
    trap - EXIT
    exit "$rc"
}

# ---------------------------------------------------------------------------
# sampling

emit_row() { # phase host own_line peer_line
    local phase=$1 host=$2 own=$3 peer=$4
    local mem slab cached k8 k16 k32 k64 k128
    local outq outqb drop rx submit cn cm cg
    local pn pm pg ring shares slots arena credit replay oom
    local xk xs xc xa src xcount=0
    mem=$(q070_kv mem "$own"); slab=$(q070_kv slab "$own")
    cached=$(q070_kv cached "$own"); oom=$(q070_kv oom "$own")
    k8=$(q070_kv k8 "$own"); k16=$(q070_kv k16 "$own")
    k32=$(q070_kv k32 "$own"); k64=$(q070_kv k64 "$own")
    k128=$(q070_kv k128 "$own")
    outq=$(q070_kv outq "$own"); outqb=$(q070_kv outq_bytes "$own")
    drop=$(q070_kv drop_window "$own")
    rx=$(q070_kv rx_delivered "$own"); submit=$(q070_kv submit_bytes "$own")
    cn=$(q070_kv n "$own"); cm=$(q070_kv m "$own"); cg=$(q070_kv groups "$own")
    pn=$(q070_kv n "$peer"); pm=$(q070_kv m "$peer"); pg=$(q070_kv groups "$peer")
    xk=$(q070_kv x_replay_k "$own"); xs=$(q070_kv x_slots "$own")
    xc=$(q070_kv x_credit "$own"); xa=$(q070_kv x_arena "$own")
    ring=NA; replay=NA; credit=NA; slots=NA; arena=NA
    src=derived
    if q070_num "$cn" && q070_num "$cm" && q070_num "$cg" && [ "$cg" -gt 0 ]; then
        shares=$((cm + cn))
        ring=$(q070_ring_k "$cg" "$shares")
        replay=$((ring * cg * shares * (8192 + 48)))
    fi
    if q070_num "$pn" && q070_num "$pm" && q070_num "$pg" && [ "$pg" -gt 0 ]; then
        slots=$(q070_arena_slots "$pg" "$((pn + pm))")
        arena=$((slots * pg * (pn + pm) * 8192))
    fi
    if q070_num "$xk"; then
        ring=$xk
        if q070_num "$cn" && q070_num "$cm" && q070_num "$cg"; then
            replay=$((ring * cg * (cm + cn) * (8192 + 48)))
        fi
        xcount=$((xcount + 1))
    fi
    if q070_num "$xs"; then slots=$xs; xcount=$((xcount + 1)); fi
    if q070_num "$xa"; then arena=$xa; xcount=$((xcount + 1)); fi
    if q070_num "$ring" && q070_num "$cn" && q070_num "$cg" && q070_num "$slots"; then
        credit=$(q070_credit "$cg" "$cn" "$ring" "$slots")
    fi
    if q070_num "$xc"; then credit=$xc; xcount=$((xcount + 1)); fi
    if [ "$xcount" -eq 4 ]; then src=proc; elif [ "$xcount" -gt 0 ]; then src=mixed; fi
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$(q070_epoch)" "$phase" "$host" "$mem" "$slab" "$cached" \
        "$k8" "$k16" "$k32" "$k64" "$k128" \
        "$outq" "$outqb" "$drop" "$rx" "$submit" \
        "$cn" "$cm" "$cg" "$ring" "$replay" "$credit" "$slots" "$arena" "$oom" \
        "$src" \
        >>"$CSV"
}

sample_once() { # phase
    local phase=$1 n f
    n=$(q070_sample_line "$NEAR_HOST")
    f=$(q070_sample_line "$FAR_HOST")
    emit_row "$phase" near "$n" "$f"
    emit_row "$phase" far "$f" "$n"
    printf '%s %s\n' "$(q070_now)" "$phase near:$n far:$f" >>"$EV/sample.log"
}

# ---------------------------------------------------------------------------
# plan

plan() {
    local bytes=$((LIMIT_MIB * 1024))
    q070_say "dry-run: $SCRIPT"
    q070_say "  rig: near $NEAR_HOST ($NEAR_IP) far $FAR_HOST ($FAR_IP)"
    q070_say "  shape: near TX n=8 m=24 L=16, far TX n=128 m=128 L=8, carriers=$CARRIERS"
    q070_say "  config: tunnel $TUNNEL, mode=sc-ldpc, crypto=on, http-kal; far target 127.0.0.1:$TARGET_PORT"
    q070_say "  traffic: 2 concurrent streams (dl+ul), ${CHUNK_MIB} MiB chunks, no hash"
    q070_say "  inject:  far: stress-ng --vm 1 --vm-bytes $STRESS_BYTES --timeout ${DURATION}s"
    q070_say "           near: prio 700: + netem 740: $NETEM_DELAY $NETEM_LOSS on $NETEM_CARRIERS carrier sport filters"
    q070_say "  sample:  1 s x $DURATION, both ends: meminfo/slabinfo/stats/c13/oom_kill"
    q070_say "  pass:    dMemAvailable <= $bytes KiB per end; oom_kill delta 0; fec_drop_window delta 0;"
    q070_say "           rx_delivered advances every 5 s; up stays $CARRIERS"
    q070_say "  evidence: ${EVIDENCE_ROOT:-<repo>/review/wan-experiment/arm64}"
    q070_say "  no ssh, no writes (dry-run)"
}

# ---------------------------------------------------------------------------
# checks

check_mem() { # host base_kb min_kb
    local host=$1 base=$2 min=$3 delta limit
    limit=$((LIMIT_MIB * 1024))
    if ! q070_num "$base" || ! q070_num "$min"; then
        q070_check "mem_$host" 1 "base=$base min=$min missing"
        return
    fi
    delta=$((base - min))
    q070_check "mem_$host" \
        "$([ "$delta" -le "$limit" ]; echo $?)" \
        "base=${base}KiB min=${min}KiB delta=${delta}KiB limit=${limit}KiB"
}

csv_metric() { # field min|max [phase]
    local f=$1 how=$2 phase=${3:-run}
    awk -F, -v f="$f" -v how="$how" -v p="$phase" '
        $2 == p && $f ~ /^[0-9]+$/ {
            if (!n++) { v = $f + 0 }
            if (how == "min" && $f + 0 < v) v = $f + 0
            if (how == "max" && $f + 0 > v) v = $f + 0
        }
        END { print (n ? v : "NA") }' "$CSV"
}

first_last() { # field phase host
    awk -F, -v f="$1" -v p="$2" -v h="$3" '
        $2 == p && $3 == h && $f ~ /^[0-9]+$/ {
            if (!n++) first = $f + 0
            last = $f + 0
        }
        END { print (n ? first : "NA"), (n ? last : "NA") }' "$CSV"
}

frontier() { # host -> "bad checked"
    awk -F, -v h="$1" '
        $2 == "run" && $3 == h && $15 ~ /^[0-9]+$/ {
            n++
            rx[n] = $15 + 0
        }
        END {
            if (n < 6) { print 0, 0; exit }
            bad = 0; c = 0
            for (i = 6; i <= n; i++) {
                c++
                if (rx[i] <= rx[i - 5]) bad++
            }
            print bad, c
        }' "$CSV"
}

# ---------------------------------------------------------------------------
# run

run_case() {
    local deadline ports np base_near base_far min_near min_far
    local dnp dfp onp ofp fcn fcf bad c inject_epoch alive_checked=0
    local stress_alive=1 mark_alive=1
    local chunk_bytes=$((CHUNK_MIB * 1024 * 1024))

    q070_evidence_init "$SCRIPT" >/dev/null || q070_die "cannot create evidence dir"
    FAILED=0
    CSV=$EV/mem.csv
    : >"$EV/sample.log"
    printf 'ts,phase,host,mem_kb,slab_kb,cached_kb,kmalloc_8k,kmalloc_16k,kmalloc_32k,kmalloc_64k,kmalloc_128k,fec_outq_depth,fec_outq_bytes,fec_drop_window,rx_delivered,submit_bytes,shape_n,shape_m,shape_groups,ring_k,replay_bytes,credit,rx_slots,rx_arena,oom_kill,mem_src\n' >"$CSV"

    q070_note "preflight"
    q070_require_rig \
        "tc ss python3 sha256sum awk sed setsid sleep sort" \
        "tc ss python3 sha256sum awk sed setsid sleep sort stress-ng"
    q070_qdisc_root_ok "$NEAR_HOST" "$NEAR_DEV" ||
        q070_die "near $NEAR_DEV root qdisc not restorable by this script"

    q070_note "configure tunnel $TUNNEL"
    q070_configure
    q070_join || q070_die "carriers did not join"

    q070_run "$NEAR_HOST" "mkdir -p '$DIR_NEAR' && rm -f '$STOP_NEAR'"
    q070_run "$FAR_HOST" "mkdir -p '$DIR_FAR/results' && rm -f '$DIR_FAR/ready'"
    q070_push "$NEAR_HOST" "$SELF/bidir-io.py" "$DIR_NEAR/bidir-io.py" ||
        q070_die "cannot push bidir-io.py to near"
    q070_push "$FAR_HOST" "$SELF/bidir-io.py" "$DIR_FAR/bidir-io.py" ||
        q070_die "cannot push bidir-io.py to far"

    q070_note "start far target"
    if ! q070_script "$FAR_HOST" "$DIR_FAR" "$TARGET_PORT" <<'EOS'
D=$1; P=$2
cd "$D" || exit 3
rm -f ready
setsid nohup python3 "$D/bidir-io.py" serve --bind 127.0.0.1 --port "$P" \
    --result-dir "$D/results" --ready-file "$D/ready" --no-hash \
    </dev/null >"$D/serve.log" 2>&1 &
echo $! > target.pid
EOS
    then
        q070_die "far target failed to start"
    fi
    q070_wait_file "$FAR_HOST" "$DIR_FAR/ready" 20 ||
        q070_die "far target did not become ready"

    q070_note "start bidirectional traffic (chunk ${CHUNK_MIB} MiB)"
    if ! q070_script "$NEAR_HOST" "$DIR_NEAR" "$chunk_bytes" "$NEAR_PORT" <<'EOS'
D=$1; N=$2; P=$3
cd "$D" || exit 3
rm -f stop
for role in dl ul; do
    setsid sh -c 'while [ ! -e stop ]; do python3 '"$D"'/bidir-io.py client \
        --host 127.0.0.1 --port '"$P"' --role '"$role"' --bytes '"$N"' --no-hash \
        >>'"$role"'.log 2>&1 || exit 1; done' </dev/null >/dev/null 2>&1 &
    echo $! > "client-$role.pid"
done
EOS
    then
        q070_die "near traffic loops failed to start"
    fi

    sleep 3
    q070_note "baseline"
    sample_once base
    sleep "$SAMPLE_SECS"
    sample_once base
    sleep "$SAMPLE_SECS"
    sample_once base

    q070_note "inject: netem $NETEM_DELAY $NETEM_LOSS on $NETEM_CARRIERS carriers"
    ports=$(q070_carrier_ports "$NEAR_BLOCK" "$NETEM_CARRIERS" | tr '\n' ' ')
    np=$(printf '%s\n' "$ports" | wc -w | tr -d ' ')
    q070_check "netem_carrier_count" \
        "$([ "$np" -eq "$NETEM_CARRIERS" ]; echo $?)" \
        "selected=$np want=$NETEM_CARRIERS"
    if [ "$np" -ne "$NETEM_CARRIERS" ]; then
        q070_die "need $NETEM_CARRIERS live carriers to impair, got $np"
    fi
    q070_netem_ports_apply "$NEAR_HOST" "$NEAR_DEV" "$ports" \
        "$NETEM_DELAY" "$NETEM_LOSS" "$MARK_NEAR" \
        $((DURATION * 2 + 300)) ||
        q070_die "netem apply failed"

    q070_note "inject: stress-ng $STRESS_BYTES on far"
    if ! q070_script "$FAR_HOST" "$DIR_FAR" "$STRESS_BYTES" "$DURATION" <<'EOS'
D=$1; B=$2; T=$3
cd "$D" || exit 3
setsid nohup stress-ng --vm 1 --vm-bytes "$B" --timeout "${T}s" \
    --metrics-brief </dev/null >stress.log 2>&1 &
echo $! > stress.pid
EOS
    then
        q070_die "stress-ng failed to start"
    fi

    q070_note "sample $DURATION s"
    inject_epoch=$(date +%s)
    deadline=$((inject_epoch + DURATION))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if [ "$alive_checked" = 0 ] &&
            [ "$(date +%s)" -ge $((deadline - 3)) ]; then
            q070_ssh "$FAR_HOST" "kill -0 \$(cat '$STRESS_PID' 2>/dev/null)" \
                >/dev/null 2>&1
            stress_alive=$?
            alive_checked=1
        fi
        sample_once run
        now=$(date +%s)
        [ "$now" -lt "$deadline" ] && sleep 1
    done
    [ "$alive_checked" = 1 ] || stress_alive=1

    q070_note "injection still live?"
    if q070_file_exists "$NEAR_HOST" "$MARK_NEAR"; then mark_alive=0; fi

    sample_once end
    clean_injection

    q070_note "checks"
    base_near=$(first_last 4 base near | awk '{print $2}')
    base_far=$(first_last 4 base far | awk '{print $2}')
    min_near=$(csv_metric 4 min run)
    min_far=$(csv_metric 4 min run)
    check_mem near "$base_near" "$min_near"
    check_mem far "$base_far" "$min_far"

    dfp=$(first_last 14 base near | awk '{print $2}')
    dnp=$(first_last 14 run near | awk '{print $2}')
    q070_check drop_window_near \
        "$([ "${dfp:-x}" = "${dnp:-y}" ]; echo $?)" "base=$dfp last=$dnp"
    dfp=$(first_last 14 base far | awk '{print $2}')
    dnp=$(first_last 14 run far | awk '{print $2}')
    q070_check drop_window_far \
        "$([ "${dfp:-x}" = "${dnp:-y}" ]; echo $?)" "base=$dfp last=$dnp"

    onp=$(first_last 25 base near | awk '{print $1}')
    ofp=$(first_last 25 run near | awk '{print $2}')
    q070_check oom_near \
        "$([ "${onp:-x}" = "${ofp:-y}" ]; echo $?)" "base=$onp last=$ofp"
    onp=$(first_last 25 base far | awk '{print $1}')
    ofp=$(first_last 25 run far | awk '{print $2}')
    q070_check oom_far \
        "$([ "${onp:-x}" = "${ofp:-y}" ]; echo $?)" "base=$onp last=$ofp"

    read -r bad c <<<"$(frontier near)"
    q070_check frontier_near \
        "$([ "${c:-0}" -ge $((DURATION / 5 - 1)) ] && [ "${bad:-1}" -eq 0 ]; echo $?)" \
        "windows=$c stalled=$bad"
    read -r bad c <<<"$(frontier far)"
    q070_check frontier_far \
        "$([ "${c:-0}" -ge $((DURATION / 5 - 1)) ] && [ "${bad:-1}" -eq 0 ]; echo $?)" \
        "windows=$c stalled=$bad"

    q070_check stress_alive "$stress_alive" "stress-ng seen at the end"
    q070_check netem_active "$mark_alive" "netem marker $MARK_NEAR"

    dnp=$(q070_stats_key "$NEAR_HOST" up c3)
    dfp=$(q070_stats_key "$FAR_HOST" up c3)
    q070_check carriers_near "$([ "${dnp:-0}" = "$CARRIERS" ]; echo $?)" "up=$dnp want=$CARRIERS"
    q070_check carriers_far "$([ "${dfp:-0}" = "$CARRIERS" ]; echo $?)" "up=$dfp want=$CARRIERS"

    fcf=$(awk -F, '$2 == "run" { n++ } END { print n + 0 }' "$CSV")
    fbn=$((DURATION * 2))
    q070_check csv_rows \
        "$([ "$fcf" -ge $((fbn - 4)) ]; echo $?)" "run_rows=$fcf want>=$((fbn - 4))"

    {
        printf 'duration=%ss limit_mib=%s stress=%s netem_carriers=%s\n' \
            "$DURATION" "$LIMIT_MIB" "$STRESS_BYTES" "$NETEM_CARRIERS"
        printf 'near base=%sKiB min=%sKiB far base=%sKiB min=%sKiB\n' \
            "$base_near" "$min_near" "$base_far" "$min_far"
    } >"$EV/mem-verdict.txt"
    q070_verdict MEM | tee -a "$EV/mem-verdict.txt"
}

# ---------------------------------------------------------------------------

case $MODE in
cleanup)
    clean_all
    q070_say "mem-512m: cleanup done"
    exit 0
    ;;
esac

if [ "$DRY_RUN" = 1 ]; then
    plan
    exit 0
fi

[ "$(id -u)" = 0 ] || q070_warn "not root locally; remote writes still run as the ssh user"

CSV=
trap on_exit EXIT INT TERM
run_case
