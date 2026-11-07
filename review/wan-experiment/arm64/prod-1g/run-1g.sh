#!/usr/bin/env bash
# run-1g.sh - the 0.2.0 1 GiB acceptance run.
#
# Drives the near host (.65) and the far host (45.11.1.62) from the
# workstation. Far publishes a 1 GiB random file over a loopback-only busybox
# httpd; near fetches it once through the tunnel; both ends are judged.
#
# Shape (release design section 3):
#   256 carriers, n=4 m=4, fec=on, mode=grouped, w=8, crypto=on,
#   profile=http-kal, nonce=00112233445566778899aabbccddeeff
#   near listen 127.0.0.1:9001, far listen 0.0.0.0:9000, target 127.0.0.1:5210
#
# Judgment (all must hold):
#   1. curl rc=0, http=200, size_download=1073741824
#   2. local file size = 1073741824 and sha256 equals far's
#   3. both ends: deltas of fec_proto_fail, fec_hdr_crc_fail,
#      fec_payload_crc_fail, gaps_total, rx_errs, connect_fail, alloc_fail,
#      join_bad are all zero
#   4. no BUG:/Oops/WARNING:/general protection fault in either kernel window
#   5. near boot_id unchanged (no restart)
#   6. the hardware watchdog feeder is still running at the end
#   7. dead_now is bounded; sess_refs=0 when the build exports it
#
# Modes
#   --preflight   read-only checks on both ends; changes nothing
#   --run         full run (default), single shot, no retry
#   --cleanup     remove module/config/files on both ends, then verify
#   --keep        keep module, config and files after the run (forensics)
#   --dry-run     print the plan; no ssh, no writes
#
# Exit codes
#   0 PASS   2 usage   3 preflight   4 setup/join   5 transfer
#   6 checksum   7 counters/kernel/restart   9 cleanup
set -euo pipefail

PROG=${0##*/}
MODE=run
KEEP=0
DRY=0
EVIDENCE_ROOT=${EVIDENCE_ROOT:-$(cd -- "$(dirname -- "$0")/.." && pwd)}
NEAR_HOST=${NEAR_HOST:-root@192.168.123.65}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
FAR_IP=${FAR_IP:-45.11.1.62}
NEAR_KO=${NEAR_KO:-/root/tbtun-0.2.0/tbtun.ko}
FAR_KO=${FAR_KO:-/root/tbtun-0.2.0/tbtun.ko}
NEAR_SRCVERSION=${NEAR_SRCVERSION:-}
FAR_SRCVERSION=${FAR_SRCVERSION:-}
MODULES_LOADED=${MODULES_LOADED:-0}
CARRIERS=${CARRIERS:-256}
JOIN_TIMEOUT=${JOIN_TIMEOUT:-180}
SSH_TMO=${SSH_TMO:-20}
KEEP_SRC=${KEEP_SRC:-0}
REQUIRE_SESS_REFS=${REQUIRE_SESS_REFS:-0}
FAR_DIR=${FAR_DIR:-/root/accept-1g}
DL_DIR=${DL_DIR:-/root/dl-1g}
EXPECT_BYTES=1073741824
TUNNEL=1
NONCE=00112233445566778899aabbccddeeff

EV=
FAILED=0

usage() {
    cat <<EOF
usage: $PROG [--preflight|--run|--cleanup] [--keep] [--dry-run]

Environment:
  NEAR_HOST=root@192.168.123.65     FAR_HOST=root@45.11.1.62
  NEAR_KO=/root/tbtun-0.2.0/tbtun.ko
  FAR_KO=/root/tbtun-0.2.0/tbtun.ko
  NEAR_SRCVERSION= FAR_SRCVERSION=   empty: read from the ko
  MODULES_LOADED=0                 1: do not insmod, only verify
  CARRIERS=256 JOIN_TIMEOUT=180 SSH_TMO=20
  EVIDENCE_ROOT=<repo>/review/wan-experiment/arm64
  KEEP_SRC=0                       1: keep the uploaded scripts on the hosts
  REQUIRE_SESS_REFS=0              1: fail when the build does not export sess_refs

Example:
  review/wan-experiment/arm64/prod-1g/run-1g.sh --preflight
  review/wan-experiment/arm64/prod-1g/run-1g.sh --run
EOF
}

while (($#)); do
    case $1 in
        --preflight) MODE=preflight ;;
        --run) MODE=run ;;
        --cleanup) MODE=cleanup ;;
        --keep) KEEP=1 ;;
        --dry-run) DRY=1 ;;
        -h | --help) usage; exit 0 ;;
        *) printf '%s: unknown argument: %s\n' "$PROG" "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

say() { printf '%s\n' "$*"; }
note() { printf '%s %s\n' "$(date -u +%H:%M:%SZ)" "$*" | tee -a "$EV/run.log" >&2; }
mark() {
    printf '%s\t%s\t%s%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(date +%s)" \
        "$1" "${2:+ | $2}" | tee -a "$EV/timeline.txt" >&2
}

ssh_near() { timeout "$SSH_TMO" ssh -o BatchMode=yes "$NEAR_HOST" "$@"; }
ssh_far() { timeout "$SSH_TMO" ssh -o BatchMode=yes "$FAR_HOST" "$@"; }
scp_near() { timeout "$SSH_TMO" scp -q "$@"; }
scp_far() { timeout "$SSH_TMO" scp -q "$@"; }

# ---------------------------------------------------------------------------
# stats parsing (same field names as near-fetch.sh)

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

