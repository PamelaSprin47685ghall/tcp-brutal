#!/usr/bin/env bash
# review/lint.sh - mechanical invariant checks for tbtun.
#
# Each check is one invariant from review/findings.md, section "Invariants a
# reviewer can check mechanically". The checks are textual/structural on
# purpose: they catch the classes of bug a normal read-through misses
# (teardown ordering, double insert, freed-object references, spec drift in
# constants) without a kernel build, root, or a module load.
#
# The three checks that need judgement are written as reusable functions so
# they can be self-tested against synthetic snippets:
#   kfree_marker_violations <file>      (linked-object free without proof)
#   pump_lock_violations <file>         (cancel that waits on pump_lock)
#   callback_lock_violations <file>     (sk_user_data read without the lock)
# The self-tests at the bottom feed each function a correct snippet and the
# failing shapes; a check that stops detecting its bug fails its self-test.
#
# kfree marker rule: a kfree of a linked pointer is legitimate only with an
# explicit marker on the line above it:
#   /* ctor-fail */      in a function whose name contains _new or _alloc and
#                        which never calls cancel_work_sync;
#   /* retire-drain */   when the same function removed the object earlier
#                        (list_del/hlist_del/<var> = NULL/sess->car[] = NULL)
#                        and no cancel_work_sync runs between removal and free.
#
# Usage: review/lint.sh [repo-root]

set -u

root=${1:-$(cd "$(dirname "$0")/.." && pwd)}
cd "$root" || { echo "lint: cannot cd to $root" >&2; exit 2; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fails=0
pass() { printf 'PASS  %-34s %s\n' "$1" "${2-}"; }
fail() { printf 'FAIL  %-34s %s\n' "$1" "${2-}"; fails=$((fails + 1)); }

# Body of a top-level function definition (column-0 name, no trailing ';').
func_body() {
    awk -v fn="$2" '
        $0 ~ "^[a-zA-Z_].*[ *]" fn "\\(" && $0 !~ /;[[:space:]]*$/ { p = 1 }
        p { print }
        p && /^}/ { exit }
    ' "$1"
}

# ---------------------------------------------------------------------------
# Checker functions (file-scoped, so the self-tests can reuse them).

