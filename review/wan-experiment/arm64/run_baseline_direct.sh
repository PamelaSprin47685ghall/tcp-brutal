#!/usr/bin/env bash
# Direct-WAN HTTP/1.1 baseline, near side (.65) -> far HND0. No iperf3.
#
# Run this ON the near machine. It changes that machine's ip rule and sysctl,
# so it holds /tmp/tbtun-rk3588.lock for the whole run and refuses to start if
# someone else holds it. The HND0 server takes /tmp/tbtun-hnd0.lock through
# `http_server.py --lock`, so the far machine is covered for exactly the load
# window and released when the unit stops.
#
# Shape: 8 independent client processes, one keep-alive HTTP/1.1 connection
# each, 8 MiB per request, 10 s x 2 runs per direction. This mirrors the
# baseline-direct P8 10 s x 2 iperf3 run (review/wan-experiment/baseline-direct)
# so the two are comparable, but it needs no iperf3 on either end and it
# reports application-level Mbit/s.
#
# usage:
#   run_baseline_direct.sh [--out DIR] [HOST [PORT [RUNS [SECONDS [DIRECTION]]]]]
#
#   HOST       far side address          default 45.11.1.62
#   PORT       far side server port      default 10000
#   RUNS       repetitions               default 2
#   SECONDS    seconds per phase         default 10
#   DIRECTION  up | down | both          default both (up phase, then down phase)
#   --out DIR  evidence directory        default ./runs/<UTC stamp>
#
# Far side server: transient systemd unit `tbt-http-baseline`, started with
# `systemd-run` so it survives the ssh session and stops cleanly with
# `systemctl stop`. Its log and counters land in /tmp/tbt-http-baseline/ on
# HND0 and are copied back into --out. The unit gets RuntimeMaxSec as a
# safety net: if this wrapper is killed, the unit still stops (and releases the
# HND0 machine lock) instead of pinning it until someone notices.
#
# Side effects, all reverted by the EXIT trap:
#   ip rule add to <HOST>/32 lookup main pref 100   (only if it was absent)
#   sysctl net.core.rmem_max/wmem_max=67108864      (previous values restored)
#   systemctl stop tbt-http-baseline                (idempotent)
#   /tmp/tbtun-rk3588.lock, /tmp/tbtun-hnd0.lock    (released on exit)
#
# Output in --out:
#   client.json                 per-run/per-phase/per-process app bytes, Mbit/s, CPU
#   client.stdout/.stderr       raw client output
#   summary.json / summary.txt  merged view: app throughput + NIC bytes + CPU
#   near-*.before/.after        /proc/net/dev, /proc/stat, free -m snapshots
#   far-*.before/.after         same, on HND0
#   near-ss-ti.txt far-ss-ti.txt  mid-run socket snapshots
#   hnd0-stats.json hnd0-server.log  far server counters and log
#   env.txt                     versions, hashes, route and sysctl state

set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
CLIENT="$HERE/http_client.py"
SERVER="$HERE/http_server.py"
FAR_DIR=/tmp/tbt-http-baseline
UNIT=tbt-http-baseline
PROCS=8
MIB=8
SYSCTL_VALUE=67108864
LOCK=${TB_LOCK:-/tmp/tbtun-rk3588.lock}
HND0_LOCK=/tmp/tbtun-hnd0.lock

usage() {
    sed -n '2,/^set -euo pipefail$/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'
}

OUT=""
POSITIONAL=()
while (($#)); do
    case "$1" in
        --out) OUT=${2:?--out needs a directory}; shift 2 ;;
        --out=*) OUT=${1#--out=}; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; POSITIONAL+=("$@"); break ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) POSITIONAL+=("$1"); shift ;;
    esac
done

HOST=${POSITIONAL[0]:-45.11.1.62}
PORT=${POSITIONAL[1]:-10000}
RUNS=${POSITIONAL[2]:-2}
DUR=${POSITIONAL[3]:-10}
DIRECTION=${POSITIONAL[4]:-both}

case "$DIRECTION" in up|down|both) ;; *) echo "DIRECTION must be up, down or both" >&2; exit 2 ;; esac
[[ "$RUNS" =~ ^[0-9]+$ ]] || { echo "RUNS must be a positive integer" >&2; exit 2; }
[[ "$DUR" =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo "SECONDS must be a positive number" >&2; exit 2; }
FAR="root@$HOST"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "$FAR")

if [[ -z "$OUT" ]]; then
    OUT="$HERE/runs/$(date -u +%Y%m%dT%H%M%SZ)"
fi
mkdir -p "$OUT"

