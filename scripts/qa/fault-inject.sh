#!/usr/bin/env bash
# fault-inject.sh - 0.3.3 real-hardware carrier fault injection.
#
# One run starts a 60 s near-side HTTP stream through the tunnel, kills
# carriers on the near side, rejects carrier packets on the far side with
# RST for 2 s, waits for the carrier set to recover, and judges the run.
# The run is single shot. A failed run is recorded and not retried.
#
# Modes
#   --run       full run (default)
#   --status    read-only: print both configs and the c3/c6/c10/c11 lines
#   --cleanup   stop the far source, remove the RST rule, remove temp files
#
# Flags
#   --keep      keep tbtun loaded; --rmmod is skipped
#   --cycles N  number of carriers to kill (default 3)
#   --gap S     seconds between kills (default 0.2)
#   --no-rst    skip the far-side RST window
#   --rmmod     unload tbtun on both ends during cleanup (default off)
#
# Injected surfaces
#   1. ss -K on one near-side carrier TCP pair (lport,rport). The near source
#      port is inside 30000-30511 and the far port is 9000, so the SSH port
#      22 can never match. The kernel sends RST; the module marks the carrier
#      stopping and dials a new one.
#   2. far-side iptables INPUT REJECT --reject-with tcp-reset on dport 9000,
#      held for 2 s. During the window every carrier packet is reset and
#      fresh dials fail. The rule is removed by the trap.
#   3. --rmmod unloads tbtun on both ends. Every tunnel on the host drops.
#
# Risks
#   - A kill can reset a stream if it removes the last carrying carrier.
#   - The RST window can exceed the hole budget and reset a stream.
#   - A stale RST rule rejects every carrier dial. The EXIT trap removes it.
#   - --rmmod stops all traffic through tbtun until the module is loaded.
#   - Run this on the test pair only. The near app is live during the run.
#
# Environment
#   NEAR_HOST=root@192.168.123.82   FAR_HOST=root@45.11.1.62
#   FAR_IP=45.11.1.62               NEAR_PORT=9001
#   FAR_PORT=5210                   TUNNEL=1
#   SSH_TMO=20                      PROC_ROOT=/proc/net/tbtun
#   CARRIER_SPORT_MIN=30000         CARRIER_SPORT_MAX=30511
#   CARRIER_DPORT=9000
#   STREAM_SECONDS=60               REMOTE_QA=/root/tbtun-qa
#   PYTHON=python3                  EVIDENCE_ROOT=<repo>/review/wan-experiment/arm64
#   CYCLES=3                        GAP=0.2
#   DSTATE_WHITELIST=               optional regex of D-state lines to ignore
#
# Exit codes
#   0 PASS   2 usage   3 preflight   4 far source   5 stream
#   6 injection   7 checks   9 cleanup residual
set -uo pipefail

PROG=${0##*/}
SELF_DIR=$(cd -- "$(dirname -- "$0")" && pwd)

# shellcheck source=lib.sh
. "$SELF_DIR/lib.sh"

MODE=run
KEEP=${KEEP:-0}
CYCLES=${CYCLES:-3}
GAP=${GAP:-0.2}
DO_RST=1
RMMOD_REQUESTED=0
DSTATE_WHITELIST=${DSTATE_WHITELIST:-}

CLEANUP_MODE=0
RST_ADDED=0
CLEANED=0
SUMMARY_WRITTEN=0
RESIDUAL=0
FAILED=0
STREAM_BAD=0

START_EPOCH=
END_EPOCH=
CURL_START_EPOCH=
RECOVER_SECONDS=?
KILL_LOG=()
CURL_LINE=
CURL_SIZE=?
CURL_HTTP=?
KW_NEAR_STATE=not-run
KW_FAR_STATE=not-run
UP_MIN=
UP_MIN_DETAIL=
INVARIANT_BAD=0
INVARIANT_DETAIL=
TRANSIENT_NOTE=
DSTATE_SUSTAINED=0
DSTATE_SAMPLE=
DSTATE_ROW_NOTE=
declare -A DSTATE_HITS=()

CSV_HEADER='ts,cycle,phase,near_up,far_up,near_replacing,far_replacing,near_empty,far_empty,near_stream_reset,far_stream_reset,near_holes_unrec,far_holes_unrec,near_deliver_bad,far_deliver_bad,near_data_tx,far_data_tx,note'

usage() {
    cat <<EOF
usage: $PROG [--run|--status|--cleanup] [--keep] [--cycles N] [--gap S]
              [--no-rst] [--rmmod]

Modes:
  --run        full fault-injection run (default)
  --status     read-only. Print both configs and the c3/c6/c10/c11 lines.
  --cleanup    stop the far source, remove the RST rule, remove temp files.
               Add --rmmod to unload tbtun on both ends.

Flags:
  --keep       keep tbtun loaded after the run; overrides --rmmod
  --cycles N   number of different carriers to kill (default $CYCLES)
  --gap S      seconds between kills (default $GAP)
  --no-rst     skip the far-side RST window (default: RST enabled)
  --rmmod      unload tbtun on both ends during cleanup (default off)
  -h, --help   print this text

Environment overrides:
  NEAR_HOST FAR_HOST FAR_IP NEAR_PORT FAR_PORT TUNNEL SSH_TMO
  PROC_ROOT CARRIER_SPORT_MIN CARRIER_SPORT_MAX CARRIER_DPORT
  STREAM_SECONDS
  REMOTE_QA PYTHON EVIDENCE_ROOT CYCLES GAP KEEP DSTATE_WHITELIST

Injected surfaces and risks:
  kill    ss -K on one near-side carrier TCP pair. The near source port is
          inside CARRIER_SPORT_MIN..MAX and the far port is CARRIER_DPORT
          (9000), so SSH port 22 can never match. A kill can reset the
          stream when it removes the last carrying carrier.
  rst     far iptables INPUT REJECT --reject-with tcp-reset on dport
          CARRIER_DPORT, held for 2 s. Every carrier packet is reset and
          fresh dials fail. The EXIT trap and --cleanup remove the rule.
          A stale rule breaks every carrier dial.
  rmmod   unloads tbtun on both ends. Every tunnel on the host drops until
          the module is loaded again.

Exit codes:
  0 PASS   2 usage   3 preflight   4 far source   5 stream
  6 injection   7 checks   9 cleanup residual
EOF
}

usage_error() {
    printf '%s: %s\n' "$PROG" "$1" >&2
    usage >&2
    exit 2
}

while (($#)); do
    case $1 in
        --run) MODE=run ;;
        --status) MODE=status ;;
        --cleanup) MODE=cleanup ;;
        --keep) KEEP=1 ;;
        --cycles)
            (($# >= 2)) || usage_error "--cycles needs a number"
            CYCLES=$2
            shift
            ;;
        --gap)
            (($# >= 2)) || usage_error "--gap needs a number"
            GAP=$2
            shift
            ;;
        --no-rst) DO_RST=0 ;;
        --rmmod) RMMOD_REQUESTED=1 ;;
        -h | --help) usage; exit 0 ;;
        *) usage_error "unknown argument: $1" ;;
    esac
    shift
