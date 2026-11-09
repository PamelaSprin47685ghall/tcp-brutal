/* tbt_rx.c - carrier bytes -> frames -> ordered delivery, and feedback.
 *
 * C3 scope: one stream per session, up to 16 carriers. Frames can arrive out
 * of order across carriers, so the receiver keeps an rb-tree keyed by the
 * stream byte offset and delivers only the contiguous run that starts at
 * `rx_next`. A hole is normal and is filled by the sender's retransmit; a hole
 * that lets the tree grow past `rx_reorder_bytes` kills the stream with
 * TBT_ST_REORDER. There is no placeholder frame: delivery never advances
 * across a gap, and a hole the sender cannot fill ends in the bounded reset
 * of tbt_rx_check_hole().
 *
 * Owns:
 *   - driving the per-carrier partial-record buffer (the buffer itself lives on
 *     struct tbt_carrier; this file must never hold module-global parse state).
 *   - validation of every record through tbt_hdr_check and the HELLO gate.
 *   - the reorder tree, duplicate rejection, and the ready-to-deliver queue.
 *   - OPEN/FIN/RST/FBK handling and the far side's stream adoption rule.
 *
 * Must never:
 *   - assume a record is fully contained in one read.
 *   - deliver across a gap. Delivery reads only rx_q, which is contiguous by
 *     construction.
 *   - allocate from a length the peer chose before checking it against
 *     TBT_PAYLOAD_MAX.
 */
#include <linux/ktime.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <net/inet_sock.h>

#include "tbtun.h"
#include "tbt_fec.h"
#include "tbt_rec.h"

/* The receiver's hole budget. Explicit config wins; auto is k x SRTT with
 * k's upper bound (5), floored at 2 s. The floor must cover the carrier TCP's
 * own RTO backoff, not one RTO: a lost retransmit already waits 200 + 400 +
 * 800 ms, and the tunnel's missing record can only be resent after the
 * carrier recovers. A 1 s floor reset probe J's stream at 1.005 s while the
 * carrier was still recovering (full A-K sequence, 1% loss + 2% reorder),
 * although the sender's own stall budget is max(5 s, 5 x RTO) and would have
 * repaired it. The receiver must not be tighter than the repair it waits for.
 * rtt_mean_us is 0 until the pump has a sample, so a fresh session gets the
 * floor. */
#define TBT_RX_HOLE_FLOOR_US 2000000ULL

static u64 tbt_rx_hole_budget_ns(struct tbt_session *sess)
{
    u32 ms = sess->tunnel->cfg.hole_budget_ms;
    u64 us;

    if (ms)
        return (u64)ms * NSEC_PER_MSEC;
    us = 5ULL * READ_ONCE(sess->rtt_mean_us);
    if (us < TBT_RX_HOLE_FLOOR_US)
        us = TBT_RX_HOLE_FLOOR_US;
    return us * NSEC_PER_USEC;
}

/* Move every tree frame that continues the contiguous run into rx_q. Called
 * with rx_lock held. */
static void tbt_rx_pull_locked(struct tbt_stream *st)
{
    for (;;)
    {
        struct rb_node *node = rb_first_cached(&st->rx_tree);
        struct tbt_frame *fr;

        if (!node)
            break;
        fr = rb_entry(node, struct tbt_frame, rb);
        if (fr->hdr.seq > st->rx_next)
            break;

        if (fr->hdr.seq + fr->len <= st->rx_next)
        {
            /* Full stale: the whole node was already delivered by another
             * copy. The node leaves the tree; rx_ooo_bytes loses the whole
             * len, not just an overlap. */
            rb_erase_cached(&fr->rb, &st->rx_tree);
            st->rx_ooo_bytes -= fr->len;
            st->rx_ooo_frames--;
            atomic64_sub(fr->len, &st->sess->rx_ooo_total);
            kfree(fr);
            continue;
        }

        if (fr->hdr.seq == st->rx_next)
        {
            /* Exact: the normal contiguous advance. */
            rb_erase_cached(&fr->rb, &st->rx_tree);
            st->rx_ooo_bytes -= fr->len;
            st->rx_ooo_frames--;
            atomic64_sub(fr->len, &st->sess->rx_ooo_total);
            list_add_tail(&fr->list, &st->rx_q);
            st->rx_queued += fr->len;
            st->rx_next = fr->hdr.seq + fr->len;
            continue;
        }

        /* Straddle: seq < rx_next < seq + len. Drop the already-delivered
         * prefix; off records it and the tail is queued. */
        {
            u32 dup = st->rx_next - fr->hdr.seq;

            rb_erase_cached(&fr->rb, &st->rx_tree);
            st->rx_ooo_bytes -= fr->len;
            st->rx_ooo_frames--;
            atomic64_sub(fr->len, &st->sess->rx_ooo_total);
            fr->off += dup;
            fr->hdr.seq = st->rx_next;
            list_add_tail(&fr->list, &st->rx_q);
            st->rx_queued += fr->len - fr->off;
            st->rx_next = fr->hdr.seq + fr->len - fr->off;
        }
    }
    /* A hole budget is per hole, not per episode: a sustained outage on one
     * path leaves the tree non-empty for its whole duration while each hole
     * is repaired within an RTO. Restart the timer when the head advances to
     * the next hole, and clear it when the tree empties. */
    if (st->hole_since_ns)
    {
        u64 stall = ktime_get_ns() - st->hole_since_ns;

        if (stall / NSEC_PER_USEC > st->rx_max_stall_us)
            st->rx_max_stall_us = stall / NSEC_PER_USEC;
        st->hole_since_ns = st->rx_ooo_frames ? ktime_get_ns() : 0;
    }
}

/* Insert one out-of-order frame. Returns false when the seq is already
 * present (a duplicate copy, which C3 cannot produce itself but a retransmit
 * racing the original can). Called with rx_lock held. */
static bool tbt_rx_tree_insert_locked(struct tbt_stream *st, struct tbt_frame *fr)
{
    struct rb_node **p = &st->rx_tree.rb_root.rb_node;
    struct rb_node *parent = NULL;
    bool leftmost = true;

    while (*p)
    {
        struct tbt_frame *o = rb_entry(*p, struct tbt_frame, rb);

        parent = *p;
        if (fr->hdr.seq < o->hdr.seq)
        {
            p = &parent->rb_left;
        }
        else if (fr->hdr.seq > o->hdr.seq)
        {
            p = &parent->rb_right;
            leftmost = false;
        }
        else
        {
            return false;
        }
    }
    rb_link_node(&fr->rb, parent, p);
    rb_insert_color_cached(&fr->rb, &st->rx_tree, leftmost);
    st->rx_ooo_bytes += fr->len;
    st->rx_ooo_frames++;
    atomic64_add(fr->len, &st->sess->rx_ooo_total);
    if (st->rx_ooo_frames > st->rx_max_depth)
        st->rx_max_depth = st->rx_ooo_frames;
    return true;
}