log() { printf '%s %s\n' "$(date -u +%H:%M:%SZ)" "$*" >&2; }
run() { printf '+ %s\n' "$*" >&2; "$@"; }

acquire_near_lock() {
    local writable=1
    if [[ -w "$LOCK" || ( ! -e "$LOCK" && -w "$(dirname -- "$LOCK")" ) ]]; then
        exec 9>>"$LOCK"
    elif [[ -r "$LOCK" ]]; then
        writable=0
        exec 9<"$LOCK"
    else
        echo "cannot access the .65 machine lock $LOCK" >&2
        exit 1
    fi
    if ! flock -n 9; then
        echo "the .65 machine lock $LOCK is held by:" >&2
        tail -3 "$LOCK" >&2 || true
        exit 1
    fi
    if ((writable)); then
        printf 'pid=%s time=%s user=%s purpose=http-baseline-direct host=%s\n' \
            "$$" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(id -un)" "$(hostname)" >&9 || true
    else
        log "warning: $LOCK is not writable, holder line not recorded"
    fi
    NEAR_LOCK_HELD=1
}

RULE_ADDED=0
SERVER_STARTED=0
NEAR_LOCK_HELD=0
CLIENT_PID=""
OLD_RMEM=""
OLD_WMEM=""

cleanup() {
    local rc=$?
    set +e
    if [[ -n "$CLIENT_PID" ]] && kill -0 "$CLIENT_PID" 2>/dev/null; then
        # setsid made the client its own process group; kill the group so the 8
        # workers cannot outlive the wrapper after the ip rule is removed.
        kill -TERM -- "-$CLIENT_PID" 2>/dev/null || kill -TERM "$CLIENT_PID" 2>/dev/null
        wait "$CLIENT_PID" 2>/dev/null
    fi
    if [[ "$SERVER_STARTED" == 1 ]]; then
        "${SSH[@]}" "systemctl stop $UNIT 2>/dev/null; systemctl reset-failed $UNIT 2>/dev/null" >/dev/null 2>&1
    fi
    if [[ -n "$OLD_RMEM" ]]; then
        sudo -n sysctl -q -w "net.core.rmem_max=$OLD_RMEM" "net.core.wmem_max=$OLD_WMEM" >/dev/null 2>&1
    fi
    if [[ "$RULE_ADDED" == 1 ]]; then
        sudo -n ip rule del to "$HOST/32" lookup main pref 100 >/dev/null 2>&1
    fi
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ---------------------------------------------------------------- preflight
if ! sudo -n true 2>/dev/null; then
    echo "passwordless sudo is required for ip rule and sysctl" >&2
    exit 1
fi
command -v python3 >/dev/null || { echo "python3 missing on near side" >&2; exit 1; }
command -v ip >/dev/null || { echo "iproute2 missing on near side" >&2; exit 1; }
command -v flock >/dev/null || { echo "flock missing on near side" >&2; exit 1; }

acquire_near_lock
NEAR_ARCH=$(uname -m)
if [[ "$NEAR_ARCH" != "aarch64" ]]; then
    log "WARNING: near arch is $NEAR_ARCH, not aarch64; the arm64 acceptance expects this on .65"
fi

log "out=$OUT host=$HOST port=$PORT runs=$RUNS seconds=$DUR direction=$DIRECTION procs=$PROCS mib=$MIB"

PHASES=1
if [[ "$DIRECTION" == both ]]; then PHASES=2; fi
UNIT_MAX_SEC=$(awk -v r="$RUNS" -v p="$PHASES" -v d="$DUR" 'BEGIN { printf "%d", r * p * d + 180 }')

log "plan:"
log "  1. flock $LOCK (near, held for the whole run)"
log "  2. ip rule add to $HOST/32 lookup main pref 100        (skipped if already present)"
log "  3. sysctl net.core.rmem_max=net.core.wmem_max=$SYSCTL_VALUE  (restored on exit)"
log "  4. ssh $FAR systemd-run --unit=$UNIT (RuntimeMaxSec=${UNIT_MAX_SEC}s) -- /usr/bin/python3 $FAR_DIR/http_server.py --host 0.0.0.0 --port $PORT --lock $HND0_LOCK"
log "  5. python3 http_client.py --host $HOST --port $PORT --procs $PROCS --mib $MIB --seconds $DUR --runs $RUNS --direction $DIRECTION"
log "  6. ssh $FAR systemctl stop $UNIT; fetch stats and log"

# HND0 bypass: keep direct-WAN traffic out of the mihomo TUN.
# `ip rule show` prints a host rule as `to 45.11.1.62`, without the /32.
if ip rule show | grep -q "to ${HOST} lookup main"; then
    log "ip rule to $HOST already present, leaving it alone"
else
    run sudo -n ip rule add to "$HOST/32" lookup main pref 100
    RULE_ADDED=1
fi
ip route get "$HOST" | tee "$OUT/near-route.txt"
NEAR_IF=$(ip route get "$HOST" | awk '{for (i = 1; i < NF; i++) if ($i == "dev") {print $(i + 1); exit}}')
case "$NEAR_IF" in
    ""|tun*|utun*|Meta*)
        echo "route to $HOST does not use a physical NIC (dev='${NEAR_IF}'), refusing to measure through the tunnel" >&2
        exit 1
        ;;