check() {
    local name=$1 rc=$2 detail=${3:-} line
    if ((rc == 0)); then
        line="CHECK $name PASS${detail:+ ($detail)}"
    else
        line="CHECK $name FAIL${detail:+ ($detail)}"
        FAILED=$((FAILED + 1))
    fi
    say "$line"
    if [[ -n ${EV:-} ]]; then
        printf '%s\n' "$line" >>"$EV/run.log"
    fi
    return 0
}
skip() {
    local line="CHECK $1 SKIP${2:+ ($2)}"
    say "$line"
    if [[ -n ${EV:-} ]]; then
        printf '%s\n' "$line" >>"$EV/run.log"
    fi
    return 0
}

# ---------------------------------------------------------------------------
# evidence directory

if ((DRY)); then
    say "dry-run plan:"
    say "  1. preflight: ssh both, feeder alive, tools, disk, ports"
    say "  2. far: upload far-serve.sh, --start (1 GiB, sha256, httpd 5210)"
    say "  3. modules: insmod $NEAR_KO / $FAR_KO, srcversion, ports, config"
    say "  4. join gate: both ends c3 up=$CARRIERS within ${JOIN_TIMEOUT}s"
    say "  5. near: upload near-fetch.sh, one curl (max 1800 s, stall 120 s <64 KiB/s)"
    say "  6. collect evidence, judge size/sha/counters/kernel/restart/feeder"
    say "  7. cleanup unless --keep"
    exit 0
fi

if [[ $MODE != cleanup ]]; then
    EV=$EVIDENCE_ROOT/accept-1g-$(date -u +%Y%m%dT%H%M%SZ)
    mkdir -p "$EV"
    : >"$EV/run.log"
    : >"$EV/timeline.txt"
    say "evidence: $EV"
fi

# ---------------------------------------------------------------------------
# remote helpers

near_ok() { ssh_near true >/dev/null 2>&1; }
far_ok() { ssh_far true >/dev/null 2>&1; }

