#!/bin/sh
# near-fetch.sh - the 1 GiB download step, run on the near host (.65).
#
# The near side only fetches. The module is already loaded and configured by
# run-1g.sh (256 carriers, n=4 m=4, grouped, w=8, crypto=on, http-kal); this
# script verifies that shape, waits for the join gate, downloads once, and
# judges the local evidence. It never retries, never reloads the module, and
# never reconfigures the tunnel.
#
# The hardware watchdog feeder must already be running: stopping it starts a
# 44 s reset countdown (MAGICCLOSE=0). This script only reads that state.
#
# The download is one curl and --max-time is its only deadline. There is no
# outer timeout wrapper: a harness timeout must never truncate the transfer
# and pass its own exit code off as the near-side verdict.
#
# Carrier path: the primary fix is the tunnel's dev= key (SO_BINDTODEVICE on
# the near carriers). NEAR_DEV defaults to eth0; this script requires
# dev=$NEAR_DEV in the live config (set NEAR_DEV= to disable). The pref-100
# ip rule below is second insurance, not the primary fix: it keeps
# `ip route get <far>` on eth0 while mihomo owns the default path.
#
# Modes
#   --preflight   checks: tools, bypass rule, config, listen, feeder, join gate
#   --run         preflight, one curl, local checks, evidence, verdict
#
# Output markers (machine-judged by run-1g.sh):
#   NEAR_CHECK <name> PASS|FAIL|SKIP (detail)
#   NEAR_CURL_RC=<n>
#   NEAR_HTTP=<code>
#   NEAR_SIZE=<bytes>
#   NEAR_SHA256=<hex>
#   NEAR_ELAPSED=<seconds>
#   NEAR_SPEED=<bytes/s>
#   NEAR_RESULT PASS|FAIL
set -eu

MODE=preflight
URL=${NEAR_URL:-http://127.0.0.1:9001/accept-1g.bin}
DL_DIR=${DL_DIR:-/root/dl-1g}
FILE=${FILE:-accept-1g.bin}
EXPECT_BYTES=${EXPECT_BYTES:-1073741824}
FAR_HASH=${FAR_HASH:-}
FAR_IP=${FAR_IP:-45.11.1.62}
NEAR_DEV=${NEAR_DEV:-eth0}
BYPASS_PREF=${BYPASS_PREF:-100}
BYPASS_MARK=${BYPASS_MARK:-/root/.tbtun-bypass-pref100}
TUNNEL=${TUNNEL:-1}
CARRIERS=${CARRIERS:-256}
JOIN_TIMEOUT=${JOIN_TIMEOUT:-180}
MAX_TIME=${MAX_TIME:-1800}
SPEED_LIMIT=${SPEED_LIMIT:-65536}
SPEED_TIME=${SPEED_TIME:-120}
DEAD_NOW_MAX=${DEAD_NOW_MAX:-64}
REQUIRE_SESS_REFS=${REQUIRE_SESS_REFS:-0}
EVIDENCE=${EVIDENCE:-$DL_DIR/evidence}

FAILED=0

say() { printf '%s\n' "$*"; }
check() {
    name=$1
    rc=${2:-1}
    detail=${3:-}
    # A failed command substitution under `set -e` can arrive empty; an empty
    # or non-numeric rc is a FAIL, never a PASS.
    case $rc in '' | *[!0-9]*) rc=1 ;; esac
    if [ "$rc" -eq 0 ]; then
        say "NEAR_CHECK $name PASS${detail:+ ($detail)}"
    else
        say "NEAR_CHECK $name FAIL${detail:+ ($detail)}"
        FAILED=$((FAILED + 1))
    fi
}
skip() { say "NEAR_CHECK $1 SKIP${2:+ ($2)}"; }
have() { command -v "$1" >/dev/null 2>&1; }
inc() { echo $(( $1 - $2 )); }