esac
log "near interface for $HOST: $NEAR_IF"

OLD_RMEM=$(sysctl -n net.core.rmem_max)
OLD_WMEM=$(sysctl -n net.core.wmem_max)
run sudo -n sysctl -q -w "net.core.rmem_max=$SYSCTL_VALUE" "net.core.wmem_max=$SYSCTL_VALUE"

{
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "near_host: $(hostname)"
    echo "near_arch: $NEAR_ARCH $(uname -r)"
    echo "near_python: $(python3 --version 2>&1)"
    echo "near_client_sha256: $(sha256sum "$CLIENT" | cut -d' ' -f1)"
    echo "near_server_sha256: $(sha256sum "$SERVER" | cut -d' ' -f1)"
    echo "near_route: $(cat "$OUT/near-route.txt" | tr '\n' ' ')"
    echo "near_iface: $NEAR_IF"
    echo "near_lock: $LOCK held=$NEAR_LOCK_HELD holder=$(tail -1 "$LOCK" 2>/dev/null || true)"
    echo "hnd0_lock: $HND0_LOCK (held by the server unit for its lifetime)"
    echo "near_sysctl_before: rmem_max=$OLD_RMEM wmem_max=$OLD_WMEM"
    echo "near_sysctl_during: rmem_max=$(sysctl -n net.core.rmem_max) wmem_max=$(sysctl -n net.core.wmem_max)"
    echo "near_ip_rule_added_by_wrapper: $RULE_ADDED"
    echo "far_python: $("${SSH[@]}" 'python3 --version 2>&1; command -v python3')"
    echo "far_sysctl: $("${SSH[@]}" 'sysctl -n net.core.rmem_max net.core.wmem_max net.ipv4.tcp_congestion_control' | tr '\n' ' ')"
    echo "far_nproc: $("${SSH[@]}" nproc)"
    echo "far_mem_mib: $("${SSH[@]}" 'awk "/MemTotal/ {print int(\$2/1024)}" /proc/meminfo')"
} | tee "$OUT/env.txt"

# ------------------------------------------------------------- far side server
log "starting $UNIT on $FAR"
run "${SSH[@]}" "mkdir -p $FAR_DIR && rm -f $FAR_DIR/server.log $FAR_DIR/stats.json"
run scp -q -o BatchMode=yes "$SERVER" "$FAR:$FAR_DIR/http_server.py"
run "${SSH[@]}" "systemctl stop $UNIT 2>/dev/null; systemctl reset-failed $UNIT 2>/dev/null
systemd-run --unit=$UNIT \
  --property=StandardOutput=file:$FAR_DIR/server.log \
  --property=StandardError=file:$FAR_DIR/server.log \
  --property=Restart=no \
  --property=RuntimeMaxSec=$UNIT_MAX_SEC \
  -- /usr/bin/python3 $FAR_DIR/http_server.py --host 0.0.0.0 --port $PORT \
     --stats-out $FAR_DIR/stats.json --stats-interval 5 --lock $HND0_LOCK"
SERVER_STARTED=1

probe() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsS --max-time 2 "http://$HOST:$PORT/health" >/dev/null 2>&1
    else
        python3 - "$HOST" "$PORT" <<'PY' >/dev/null 2>&1
import socket, sys
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=2)
s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
sys.exit(0 if b"200" in s.recv(4096).split(b"\r\n")[0] else 1)
PY
    fi
}
ready=0
for _ in $(seq 1 30); do
    if probe; then ready=1; break; fi
    sleep 0.5
done
if [[ "$ready" != 1 ]]; then
    echo "far server did not answer GET /health on $HOST:$PORT" >&2
    if "${SSH[@]}" "grep -q lock-busy $FAR_DIR/server.log 2>/dev/null"; then
        echo "the HND0 machine lock $HND0_LOCK is held by someone else:" >&2
        "${SSH[@]}" "tail -3 $HND0_LOCK" >&2 || true
    fi
    "${SSH[@]}" "tail -20 $FAR_DIR/server.log" >&2 || true
    exit 1
