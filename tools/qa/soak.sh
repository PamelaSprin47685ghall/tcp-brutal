#!/usr/bin/env bash
# soak.sh - 0.3.3 real-hardware long soak.
#
# One HTTP stream runs per cycle. The stream lives for S seconds and both
# ends take a sample every K seconds. The tool then proves the carrier
# invariant, the zero-delta counters, and the kernel window for that cycle.
# The defaults give a one-hour run: 60 cycles of 60 seconds.
#
# The near side serves the user app on 127.0.0.1:$NEAR_PORT. The far side
# serves stream-server.py on 127.0.0.1:$FAR_PORT. The tunnel joins them.
#
# Modes
#   --run       full soak (default)
#   --status    read-only report: configs, c3/c6/c10/c11, RSS, slab
#   --cleanup   stop the far source, remove temp files, optional rmmod
#
# Exit codes
#   0 PASS  2 usage  3 preflight  4 far source  5 traffic  6 resource
#   7 checks  9 cleanup residual
set -uo pipefail

. "$(dirname -- "$0")/lib.sh"

PROG=${0##*/}
MODE=run
KEEP=${KEEP:-0}
RMMOD=${RMMOD:-0}
CYCLES=${CYCLES:-60}
WIN_SECONDS=${WIN_SECONDS:-60}
SAMPLE_SECONDS=${SAMPLE_SECONDS:-5}
MAX_MEM_DROP_MIB=${MAX_MEM_DROP_MIB:-64}

# Judgement constants. The usage text repeats them for the operator.
LEAK_SLAB_DELTA=32      # active_objs, last 10 cycles minus first 10
FAR_RSS_GROWTH_MIB=16   # far stream-server RSS growth limit
FINAL_SAMPLES=3         # rest samples for dead_release and D-state
FINAL_GAP=2             # seconds between rest samples
TEARDOWN_WAIT=5         # seconds between last cycle and first rest sample

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
FAR_DIR=$REMOTE_QA
FAR_PY=$FAR_DIR/stream-server.py
FAR_PIDFILE=$FAR_DIR/stream-server.pid
FAR_LOGFILE=$FAR_DIR/stream-server.log

EV=
START_EPOCH=
START_UTC=
RUN_END=
RUN_END_UTC=
FAILED=0
CLEANED=0
CLEANUP_RC=0
CLEANUP_STATE=unknown
FORCE_CLEAN=0
NEAR_USED=0
FAR_STARTED=0
CURL_LIVE=0
FAR_PID=
CURL_PID=
BASE_NEAR_MEM=
BASE_FAR_MEM=
FAR_RSS_BASE=
UP_MIN=
FINAL_NEAR_MEM=
FINAL_FAR_MEM=
FINAL_FAR_RSS=
RESOURCE_FAIL=0
RESOURCE_TEXT=
KW_PASS=0
KW_SKIP=0
KW_FAIL=0
DSTATE_BAD=0
DSTATE_NOTE=
D_PREV_near=
D_PREV_far=
DR_NZ_near=0
DR_NZ_far=0
DR_ABSENT_near=0
DR_ABSENT_far=0
CYC_START=0
CYC_END=0
CYC_ELAPSED=0
CYC_BYTES=0
CYC_HTTP=
CYC_NEAR_UP=
CYC_FAR_UP=
CYC_DATA_TX=

declare -A MAXD=()
declare -A ZABSENT=()
declare -A D_STREAK=()
declare -a SKIPS=()

# Fields that must not move during a cycle. The class is a hint; the
# parser falls back to the first tunnel line that carries the field.
# The drop counters are part of the leak/drop judgement: a steady stream
# must not lose frames to a budget, a queue, a window, or an unknown sid.
ZERO_SPECS="c3:stream_reset_tcp_failed c11:fec_reset_tcp_failed c11:fec_reset_hole c6:holes_unrecoverable c10:fec_deliver_bad c9:fec_proto_fail c9:fec_hdr_crc_fail c9:fec_payload_crc_fail c3:retain_drop c3:stall_drop c11:fec_outq_drop c11:fec_replay_drop c9:fec_drop_window c9:fec_drop_sid c9:fec_drop_crypto c9:bind_drop_no_stream"

CYCLE_HEADER='cycle,start_epoch,end_epoch,elapsed,bytes,http,near_up,far_up,data_tx_delta,rc'
CSV_HEADER='cycle,phase,ts,host,mem_available_kb,slab_carrier_active,slab_carrier_num,slab_session_active,slab_session_num,slab_stream_active,slab_stream_num,rss_kib,dstate'

usage() {
    cat <<EOF
usage: $PROG [--run|--status|--cleanup] [--keep] [--cycles N]
              [--seconds S] [--sample S] [--rmmod] [--max-mem-drop-mib M]

Modes:
  --run       full soak (default). Exit 0 only when every check passes.
  --status    read-only: both configs, c3/c6/c10/c11, RSS, slab. Exit 0.
  --cleanup   stop the far source, remove temp files, optional rmmod.

Flags:
  --keep      leave the far source and the module in place after the run
  --cycles N  cycles to run (default 60, at least 1)
  --seconds S seconds of traffic per cycle (default 60, at least 1)
  --sample S  seconds between samples inside a cycle (default 5)
  --rmmod     rmmod tbtun on both ends during cleanup (default off)
  --max-mem-drop-mib M  MemAvailable drop limit per end (default 64)

Environment:
  NEAR_HOST=$NEAR_HOST
  FAR_HOST=$FAR_HOST
  NEAR_PORT=$NEAR_PORT  FAR_PORT=$FAR_PORT  FAR_IP=$FAR_IP  TUNNEL=$TUNNEL
  PROC_ROOT=$PROC_ROOT  REMOTE_QA=$REMOTE_QA  PYTHON=$PYTHON
  SSH_TMO=$SSH_TMO  EVIDENCE_ROOT=$EVIDENCE_ROOT
  CYCLES=$CYCLES  WIN_SECONDS=$WIN_SECONDS
  SAMPLE_SECONDS=$SAMPLE_SECONDS  MAX_MEM_DROP_MIB=$MAX_MEM_DROP_MIB
  KEEP=0  RMMOD=0
  Every value above also comes from the environment. A flag wins.

Judgement:
  Traffic: size_download > 0 in every cycle.
  Carriers: up == n and replacing == empty == 0 on both ends.
  Zero deltas per cycle: stream_reset_tcp_failed, fec_reset_tcp_failed,
    fec_reset_hole, holes_unrecoverable, fec_deliver_bad, fec_proto_fail,
    fec_hdr_crc_fail, fec_payload_crc_fail, retain_drop, stall_drop,
    fec_outq_drop, fec_replay_drop, fec_drop_window, fec_drop_sid,
    fec_drop_crypto, bind_drop_no_stream.
  Data: near data_tx delta > 0 in every cycle.
  Kernel window: BUG/Oops/WARNING/UBSAN/general protection fault fails a
    cycle and stops the run. An empty window is SKIP.
  Memory: MemAvailable drop from the baseline > --max-mem-drop-mib on
    either end fails.
  Leak guard: mean tbt_carrier, tbt_session, and tbt_stream active_objs of
    the last 10 cycles may exceed the mean of the first 10 cycles by at
    most $LEAK_SLAB_DELTA on either end. Fewer than 20 cycles, or missing
    slab values, is SKIP.
  Far source RSS growth must not exceed $FAR_RSS_GROWTH_MIB MiB.
  c3 dead_release must be 0 at rest. A nonzero value in 3 consecutive
    final samples fails.
  A new D-state pid present in 3 consecutive samples fails.

Exit codes:
  0 PASS  2 usage  3 preflight  4 far source  5 traffic  6 resource
  7 checks  9 cleanup residual (listener or module remains, or cleanup
  could not verify it). --status always exits 0.
EOF
}

# ---------------------------------------------------------------------------
# small parsers

sf() { # stats-file class field
    local v
    v=$(qa_field "$1" "$2" "$3")
    if [[ -z $v ]]; then
        v=$(qa_field_any "$1" "$3")
    fi
    printf '%s' "$v"
}

f_epoch() { sed -n 's/^EPOCH=//p' "$1" 2>/dev/null | head -n 1; }
f_mem() { sed -n 's/^MEM_AVAILABLE_KB=//p' "$1" 2>/dev/null | head -n 1; }
f_rss() { sed -n 's/^RSS_KB=//p' "$1" 2>/dev/null | head -n 1; }
f_dstate() { sed -n 's/^DSTATE=//p' "$1" 2>/dev/null | head -n 1; }

last_field() { # text-file key
    sed -n "s/.*$2=\([0-9][0-9]*\).*/\1/p" "$1" 2>/dev/null | tail -n 1
}

slab_pair() { # sample-file slab-name -> "active num"
    awk -F'[ =]+' -v n="$2" '$1 == "SLAB" && $2 == n { print $3, $4; exit }' \
        "$1" 2>/dev/null
}

record_skip() {
    SKIPS+=("$1${2:+ ($2)}")
    qa_skip "$1" "${2:-}"
}

# ---------------------------------------------------------------------------
# far source

far_port_listening() {
    qa_ssh far "ss -ltnH 2>/dev/null | awk '\$4 ~ /:$FAR_PORT\$/ { f = 1 } END { exit !f }'" \
        >/dev/null 2>&1
}

far_port_state() { # 0 listening, 1 free, 2 far unreachable
    if ! qa_ssh far true >/dev/null 2>&1; then
        printf '2'
        return 0
    fi
    if far_port_listening; then
        printf '0'
    else
        printf '1'
    fi
}

far_source_pid() {
    local pid
    pid=$(qa_ssh far "cat '$FAR_PIDFILE' 2>/dev/null" 2>/dev/null |
        tr -d '[:space:]')
    if qa_num "$pid" &&
        qa_ssh far "ps -o args= -p $pid 2>/dev/null | grep -q 'stream-server[.]py'" \
            >/dev/null 2>&1; then
        printf '%s' "$pid"
        return 0
    fi
    pid=$(qa_ssh far "pgrep -f '[s]tream-server[.]py --host 127[.]0[.]0[.]1 --port $FAR_PORT' 2>/dev/null | head -n 1" 2>/dev/null |
        tr -d '[:space:]')
    if qa_num "$pid"; then
        printf '%s' "$pid"
        return 0
    fi
    return 1
}

start_far_source() {
    local i pid
    qa_ssh far "mkdir -p '$FAR_DIR'" >/dev/null 2>&1 || return 1
    timeout "$SSH_TMO" scp -q -o BatchMode=yes \
        "$HERE/stream-server.py" "$FAR_HOST:$FAR_PY" || return 1
    qa_ssh far "test -s '$FAR_PY'" >/dev/null 2>&1 || return 1
    if far_port_listening; then
        qa_note "far 127.0.0.1:$FAR_PORT is already listening; refuse to start"
        return 1
    fi
    qa_ssh far "rm -f '$FAR_PIDFILE' '$FAR_LOGFILE'; setsid '$PYTHON' '$FAR_PY' --host 127.0.0.1 --port '$FAR_PORT' --seconds 900 >'$FAR_LOGFILE' 2>&1 </dev/null & echo \$! > '$FAR_PIDFILE'" \
        >/dev/null 2>&1 || return 1
    # From here the server is ours. Cleanup must stop it even if the listen
    # wait or the pid lookup fails.
    FAR_STARTED=1
    i=0
    while ((i < 20)); do
        far_port_listening && break
        sleep 0.5
        i=$((i + 1))
    done
    if ! far_port_listening; then
        qa_ssh far "tail -n 20 '$FAR_LOGFILE' 2>/dev/null" \
            >>"$EV/far-source.log" 2>&1 || true
        return 1
    fi
    pid=$(far_source_pid) || return 1
    FAR_PID=$pid
    printf '%s\n' "$pid" >"$EV/far-source.pid"
    qa_note "far source LISTEN 127.0.0.1:$FAR_PORT pid=$pid"
    return 0
}

stop_far_source() {
    local pid state
    pid=$(qa_ssh far "cat '$FAR_PIDFILE' 2>/dev/null" 2>/dev/null |
        tr -d '[:space:]')
    if qa_num "$pid"; then
        qa_ssh far "kill $pid 2>/dev/null; i=0; while kill -0 $pid 2>/dev/null && [ \$i -lt 20 ]; do sleep 0.25; i=\$((i+1)); done; kill -9 $pid 2>/dev/null || true" \
            >/dev/null 2>&1 || true
    fi
    qa_ssh far "pkill -f '[s]tream-server[.]py --host 127[.]0[.]0[.]1 --port $FAR_PORT' 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    qa_ssh far "rm -f '$FAR_PIDFILE'" >/dev/null 2>&1 || true
    state=$(far_port_state)
    case $state in
        0) return 1 ;;
        2) return 2 ;;
        *) return 0 ;;
    esac
}