near_boot_id() { ssh_near 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true; }
near_uptime() { ssh_near "cut -d' ' -f1 /proc/uptime" 2>/dev/null || true; }
near_feeder() {
    ssh_near 'bash -s' <<'EOS' 2>/dev/null || true
for fddir in /proc/[0-9]*/fd; do
    if ls -l "$fddir" 2>/dev/null | grep -qF /dev/watchdog0; then
        pid=${fddir#/proc/}; pid=${pid%/fd}
        printf 'pid=%s(%s)\n' "$pid" "$(cat "/proc/$pid/comm" 2>/dev/null || echo '?')"
        exit 0
    fi
done
exit 1
EOS
}

capture_common() {
    local host=$1 out=$2
    if [[ $host == near ]]; then
        ssh_near 'bash -s' >"$out" 2>&1 <<'EOS' || true
set -u
s() { printf '\n=== %s ===\n' "$1"; }
s date; date -u +%Y-%m-%dT%H:%M:%SZ
s boot_id; cat /proc/sys/kernel/random/boot_id 2>/dev/null || true
s uptime; cat /proc/uptime 2>/dev/null || true
s uname; uname -a
s loadavg; cat /proc/loadavg 2>/dev/null || true
s module; lsmod | grep -E '^tbtun' || true
s modinfo; modinfo -F version tbtun 2>/dev/null || true
s max_carriers; cat /sys/module/tbtun/parameters/max_carriers 2>/dev/null || true
s config; cat /proc/net/tbtun/config 2>/dev/null || true
s ports; cat /proc/net/tbtun/ports 2>/dev/null || true
s tunnels; cat /proc/net/tbtun/tunnels 2>/dev/null || true
s stats; cat /proc/net/tbtun/stats 2>/dev/null || true
s ss; ss -ltnp 2>/dev/null || true
s meminfo; awk '/^(MemTotal|MemAvailable|MemFree):/{print}' /proc/meminfo 2>/dev/null || true
s slab; grep -E '^tbt_(carrier|session|stream)' /proc/slabinfo 2>/dev/null || true
s dmesg; dmesg 2>/dev/null | tail -n 400 || true
s pstore; ls -la /sys/fs/pstore 2>/dev/null || true
s netwatch; systemctl is-active tbtun-netwatch 2>/dev/null || true
s netwatch_unit; systemctl show -p Restart -p MainPID -p RuntimeWatchdogUSec tbtun-netwatch 2>/dev/null || true
s netwatch_log; tail -n 20 /var/log/tbtun-watchdog.log 2>/dev/null || true
EOS
    else
        ssh_far 'bash -s' >"$out" 2>&1 <<'EOS' || true
set -u
s() { printf '\n=== %s ===\n' "$1"; }
s date; date -u +%Y-%m-%dT%H:%M:%SZ
s boot_id; cat /proc/sys/kernel/random/boot_id 2>/dev/null || true
s uname; uname -a
s module; lsmod | grep -E '^tbtun' || true
s modinfo; modinfo -F version tbtun 2>/dev/null || true
s max_carriers; cat /sys/module/tbtun/parameters/max_carriers 2>/dev/null || true
s config; cat /proc/net/tbtun/config 2>/dev/null || true
s ports; cat /proc/net/tbtun/ports 2>/dev/null || true
s tunnels; cat /proc/net/tbtun/tunnels 2>/dev/null || true
s stats; cat /proc/net/tbtun/stats 2>/dev/null || true
s ss; ss -ltnp 2>/dev/null || true
s meminfo; awk '/^(MemTotal|MemAvailable|MemFree):/{print}' /proc/meminfo 2>/dev/null || true
s slab; grep -E '^tbt_(carrier|session|stream)' /proc/slabinfo 2>/dev/null || true
s dmesg; dmesg 2>/dev/null | tail -n 400 || true
s httpd; ss -ltnH 'sport = :5210' 2>/dev/null || true
s httpd_head; curl -sSI -m 5 http://127.0.0.1:5210/accept-1g.bin 2>&1 | head -n 5 || true
EOS
    fi
}

kernel_window() {
    local host=$1 since=$2 out=$3
    if [[ $host == near ]]; then
        ssh_near "journalctl -k --since '$since' --no-pager 2>/dev/null || dmesg 2>/dev/null | tail -n 2000" >"$out" 2>/dev/null || true
    else
        ssh_far "journalctl -k --since '$since' --no-pager 2>/dev/null || dmesg 2>/dev/null | tail -n 2000" >"$out" 2>/dev/null || true
    fi
}

# ---------------------------------------------------------------------------
# cleanup

cleanup_all() {
    mark cleanup-start
    {
        say "== near"
        ssh_near "printf 'del 1\n' > /proc/net/tbtun/config 2>/dev/null || true
                  rmmod tbtun 2>/dev/null || true
                  rm -rf '$DL_DIR' 2>/dev/null || true
                  lsmod | grep -E '^tbtun ' || echo 'near: module unloaded'
                  test -e /proc/net/tbtun && echo 'near: /proc/net/tbtun still present' || echo 'near: procfs gone'
                  ss -ltnH 'sport = :9001' | grep -q . && echo 'near: 9001 busy' || echo 'near: 9001 free'" 2>&1 || true
        say "== far"
        ssh_far "printf 'del 1\n' > /proc/net/tbtun/config 2>/dev/null || true
                 rmmod tbtun 2>/dev/null || true
                 if [ -f '$FAR_DIR/httpd.pid' ]; then kill \$(cat '$FAR_DIR/httpd.pid') 2>/dev/null || true; fi
                 pkill -f 'busybox httpd -f -p 127.0.0.1:5210' 2>/dev/null || true
                 rm -rf '$FAR_DIR' 2>/dev/null || true
                 lsmod | grep -E '^tbtun ' || echo 'far: module unloaded'
                 test -e /proc/net/tbtun && echo 'far: /proc/net/tbtun still present' || echo 'far: procfs gone'
                 ss -ltnH 'sport = :5210' | grep -q . && echo 'far: 5210 busy' || echo 'far: 5210 free'" 2>&1 || true
        if [[ $KEEP_SRC -eq 0 ]]; then
            ssh_near "rm -f /root/far-serve.sh /root/near-fetch.sh" 2>/dev/null || true
            ssh_far "rm -f /root/far-serve.sh /root/near-fetch.sh" 2>/dev/null || true
        fi
    } | tee -a "$EV/cleanup.txt" >&2
    if grep -qE '(still present|busy)' "$EV/cleanup.txt" 2>/dev/null; then
        note "cleanup incomplete"
        return 1
    fi
    note "cleanup complete"
    return 0
}

if [[ $MODE == cleanup ]]; then
    EV=$EVIDENCE_ROOT/cleanup-$(date -u +%Y%m%dT%H%M%SZ)
    mkdir -p "$EV"
    : >"$EV/run.log"
    : >"$EV/timeline.txt"
    if cleanup_all; then exit 0; else exit 9; fi
fi

# ---------------------------------------------------------------------------
# preflight

mark preflight-start
say "== 1. reachability"
if near_ok; then check near_ssh 0 "$NEAR_HOST"; else check near_ssh 1 "$NEAR_HOST"; fi
if far_ok; then check far_ssh 0 "$FAR_HOST"; else check far_ssh 1 "$FAR_HOST"; fi
if ((FAILED)); then note "no ssh"; exit 3; fi

say "== 2. watchdog feeder on near (acceptance precondition)"
feeder=$(near_feeder || true)
if [[ -n $feeder ]]; then
    check near_feeder 0 "$feeder holds /dev/watchdog0"
else
    check near_feeder 1 "no process holds /dev/watchdog0; stopping the feeder is a 44 s reset"
fi
say "   netwatch: $(ssh_near 'systemctl is-active tbtun-netwatch 2>/dev/null || true' || true)"
say "   RuntimeWatchdogUSec: $(ssh_near 'systemctl show -p RuntimeWatchdogUSec --value 2>/dev/null || true' || true)"
say "   uptime: $(near_uptime) s (grace 600 s)"

say "== 3. near tools and shape"
near_tools=$(ssh_near 'for t in curl sha256sum awk ss timeout stat; do command -v $t >/dev/null || echo missing:$t; done' || true)
if [[ -z $near_tools ]]; then check near_tools 0 "curl sha256sum awk ss timeout stat"; else check near_tools 1 "$near_tools"; fi

say "== 4. far tools, disk and port"
far_tools=$(ssh_far 'for t in busybox dd sha256sum curl ss awk; do command -v $t >/dev/null || echo missing:$t; done' || true)
if [[ -z $far_tools ]]; then check far_tools 0 "busybox dd sha256sum curl ss awk"; else check far_tools 1 "$far_tools"; fi
if ssh_far 'busybox --list 2>/dev/null | grep -qx httpd'; then
    check far_httpd 0 "busybox httpd"
else
    check far_httpd 1 "busybox httpd missing"
fi
avail=$(ssh_far "df -P -k /root | awk 'NR==2{print \$4}'" 2>/dev/null || echo 0)
if [[ ${avail:-0} -ge 1572864 ]]; then
    check far_disk 0 "${avail}KiB free under /root"
else
    check far_disk 1 "${avail}KiB free, need >=1.5GiB (never use /tmp: 478M)"
fi
if ssh_far "ss -ltnH 'sport = :5210' | grep -q ."; then
    check far_port 0 "5210 already in use (reuse or stop the old server)"
else
    check far_port 0 "5210 free"
fi

say "== 5. module identity"
if [[ $MODULES_LOADED -eq 0 ]]; then
    if ssh_near 'lsmod | grep -q "^tbtun "'; then
        check near_loaded 0 "already loaded; MODULES_LOADED=0 will verify, not reload"
    else
        check near_loaded 0 "not loaded; will insmod $NEAR_KO"
    fi
    if ssh_far 'lsmod | grep -q "^tbtun "'; then
        check far_loaded 0 "already loaded"
    else
        check far_loaded 0 "not loaded; will insmod $FAR_KO"
    fi
else
    if ssh_near 'lsmod | grep -q "^tbtun "'; then check near_loaded 0 "loaded"; else check near_loaded 1 "MODULES_LOADED=1 but near has no tbtun"; fi
    if ssh_far 'lsmod | grep -q "^tbtun "'; then check far_loaded 0 "loaded"; else check far_loaded 1 "MODULES_LOADED=1 but far has no tbtun"; fi
fi

if ((FAILED)); then
    capture_common near "$EV/near-pre.txt"
    capture_common far "$EV/far-pre.txt"
    note "preflight failed"
    exit 3
fi
mark preflight-done

if [[ $MODE == preflight ]]; then
    note "preflight PASS ($FAILED failed checks)"
    exit 0
fi

# ---------------------------------------------------------------------------
# setup: far server, modules, config, join gate

say "== 6. far HTTP source"
if ! scp_far "$(dirname -- "$0")/far-serve.sh" "$FAR_HOST:/root/far-serve.sh"; then
    check far_upload 1 "scp far-serve.sh failed"
    exit 4
fi
far_out=$(ssh_far "sh /root/far-serve.sh --start" 2>&1) || true
printf '%s\n' "$far_out" | tee "$EV/far-serve.log" >&2
FAR_HASH=$(printf '%s\n' "$far_out" | sed -n 's/^FAR_SHA256=//p' | tail -n 1)
FAR_SIZE=$(printf '%s\n' "$far_out" | sed -n 's/^FAR_SIZE=//p' | tail -n 1)
printf '%s\n' "$FAR_HASH" >"$EV/far-sha256.txt"
printf '%s\n' "$FAR_SIZE" >"$EV/far-size.txt"
ssh_far "cat $FAR_DIR/accept-1g.bin.sha256" >"$EV/far-sha256-file.txt" 2>/dev/null || true
ssh_far "cat $FAR_DIR/httpd.log" >"$EV/far-httpd.log" 2>/dev/null || true
ssh_far "curl -sSI -m 5 http://127.0.0.1:5210/accept-1g.bin" >"$EV/far-httpd-headers.txt" 2>/dev/null || true
if printf '%s\n' "$far_out" | grep -q '^FAR_RESULT PASS'; then
    check far_serve 0 "size=$FAR_SIZE sha=${FAR_HASH:0:16}..."
else
    check far_serve 1 "see far-serve.log"
fi
check far_size "$([ "${FAR_SIZE:-0}" = "$EXPECT_BYTES" ]; echo $?)" "FAR_SIZE=$FAR_SIZE"
if ((FAILED)); then note "far server failed"; exit 4; fi

say "== 7. modules"
if [[ $MODULES_LOADED -eq 0 ]]; then
    if ! ssh_near 'lsmod | grep -q "^tbtun "'; then
        ssh_near "insmod '$NEAR_KO' max_carriers=$CARRIERS max_tunnels=1 debug=0" \
            || { check near_insmod 1 "$NEAR_KO"; exit 4; }
    fi
    if ! ssh_far 'lsmod | grep -q "^tbtun "'; then
        ssh_far "insmod '$FAR_KO' max_carriers=$CARRIERS max_tunnels=1 debug=0" \
            || { check far_insmod 1 "$FAR_KO"; exit 4; }
    fi
fi
for end in near far; do
    if [[ $end == near ]]; then
        ko=$NEAR_KO; want=${NEAR_SRCVERSION:-}
        got_sv=$(ssh_near 'cat /sys/module/tbtun/srcversion 2>/dev/null' || true)
        mc=$(ssh_near 'cat /sys/module/tbtun/parameters/max_carriers 2>/dev/null' || true)
        ver=$(ssh_near 'cat /sys/module/tbtun/version 2>/dev/null' || true)
    else
        ko=$FAR_KO; want=${FAR_SRCVERSION:-}
        got_sv=$(ssh_far 'cat /sys/module/tbtun/srcversion 2>/dev/null' || true)
        mc=$(ssh_far 'cat /sys/module/tbtun/parameters/max_carriers 2>/dev/null' || true)
        ver=$(ssh_far 'cat /sys/module/tbtun/version 2>/dev/null' || true)
    fi
    if [[ -z $want ]]; then
        if [[ $end == near ]]; then
            want=$(ssh_near "modinfo -F srcversion '$ko' 2>/dev/null" || true)
        else
            want=$(ssh_far "modinfo -F srcversion '$ko' 2>/dev/null" || true)
        fi
    fi
    check "${end}_srcversion" "$([ -n "$want" ] && [ "$got_sv" = "$want" ]; echo $?)" \
        "loaded=${got_sv:-?} ko=${want:-?}"
    check "${end}_max_carriers" "$([ "$mc" = "$CARRIERS" ]; echo $?)" "max_carriers=$mc"
    say "   $end version=$ver"
done
if ((FAILED)); then
    capture_common near "$EV/near-pre.txt"
    capture_common far "$EV/far-pre.txt"
    note "module identity failed; no config written"
    exit 4
fi

ssh_near "printf 'ports 30000-30511\n' > /proc/net/tbtun/ports"
ssh_far "printf 'ports 31000-31511\n' > /proc/net/tbtun/ports"

say "== 8. config"
FAR_CFG="tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5210 n=4 m=4 fec=on mode=grouped w=8 carriers=$CARRIERS crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31511 nonce=$NONCE"
NEAR_CFG="tunnel 1 role=near listen=127.0.0.1:9001 peer=$FAR_IP:9000 n=4 m=4 fec=on mode=grouped w=8 carriers=$CARRIERS crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30511 nonce=$NONCE"
ssh_far "printf '%s\n' '$FAR_CFG' > /proc/net/tbtun/config"
ssh_near "printf '%s\n' '$NEAR_CFG' > /proc/net/tbtun/config"
ssh_far 'cat /proc/net/tbtun/config' >"$EV/far-config.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/config' >"$EV/near-config.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/tunnels' >"$EV/far-tunnels.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/tunnels' >"$EV/near-tunnels.txt" 2>/dev/null || true
for f in far near; do
    cfg=$(cat "$EV/$f-config.txt" 2>/dev/null || true)
    for tok in "carriers=$CARRIERS" "n=4" "m=4" "w=8" "fec=on" "mode=grouped" "crypto=on" "profile=http-kal"; do
        check "${f}_cfg_${tok%%=*}" "$(case " $cfg " in *" $tok "*) echo 0 ;; *) echo 1 ;; esac)" "$tok"
    done
done
if ((FAILED)); then
    capture_common near "$EV/near-pre.txt"
    capture_common far "$EV/far-pre.txt"
    note "config read-back failed; no join gate"
    exit 4
fi

say "== 9. join gate (both ends, ${JOIN_TIMEOUT}s)"
i=0
nu=?; fu=?
while ((i < JOIN_TIMEOUT)); do
    nu=$(ssh_near 'awk '\''$1=="tunnel"&&$2==1&&$3=="c3"{for(i=1;i<=NF;i++) if($i ~ /^up=/){print substr($i,4); exit}}'\'' /proc/net/tbtun/stats 2>/dev/null' || true)
    fu=$(ssh_far 'awk '\''$1=="tunnel"&&$2==1&&$3=="c3"{for(i=1;i<=NF;i++) if($i ~ /^up=/){print substr($i,4); exit}}'\'' /proc/net/tbtun/stats 2>/dev/null' || true)
    printf '%s near_up=%s far_up=%s\n' "$(date -u +%FT%TZ)" "${nu:-?}" "${fu:-?}" | tee -a "$EV/join-probes.txt" >&2
    if [[ ${nu:-0} == "$CARRIERS" && ${fu:-0} == "$CARRIERS" ]]; then break; fi
    sleep 1
    i=$((i + 1))
done
check near_join "$([ "${nu:-0}" = "$CARRIERS" ]; echo $?)" "up=${nu:-?} after ${i}s"
check far_join "$([ "${fu:-0}" = "$CARRIERS" ]; echo $?)" "up=${fu:-?} after ${i}s"
if ((FAILED)); then
    capture_common near "$EV/near-pre.txt"
    capture_common far "$EV/far-pre.txt"
    note "join gate failed; no transfer attempted"
    exit 4
fi
mark join-done

# ---------------------------------------------------------------------------
# one download

say "== 10. download (single shot)"
ssh_far 'cat /proc/net/tbtun/stats' >"$EV/far-stats-pre.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/stats' >"$EV/near-stats-pre.txt" 2>/dev/null || true
ssh_far 'cat /proc/stat' >"$EV/far-procstat-pre.txt" 2>/dev/null || true
ssh_near 'cat /proc/stat' >"$EV/near-procstat-pre.txt" 2>/dev/null || true
NEAR_BOOT_BEFORE=$(near_boot_id)
START_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
mark transfer-start "expect=$EXPECT_BYTES"

if ! scp_near "$(dirname -- "$0")/near-fetch.sh" "$NEAR_HOST:/root/near-fetch.sh"; then
    check near_upload 1 "scp near-fetch.sh failed"
    note "upload failed; no transfer attempted"
    exit 4
fi
near_rc=0
ssh_near "FAR_HASH='$FAR_HASH' REQUIRE_SESS_REFS='$REQUIRE_SESS_REFS' sh /root/near-fetch.sh --run" 2>&1 |
    tee "$EV/near-fetch.log" >&2 || near_rc=$?
say "NEAR_FETCH_RC=$near_rc"

END_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
mark transfer-end "rc=$near_rc"

ssh_far 'cat /proc/net/tbtun/stats' >"$EV/far-stats-post.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/stats' >"$EV/near-stats-post.txt" 2>/dev/null || true
ssh_far 'cat /proc/stat' >"$EV/far-procstat-post.txt" 2>/dev/null || true
ssh_near 'cat /proc/stat' >"$EV/near-procstat-post.txt" 2>/dev/null || true
NEAR_BOOT_AFTER=$(near_boot_id)

# ---------------------------------------------------------------------------
# evidence

say "== 11. evidence"
ssh_near "cat $DL_DIR/curl.txt" >"$EV/near-curl.txt" 2>/dev/null || true
ssh_near "cat $DL_DIR/size.txt" >"$EV/near-size.txt" 2>/dev/null || true
ssh_near "cat $DL_DIR/sha256.txt" >"$EV/near-sha256.txt" 2>/dev/null || true
ssh_near "cat $DL_DIR/counters.txt" >"$EV/near-counters.txt" 2>/dev/null || true
ssh_near "cat $DL_DIR/perf.txt" >"$EV/near-perf.txt" 2>/dev/null || true
ssh_near "cat $DL_DIR/near-verdict.txt" >"$EV/near-verdict.txt" 2>/dev/null || true
ssh_near "cat /proc/net/tbtun/stats" >"$EV/near-stats-post2.txt" 2>/dev/null || true
capture_common near "$EV/near-post.txt"
capture_common far "$EV/far-post.txt"
kernel_window near "$START_UTC" "$EV/near-kernel-window.txt"
kernel_window far "$START_UTC" "$EV/far-kernel-window.txt"
for end in near far; do
    src=$(ssh_"$end" 'command -v journalctl >/dev/null 2>&1 && journalctl -k -n 1 --no-pager >/dev/null 2>&1 && echo journalctl || { dmesg >/dev/null 2>&1 && echo dmesg || echo none; }' 2>/dev/null || echo none)
    check "${end}_kernel_source" "$([ "$src" != none ]; echo $?)" "source=${src:-none}"
done
ssh_near 'cat /proc/net/tbtun/config' >"$EV/near-config-post.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/config' >"$EV/far-config-post.txt" 2>/dev/null || true
ssh_near 'cat /proc/net/tbtun/tunnels' >"$EV/near-tunnels-post.txt" 2>/dev/null || true
ssh_far 'cat /proc/net/tbtun/tunnels' >"$EV/far-tunnels-post.txt" 2>/dev/null || true
ssh_near 'ss -ltnp' >"$EV/near-ss.txt" 2>/dev/null || true
ssh_far 'ss -ltnp' >"$EV/far-ss.txt" 2>/dev/null || true
ssh_near 'ls -la /sys/fs/pstore 2>/dev/null; for f in /sys/fs/pstore/*; do [ -f "$f" ] && echo "== $f" && head -c 4000 "$f"; done' >"$EV/near-pstore.txt" 2>/dev/null || true
ssh_far 'dmesg 2>/dev/null | tail -n 800' >"$EV/far-dmesg.txt" 2>/dev/null || true
ssh_near 'dmesg 2>/dev/null | tail -n 800' >"$EV/near-dmesg.txt" 2>/dev/null || true
ssh_near 'journalctl -u tbtun-netwatch -n 80 --no-pager' >"$EV/near-netwatch.txt" 2>/dev/null || true
ssh_near 'tail -n 80 /var/log/tbtun-watchdog.log' >"$EV/near-watchdog.log" 2>/dev/null || true
for end in near far; do
    ko=$([[ $end == near ]] && echo "$NEAR_KO" || echo "$FAR_KO")
    {
        printf 'ko=%s\n' "$ko"
        ssh_"$end" "modinfo -F version '$ko' 2>/dev/null; modinfo -F srcversion '$ko' 2>/dev/null; modinfo -F vermagic '$ko' 2>/dev/null" || true
        printf 'loaded_srcversion=%s\n' "$(ssh_"$end" 'cat /sys/module/tbtun/srcversion 2>/dev/null || true')"
    } >"$EV/$end-module.txt" 2>/dev/null || true
done
if [[ $KEEP_SRC -eq 0 ]]; then
    mkdir -p "$EV/near-evidence"
scp_near -r "$NEAR_HOST:$DL_DIR/evidence/." "$EV/near-evidence/" 2>/dev/null || true
ssh_near 'rm -f /root/near-fetch.sh' 2>/dev/null || true
    ssh_far 'rm -f /root/far-serve.sh' 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# verdict

say "== 12. verdict"

curl_line=$(cat "$EV/near-curl.txt" 2>/dev/null || true)
http=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^http=/){sub("http=","",$i); print $i}}' <<<"$curl_line")
size_dl=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^size=/){sub("size=","",$i); print $i}}' <<<"$curl_line")
elapsed=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^time=/){sub("time=","",$i); print $i}}' <<<"$curl_line")
speed=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^speed=/){sub("speed=","",$i); print $i}}' <<<"$curl_line")
check http "$([ "${http:-000}" = "200" ]; echo $?)" "http=${http:-000}"
check size_download "$([ "${size_dl:-0}" = "$EXPECT_BYTES" ]; echo $?)" \
    "size_download=${size_dl:-0} expected=$EXPECT_BYTES"