# mihomo bypass (second insurance). The rule is added only when absent; the
# marker records that this harness added it, so cleanup never deletes a
# pre-existing (mihomo) rule.
bypass_rule_exists() {
    ip rule show 2>/dev/null | awk -v p="$BYPASS_PREF:" -v ip="$FAR_IP" '
        $1 == p {
            for (i = 2; i < NF; i++)
                if ($i == "to" && ($(i + 1) == ip || $(i + 1) == ip "/32")) found = 1
        }
        END { exit(found ? 0 : 1) }'
}
bypass_ensure() {
    if bypass_rule_exists; then
        return 0
    fi
    ip rule add to "$FAR_IP/32" lookup main pref "$BYPASS_PREF" 2>/dev/null || return 1
    printf 'pref=%s to=%s/32 lookup=main\n' "$BYPASS_PREF" "$FAR_IP" >"$BYPASS_MARK" 2>/dev/null || true
    return 0
}
bypass_owned() {
    [ -f "$BYPASS_MARK" ] && grep -qF "to=$FAR_IP/32" "$BYPASS_MARK" 2>/dev/null
}
bypass_dev() {
    ip route get "$FAR_IP" 2>/dev/null | awk '
        { for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }'
}
bypass_dev_ok() {
    ddev=${1:-}
    if [ -z "$ddev" ] || [ "$ddev" = none ]; then return 1; fi
    case $ddev in
        Meta | tun* | utun* | wg* | tailscale*) return 1 ;;
    esac
    [ "$ddev" = eth0 ]
}
bypass_record() {
    btag=$1
    {
        printf '=== tag ===\n%s\n' "$btag"
        printf '=== ip rule show ===\n'
        ip rule show 2>&1 || true
        printf '=== ip route get %s ===\n' "$FAR_IP"
        ip route get "$FAR_IP" 2>&1 || true
        printf '=== dev ===\n%s\n' "$(bypass_dev)"
    } >"$EVIDENCE/near-ip-rule-$btag.txt"
}

for arg in "$@"; do
    case "$arg" in
        --preflight) MODE=preflight ;;
        --run) MODE=run ;;
        -h | --help)
            sed -n '2,35p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            say "near-fetch: unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

# ---------------------------------------------------------------------------
# stats parsing. One line per counter family:
#   c3  n=.. up=.. ... connect_fail=.. alloc_fail=.. join_bad=.. dead_now=..
#   c9  fec_proto_fail=.. fec_hdr_crc_fail=.. fec_payload_crc_fail=..
#   rx  ... rx_errs=.. ...
#   gaps_total=<n>   (session prefix line)
c3_field() {
    awk -v f="$2=" -v t="$TUNNEL" '
        $1 == "tunnel" && $2 == t && $3 == "c3" {
            for (i = 1; i <= NF; i++)
                if (index($i, f) == 1) { print substr($i, length(f) + 1); exit }
        }' "$1" 2>/dev/null
}
c9_field() {
    awk -v f="$2=" -v t="$TUNNEL" '
        $1 == "tunnel" && $2 == t && $3 == "c9" {
            for (i = 1; i <= NF; i++)
                if (index($i, f) == 1) { print substr($i, length(f) + 1); exit }
        }' "$1" 2>/dev/null
}
rx_field() {
    awk -v f="$2=" '
        / rx_ready_calls=/ {
            for (i = 1; i <= NF; i++)
                if (index($i, f) == 1) { print substr($i, length(f) + 1); exit }
        }' "$1" 2>/dev/null
}
gaps_field() {
    awk '/^gaps_total=/ { sub("gaps_total=", ""); print; exit }' "$1" 2>/dev/null
}
sess_refs_field() {
    awk -v f="sess_refs=" '
        {
            for (i = 1; i <= NF; i++)
                if (index($i, f) == 1) { print substr($i, length(f) + 1); exit }
        }' "$1" 2>/dev/null
}

cpu_busy() {
    awk 'NR == 1 {
        busy = $2 + $3 + $4 + $7 + $8 + $9
        idle = $5 + $6
        printf "%s %s\n", busy, idle
    }' /proc/stat
}

