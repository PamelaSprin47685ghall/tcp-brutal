#!/bin/sh
# fix42-sampler.sh - sample one tunnel's counters on the host that runs it.
#
# usage: fix42-sampler.sh <full|compact> <log> <stop> <tunnel> <dev> [interval]
#
# full     one timestamped block per tick: the c3/c6/c9/c10/c11/c12/c13 and
#          stream lines plus netdev and /proc/stat. Used for the 15 s first
#          connect, where the near tunnel can disappear at teardown and the
#          only surviving evidence is what this sampler already wrote.
# compact  one line per tick with the fields the steady and 1 GiB judgments
#          need; small enough to run for hours.
#
# The sampler stops when <stop> exists. It never writes outside <log>.
set -u

mode=${1:?mode}
log=${2:?log}
stop=${3:?stop}
t=${4:?tunnel}
dev=${5:?dev}
interval=${6:-0.1}

while [ ! -e "$stop" ]; do
    {
        date +%s.%N
        if [ "$mode" = full ]; then
            awk -v t="$t" '
                $1 == "tunnel" && $2 == t &&
                ($3 ~ /^capture_runs/ || $3 == "c3" || $3 == "c6" ||
                 $3 == "c9" || $3 == "c10" || $3 == "c11" ||
                 $3 == "c12" || $3 == "c13" || $3 == "stream")' \
                /proc/net/tbtun/stats
            awk -v d="$dev:" '$1 == d { print "netdev_rx=" $2 " netdev_tx=" $10 }' \
                /proc/net/dev
            awk 'NR == 1 {
                print "cpu_busy=" $2+$3+$4+$7+$8+$9 " cpu_idle=" $5+$6
            }' /proc/stat
        else
            awk -v t="$t" '
                $1 == "tunnel" && $2 == t {
                    for (i = 4; i <= NF; i++) {
                        p = index($i, "=")
                        if (!p)
                            continue
                        k = substr($i, 1, p - 1)
                        v = substr($i, p + 1)
                        if ($3 == "stream" && (k == "state" || k == "kill"))
                            st[k] = v
                        else if ($3 == "c13" && (k == "adaptive" || k == "state"))
                            c13[k] = v
                        else
                            kv[k] = v
                    }
                }
                END {
                    printf "up=%s decode_blocks=%s nack_retx=%s ",
                           kv["up"], kv["fec_decode_blocks"], kv["fec_nack_retx"]
                    printf "replay_queued=%s replay_queue_high=%s enospc=%s ",
                           kv["fec_replay_queued"], kv["fec_replay_queue_high"],
                           kv["fec_replay_miss_enospc"]
                    printf "replay_drop=%s reset_tcp=%s reset_hole=%s ",
                           kv["fec_replay_drop"], kv["fec_reset_tcp_failed"],
                           kv["fec_reset_hole"]
                    printf "credit_wait=%s outq_drop=%s outq_depth=%s ",
                           kv["fec_credit_wait"], kv["fec_outq_drop"],
                           kv["fec_outq_depth"]
                    printf "ack_tx=%s ack_rx=%s stream_state=%s stream_kill=%s ",
                           kv["fec_ack_tx"], kv["fec_ack_rx"],
                           st["state"], st["kill"]
                    printf "submit_bytes=%s",
                           kv["submit_bytes"]
                }' /proc/net/tbtun/stats
            awk -v d="$dev:" '$1 == d {
                printf " netdev_rx=%s netdev_tx=%s", $2, $10
            }' /proc/net/dev
            awk 'NR == 1 {
                printf " cpu_busy=%s cpu_idle=%s\n", $2+$3+$4+$7+$8+$9, $5+$6
            }' /proc/stat
        fi
    } >>"$log" 2>/dev/null
    sleep "$interval"
done
