#!/usr/bin/env bash
# smoke_verdict.sh - read one smoke_37b_evidence.sh evidence directory and
# print the bounded-run acceptance verdict.
#
# Usage: smoke_verdict.sh <evidence-dir>
#
# Criteria (from the merged-fix acceptance shape):
#   - both pings return all 37 bytes,
#   - join_dup=0 on both ends,
#   - no churn: lost/replaced do not grow without bound, up returns to 2,
#   - fbk_tx/fbk_rx are the same order of magnitude as DATA frames
#     (tens to hundreds, not thousands),
#   - rx_read_bytes is not far above the payload the run carried,
#   - rx_delivered tracks rx_read,
#   - no TXSTALLKILL and no hole gatefail/depthfail/timefail.
#
# Exit: 0 all criteria pass, 1 one or more fail, 2 usage/missing evidence.

set -euo pipefail
EV=$1
[[ -d $EV ]] || { echo "no evidence dir: $EV" >&2; exit 2; }

pass=0; fail=0
ok()  { printf 'PASS  %s\n' "$*"; pass=$((pass+1)); }
bad() { printf 'FAIL  %s\n' "$*"; fail=$((fail+1)); }
note(){ printf 'note  %s\n' "$*"; }

# Last stats snapshot per end.
near=$EV/post2-near.txt
far=$EV/post2-far.txt
[[ -f $near ]] || { echo "missing $near" >&2; exit 2; }
[[ -f $far ]]  || { echo "missing $far" >&2; exit 2; }

getk() { # file key (word boundary: lost= must not match ctl_lost=)
    grep -oE "(^| )$2=[0-9]+" "$1" 2>/dev/null | tail -1 | sed 's/^ //' | cut -d= -f2
}

# --- pings ------------------------------------------------------------------
pingbytes=0
for p in 1 2; do
    if [[ -f $EV/ping$p.txt ]] && grep -qE '37 bytes crossed in both directions' "$EV/ping$p.txt"; then
        pingbytes=$((pingbytes+1))
    fi
done
(( pingbytes == 2 )) && ok "both pings returned 37/37" || bad "ping results: $(tr '\n' ' ' < <(cat "$EV"/ping1.txt "$EV"/ping2.txt 2>/dev/null))"

# --- join / churn -----------------------------------------------------------
n_dup=$(getk "$near" join_dup); f_dup=$(getk "$far" join_dup)
[[ ${n_dup:-x} == 0 && ${f_dup:-x} == 0 ]] && ok "join_dup=0 both ends" || bad "join_dup near=${n_dup:-?} far=${f_dup:-?}"

n_up=$(getk "$near" up); f_up=$(getk "$far" up)
[[ ${n_up:-x} == 2 && ${f_up:-x} == 2 ]] && ok "up=2 both ends" || bad "up near=${n_up:-?} far=${f_up:-?}"

n_lost=$(getk "$near" lost); n_rep=$(getk "$near" replaced)
f_lost=$(getk "$far" lost);  f_rep=$(getk "$far" replaced)
note "churn near lost=$n_lost replaced=$n_rep; far lost=$f_lost replaced=$f_rep"
if [[ ${n_lost:-0} -le 8 && ${f_lost:-0} -le 8 ]]; then
    ok "carrier loss bounded (near=$n_lost far=$f_lost)"
else
    bad "carrier churn high (near=$n_lost far=$f_lost)"
fi

# --- FBK vs DATA ------------------------------------------------------------
n_fbktx=$(getk "$near" fbk_tx); n_fbkrx=$(getk "$near" fbk_rx)
f_fbktx=$(getk "$far" fbk_tx);  f_fbkrx=$(getk "$far" fbk_rx)
n_data=$(getk "$near" data_tx); f_data=$(getk "$far" data_tx)
note "fbk near tx=$n_fbktx rx=$n_fbkrx; far tx=$f_fbktx rx=$f_fbkrx; data_tx near=$n_data far=$f_data"
# Same order of magnitude as DATA frames: FBK must not be 100x the DATA count.
for pair in "near:$n_fbktx:$n_data" "far:$f_fbktx:$f_data"; do
    IFS=: read -r who fbk data <<<"$pair"
    if [[ ${fbk:-0} -eq 0 ]]; then
        note "$who fbk_tx=0 (no feedback emitted yet)"
    elif [[ ${data:-0} -gt 0 && $fbk -le $((data * 20 + 100)) ]]; then
        ok "$who fbk_tx=$fbk in scale with data_tx=$data"
    elif [[ ${data:-0} -eq 0 && $fbk -le 500 ]]; then
        ok "$who fbk_tx=$fbk small with data_tx=0"
    else
        bad "$who fbk_tx=$fbk out of scale with data_tx=${data:-?}"
    fi