done

case $CYCLES in
    '' | *[!0-9]*) usage_error "--cycles must be a positive integer" ;;
esac
((CYCLES >= 1)) || usage_error "--cycles must be a positive integer"
[[ $GAP =~ ^[0-9]+([.][0-9]+)?$ ]] || usage_error "--gap must be a number"
case $KEEP in 0 | 1) ;; *) usage_error "KEEP must be 0 or 1" ;; esac

# ---------------------------------------------------------------------------
# status: read-only

if [[ $MODE == status ]]; then
    for host in near far; do
        case $host in
            near) printf '== near %s\n' "$NEAR_HOST" ;;
            far) printf '== far %s\n' "$FAR_HOST" ;;
        esac
        qa_ssh "$host" "cat '$PROC_ROOT/config' 2>/dev/null; echo ---; \
            awk -v t='$TUNNEL' '\$1 == \"tunnel\" && \$2 == t && \
            (\$3 == \"c3\" || \$3 == \"c6\" || \$3 == \"c10\" || \$3 == \"c11\")' \
            '$PROC_ROOT/stats' 2>/dev/null" || true
    done
    exit 0
fi

# ---------------------------------------------------------------------------
# cleanup

stop_far_server() {
    qa_ssh far "REMOTE_QA='$REMOTE_QA' bash -s" >/dev/null 2>&1 <<'EOS' || true
set -u
pidfile=$REMOTE_QA/stream-server.pid
if [ -f "$pidfile" ]; then
    pid=$(cat "$pidfile" 2>/dev/null || true)
    case ${pid:-} in
        '' | *[!0-9]*) ;;
        *)
            kill -TERM "$pid" 2>/dev/null || true
            i=0
            while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 20 ]; do
                sleep 0.1
                i=$((i + 1))
            done
            kill -KILL "$pid" 2>/dev/null || true
            ;;
    esac
fi
rm -f "$pidfile"
EOS
}

rst_rule_present() {
    qa_ssh far "iptables -w 5 -C INPUT -p tcp --dport $CARRIER_DPORT -j REJECT --reject-with tcp-reset" \
        >/dev/null 2>&1
}

have_iptables() {
    qa_ssh far 'command -v iptables >/dev/null 2>&1'
}