/* The session's stream id is adopted from the first stream frame on an
 * authenticated carrier (HELLO already proved the nonce and index). OPEN is
 * the canonical handover to a new sid; DATA/FIN/RST may adopt only the first
 * stream, or follow an old stream that is already closing. */
static int tbt_rx_bind_stream(struct tbt_carrier *c, const struct tbt_hdr *h,
                              bool open, const u8 *body, u16 len)
{
    int r = tbt_session_bind_sid(c->sess, tbt_hdr_sid(h), open, body, len);

    return r == TBT_BIND_BAD ? -EPROTO : 0;
}

static int tbt_rx_data(struct tbt_carrier *c, struct tbt_hdr *hdr, const u8 *payload, u16 len)
{
    struct tbt_session *sess = c->sess;
    struct tbt_stream *st;
    struct tbt_frame *fr;
    bool kill = false;

    if (!len)
        return 0; /* the emitter never sends one; a zero-length DATA would
                   * sit at the head of the ready queue forever */
    if (hdr->seq > U64_MAX - len)
        return -EPROTO;
    st = tbt_stream_get(sess, tbt_hdr_sid(hdr), false, false);
    if (!st)
        return 0; /* unknown/tombstoned sid: drop, do not kill the carrier */
    if (READ_ONCE(st->state) == TBT_ST_DEAD ||
        READ_ONCE(st->tombstone_until_ns))
    {
        /* Tombstone: a detached stream (state still open, sid tombstoned) must
         * not receive late DATA either; tbt_rx_bind_stream() maps BIND_DROP to
         * success, so this check is the only gate.  Drop without growing rx_q. */
        tbt_stream_put(st);
        return 0;
    }
    c->rx_frames++;
    c->rx_bytes += len;
    atomic64_inc(&sess->rx_data_frames);
    /* Feedback cadence: every 10 ms, or sooner after TBT_FBK_FRAMES. */
    if (++st->fbk_frames >= TBT_FBK_FRAMES)
    {
        st->fbk_frames = 0;
        tbt_fbk_kick(sess, true);
    }
    else
    {
        tbt_fbk_kick(sess, false);
    }

    fr = kmalloc(struct_size(fr, data, len), GFP_ATOMIC);
    if (!fr)
    {
        tbt_stream_put(st);
        return -ENOMEM;
    }
    INIT_LIST_HEAD(&fr->list);
    fr->len = len;
    fr->off = 0;
    /* RX frames never enter the TX path, but the frame is kmalloc()ed and a
     * bool field must never be read from stale slab bytes. Keep every flag at
     * its zero/false initial state. */
    fr->open_pending = 0;
    fr->pinned = false;
    fr->dup_copy = false;
    fr->retx_copy = false;
    fr->dropped = false;
    fr->retx_src = TBT_RETX_NONE;
    fr->path = TBT_PATH_NONE;
    fr->gen = 0;
    fr->retries = 0;
    fr->retry_at_ns = 0;
    bitmap_zero(fr->avoid_mask, TBT_MAX_CARRIERS_LIMIT);
    fr->ret = NULL;
    fr->st = NULL;
    fr->hdr = *hdr;
    if (len)
        memcpy(fr->data, payload, len);

    spin_lock_bh(&st->rx_lock);
    if (hdr->seq < st->rx_next)
    {
        u32 dup = st->rx_next - hdr->seq;

        if (hdr->seq + len <= st->rx_next)
        {
            /* Already delivered (a retransmit that lost the race). */
            spin_unlock_bh(&st->rx_lock);
            kfree(fr);
            tbt_stream_put(st);
            return 0;
        }
        /* Overlapping retransmit: the prefix [seq, rx_next) was delivered by
         * another copy; keep only the tail. off records the clipped prefix. */
        fr->off = dup;
        fr->hdr.seq = st->rx_next;
        list_add_tail(&fr->list, &st->rx_q);
        st->rx_queued += len - dup;
        st->rx_next = fr->hdr.seq + (len - dup);
        tbt_rx_pull_locked(st);
        if (hdr->flags & TBT_FF_LOW_PRIO)
            atomic64_inc(&sess->dup_used);
    }
    else if (hdr->seq == st->rx_next)
    {
        list_add_tail(&fr->list, &st->rx_q);
        st->rx_queued += len;
        st->rx_next = hdr->seq + len;
        tbt_rx_pull_locked(st);
        if (hdr->flags & TBT_FF_LOW_PRIO)
            atomic64_inc(&sess->dup_used);
    }
    else if (!tbt_rx_tree_insert_locked(st, fr))
    {
        spin_unlock_bh(&st->rx_lock);
        kfree(fr);
        tbt_stream_put(st);
        return 0;
    }
    else
    {
        /* The insert above already bumped rx_ooo_frames, so test the timer,
         * not the count. */
        if (!st->hole_since_ns)
            st->hole_since_ns = ktime_get_ns();
        if (hdr->flags & TBT_FF_LOW_PRIO)
            atomic64_inc(&sess->dup_used);
        /* Two bounds: one stream's tree, and the session's whole tree. The
         * per-stream limit alone would let 128 streams hold 128 x the
         * configured budget; the session total is what makes the memory
         * bound explainable. A concurrent insert on another stream can
         * overshoot by at most one frame before this check fires. */
        if (st->rx_ooo_bytes > st->rx_limit ||
            atomic64_read(&sess->rx_ooo_total) >
                sess->tunnel->cfg.rx_reorder_bytes)
            kill = true;
    }
    /* The ack is the contiguous *received* offset: it bounds the sender's
     * window (and therefore this tree), and it advances even while delivery to
     * the application is slow. Delivery backpressure is the pause rule below,
     * which stops reading the carriers and therefore stops this ack. */
    WRITE_ONCE(sess->rx_ack, st->rx_next);
    WRITE_ONCE(st->sack_dirty, true);
    spin_unlock_bh(&st->rx_lock);

    if (kill)
    {
        atomic64_inc(&sess->rx_reorder_overflow);
        atomic64_inc(&sess->stats_gap);
        tbt_stream_kill(st, TBT_ST_REORDER);
    }
    tbt_rx_flush(st);
    tbt_stream_put(st);
    return 0;
}

/* Parse one FBK body: per-path delivered offset, frame count and RTT. The
 * delivered offset is what the sender's window and retention use; every path
 * carries the same value, so "every live path acked past" reduces to the
 * session-wide maximum, which is monotone. */
/* FEC v6 delivery: the decoder has already put the block's packets in order.
 * Reuse the stream's rx_q/rx_tree so the output socket and the FIN path see
 * one byte space; `off` is the packet's byte offset (seq * S). */