# Print one line per kfree of `varpat` that has no valid marker proof.
kfree_marker_violations() {
    awk -v varpat="$2" -v refzero="${3:-0}" '
        function analyze(   i, j, prev, line, var, rmidx, ok, cb) {
            if (fn == "")
                return
            has_cancel = index(body, "cancel_work_sync") ||
                         index(body, "cancel_delayed_work_sync")
            for (i = 1; i <= n; i++) {
                if (!index(L[i], "kfree("))
                    continue
                line = L[i]
                sub(/.*kfree\(/, "", line)
                sub(/\).*/, "", line)
                var = line
                if (var !~ ("^(" varpat ")$"))
                    continue
                prev = (i > 1) ? L[i - 1] : ""
                ok = 0
                if (index(prev, "/* ctor-fail") &&
                    fn ~ /(_new|_alloc)/ && !has_cancel)
                    ok = 1
                if (refzero && fn == "tbt_carrier_free" &&
                    index(prev, "/* refcount-zero"))
                    ok = 1
                if (index(prev, "/* retire-drain")) {
                    rmidx = 0
                    for (j = i - 1; j >= 1; j--) {
                        if (index(L[j], "list_del(&" var "->node)") ||
                            index(L[j], "list_del_init(&" var "->node)") ||
                            index(L[j], "hlist_del(&" var "->node)") ||
                            index(L[j], "hlist_del_init(&" var "->node)") ||
                            index(L[j], "hlist_del_rcu(&" var "->node)") ||
                            index(L[j], var " = NULL") ||
                            (index(L[j], "sess->car[") && index(L[j], "] = NULL"))) {
                            rmidx = j
                            break
                        }
                    }
                    if (rmidx) {
                        cb = 0
                        for (j = rmidx + 1; j < i; j++)
                            if (index(L[j], "cancel_work_sync") ||
                                index(L[j], "cancel_delayed_work_sync"))
                                cb = 1
                        if (!cb)
                            ok = 1
                    }
                }
                if (!ok)
                    printf "%s:%d: %s\n", fn, N[i], L[i]
            }
            fn = ""
            n = 0
            body = ""
        }
        /^[a-zA-Z_][a-zA-Z0-9_ *]*\(/ && $0 !~ /;[[:space:]]*$/ {
            if (fn != "")
                analyze()
            line = $0
            sub(/\(.*/, "", line)
            k = split(line, a, /[ *]+/)
            fn = a[k]
            n = 0
            body = ""
        }
        fn != "" {
            L[++n] = $0
            N[n] = NR
            body = body "\n" $0
        }
        /^}/ { analyze() }
    ' "$1"
}

# Print one line per cancel that can wait on pump_lock while it is held.
pump_lock_violations() {
    awk '
        /mutex_lock\(&sess->pump_lock\)/ { depth++ }
        /mutex_unlock\(&sess->pump_lock\)/ { if (depth > 0) depth-- }
        depth > 0 && /cancel_(delayed_)?work_sync/ { printf "%d: %s\n", NR, $0 }
    ' "$1"
    ret=$(func_body "$1" tbt_carrier_retire)
    printf '%s\n' "$ret" | grep -nE 'cancel_(delayed_)?work_sync\(&c->(w_tx|w_pump)\)' |
        cut -d: -f2- | sed 's/^[[:space:]]*//'
}

# Print the name of each callback that reads sk_user_data without the lock.
callback_lock_violations() {
    for fn in tbt_ready tbt_car_ready tbt_car_write_space tbt_car_state_change \
        tbt_pub_ready tbt_pub_write_space tbt_pub_state_change; do
        body=$(func_body "$1" "$fn")
        [ -z "$body" ] && continue
        if printf '%s\n' "$body" | grep -q 'sk_user_data' &&
            ! printf '%s\n' "$body" | grep -q 'sk_callback_lock'; then
            printf '%s\n' "$fn"
        fi
    done
}

# Print a violation when tbt_carrier_new() frees before INIT_LIST_HEAD(&c->node).
carrier_ctor_order_violation() {
    body=$(func_body "$1" tbt_carrier_new)
    [ -z "$body" ] && return 0
    ni=$(printf '%s\n' "$body" | grep -nF 'INIT_LIST_HEAD(&c->node)' | head -1 | cut -d: -f1)
    nf=$(printf '%s\n' "$body" | grep -nF 'tbt_carrier_free(c)' | head -1 | cut -d: -f1)
    if [ -n "$nf" ] && { [ -z "$ni" ] || [ "$ni" -gt "$nf" ]; }; then
        printf 'tbt_carrier_new frees before INIT_LIST_HEAD(&c->node)'
    fi
}

# Session statistics counters that must stay atomic64_t (DESIGN.md §8.7).
# Keep this list in step with struct tbt_session: the declaration half of the
# check fails if one is changed back to u64, and the access half fails on a
# bare read or write. Per-carrier counters are not listed: tbt_carrier_xmit()
# is their only writer, under pump_lock.
atomic_stats_names='
    stats_gap rx_work_runs rx_drain_entries rx_drain_bytes rx_paused_skips
    rx_parse_proto rx_records rx_ready_calls rx_work_early rx_reads
    rx_read_bytes rx_resume_calls rx_read_zero rx_budget_yields rx_resume_busy rx_errs
    carrier_write_space_runs poll_rescues tx_wake_calls tx_wake_busy
    tx_work_runs carrier_ws_calls carrier_nospace_armed pump_hw_fired carrier_lost carrier_replaced
    carrier_alloc_fail connect_fail port_exhaust port_alloc_fail
    profile_mismatch http_parse_err http_status_err pad_tx drip_tx dup_bytes
    dup_used dup_suppressed holes_unrecoverable hole_checks hole_gate_fail
    hole_depth_fail hole_time_fail session_join_bad join_nonce_reject
    join_src_reject fbk_rx retx_queued sack_rx sack_unknown
    rx_reorder_overflow rx_data_frames rx_delivered_bytes pub_capture_runs
    pub_actor_calls tx_submit_calls tx_submit_bytes dbg_alloc_fail
    dbg_over_qmax dbg_copy_fail dbg_empty_len dbg_pump_runs dbg_pump_nocar
    ctl_lost retired_tx_bytes retired_data_frames retired_retx_data
    stall_paused_skips car_shell_retrans_mutated car_shell_dsack_rescued
'

# Print one line per access to one of the named session counters that is not
# wrapped in an atomic64_* call (or tbt_stat_read()). Labels such as
# `retx_queued:` and the key text inside seq_printf strings are ignored: the
# match requires `->name`, and comment-only lines are skipped.
atomic_stats_violations() {
    _file=$1
    shift
    for _name in "$@"; do
        awk -v name="$_name" '
            {
                line = $0
                sub(/^[[:space:]]+/, "", line)
                if (line ~ /^\*/ || line ~ /^\/\*/)
                    next
                gsub(/\/\*[^*]*\*\//, "", line)
                if (line !~ ("->[[:space:]]*" name "([^A-Za-z0-9_]|$)"))
                    next
                if (line ~ /atomic64_(read|set|inc|add|sub|dec|xchg|cmpxchg|add_return|inc_return|dec_return|fetch_add|fetch_sub|try_cmpxchg)[[:space:]]*\(/ ||
                    line ~ /tbt_stat_read[[:space:]]*\(/)
                    next
                printf "%s:%d: %s\n", FILENAME, FNR, $0
            }' "$_file"
    done
}

# ---------------------------------------------------------------------------
# The 16 checks.

check_pump_lock() {
    v=$(pump_lock_violations "$1/tbtun/tbt_carrier.c")
    [ -z "$v" ] && return 0
    printf '%s' "$v" | tr '\n' ';'
    return 1
}

check_sk_release() {
    # P1 moved the actual release into tbt_stream_release_socket_now(); keep
    # the same ordering rule on whichever function owns sock_release().
    body=$(func_body "$1/tbtun/tbt_sock.c" tbt_stream_release_socket_now)
    if [ -z "$body" ]; then
        body=$(func_body "$1/tbtun/tbt_sock.c" tbt_kill_work)
    fi
    n_sk=$(printf '%s\n' "$body" | grep -nF 'WRITE_ONCE(st->sk, NULL)' | head -1 | cut -d: -f1)
    n_rel=$(printf '%s\n' "$body" | grep -nF 'sock_release(sock)' | head -1 | cut -d: -f1)
    if [ -n "$n_sk" ] && [ -n "$n_rel" ] && [ "$n_sk" -lt "$n_rel" ]; then
        return 0
    fi
    printf 'st->sk cleared after sock_release (sk=%s release=%s)' "${n_sk:-?}" "${n_rel:-?}"
    return 1
}

check_stream_insert() {
    # 0.2.0: the get-or-create body is tbt_stream_get_created(); the public
    # tbt_stream_get() is a thin wrapper. Fall back to the old name so the
    # check still fails on a tree where the helper was folded back.
    body=$(func_body "$1/tbtun/tbt_session.c" tbt_stream_get_created)
    [ -n "$body" ] || body=$(func_body "$1/tbtun/tbt_session.c" tbt_stream_get)
    n=$(printf '%s\n' "$body" | grep -c 'hlist_for_each_entry')
    [ "$n" -ge 2 ] && return 0
    printf 'only %s hash lookup(s) before hlist_add_head_rcu' "$n"
    return 1
}

check_stats_rcu_sleep() {
    # seq_printf() grows the seq_file buffer with GFP_KERNEL and may sleep.
    # Sleeping inside an RCU read section stalls synchronize_rcu() and can
    # hang config teardown/rmmod (fix21 64-carrier procfs hang).
    body=$(func_body "$1/tbtun/tbt_procfs.c" tbt_show_stats)
    bad=$(printf '%s\n' "$body" | awk '
        /rcu_read_lock/ { in_rcu = 1; next }
        /rcu_read_unlock/ { in_rcu = 0; next }
        in_rcu && /seq_printf/ { print NR ": " $0 }
    ')
    [ -z "$bad" ] && return 0
    printf 'seq_printf inside RCU: %s' "$bad"
    return 1
}

check_callback_locks() {
    v=$(callback_lock_violations "$1/tbtun/tbt_carrier.c")
    v="$v $(callback_lock_violations "$1/tbtun/tbt_listen.c")"
    v="$v $(callback_lock_violations "$1/tbtun/tbt_sock.c")"
    v=$(printf '%s' "$v" | tr -s ' ' | sed 's/^ //; s/ $//')
    [ -z "$v" ] && return 0
    printf 'unlocked: %s' "$v"
    return 1
}

check_listen_order() {
    body=$(func_body "$1/tbtun/tbt_listen.c" tbt_listen_stop)
    # tbt_sk_ops_restore() clears sk_user_data under the callback lock and
    # writes the core callbacks.  Either form clears the wiring before the
    # accept work is cancelled.
    n_clr=$(printf '%s\n' "$body" | grep -nE 'sk_user_data = NULL|tbt_sk_ops_restore' | head -1 | cut -d: -f1)
    n_can=$(printf '%s\n' "$body" | grep -nE 'cancel_work_sync\(&(t->w_accept|l->w_accept)\)' | head -1 | cut -d: -f1)
    if [ -n "$n_clr" ] && [ -n "$n_can" ] && [ "$n_clr" -lt "$n_can" ]; then
        return 0
    fi
    printf 'clear=%s cancel=%s' "${n_clr:-?}" "${n_can:-?}"
    return 1
}

check_proc_entries() {
    src="$1/tbtun/tbt_procfs.c"
    bad=""
    # remove_proc_subtree() on the directory is proof for every child created
    # under it (review 34: a partial init must not remove names it never
    # created).  It only covers entries actually created under that directory,
    # so the directory assignment and the helper's parent are checked too.
    subtree=0
    if grep -q 'remove_proc_subtree(TBT_PROC_SUBDIR' "$src" &&
       grep -q 'tbt_proc_dir = proc_mkdir(TBT_PROC_SUBDIR' "$src" &&
       grep -q 'proc_create_data(name, mode, tbt_proc_dir' "$src"; then
        subtree=1
    fi
    for name in $(grep -oE '#define TBT_[A-Z_]+_NAME' "$src" | awk '{print $2}'); do
        c=$(grep -cE "(proc_create|tbt_proc_create)\($name," "$src")
        r=$(grep -cE "remove_proc_entry\($name," "$src")
        [ "$c" -eq 0 ] || [ "$r" -gt 0 ] && continue
        if [ "$subtree" -eq 0 ]; then
            bad="$bad $name"
            continue
        fi
        # A direct proc_create() must name the removed directory as parent;
        # entries made through tbt_proc_create() are covered by the helper
        # check above.
        if grep -qE "([^_a-zA-Z]|^)proc_create\($name," "$src" &&
           ! grep -qE "([^_a-zA-Z]|^)proc_create\($name, [^,]+, tbt_proc_dir," "$src"; then
            bad="$bad $name(parent)"
        fi
    done
    mk=$(grep -c 'proc_mkdir(TBT_PROC_SUBDIR' "$src")
    rm=$(grep -cE 'remove_proc_entry\(TBT_PROC_SUBDIR|remove_proc_subtree\(TBT_PROC_SUBDIR' "$src")
    [ "$mk" -gt 0 ] && [ "$rm" -eq 0 ] && bad="$bad TBT_PROC_SUBDIR"
    [ -z "$bad" ] && return 0
    printf 'created but not removed:%s' "$bad"
    return 1
}

check_frame_types() {
    bad=""
    for t in $(grep -oE 'TBT_F_[A-Z]+' "$1/tbtun/tbt_uapi.h" | sort -u); do
        name=${t#TBT_F_}
        grep -q "case $t:" "$1/tbtun/tbt_uapi.h" || bad="$bad $t(bounds)"
        grep -qE "(^|[^A-Z_])$name([^A-Z_]|$)" "$1/DESIGN.md" || bad="$bad $t(design)"
    done
    [ -z "$bad" ] && return 0
    printf 'missing:%s' "$bad"
    return 1
}

check_zero_data() {
    dmin=$(awk '/case TBT_F_DATA:/{ f = 1; next } f && /\*min/ { gsub(/[^0-9]/, ""); print; exit }' \
        "$1/tbtun/tbt_uapi.h")
    early=$(func_body "$1/tbtun/tbt_rx.c" tbt_rx_data | grep -c 'if (!len)')
    if [ "${dmin:-0}" -ge 1 ] || [ "$early" -gt 0 ]; then
        return 0
    fi
    printf 'DATA min=%s, tbt_rx_data has no !len guard' "${dmin:-0}"
    return 1
}

check_carrier_buffers() {
    f="$1/tbtun/tbt_carrier.c"
    missing=""
    grep -q 'http_hdr = kmalloc(TBT_HTTP_HDR_MAX' "$f" || missing="$missing http_hdr"
    grep -Eq 'part = kmalloc\((TBT_REC_MAX|TBT_CAR_PART_MAX)' "$f" || missing="$missing part"
    # parser and http_rx must be born zeroed. Both carry a byte count (`have`,
    # `http_have`) that the parsers use as a length, so a kmalloc()ed object
    # with a dirty slab behind it walks off its buffer. 0.2.0 never reuses a
    # carrier, so kzalloc() at birth is the reset; the close path must still
    # zero `have` and re-arm the chunk assembler for the next object.
    grep -q 'parser = kzalloc(sizeof(\*c->parser)' "$f" || missing="$missing parser"
    grep -q 'http_rx = kzalloc(sizeof(\*c->http_rx)' "$f" || missing="$missing http_rx"
    # The length must be cleared before the object can be reused/freed, and
    # the chunk state must not resume a retired stream's message.
    grep -Eq 'parser->have = 0|memset\(parser, 0, sizeof\(\*parser\)\)' "$f" ||
        missing="$missing parser-reset"
    grep -Eq 'tbt_http_rx_new_message\((c->)?http_rx\)' "$f" ||
        missing="$missing http_rx-reset"
    grep -q '!c->http_rx' "$f" || missing="$missing http_rx-guard"
    [ -z "$missing" ] && return 0
    printf 'carrier buffer allocation/guard missing:%s' "$missing"
    return 1
}

check_render_fits() {
    cat >"$tmp/render_len.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "tbt_parse.h"
int main(void)
{
    struct tbt_cfg c;
    char buf[4096];
    memset(&c, 0, sizeof c);
    c.id = ~0ull;
    c.role = TBT_ROLE_NEAR;
    c.listen.family = TBT_AF_INET;
    c.listen.u.v4 = 0xffffffff;
    c.listen_port = 0xffff;
    c.peer.family = TBT_AF_INET;
    c.peer.u.v4 = 0xffffffff;
    c.peer_port = 0xffff;
    c.n = 16;
    c.m = 0;
    c.dup = 3;
    c.profile = 2;
    c.keepalive_ms = 86400000;
    c.drip_ms = 3600000;
    c.replay = 268435456;
    c.tunnel_rate = 0xffffffffu;
    c.max_inflate_milli = 1000000;
    c.hole_budget_ms = 60000;
    c.rx_reorder_bytes = 268435456;
    c.tx_retain_bytes = 0xffffffffu;
    c.carrier_stall_ms = 3600000;
    c.portblock_lo = 65535;
    c.portblock_hi = 65535;
    c.nmap = TBT_MAP_MAX - 1;
    for (int i = 0; i < c.nmap; i++) {
        c.maps[i].listen_port = 65535;
        c.maps[i].target_port = 65535;
    }
    c.nsrc = 8;
    for (int i = 0; i < 8; i++)
    {
        c.srcaddrs[i].family = TBT_AF_INET;
        c.srcaddrs[i].u.v4 = 0xffffffff;
    }
    printf("%d\n", tbt_render_cfg(&c, buf, sizeof buf));
    return 0;
}
EOF
    lim=$(awk '/#define TBT_LINE_MAX/{ print $3; exit }' "$1/tbtun/tbt_parse.h")
    # tbt_parse_line() validates adaptive mode and portblock through the two
    # pure helpers, so the probe links the same three sources as host-test
    # (Makefile HOST_TEST_SRCS). Linking only tbt_parse.c leaves the symbols
    # unresolved even though the probe never calls the parser.
    if ! cc -std=gnu99 -O2 -I "$1/tbtun" -o "$tmp/render_len" \
        "$tmp/render_len.c" "$1/tbtun/tbt_parse.c" "$1/tbtun/tbt_map.c" \
        "$1/tbtun/tbt_portmap.c" "$1/tbtun/tbt_fec_adapt_sclc.c" 2>"$tmp/cc.err"; then
        printf 'cannot compile probe: %s' "$(head -1 "$tmp/cc.err")"
        return 1
    fi
    n=$("$tmp/render_len")
    [ "$n" -lt "$lim" ] && return 0
    printf 'maximal render %sB >= TBT_LINE_MAX %sB' "$n" "$lim"
    return 1
}

check_retain_trim() {
    # The trim may free a retain only where no frame outside tx_q can point at
    # it: under pump_lock, i.e. in the pump, not on the capture path.
    if func_body "$1/tbtun/tbt_tx.c" tbt_tx_submit | grep -q 'if (st->retain_max'; then
        printf 'retention trim runs on the capture path; a pump batch/candidate can still reference the entry'
        return 1
    fi
    blk=$(awk '/if \(st->retain_max/{ p = 1 } p { print } p && /tbt_tx_wake\(sess\)/{ exit }' \
        "$1/tbtun/tbt_tx.c")
    printf '%s\n' "$blk" | grep -qF 'fr->ret = NULL' && return 0
    printf 'trim frees retains without clearing fr->ret'
    return 1
}

check_kfree_markers() {
    # 0.2.0 proof: tbt_carrier_free() is reachable only from the last
    # tbt_carrier_put() (refcount_dec_and_test -> call_rcu) or from the
    # never-published discard path. The marker is accepted only while all
    # three links exist, so removing the refcount turns the check red.
    refzero=0
    if grep -q 'refcount_dec_and_test(&c->refs)' "$1/tbtun/tbt_carrier.c" &&
        grep -q 'call_rcu(&c->rcu, tbt_carrier_free_rcu)' "$1/tbtun/tbt_carrier.c" &&
        func_body "$1/tbtun/tbt_carrier.c" tbt_carrier_free_rcu |
            grep -q 'tbt_carrier_free(c)' &&
        func_body "$1/tbtun/tbt_carrier.c" tbt_carrier_free |
            grep -q 'tbt_session_put(c->sess)'; then
        refzero=1
    fi
    v=$(kfree_marker_violations "$1/tbtun/tbt_carrier.c" 'c' "$refzero")
    join=$(func_body "$1/tbtun/tbt_session.c" tbt_session_join)
    # A replaced carrier leaves car[] under sess->lock, keeps a reference for
    # the stats pass, and is queued on dead_release. It must never go back to
    # the deleted dead pool.
    rel=$(printf '%s\n' "$join" |
        grep -c 'list_add_tail(&old->release_node, &sess->dead_release)')
    held=$(printf '%s\n' "$join" | grep -c 'refcount_inc(&old->refs)')
    grave=$(printf '%s\n' "$join" | grep -c '&sess->dead)')
    ctor=$(carrier_ctor_order_violation "$1/tbtun/tbt_carrier.c")
    if [ -z "$v" ] && [ "$refzero" -eq 1 ] && [ "$rel" -ge 1 ] &&
        [ "$held" -ge 1 ] && [ "$grave" -eq 0 ] && [ -z "$ctor" ]; then
        return 0
    fi
    [ -n "$v" ] && printf '%s;' "$(printf '%s' "$v" | tr '\n' ';')"
    [ "$refzero" -eq 0 ] && printf 'carrier free is not proven refcount-zero;'
    [ "$rel" -lt 1 ] && printf 'join does not queue old on sess->dead_release;'
    [ "$held" -lt 1 ] && printf 'join does not hold old across the unlock;'
    [ "$grave" -gt 0 ] && printf 'join still uses the deleted sess->dead pool;'
    [ -n "$ctor" ] && printf '%s;' "$ctor"
    return 1
}

check_no_gfp_rx() {
    n=$(grep -c 'GFP_KERNEL' "$1/tbtun/tbt_rx.c")
    [ "$n" -eq 0 ] && return 0
    printf '%s GFP_KERNEL use(s) in tbt_rx.c' "$n"
    return 1
}

check_retx_snd_una() {
    # DESIGN 4.3: a frame is a retransmit candidate only after the carrier's
    # TCP acknowledged its bytes, with the RTO running from that observation.
    if grep -q 'snd_una' "$1/tbtun/tbt_tx.c" &&
        grep -q 'first_sent_seq' "$1/tbtun/tbtun.h"; then
        return 0
    fi
    printf 'no per-copy snd_una mark in the retransmit path'
    return 1
}

check_chunk_one_record() {
    bad=""
    for pat in 'rec_need != rx->chunk_size' 'rec_len != rx->chunk_size' 'rec_len != rx->rec_need'; do
        grep -qF "$pat" "$1/tbtun/tbt_http.c" || bad="$bad [$pat]"
    done
    [ -z "$bad" ] && return 0
    printf 'missing comparison(s):%s' "$bad"
    return 1
}

check_atomic_stats() {
    hdr="$1/tbtun/tbtun.h"
    bad=""
    for n in $atomic_stats_names; do
        awk -v name="$n" '
            /^struct tbt_session$/ { p = 1; next }
            p && /^};/ { p = 0 }
            p && $0 ~ ("(^|[^A-Za-z0-9_])" name "([^A-Za-z0-9_]|$)") &&
                 $0 ~ /^[[:space:]]*atomic64_t([[:space:]]|$)/ { ok = 1 }
            END { exit ok ? 0 : 1 }' "$hdr" || bad="$bad $n(decl)"
    done
    for f in "$1"/tbtun/*.c; do
        v=$(atomic_stats_violations "$f" $atomic_stats_names)
        [ -z "$v" ] || bad="$bad $(printf '%s' "$v" | tr '\n' ';')"
    done
    [ -z "$bad" ] && return 0
    printf 'not atomic64_t or bare access:%s' "$bad"
    return 1
}

run_check() {
    name=$1
    fn=$2
    out=$("$fn" "$root" 2>&1)
    rc=$?
    out=$(printf '%s' "$out" | tr '\n' ' ' | sed 's/[[:space:]]*$//')
    if [ "$rc" -eq 0 ]; then
        pass "$name" "$out"
    else
        fail "$name" "$out"
    fi
}

run_check no-pump-lock-work-cancel check_pump_lock
run_check sk-cleared-before-release check_sk_release
run_check stream-insert-rechecks check_stream_insert
run_check callbacks-lock-user-data check_callback_locks
run_check stats-rcu-no-sleep check_stats_rcu_sleep
run_check listen-clear-before-cancel check_listen_order
run_check proc-entries-removed check_proc_entries
run_check frame-types-total check_frame_types
run_check no-zero-length-data check_zero_data
run_check carrier-buffers-allocated check_carrier_buffers
run_check render-fits-line-max check_render_fits
run_check retain-trim-serialized check_retain_trim
run_check kfree-marker-proof check_kfree_markers
run_check no-gfp-kernel-in-rx check_no_gfp_rx
run_check retx-snd-una-gated check_retx_snd_una
run_check chunk-one-record check_chunk_one_record
run_check atomic-stats-atomic64 check_atomic_stats

# ---------------------------------------------------------------------------
# Self-tests: feed the checker functions snippets with known verdicts.

st_pass=0
st_fail=0
st() {
    name=$1
    shift
    if "$@"; then
        printf 'SELFTEST PASS  %s\n' "$name"
        st_pass=$((st_pass + 1))
    else
        printf 'SELFTEST FAIL  %s\n' "$name"
        st_fail=$((st_fail + 1))
    fi
}
empty() { [ -z "$1" ]; }
nonempty() { [ -n "$1" ]; }

d=$(mktemp -d)

# kfree: constructor failure, marker present, no cancel.
cat >"$d/ctor_ok.c" <<'EOF'
static struct tbt_carrier *tbt_carrier_new(struct tbt_session *sess)
{
    struct tbt_carrier *c = kzalloc(sizeof(*c), 0);
    if (!c)
        return NULL;
    /* ctor-fail: never linked */
    kfree(c);
    return NULL;
}
EOF
# kfree: constructor failure, marker missing.
cat >"$d/ctor_no_marker.c" <<'EOF'
static struct tbt_carrier *tbt_carrier_new(struct tbt_session *sess)
{
    struct tbt_carrier *c = kzalloc(sizeof(*c), 0);
    kfree(c);
    return NULL;
}
EOF
# kfree: drain, removal before the free, no cancel between.
cat >"$d/drain_ok.c" <<'EOF'
void tbt_carriers_stop(struct tbt_session *sess)
{
    struct tbt_carrier *c, *tmp;
    list_for_each_entry_safe(c, tmp, &sess->dead, node) {
        list_del_init(&c->node);
        /* retire-drain: removed first */
        kfree(c);
    }
}
EOF
# kfree: drain marker, but a cancel runs between removal and free.
cat >"$d/drain_cancel.c" <<'EOF'
void tbt_carriers_stop(struct tbt_session *sess)
{
    struct tbt_carrier *c, *tmp;
    list_for_each_entry_safe(c, tmp, &sess->dead, node) {
        list_del_init(&c->node);
        cancel_work_sync(&c->w_tx);
        /* retire-drain: removed first */
        kfree(c);
    }
}
EOF
# kfree: drain marker, but no removal at all.
cat >"$d/drain_no_removal.c" <<'EOF'
void tbt_carriers_stop(struct tbt_session *sess)
{
    struct tbt_carrier *c;
    /* retire-drain: removed first */
    kfree(c);
}
EOF

v1=$(kfree_marker_violations "$d/ctor_ok.c" 'c')
v2=$(kfree_marker_violations "$d/ctor_no_marker.c" 'c')
v3=$(kfree_marker_violations "$d/drain_ok.c" 'c')
v4=$(kfree_marker_violations "$d/drain_cancel.c" 'c')
v5=$(kfree_marker_violations "$d/drain_no_removal.c" 'c')
st kfree-ctor-ok empty "$v1"
st kfree-ctor-missing-marker nonempty "$v2"
st kfree-drain-ok empty "$v3"
st kfree-drain-cancel nonempty "$v4"
st kfree-drain-no-removal nonempty "$v5"

# pump_lock: cancel before the lock is safe; under the lock is not; retire
# must not cancel w_tx/w_pump.
cat >"$d/pump_ok.c" <<'EOF'
void tbt_carriers_stop(struct tbt_session *sess)
{
    cancel_work_sync(&c->w_tx);
    mutex_lock(&sess->pump_lock);
    mutex_unlock(&sess->pump_lock);
}
void tbt_carrier_retire(struct tbt_session *sess, struct tbt_carrier *c)
{
    cancel_delayed_work_sync(&c->w_poll);
}
EOF
cat >"$d/pump_under_lock.c" <<'EOF'
void tbt_carriers_stop(struct tbt_session *sess)
{
    mutex_lock(&sess->pump_lock);
    cancel_work_sync(&c->w_tx);
    mutex_unlock(&sess->pump_lock);
}
EOF
cat >"$d/pump_retire.c" <<'EOF'
void tbt_carrier_retire(struct tbt_session *sess, struct tbt_carrier *c)
{
    cancel_delayed_work_sync(&c->w_pump);
}
EOF

p1=$(pump_lock_violations "$d/pump_ok.c")
p2=$(pump_lock_violations "$d/pump_under_lock.c")
p3=$(pump_lock_violations "$d/pump_retire.c")
st pump-cancel-before-lock empty "$p1"
st pump-cancel-under-lock nonempty "$p2"
st pump-retire-w_pump nonempty "$p3"

# callbacks: a user_data reader with the callback lock is safe; without is not.
cat >"$d/cb_ok.c" <<'EOF'
static void tbt_ready(struct sock *sk)
{
    read_lock_bh(&sk->sk_callback_lock);
    t = sk->sk_user_data;
    read_unlock_bh(&sk->sk_callback_lock);
}
EOF
cat >"$d/cb_bad.c" <<'EOF'
static void tbt_ready(struct sock *sk)
{
    t = sk->sk_user_data;
    schedule_work(&t->w_accept);
}
EOF

c1=$(callback_lock_violations "$d/cb_ok.c")
c2=$(callback_lock_violations "$d/cb_bad.c")
st callback-locked empty "$c1"
st callback-unlocked nonempty "$c2"

# constructor order: INIT_LIST_HEAD before tbt_carrier_free is safe; the
# reverse order unlinks a zeroed node.
cat >"$d/ctor_order_ok.c" <<'EOF'
struct tbt_carrier *tbt_carrier_new(struct tbt_session *sess)
{
    struct tbt_carrier *c = kzalloc(sizeof(*c), 0);
    INIT_LIST_HEAD(&c->node);
    if (!c->http_tx)
        tbt_carrier_free(c);
    return c;
}
EOF
cat >"$d/ctor_order_bad.c" <<'EOF'
struct tbt_carrier *tbt_carrier_new(struct tbt_session *sess)
{
    struct tbt_carrier *c = kzalloc(sizeof(*c), 0);
    if (!c->http_tx)
        tbt_carrier_free(c);
    INIT_LIST_HEAD(&c->node);
    return c;
}
EOF
o1=$(carrier_ctor_order_violation "$d/ctor_order_ok.c")
o2=$(carrier_ctor_order_violation "$d/ctor_order_bad.c")
st ctor-order-ok empty "$o1"
st ctor-order-bad nonempty "$o2"

# atomic stats: a bare increment/read fails; the atomic wrappers, a label with
# the same spelling, and a seq_printf key do not.
cat >"$d/atomic_bad.c" <<'EOF'
void f(struct tbt_session *s)
{
    s->rx_data_frames++;
    x = s->rx_errs;
}
EOF
cat >"$d/atomic_ok.c" <<'EOF'
void f(struct tbt_session *s)
{
    atomic64_inc(&s->rx_data_frames);
    atomic64_add(1, &s->rx_drain_bytes);
    x = tbt_stat_read(&s->rx_errs);
    seq_printf(m, "tunnel %llu rx_errs=%llu\n", id, tbt_stat_read(&s->rx_errs));
    goto retx_queued;
retx_queued:
    return;
}
EOF
a1=$(atomic_stats_violations "$d/atomic_bad.c" rx_data_frames rx_errs)
a2=$(atomic_stats_violations "$d/atomic_ok.c" rx_data_frames rx_drain_bytes rx_errs)
st atomic-stats-bare-access nonempty "$a1"
st atomic-stats-wrapped-access empty "$a2"

echo
printf '%d/%d checks passed, %d failed\n' "$((17 - fails))" 17 "$fails"
printf '%d/%d self-tests passed, %d failed\n' "$st_pass" "$((st_pass + st_fail))" "$st_fail"
[ "$fails" -eq 0 ] && [ "$st_fail" -eq 0 ]