join_state() {
    up=$(c3_field /proc/net/tbtun/stats up 2>/dev/null || true)
    n=$(c3_field /proc/net/tbtun/stats n 2>/dev/null || true)
    printf '%s %s\n' "${up:-?}" "${n:-?}"
}

feeder_state() {
    # Read-only: no open() of the watchdog.
    for fddir in /proc/[0-9]*/fd; do
        if ls -l "$fddir" 2>/dev/null | grep -qF /dev/watchdog0; then
            pid=${fddir#/proc/}
            pid=${pid%/fd}
            printf 'pid=%s(%s)\n' "$pid" "$(cat "/proc/$pid/comm" 2>/dev/null || echo '?')"
            return 0
        fi
    done
    return 1
}

snapshot() {
    out=$1
    {
        printf '=== date ===\n'; date -u +%Y-%m-%dT%H:%M:%SZ
        printf '=== boot_id ===\n'; cat /proc/sys/kernel/random/boot_id 2>/dev/null || true
        printf '=== uptime ===\n'; cat /proc/uptime 2>/dev/null || true
        printf '=== loadavg ===\n'; cat /proc/loadavg 2>/dev/null || true
        printf '=== config ===\n'; cat /proc/net/tbtun/config 2>/dev/null || true
        printf '=== tunnels ===\n'; cat /proc/net/tbtun/tunnels 2>/dev/null || true
        printf '=== stats ===\n'; cat /proc/net/tbtun/stats 2>/dev/null || true
        printf '=== ss ===\n'; ss -ltnH 2>/dev/null || true
        printf '=== meminfo ===\n'; awk '/^(MemTotal|MemAvailable|MemFree):/{print}' /proc/meminfo 2>/dev/null || true
        printf '=== slab ===\n'; grep -E '^tbt_(carrier|session|stream)' /proc/slabinfo 2>/dev/null || true
    } >"$out"
}

# ---------------------------------------------------------------------------
# preflight

say "== 1. tools"
for t in curl sha256sum awk stat ss ip; do
    if have "$t"; then check "tool_$t" 0 "$t"; else check "tool_$t" 1 "$t missing"; fi
done

say "== 2. near bypass (mihomo isolation; pref $BYPASS_PREF to $FAR_IP/32 lookup main)"
mkdir -p "$DL_DIR" "$EVIDENCE"
bypass_ensure || true
bypass_record preflight
bdev=$(bypass_dev)
if bypass_rule_exists; then
    if bypass_owned; then brule=added; else brule=preexisting; fi
else
    brule=missing
fi
if bypass_dev_ok "$bdev"; then
    check bypass_route 0 "dev=$bdev rule=$brule to=$FAR_IP/32 pref=$BYPASS_PREF"
else
    check bypass_route 1 "dev=${bdev:-none} rule=$brule; need eth0, not Meta/tun*/utun*/wg*/tailscale*"
    say "NEAR_RESULT FAIL"
    if [ "$MODE" = run ]; then exit 4; else exit 3; fi
fi

say "== 3. module and config"
if [ -e /proc/net/tbtun/config ]; then
    check procfs 0 "/proc/net/tbtun/config"
    cfg=$(cat /proc/net/tbtun/config)
    say "   config: $cfg"
    for tok in \
        "carriers=$CARRIERS" "n=4" "m=4" "fec=on" "mode=grouped" "w=8" \
        "crypto=on" "profile=http-kal" "portblock=30000-30511"; do
        case " $cfg " in
            *" $tok "*) check "cfg_${tok%%=*}" 0 "$tok" ;;
            *) check "cfg_${tok%%=*}" 1 "missing token $tok" ;;
        esac
    done
    if [ -n "$NEAR_DEV" ]; then
        case " $cfg " in
            *" dev=$NEAR_DEV "*) check cfg_dev 0 "dev=$NEAR_DEV" ;;
            *) check cfg_dev 1 "missing dev=$NEAR_DEV" ;;
        esac
    else
        skip cfg_dev "NEAR_DEV unset; dev= not in the config (tbtun key pending)"
    fi
else
    check procfs 1 "/proc/net/tbtun/config missing (module not configured)"
    cfg=""
fi

