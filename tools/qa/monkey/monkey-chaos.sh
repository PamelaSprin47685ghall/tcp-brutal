#!/bin/sh
# monkey-chaos.sh - near-side chaos actions for the tbtun 0.10.0 test tunnel.
#
# Every action runs on one near host (lubancat or 760K). Each action writes a
# mark into the harness timeline before and after. The report uses these marks
# to attribute an event to a dimension.
#
# The client is the userspace tbt_client. It is not a systemd unit on these
# hosts, so this script starts it and stops it by pid.
#
# Usage:
#   monkey-chaos.sh --run R client-start|client-stop|client-kill|client-status
#   monkey-chaos.sh --run R restart-churn --iterations 20 [--delays 0,1,3,10]
#   monkey-chaos.sh --run R freeze --secs 1,5,30 [--repeat 3]
#   monkey-chaos.sh --run R idle --secs 60,300 [--persist 300]
#   monkey-chaos.sh --run R stream-churn [--clients 8,16,32,64] [--rounds 5]
#   monkey-chaos.sh --run R halfclose [--hold 60]
#   monkey-chaos.sh --run R soft-flap --secs 2,5,30
#
# Exit: 0 done, 2 usage, 3 client will not start.
set -u

RUN=
ITER=20
DELAYS=0,1,3,10
SECS=1,5,30
REPEAT=3
CLIENTS=8,16,32,64
ROUNDS=5
HOLD=60
PERSIST=300
MODE=

CLIENT_DIR=/root/tbtun-090-src
CLIENT_BIN=./host/tbt_client
CLIENT_CONF=/root/tbtun-090/tbtun.conf
PROXY=http://127.0.0.1:7899
PROBE=https://example.com
MONKEY=/root/monkey
CLIENTS_PY=$MONKEY/monkey-clients.py

say() { printf '%s\n' "$*" >&2; }

usage() {
    sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

while [ $# -gt 0 ]; do
    case $1 in
    --run) RUN=$2; shift 2 ;;
    --iterations) ITER=$2; shift 2 ;;
    --delays) DELAYS=$2; shift 2 ;;
    --secs) SECS=$2; shift 2 ;;
    --repeat) REPEAT=$2; shift 2 ;;
    --clients) CLIENTS=$2; shift 2 ;;
    --rounds) ROUNDS=$2; shift 2 ;;
    --hold) HOLD=$2; shift 2 ;;
    --persist) PERSIST=$2; shift 2 ;;
    -h | --help) usage ;;
    client-start | client-stop | client-kill | client-status | restart-churn | \
        freeze | idle | stream-churn | halfclose | soft-flap)
        MODE=$1
        shift
        ;;
    *) say "unknown option: $1"; usage ;;
    esac
done

[ -n "$RUN" ] || usage
[ -n "$MODE" ] || usage
RUN_DIR=$MONKEY/evidence/$RUN
CHAOS_DIR=$RUN_DIR/chaos
mkdir -p "$CHAOS_DIR" "$MONKEY/logs" || exit 2

mark() { python3 "$MONKEY/monkey.py" mark --run "$RUN" --text "$*" >/dev/null; }

client_pid() { pgrep -x tbt_client | head -n 1; }

client_stop() {
    p=$(client_pid)
    [ -n "$p" ] || return 0
    kill -TERM "$p" 2>/dev/null
    i=0
    while [ $i -lt 50 ]; do
        kill -0 "$p" 2>/dev/null || return 0
        sleep 0.1
        i=$((i + 1))
    done
    say "client_stop: pid $p ignored SIGTERM for 5 s, sending SIGKILL"
    echo "stop_timeout" >>"$CHAOS_DIR/stop-timeouts.txt"
    kill -KILL "$p" 2>/dev/null
    sleep 0.2
    return 0
}