check near_fetch_rc "$([ "$near_rc" -eq 0 ]; echo $?)" "rc=$near_rc"

fsize=$(tr -d '[:space:]' <"$EV/near-size.txt" 2>/dev/null || true)
check file_size "$([ "${fsize:-0}" = "$EXPECT_BYTES" ]; echo $?)" \
    "file=${fsize:-0} expected=$EXPECT_BYTES"

near_hash=$(awk 'NR==1{print $1}' "$EV/near-sha256.txt" 2>/dev/null || true)
far_hash_file=$(awk 'NR==1{print $1}' "$EV/far-sha256.txt" 2>/dev/null || true)
check sha256 "$([ -n "$near_hash" ] && [ "$near_hash" = "$far_hash_file" ]; echo $?)" \
    "near=${near_hash:0:16}... far=${far_hash_file:0:16}..."
size_bad=0
sha_bad=0
if [[ "${size_dl:-0}" != "$EXPECT_BYTES" || "${fsize:-0}" != "$EXPECT_BYTES" ]]; then
    size_bad=1
fi
if [[ -n $near_hash && "$near_hash" != "$far_hash_file" ]]; then
    sha_bad=1
fi

# counters: increments across the transfer, both ends
counter_report() {
    local end=$1 out=$2
    local pre=$EV/$end-stats-pre.txt post=$EV/$end-stats-post.txt
    {
        for f in fec_proto_fail fec_hdr_crc_fail fec_payload_crc_fail; do
            a=$(c9_field "$pre" "$f"); b=$(c9_field "$post" "$f")
            printf '%s.%s=%s\n' "$f" "${b:-?}" "$(if [[ -n $a && -n $b ]]; then echo $((b - a)); else echo '?'; fi)"
        done
        for f in rx_errs; do
            a=$(rx_field "$pre" "$f"); b=$(rx_field "$post" "$f")
            printf '%s=%s\n' "$f" "$(if [[ -n $a && -n $b ]]; then echo $((b - a)); else echo '?'; fi)"
        done
        for f in connect_fail alloc_fail join_bad; do
            a=$(c3_field "$pre" "$f"); b=$(c3_field "$post" "$f")
            printf '%s=%s\n' "$f" "$(if [[ -n $a && -n $b ]]; then echo $((b - a)); else echo '?'; fi)"
        done
        a=$(gaps_field "$pre"); b=$(gaps_field "$post")
        printf 'gaps_total=%s\n' "$(if [[ -n $a && -n $b ]]; then echo $((b - a)); else echo '?'; fi)"
        printf 'dead_now=%s\n' "$(c3_field "$post" dead_now)"
        printf 'up_post=%s\n' "$(c3_field "$post" up)"
        printf 'n_post=%s\n' "$(c3_field "$post" n)"
        printf 'replaced_post=%s\n' "$(c3_field "$post" replaced)"
        printf 'lost_post=%s\n' "$(c3_field "$post" lost)"
        printf 'sess_refs=%s\n' "$(sess_refs_field "$post")"
        printf 'data_tx=%s\n' "$(c3_field "$post" data_tx)"
        printf 'retx_bytes=%s\n' "$(c3_field "$post" retx_bytes)"
        printf 'dup_bytes_ppm=%s\n' "$(c3_field "$post" dup_bytes_ppm)"
    } >"$out"
}
counter_report near "$EV/counters-near.txt"
counter_report far "$EV/counters-far.txt"
for end in near far; do
    while IFS='=' read -r k v; do
        if [[ $k == dead_now || $k == up_post || $k == n_post || $k == replaced_post || \
              $k == lost_post || $k == sess_refs || $k == data_tx || $k == retx_bytes || \
              $k == dup_bytes_ppm ]]; then
            continue
        fi
        check "${end}_${k}" "$([ "${v:-?}" = "0" ]; echo $?)" "delta=$v"
    done <"$EV/counters-$end.txt"