cleanup_all() {
    ((CLEANED)) && return 0
    CLEANED=1
    local out
    out=${EV:-/dev/null}/cleanup.txt
    [[ -n ${EV:-} ]] && : >"$out"

    {
        printf '=== %s cleanup ===\n' "$(qa_now)"
        printf -- '--- far RST rule ---\n'
        if have_iptables; then
            if ((RST_ADDED || CLEANUP_MODE)); then
                if qa_rst_del; then
                    printf 'iptables -D returned 0\n'
                else
                    printf 'iptables -D returned nonzero\n'
                fi
            else
                printf 'no rule was added by this run\n'
            fi
        else
            printf 'iptables absent on far\n'
        fi

        printf -- '--- near curl ---\n'
        qa_stream_curl_stop
        printf 'curl stop requested\n'

        printf -- '--- far stream server ---\n'
        stop_far_server
        printf 'pidfile stop requested\n'
    } >>"$out" 2>&1

    if [[ -n ${EV:-} ]]; then
        qa_ssh far "cat '$REMOTE_QA/stream-server.log' 2>/dev/null" \
            >"$EV/far-stream-server.log" 2>/dev/null || true
    fi

    {
        printf -- '--- temp files ---\n'
        qa_ssh near "rm -f '$REMOTE_QA/near-curl.txt'" >/dev/null 2>&1 || true
        qa_ssh far "rm -f '$REMOTE_QA/stream-server.pid' '$REMOTE_QA/stream-server.log'" \
            >/dev/null 2>&1 || true
        printf 'near curl file and far pid/log removed\n'

        printf -- '--- rmmod ---\n'
        if ((RMMOD_REQUESTED && !KEEP)); then
            local host
            for host in near far; do
                if qa_ssh "$host" "lsmod | grep -q '^tbtun '"; then
                    qa_ssh "$host" "rmmod tbtun 2>&1 || true" || true
                    printf '%s: rmmod requested\n' "$host"
                else
                    printf '%s: tbtun not loaded\n' "$host"
                fi
            done
        elif ((RMMOD_REQUESTED && KEEP)); then
            printf 'rmmod skipped by --keep\n'
        else
            printf 'rmmod not requested\n'
        fi
    } >>"$out" 2>&1

    # Residuals: each check names a state this tool can create.
    RESIDUAL=0
    if ! qa_ssh far true; then
        printf 'RESIDUAL far unreachable; rule state unknown\n' >>"$out"
        RESIDUAL=1
    elif have_iptables && rst_rule_present; then
        printf 'RESIDUAL far RST rule still present\n' >>"$out"
        RESIDUAL=1
    fi
    if ! qa_ssh near true; then
        printf 'RESIDUAL near unreachable; curl state unknown\n' >>"$out"
        RESIDUAL=1
    else
        local i
        for i in 1 2 3 4 5 6 7 8 9 10; do
            qa_ssh near "pgrep -f 'curl.*127[.]0[.]0[.]1:$NEAR_PORT' >/dev/null 2>&1" ||
                break
            sleep 0.2
        done
        if qa_ssh near "pgrep -f 'curl.*127[.]0[.]0[.]1:$NEAR_PORT' >/dev/null 2>&1"; then
            printf 'RESIDUAL near curl still running\n' >>"$out"
            RESIDUAL=1
        fi
    fi
    if qa_ssh far "ss -Hltn 'sport = :$FAR_PORT' 2>/dev/null | grep -q ."; then
        printf 'RESIDUAL far port %s still listening\n' "$FAR_PORT" >>"$out"
        RESIDUAL=1
    fi
    if ((RMMOD_REQUESTED && !KEEP)); then
        local host count
        for host in near far; do
            count=$(qa_ssh "$host" "lsmod | grep -c '^tbtun ' || true" 2>/dev/null || true)
            case ${count:-?} in
                0) ;;
                *)
                    printf 'RESIDUAL %s tbtun still loaded (lsmod count=%s)\n' \
                        "$host" "${count:-?}" >>"$out"
                    RESIDUAL=1
                    ;;
            esac
        done
    fi
    printf 'RESIDUAL=%s\n' "$RESIDUAL" >>"$out"
    cat "$out" >&2 2>/dev/null || true
    return 0
}

field_delta() { # near|far class field
    local b f
    b=$(qa_field "$EV/$1-stats-run-pre.txt" "$2" "$3")
    f=$(qa_field "$EV/$1-stats-final.txt" "$2" "$3")
    qa_delta "${b:-}" "${f:-}"
}