client_start() {
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    log=$MONKEY/logs/client-$stamp-$$.log
    if (cd "$CLIENT_DIR" && setsid nohup "$CLIENT_BIN" --config "$CLIENT_CONF" \
        verbose=1 >"$log" 2>&1 </dev/null &); then
        echo "$log" >"$MONKEY/logs/current"
    else
        say "client_start: launch failed"
        return 1
    fi
    i=0
    while [ $i -lt 100 ]; do
        if ss -Hltn 2>/dev/null | grep -q '127\.0\.0\.1:19000'; then
            return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    say "client_start: port 19000 did not listen in 10 s"
    return 1
}

# time_to_200: poll the sidecar until one probe returns 200. Prints seconds
# with 2 decimals, or "fail" after the cap.
time_to_200() {
    cap=${1:-30}
    t0=$(date +%s%N)
    while :; do
        code=$(curl -s -o /dev/null -w '%{http_code}' -x "$PROXY" \
            --max-time 3 "$PROBE" 2>/dev/null)
        if [ "$code" = 200 ]; then
            t1=$(date +%s%N)
            echo "$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.2f", (b-a)/1000000000 }')"
            return 0
        fi
        t1=$(date +%s%N)
        el=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", (b-a)/1000000000 }')
        if awk -v e="$el" -v c="$cap" 'BEGIN { exit !(e >= c) }'; then
            echo fail
            return 1
        fi
        sleep 0.2
    done
}

do_status() {
    p=$(client_pid)
    echo "pid=${p:-none}"
    echo "log=$(cat "$MONKEY/logs/current" 2>/dev/null)"
    ss -Hltn 2>/dev/null | grep '127\.0\.0\.1:19000' || echo "port19000=down"
    ss -Htn state established 2>/dev/null | grep -c ':19002' |
        sed 's/^/carriers_est=/'
    curl -s -o /dev/null -w 'probe_code=%{http_code} ttfb=%{time_starttransfer}\n' \
        -x "$PROXY" --max-time 10 "$PROBE" 2>/dev/null
}

case $MODE in
client-start) mark "client-start"; client_start && mark "client-start done" ;;
client-stop) mark "client-stop"; client_stop && mark "client-stop done" ;;
client-kill)
    mark "client-kill"
    p=$(client_pid)
    [ -n "$p" ] && kill -KILL "$p" 2>/dev/null
    sleep 0.2
    mark "client-kill done pid=${p:-none}"
    ;;
client-status) do_status ;;
restart-churn)
    csv=$CHAOS_DIR/restart-churn.csv
    echo "iter,delay,mode,pid,recover_s,rc" >"$csv"
    i=1
    while [ "$i" -le "$ITER" ]; do
        d=$(echo "$DELAYS" | cut -d, -f$(( (i - 1) % $(echo "$DELAYS" | tr ',' '\n' | wc -l) + 1 )))
        if [ $((i % 2)) -eq 0 ]; then m=kill; else m=term; fi
        mark "churn $i start delay=$d mode=$m"
        if [ "$m" = kill ]; then
            p=$(client_pid)
            [ -n "$p" ] && kill -KILL "$p" 2>/dev/null
            sleep 0.05
        else
            client_stop
        fi
        client_start
        rc=$?
        p=$(client_pid)
        rec=$(time_to_200 30)
        mark "churn $i up pid=${p:-none} recover=$rec"
        echo "$i,$d,$m,${p:-none},$rec,$rc" >>"$csv"
        sleep "$d"
        i=$((i + 1))
    done
    mark "restart-churn done $((ITER))"
    ;;
freeze)
    for s in $(echo "$SECS" | tr ',' ' '); do
        r=1
        while [ "$r" -le "$REPEAT" ]; do
            p=$(client_pid)
            [ -n "$p" ] || { say "freeze: no client"; exit 3; }
            mark "freeze start secs=$s rep=$r pid=$p"
            kill -STOP "$p" 2>/dev/null
            sleep "$s"
            kill -CONT "$p" 2>/dev/null
            rec=$(time_to_200 30)
            mark "freeze done secs=$s rep=$r recover=$rec"
            echo "$s,$r,$rec" >>"$CHAOS_DIR/freeze.csv"
            sleep 2
            r=$((r + 1))
        done
    done
    ;;