done

dead=$(sed -n 's/^dead_now=//p' "$EV/counters-near.txt")
check dead_now_bound "$([ -n "$dead" ] && [ "$dead" -le 64 ]; echo $?)" "near dead_now=${dead:-?} max=64"

refs=$(sed -n 's/^sess_refs=//p' "$EV/counters-near.txt")
if [[ -n $refs ]]; then
    check sess_refs "$([ "$refs" -eq 0 ]; echo $?)" "sess_refs=$refs"
else
    skip sess_refs "not exported by this build"
fi

for end in near far; do
    if grep -qE 'BUG:|Oops|WARNING:|general protection fault' "$EV/$end-kernel-window.txt" 2>/dev/null; then
        check "${end}_kernel" 1 \
            "$(grep -cE 'BUG:|Oops|WARNING:|general protection fault' "$EV/$end-kernel-window.txt") matching lines"
    else
        check "${end}_kernel" 0 "window ${START_UTC}..${END_UTC}"
    fi
done
for end in near far; do
    if grep -qE 'BUG:|Oops|general protection fault' "$EV/$end-dmesg.txt" 2>/dev/null; then
        check "${end}_dmesg_tail" 1 "matching line in last 800 lines"
    else
        check "${end}_dmesg_tail" 0 "no BUG/Oops/GPF in last 800 lines"
    fi