write_summary() { # verdict code
    local verdict=$1 code=$2
    ((SUMMARY_WRITTEN)) && return 0
    SUMMARY_WRITTEN=1
    [[ -n ${EV:-} ]] || return 0
    local field host near_d far_d kill
    {
        printf 'fault-inject 0.3.3\n\n'
        printf 'verdict: %s (exit %s)\n' "$verdict" "$code"
        printf 'start: %s\n' "${START_UTC:-?}"
        printf 'end: %s\n' "${END_UTC:-?}"
        printf 'near: %s\nfar: %s\nevidence: %s\n\n' \
            "$NEAR_HOST" "$FAR_HOST" "${EV:-?}"
        printf 'config: cycles=%s gap=%s rst=%s rmmod=%s keep=%s stream_seconds=%s\n\n' \
            "$CYCLES" "$GAP" "$DO_RST" "$RMMOD_REQUESTED" "$KEEP" "$STREAM_SECONDS"

        printf 'kills:\n'
        if ((${#KILL_LOG[@]})); then
            for kill in "${KILL_LOG[@]}"; do
                printf '  %s\n' "$kill"
            done
        else
            printf '  none\n'
        fi
        printf '\nup recovery: %s s (up_min=%s)\n\n' \
            "$RECOVER_SECONDS" "${UP_MIN:-?}"

        printf 'counter deltas (run pre -> final):\n'
        printf '  %-24s %10s %10s\n' 'field' 'near' 'far'
        for field in \
            'c3 stream_reset_tcp_failed' \
            'c11 fec_reset_tcp_failed' \
            'c11 fec_reset_hole' \
            'c6 holes_unrecoverable' \
            'c10 fec_deliver_bad' \
            'c9 fec_proto_fail' \
            'c9 fec_hdr_crc_fail' \
            'c9 fec_payload_crc_fail' \
            'c3 data_tx' \
            'c3 retain_drop' \
            'c3 stall_drop' \
            'c11 fec_outq_drop' \
            'c11 fec_replay_drop' \
            'c9 fec_drop_window' \
            'c3 connect_fail' \
            'c3 connect_retry' \
            'c3 lost' \
            'c3 replaced'; do
            set -- $field
            near_d=$(field_delta near "$1" "$2")
            far_d=$(field_delta far "$1" "$2")
            printf '  %-24s %10s %10s\n' "$2" "$near_d" "$far_d"
        done

        printf '\nnear rx_delivered delta: %s (downlink)\n' "${DATA_TX:-?}"
        printf 'curl: %s\n' "${CURL_LINE:-unavailable}"
        printf 'curl size: %s http: %s\n' "$CURL_SIZE" "$CURL_HTTP"
        printf 'kernel window: near=%s far=%s\n' "$KW_NEAR_STATE" "$KW_FAR_STATE"
        printf 'cleanup residual: %s (see cleanup.txt)\n' "$RESIDUAL"
        if ((RMMOD_REQUESTED)); then
            for host in near far; do
                printf 'module %s: %s\n' "$host" \
                    "$(qa_ssh "$host" "lsmod | grep -c '^tbtun ' || true" 2>/dev/null || echo '?')"
            done
        fi

        printf '\nchecks:\n'
        grep -E '^CHECK ' "$EV/run.log" 2>/dev/null || true
    } >"$EV/SUMMARY.txt"
}

on_exit() {
    local rc=$?
    trap - EXIT INT TERM
    if ((!SUMMARY_WRITTEN)); then
        cleanup_all
        ((RESIDUAL && rc == 0)) && rc=9
        write_summary INTERRUPTED "$rc"
    fi
    exit "$rc"
}

install_traps() {
    trap 'exit 130' INT
    trap 'exit 143' TERM
    trap on_exit EXIT
}

finish() { # code
    local code=$1 verdict=PASS
    END_UTC=${END_UTC:-$(qa_now)}
    ((code != 0 || FAILED > 0)) && verdict=FAIL
    cleanup_all
    ((RESIDUAL && code == 0)) && code=9
    write_summary "$verdict" "$code"
    exit "$code"
}

# ---------------------------------------------------------------------------
# evidence and helpers

qa_evidence_init fault-0.3.3 || exit 3
install_traps

if [[ $MODE == cleanup ]]; then
    CLEANUP_MODE=1
    qa_say "evidence: $EV"
    cleanup_all
    code=0
    ((RESIDUAL)) && code=9
    write_summary CLEANUP "$code"
    exit "$code"
fi

START_UTC=$(qa_now)
qa_mark run-start

fld() { # near|far class field
    local v
    v=$(qa_field "$EV/$1-live-stats.txt" "$2" "$3")
    printf '%s' "${v:-?}"
}

csv_text() {
    printf '%s' "$1" | tr ',\n\r' ';  '
}

csv_row() { # cycle phase note
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$(qa_now)" "$1" "$2" \
        "$(fld near c3 up)" "$(fld far c3 up)" \
        "$(fld near c3 replacing)" "$(fld far c3 replacing)" \
        "$(fld near c3 empty)" "$(fld far c3 empty)" \
        "$(fld near c3 stream_reset_tcp_failed)" \
        "$(fld far c3 stream_reset_tcp_failed)" \
        "$(fld near c6 holes_unrecoverable)" \
        "$(fld far c6 holes_unrecoverable)" \
        "$(fld near c10 fec_deliver_bad)" \
        "$(fld far c10 fec_deliver_bad)" \
        "$(fld near c3 data_tx)" "$(fld far c3 data_tx)" \
        "$(csv_text "${3:-}")" >>"$EV/fault.csv"
}

check_invariant() { # phase
    local host n up rep emp strict=0
    # A replacement window is a normal transient: up drops, replacing/empty
    # rises, and the slot sum can lag one sample behind. Judge the sum only
    # at rest; judge up==n after recovery, not inside the window.
    case $1 in
        pre | post | final) strict=1 ;;
    esac
    for host in near far; do
        n=$(qa_field "$EV/$host-live-stats.txt" c3 n)
        up=$(qa_field "$EV/$host-live-stats.txt" c3 up)
        rep=$(qa_field "$EV/$host-live-stats.txt" c3 replacing)
        emp=$(qa_field "$EV/$host-live-stats.txt" c3 empty)
        if qa_num "$n" && qa_num "$up" && qa_num "$rep" && qa_num "$emp"; then
            if [[ -z $UP_MIN ]] || ((up < UP_MIN)); then
                UP_MIN=$up
                UP_MIN_DETAIL="$1 $host up=$up n=$n replacing=$rep empty=$emp"
            fi
            if ((up + rep + emp != n)); then
                if ((strict)); then
                    INVARIANT_BAD=1
                    INVARIANT_DETAIL="$1 $host n=$n up=$up replacing=$rep empty=$emp"
                else
                    TRANSIENT_NOTE="$1 $host n=$n up=$up replacing=$rep empty=$emp"
                fi
            fi
        fi
    done
}

sample_stats() { # cycle phase [note]
    qa_snap_stats near "$EV/near-live-stats.txt"
    qa_snap_stats far "$EV/far-live-stats.txt"
    check_invariant "$2"
    csv_row "$1" "$2" "${3:-}"
}

dstate_sample() {
    local host line key
    DSTATE_ROW_NOTE=
    for host in near far; do
        qa_dstate_pids "$host" >"$EV/$host-dstate-live.txt"
        qa_dstate_delta "$EV/$host-dstate-pre.txt" "$EV/$host-dstate-live.txt" \
            >"$EV/$host-dstate-new.txt"
        while IFS= read -r line; do
            [[ -n $line ]] || continue
            if [[ -n $DSTATE_WHITELIST && $line =~ $DSTATE_WHITELIST ]]; then
                continue
            fi
            key="$host:$line"
            DSTATE_HITS[$key]=$(( ${DSTATE_HITS[$key]:-0} + 1 ))
            if ((${DSTATE_HITS[$key]} >= 3)); then
                DSTATE_SUSTAINED=1
                DSTATE_SAMPLE=$key
            fi
            DSTATE_ROW_NOTE+="${DSTATE_ROW_NOTE:+; }dstate $key x${DSTATE_HITS[$key]}"
        done <"$EV/$host-dstate-new.txt"
    done

    if ((${#DSTATE_HITS[@]})); then
        for key in "${!DSTATE_HITS[@]}"; do
            host=${key%%:*}
            line=${key#*:}
            if ! grep -Fxq -- "$line" "$EV/$host-dstate-new.txt" 2>/dev/null; then
                unset "DSTATE_HITS[$key]"
            fi
        done
    fi
}

sample_all() { # cycle phase [note]
    qa_snap_stats near "$EV/near-live-stats.txt"
    qa_snap_stats far "$EV/far-live-stats.txt"
    check_invariant "$2"
    dstate_sample
    csv_row "$1" "$2" "${3:-}${3:+; }${DSTATE_ROW_NOTE}"
}

# ---------------------------------------------------------------------------
# run: preflight

qa_say "evidence: $EV"
qa_say "== 1. preflight"
if ! qa_preflight; then
    qa_capture_diag preflight
    qa_note "preflight failed"
    finish 3
fi
qa_mark preflight-done

# ---------------------------------------------------------------------------
# run: far HTTP source

qa_say "== 2. far source"
if qa_ssh far "ss -Hltn 'sport = :$FAR_PORT' 2>/dev/null | grep -q ."; then
    qa_check far_port_free 1 "127.0.0.1:$FAR_PORT already listening"
    finish 4
fi
qa_check far_port_free 0 "127.0.0.1:$FAR_PORT free"

qa_ssh far "mkdir -p '$REMOTE_QA'" >/dev/null 2>&1 || true
if ! timeout "$SSH_TMO" scp -q -o BatchMode=yes "$SELF_DIR/stream-server.py" \
    "$FAR_HOST:$REMOTE_QA/stream-server.py"; then
    qa_check far_source_deploy 1 "scp to $REMOTE_QA failed"
    finish 4
fi
qa_check far_source_deploy 0 "$REMOTE_QA/stream-server.py"

qa_ssh far "REMOTE_QA='$REMOTE_QA' PY='$PYTHON' PORT='$FAR_PORT' bash -s" \
    >/dev/null 2>&1 <<'EOS' || true
set -u
cd "$REMOTE_QA" || exit 1
rm -f stream-server.log stream-server.pid
setsid bash -c 'echo $$ > stream-server.pid; exec "$0" stream-server.py --host 127.0.0.1 --port "$1" --seconds 900' \
    "$PY" "$PORT" </dev/null >stream-server.log 2>&1 &
EOS

SOURCE_OK=0
SOURCE_DEADLINE=$(( $(date +%s) + 10 ))
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if qa_ssh far "grep -q 'LISTEN 127.0.0.1:$FAR_PORT' '$REMOTE_QA/stream-server.log' 2>/dev/null" &&
        qa_ssh far "[ -s '$REMOTE_QA/stream-server.pid' ] && kill -0 \$(cat '$REMOTE_QA/stream-server.pid') 2>/dev/null"; then
        SOURCE_OK=1
        break
    fi
    if (($(date +%s) >= SOURCE_DEADLINE)); then
        break
    fi
    sleep 0.5
done
if ((!SOURCE_OK)); then
    qa_ssh far "tail -n 20 '$REMOTE_QA/stream-server.log' 2>/dev/null" \
        >"$EV/far-stream-server-start.log" 2>/dev/null || true
    qa_check far_source_listen 1 "no LISTEN in 10 s"
    finish 4
fi
qa_check far_source_listen 0 "127.0.0.1:$FAR_PORT"
qa_mark far-source-up

# ---------------------------------------------------------------------------
# run: stream

qa_say "== 3. stream start (${STREAM_SECONDS}s)"
if ! qa_stream_curl_start near "$EV/near-curl.txt" "$STREAM_SECONDS"; then
    qa_check stream_start 1 "curl start failed"
    finish 5
fi

CURL_OK=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if qa_ssh near "pgrep -f 'curl.*127[.]0[.]0[.]1:$NEAR_PORT' >/dev/null 2>&1"; then
        CURL_OK=1
        break
    fi
    sleep 0.5
done
if ((!CURL_OK)); then
    qa_check stream_start 1 "curl process not found"
    finish 5
fi
qa_check stream_start 0 "near curl pid found"
CURL_START_EPOCH=$(date +%s)

qa_snap_stats near "$EV/near-stats-run-pre.txt"
qa_snap_stats far "$EV/far-stats-run-pre.txt"
if [[ ! -s $EV/near-stats-run-pre.txt || ! -s $EV/far-stats-run-pre.txt ]]; then
    qa_check stats_run_pre 1 "empty pre-run stats"
    STREAM_BAD=1
    finish 5
fi
qa_check stats_run_pre 0 "both ends"
START_EPOCH=$(date +%s)
printf '%s\n' "$CSV_HEADER" >"$EV/fault.csv"
sample_all 0 pre
qa_mark stream-start

# ---------------------------------------------------------------------------
# run: injection phase A

qa_say "== 4. carrier kills (cycles=$CYCLES gap=$GAP)"
readarray -t CONN_LIST < <(qa_carrier_conns | awk 'NF')
CONN_COUNT=${#CONN_LIST[@]}
if ((CONN_COUNT < CYCLES + 1)); then
    qa_check carrier_pool 1 "need >= $((CYCLES + 1)) carriers, found $CONN_COUNT"
    finish 6
fi
qa_check carrier_pool 0 "found $CONN_COUNT, need >= $((CYCLES + 1))"

KILL_NO=0
for conn in "${CONN_LIST[@]:0:CYCLES}"; do
    read -r lp rp <<<"$conn"
    KILL_NO=$((KILL_NO + 1))
    KILL_LOG+=("cycle=$KILL_NO lport=$lp rport=$rp")

    qa_snap_stats near "$EV/kill-$KILL_NO-near-before.txt"
    qa_snap_stats far "$EV/kill-$KILL_NO-far-before.txt"
    sample_all "$KILL_NO" kill-before "carrier=$lp-$rp"

    if ! qa_carrier_kill "$lp" "$rp"; then
        qa_check "kill_$KILL_NO" 1 "ss -K $lp->$rp failed"
        finish 6
    fi
    qa_check "kill_$KILL_NO" 0 "$lp->$rp"
    sample_all "$KILL_NO" kill-after "carrier=$lp-$rp"

    # ss -K kills one near carrier socket. The carrier object stays in the
    # pool, so lost/replaced do not move. The observable repair is a stall
    # declaration/recovery or a dial retry. Judge those; keep lost/replaced
    # as information only.
    st_b=$(qa_field "$EV/kill-$KILL_NO-near-before.txt" c3 stall_declared)
    sr_b=$(qa_field "$EV/kill-$KILL_NO-near-before.txt" c3 stall_recovered)
    cr_b=$(qa_field "$EV/kill-$KILL_NO-near-before.txt" c3 connect_retry)
    st_a=$st_b
    sr_a=$sr_b
    cr_a=$cr_b
    grew=0
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        sample_stats "$KILL_NO" kill-wait \
            "stall_declared=$st_b stall_recovered=$sr_b connect_retry=$cr_b"
        st_a=$(qa_field "$EV/near-live-stats.txt" c3 stall_declared)
        sr_a=$(qa_field "$EV/near-live-stats.txt" c3 stall_recovered)
        cr_a=$(qa_field "$EV/near-live-stats.txt" c3 connect_retry)
        if qa_num "$st_b" && qa_num "$st_a" && ((st_a > st_b)); then
            grew=1
            break
        fi
        if qa_num "$sr_b" && qa_num "$sr_a" && ((sr_a > sr_b)); then
            grew=1
            break
        fi
        if qa_num "$cr_b" && qa_num "$cr_a" && ((cr_a > cr_b)); then
            grew=1
            break
        fi
        sleep 0.5
    done
    qa_check "kill_${KILL_NO}_reconnect" "$([ "$grew" -eq 1 ]; echo $?)" \
        "stall_declared=$st_b->$st_a stall_recovered=$sr_b->$sr_a connect_retry=$cr_b->$cr_a"

    qa_snap_stats near "$EV/kill-$KILL_NO-near-after.txt"
    qa_snap_stats far "$EV/kill-$KILL_NO-far-after.txt"
    {
        printf '=== near before ===\n'
        qa_line "$EV/kill-$KILL_NO-near-before.txt" c3
        printf '=== near after ===\n'
        qa_line "$EV/kill-$KILL_NO-near-after.txt" c3
        printf '=== far before ===\n'
        qa_line "$EV/kill-$KILL_NO-far-before.txt" c3
        printf '=== far after ===\n'
        qa_line "$EV/kill-$KILL_NO-far-after.txt" c3
    } >"$EV/kill-$KILL_NO-c3.txt"

    if ((KILL_NO < CYCLES)); then
        sleep "$GAP"
    fi
done
qa_note "up_min during the run: ${UP_MIN:-?} (${UP_MIN_DETAIL:-no sample})"
qa_mark kills-done

# ---------------------------------------------------------------------------
# run: RST phase
#
# The RST rule resets every carrier packet, so this phase runs after the
# kill loop. The kill samples then see only the single-carrier fault.

if ((DO_RST)); then
    qa_say "== 5. RST window (2 s)"
    if ! have_iptables; then
        qa_skip rst_injection "iptables absent on far"
    elif rst_rule_present; then
        qa_check rst_preexisting 1 "exact rule already present; left untouched"
    else
        qa_check rst_preexisting 0 "no exact rule before injection"
        cf0=$(qa_field "$EV/near-live-stats.txt" c3 connect_fail)
        cr0=$(qa_field "$EV/near-live-stats.txt" c3 connect_retry)
        RST_ADDED=1
        if ! qa_rst_add; then
            qa_check rst_add 1 "iptables -I failed"
            finish 6
        fi
        qa_check rst_add 0 "$CARRIER_DPORT"
        sample_stats 0 rst-add "connect_fail=$cf0 connect_retry=$cr0"
        sleep 2
        if ! qa_rst_del; then
            qa_check rst_del 1 "iptables -D failed; cleanup will retry"
            finish 6
        fi
        RST_ADDED=0
        if rst_rule_present; then
            qa_check rst_removed 1 "rule still present after -D"
        else
            qa_check rst_removed 0 "rule absent after -D"
        fi
        sample_stats 0 rst-del "window ended"
        cf1=$(qa_field "$EV/near-live-stats.txt" c3 connect_fail)
        cr1=$(qa_field "$EV/near-live-stats.txt" c3 connect_retry)
        qa_note "RST info: near connect_fail $cf0 -> $cf1, connect_retry $cr0 -> $cr1"
    fi
    qa_mark rst-done
else
    qa_skip rst_injection "--no-rst"
fi

# ---------------------------------------------------------------------------
# run: recovery

qa_say "== 6. recovery (up==n, max 60 s)"
RECOVER_START=$(date +%s)
RECOVERED=0
RECOVER_UP=?
RECOVER_N=?
while :; do
    sample_all 0 recover
    RECOVER_UP=$(qa_field "$EV/near-live-stats.txt" c3 up)
    RECOVER_N=$(qa_field "$EV/near-live-stats.txt" c3 n)
    if qa_num "$RECOVER_UP" && qa_num "$RECOVER_N" &&
        ((RECOVER_UP == RECOVER_N)); then
        RECOVERED=1
        break
    fi
    if (($(date +%s) - RECOVER_START >= 60)); then
        break
    fi
    sleep 0.5
done
RECOVER_SECONDS=$(( $(date +%s) - RECOVER_START ))
qa_check up_recovered "$([ "$RECOVERED" -eq 1 ]; echo $?)" \
    "up=$RECOVER_UP n=$RECOVER_N in ${RECOVER_SECONDS}s (deadline 60)"
qa_mark recovery-done

# ---------------------------------------------------------------------------
# run: stream result

qa_say "== 7. stream result"
CURL_ALIVE=0
CURL_DEADLINE=$(( CURL_START_EPOCH + STREAM_SECONDS + 15 ))
while qa_ssh near "pgrep -f 'curl.*127[.]0[.]0[.]1:$NEAR_PORT' >/dev/null 2>&1"; do
    CURL_ALIVE=1
    if (($(date +%s) >= CURL_DEADLINE)); then
        break
    fi
    sleep 1
done
if ((CURL_ALIVE)) &&
    qa_ssh near "pgrep -f 'curl.*127[.]0[.]0[.]1:$NEAR_PORT' >/dev/null 2>&1"; then
    qa_check curl_window 1 "curl still running after ${STREAM_SECONDS}s + 15s"
    qa_stream_curl_stop
    STREAM_BAD=1
else
    qa_check curl_window 0 "finished inside ${STREAM_SECONDS}s + 15s"
fi

CURL_LINE=$(qa_stream_curl_result near "$EV/near-curl.txt" 2>/dev/null || true)
CURL_SIZE=$(printf '%s\n' "$CURL_LINE" |
    sed -n 's/.*size=\([0-9][0-9]*\).*/\1/p' | head -n 1)
CURL_HTTP=$(printf '%s\n' "$CURL_LINE" |
    sed -n 's/.*http=\([0-9][0-9]*\).*/\1/p' | head -n 1)
if qa_num "${CURL_SIZE:-}" && ((CURL_SIZE > 0)); then
    qa_check curl_size 0 "size=$CURL_SIZE http=${CURL_HTTP:-?}"
else
    qa_check curl_size 1 "size=${CURL_SIZE:-missing} line=${CURL_LINE:-empty}"
    STREAM_BAD=1
fi

qa_snap_stats near "$EV/near-stats-post.txt"
qa_snap_stats far "$EV/far-stats-post.txt"
sample_all 0 post "curl size=${CURL_SIZE:-?}"
sleep 2
sample_all 0 final
qa_snap_stats near "$EV/near-stats-final.txt"
qa_snap_stats far "$EV/far-stats-final.txt"
END_EPOCH=$(date +%s)
END_UTC=$(qa_now)

# ---------------------------------------------------------------------------
# run: kernel window and verdict

qa_say "== 8. kernel window"
qa_kernel_window near "$START_EPOCH" "$END_EPOCH" "$EV/near-kernel-window.txt"
qa_kernel_window far "$START_EPOCH" "$END_EPOCH" "$EV/far-kernel-window.txt"
for host in near far; do
    state=$(qa_kw_state "$EV/$host-kernel-window.txt")
    case $host in
        near) KW_NEAR_STATE=$state ;;
        far) KW_FAR_STATE=$state ;;
    esac
    case $state in
        PASS) qa_check "${host}_kernel_window" 0 "no BUG/Oops/WARNING" ;;
        FAIL) qa_check "${host}_kernel_window" 1 "BUG/Oops/WARNING in window" ;;
        *) qa_skip "${host}_kernel_window" "empty window" ;;
    esac
done

qa_say "== 9. verdict"
for host in near far; do
    n=$(qa_field "$EV/$host-stats-final.txt" c3 n)
    up=$(qa_field "$EV/$host-stats-final.txt" c3 up)
    rep=$(qa_field "$EV/$host-stats-final.txt" c3 replacing)
    emp=$(qa_field "$EV/$host-stats-final.txt" c3 empty)
    qa_check "final_${host}_up" "$([ -n "$n" ] && [ "$up" = "$n" ]; echo $?)" \
        "up=$up n=$n"
    qa_check "final_${host}_replacing" "$([ "$rep" = 0 ]; echo $?)" \
        "replacing=$rep"
    qa_check "final_${host}_empty" "$([ "$emp" = 0 ]; echo $?)" "empty=$emp"
done

zero_counter() { # name class field
    local dn df
    dn=$(field_delta near "$2" "$3")
    df=$(field_delta far "$2" "$3")
    qa_check "zero_$1" "$([ "$dn" = 0 ] && [ "$df" = 0 ]; echo $?)" \
        "near=$dn far=$df"
}

zero_counter stream_reset_tcp_failed c3 stream_reset_tcp_failed
zero_counter fec_reset_tcp_failed c11 fec_reset_tcp_failed
zero_counter fec_reset_hole c11 fec_reset_hole
zero_counter holes_unrecoverable c6 holes_unrecoverable
zero_counter fec_deliver_bad c10 fec_deliver_bad
zero_counter fec_proto_fail c9 fec_proto_fail
zero_counter fec_hdr_crc_fail c9 fec_hdr_crc_fail
zero_counter fec_payload_crc_fail c9 fec_payload_crc_fail

# The run is a far->near download. c3 data_tx is an uplink counter and stays
# 0 in both directions here; the delivered downlink bytes are the tunnel's
# rx_delivered line on near.
DATA_PRE=$(qa_field_any "$EV/near-stats-run-pre.txt" rx_delivered)
DATA_POST=$(qa_field_any "$EV/near-stats-final.txt" rx_delivered)
DATA_TX=$(qa_delta "${DATA_PRE:-}" "${DATA_POST:-}")
qa_check data_tx "$(qa_num "$DATA_TX" && [ "$DATA_TX" -gt 0 ]; echo $?)" \
    "near rx_delivered delta=$DATA_TX (downlink)"

qa_check c3_invariant_samples "$([ "$INVARIANT_BAD" -eq 0 ]; echo $?)" \
    "${INVARIANT_DETAIL:-no at-rest violation}${TRANSIENT_NOTE:+; transient: $TRANSIENT_NOTE}"
qa_check dstate_sustained "$([ "$DSTATE_SUSTAINED" -eq 0 ]; echo $?)" \
    "${DSTATE_SAMPLE:-no new D-state line in 3 consecutive samples}"

if have_iptables; then
    if rst_rule_present; then
        qa_check rst_absent_final 1 "rule still present"
    else
        qa_check rst_absent_final 0 "rule absent"
    fi
else
    qa_skip rst_absent_final "iptables absent on far"
fi

CODE=0
((STREAM_BAD)) && CODE=5
((CODE == 0 && FAILED > 0)) && CODE=7
finish "$CODE"