fi
log "far server ready"

# ------------------------------------------------------------------ snapshots
snap_before() {
    run "${SSH[@]}" "cat /proc/net/dev" >"$OUT/far-netdev.before"
    run "${SSH[@]}" "cat /proc/stat" >"$OUT/far-stat.before"
    run "${SSH[@]}" "free -m" >"$OUT/far-free.before"
    cat "/proc/net/dev" >"$OUT/near-netdev.before"
    cat "/proc/stat" >"$OUT/near-stat.before"
    free -m >"$OUT/near-free.before"
}
snap_after() {
    run "${SSH[@]}" "cat /proc/net/dev" >"$OUT/far-netdev.after"
    run "${SSH[@]}" "cat /proc/stat" >"$OUT/far-stat.after"
    run "${SSH[@]}" "free -m" >"$OUT/far-free.after"
    cat "/proc/net/dev" >"$OUT/near-netdev.after"
    cat "/proc/stat" >"$OUT/near-stat.after"
    free -m >"$OUT/near-free.after"
}

snap_before

# --------------------------------------------------------------------- client
CLIENT_CMD=(python3 "$CLIENT" --host "$HOST" --port "$PORT" --procs "$PROCS" --mib "$MIB"
    --seconds "$DUR" --runs "$RUNS" --direction "$DIRECTION" --out "$OUT/client.json")
log "client: ${CLIENT_CMD[*]}"
if command -v setsid >/dev/null 2>&1; then
    setsid "${CLIENT_CMD[@]}" >"$OUT/client.stdout" 2>"$OUT/client.stderr" &
else
    "${CLIENT_CMD[@]}" >"$OUT/client.stdout" 2>"$OUT/client.stderr" &
fi
CLIENT_PID=$!

MID=$(awk -v d="$DUR" 'BEGIN { m = int(d / 2); if (m < 1) m = 1; print m }')
sleep "$MID"
log "mid-run socket snapshots after ${MID}s"
ss -tin dst "$HOST" >"$OUT/near-ss-ti.txt" 2>/dev/null || ss -tin >"$OUT/near-ss-ti.txt" 2>/dev/null || true
"${SSH[@]}" "ss -tin" >"$OUT/far-ss-ti.txt" 2>/dev/null || true

CLIENT_RC=0
wait "$CLIENT_PID" || CLIENT_RC=$?
log "client exit=$CLIENT_RC"

snap_after

# --------------------------------------------------------- stop and fetch far
run "${SSH[@]}" "systemctl stop $UNIT" || true
SERVER_STARTED=0
sleep 0.5
run "${SSH[@]}" "cat $FAR_DIR/stats.json" >"$OUT/hnd0-stats.json" || true
run "${SSH[@]}" "cat $FAR_DIR/server.log" >"$OUT/hnd0-server.log" || true

# -------------------------------------------------------------------- summary
python3 - "$OUT" "$NEAR_IF" "$HOST" <<'PY' | tee "$OUT/summary.txt"
import json
import os
import sys

out, near_if, host = sys.argv[1], sys.argv[2], sys.argv[3]


def read(name):
    path = os.path.join(out, name)
    try:
        with open(path) as fh:
            return fh.read()
    except OSError:
        return None


def netdev(text, iface):
    for line in (text or "").splitlines():
        name, _, rest = line.partition(":")
        if name.strip() != iface:
            continue
        f = rest.split()
        if len(f) < 9:
            return None
        return {"rx_bytes": int(f[0]), "tx_bytes": int(f[8])}
    return None


def cpu(text):
    for line in (text or "").splitlines():
        if line.startswith("cpu "):
            v = [int(x) for x in line.split()[1:]]
            idle = v[3] + (v[4] if len(v) > 4 else 0)
            return {"total": sum(v), "idle": idle}
    return None


def mem(text):
    for line in (text or "").splitlines():
        if line.startswith("Mem:"):
            f = line.split()
            return {"total_mib": int(f[1]), "used_mib": int(f[2]), "available_mib": int(f[6])}
    return None


def delta(before, after):
    if not before or not after:
        return None
    return {k: after[k] - before[k] for k in before}


def busy_pct(before, after):
    if not before or not after:
        return None
    dt = after["total"] - before["total"]
    di = after["idle"] - before["idle"]
    if dt <= 0:
        return None
    return round(100.0 * (1.0 - di / dt), 2)