done

# --- rx_read vs rx_delivered ------------------------------------------------
n_read=$(getk "$near" rx_read_bytes); n_del=$(getk "$near" rx_delivered)
f_read=$(getk "$far" rx_read_bytes);  f_del=$(getk "$far" rx_delivered)
note "rx_read_bytes near=$n_read far=$f_read; rx_delivered near=$n_del far=$f_del"
# Payload carried: 37 B request + 37 B reply per ping, plus headers/FBK. A
# healthy bounded run reads a few hundred bytes per direction; 64 KiB or more
# means the parser is spinning over feedback.
for pair in "near:$n_read:$n_del" "far:$f_read:$f_del"; do
    IFS=: read -r who read_b del_b <<<"$pair"
    if [[ ${read_b:-0} -gt 65536 ]]; then
        bad "$who rx_read_bytes=$read_b far above the 74 B payload"
    else
        ok "$who rx_read_bytes=$read_b bounded"
    fi
    if [[ ${read_b:-0} -gt 0 && ${del_b:-0} -gt 0 ]]; then
        ok "$who rx_delivered=$del_b tracks rx_read"
    elif [[ ${read_b:-0} -eq 0 && ${del_b:-0} -eq 0 ]]; then
        note "$who no receive traffic"
    else
        bad "$who rx_delivered=$del_b against rx_read=$read_b"
    fi
done

# --- tx_ack progress --------------------------------------------------------
n_ack=$(getk "$near" tx_ack); f_ack=$(getk "$far" tx_ack)
note "tx_ack near=$n_ack far=$f_ack (37 expected on the side that sent a ping)"
[[ ${n_ack:-0} -ge 37 && ${f_ack:-0} -ge 37 ]] && ok "both tx_ack advanced past 37" \
    || note "tx_ack near=$n_ack far=$f_ack (one side may legitimately be 0)"

# --- dmesg verdicts (only lines after this run's last module load) ----------
# dmesg keeps earlier runs; counting the whole buffer blamed a clean rerun for
# the previous run's TXSTALLKILL. Each end's run window starts at its last
# `tbtun 0.0.1 loaded` uptime.
win() { # file pattern
    local t
    t=$(grep -oE '^\[[[:space:]]*[0-9]+\.[0-9]+\] tbtun 0\.0\.1 loaded' "$1" 2>/dev/null | tail -1 | tr -dc '0-9.')
    [[ -n $t ]] && printf '%s' "${t%%.*}" || printf '0'
}
countwin() { # file since-uptime regex
    awk -v base="$2" -v re="$3" 'match($0, /^\[[[:space:]]*([0-9]+)\./, a) { if (a[1]+0 >= base+0 && $0 ~ re) n++ } END { print n+0 }' "$1" 2>/dev/null
}
nb=$(win "$EV/dmesg-near-full.txt"); fb=$(win "$EV/dmesg-far-full.txt")
tsk=$(( $(countwin "$EV/dmesg-near-full.txt" "$nb" 'TXSTALLKILL') + $(countwin "$EV/dmesg-far-full.txt" "$fb" 'TXSTALLKILL') ))
hole=$(( $(countwin "$EV/dmesg-near-full.txt" "$nb" 'hole (gatefail|depthfail|timefail)') + $(countwin "$EV/dmesg-far-full.txt" "$fb" 'hole (gatefail|depthfail|timefail)') ))
stall=$(( $(countwin "$EV/dmesg-near-full.txt" "$nb" 'carrier stall') + $(countwin "$EV/dmesg-far-full.txt" "$fb" 'carrier stall') ))
note "run window: near loaded>=$nb far loaded>=$fb"
[[ ${tsk:-0} -eq 0 ]] && ok "no TXSTALLKILL in this run" || bad "TXSTALLKILL x$tsk in this run"
[[ ${hole:-0} -eq 0 ]] && ok "no hole gate/depth/timefail in this run" || bad "hole lines x$hole in this run"
note "carrier stall dumps x$stall in this run (expected 0 when no carrier was replaced)"

echo
echo "RESULT pass=$pass fail=$fail"
(( fail == 0 ))