say "== 4. listen 127.0.0.1:9001"
if ss -ltnH 2>/dev/null | grep -qF '127.0.0.1:9001'; then
    check listen 0 "127.0.0.1:9001"
else
    check listen 1 "127.0.0.1:9001 not listening"
fi

say "== 5. watchdog feeder (must stay up; stopping it resets the board)"
if holder=$(feeder_state); then
    check feeder 0 "$holder holds /dev/watchdog0"
else
    check feeder 1 "no process holds /dev/watchdog0"
fi

say "== 6. join gate (near side only; run-1g.sh checks far)"
i=0
up=?; n=?
while [ "$i" -lt "$JOIN_TIMEOUT" ]; do
    set -- $(join_state)
    up=$1
    n=$2
    if [ "$up" = "$CARRIERS" ] && [ "$n" = "$CARRIERS" ]; then
        break
    fi
    sleep 1
    i=$((i + 1))
done
check join_up "$([ "$up" = "$CARRIERS" ]; echo $?)" "up=$up expected=$CARRIERS after ${i}s"
check join_n "$([ "$n" = "$CARRIERS" ]; echo $?)" "n=$n expected=$CARRIERS"

if [ "$FAILED" -ne 0 ]; then
    say "NEAR_RESULT FAIL"
    if [ "$MODE" = run ]; then
        exit 4
    fi
    exit 3
fi

if [ "$MODE" = preflight ]; then
    say "NEAR_RESULT PASS"
    exit 0
fi

# ---------------------------------------------------------------------------
# second insurance, re-asserted immediately before the transfer

say "== 7. near bypass re-assert (before transfer)"
bypass_ensure || true
bypass_record pretransfer
bdev=$(bypass_dev)
if bypass_rule_exists; then
    if bypass_owned; then brule=added; else brule=preexisting; fi
else
    brule=missing
fi
if bypass_dev_ok "$bdev"; then
    check bypass_route_pretransfer 0 "dev=$bdev rule=$brule"
else
    check bypass_route_pretransfer 1 "dev=${bdev:-none} rule=$brule; refusing transfer"
    say "NEAR_RESULT FAIL"
    exit 4
fi

# ---------------------------------------------------------------------------
# one download, no retry

say "== 8. download"
mkdir -p "$DL_DIR" "$EVIDENCE"
rm -f "$DL_DIR/$FILE"
snapshot "$DL_DIR/near-stats-pre.txt"
dmesg_before=$(dmesg 2>/dev/null | wc -l || echo 0)
boot_before=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || true)
start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
start_epoch=$(date +%s)
set -- $(cpu_busy)
cpu_busy_before=$1
cpu_idle_before=$2

rc=0
# curl's --max-time is the single transfer authority. No outer timeout wrapper:
# a harness timeout would kill curl early and hide its verdict (run-1g.sh used
# to kill the ssh client at 20 s while this script kept running).
curl -sS --fail \
    --max-time "$MAX_TIME" \
    --speed-limit "$SPEED_LIMIT" --speed-time "$SPEED_TIME" \
    -o "$DL_DIR/$FILE" \
    -w 'http=%{http_code} size=%{size_download} time=%{time_total} speed=%{speed_download}\n' \
    "$URL" >"$DL_DIR/curl.txt" 2>"$DL_DIR/curl.err" || rc=$?

end_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
end_epoch=$(date +%s)
set -- $(cpu_busy)
cpu_busy_after=$1
cpu_idle_after=$2
snapshot "$DL_DIR/near-stats-post.txt"
boot_after=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || true)

say "NEAR_CURL_RC=$rc"
say "   curl: $(cat "$DL_DIR/curl.txt" 2>/dev/null || echo 'no -w line')"
if [ -s "$DL_DIR/curl.err" ]; then
    say "   curl stderr: $(head -n 3 "$DL_DIR/curl.err")"
fi

http=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^http=/){sub("http=","",$i); print $i}}' \
    "$DL_DIR/curl.txt" 2>/dev/null || true)
