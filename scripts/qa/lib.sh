# shellcheck shell=bash
# lib.sh - shared helpers for the 0.3.3 fault-injection and soak tools.
#
# Source this file. It defines defaults, ssh wrappers, procfs parsers,
# evidence capture, and preflight. It runs nothing and changes no remote
# state. The caller owns the modes, the verdict, and the cleanup.
#
# The remote layout is the one in review/wan-experiment/arm64:
#   /proc/net/tbtun/{config,tunnels,ports,peers,stats,http}
#   stats lines are "tunnel <id> c<N> <field>=<value> ..."
#   the tunnel entry is /proc/net/tbtun/stats, not /proc/tbtun.

QA_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
QA_REPO=$(cd -- "$QA_DIR/../.." && pwd)

NEAR_HOST=${NEAR_HOST:-root@192.168.123.82}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
FAR_IP=${FAR_IP:-45.11.1.62}
NEAR_PORT=${NEAR_PORT:-9001}
FAR_PORT=${FAR_PORT:-5210}
TUNNEL=${TUNNEL:-1}
SSH_TMO=${SSH_TMO:-20}
PROC_ROOT=${PROC_ROOT:-/proc/net/tbtun}
CARRIER_PORT_MIN=${CARRIER_PORT_MIN:-31000}
CARRIER_PORT_MAX=${CARRIER_PORT_MAX:-31511}
STREAM_SECONDS=${STREAM_SECONDS:-60}
EVIDENCE_ROOT=${EVIDENCE_ROOT:-$QA_REPO/review/wan-experiment/arm64}
REMOTE_QA=${REMOTE_QA:-/root/tbtun-qa}
PYTHON=${PYTHON:-python3}

# A caller must set FAILED=0 before the first qa_check. EV is the evidence
# directory; every function below tolerates EV being unset except
# qa_evidence_init, qa_capture_diag, and qa_preflight.

qa_now() { date -u +%Y-%m-%dT%H:%M:%SZ; }

qa_epoch() { date +%s; }

qa_say() { printf '%s\n' "$*"; }

qa_note() {
    printf '%s %s\n' "$(qa_now)" "$*" |
        tee -a "${EV:-/dev/null}/run.log" >&2
}

qa_mark() {
    printf '%s\t%s\n' "$(qa_now)" "$1" |
        tee -a "${EV:-/dev/null}/timeline.txt" >&2
}

qa_ssh() { # near|far command...
    local host=$1
    shift
    case $host in
        near) timeout "$SSH_TMO" ssh -o BatchMode=yes "$NEAR_HOST" "$@" ;;
        far) timeout "$SSH_TMO" ssh -o BatchMode=yes "$FAR_HOST" "$@" ;;
        *)
            printf 'qa_ssh: unknown host: %s\n' "$host" >&2
            return 2
            ;;
    esac
}

qa_ssh_long() { # seconds near|far command...
    local tmo=$1 host=$2
    shift 2
    case $host in
        near) timeout "$tmo" ssh -o BatchMode=yes "$NEAR_HOST" "$@" ;;
        far) timeout "$tmo" ssh -o BatchMode=yes "$FAR_HOST" "$@" ;;
        *)
            printf 'qa_ssh_long: unknown host: %s\n' "$host" >&2
            return 2
            ;;
    esac
}

qa_num() { case ${1:-} in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }

qa_delta() { # before after -> difference, or ? when either is not a number
    if qa_num "${1:-}" && qa_num "${2:-}"; then
        printf '%s' "$(( $2 - $1 ))"
    else
        printf '?'
    fi
}

qa_field() { # stats-file class field [tunnel]
    awk -v c="$2" -v f="$3=" -v t="${4:-$TUNNEL}" '
        $1 == "tunnel" && $2 == t && $3 == c {
            for (i = 1; i <= NF; i++)
                if (index($i, f) == 1) {
                    print substr($i, length(f) + 1)
                    exit
                }
        }' "$1" 2>/dev/null
}

qa_line() { # stats-file class [tunnel]
    awk -v c="$2" -v t="${3:-$TUNNEL}" \
        '$1 == "tunnel" && $2 == t && $3 == c { print; exit }' "$1" 2>/dev/null
}

qa_field_any() { # stats-file field: first tunnel that carries it
    awk -v f="$2=" '
        $1 == "tunnel" {
            for (i = 1; i <= NF; i++)
                if (index($i, f) == 1) {
                    print substr($i, length(f) + 1)
                    exit
                }
        }' "$1" 2>/dev/null
}

qa_evidence_init() { # prefix
    EV=$EVIDENCE_ROOT/$1-$(date -u +%Y%m%dT%H%M%SZ)
    mkdir -p "$EV" || return 1
    : >"$EV/run.log"
    : >"$EV/timeline.txt"
    printf 'EVIDENCE=%s\n' "$EV"
}

qa_check() { # name rc [detail]
    local name=$1 rc=${2:-1} detail=${3:-} line
    qa_num "$rc" || rc=1
    if ((rc == 0)); then
        line="CHECK $name PASS${detail:+ ($detail)}"
    else
        line="CHECK $name FAIL${detail:+ ($detail)}"
        FAILED=$((${FAILED:-0} + 1))
    fi
    qa_say "$line"
    [[ -n ${EV:-} ]] && printf '%s\n' "$line" >>"$EV/run.log"
    return 0
}