find_near_curl_pid() {
    qa_ssh near "pgrep -f 'curl.*127[.]0[.]0[.]1:$NEAR_PORT' 2>/dev/null | head -n 1" 2>/dev/null |
        tr -d '[:space:]'
}

rss_of_pid() { # near|far pid
    qa_ssh "$1" "ps -o rss= -p $2 2>/dev/null" 2>/dev/null |
        tr -d '[:space:]'
}

# ---------------------------------------------------------------------------
# status (read-only)

status_end() { # near|far
    qa_say "== $1"
    if ! qa_ssh "$1" "PROC_ROOT='$PROC_ROOT' TUNNEL='$TUNNEL' bash -s" 2>/dev/null <<'EOS'
set -u
printf -- '--- config ---\n'
cat "$PROC_ROOT/config" 2>/dev/null || true
printf -- '--- stats c3/c6/c10/c11 ---\n'
awk -v t="$TUNNEL" '$1 == "tunnel" && $2 == t && ($3 == "c3" || $3 == "c6" || $3 == "c10" || $3 == "c11")' \
    "$PROC_ROOT/stats" 2>/dev/null || true
printf -- '--- mem ---\n'
awk '/^MemAvailable:/ { print; exit }' /proc/meminfo 2>/dev/null || true
printf -- '--- slab ---\n'
grep -E '^tbt_(carrier|session|stream)[[:space:]]' /proc/slabinfo 2>/dev/null || true
printf -- '--- rss top ---\n'
ps -eo pid,rss,comm --sort=-rss 2>/dev/null | head -n 8 || true
EOS
    then
        qa_say "   (unreachable)"
    fi
}