size_dl=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^size=/){sub("size=","",$i); print $i}}' \
    "$DL_DIR/curl.txt" 2>/dev/null || true)
elapsed=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^time=/){sub("time=","",$i); print $i}}' \
    "$DL_DIR/curl.txt" 2>/dev/null || true)
speed=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^speed=/){sub("speed=","",$i); print $i}}' \
    "$DL_DIR/curl.txt" 2>/dev/null || true)

check curl_rc "$([ "$rc" -eq 0 ]; echo $?)" \
    "rc=$rc (28=max-time or stall, 18=partial, 56=recv failure)"
check http "$([ "${http:-000}" = "200" ]; echo $?)" "http=${http:-000}"
check size_download "$([ "${size_dl:-0}" = "$EXPECT_BYTES" ]; echo $?)" \
    "size_download=${size_dl:-0} expected=$EXPECT_BYTES"

say "== 9. local file"
fsize=$(stat -c '%s' "$DL_DIR/$FILE" 2>/dev/null || echo 0)
stat -c '%s' "$DL_DIR/$FILE" >"$DL_DIR/size.txt" 2>/dev/null || true
check file_size "$([ "$fsize" = "$EXPECT_BYTES" ]; echo $?)" \
    "file=$fsize expected=$EXPECT_BYTES"

if have sha256sum && [ "$fsize" -gt 0 ]; then
    # A hashing failure must not abort the script: counters, kernel window and
    # the verdict are still first-hand evidence.
    if sha256sum "$DL_DIR/$FILE" >"$DL_DIR/sha256.txt" 2>"$DL_DIR/sha256.err"; then
        got_hash=$(awk 'NR==1{print $1}' "$DL_DIR/sha256.txt")
    else
        got_hash=""
    fi
else
    got_hash=""
fi
say "NEAR_SHA256=${got_hash:-unavailable}"
if [ -n "$FAR_HASH" ] && [ -n "$got_hash" ]; then
    check sha256 "$([ "$got_hash" = "$FAR_HASH" ]; echo $?)" \
        "near=$(printf '%.16s' "$got_hash")... far=$(printf '%.16s' "$FAR_HASH")..."
else
    skip sha256 "FAR_HASH not provided or sha256sum unavailable"
fi

say "== 10. counters (increments across the download)"
pre=$DL_DIR/near-stats-pre.txt
post=$DL_DIR/near-stats-post.txt
{
    for f in fec_proto_fail fec_hdr_crc_fail fec_payload_crc_fail; do
        a=$(c9_field "$pre" "$f")
        b=$(c9_field "$post" "$f")
        printf 'c9.%s.pre=%s post=%s delta=%s\n' "$f" "${a:-?}" "${b:-?}" \
            "$(if [ -n "$a" ] && [ -n "$b" ]; then inc "$b" "$a"; else echo '?'; fi)"
    done
    for f in rx_errs; do
        a=$(rx_field "$pre" "$f")
        b=$(rx_field "$post" "$f")
        printf '%s.pre=%s post=%s delta=%s\n' "$f" "${a:-?}" "${b:-?}" \
            "$(if [ -n "$a" ] && [ -n "$b" ]; then inc "$b" "$a"; else echo '?'; fi)"
    done
    for f in connect_fail alloc_fail join_bad; do
        a=$(c3_field "$pre" "$f")
        b=$(c3_field "$post" "$f")
        printf '%s.pre=%s post=%s delta=%s\n' "$f" "${a:-?}" "${b:-?}" \
            "$(if [ -n "$a" ] && [ -n "$b" ]; then inc "$b" "$a"; else echo '?'; fi)"
    done
    a=$(gaps_field "$pre")
    b=$(gaps_field "$post")
    printf 'gaps_total.pre=%s post=%s delta=%s\n' "${a:-?}" "${b:-?}" \
        "$(if [ -n "$a" ] && [ -n "$b" ]; then inc "$b" "$a"; else echo '?'; fi)"
    printf 'dead_now=%s\n' "$(c3_field "$post" dead_now)"
} >"$DL_DIR/counters.txt"
cat "$DL_DIR/counters.txt"