int tbt_rx_fec_deliver(struct tbt_stream *st, u64 off, const u8 *data, u16 len)
{
    struct tbt_frame *fr;
    bool queued = false;

    if (!st || !len)
        return 0;
    if (off > U64_MAX - len)
        return -EPROTO;
    fr = kmalloc(struct_size(fr, data, len), GFP_ATOMIC);
    if (!fr)
        return -ENOMEM;
    INIT_LIST_HEAD(&fr->list);
    fr->len = len;
    fr->off = 0;
    memset(&fr->hdr, 0, sizeof(fr->hdr));
    fr->hdr.seq = off;
    fr->st = NULL;
    memcpy(fr->data, data, len);

    spin_lock_bh(&st->rx_lock);
    {
        int step = tbt_fec_deliver_step(off, len, st->rx_next);

        if (step)
        {
            spin_unlock_bh(&st->rx_lock);
            kfree(fr);
            return step;
        }
    }
    if (off == st->rx_next)
    {
        list_add_tail(&fr->list, &st->rx_q);
        st->rx_queued += len;
        st->rx_next = off + len;
        tbt_rx_pull_locked(st);
        queued = true;
    }
    else
    {
        /* The FEC walk is ordered: the next share starts exactly at rx_next.
         * A partial overlap is a duplicate prefix (return 1); a hole means
         * the caller's cursor ran ahead of the socket (return 2). Neither may
         * be queued in the reorder tree: that would deliver out of order and
         * silently corrupt the stream. */
        int step = off < st->rx_next ? 1 : 2;

        spin_unlock_bh(&st->rx_lock);
        kfree(fr);
        return step;
    }
    spin_unlock_bh(&st->rx_lock);
    if (queued)
        tbt_stream_schedule(st, &st->w_drain, TBT_STREAM_F_SCHED_DRAIN);
    return 0;
}

static int tbt_rx_fbk(struct tbt_carrier *c, struct tbt_hdr *hdr, const u8 *body)
{
    struct tbt_session *sess = c->sess;
    struct tbt_stream *st;
    bool paused = false;
    u32 nr = hdr->path_ack_count;
    u32 i;

    for (i = 0; i < nr; i++)
    {
        const u8 *e = body + i * TBT_FBK_ENTRY_LEN;
        struct tbt_carrier *pc;
        u8 path = e[TBT_FBK_PATH] & 0xf;
        u8 gen = e[TBT_FBK_PATH] >> 4;
        u8 state = e[TBT_FBK_RXSTATE];
        u64 uack = tbt_le64(e + TBT_FBK_UACK);
        u32 cnt = tbt_le32(e + TBT_FBK_RECVCNT);
        u32 rtt = tbt_le32(e + TBT_FBK_RTT);

        if (path >= sess->n_car)
            continue;
        pc = tbt_carrier_get(sess, path);
        /* A replacement carrier is a new path with an empty sequence space;
         * feedback about the old generation must not touch it. */
        if (!pc || (pc->gen & 0xf) != gen)
        {
            if (pc)
                tbt_carrier_put(pc);
            continue;
        }
        if (uack > READ_ONCE(pc->uack))
        {
            WRITE_ONCE(pc->uack, uack);
            WRITE_ONCE(pc->uack_ns, ktime_get_ns());
        }
        /* loss_ppm is the confirmed-loss ratio computed by the sender's hole
         * check: records written below this path's high-water that the peer's
         * recv_cnt does not account for. A frame still in our own socket queue
         * is above the high-water and cannot bias it. */
        WRITE_ONCE(pc->recv_cnt, cnt);
        if (rtt)
        {
            if (!pc->rtt_min_us || rtt < pc->rtt_min_us)
                pc->rtt_min_us = rtt;
            pc->rtt_us = pc->rtt_us ? (pc->rtt_us * 7 + rtt) / 8 : rtt;
        }
        if (state == 2)
            paused = true;
        tbt_carrier_put(pc);
    }
    atomic64_inc(&sess->fbk_rx);
    st = tbt_stream_get(sess, tbt_hdr_sid(hdr), false, false);
    if (st)
    {
        if (READ_ONCE(st->peer_paused) && !paused)
            st->pause_cleared_ns = ktime_get_ns();
        WRITE_ONCE(st->peer_paused, paused);
        WRITE_ONCE(sess->peer_paused, paused); /* proc/control mirror */
        tbt_tx_hole_check(sess, st);
        tbt_stream_put(st);
    }
    return 0;
}

static int tbt_rx_frame(struct tbt_carrier *c, struct tbt_hdr *hdr, const u8 *payload, u16 len)
{
    struct tbt_session *sess = c->sess;
    struct tbt_stream *st;

    /* Every frame carries the sender's receive offset for the opposite
     * direction; only the current stream may consume it. */
    if (tbt_hdr_sid(hdr))
        tbt_tx_ack(sess, c, hdr->uack, tbt_hdr_sid(hdr));

    switch (hdr->type)
    {
    case TBT_F_DATA:
        if (tbt_rx_bind_stream(c, hdr, false, NULL, 0))
            return -EPROTO;
        return tbt_rx_data(c, hdr, payload, len);

    case TBT_F_OPEN:
        /* The body starts with the 32-byte group bitmap the flow admission
         * uses; the trailing u16 is the target port bind checks and the far
         * side authorizes. */
        return tbt_rx_bind_stream(c, hdr, true, payload, len);

    case TBT_F_FIN:
        if (tbt_rx_bind_stream(c, hdr, false, NULL, 0))
            return -EPROTO;
        c->rx_bytes += len;
        /* FIN body: flow-local packet range, little-endian. A peer that names
         * an end before its base is refused before any stream state is
         * touched. */
        {
            u64 pkt_end = tbt_le64(payload);
            u64 pkt_base = tbt_le64(payload + 8);

            if (pkt_end < pkt_base)
            {
                atomic64_inc(&sess->fec_fin_bad);
                return -EPROTO;
            }
        }
        st = tbt_stream_get(sess, tbt_hdr_sid(hdr), false, false);
        if (!st)
            return 0;
        spin_lock_bh(&st->rx_lock);
        if (!st->rx_fin_seen)
        {
            st->rx_fin_seen = true;
            st->rx_fin_seq = hdr->seq;
        }
        spin_unlock_bh(&st->rx_lock);
        /* FEC delivers whole blocks; if the final block's parity and LAST
         * flag were both lost, the FIN offset is the only remaining statement
         * of how many packets the stream holds. */
        if (sess->fec)
            tbt_fec_rx_fin(sess, st, hdr->seq, tbt_le64(payload),
                           tbt_le64(payload + 8));
        WRITE_ONCE(st->sack_dirty, true);
        /* The drain shuts the write side once rx_next reaches the FIN's seq
         * and every earlier byte has been delivered. */
        tbt_sock_kick(st);
        /* A FIN is also the close timer's input: the FBK tick runs
         * tbt_tx_close_check() and the FIN retry. It may have stopped while
         * the session had no stream, so wake it here. */
        tbt_fbk_kick(sess, false);
        tbt_stream_put(st);
        return 0;

    case TBT_F_RST:
        if (tbt_rx_bind_stream(c, hdr, false, NULL, 0))
            return -EPROTO;
        tbt_stream_kill_by_sid(sess, tbt_hdr_sid(hdr), TBT_ST_DEAD);
        return 0;

    case TBT_F_RETIRE:
        /* The near is about to replace carrier `body[0]` at generation
         * `hdr->seq`; free the far-side slot now so the replacement's HELLO
         * joins instead of returning EALREADY. */
        if (hdr->body_len != 1)
            return -EPROTO;
        tbt_session_retire(sess, payload[0], (u8)hdr->seq);
        return 0;

    case TBT_F_FBK:
        return tbt_rx_fbk(c, hdr, payload);

    case TBT_F_SACK:
        if (hdr->body_len != 1)
            return -EPROTO;
        st = tbt_stream_get(sess, tbt_hdr_sid(hdr), false, false);
        if (!st)
        {
            atomic64_inc(&sess->sack_unknown);
            return 0;
        }
        atomic64_inc(&sess->sack_rx);
        if (READ_ONCE(st->peer_paused) && payload[0] != 2)
            st->pause_cleared_ns = ktime_get_ns();
        WRITE_ONCE(st->peer_paused, payload[0] == 2);
        WRITE_ONCE(sess->peer_paused, READ_ONCE(st->peer_paused));
        if (payload[0] == 3)
            tbt_stream_kill(st, TBT_ST_DEAD);
        tbt_stream_put(st);
        return 0;

    case TBT_F_PAD:
        return 0;

    default:
        /* WINDOW belongs to C5 and is refused until then, rather than
         * silently ignored: a peer that sends it is speaking a revision this
         * module does not implement. The 0x11/0x12 placeholder frames were
         * removed in TBT_CONV 0x0005 and land here too. */
        return -EPROTO;
    }
}