status_mode() {
    local pid cpid
    status_end near
    status_end far
    pid=$(far_source_pid 2>/dev/null || true)
    if [[ -n $pid ]]; then
        qa_say "far_source pid=$pid rss_kib=$(rss_of_pid far "$pid")"
    else
        qa_say "far_source pid=none"
    fi
    cpid=$(find_near_curl_pid 2>/dev/null || true)
    if [[ -n $cpid ]]; then
        qa_say "near_curl pid=$cpid rss_kib=$(rss_of_pid near "$cpid")"
    else
        qa_say "near_curl pid=none"
    fi
    exit 0
}

# ---------------------------------------------------------------------------
# cleanup

cleanup_once() {
    local out="${EV:-/dev/null}/cleanup.txt" rc=0 src_rc=0 host
    ((CLEANED)) && return 0
    CLEANED=1

    printf '=== cleanup %s ===\n' "$(qa_now)" >>"$out"
    printf 'keep=%s rmmod=%s far_started=%s curl_live=%s\n' \
        "$KEEP" "$RMMOD" "$FAR_STARTED" "$CURL_LIVE" >>"$out"

    if ((KEEP && !FORCE_CLEAN)); then
        printf 'result=skipped (--keep)\n' >>"$out"
        CLEANUP_STATE="skipped (--keep)"
        CLEANUP_RC=0
        cat "$out" >&2 2>/dev/null || true
        return 0
    fi

    if ((CURL_LIVE)); then
        qa_stream_curl_stop
        CURL_LIVE=0
        printf 'near_curl=stopped\n' >>"$out"
    else
        printf 'near_curl=not_live\n' >>"$out"
    fi

    if ((FAR_STARTED || FORCE_CLEAN)); then
        qa_ssh far "tail -n 20 '$FAR_LOGFILE' 2>/dev/null" >>"$out" 2>&1 || true
        stop_far_source
        src_rc=$?
        case $src_rc in
            0)
                printf 'far_source=stopped\n' >>"$out"
                FAR_STARTED=0
                ;;
            1)
                printf 'far_source=RESIDUAL listener still present\n' >>"$out"
                rc=9
                ;;
            *)
                printf 'far_source=unverified (far host unreachable)\n' >>"$out"
                rc=9
                ;;
        esac
    else
        printf 'far_source=not_started\n' >>"$out"
    fi

    if ((NEAR_USED || FORCE_CLEAN)); then
        qa_ssh near "rm -f '$REMOTE_QA'/near-curl-c*.txt" >/dev/null 2>&1 || true
        printf 'near_temp_files=removed (best effort)\n' >>"$out"
    fi

    if ((RMMOD)); then
        for host in near far; do
            if ! qa_ssh "$host" true >/dev/null 2>&1; then
                printf '%s_module=unverified\n' "$host" >>"$out"
                rc=9
                continue
            fi
            qa_ssh "$host" "rmmod tbtun 2>/dev/null || true" >/dev/null 2>&1 || true
            if qa_ssh "$host" "lsmod 2>/dev/null | grep -q '^tbtun '" >/dev/null 2>&1; then
                printf '%s_module=RESIDUAL loaded\n' "$host" >>"$out"
                rc=9
            else
                printf '%s_module=unloaded\n' "$host" >>"$out"
            fi
        done
    else
        printf 'module=left_loaded (no --rmmod)\n' >>"$out"
    fi

    if ((rc == 0)); then
        printf 'result=clean\n' >>"$out"
        CLEANUP_STATE=clean
    else
        printf 'result=residual\n' >>"$out"
        CLEANUP_STATE=residual
    fi
    CLEANUP_RC=$rc
    cat "$out" >&2 2>/dev/null || true
    return 0
}

cleanup_mode() {
    FORCE_CLEAN=1
    KEEP=0
    EV=${EV:-$EVIDENCE_ROOT/soak-0.3.3-$(date -u +%Y%m%dT%H%M%SZ)}
    mkdir -p "$EV" || {
        printf 'soak: cannot create %s\n' "$EV" >&2
        exit 3
    }
    [[ -e $EV/run.log ]] || : >"$EV/run.log"
    [[ -e $EV/timeline.txt ]] || : >"$EV/timeline.txt"
    cleanup_once
    printf 'evidence=%s\n' "$EV"
    exit "$CLEANUP_RC"
}

