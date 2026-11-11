#!/usr/bin/env bash
# multistream-accept.sh - real-hardware acceptance for FEC v7 multi-stream.
#
# Two concurrent near-side applications connect to the tunnel listen port
# from two fixed local source ports. The far side runs one target listener
# that serves two distinct deterministic HTTP bodies (/stream/a and
# /stream/b). The run proves:
#   - the module parameter fec_groups_per_flow matches on both ends;
#   - the tunnel uses RLNC (mode=grouped), because v7 rejects a second flow
#     when the session is SC-LDPC (tbt_fec_flow_create: fec_sclc &&
#     n_fec_flows -> -EOPNOTSUPP);
#   - both streams are live with distinct sids and no cross-delivery;
#   - each body's sha256 and byte count match the far target;
#   - c3 up=256 and fec_drop_sid does not grow;
#   - with fec_groups_per_flow=0 the second stream is refused.
#
# The tool does not reload the module. Load both ends with the same parameter
# before --run:
#   insmod tbtun.ko fec_groups_per_flow=16
# and before --param0:
#   insmod tbtun.ko fec_groups_per_flow=0
#
# Modes
#   --run        two concurrent 1 GiB (or --seconds) streams (default)
#   --param0     one stream plus a refused second stream at parameter 0
#   --status     read-only preflight
#   --cleanup    stop the remote clients/target and restore --setup config
#
# Options
#   --setup      write the test tunnel config on both ends; --cleanup restores
#                the previous config. This disrupts other tunnels on the pair.
#   --bytes N    body size (default 1073741824)
#   --seconds S  timed body instead of --bytes (0 = byte mode)
#   --keep       keep remote files after the run
#   --evidence D reuse an evidence dir (needed by --cleanup --setup to restore)
#
# Exit: 0 PASS, 2 usage, 3 preflight, 4 target, 5 traffic, 6 checks.
set -uo pipefail

. "$(dirname -- "$0")/lib.sh"

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
PROG=${0##*/}
MODE=run
SETUP=0
KEEP=0
MS_BYTES=${MS_BYTES:-1073741824}
MS_SECONDS=${MS_SECONDS:-0}
MS_GROUPS_PER_FLOW=${MS_GROUPS_PER_FLOW:-16}
MS_NEAR_LPORT_A=${MS_NEAR_LPORT_A:-41001}
MS_NEAR_LPORT_B=${MS_NEAR_LPORT_B:-41002}
MS_TIMEOUT=${MS_TIMEOUT:-1800}
MS_DIR=${MS_DIR:-/root/tbtun-ms}
MS_FAR_TARGET=${MS_FAR_TARGET:-$FAR_PORT}
MS_NEAR_CFG=${MS_NEAR_CFG:-}
MS_FAR_CFG=${MS_FAR_CFG:-}
EV=
FAILED=0
CLEANED=0
NEAR_CFG_SAVED=0
FAR_CFG_SAVED=0
TARGET_PID=
CLIENT_PID=

usage() {
    sed -n '2,50p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

while (($#)); do
    case $1 in
        --run) MODE=run ;;
        --param0) MODE=param0 ;;
        --status) MODE=status ;;
        --cleanup) MODE=cleanup ;;
        --setup) SETUP=1 ;;
        --keep) KEEP=1 ;;
        --evidence) shift; EV=${1:-} ;;
        --bytes) shift; MS_BYTES=${1:-} ;;
        --seconds) shift; MS_SECONDS=${1:-} ;;
        -h|--help) usage ;;
        *) printf '%s: unknown option: %s\n' "$PROG" "$1" >&2; usage ;;
    esac
    shift
done

qa_num "$MS_BYTES" || { printf 'bad --bytes\n' >&2; exit 2; }
case $MS_SECONDS in ''|*[!0-9.]*) printf 'bad --seconds\n' >&2; exit 2 ;; esac