/* Crypto fields of a FEC v6 HELLO. The session lock serializes the one-time
 * key adoption and rx-context init: several carriers may present HELLO at the
 * same time. body_xor() is read-only after init, so the data paths do not take
 * this lock.
 *
 * `why` names the check that refused the HELLO. Every failure here is -EPROTO,
 * and a wrong key rule is indistinguishable from a wrong key on the wire
 * without it. */
static int tbt_rx_hello_crypto(struct tbt_session *sess, const u8 *body,
                               size_t len, const char **why)
{
    bool enabled = false;
    u8 key_id = TBT_CRYPTO_KEY_ID_NONE;
    u8 peer_nonce[TBT_CRYPTO_NONCE_LEN];
    u8 hello_key[TBT_CRYPTO_KEY_LEN];
    u8 key[TBT_CRYPTO_KEY_LEN];
    int err = 0;

    TBT_DIAG("hello crypto parse tunnel=%llu body_len=%zu",
             (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
             len);
    if (tbt_crypto_hello_parse(body, len, &enabled, &key_id, peer_nonce,
                               hello_key))
    {
        *why = "crypto hello parse";
        pr_warn_ratelimited("tbtun: tunnel %llu: HELLO crypto parse failed: -EPROTO\n",
                            (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0));
        return -EPROTO;
    }
    TBT_DIAG("hello crypto parsed tunnel=%llu enabled=%d key_id=%u have_key=%d key_peer=%d",
             (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
             enabled, key_id, READ_ONCE(sess->crypto_have_key),
             READ_ONCE(sess->crypto_key_peer));

    spin_lock(&sess->lock);
    if (enabled != sess->crypto)
    {
        *why = "crypto enabled mismatch";
        err = -EPROTO;
        goto out;
    }
    if (!enabled)
        goto out;

    /* A key this side owns (configured, or generated and announced in our own
     * HELLO) must be the one the peer uses: key_id 0. A key we adopted from
     * the peer keeps travelling with key_id 1, so it is not "local" here --
     * passing crypto_have_key made every carrier after the first one fail
     * resolve_key with -EPROTO and left the session with a single carrier
     * (measured: probe N, 315 HELLO rejections in 4 s). */
    if (tbt_crypto_resolve_key(sess->crypto_have_key && !sess->crypto_key_peer,
                               sess->crypto_key, enabled, key_id, hello_key, key))
    {
        *why = "crypto key resolve";
        err = -EPROTO;
        goto out;
    }
    if (!sess->crypto_have_key)
    {
        /* Far side: adopt the near's key. Its own TX context was waiting for
         * exactly this. */
        memcpy(sess->crypto_key, key, sizeof(sess->crypto_key));
        sess->crypto_have_key = true;
        sess->crypto_key_peer = true;
        if (tbt_crypto_init(&sess->crypto_ctx.tx, key, sess->crypto_tx_nonce, 0))
        {
            *why = "crypto tx init";
            err = -EPROTO;
            goto out;
        }
        smp_store_release(&sess->crypto_tx_ready, true);
    }
    else if (memcmp(sess->crypto_key, key, sizeof(key)) != 0)
    {
        *why = "crypto key changed";
        err = -EPROTO;
        goto out;
    }

    if (sess->crypto_ready)
    {
        /* Every carrier of one session repeats the same TX nonce. */
        if (memcmp(sess->crypto_rx_nonce, peer_nonce, sizeof(peer_nonce)) != 0)
        {
            *why = "crypto nonce changed";
            err = -EPROTO;
        }
        goto out;
    }

    memcpy(sess->crypto_rx_nonce, peer_nonce, sizeof(peer_nonce));
    if (tbt_crypto_init(&sess->crypto_ctx.rx, key, peer_nonce, 0))
    {
        *why = "crypto rx init";
        err = -EPROTO;
        goto out;
    }
    smp_store_release(&sess->crypto_ready, true);

out:
    spin_unlock(&sess->lock);
    if (err)
        pr_warn_ratelimited("tbtun: tunnel %llu: HELLO crypto resolve failed: %s (%d)\n",
                            (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
                            *why, err);
    else
        TBT_DIAG("hello crypto ready tunnel=%llu key_peer=%d tx_ready=%d rx_ready=%d",
                 (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
                 READ_ONCE(sess->crypto_key_peer),
                 READ_ONCE(sess->crypto_tx_ready),
                 READ_ONCE(sess->crypto_ready));
    return err;
}

/* Why the peer's HELLO was refused. The container is opaque otherwise: a
 * rejected HELLO and a rejected chunk both surface as -EPROTO, and the far's
 * only other evidence is "awaiting HELLO" repeating every join window. */
static int tbt_hello_bad(struct tbt_carrier *c, const char *why)
{
    if (debug)
        pr_info_ratelimited("tbtun: tunnel %llu carrier %u HELLO rejected: %s\n",
                            (unsigned long long)c->sess->tunnel->cfg.id, c->index,
                            why);
    return -EPROTO;
}

/* Validate HELLO, adopt or check the nonce, and join the far side's slot. */
int tbt_rx_hello(struct tbt_carrier *c, const struct tbt_hdr *h, const u8 *body)
{
    struct tbt_session *sess = c->sess;
    struct tbt_tunnel *t = sess->tunnel;
    u8 want_role = t->cfg.role == TBT_ROLE_NEAR ? TBT_ROLE_FAR : TBT_ROLE_NEAR;
    int err;

    if (h->body_len != (sess->fec ? TBT_HELLO_BODY_LEN_V6 : TBT_HELLO_BODY_LEN) ||
        body[0] != (sess->fec ? TBT_WIRE_VERSION_V6 : TBT_WIRE_VERSION) ||
        body[1] != want_role)
        return tbt_hello_bad(c, "length/version/role");
    /* The last HELLO byte names the container. Both ends derive the expected
     * value from the carrier index (spares of an http-kal tunnel drip), so a
     * restart with a different profile is refused here instead of being
     * mis-parsed as binary records. */
    {
        bool spare = h->path_id >= sess->n_pri;
        u8 want = tbt_carrier_profile(sess, h->path_id, spare);
        u8 prof = body[22] & 3;
        u8 gen = (body[22] >> 2) & 0xf;

        if (prof != want)
        {
            atomic64_inc(&sess->profile_mismatch);
            return tbt_hello_bad(c, "profile");
        }
        c->peer_gen = gen;
    }
    if (tbt_le16(body + 18) != TBT_PAYLOAD_MAX || tbt_le16(body + 20) != TBT_CHUNK_MAX)
        return tbt_hello_bad(c, "payload/chunk ceiling");

    if (sess->fec)
    {
        const char *why = "crypto fields";
        int cerr = tbt_rx_hello_crypto(sess, body, h->body_len, &why);

        if (cerr)
            return tbt_hello_bad(c, why);
    }

    if (t->cfg.role == TBT_ROLE_FAR)
    {
        struct sock *csk = tbt_carrier_sk_get(c);
        __be32 src;

        if (h->path_id >= sess->n_car || !csk)
        {
            if (csk)
                tbt_carrier_sk_put(csk);
            return tbt_hello_bad(c, "path id");
        }
        src = inet_sk(csk)->inet_daddr;
        spin_lock(&sess->lock);
        /* An established anchor rejects every other source, free carrier
         * index included, before the nonce is even examined: a stranger
         * cannot probe the nonce. */
        if (sess->anchor_set && src != sess->anchor_addr)
        {
            atomic64_inc(&sess->join_src_reject);
            spin_unlock(&sess->lock);
            tbt_carrier_sk_put(csk);
            return tbt_hello_bad(c, "source address");
        }
        if (sess->nonce_pinned)
        {
            if (memcmp(body + 2, sess->nonce, sizeof(sess->nonce)))
            {
                atomic64_inc(&sess->session_join_bad);
                spin_unlock(&sess->lock);
                tbt_carrier_sk_put(csk);
                return tbt_hello_bad(c, "session nonce");
            }
        }
        else if (!sess->nonce_set)
        {
            /* Only the configured near peer may define the session nonce on
             * its first carrier; otherwise any connection could name a
             * session without the secret. A far tunnel has no configured
             * peer (0), so the first HELLO defines it and every later one
             * must match; a configured peer must match its source address. */
            if (!csk || (t->cfg.peer_addr &&
                         inet_sk(csk)->inet_daddr != t->cfg.peer_addr))
            {
                atomic64_inc(&sess->join_nonce_reject);
                spin_unlock(&sess->lock);
                tbt_carrier_sk_put(csk);
                return tbt_hello_bad(c, "nonce authority");
            }
            memcpy(sess->nonce, body + 2, sizeof(sess->nonce));
            sess->nonce_set = true;
        }
        else if (memcmp(body + 2, sess->nonce, sizeof(sess->nonce)))
        {
            atomic64_inc(&sess->session_join_bad);
            spin_unlock(&sess->lock);
            tbt_carrier_sk_put(csk);
            return tbt_hello_bad(c, "session nonce");
        }
        /* Only a HELLO that passed the nonce check may fix the anchor. */
        if (!sess->anchor_set)
        {
            sess->anchor_addr = src;
            sess->anchor_set = true;
        }
        spin_unlock(&sess->lock);
        tbt_carrier_sk_put(csk);
        err = tbt_session_join(sess, c, h->path_id);
        if (err)
            return err;
    }
    else
    {
        if (memcmp(body + 2, sess->nonce, sizeof(sess->nonce)))
            return tbt_hello_bad(c, "session nonce");
        if (h->path_id != c->index)
            return tbt_hello_bad(c, "path id");
        WRITE_ONCE(c->peer_hello, true);
        /* The far joined this generation: the join_dup storm is over. */
        sess->join_retries = 0;
    }
    return 0;
}

// Parse every complete record currently in the carrier's buffer. Returns 0, a
// negative errno after a protocol error (the caller stops the carrier), or
// -EAGAIN when a record must stay buffered for the delivery ring.
/* One decoded record from the HTTP container: the first must be HELLO (the
 * same admission rule as the raw profile), the rest go to the frame handler. */
static int tbt_http_record(struct tbt_carrier *c, struct tbt_rec *rec)
{
    if (!c->saw_hello)
    {
        int err;

        if (rec->hdr.type != TBT_F_HELLO)
            return -EPROTO;
        err = tbt_rx_hello(c, &rec->hdr, rec->body);
        if (err)
            return err;
        c->saw_hello = true;
        return 0;
    }
    return tbt_rx_frame(c, &rec->hdr, rec->body, rec->body_len);
}

/* The HTTP container refused the stream: say which stage and show the bytes.
 * The raw profile dumps the record its scanner cannot accept; the HTTP profile
 * needs the same evidence, and here the failure can be in a header block, a
 * chunk size line, or a record inside a chunk, so the stage name and the
 * per-carrier phase state are part of the message. */
static int tbt_http_bad(struct tbt_carrier *c, const char *why, int err)
{
    struct tbt_parser *p = c->parser;

    if (debug)
    {
        pr_info_ratelimited("tbtun: tunnel %llu carrier %u http %s: "
                            "hdr_have=%u hdr_done=%d req_done=%d have=%zu "
                            "chunk_state=%d chunk_size=%u rec_len=%u\n",
                            (unsigned long long)c->sess->tunnel->cfg.id, c->index,
                            why, c->http_have, c->http_hdr_done, c->http_req_done,
                            p ? p->have : 0,
                            c->http_rx ? c->http_rx->chunk.state : -1,
                            c->http_rx ? c->http_rx->chunk_size : 0,
                            c->http_rx ? c->http_rx->rec_len : 0);
        /* The dump is per failing connection; a connect flood makes it a
         * console flood of its own. Keep it behind the most verbose level and
         * leave the one-line reason at `debug`. */
        if (debug >= 3 && p && p->have)
            print_hex_dump(KERN_INFO, "tbtun http rx: ", DUMP_PREFIX_OFFSET,
                           16, 1, p->buf, min_t(size_t, p->have, 64), false);
        else if (debug >= 3 && c->http_have)
            print_hex_dump(KERN_INFO, "tbtun http hdr: ", DUMP_PREFIX_OFFSET,
                           16, 1, c->http_hdr, min_t(size_t, c->http_have, 64),
                           false);
    }
    return err;
}

/* The HTTP container's receive state machine: header block (request on the far
 * side, response on the near side), then chunks decoded by the host-tested
 * assembler, then the next message's header after the terminator. Any PROTO is
 * permanent: the chunk stream cannot be resynchronised, so the carrier is
 * poisoned by the caller. */
static int tbt_http_rx_parse(struct tbt_carrier *c)
{
    struct tbt_parser *p = c->parser;
    struct tbt_session *sess = c->sess;
    bool near = sess->tunnel->cfg.role == TBT_ROLE_NEAR;

    /* Before the peer's HELLO is accepted, print the exact parser state the
     * container is about to consume. `have` is a byte count used as a memmove
     * length below; a dirty value here is the difference between a clean
     * HELLO parse and an out-of-bounds copy. Bounded by saw_hello: after the
     * handshake this line disappears. */
    if (!READ_ONCE(c->saw_hello))
        TBT_DIAG("http rx parse tunnel=%llu carrier=%u gen=%u have=%zu http_have=%u hdr_done=%d req_done=%d chunk_state=%d",
                 (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
                 c->index, c->gen, p ? p->have : 0, c->http_have,
                 c->http_hdr_done, c->http_req_done,
                 c->http_rx ? c->http_rx->chunk.state : -1);

    /* `have` is a byte count that the memmove() below uses as a length. The
     * parser buffer is kmalloc()ed, so a carrier whose initialization was
     * missed would carry a dirty value here and turn that copy into an
     * unbounded overwrite of kernel memory. Refuse the carrier instead. */
    if (!p || !c->http_rx || p->have > sizeof(p->buf) ||
        c->http_have > TBT_HTTP_HDR_MAX)
    {
        atomic64_inc(&sess->http_parse_err);
        return tbt_http_bad(c, "parser state", -EPROTO);
    }

    for (;;)
    {
        if (c->http_pending)
        {
            int err = tbt_rx_frame(c, &c->http_pending_hdr,
                                   c->http_rx->rec + TBT_HDR_LEN,
                                   c->http_pending_len);

            if (err == -EAGAIN)
                return -EAGAIN;
            c->http_pending = false;
            if (err)
                return err;
        }
        if (!c->http_hdr_done)
        {
            size_t take, hlen;
            int perr = 0;

            if (!p->have)
                return 0;
            take = min_t(size_t, p->have, TBT_HTTP_HDR_MAX - c->http_have);
            if (!take)
            {
                atomic64_inc(&sess->http_parse_err);
                return tbt_http_bad(c, "header full", -EPROTO);
            }
            memcpy(c->http_hdr + c->http_have, p->buf, take);
            c->http_have += take;
            p->have -= take;
            memmove(p->buf, p->buf + take, p->have);

            if (near)
            {
                struct tbt_http_resp resp;

                hlen = tbt_http_resp_parse(c->http_hdr, c->http_have, &resp, &perr);
                if (perr == TBT_HTTP_NEED_MORE)
                {
                    if (c->http_have == TBT_HTTP_HDR_MAX)
                    {
                        atomic64_inc(&sess->http_parse_err);
                        return tbt_http_bad(c, "response header full", -EPROTO);
                    }
                    return 0;
                }
                if (perr == TBT_HTTP_PROTO)
                {
                    atomic64_inc(&sess->http_parse_err);
                    return tbt_http_bad(c, "response header proto", -EPROTO);
                }
                if (resp.status != 200 || !resp.chunked || !resp.keep_alive)
                {
                    atomic64_inc(&sess->http_status_err);
                    return tbt_http_bad(c, "response status", -EPROTO);
                }
            }
            else
            {
                struct tbt_http_req req;

                hlen = tbt_http_req_parse(c->http_hdr, c->http_have, &req, &perr);
                if (perr == TBT_HTTP_NEED_MORE)
                {
                    if (c->http_have == TBT_HTTP_HDR_MAX)
                    {
                        atomic64_inc(&sess->http_parse_err);
                        return tbt_http_bad(c, "request header full", -EPROTO);
                    }
                    return 0;
                }
                if (perr == TBT_HTTP_PROTO)
                {
                    atomic64_inc(&sess->http_parse_err);
                    return tbt_http_bad(c, "request header proto", -EPROTO);
                }
                if (!req.chunked || !req.keep_alive)
                {
                    atomic64_inc(&sess->http_parse_err);
                    return tbt_http_bad(c, "request framing", -EPROTO);
                }
                c->http_req_done = true;
                tbt_tx_wake(sess);
            }
            if (hlen > c->http_have)
            {
                atomic64_inc(&sess->http_parse_err);
                return tbt_http_bad(c, "header length", -EPROTO);
            }
            {
                size_t extra = c->http_have - hlen;

                if (extra)
                {
                    memmove(p->buf + extra, p->buf, p->have);
                    memcpy(p->buf, c->http_hdr + hlen, extra);
                    p->have += extra;
                }
            }
            c->http_have = 0;
            c->http_hdr_done = true;
            continue;
        }
        while (p->have)
        {
            struct tbt_rec rec;
            int derr = 0;
            size_t n;

            if (sess->fec)
            {
                const u8 *frec = NULL;
                size_t flen = 0;
                int ferr = TBT_RX_NEED_MORE;

                n = tbt_http_fec_frame_decode(c->http_rx, p->buf, p->have, &frec,
                                              &flen, &ferr);
                p->have -= n;
                memmove(p->buf, p->buf + n, p->have);
                if (ferr == TBT_RX_OK)
                {
                    int err;

                    if (frec[0] >= TBT_FEC_DATA && frec[0] <= TBT_FEC_FBK)
                    {
                        if (!c->saw_hello)
                            return tbt_http_bad(c, "fec before hello", -EPROTO);
                        err = tbt_fec_rx_record(c, (u8 *)frec, flen);
                    }
                    else
                    {
                        struct tbt_rec orec;
                        int rerr = TBT_REC_PROTO;
                        size_t used = tbt_rec_scan(frec, flen, &orec, &rerr);

                        if (rerr != TBT_REC_OK || used != flen)
                            return tbt_http_bad(c, "record scan", -EPROTO);
                        err = tbt_http_record(c, &orec);
                    }
                    if (err)
                        return err;
                    continue;
                }
                if (ferr == TBT_RX_NEED_MORE)
                {
                    if (tbt_http_rx_done(c->http_rx))
                    {
                        tbt_http_rx_new_message(c->http_rx);
                        c->http_hdr_done = false;
                        c->http_have = 0;
                    }
                    break;
                }
                atomic64_inc(&sess->http_parse_err);
                return tbt_http_bad(c, "fec chunk", -EPROTO);
            }

            n = tbt_http_frame_decode(c->http_rx, p->buf, p->have, &rec, &derr);
            p->have -= n;
            memmove(p->buf, p->buf + n, p->have);
            if (derr == TBT_RX_OK)
            {
                int err = tbt_http_record(c, &rec);

                if (err == -EAGAIN)
                {
                    c->http_pending = true;
                    c->http_pending_hdr = rec.hdr;
                    c->http_pending_len = rec.body_len;
                    return -EAGAIN;
                }
                if (err)
                    return err;
                continue;
            }
            if (derr == TBT_RX_NEED_MORE)
            {
                if (tbt_http_rx_done(c->http_rx))
                {
                    tbt_http_rx_new_message(c->http_rx);
                    c->http_hdr_done = false;
                    c->http_have = 0;
                }
                break;
            }
            atomic64_inc(&sess->http_parse_err);
            return tbt_http_bad(c, "chunk", -EPROTO);
        }
        if (c->http_hdr_done)
            return 0;
    }
}

static int tbt_rx_parse_raw(struct tbt_carrier *c)
{
    struct tbt_parser *p = c->parser;

    for (;;)
    {
        struct tbt_rec rec;
        int st = TBT_REC_NEED_MORE;
        size_t used;

        /* FEC v6 records start with 0x30..0x35; the old 32-byte header never
         * does. Parse the 40-byte record here and leave the old scanner
         * untouched, so a mixed carrier (HELLO/OPEN/FIN + FEC shares) works. */
        if (p->have && tbt_fec_record_type(p->buf[0]))
        {
            size_t need = 0;
            int fst = tbt_fec_record_len(p->buf, p->have, &need);
            int ferr;

            if (fst == TBT_REC_NEED_MORE)
                break;
            if (fst == TBT_REC_PROTO)
                return -EPROTO;
            if (!c->saw_hello)
                return -EPROTO;
            ferr = tbt_fec_rx_record(c, p->buf, need);
            /* The record was handed to the FEC layer, which may have
             * decrypted it in place. Consume it even on error: retrying the
             * same bytes would XOR the plaintext again and turn one real
             * error into a stream of payload-CRC failures. */
            p->have -= need;
            memmove(p->buf, p->buf + need, p->have);
            if (ferr)
                return ferr;
            continue;
        }

        used = tbt_rec_scan(p->buf, p->have, &rec, &st);
        if (st == TBT_REC_NEED_MORE)
            break;
        if (st == TBT_REC_PROTO)
        {
            if (debug)
            {
                struct tbt_hdr bad;

                tbt_hdr_decode(p->buf, &bad);
                pr_info_ratelimited("tbtun: carrier proto error: "
                                    "type=%u flags=%u path=%u ackc=%u ackm=%u len=%u "
                                    "seq=%llu sid=%u:%u r2=%u conv=%u\n",
                                    bad.type, bad.flags, bad.path_id,
                                    bad.path_ack_count, bad.path_ack_mask,
                                    bad.body_len, bad.seq, bad.sid_hi,
                                    bad.sid_lo, bad.reserved2, bad.conv);
                if (debug >= 3)
                    print_hex_dump(KERN_INFO, "tbtun rx: ", DUMP_PREFIX_OFFSET,
                                   16, 1, p->buf,
                                   min_t(size_t, p->have, 64), false);
            }
            atomic64_inc(&c->sess->rx_parse_proto);
            return -EPROTO;
        }

        atomic64_inc(&c->sess->rx_records);

        /* The first frame on a carrier must be HELLO: it carries the session
         * nonce and the path index, and it is what authorises the peer to name
         * streams on this connection. */
        if (!c->saw_hello)
        {
            int err;

            if (rec.hdr.type != TBT_F_HELLO)
                return -EPROTO;
            err = tbt_rx_hello(c, &rec.hdr, rec.body);
            if (err)
                return err;
            c->saw_hello = true;
        }
        else
        {
            int err = tbt_rx_frame(c, &rec.hdr, rec.body, rec.body_len);

            if (err == -EAGAIN && debug >= 2)
                pr_info("tbtun: rx frame EAGAIN type=%u seq=%llu sid=%u len=%u have=%zu\n",
                        rec.hdr.type, rec.hdr.seq, rec.hdr.sid_lo, rec.body_len,
                        p->have);
            if (err == -EAGAIN)
                return -EAGAIN; // record stays buffered; carrier pauses
            if (err)
                return err;
        }
        p->have -= used;
        memmove(p->buf, p->buf + used, p->have);
    }
    return 0;
}

static int tbt_rx_parse(struct tbt_carrier *c)
{
    if (c->profile != 3)
        return tbt_http_rx_parse(c);
    return tbt_rx_parse_raw(c);
}

/* Parse the records this carrier already holds, without touching the socket.
 * The actor is only entered from tcp_read_sock()'s skb walk, so a record left
 * behind by -EAGAIN has no way to be retried once its bytes have been copied
 * out of the socket (copied_seq == rcv_nxt): sk_data_ready fires on new bytes
 * only, and the poll's copied_seq check is false by then. Called from w_rx,
 * the same single writer that runs the actor. */
int tbt_rx_parse_pending(struct tbt_carrier *c)
{
    /* The parser buffer is kmalloc()ed and the carrier reuse path keeps it.
     * A missed initialization leaves `have` dirty, and both parsers use it
     * as a length; refuse the carrier instead of walking that many bytes. */
    if (!c->parser || c->parser->have > sizeof(c->parser->buf))
        return -EPROTO;
    if (!c->parser->have)
        return 0;
    return tbt_rx_parse(c);
}

// Feed one run of a received skb through the parser. tcp_read_sock() hands the
// actor a possibly nonlinear skb: the carrier writes through tcp_sendmsg_locked
// put the payload in page frags, so `skb->data + offset` covers only the linear
// head. skb_copy_bits() is the only correct source here, and it copies straight
// into the per-carrier parse buffer, so there is no bounce buffer.
// Sleepable: the caller is w_rx, never softirq.
int tbt_rx_skb(struct tbt_carrier *c, struct sk_buff *skb, unsigned int offset,
               size_t len, size_t *consumed)
{
    struct tbt_parser *p = c->parser;
    size_t done = 0;

    /* The same upper bound tbt_rx_parse_pending() applies. `have` is a length
     * in both parsers; a dirty value larger than the buffer would make the
     * room computation below underflow to a huge `take`. */
    if (!p || p->have > sizeof(p->buf))
        return -EPROTO;

    while (done < len)
    {
        size_t take = min(len - done, sizeof(p->buf) - p->have);
        int err;

        // p->buf is full: either a record larger than the buffer (impossible
        // while TBT_PARSER_BUF == TBT_REC_MAX) or a paused record that the
        // drain has not made room for yet. Both mean "stop reading".
        if (!take)
        {
            if (debug)
            {
                struct tbt_hdr h;

                tbt_hdr_decode(p->buf, &h);
                pr_info_ratelimited("tbtun: parser stuck: have=%zu type=%u len=%u "
                                    "seq=%llu sid=%u\n",
                                    p->have, h.type, h.body_len, h.seq, h.sid_lo);
            }
            /* The buffer is full. If it holds a complete record that could
             * not be queued when it was parsed (the stream's delivery ring was
             * full), retry it now: only parsing can free the space this call
             * needs. */
            err = tbt_rx_parse(c);
            if (err == -EAGAIN)
            {
                *consumed = done;
                return -EAGAIN;
            }
            if (err)
            {
                *consumed = done;
                return err;
            }
            if (p->have == sizeof(p->buf))
            {
                *consumed = done;
                return -EAGAIN;
            }
            continue;
        }
        if (skb_copy_bits(skb, offset + done, p->buf + p->have, take))
        {
            *consumed = done;
            return -EFAULT;
        }
        p->have += take;
        done += take;

        err = tbt_rx_parse(c);
        if (err == -EAGAIN)
        {
            *consumed = done;
            return -EAGAIN;
        }
        if (err)
        {
            *consumed = done;
            return err;
        }
    }
    *consumed = done;
    return 0;
}

// Bytes are queued by tbt_rx_data(); delivery happens in tbt_sock.c's w_drain
// via tcp_sendmsg_locked on the stream's socket. This function only applies the
// pause rule and nudges the drain.
//
// Pause rule: while rx_q holds more than TBT_RX_RING_MAX bytes, the stream is
// paused and the carriers stop reading, so TCP's receive window closes and the
// far side slows down. The sender's delivered-offset window keeps the queue
// below this mark in normal operation; reaching it is a safety net, not the
// flow-control mechanism.
void tbt_rx_check_hole(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;
    u64 now, age = 0;
    u32 budget_bytes;
    bool kill = false;

    spin_lock_bh(&st->rx_lock);
    /* Read the clock under the lock: another carrier's RX work can set a
     * newer hole_since_ns between an unlocked read and the subtraction, and
     * the unsigned wrap then looks like a multi-minute stall (measured:
     * rx_max_stall_us=1.27e9 on probe C's carrier loss). */
    now = ktime_get_ns();
    if (!st->hole_since_ns || now <= st->hole_since_ns)
    {
        spin_unlock_bh(&st->rx_lock);
        return;
    }
    /* While the application is not draining, the hole cannot fill by anything
     * the network does. Slide the timer so the budget measures network
     * stalls, not application backpressure. The mark is the same one the FBK
     * reports as rx_state=2: if we tell the peer we are backed up, our own
     * deadline must agree, or the budget expires while the peer is correctly
     * holding its retransmits (measured: bench n=4 strict, rxq 26.5 MB,
     * rx_max_stall 4.93 s, holes_unrecoverable 1). */
    if (READ_ONCE(st->paused) || st->rx_queued >= TBT_RX_PAUSED_MARK)
    {
        st->hole_since_ns = now;
        spin_unlock_bh(&st->rx_lock);
        return;
    }
    if ((now - st->hole_since_ns) / NSEC_PER_USEC > st->rx_max_stall_us)
        st->rx_max_stall_us = (now - st->hole_since_ns) / NSEC_PER_USEC;
    /* The byte budget is the reorder bound (or 64 KiB, whichever is larger):
     * a hole that lets the tree reach the reorder bound has already failed. */
    budget_bytes = sess->tunnel->cfg.rx_reorder_bytes;
    if (budget_bytes < TBT_HOLE_BUDGET_BYTES)
        budget_bytes = TBT_HOLE_BUDGET_BYTES;
    if (now - st->hole_since_ns > tbt_rx_hole_budget_ns(sess) ||
        st->rx_ooo_bytes > budget_bytes)
    {
        age = now - st->hole_since_ns;
        st->hole_since_ns = 0;
        kill = true;
    }
    spin_unlock_bh(&st->rx_lock);
    if (kill)
    {
        if (debug >= 2)
            pr_info("tbtun: RXHOLEKILL sid=%llu age_us=%llu ooo=%u limit=%u queued=%u\n",
                    (unsigned long long)st->sid, age / NSEC_PER_USEC,
                    st->rx_ooo_bytes, budget_bytes, st->rx_queued);
        atomic64_inc(&sess->holes_unrecoverable);
        tbt_stream_kill(st, TBT_ST_HOLE);
    }
}

/* The peer has closed its write side (its FIN is in), so no more bytes will
 * arrive; the ready queue can only shrink.  If it does not, the output socket
 * has stopped accepting bytes -- the target or application stopped reading --
 * and the FIN that ends the handshake can never be delivered.  The stream
 * would then pin both ends and the output socket until the session dies.
 *
 * The test is progress, not queue depth: a slow reader that keeps consuming
 * is never cut, and one large frame in flight is not a stall.  The check is
 * gated on rx_fin_seen so ordinary backpressure before a close is never
 * treated as a dead peer.  The FBK tick is the only caller, so the marks need
 * no lock. */
void tbt_rx_deliver_stall_check(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;
    u64 delivered = READ_ONCE(st->rx_delivered);
    u64 now = ktime_get_ns();

    if (READ_ONCE(st->state) == TBT_ST_DEAD ||
        !READ_ONCE(st->rx_fin_seen) || READ_ONCE(st->rx_fin_done) ||
        !READ_ONCE(st->rx_queued))
    {
        st->deliver_mark = delivered;
        st->deliver_since_ns = 0;
        return;
    }
    if (delivered != st->deliver_mark)
    {
        st->deliver_mark = delivered;
        st->deliver_since_ns = 0;
        return;
    }
    atomic64_inc(&sess->dbg_deliver_checks);
    if (!st->deliver_since_ns)
    {
        st->deliver_since_ns = now;
        /* Rule out a missed wake before calling the target dead. */
        tbt_sock_kick(st);
        return;
    }
    if (now > st->deliver_since_ns &&
        now - st->deliver_since_ns > tbt_tx_wait_budget_ns(sess, st))
    {
        if (debug >= 2)
            pr_info("tbtun: DELIVERTIMEOUT sid=%llu rxq=%u delivered=%llu age_us=%llu\n",
                    (unsigned long long)st->sid, READ_ONCE(st->rx_queued),
                    delivered,
                    (now - st->deliver_since_ns) / NSEC_PER_USEC);
        st->deliver_since_ns = 0;
        atomic64_inc(&sess->deliver_timeouts);
        tbt_stream_kill(st, TBT_ST_DEAD);
    }
}

int tbt_rx_flush(struct tbt_stream *st)
{
    bool pause;

    spin_lock_bh(&st->rx_lock);
    pause = st->rx_queued >= TBT_RX_RING_MAX;
    if (pause)
        WRITE_ONCE(st->paused, true);
    spin_unlock_bh(&st->rx_lock);

    tbt_rx_check_hole(st);
    tbt_sock_kick(st);
    return 0;
}

void tbt_rx_free_queue(struct tbt_stream *st)
{
    struct tbt_frame *fr, *tmp;
    struct rb_node *node;

    spin_lock(&st->rx_lock);
    list_for_each_entry_safe(fr, tmp, &st->rx_q, list)
    {
        list_del(&fr->list);
        kfree(fr);
    }
    while ((node = rb_first_cached(&st->rx_tree)))
    {
        fr = rb_entry(node, struct tbt_frame, rb);
        rb_erase_cached(&fr->rb, &st->rx_tree);
        kfree(fr);
    }
    atomic64_sub(st->rx_ooo_bytes, &st->sess->rx_ooo_total);
    st->rx_queued = 0;
    st->rx_ooo_bytes = 0;
    st->rx_ooo_frames = 0;
    spin_unlock(&st->rx_lock);
}