done

check near_no_restart "$([ -n "$NEAR_BOOT_BEFORE" ] && [ "$NEAR_BOOT_BEFORE" = "$NEAR_BOOT_AFTER" ]; echo $?)" \
    "boot_id ${NEAR_BOOT_BEFORE:0:8}.. -> ${NEAR_BOOT_AFTER:0:8}.."
feeder_after=$(near_feeder || true)
check feeder_alive "$([ -n "$feeder_after" ]; echo $?)" "${feeder_after:-no holder}"

# ---------------------------------------------------------------------------
# performance

near_busy() {
    awk 'NR==1{printf "%s %s", $2+$3+$4+$7+$8+$9, $5+$6}' "$1" 2>/dev/null || echo "0 0"
}
read -r nb ni < <(near_busy "$EV/near-procstat-pre.txt")
read -r nb2 ni2 < <(near_busy "$EV/near-procstat-post.txt")
read -r fb fi < <(near_busy "$EV/far-procstat-pre.txt")
read -r fb2 fi2 < <(near_busy "$EV/far-procstat-post.txt")
cpu_pct() {
    awk -v b1="$1" -v i1="$2" -v b2="$3" -v i2="$4" 'BEGIN{
        b=b2-b1; i=i2-i1; t=b+i
        if (t > 0) printf "%.1f", b*100/t
        else printf "0"}'
}
near_cpu=$(cpu_pct "${nb:-0}" "${ni:-0}" "${nb2:-0}" "${ni2:-0}")
far_cpu=$(cpu_pct "${fb:-0}" "${fi:-0}" "${fb2:-0}" "${fi2:-0}")
mbit=$(awk -v s="${speed:-0}" 'BEGIN{printf "%.2f", s*8/1000000}')
{
    printf 'bytes=%s\n' "$EXPECT_BYTES"
    printf 'elapsed_s=%s\n' "${elapsed:-0}"
    printf 'speed_bytes_s=%s\n' "${speed:-0}"
    printf 'throughput_mbit_s=%s\n' "$mbit"
    printf 'near_cpu_busy_pct=%s\n' "$near_cpu"
    printf 'far_cpu_busy_pct=%s\n' "$far_cpu"
    printf 'near_loadavg=%s\n' "$(awk '/^=== loadavg ===/{getline; print; exit}' "$EV/near-post.txt" 2>/dev/null)"
    printf 'near_meminfo=%s\n' "$(awk '/^=== meminfo ===/{f=1;next} f&&/^===/{exit} f{printf "%s;",$0}' "$EV/near-post.txt" 2>/dev/null)"
    printf 'near_slab=%s\n' "$(awk '/^=== slab ===/{f=1;next} f&&/^===/{exit} f{printf "%s;",$0}' "$EV/near-post.txt" 2>/dev/null)"
    printf 'near_replaced=%s\n' "$(c3_field "$EV/near-stats-post.txt" replaced)"
    printf 'near_data_tx=%s\n' "$(c3_field "$EV/near-stats-post.txt" data_tx)"
    printf 'near_retx_bytes=%s\n' "$(c3_field "$EV/near-stats-post.txt" retx_bytes)"
    printf 'near_dup_bytes_ppm=%s\n' "$(c3_field "$EV/near-stats-post.txt" dup_bytes_ppm)"
} >"$EV/perf.txt"
cat "$EV/perf.txt" | tee -a "$EV/run.log" >&2