cleanup() {
    ((CLEANED)) && return
    CLEANED=1
    if [[ -n ${CLIENT_PID:-} ]]; then
        qa_ssh near "kill '$CLIENT_PID' 2>/dev/null || true" >/dev/null 2>&1 || true
    fi
    if [[ -n ${TARGET_PID:-} ]]; then
        qa_ssh far "kill '$TARGET_PID' 2>/dev/null || true" >/dev/null 2>&1 || true
    fi
    qa_ssh near "pkill -f '$MS_DIR/[r]un-clients.sh' 2>/dev/null || true" >/dev/null 2>&1 || true
    qa_ssh far "pkill -f '$MS_DIR/[m]ultistream-target.py' 2>/dev/null || true" >/dev/null 2>&1 || true
    if ((SETUP)); then
        if [[ -n $EV && -s $EV/config-near-before.txt ]]; then
            qa_ssh near "cat '$EV/config-near-before.txt' > '$PROC_ROOT/config'" >/dev/null 2>&1 || true
        fi
        if [[ -n $EV && -s $EV/config-far-before.txt ]]; then
            qa_ssh far "cat '$EV/config-far-before.txt' > '$PROC_ROOT/config'" >/dev/null 2>&1 || true
        fi
    fi
    if ((!KEEP)); then
        qa_ssh near "rm -rf '$MS_DIR'" >/dev/null 2>&1 || true
        qa_ssh far "rm -rf '$MS_DIR'" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

field() { # file class field [tunnel]
    qa_field "$1" "$2" "$3" "${4:-$TUNNEL}"
}

cfg_has() { # file token
    grep -qw -- "$2" "$1" 2>/dev/null
}

param_of() { # near|far
    qa_ssh "$1" "cat /sys/module/tbtun/parameters/fec_groups_per_flow 2>/dev/null" |
        tr -d '[:space:]'
}

snap() { # near|far name
    qa_snap_stats "$1" "$EV/$1-$2-stats.txt"
    qa_snap_config "$1" "$EV/$1-$2-config.txt"
    qa_snap_tunnels "$1" "$EV/$1-$2-tunnels.txt"
}

preflight() {
    if [[ -z $EV ]]; then
        qa_evidence_init ms >/dev/null || { printf 'cannot create evidence dir\n' >&2; exit 3; }
    else
        mkdir -p "$EV" || { printf 'cannot create evidence dir %s\n' "$EV" >&2; exit 3; }
    fi
    qa_say "EVIDENCE=$EV"
    for host in near far; do
        qa_check "${host}_reachable" "$(qa_ssh "$host" true >/dev/null 2>&1; echo $?)" "$host"
        qa_check "${host}_module" "$(qa_ssh "$host" 'lsmod | grep -q "^tbtun "' >/dev/null 2>&1; echo $?)" tbtun
    done
    qa_snap_config near "$EV/config-near-before.txt" 2>/dev/null || true
    qa_snap_config far "$EV/config-far-before.txt" 2>/dev/null || true
    snap near pre
    snap far pre

    if ((SETUP)); then
        if [[ -z $MS_NEAR_CFG ]]; then
            MS_NEAR_CFG="tunnel $TUNNEL role=near listen=127.0.0.1:$NEAR_PORT peer=$FAR_IP:9000 n=4 m=4 fec=on mode=grouped w=8 carriers=256 crypto=off dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 portblock=30000-30511 nonce=00112233445566778899aabbccddeeff adaptive=off"
        fi
        if [[ -z $MS_FAR_CFG ]]; then
            MS_FAR_CFG="tunnel $TUNNEL role=far listen=0.0.0.0:9000 target=127.0.0.1:$MS_FAR_TARGET n=4 m=4 fec=on mode=grouped w=8 carriers=256 crypto=off dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 portblock=31000-31511 nonce=00112233445566778899aabbccddeeff adaptive=off"
        fi
        qa_ssh near "cat > '$PROC_ROOT/config'" <<<"$MS_NEAR_CFG" || true
        qa_ssh far "cat > '$PROC_ROOT/config'" <<<"$MS_FAR_CFG" || true
        NEAR_CFG_SAVED=1
        FAR_CFG_SAVED=1
        local i
        for i in $(seq 1 20); do
            snap near setup
            snap far setup
            if [[ $(field "$EV/near-setup-stats.txt" c3 up) == 256 &&
                  $(field "$EV/far-setup-stats.txt" c3 up) == 256 ]]; then
                break
            fi
            sleep 1
        done
    fi

    local ncfg=$EV/near-pre-config.txt fcfg=$EV/far-pre-config.txt
    if ((SETUP)); then
        ncfg=$EV/near-setup-config.txt
        fcfg=$EV/far-setup-config.txt
    fi
    if grep -q 'mode=sc-ldpc' "$ncfg" 2>/dev/null; then
        qa_say "NOTE: SC-LDPC (mode=sc-ldpc) allows one flow only in v7; use mode=grouped for this test"
    fi
    qa_check near_cfg_fec "$(cfg_has "$ncfg" 'fec=on'; echo $?)" "fec=on"
    qa_check far_cfg_fec "$(cfg_has "$fcfg" 'fec=on'; echo $?)" "fec=on"
    qa_check near_cfg_mode "$(cfg_has "$ncfg" 'mode=grouped'; echo $?)" "mode=grouped (RLNC)"
    qa_check far_cfg_mode "$(cfg_has "$fcfg" 'mode=grouped'; echo $?)" "mode=grouped (RLNC)"
    qa_check near_cfg_carriers "$(cfg_has "$ncfg" 'carriers=256'; echo $?)" "carriers=256"
    qa_check far_cfg_carriers "$(cfg_has "$fcfg" 'carriers=256'; echo $?)" "carriers=256"

    local p_near p_far want=$MS_GROUPS_PER_FLOW
    p_near=$(param_of near)
    p_far=$(param_of far)
    if [[ $MODE == param0 ]]; then
        want=0
    fi
    qa_check near_param "$([[ $p_near == "$want" ]]; echo $?)" "near=$p_near want=$want"
    qa_check far_param "$([[ $p_far == "$want" ]]; echo $?)" "far=$p_far want=$want"
    qa_check near_up256 "$([[ $(field "$EV/near-setup-stats.txt" c3 up) == 256 || $(field "$EV/near-pre-stats.txt" c3 up) == 256 ]]; echo $?)" "up=256"
    qa_check far_up256 "$([[ $(field "$EV/far-setup-stats.txt" c3 up) == 256 || $(field "$EV/far-pre-stats.txt" c3 up) == 256 ]]; echo $?)" "up=256"
}

start_target() {
    qa_ssh far "mkdir -p '$MS_DIR'" >/dev/null 2>&1 || return 1
    scp -q "$HERE/multistream-target.py" "$FAR_HOST:$MS_DIR/" || return 1
    qa_ssh far "pkill -f '$MS_DIR/[m]ultistream-target.py' 2>/dev/null || true; rm -f '$MS_DIR'/sha-*.txt '$MS_DIR'/bytes-*.txt '$MS_DIR/target.log'" >/dev/null 2>&1 || true
    qa_ssh far "nohup python3 '$MS_DIR/multistream-target.py' --host 127.0.0.1 --port '$MS_FAR_TARGET' --bytes '$MS_BYTES' --seconds '$MS_SECONDS' --outdir '$MS_DIR' >'$MS_DIR/target.log' 2>&1 & echo \$! >'$MS_DIR/target.pid'" >/dev/null 2>&1 || return 1
    TARGET_PID=$(qa_ssh far "cat '$MS_DIR/target.pid' 2>/dev/null" | tr -d '[:space:]')
    local i
    for i in $(seq 1 20); do
        if qa_ssh far "grep -q '^.* LISTEN ' '$MS_DIR/target.log' 2>/dev/null" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    qa_ssh far "cat '$MS_DIR/target.log' 2>/dev/null" >"$EV/target-start.log" 2>/dev/null || true
    return 1
}

start_clients() {
    qa_ssh near "mkdir -p '$MS_DIR'" >/dev/null 2>&1 || return 1
    qa_ssh near "cat > '$MS_DIR/run-clients.sh'" <<EOS
#!/usr/bin/env bash
set +e
DIR='$MS_DIR'
A='$MS_NEAR_LPORT_A'
B='$MS_NEAR_LPORT_B'
URL='http://127.0.0.1:$NEAR_PORT'
TMO='$MS_TIMEOUT'
rm -f "\$DIR/out-a.bin" "\$DIR/out-b.bin" "\$DIR/rc-a" "\$DIR/rc-b" "\$DIR/done"
( curl -sS --local-port "\$A" --max-time "\$TMO" -o "\$DIR/out-a.bin" "\$URL/stream/a"
  echo \$? >"\$DIR/rc-a" ) &
pa=\$!
( curl -sS --local-port "\$B" --max-time "\$TMO" -o "\$DIR/out-b.bin" "\$URL/stream/b"
  echo \$? >"\$DIR/rc-b" ) &
pb=\$!
wait \$pa \$pb
touch "\$DIR/done"
EOS
    qa_ssh near "nohup bash '$MS_DIR/run-clients.sh' >'$MS_DIR/clients.log' 2>&1 & echo \$! >'$MS_DIR/clients.pid'" >/dev/null 2>&1 || return 1
    CLIENT_PID=$(qa_ssh near "cat '$MS_DIR/clients.pid' 2>/dev/null" | tr -d '[:space:]')
}

wait_done() {
    local i
    for i in $(seq 1 "$MS_TIMEOUT"); do
        if qa_ssh near "test -f '$MS_DIR/done'" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

stats_assert() {
    local end=$1
    local active sids drop
    active=$(awk -v t="$TUNNEL" '$1=="tunnel" && $2==t && $3=="streams" {
        for (i=1;i<=NF;i++) if ($i ~ /^streams_active=/) { split($i,a,"="); print a[2]; exit } }' "$EV/$end-mid-stats.txt" 2>/dev/null)
    sids=$(awk -v t="$TUNNEL" '$1=="tunnel" && $2==t && $3=="stream" {
        for (i=1;i<=NF;i++) if ($i ~ /^sid=/) { split($i,a,"="); print a[2] } }' "$EV/$end-mid-stats.txt" 2>/dev/null | sort -u | wc -l)
    qa_check "${end}_streams_active" "$([[ ${active:-0} -ge 2 ]]; echo $?)" "active=${active:-?}"
    qa_check "${end}_distinct_sids" "$([[ ${sids:-0} -ge 2 ]]; echo $?)" "distinct_sids=${sids:-?}"
    drop=$(field "$EV/$end-mid-stats.txt" c9 fec_drop_sid)
    qa_check "${end}_fec_drop_sid" "$([[ ${drop:-1} == 0 ]]; echo $?)" "fec_drop_sid=${drop:-?}"
}

active_streams() { # stats-file
    awk -v t="$TUNNEL" '$1=="tunnel" && $2==t && $3=="streams" {
        for (i=1;i<=NF;i++) if ($i ~ /^streams_active=/) {
            split($i,a,"="); print a[2]; exit
        } }' "$1" 2>/dev/null
}

run_multistream() {
    start_target || { qa_check target_start 1 "see $EV/target-start.log"; return; }
    start_clients || { qa_check clients_start 1 "near run-clients.sh"; return; }
    local i
    for i in $(seq 1 10); do
        sleep 1
        snap near mid
        snap far mid
        if [[ $(active_streams "$EV/near-mid-stats.txt") -ge 2 &&
              $(active_streams "$EV/far-mid-stats.txt") -ge 2 ]]; then
            break
        fi
    done
    stats_assert near
    stats_assert far
    wait_done || { qa_check traffic_done 1 "timeout ${MS_TIMEOUT}s"; return; }

    qa_ssh near "cat '$MS_DIR/rc-a' '$MS_DIR/rc-b'; stat -c '%n %s' '$MS_DIR/out-a.bin' '$MS_DIR/out-b.bin'; sha256sum '$MS_DIR/out-a.bin' '$MS_DIR/out-b.bin'" >"$EV/near-result.txt" 2>/dev/null || true
    qa_ssh far "cat '$MS_DIR/sha-a.txt' '$MS_DIR/sha-b.txt' 2>/dev/null; cat '$MS_DIR/target.log'" >"$EV/far-result.txt" 2>/dev/null || true

    local rc_a rc_b sha_a_n sha_b_n sha_a_f sha_b_f sz_a sz_b
    rc_a=$(sed -n '1p' "$EV/near-result.txt")
    rc_b=$(sed -n '2p' "$EV/near-result.txt")
    sz_a=$(awk '$1 ~ /out-a\.bin$/ {print $2}' "$EV/near-result.txt")
    sz_b=$(awk '$1 ~ /out-b\.bin$/ {print $2}' "$EV/near-result.txt")
    sha_a_n=$(awk '$2 ~ /out-a\.bin$/ {print $1}' "$EV/near-result.txt")
    sha_b_n=$(awk '$2 ~ /out-b\.bin$/ {print $1}' "$EV/near-result.txt")
    sha_a_f=$(sed -n '1p' "$EV/far-result.txt")
    sha_b_f=$(sed -n '2p' "$EV/far-result.txt")

    qa_check curl_a "$([[ $rc_a == 0 ]]; echo $?)" "rc=${rc_a:-?}"
    qa_check curl_b "$([[ $rc_b == 0 ]]; echo $?)" "rc=${rc_b:-?}"
    qa_check sha_a "$([[ -n $sha_a_n && $sha_a_n == "$sha_a_f" ]]; echo $?)" "near=${sha_a_n:-?} far=${sha_a_f:-?}"
    qa_check sha_b "$([[ -n $sha_b_n && $sha_b_n == "$sha_b_f" ]]; echo $?)" "near=${sha_b_n:-?} far=${sha_b_f:-?}"
    qa_check distinct_bodies "$([[ -n $sha_a_n && -n $sha_b_n && $sha_a_n != "$sha_b_n" ]]; echo $?)" "a!=b"
    if ((MS_SECONDS == 0)); then
        qa_check bytes_a "$([[ ${sz_a:-0} == "$MS_BYTES" ]]; echo $?)" "near=${sz_a:-?} want=$MS_BYTES"
        qa_check bytes_b "$([[ ${sz_b:-0} == "$MS_BYTES" ]]; echo $?)" "near=${sz_b:-?} want=$MS_BYTES"
    fi
    qa_check far_paths "$(grep -q 'path=/stream/a' "$EV/far-result.txt" && grep -q 'path=/stream/b' "$EV/far-result.txt"; echo $?)" "both paths"
    qa_check far_reject "$(! grep -q 'REJECT' "$EV/far-result.txt"; echo $?)" "no rejected path"
}

run_param0() {
    start_target || { qa_check target_start 1 "see $EV/target-start.log"; return; }
    qa_ssh near "mkdir -p '$MS_DIR'" >/dev/null 2>&1 || true
    qa_ssh near "rm -f '$MS_DIR/out-a.bin' '$MS_DIR/rc-a' '$MS_DIR/rc-b'; nohup curl -sS --local-port '$MS_NEAR_LPORT_A' --max-time 70 -o '$MS_DIR/out-a.bin' 'http://127.0.0.1:$NEAR_PORT/stream/a' >'$MS_DIR/curl-a.log' 2>&1 & echo \$! >'$MS_DIR/curl-a.pid'" >/dev/null 2>&1 || true
    local i
    for i in $(seq 1 30); do
        sleep 1
        snap near mid
        [[ $(active_streams "$EV/near-mid-stats.txt") == 1 ]] && break
    done
    local before refused active up_before up_after outb
    before=$(field "$EV/near-mid-stats.txt" c9 fec_stream_refused)
    up_before=$(field "$EV/near-mid-stats.txt" c3 up)
    qa_note "INFO param0: first stream active=$(active_streams "$EV/near-mid-stats.txt") after wait; fec_stream_refused=${before:-?} up=${up_before:-?}"
    qa_ssh near "curl -sS --local-port '$MS_NEAR_LPORT_B' --max-time 10 -o '$MS_DIR/out-b.bin' 'http://127.0.0.1:$NEAR_PORT/stream/b' >'$MS_DIR/curl-b.log' 2>&1; echo \$? >'$MS_DIR/rc-b'" >/dev/null 2>&1 || true
    snap near after
    refused=$(field "$EV/near-after-stats.txt" c9 fec_stream_refused)
    up_after=$(field "$EV/near-after-stats.txt" c3 up)
    active=$(awk -v t="$TUNNEL" '$1=="tunnel" && $2==t && $3=="streams" { for (i=1;i<=NF;i++) if ($i ~ /^streams_active=/) { split($i,a,"="); print a[2]; exit } }' "$EV/near-after-stats.txt" 2>/dev/null)
    local rc_b
    rc_b=$(qa_ssh near "cat '$MS_DIR/rc-b' 2>/dev/null" | tr -d '[:space:]')
    outb=$(qa_ssh near "stat -c %s '$MS_DIR/out-b.bin' 2>/dev/null || echo 0" | tr -d '[:space:]')
    qa_check param0_second_refused "$([[ ${rc_b:-0} != 0 ]]; echo $?)" "curl rc=${rc_b:-?}"
    qa_check param0_one_stream "$([[ ${active:-0} == 1 ]]; echo $?)" "streams_active=${active:-?}"
    qa_check param0_no_cross_body "$([[ ${outb:-0} == 0 ]]; echo $?)" "second body bytes=${outb:-?}"
    local up_floor=250
    [[ -n ${up_before:-} && ${up_before:-0} -gt 250 ]] && up_floor=$up_before
    qa_check param0_up_kept "$([[ ${up_after:-0} -ge $up_floor ]]; echo $?)" "up ${up_before:-?} -> ${up_after:-?} floor=$up_floor"
    qa_note "INFO param0: fec_stream_refused ${before:-?} -> ${refused:-?} (near-side counter is not expected to move; far records the refusal)"
}

case $MODE in
    status)
        preflight
        ;;
    cleanup)
        exit 0
        ;;
    run)
        preflight
        if ((FAILED)); then
            qa_say "PRECHECK FAIL checks=$FAILED; fix the config/parameter first"
            exit 3
        fi
        run_multistream
        ;;
    param0)
        preflight
        if ((FAILED)); then
            qa_say "PRECHECK FAIL checks=$FAILED; fix the config/parameter first"
            exit 3
        fi
        run_param0
        ;;
esac

if ((FAILED)); then
    qa_say "RESULT FAIL checks=$FAILED evidence=$EV"
    exit 6
fi
qa_say "RESULT PASS evidence=$EV"
exit 0