qa_skip() { # name [detail]
    local line="CHECK $1 SKIP${2:+ ($2)}"
    qa_say "$line"
    [[ -n ${EV:-} ]] && printf '%s\n' "$line" >>"$EV/run.log"
    return 0
}

qa_snap_stats() { # near|far outfile
    qa_ssh "$1" "cat '$PROC_ROOT/stats'" >"$2" 2>/dev/null || true
}

qa_snap_config() { # near|far outfile
    qa_ssh "$1" "cat '$PROC_ROOT/config'" >"$2" 2>/dev/null || true
}

qa_snap_tunnels() { # near|far outfile
    qa_ssh "$1" "cat '$PROC_ROOT/tunnels'" >"$2" 2>/dev/null || true
}

qa_snap_mem() { # near|far outfile
    qa_ssh "$1" 'cat /proc/meminfo 2>/dev/null; echo ---slab---;
        grep -E "^tbt_(carrier|session|stream)" /proc/slabinfo 2>/dev/null;
        echo ---ps---;
        ps -eo pid,rss,stat,comm --sort=-rss 2>/dev/null | head -n 20' \
        >"$2" 2>/dev/null || true
}

qa_dstate_pids() { # near|far
    qa_ssh "$1" "ps -eo stat=,pid=,comm= 2>/dev/null | awk '\$1 ~ /^D/' |
        sed 's/^[[:space:]]*//'" 2>/dev/null || true
}

# Kernel window: journalctl by @epoch, then dmesg by monotonic seconds. The
# first line is KW_SOURCE; an empty body is SKIP, never PASS. This is the
# same method as sclc-verify/verify-0.3.0.sh so the two tools agree.
qa_kernel_window() { # near|far since until outfile
    local host=$1 out=$4
    qa_ssh "$host" "KW_SINCE=$2 KW_UNTIL=$3 bash -s" >"$out" 2>/dev/null <<'EOS' || true
set -u
src=none
body=
jr=0
if command -v journalctl >/dev/null 2>&1 &&
    journalctl -k -b 0 -n 1 --no-pager 2>/dev/null | grep -qv '^-- No entries --$'; then
    jr=1
fi
if [ "$jr" -eq 1 ]; then
    src=journalctl
    raw=$(journalctl -k -b 0 --no-pager --since "@$KW_SINCE" --until "@$KW_UNTIL" 2>/dev/null || true)
    case $raw in '' | *'-- No entries --'*) raw= ;; esac
    body=$(printf '%s\n' "$raw" | grep -v '^-- Logs begin' || true)
fi
if [ -z "$body" ] && command -v dmesg >/dev/null 2>&1; then
    src=dmesg
    body=$(dmesg 2>/dev/null | awk -v s="$KW_SINCE" -v u="$KW_UNTIL" -v now="$(date +%s)" '
        /^\[[ 0-9]*\.[0-9]+\]/ {
            m = $0; sub(/^\[/, "", m); sub(/\].*/, "", m)
            mono = m + 0
            wall = now - (uptime_seconds - mono)
            if (wall >= s && wall <= u) print
        }' uptime_seconds="$(cut -d' ' -f1 /proc/uptime)" 2>/dev/null || true)
fi
printf 'KW_SOURCE=%s\n' "$src"
printf '%s\n' "$body"
EOS
}

qa_kw_source() { sed -n 's/^KW_SOURCE=//p' "$1" 2>/dev/null | head -n 1; }

qa_kw_body() { sed '1d' "$1" 2>/dev/null; }