# ---------------------------------------------------------------------------
# summary

verdict=PASS
if ((FAILED > 0)); then verdict=FAIL; fi
{
    printf '# 1 GiB acceptance run\n\n'
    printf -- '- start: %s\n- end: %s\n' "$START_UTC" "$END_UTC"
    printf -- '- near: `%s` (aarch64, 6.1.172)\n' "$NEAR_HOST"
    printf -- '- far: `%s`\n' "$FAR_HOST"
    printf -- '- shape: %s carriers, n=4 m=4, grouped, w=8, crypto=on, http-kal\n' "$CARRIERS"
    printf -- '- evidence: `%s`\n\n' "$EV"
    printf '## Verdict\n\n**%s**\n\n' "$verdict"
    printf '| check | value |\n|---|---|\n'
    printf '| curl http | %s |\n' "${http:-000}"
    printf '| size_download | %s |\n' "${size_dl:-0}"
    printf '| file size | %s |\n' "${fsize:-0}"
    printf '| near sha256 | %s |\n' "${near_hash:-unavailable}"
    printf '| far sha256 | %s |\n' "${far_hash_file:-unavailable}"
    printf '| elapsed | %s s |\n' "${elapsed:-0}"
    printf '| throughput | %s Mbit/s |\n' "$mbit"
    printf '| near CPU busy | %s%% |\n' "$near_cpu"
    printf '| far CPU busy | %s%% |\n' "$far_cpu"
    printf '| near boot_id stable | %s |\n' "$([ "$NEAR_BOOT_BEFORE" = "$NEAR_BOOT_AFTER" ] && echo yes || echo no)"
    printf '| feeder after run | %s |\n\n' "${feeder_after:-none}"
    printf '## Counter increments\n\n'
    printf '```\nnear:\n'; cat "$EV/counters-near.txt"
    printf 'far:\n'; cat "$EV/counters-far.txt"; printf '```\n\n'
    printf '## Checks\n\n```\n'
    grep -E '^(CHECK|NEAR_CHECK) ' "$EV/run.log" "$EV/near-fetch.log" 2>/dev/null || true
    printf '```\n\n'
    printf '## Cleanup\n\n'
    if [[ $KEEP -eq 1 ]]; then
        printf 'skipped (--keep); module, config and files stay for forensics\n'
    else
        printf 'see `cleanup.txt`\n'
    fi
} >"$EV/SUMMARY.md"

# ---------------------------------------------------------------------------
# cleanup

cleanup_rc=0
if ((KEEP == 0)); then
    if ! cleanup_all; then
        cleanup_rc=9
    fi
else
    note "keep requested; cleanup skipped"
fi

if ((FAILED > 0)); then
    note "verdict FAIL ($FAILED failed checks)"
    note "evidence kept in $EV"
    if ((size_bad == 1)); then exit 5; fi
    if ((sha_bad == 1)); then exit 6; fi
    if ((near_rc != 0)); then exit 5; fi
    exit 7
fi
if ((cleanup_rc != 0)); then
    note "cleanup incomplete; evidence kept in $EV"
    exit 9
fi
note "verdict PASS"
exit 0