idle)
    echo "idle_s,persist_ok,persist_wait_s,first_ttfb" >"$CHAOS_DIR/idle.csv"
    for s in $(echo "$SECS" | tr ',' ' '); do
        python3 "$MONKEY/monkey.py" set --run "$RUN" probe=0 bulk=0 >/dev/null
        mark "idle start secs=$s"
        sleep "$s"
        out=$(curl -s -o /dev/null -w '%{http_code} %{time_starttransfer}' \
            -x "$PROXY" --max-time 40 "$PROBE" 2>/dev/null)
        mark "idle first-request after $s: $out"
        echo "$s,,,$out" >>"$CHAOS_DIR/idle.csv"
        python3 "$MONKEY/monkey.py" set --run "$RUN" probe=1 bulk=1 >/dev/null
        sleep 3
    done
    if [ "$PERSIST" -gt 0 ]; then
        mark "persistent idle hold=$PERSIST"
        python3 "$CLIENTS_PY" persistent --hold "$PERSIST" \
            >"$CHAOS_DIR/persistent.json" 2>&1
        mark "persistent done rc=$?"
    fi
    ;;
stream-churn)
    for c in $(echo "$CLIENTS" | tr ',' ' '); do
        mark "stream-churn clients=$c rounds=$ROUNDS"
        python3 "$CLIENTS_PY" aborts --clients "$c" --rounds "$ROUNDS" \
            >>"$CHAOS_DIR/stream-churn.jsonl" 2>&1
        mark "stream-churn clients=$c done rc=$?"
    done
    ;;
halfclose)
    mark "halfclose start hold=$HOLD"
    python3 "$CLIENTS_PY" halfclose --hold "$HOLD" \
        >"$CHAOS_DIR/halfclose.json" 2>&1
    mark "halfclose done rc=$?"
    python3 "$CLIENTS_PY" earlyclose --after 1024 \
        >"$CHAOS_DIR/earlyclose.json" 2>&1
    mark "earlyclose done rc=$?"
    curl -s -o /dev/null -w 'range_code=%{http_code} bytes=%{size_download}\n' \
        -x "$PROXY" --max-time 30 -H 'Range: bytes=0-1023' \
        http://127.0.0.1:18089/16m.bin >>"$CHAOS_DIR/halfclose.json" 2>&1
    curl -s -o /dev/null -w 'http11_code=%{http_code} bytes=%{size_download}\n' \
        -x "$PROXY" --http1.1 --max-time 30 \
        http://127.0.0.1:18089/16m.bin >>"$CHAOS_DIR/halfclose.json" 2>&1
    mark "halfclose range/http1.1 done"
    ;;
soft-flap)
    # The carrier path is also the SSH path, so an interface flap would cut
    # this script. netem loss 100% on the carrier ports is the honest
    # equivalent: the carriers stop, the SSH session does not.
    for s in $(echo "$SECS" | tr ',' ' '); do
        mark "soft-flap start secs=$s"
        tc qdisc add dev eth0 root handle 9: prio bands 4 2>/dev/null
        tc qdisc add dev eth0 parent 9:4 handle 90: netem loss 100% 2>/dev/null
        tc filter add dev eth0 parent 9: protocol ip prio 1 u32 \
            match ip dport 19002 0xffff flowid 9:4 2>/dev/null
        tc filter add dev eth0 parent 9: protocol ip prio 1 u32 \
            match ip dport 19012 0xffff flowid 9:4 2>/dev/null
        sleep "$s"
        tc qdisc del dev eth0 root 2>/dev/null
        mark "soft-flap end secs=$s"
        sleep 3
        echo "$s,$(time_to_200 40)" >>"$CHAOS_DIR/soft-flap.csv"
    done
    ;;
esac
exit 0