on_exit() {
    local rc=$?
    cleanup_once
    if ((rc == 0 && CLEANUP_RC > 0)); then
        exit "$CLEANUP_RC"
    fi
    exit "$rc"
}

# ---------------------------------------------------------------------------
# sampling

snap_phase() { # host cycle tag phase rss-pid
    local host=$1 cyc=$2 tag=$3 phase=$4 rss_pid=$5 out ts mem rss ds
    local ca cn sa sn ta tn
    out=$EV/c$cyc-$host-$tag.txt
    qa_ssh "$host" "RSS_PID='$rss_pid' PROC_ROOT='$PROC_ROOT' bash -s" \
        >"$out" 2>/dev/null <<'EOS' || true
set -u
printf 'EPOCH=%s\n' "$(date +%s)"
cat "$PROC_ROOT/stats" 2>/dev/null || true
printf '__STATS_END__\n'
awk '/^MemAvailable:/ { printf "MEM_AVAILABLE_KB=%s\n", $2; exit }' /proc/meminfo 2>/dev/null || true
grep -E '^tbt_(carrier|session|stream)[[:space:]]' /proc/slabinfo 2>/dev/null | sed 's/^/SLAB=/' || true
rss=
if [ -n "${RSS_PID:-}" ]; then
    rss=$(ps -o rss= -p "$RSS_PID" 2>/dev/null | tr -d ' ')
fi
printf 'RSS_KB=%s\n' "$rss"
printf 'DSTATE='
ps -eo stat=,pid=,comm= 2>/dev/null | awk '$1 ~ /^D/ { printf "%s %s;", $2, $3 }'
printf '\n'
EOS
    ts=$(f_epoch "$out")
    mem=$(f_mem "$out")
    rss=$(f_rss "$out")
    ds=$(f_dstate "$out")
    read -r ca cn < <(slab_pair "$out" tbt_carrier) || true
    read -r sa sn < <(slab_pair "$out" tbt_session) || true
    read -r ta tn < <(slab_pair "$out" tbt_stream) || true
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$cyc" "$phase" "${ts:-}" "$host" "${mem:-}" \
        "${ca:-}" "${cn:-}" "${sa:-}" "${sn:-}" "${ta:-}" "${tn:-}" \
        "${rss:-}" "${ds:-}" >>"$EV/soak.csv"
}

samples_append() { # host cycle k sample-file
    local host=$1 c=$2 k=$3 f=$4 cls
    printf 'SAMPLE ts=%s host=%s k=%s\n' "$(f_epoch "$f")" "$host" "$k" \
        >>"$EV/c$c-samples.txt"
    for cls in c3 c6 c10 c11; do
        qa_line "$f" "$cls" | sed "s/^/$host /" >>"$EV/c$c-samples.txt"
    done
}

dstate_pid_list() { # baseline file from qa_dstate_pids
    awk 'NF >= 2 && $1 ~ /^D/ { print $2 }' "$1" 2>/dev/null | sort -u
}

dstate_ids_string() {
    printf '%s' "$1" | tr ';' '\n' | awk 'NF { print $1 }'
}

dstate_track() { # host dstate-string
    local host=$1 ds=$2 pid prev_var prev key base_ids newp= entry=
    base_ids=$(dstate_pid_list "$EV/$host-dstate-pre.txt")
    prev_var="D_PREV_$host"
    prev=${!prev_var}
    for pid in $(dstate_ids_string "$ds"); do
        grep -qx "$pid" <<<"$base_ids" && continue
        key="$host:$pid"
        if [[ " $prev " == *" $pid "* ]]; then
            D_STREAK[$key]=$(( ${D_STREAK[$key]:-0} + 1 ))
        else
            D_STREAK[$key]=1
        fi
        if ((D_STREAK[$key] >= 3)); then
            DSTATE_BAD=1
            DSTATE_NOTE+="$host pid=$pid in 3 consecutive samples; "
            D_STREAK[$key]=0
        fi
        newp+="$pid "
    done
    printf -v "$prev_var" '%s' "$newp"
}

dr_check() { # host sample-file
    local host=$1 f=$2 v
    v=$(sf "$f" c3 dead_release)
    if [[ -z $v ]]; then
        if [[ $host == near ]]; then
            DR_ABSENT_near=1
            DR_NZ_near=0
        else
            DR_ABSENT_far=1
            DR_NZ_far=0
        fi
    elif qa_num "$v" && ((v != 0)); then
        if [[ $host == near ]]; then
            DR_NZ_near=$((DR_NZ_near + 1))
        else
            DR_NZ_far=$((DR_NZ_far + 1))
        fi
    else
        if [[ $host == near ]]; then
            DR_NZ_near=0
        else
            DR_NZ_far=0
        fi
    fi
    printf 'dead_release %s %s=%s\n' "$(qa_now)" "$host" "${v:-absent}" \
        >>"$EV/dead-release.txt"
}

kw_cycle() { # cycle host since until
    local c=$1 host=$2 t0=$3 t1=$4 f state
    f=$EV/c$c-kernel-$host.txt
    if ! qa_num "$t0" || ! qa_num "$t1"; then
        KW_SKIP=$((KW_SKIP + 1))
        record_skip "c${c}_${host}_kernel" "no epoch in the pre or post snapshot"
        return 0
    fi
    qa_kernel_window "$host" "$t0" "$t1" "$f"
    state=$(qa_kw_state "$f")
    case $state in
        PASS)
            KW_PASS=$((KW_PASS + 1))
            qa_check "c${c}_${host}_kernel" 0 "clean source=$(qa_kw_source "$f")"
            return 0
            ;;
        SKIP)
            KW_SKIP=$((KW_SKIP + 1))
            record_skip "c${c}_${host}_kernel" "empty window source=$(qa_kw_source "$f")"
            return 0
            ;;
        *)
            KW_FAIL=$((KW_FAIL + 1))
            qa_check "c${c}_${host}_kernel" 1 "BUG/Oops/WARNING in window"
            return 1
            ;;
    esac
}

# ---------------------------------------------------------------------------
# resource verdict