delta_zero() {
    name=$1
    line=$(grep "^$name\." "$DL_DIR/counters.txt" 2>/dev/null || true)
    d=$(printf '%s\n' "$line" | sed -n 's/.*delta=//p')
    if [ -z "$d" ] || [ "$d" = "?" ]; then
        check "counter_$name" 1 "counter missing or non-numeric"
    else
        check "counter_$name" "$([ "$d" -eq 0 ]; echo $?)" "delta=$d"
    fi
}
for c in fec_proto_fail fec_hdr_crc_fail fec_payload_crc_fail rx_errs \
    connect_fail alloc_fail join_bad gaps_total; do
    delta_zero "$c"
done

refs=$(sess_refs_field "$post")
if [ -z "$refs" ]; then
    if [ "$REQUIRE_SESS_REFS" -eq 1 ]; then
        check sess_refs 1 "sess_refs missing and REQUIRE_SESS_REFS=1"
    else
        skip sess_refs "field not exported by this build"
    fi
else
    check sess_refs "$([ "$refs" -eq 0 ]; echo $?)" "sess_refs=$refs"
fi

dead=$(c3_field "$post" dead_now)
if [ -n "$dead" ]; then
    check dead_now "$([ "$dead" -le "$DEAD_NOW_MAX" ]; echo $?)" \
        "dead_now=$dead max=$DEAD_NOW_MAX"
else
    skip dead_now "c3 dead_now missing"
fi

say "== 11. kernel window and restart"
# `journalctl -k --since <ISO with Z>` printed "-- No entries --" here and still
# exited 0, so the old window passed vacuously. Use the current boot with an
# @epoch window (timezone-free), then dmesg by line count. kw_src names the
# source that is readable; an empty body means the window held no messages and
# kernel_clean is SKIP, never PASS.
kw_src=none
kw_body=
jr=0
if have journalctl &&
    journalctl -k -b 0 -n 1 --no-pager 2>/dev/null | grep -qv '^-- No entries --$'; then
    jr=1
fi
if [ "$jr" -eq 1 ]; then
    kw_src=journalctl
    kw_raw=$(journalctl -k -b 0 --no-pager --since "@$start_epoch" --until "@$end_epoch" 2>/dev/null || true)
    case $kw_raw in
        '' | *'-- No entries --'*) kw_raw= ;;
    esac
    kw_body=$(printf '%s\n' "$kw_raw" | grep -v '^-- Logs begin' || true)
fi
if [ -z "$kw_body" ]; then
    if dmesg >/dev/null 2>&1; then
        if [ "$kw_src" = none ]; then kw_src=dmesg; fi
        dbody=$(dmesg 2>/dev/null | tail -n "+$((dmesg_before + 1))" || true)
        if [ -n "$dbody" ]; then
            kw_body=$dbody
            kw_src=dmesg
        fi
    fi
fi
if [ -n "$kw_body" ]; then
    printf '%s\n' "$kw_body" >"$EVIDENCE/near-kernel-window.txt"
else
    : >"$EVIDENCE/near-kernel-window.txt"
fi
check kernel_window_source "$([ "$kw_src" != none ]; echo $?)" "source=$kw_src"
if [ -z "$kw_body" ]; then
    skip kernel_clean "empty window ${start_utc}..${end_utc} (source=$kw_src); see near-dmesg.txt"
elif grep -qE 'BUG:|Oops|WARNING:|general protection fault' \
    "$EVIDENCE/near-kernel-window.txt" 2>/dev/null; then
    check kernel_clean 1 \
        "$(grep -cE 'BUG:|Oops|WARNING:|general protection fault' "$EVIDENCE/near-kernel-window.txt") matching lines"
else
    check kernel_clean 0 "window ${start_utc}..${end_utc} source=$kw_src"
fi

check no_restart "$([ -n "$boot_before" ] && [ "$boot_before" = "$boot_after" ]; echo $?)" \
    "boot_id $(printf '%.8s' "$boot_before").. -> $(printf '%.8s' "$boot_after").."