near_before = netdev(read("near-netdev.before"), near_if)
near_after = netdev(read("near-netdev.after"), near_if)
far_before = netdev(read("far-netdev.before"), "eth0")
far_after = netdev(read("far-netdev.after"), "eth0")
near_d = delta(near_before, near_after)
far_d = delta(far_before, far_after)

try:
    with open(os.path.join(out, "client.json")) as fh:
        client = json.load(fh)
except (OSError, ValueError):
    client = {"runs": [], "config": {}, "exit_code": None}

try:
    with open(os.path.join(out, "hnd0-stats.json")) as fh:
        server = json.load(fh)
except (OSError, ValueError):
    server = {}

summary = {
    "host": host,
    "near_iface": near_if,
    "far_iface": "eth0",
    "config": client.get("config"),
    "client_exit_code": client.get("exit_code"),
    "nic": {
        "near": near_d,
        "far": far_d,
        "near_mib": None if not near_d else {
            "rx_mib": round(near_d["rx_bytes"] / 2**20, 2),
            "tx_mib": round(near_d["tx_bytes"] / 2**20, 2)},
        "far_mib": None if not far_d else {
            "rx_mib": round(far_d["rx_bytes"] / 2**20, 2),
            "tx_mib": round(far_d["tx_bytes"] / 2**20, 2)},
    },
    "cpu_busy_pct": {
        "near": busy_pct(cpu(read("near-stat.before")), cpu(read("near-stat.after"))),
        "far": busy_pct(cpu(read("far-stat.before")), cpu(read("far-stat.after"))),
    },
    "mem": {
        "near_before": mem(read("near-free.before")),
        "near_after": mem(read("near-free.after")),
        "far_before": mem(read("far-free.before")),
        "far_after": mem(read("far-free.after")),
    },
    "server_counters": server,
    "phases": [],
}

for run in client.get("runs", []):
    for phase in run["phases"]:
        agg = dict(phase["aggregate"])
        agg["run"] = run["run"]
        summary["phases"].append(agg)

with open(os.path.join(out, "summary.json"), "w") as fh:
    json.dump(summary, fh, indent=1, sort_keys=True)
    fh.write("\n")

print("config:", json.dumps(client.get("config"), sort_keys=True))
print("client exit:", client.get("exit_code"))
print("")
print("| run | dir | procs | MiB | Mbit/s | reqs | partial | errs | cpu_s | cpu_cores |")
print("|---|---|---|---|---|---|---|---|---|---|")
for a in summary["phases"]:
    print("| %s | %s | %s/%s | %s | %.2f | %s | %s | %s | %s | %.2f |" % (
        a["run"], a["direction"], a["procs_reported"], a["procs_expected"], a["mib"],
        a["mbps"], a["requests"], a["partial_requests"], a["errors"], a["cpu_s"],
        a["cpu_cores"]))
print("")
if near_d:
    print("near %s: rx %s MiB, tx %s MiB" % (
        near_if, summary["nic"]["near_mib"]["rx_mib"], summary["nic"]["near_mib"]["tx_mib"]))
if far_d:
    print("far eth0: rx %s MiB, tx %s MiB" % (
        summary["nic"]["far_mib"]["rx_mib"], summary["nic"]["far_mib"]["tx_mib"]))
print("cpu busy %%: near %s, far %s" % (
    summary["cpu_busy_pct"]["near"], summary["cpu_busy_pct"]["far"]))
if server:
    print("far server counters: requests=%s uploads=%s downloads=%s rx=%s tx=%s "
          "client_aborts=%s errors=%s" % (
              server.get("requests"), server.get("uploads"), server.get("downloads"),
              server.get("rx_bytes"), server.get("tx_bytes"),
              server.get("client_aborts"), server.get("errors")))
    up = [a for a in summary["phases"] if a["direction"] == "up"]
    down = [a for a in summary["phases"] if a["direction"] == "down"]
    if up and server.get("rx_bytes"):
        print("up cross-check: client body bytes %d vs server rx %d (%.2f%%)" % (
            sum(a["bytes"] for a in up), server["rx_bytes"],
            100.0 * sum(a["bytes"] for a in up) / server["rx_bytes"]))
    if down and server.get("tx_bytes"):
        print("down cross-check: client body bytes %d vs server tx %d (%.2f%%)" % (
            sum(a["bytes"] for a in down), server["tx_bytes"],
            100.0 * sum(a["bytes"] for a in down) / server["tx_bytes"]))
PY

log "evidence in $OUT"
exit "$CLIENT_RC"