slab_means() {
    awk -F, -v lim="$LEAK_SLAB_DELTA" '
        $1 ~ /^[0-9]+$/ && $2 == "post" && $4 != "" {
            h = $4
            n[h]++
            val["tbt_carrier" SUBSEP h SUBSEP n[h]] = $6
            val["tbt_session" SUBSEP h SUBSEP n[h]] = $8
            val["tbt_stream" SUBSEP h SUBSEP n[h]] = $10
        }
        END {
            split("tbt_carrier tbt_session tbt_stream", names, " ")
            for (h in n) {
                for (j = 1; j <= 3; j++) {
                    name = names[j]
                    if (n[h] < 20) {
                        printf "slab %s %s - - - SKIP\n", name, h
                        continue
                    }
                    miss = 0; a = 0; b = 0
                    for (i = 1; i <= 10; i++) {
                        v = val[name SUBSEP h SUBSEP i]
                        if (v == "") miss = 1
                        a += v
                    }
                    for (i = n[h] - 9; i <= n[h]; i++) {
                        v = val[name SUBSEP h SUBSEP i]
                        if (v == "") miss = 1
                        b += v
                    }
                    if (miss) {
                        printf "slab %s %s - - - SKIP\n", name, h
                        continue
                    }
                    a /= 10; b /= 10
                    printf "slab %s %s %.1f %.1f %.1f %s\n",
                        name, h, a, b, b - a, (b - a > lim ? "FAIL" : "PASS")
                }
            }
        }' "$EV/soak.csv" | sort -k2,2 -k3,3
}

assess_resources() { # label near-mem far-mem far-rss dead-verdict
    local label=$1 nmem=${2:-} fmem=${3:-} frss=${4:-} dead=${5:-SKIP}
    local fail=0 rc verdict text drop growth limit_kb out
    local tag name host first last diff sverd dsverd
    limit_kb=$((MAX_MEM_DROP_MIB * 1024))
    text="resource label=$label at $(qa_now)"$'\n'

    drop=na
    verdict=SKIP
    rc=1
    if qa_num "$nmem" && qa_num "${BASE_NEAR_MEM:-}"; then
        drop=$((BASE_NEAR_MEM - nmem))
        if ((drop > limit_kb)); then
            verdict=FAIL
        else
            verdict=PASS
            rc=0
        fi
    fi
    [[ $verdict == FAIL ]] && fail=1
    text+="mem near baseline_kb=${BASE_NEAR_MEM:-?} now_kb=${nmem:-?} drop_kb=$drop limit_kb=$limit_kb verdict=$verdict"$'\n'
    if [[ $label == final ]]; then
        qa_check resource_mem_near "$rc" "drop_kb=$drop limit_kb=$limit_kb"
    fi

    drop=na
    verdict=SKIP
    rc=1
    if qa_num "$fmem" && qa_num "${BASE_FAR_MEM:-}"; then
        drop=$((BASE_FAR_MEM - fmem))
        if ((drop > limit_kb)); then
            verdict=FAIL
        else
            verdict=PASS
            rc=0
        fi
    fi
    [[ $verdict == FAIL ]] && fail=1
    text+="mem far baseline_kb=${BASE_FAR_MEM:-?} now_kb=${fmem:-?} drop_kb=$drop limit_kb=$limit_kb verdict=$verdict"$'\n'
    if [[ $label == final ]]; then
        qa_check resource_mem_far "$rc" "drop_kb=$drop limit_kb=$limit_kb"
    fi

    growth=na
    verdict=SKIP
    rc=1
    if qa_num "$frss" && qa_num "${FAR_RSS_BASE:-}"; then
        growth=$((frss - FAR_RSS_BASE))
        if ((growth > FAR_RSS_GROWTH_MIB * 1024)); then
            verdict=FAIL
        else
            verdict=PASS
            rc=0
        fi
    fi
    [[ $verdict == FAIL ]] && fail=1
    text+="far_source rss growth_kb=$growth limit_kb=$((FAR_RSS_GROWTH_MIB * 1024)) verdict=$verdict"$'\n'
    if [[ $label == final ]]; then
        qa_check resource_far_rss "$rc" "growth_kb=$growth limit_kb=$((FAR_RSS_GROWTH_MIB * 1024))"
    fi

    out=$(slab_means)
    while read -r tag name host first last diff sverd; do
        [[ $tag == slab ]] || continue
        [[ -n $name ]] || continue
        text+="slab $name $host active_objs first10=$first last10=$last delta=$diff limit=$LEAK_SLAB_DELTA verdict=$sverd"$'\n'
        [[ $sverd == FAIL ]] && fail=1
        if [[ $label == final ]]; then
            case $sverd in
                PASS)
                    qa_check "resource_slab_${name}_${host}" 0 \
                        "first10=$first last10=$last delta=$diff"
                    ;;
                FAIL)
                    qa_check "resource_slab_${name}_${host}" 1 \
                        "first10=$first last10=$last delta=$diff"
                    ;;
                *)
                    record_skip "resource_slab_${name}_${host}" \
                        "fewer than 20 cycles or slab values missing"
                    ;;
            esac
        fi
    done <<<"$out"

    text+="dead_release verdict=$dead nonzero_samples near=$DR_NZ_near far=$DR_NZ_far"$'\n'
    if [[ $label == final ]]; then
        case $dead in
            FAIL)
                qa_check resource_dead_release 1 \
                    "nonzero in 3 consecutive final samples near=$DR_NZ_near far=$DR_NZ_far"
                fail=1
                ;;
            SKIP)
                record_skip resource_dead_release "c3 dead_release absent on both ends"
                ;;
            *)
                qa_check resource_dead_release 0 \
                    "zero at rest near=$DR_NZ_near far=$DR_NZ_far"
                ;;
        esac
    fi

    dsverd=PASS
    ((DSTATE_BAD)) && dsverd=FAIL
    text+="dstate verdict=$dsverd note=${DSTATE_NOTE:-none}"$'\n'
    if [[ $label == final ]]; then
        if ((DSTATE_BAD)); then
            qa_check resource_dstate 1 "${DSTATE_NOTE:-sustained new D-state}"
            fail=1
        else
            qa_check resource_dstate 0 "no new pid in 3 consecutive samples"
        fi
    fi

    RESOURCE_TEXT=$text
    printf '%s' "$text" >>"$EV/resource.txt"
    return $((fail > 0))
}

# ---------------------------------------------------------------------------
# cycles and verdict