qa_kw_state() { # file -> PASS|FAIL|SKIP
    local body
    body=$(qa_kw_body "$1")
    if [[ -z ${body//[[:space:]]/} ]]; then
        printf 'SKIP'
    elif grep -qE 'BUG:|Oops|WARNING:|UBSAN:|general protection fault' \
        <<<"$body"; then
        printf 'FAIL'
    else
        printf 'PASS'
    fi
}

qa_capture_diag() { # stage
    local stage=$1 host
    for host in near far; do
        qa_snap_stats "$host" "$EV/diag-$stage-$host-stats.txt"
        qa_snap_config "$host" "$EV/diag-$stage-$host-config.txt"
        qa_snap_tunnels "$host" "$EV/diag-$stage-$host-tunnels.txt"
        qa_snap_mem "$host" "$EV/diag-$stage-$host-mem.txt"
        qa_ssh "$host" 'dmesg 2>/dev/null | tail -n 800' \
            >"$EV/diag-$stage-$host-dmesg.txt" 2>/dev/null || true
        qa_ssh "$host" 'ps -eo stat,pid,comm 2>/dev/null' \
            >"$EV/diag-$stage-$host-ps.txt" 2>/dev/null || true
    done
    qa_note "diagnostics captured: stage=$stage"
}

# Preflight: both hosts reachable, the module loaded, one tunnel, and the
# carrier invariant exact. It records the baseline snapshots and D-state set.
qa_preflight() {
    local host n up rep emp
    local before=${FAILED:-0}

    if ! qa_ssh near true; then
        qa_check near_ssh 1 unreachable
        return 1
    fi
    qa_check near_ssh 0 reachable
    if ! qa_ssh far true; then
        qa_check far_ssh 1 unreachable
        return 1
    fi
    qa_check far_ssh 0 reachable

    for host in near far; do
        qa_check "${host}_loaded" \
            "$(qa_ssh "$host" 'lsmod | grep -q "^tbtun "' >/dev/null 2>&1; echo $?)" \
            tbtun
        qa_snap_config "$host" "$EV/$host-config.txt"
        qa_snap_tunnels "$host" "$EV/$host-tunnels.txt"
        qa_snap_stats "$host" "$EV/$host-stats-pre.txt"
        qa_dstate_pids "$host" >"$EV/$host-dstate-pre.txt"
        qa_check "${host}_tunnel" \
            "$([ -n "$(qa_line "$EV/$host-stats-pre.txt" c3)" ]; echo $?)" \
            "tunnel $TUNNEL c3"
    done

    for host in near far; do
        n=$(qa_field "$EV/$host-stats-pre.txt" c3 n)
        up=$(qa_field "$EV/$host-stats-pre.txt" c3 up)
        rep=$(qa_field "$EV/$host-stats-pre.txt" c3 replacing)
        emp=$(qa_field "$EV/$host-stats-pre.txt" c3 empty)
        qa_check "${host}_c3_invariant" \
            "$(qa_num "$n" && qa_num "$up" && qa_num "$rep" && qa_num "$emp" &&
                [ $((up + rep + emp)) -eq "$n" ]; echo $?)" \
            "n=$n up=$up replacing=$rep empty=$emp"
        qa_check "${host}_c3_up" \
            "$([ -n "${n:-}" ] && [ "${up:-}" = "$n" ]; echo $?)" \
            "up=$up n=$n"
    done

    n=$(qa_field "$EV/near-stats-pre.txt" c3 n)
    qa_check carrier_count_ge4 \
        "$(qa_num "$n" && [ "$n" -ge 4 ]; echo $?)" "n=${n:-?}"

    ((FAILED == before))
}

qa_carrier_conns() { # near-side established carrier TCP: "lport rport"
    qa_ssh near "ss -Htn state established dst $FAR_IP 2>/dev/null" 2>/dev/null |
        awk -v lo="$CARRIER_PORT_MIN" -v hi="$CARRIER_PORT_MAX" '
            NF >= 5 {
                lp = $4; rp = $5
                sub(/.*:/, "", lp)
                sub(/.*:/, "", rp)
                if (lp ~ /^[0-9]+$/ && rp ~ /^[0-9]+$/ &&
                    rp >= lo && rp <= hi)
                    print lp, rp
            }'
}

qa_carrier_kill() { # lport rport - tcp_abort on the near side
    qa_ssh near "ss -K 'sport = :$1' 'dport = :$2' >/dev/null 2>&1" &&
        return 0
    qa_ssh near "ss -K 'dport = :$2' >/dev/null 2>&1" && return 0
    return 1
}

qa_rst_add() { # far-side RST rejection for the carrier port block
    qa_ssh far "iptables -w 5 -I INPUT -p tcp --dport $CARRIER_PORT_MIN:$CARRIER_PORT_MAX -j REJECT --reject-with tcp-reset" \
        >/dev/null 2>&1
}

qa_rst_del() {
    qa_ssh far "iptables -w 5 -D INPUT -p tcp --dport $CARRIER_PORT_MIN:$CARRIER_PORT_MAX -j REJECT --reject-with tcp-reset" \
        >/dev/null 2>&1
}

qa_dstate_delta() { # baseline current - lines in current but not baseline
    awk 'NR == FNR { seen[$0] = 1; next } !($0 in seen)' "$1" "$2" 2>/dev/null
}

QA_CURL_REMOTE=

qa_stream_curl_start() { # near evidence-outfile seconds
    local evout=$2 secs=$3 rfile

    rfile="$REMOTE_QA/$(basename -- "$evout")"
    QA_CURL_REMOTE=$rfile
    qa_ssh near "mkdir -p '$REMOTE_QA'; rm -f '$rfile'; \
        setsid curl -sS -m '$secs' -o /dev/null \
        -w 'http=%{http_code} size=%{size_download} time=%{time_total} speed=%{speed_download}\n' \
        'http://127.0.0.1:$NEAR_PORT/' </dev/null >'$rfile' 2>&1 &" ||
        return 1
}

qa_stream_curl_stop() {
    qa_ssh near "pkill -f 'curl.*127[.]0[.]0[.]1:$NEAR_PORT' 2>/dev/null || true" \
        >/dev/null 2>&1 || true
}

qa_stream_curl_result() { # near evidence-outfile -> prints and stores the -w line
    local evout=$2 rfile

    rfile="$REMOTE_QA/$(basename -- "$evout")"
    qa_ssh near "cat '$rfile' 2>/dev/null" 2>/dev/null | tee "$evout" || true
}