say "== 12. performance"
busy_delta=$((cpu_busy_after - cpu_busy_before))
idle_delta=$((cpu_idle_after - cpu_idle_before))
total_delta=$((busy_delta + idle_delta))
if [ "$total_delta" -gt 0 ]; then
    cpu_pct=$(awk -v b="$busy_delta" -v t="$total_delta" 'BEGIN{printf "%.1f", b*100/t}')
else
    cpu_pct=0
fi
mbit=$(awk -v s="${speed:-0}" 'BEGIN{printf "%.2f", s*8/1000000}')
say "NEAR_ELAPSED=${elapsed:-0}"
say "NEAR_SPEED=${speed:-0}"
{
    printf 'elapsed_s=%s\n' "${elapsed:-0}"
    printf 'speed_bytes_s=%s\n' "${speed:-0}"
    printf 'throughput_mbit_s=%s\n' "$mbit"
    printf 'cpu_busy_pct=%.1f\n' "$cpu_pct"
    printf 'loadavg=%s\n' "$(cat /proc/loadavg 2>/dev/null || echo '?')"
    printf 'mem_available_kb=%s\n' \
        "$(awk '/^MemAvailable:/{print $2}' /proc/meminfo 2>/dev/null || echo '?')"
    printf 'slab=%s\n' "$(grep -E '^tbt_(carrier|session|stream)' /proc/slabinfo 2>/dev/null | tr '\n' ';' || echo '?')"
} >"$DL_DIR/perf.txt"
cat "$DL_DIR/perf.txt"

# Failure forensics: cheap, local, and always collected on a failed run.
if [ "$FAILED" -ne 0 ]; then
    say "== 13. forensics"
    {
        dmesg 2>/dev/null | tail -n 400
    } >"$EVIDENCE/near-dmesg.txt" 2>/dev/null || true
    cp "$DL_DIR/near-stats-post.txt" "$EVIDENCE/near-stats-post.txt" 2>/dev/null || true
    cat /proc/net/tbtun/tunnels >"$EVIDENCE/near-tunnels.txt" 2>/dev/null || true
    cat /proc/net/tbtun/config >"$EVIDENCE/near-config.txt" 2>/dev/null || true
    ss -ltnp >"$EVIDENCE/near-ss.txt" 2>/dev/null || true
    ls -la /sys/fs/pstore >"$EVIDENCE/near-pstore.txt" 2>/dev/null || true
    journalctl -u tbtun-netwatch -n 50 --no-pager \
        >"$EVIDENCE/near-netwatch.txt" 2>/dev/null || true
    say "   evidence in $EVIDENCE"
fi

{
    printf 'curl_rc=%s\n' "$rc"
    printf 'http=%s\n' "${http:-000}"
    printf 'size_download=%s\n' "${size_dl:-0}"
    printf 'file_size=%s\n' "$fsize"
    printf 'sha256=%s\n' "${got_hash:-unavailable}"
    printf 'far_hash=%s\n' "${FAR_HASH:-unset}"
    printf 'elapsed_s=%s\n' "${elapsed:-0}"
    printf 'speed_bytes_s=%s\n' "${speed:-0}"
    printf 'near_dev=%s\n' "${NEAR_DEV:-unset}"
    printf 'bypass_dev=%s\n' "${bdev:-none}"
    printf 'bypass_rule=%s\n' "${brule:-unknown}"
    printf 'kernel_window_source=%s\n' "$kw_src"
    printf 'kernel_window_empty=%s\n' "$([ -z "$kw_body" ] && echo yes || echo no)"
    printf 'failed_checks=%s\n' "$FAILED"
} >"$DL_DIR/near-verdict.txt"

if [ "$FAILED" -eq 0 ]; then
    say "NEAR_RESULT PASS"
    exit 0
fi
say "NEAR_RESULT FAIL"
if [ "$rc" -eq 0 ]; then
    # The transfer finished but a local check failed (counters, kernel, ...).
    exit 7
fi
# curl itself failed: 28 max-time or stall, 18 partial, 56 receive failure.
exit 5