cycle_record() { # cycle rc
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$1" "${CYC_START:-}" "${CYC_END:-}" "${CYC_ELAPSED:-0}" \
        "${CYC_BYTES:-0}" "${CYC_HTTP:-}" "${CYC_NEAR_UP:-}" \
        "${CYC_FAR_UP:-}" "${CYC_DATA_TX:-}" "$2" >>"$EV/cycles.txt"
}

write_summary() { # [stop] [kind] [detail]
    local stop=${1:-complete} kind=${2:-} detail=${3:-}
    local verdict=PASS n_cyc=0 total=0 mean_rate=0 min_rate=0
    local wall=0 need=$((CYCLES * WIN_SECONDS)) mean_mbit=0 i spec fld

    if ! qa_num "${RUN_END:-}"; then
        RUN_END=$(qa_epoch)
        RUN_END_UTC=$(qa_now)
    fi

    if [[ -s ${EV:-}/cycles.txt ]]; then
        read -r n_cyc total mean_rate min_rate < <(awk -F, '
            NR > 1 && $1 ~ /^[0-9]+$/ {
                seen++
                if ($10 + 0 == 0) done++
                bytes += $5
                if ($4 + 0 > 0) {
                    r = $5 / $4
                    sumr += r
                    if (nr == 0 || r < minr) minr = r
                    nr++
                }
            }
            END {
                if (seen == 0) { print "0 0 0 0"; exit }
                printf "%d %.0f %.0f %.0f\n", done, bytes,
                    (nr ? sumr / nr : 0), minr
            }' "$EV/cycles.txt") || true
    fi
    if qa_num "${RUN_END:-}" && qa_num "${START_EPOCH:-}"; then
        wall=$((RUN_END - START_EPOCH))
    fi
    mean_mbit=$(awk -v r="${mean_rate:-0}" 'BEGIN { printf "%.2f", r * 8 / 1000000 }')

    [[ $stop != complete ]] && verdict=FAIL
    ((FAILED > 0)) && verdict=FAIL
    ((CLEANUP_RC > 0)) && verdict=FAIL

    {
        printf 'soak 0.3.3\n'
        printf 'start=%s end=%s\n' "${START_UTC:-?}" "${RUN_END_UTC:-?}"
        printf 'near=%s far=%s tunnel=%s\n' "$NEAR_HOST" "$FAR_HOST" "$TUNNEL"
        printf 'evidence=%s\n' "$EV"
        printf 'verdict=%s\n' "$verdict"
        printf 'stop=%s' "$stop"
        [[ -n $kind ]] && printf ' kind=%s' "$kind"
        [[ -n $detail ]] && printf ' detail=%s' "$detail"
        printf '\n\n'
        printf 'run\n'
        printf '  cycles_done=%s/%s\n' "$n_cyc" "$CYCLES"
        printf '  wall_seconds=%s required_seconds=%s\n' "$wall" "$need"
        printf '  total_bytes=%s\n' "$total"
        printf '  mean_rate_Bps=%s (%s Mbit/s)\n' "$mean_rate" "$mean_mbit"
        printf '  min_rate_Bps=%s\n' "$min_rate"
        printf '  up_min=%s\n' "${UP_MIN:-n/a}"
        printf '  kernel_window pass=%s skip=%s fail=%s\n' \
            "$KW_PASS" "$KW_SKIP" "$KW_FAIL"
        printf '\nzero_delta max_over_cycles_and_ends\n'
        for spec in $ZERO_SPECS; do
            fld=${spec#*:}
            printf '  %s max_delta=%s' "$fld" "${MAXD[$fld]:-0}"
            [[ -n ${ZABSENT[$fld]:-} ]] &&
                printf ' absent_observations=%s' "${ZABSENT[$fld]}"
            printf '\n'
        done
        printf '\nresource\n'
        if [[ -n ${RESOURCE_TEXT:-} ]]; then
            printf '%s' "$RESOURCE_TEXT"
        else
            printf '  not evaluated\n'
        fi
        printf '\nskips\n'
        if ((${#SKIPS[@]} == 0)); then
            printf '  none\n'
        else
            for i in "${SKIPS[@]}"; do
                printf '  - %s\n' "$i"
            done
        fi
        printf '\nchecks\n'
        printf '  failed=%s\n' "$FAILED"
        grep -E '^CHECK .* FAIL' "$EV/run.log" 2>/dev/null | sed 's/^/  /' || true
        printf '\ncleanup\n'
        printf '  state=%s residual_rc=%s\n' "$CLEANUP_STATE" "$CLEANUP_RC"
        printf '  file=%s/cleanup.txt\n' "$EV"
    } >"$EV/SUMMARY.txt"
    qa_say "SUMMARY $EV/SUMMARY.txt verdict=$verdict"
}

cycle_abort() { # cycle kind detail
    local c=$1 kind=$2 detail=$3 rc
    if [[ $kind == traffic ]]; then
        rc=5
    else
        rc=7
    fi
    qa_note "cycle $c FAIL kind=$kind detail=$detail"
    cycle_record "$c" "$rc"
    qa_capture_diag "cycle-$c"
    cleanup_once
    write_summary "cycle-$c" "$kind" "$detail"
    exit "$rc"
}

# ---------------------------------------------------------------------------
# run

run_soak() {
    local c k win_start now next nearmem farmem near_t0 far_t0 tag
    local pre post p q d spec cls fld n up rep emp inv_fail checks_fail
    local traffic_fail fail_detail dead wall

    qa_evidence_init soak-0.3.3 >/dev/null || {
        printf 'soak: cannot create the evidence directory under %s\n' \
            "$EVIDENCE_ROOT" >&2
        exit 3
    }
    trap on_exit EXIT
    printf 'EVIDENCE=%s\n' "$EV"
    START_UTC=$(qa_now)
    START_EPOCH=$(qa_epoch)
    qa_mark run-start
    printf '%s\n' "$CYCLE_HEADER" >"$EV/cycles.txt"
    printf '%s\n' "$CSV_HEADER" >"$EV/soak.csv"
    for spec in $ZERO_SPECS; do
        MAXD[${spec#*:}]=0
    done

    if ! qa_preflight; then
        qa_capture_diag preflight
        cleanup_once
        write_summary preflight
        exit 3
    fi
    NEAR_USED=1

    snap_phase near 0 baseline baseline ""
    snap_phase far 0 baseline baseline ""
    BASE_NEAR_MEM=$(f_mem "$EV/c0-near-baseline.txt")
    BASE_FAR_MEM=$(f_mem "$EV/c0-far-baseline.txt")

    if ! start_far_source; then
        qa_check far_source 1 "start failed or 127.0.0.1:$FAR_PORT busy"
        qa_capture_diag far-source
        cleanup_once
        write_summary far-source
        exit 4
    fi
    FAR_STARTED=1
    qa_check far_source 0 "LISTEN 127.0.0.1:$FAR_PORT pid=$FAR_PID"
    snap_phase far 0 baseline-src baseline-src "$FAR_PID"
    BASE_FAR_MEM=$(f_mem "$EV/c0-far-baseline-src.txt")
    [[ -n $BASE_FAR_MEM ]] || BASE_FAR_MEM=$(f_mem "$EV/c0-far-baseline.txt")
    FAR_RSS_BASE=$(f_rss "$EV/c0-far-baseline-src.txt")

    for ((c = 1; c <= CYCLES; c++)); do
        qa_mark "cycle-$c-start"
        CYC_START=$(qa_epoch)
        CYC_END=0
        CYC_ELAPSED=0
        CYC_BYTES=0
        CYC_HTTP=
        CYC_NEAR_UP=
        CYC_FAR_UP=
        CYC_DATA_TX=
        inv_fail=0
        checks_fail=0
        traffic_fail=0
        fail_detail=

        snap_phase near "$c" pre pre ""
        near_t0=$(f_epoch "$EV/c$c-near-pre.txt")
        snap_phase far "$c" pre pre ""
        far_t0=$(f_epoch "$EV/c$c-far-pre.txt")
        : >"$EV/c$c-samples.txt"

        if ! qa_stream_curl_start near "$EV/near-curl-c$c.txt" "$WIN_SECONDS"; then
            CYC_END=$(qa_epoch)
            CYC_ELAPSED=$((CYC_END - CYC_START))
            cycle_abort "$c" traffic "qa_stream_curl_start failed"
        fi
        CURL_LIVE=1
        CURL_PID=$(find_near_curl_pid || true)
        qa_num "$CURL_PID" || CURL_PID=
        if [[ -n $CURL_PID ]]; then
            printf '%s\n' "$CURL_PID" >"$EV/near-curl-c$c.pid"
        fi

        win_start=$(qa_epoch)
        k=0
        while :; do
            now=$(qa_epoch)
            ((now - win_start >= WIN_SECONDS)) && break
            k=$((k + 1))
            snap_phase near "$c" "s$k" sample "$CURL_PID"
            samples_append near "$c" "$k" "$EV/c$c-near-s$k.txt"
            dstate_track near "$(f_dstate "$EV/c$c-near-s$k.txt")"
            snap_phase far "$c" "s$k" sample "$FAR_PID"
            samples_append far "$c" "$k" "$EV/c$c-far-s$k.txt"
            dstate_track far "$(f_dstate "$EV/c$c-far-s$k.txt")"
            next=$((win_start + k * SAMPLE_SECONDS))
            now=$(qa_epoch)
            if ((next > now)); then
                sleep $((next - now))
            fi
        done

        if [[ -n $CURL_PID ]]; then
            qa_ssh near "i=0; while kill -0 $CURL_PID 2>/dev/null && [ \$i -lt 20 ]; do sleep 0.25; i=\$((i+1)); done" \
                >/dev/null 2>&1 || true
        fi
        qa_stream_curl_stop
        CURL_LIVE=0
        qa_stream_curl_result near "$EV/near-curl-c$c.txt" >/dev/null
        CYC_BYTES=$(last_field "$EV/near-curl-c$c.txt" size)
        CYC_HTTP=$(last_field "$EV/near-curl-c$c.txt" http)
        qa_num "$CYC_BYTES" || CYC_BYTES=0
        CYC_END=$(qa_epoch)
        CYC_ELAPSED=$((CYC_END - CYC_START))

        snap_phase near "$c" post post ""
        snap_phase far "$c" post post "$FAR_PID"

        kw_cycle "$c" near "$near_t0" "$(f_epoch "$EV/c$c-near-post.txt")" || {
            checks_fail=1
            fail_detail+="near_kernel; "
        }
        kw_cycle "$c" far "$far_t0" "$(f_epoch "$EV/c$c-far-post.txt")" || {
            checks_fail=1
            fail_detail+="far_kernel; "
        }

        for tag in near far; do
            post=$EV/c$c-$tag-post.txt
            n=$(sf "$post" c3 n)
            up=$(sf "$post" c3 up)
            rep=$(sf "$post" c3 replacing)
            emp=$(sf "$post" c3 empty)
            if qa_num "$n" && qa_num "$up" && qa_num "$rep" && qa_num "$emp" &&
                ((up == n && rep == 0 && emp == 0)); then
                :
            else
                inv_fail=1
                fail_detail+="${tag}_carrier(up=$up n=$n replacing=$rep empty=$emp); "
            fi
            if [[ $tag == near ]]; then
                CYC_NEAR_UP=$up
            else
                CYC_FAR_UP=$up
            fi
            if qa_num "$up"; then
                if [[ -z $UP_MIN ]] || ((up < UP_MIN)); then
                    UP_MIN=$up
                fi
            fi
        done

        {
            printf 'cycle=%s\n' "$c"
            for tag in near far; do
                printf -- '-- %s\n' "$tag"
                pre=$EV/c$c-$tag-pre.txt
                post=$EV/c$c-$tag-post.txt
                for spec in $ZERO_SPECS; do
                    cls=${spec%%:*}
                    fld=${spec#*:}
                    p=$(sf "$pre" "$cls" "$fld")
                    q=$(sf "$post" "$cls" "$fld")
                    if [[ -z $p && -z $q ]]; then
                        ZABSENT[$fld]=$(( ${ZABSENT[$fld]:-0} + 1 ))
                        printf '%s absent\n' "$fld"
                        continue
                    fi
                    d=$(qa_delta "$p" "$q")
                    printf '%s pre=%s post=%s delta=%s\n' \
                        "$fld" "${p:-?}" "${q:-?}" "$d"
                    if [[ $d == 0 ]]; then
                        :
                    elif qa_num "$d"; then
                        checks_fail=1
                        ((d > MAXD[$fld])) && MAXD[$fld]=$d
                        fail_detail+="${tag}_${fld}(delta=$d); "
                        qa_check "c${c}_${tag}_${fld}" 1 "delta=$d" >&2
                    else
                        checks_fail=1
                        fail_detail+="${tag}_${fld}(delta=$d); "
                        qa_check "c${c}_${tag}_${fld}" 1 "delta=$d" >&2
                    fi
                done
            done
            pre=$EV/c$c-near-pre.txt
            post=$EV/c$c-near-post.txt
            p=$(sf "$pre" c3 data_tx)
            q=$(sf "$post" c3 data_tx)
            CYC_DATA_TX=$(qa_delta "$p" "$q")
            printf 'data_tx pre=%s post=%s delta=%s\n' \
                "${p:-?}" "${q:-?}" "$CYC_DATA_TX"
        } >"$EV/c$c-deltas.txt"

        if qa_num "$CYC_DATA_TX" && ((CYC_DATA_TX > 0)); then
            :
        else
            checks_fail=1
            fail_detail+="near_data_tx(delta=$CYC_DATA_TX); "
            qa_check "c${c}_near_data_tx" 1 "delta=$CYC_DATA_TX"
        fi

        if ((CYC_BYTES > 0)); then
            :
        else
            traffic_fail=1
            fail_detail+="size_download=$CYC_BYTES; "
            qa_check "c${c}_traffic" 1 \
                "size_download=$CYC_BYTES http=${CYC_HTTP:-?}"
        fi

        if ((c % 10 == 0 && c < CYCLES)); then
            assess_resources "progress-cycle-$c" \
                "$(f_mem "$EV/c$c-near-post.txt")" \
                "$(f_mem "$EV/c$c-far-post.txt")" \
                "$(f_rss "$EV/c$c-far-post.txt")" SKIP >/dev/null || true
        fi

        if ((traffic_fail)); then
            cycle_abort "$c" traffic "$fail_detail"
        fi
        if ((inv_fail || checks_fail)); then
            cycle_abort "$c" checks "$fail_detail"
        fi
        cycle_record "$c" 0
        qa_check "c${c}_cycle" 0 \
            "bytes=$CYC_BYTES up=$CYC_NEAR_UP/$CYC_FAR_UP data_tx=$CYC_DATA_TX"
        qa_mark "cycle-$c-done"
    done

    qa_note "cycles complete; teardown wait ${TEARDOWN_WAIT}s"
    sleep "$TEARDOWN_WAIT"
    for ((k = 1; k <= FINAL_SAMPLES; k++)); do
        tag=final$k
        snap_phase near "$CYCLES" "$tag" final ""
        nearmem=$(f_mem "$EV/c$CYCLES-near-$tag.txt")
        dstate_track near "$(f_dstate "$EV/c$CYCLES-near-$tag.txt")"
        dr_check near "$EV/c$CYCLES-near-$tag.txt"
        snap_phase far "$CYCLES" "$tag" final "$FAR_PID"
        farmem=$(f_mem "$EV/c$CYCLES-far-$tag.txt")
        dstate_track far "$(f_dstate "$EV/c$CYCLES-far-$tag.txt")"
        dr_check far "$EV/c$CYCLES-far-$tag.txt"
        FINAL_NEAR_MEM=$nearmem
        FINAL_FAR_MEM=$farmem
        FINAL_FAR_RSS=$(f_rss "$EV/c$CYCLES-far-$tag.txt")
        if ((k < FINAL_SAMPLES)); then
            sleep "$FINAL_GAP"
        fi
    done

    RUN_END=$(qa_epoch)
    RUN_END_UTC=$(qa_now)
    qa_mark run-done
    wall=$((RUN_END - START_EPOCH))
    qa_check wall_seconds \
        "$([ "$wall" -ge $((CYCLES * WIN_SECONDS)) ]; echo $?)" \
        "wall=${wall}s need=$((CYCLES * WIN_SECONDS))s"

    if ((DR_NZ_near >= FINAL_SAMPLES || DR_NZ_far >= FINAL_SAMPLES)); then
        dead=FAIL
    elif ((DR_ABSENT_near && DR_ABSENT_far)); then
        dead=SKIP
    else
        dead=PASS
    fi

    assess_resources final "$FINAL_NEAR_MEM" "$FINAL_FAR_MEM" \
        "$FINAL_FAR_RSS" "$dead"
    RESOURCE_FAIL=$?
    cleanup_once
    write_summary complete
    if ((RESOURCE_FAIL)); then
        exit 6
    fi
    if ((FAILED > 0)); then
        exit 7
    fi
    if ((CLEANUP_RC > 0)); then
        exit 9
    fi
    exit 0
}

# ---------------------------------------------------------------------------
# argument parsing

while (($#)); do
    case $1 in
        --run) MODE=run ;;
        --status) MODE=status ;;
        --cleanup) MODE=cleanup ;;
        --keep) KEEP=1 ;;
        --rmmod) RMMOD=1 ;;
        --cycles) CYCLES=${2:-}; shift 2 || true; continue ;;
        --cycles=*) CYCLES=${1#*=} ;;
        --seconds) WIN_SECONDS=${2:-}; shift 2 || true; continue ;;
        --seconds=*) WIN_SECONDS=${1#*=} ;;
        --sample) SAMPLE_SECONDS=${2:-}; shift 2 || true; continue ;;
        --sample=*) SAMPLE_SECONDS=${1#*=} ;;
        --max-mem-drop-mib) MAX_MEM_DROP_MIB=${2:-}; shift 2 || true; continue ;;
        --max-mem-drop-mib=*) MAX_MEM_DROP_MIB=${1#*=} ;;
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

if ! qa_num "$CYCLES" || ((CYCLES < 1)) ||
    ! qa_num "$WIN_SECONDS" || ((WIN_SECONDS < 1)) ||
    ! qa_num "$SAMPLE_SECONDS" || ((SAMPLE_SECONDS < 1)) ||
    ! qa_num "$MAX_MEM_DROP_MIB"; then
    printf '%s: run length and memory limit must be non-negative integers\n' \
        "$PROG" >&2
    usage >&2
    exit 2
fi
if ((SAMPLE_SECONDS > WIN_SECONDS)); then
    printf '%s: --sample must not exceed --seconds\n' "$PROG" >&2
    usage >&2
    exit 2
fi

case $MODE in
    run) run_soak ;;
    status) status_mode ;;
    cleanup) cleanup_mode ;;
esac
