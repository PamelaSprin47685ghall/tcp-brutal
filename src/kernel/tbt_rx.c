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
#include "tbt_tx_acct.h"

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

/* The largest authenticated HELLO generation a fixed emitter can name:
 * floor(2^63 / NSEC_PER_USEC), i.e. the microsecond value of the largest
 * positive signed 64-bit nanosecond clock. Both pre-1970 wrap classes land
 * at or above this bound: the old kernel cast nanoseconds to u64 before
 * dividing and stays below 2^64/1000 with bit 63 clear, the old client cast
 * a negative seconds count to ~U64_MAX. A top-bit test misses the kernel
 * class. The emitter in tbt_session.c caps its output one below this limit,
 * so refusing every value >= limit rejects the wrap class without touching
 * any fixed peer. */
#define TBT_RX_HELLO_EPOCH_LIMIT ((1ULL << 63) / NSEC_PER_USEC)

/* The receiver's effective reorder budget mirrors the sender's automatic
 * window cap in tbt_tx_window(): max(cfg.rx_reorder_bytes,
 * tbt_tx_reorder_floor(n_car)). tbt_tx_window_update() must not rewrite the
 * parsed tunnel config, because /proc/net/tbtun/config renders it and
 * `cat config > backup; cat backup > config` would pin the grown value.
 * An explicit tx_window_bytes is checked against cfg.rx_reorder_bytes by the
 * parser, so it needs no floor. */
static u64 tbt_rx_reorder_budget(const struct tbt_session *sess)
{
    const struct tbt_cfg *cfg = &sess->tunnel->cfg;
    u64 budget = READ_ONCE(cfg->rx_reorder_bytes);

    if (!READ_ONCE(cfg->tx_window_bytes))
        budget = max_t(u64, budget,
                       tbt_tx_reorder_floor(READ_ONCE(sess->n_car)));
    return budget;
}

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
    int r = tbt_session_bind_sid(c->sess, tbt_hdr_sid(h), open, body, len,
                                 c->index);

    return r == TBT_BIND_BAD ? -EPROTO : 0;
}

/* Apply the sender's receive offset for the opposite direction. Only a frame
 * that passed its own validation may move it: tbt_tx_ack() advances tx_ack
 * and takes an RTT sample, so a record that is about to be refused (-EPROTO
 * stops the carrier) must not reach it. Every accepted frame still
 * piggybacks its uack, as before. */
static void tbt_rx_uack(struct tbt_carrier *c, const struct tbt_hdr *hdr)
{
    if (tbt_hdr_sid(hdr))
        tbt_tx_ack(c->sess, c, hdr->uack, tbt_hdr_sid(hdr));
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
    /* The frame is structurally valid from here on: tbt_rx_bind_stream()
     * already accepted the sid. Move the piggybacked ack before the
     * tombstone/state drops below, which are policy, not malformed input. */
    tbt_rx_uack(c, hdr);
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
    fr = kmalloc(struct_size(fr, data, len), GFP_ATOMIC);
    if (!fr)
    {
        /* Do not kill the carrier for a transient GFP_ATOMIC failure: the
         * carrier layer would count -ENOMEM as rx_parse_proto and close a
         * healthy subflow.
         *
         * Raw profile: the record is still in parser->buf, so -EAGAIN keeps
         * it buffered (the delivery-ring pause contract) and the 50 ms poll
         * retries it. HTTP profile: the decoded record has already left the
         * parser buffer and lives in http_rx->rec; a pending retry has no
         * wake source when the socket is empty, so drop the record (return
         * 0) and let the sender's retransmit repair it -- tx_ack does not
         * move, so the record is still in its retention ring.
         *
         * The per-carrier receive counters move below this point so a retry
         * cannot count the same record twice (they are the FBK high-water
         * the sender trusts). */
        atomic64_inc(&sess->dbg_alloc_fail);
        tbt_stream_put(st);
        return c->profile == 3 ? -EAGAIN : 0;
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
         * bound explainable. The budget is the effective one: in auto-window
         * mode the sender may run ahead by tbt_tx_reorder_floor(n_car).
         * A concurrent insert on another stream can overshoot by at most one
         * frame before this check fires. */
        {
            /* max() keeps the per-stream snapshot meaningful if the tunnel
             * config is ever replaced under a live stream, while still
             * admitting the auto-window floor. */
            u64 reorder_budget =
                max_t(u64, st->rx_limit, tbt_rx_reorder_budget(sess));

            if (st->rx_ooo_bytes > reorder_budget ||
                atomic64_read(&sess->rx_ooo_total) > reorder_budget)
                kill = true;
        }
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
        /* FEC delivery must apply the same delivery-queue pause as the ARQ
         * path (tbt_rx_flush()). Without it nothing ever sets st->paused in
         * a FEC session, so the carrier readers in tbt_car_rx_body() never
         * stop; a slow application would let rx_q grow without bound under
         * GFP_ATOMIC allocations while the FEC ACK keeps granting credit. */
        if (st->rx_queued >= TBT_RX_RING_MAX)
            WRITE_ONCE(st->paused, true);
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

/* The FEC walk reached the packet end FIN named. The FIN header carries a
 * packet frontier while the drain waits on a byte cursor, so the walk states
 * the byte end here. Until this runs, rx_fin_seq is U64_MAX and the drain
 * keeps the target's write side open; completing on the packet frontier
 * alone was the ordering bug that discarded the last short block. */
void tbt_rx_fec_fin_ready(struct tbt_stream *st, u64 bytes)
{
    bool ready = false;

    if (!st)
        return;
    spin_lock_bh(&st->rx_lock);
    if (st->rx_fin_seen && st->rx_fin_fec && !st->rx_fin_done &&
        !st->rx_fin_bytes_set)
    {
        st->rx_fin_seq = bytes;
        st->rx_fin_bytes_set = true;
        ready = true;
    }
    spin_unlock_bh(&st->rx_lock);
    if (!ready)
        return;
    pr_info_ratelimited("tbtun: fec FIN bytes sid=%llu pkt_end=%llu bytes=%llu rx_next=%llu\n",
                        (unsigned long long)st->sid,
                        (unsigned long long)st->rx_fin_pkt,
                        (unsigned long long)bytes,
                        (unsigned long long)READ_ONCE(st->rx_next));
    tbt_sock_kick(st);
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

    /* A FEC session carries stream data and feedback in the FEC plane.
     * Accepting a legacy 32-byte DATA/FBK/SACK record here would bypass the
     * FEC decoder -- and with crypto on, the DATA/PARITY ChaCha8 step and
     * payload CRC -- while still delivering bytes into the shared rx_next /
     * rx_q space. The reference client rejects the same three types; refuse
     * them before the header's uack can move tx_ack too. */
    if (sess->fec && (hdr->type == TBT_F_DATA || hdr->type == TBT_F_FBK ||
                      hdr->type == TBT_F_SACK))
        return -EPROTO;

    switch (hdr->type)
    {
    case TBT_F_DATA:
        if (tbt_rx_bind_stream(c, hdr, false, NULL, 0))
            return -EPROTO;
        return tbt_rx_data(c, hdr, payload, len);

    case TBT_F_OPEN:
        /* The body is the 36-byte v8 OPEN: group bitmap, route_slot and
         * target_port. The session binds it after the contract-order checks:
         * the bitmap admits the flow, then the route slot and target port are
         * authorized against the mapping table. */
        {
            int err = tbt_rx_bind_stream(c, hdr, true, payload, len);

            if (!err)
                tbt_rx_uack(c, hdr);
            return err;
        }

    case TBT_F_FIN:
        if (tbt_rx_bind_stream(c, hdr, false, NULL, 0))
            return -EPROTO;
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
        tbt_rx_uack(c, hdr);
        c->rx_bytes += len;
        st = tbt_stream_get(sess, tbt_hdr_sid(hdr), false, false);
        if (!st)
            return 0;
        spin_lock_bh(&st->rx_lock);
        if (!st->rx_fin_seen)
        {
            st->rx_fin_seen = true;
            if (sess->fec)
            {
                /* 0.8 FEC: the FIN header repeats the packet frontier, not
                 * the byte offset. rx_next is a byte cursor, so comparing
                 * the two would complete the FIN at once (field: 32768 >=
                 * 33,488,896) and let the drain shut the target before the
                 * last short block arrived. Wait for the walk to state the
                 * byte end instead. */
                st->rx_fin_pkt = tbt_le64(payload);
                st->rx_fin_seq = U64_MAX;
                st->rx_fin_fec = true;
                st->rx_fin_bytes_set = false;
            }
            else
            {
                st->rx_fin_seq = hdr->seq;
                st->rx_fin_bytes_set = true;
            }
            pr_info_ratelimited("tbtun: rx FIN sid=%llu seq=%llu fec=%d pkt_end=%llu carrier=%u state=%u rx_next=%llu\n",
                                (unsigned long long)st->sid,
                                (unsigned long long)hdr->seq,
                                sess->fec ? 1 : 0,
                                (unsigned long long)st->rx_fin_pkt,
                                (unsigned int)c->index,
                                READ_ONCE(st->state),
                                (unsigned long long)READ_ONCE(st->rx_next));
        }
        spin_unlock_bh(&st->rx_lock);
        /* FEC delivers whole blocks; if the final block's parity and LAST
         * flag were both lost, the FIN offset is the only remaining statement
         * of how many packets the stream holds. */
        if (sess->fec)
            tbt_fec_rx_fin(sess, st, tbt_le64(payload),
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
        tbt_rx_uack(c, hdr);
        pr_info_ratelimited("tbtun: rx RST sid=%llu seq=%llu carrier=%u\n",
                            (unsigned long long)tbt_hdr_sid(hdr),
                            (unsigned long long)hdr->seq,
                            (unsigned int)c->index);
        tbt_stream_kill_by_sid(sess, tbt_hdr_sid(hdr), TBT_ST_DEAD);
        return 0;

    case TBT_F_RETIRE:
        /* The peer has given up carrier `body[0]` at generation `hdr->seq`
         * and is about to replace it; free this side's slot now so the
         * replacement's HELLO joins instead of returning EALREADY. Either
         * role sends it: the far cannot dial, so this is how it tells the
         * near to redial a slot it has retired. */
        if (hdr->body_len != 1)
            return -EPROTO;
        tbt_rx_uack(c, hdr);
        tbt_session_retire(sess, payload[0], (u8)hdr->seq);
        return 0;

    case TBT_F_FBK:
        /* The record scanner already enforced body_len == path_ack_count x
         * TBT_FBK_ENTRY_LEN; no stream binding exists for a session-level
         * feedback record. */
        tbt_rx_uack(c, hdr);
        return tbt_rx_fbk(c, hdr, payload);

    case TBT_F_SACK:
        if (hdr->body_len != 1)
            return -EPROTO;
        tbt_rx_uack(c, hdr);
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
        {
            pr_info_ratelimited("tbtun: rx SACK close sid=%llu code=3 carrier=%u\n",
                                (unsigned long long)st->sid,
                                (unsigned int)c->index);
            tbt_stream_kill(st, TBT_ST_DEAD);
        }
        tbt_stream_put(st);
        return 0;

    case TBT_F_PAD:
        tbt_rx_uack(c, hdr);
        return 0;

    default:
        /* WINDOW belongs to C5 and is refused until then, rather than
         * silently ignored: a peer that sends it is speaking a revision this
         * module does not implement. The 0x11/0x12 placeholder frames were
         * removed in TBT_CONV 0x0005 and land here too. */
        return -EPROTO;
    }
}

/* The generation facts of one HELLO that the far gate needs before the crypto
 * step runs. It deliberately holds no key material: the gate decides the
 * anchor and the session nonce, the crypto step owns the key. */
struct tbt_hello_gen
{
    const u8 *nonce; /* the peer's 8-byte TX nonce inside the body */
    u64 epoch;       /* the peer's nonce generation */
    bool psk;        /* the tag proves the configured crypto_key= */
};

/* Crypto fields of a FEC HELLO. The session lock serializes the one-time key
 * adoption, the generation rotation and the rx-context init: several carriers
 * may present HELLO at the same time. body_xor() is read-only after init, so
 * the data paths do not take this lock.
 *
 * `why` names the check that refused the HELLO. Every failure here is -EPROTO,
 * and a wrong key rule is indistinguishable from a wrong key on the wire
 * without it. */
static int tbt_rx_hello_crypto(struct tbt_session *sess, const u8 *body,
                               size_t len, u8 path_id,
                               const struct tbt_hello_gen *g,
                               const char **why)
{
    bool enabled = false;
    u8 key_id = TBT_CRYPTO_KEY_ID_NONE;
    u8 peer_nonce[TBT_CRYPTO_NONCE_LEN];
    u8 hello_key[TBT_CRYPTO_KEY_LEN];
    u8 key[TBT_CRYPTO_KEY_LEN];
    bool reincarnated = false;
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
        tbt_memzero_explicit(hello_key, sizeof(hello_key));
        tbt_memzero_explicit(key, sizeof(key));
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
    /* A configured PSK must be proved on every HELLO, with or without the
     * data-plane cipher. This check runs before any write, so a tag-less or
     * forged HELLO neither adopts a key nor moves a nonce. Without a
     * configured key there is nothing to prove: with key_id 0 the far
     * resolves from its own key anyway, and with key_id 1 the key is in the
     * message, so a tag over it would be public. */
    if (sess->tunnel->cfg.crypto_key_set &&
        !tbt_crypto_hello_auth_ok_path(body, len,
                                       sess->tunnel->cfg.crypto_key,
                                       path_id))
    {
        atomic64_inc(&sess->hello_tag_reject);
        *why = "crypto tag";
        err = -EPROTO;
        goto out;
    }
    if (!enabled)
    {
        /* The generation high-water is session identity, not cipher state:
         * it must advance with crypto=off too, or crypto_epoch_set stays
         * false, the gate's `owner` degenerates to "any valid tag", and a
         * replayed HELLO could rename the session or move the anchor. */
        if (g->psk &&
            (!sess->crypto_epoch_set || g->epoch > sess->crypto_epoch))
        {
            sess->crypto_epoch = g->epoch;
            sess->crypto_epoch_set = true;
        }
        goto out;
    }

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
        /* Every carrier of one generation repeats the same TX nonce. A
         * different nonce is a restarted near side, and only a HELLO that
         * proved the PSK and named a strictly later generation may install
         * it: a replayed HELLO carries an older epoch, a stranger has no
         * tag, and neither can reset a keystream. */
        if (memcmp(sess->crypto_rx_nonce, peer_nonce, sizeof(peer_nonce)) == 0)
        {
            /* v11 mints a fresh epoch for every HELLO, so the authenticated
             * session-identity high-water must advance here too. Otherwise a
             * captured later HELLO of this nonce would still clear the
             * anchor-move floor below (g->epoch > crypto_epoch) after the
             * generation had already moved past it. Nonce rotation still
             * requires a strictly greater value. */
            if (g->psk &&
                (!sess->crypto_epoch_set || g->epoch > sess->crypto_epoch))
            {
                sess->crypto_epoch = g->epoch;
                sess->crypto_epoch_set = true;
            }
            goto out;
        }
        if (!g->psk ||
            (sess->crypto_epoch_set && g->epoch <= sess->crypto_epoch))
        {
            *why = "crypto nonce changed";
            err = -EPROTO;
            goto out;
        }
        memcpy(sess->crypto_rx_nonce, peer_nonce, sizeof(peer_nonce));
        if (tbt_crypto_init(&sess->crypto_ctx.rx, key, peer_nonce, 0))
        {
            *why = "crypto rx init";
            err = -EPROTO;
            goto out;
        }
        /* On the far side the downlink rotates too: its old nonce met flow
         * counters that the restarted near side will meet again from zero,
         * so reusing it could repeat a keystream. Only the far side owns the
         * downlink; on the near side this HELLO is the far's restart and
         * only the RX direction may move. The context is rebuilt in
         * w_fec_tx(), the only reader of crypto_ctx.tx, and the HELLO that
         * tbt_session_join() sends next already names the new nonce. Its
         * body epoch is a fresh tbt_hello_epoch_now(), allocated above this
         * stored nonce generation. */
        if (sess->tunnel->cfg.role == TBT_ROLE_FAR)
        {
            u64 now;

            /* Stop the pump before the announcement: a frame sealed with
             * the old nonce after the peer learned the new one would be
             * dropped. The pump re-enables the gate once it has rebuilt the
             * schedule, so the stall is one pump pass, not a timeout. */
            smp_store_release(&sess->crypto_tx_ready, false);
            get_random_bytes(sess->crypto_tx_nonce,
                             sizeof(sess->crypto_tx_nonce));
            now = tbt_hello_epoch_now();
            sess->crypto_tx_epoch =
                (now > sess->crypto_tx_epoch) ? now : sess->crypto_tx_epoch + 1;
            smp_store_release(&sess->crypto_tx_pending, true);
        }
        sess->crypto_epoch = g->epoch;
        sess->crypto_epoch_set = true;
        atomic64_inc(&sess->crypto_reincarnations);
        reincarnated = true;
        goto out;
    }

    memcpy(sess->crypto_rx_nonce, peer_nonce, sizeof(peer_nonce));
    if (tbt_crypto_init(&sess->crypto_ctx.rx, key, peer_nonce, 0))
    {
        *why = "crypto rx init";
        err = -EPROTO;
        goto out;
    }
    sess->crypto_epoch = g->epoch;
    sess->crypto_epoch_set = true;
    smp_store_release(&sess->crypto_ready, true);

out:
    spin_unlock(&sess->lock);
    /* The new incarnation's first OPEN may already be queued behind this
     * HELLO on another carrier. Retire the old incarnation's tombstones
     * before returning, so none of them can refuse it. */
    if (reincarnated && sess->tunnel->cfg.role == TBT_ROLE_FAR)
        tbt_session_reincarnate(sess);
    tbt_memzero_explicit(hello_key, sizeof(hello_key));
    tbt_memzero_explicit(key, sizeof(key));
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

/* The far side's HELLO gate: the source anchor, the session nonce and the
 * nonce authority rule. Called twice for one HELLO -- as a read-only precheck
 * before tbt_rx_hello_crypto(), and as the commit after it -- with sess->lock
 * held both times. Several carriers can pass the precheck together, so the
 * second call repeats every check against whatever the winner committed;
 * only then does it adopt an unset nonce, fix the anchor and record the peer
 * generation. Each refusal is counted exactly once, here.
 *
 * A HELLO that proved the configured PSK is the session owner: it may rename
 * the session nonce on a later generation, and it may move the anchor to a
 * new source address. That is what lets a restarted client or a re-leased
 * dynamic address rejoin instead of being refused forever. A pinned `peer=`
 * stays absolute; a pinned `nonce=` keeps its value. */
static int tbt_hello_far_gate(struct tbt_session *sess, struct tbt_carrier *c,
                              const u8 *body, const struct tbt_hello_gen *g,
                              const struct tbt_addr *src, u8 gen, bool commit,
                              const char **why)
{
    struct tbt_tunnel *t = sess->tunnel;
    bool adopt = false;
    bool rename_nonce = false;
    /* Owner: this HELLO carries the generation the session already serves
     * (the crypto nonce match covers the carriers that follow the winner of
     * a rotation) or a same/newer one. `>=` admits a same-generation HELLO,
     * which is the only way a source address can change without a restart
     * (a PPPoE re-lease); a replay of an older generation fails. With the
     * cipher on, a same-generation HELLO also matches the crypto nonce, so
     * the equality case only widens the crypto=off path where that nonce
     * does not exist. */
    bool owner = g->psk &&
                 ((sess->crypto_ready &&
                   !memcmp(sess->crypto_rx_nonce, g->nonce,
                           sizeof(sess->crypto_rx_nonce))) ||
                  !sess->crypto_epoch_set || g->epoch >= sess->crypto_epoch);

    /* An established anchor rejects every other source, free carrier index
     * included, before the nonce is even examined: a stranger cannot probe
     * the nonce. Only an owner may move it, and a configured peer= never
     * moves. A moved source must additionally prove a strictly newer
     * authenticated generation: a same-epoch HELLO replayed from another
     * address is otherwise indistinguishable from the owner and could move
     * the anchor. This deliberately refuses a same-epoch dynamic-address
     * re-lease; only a restarted peer (new epoch, PSK-proved) may roam.
     * tbt_session_join() separately requires a strict epoch advance for
     * every authenticated replacement, so a replayed same-epoch HELLO from
     * the same anchor cannot take a live slot either. */
    if (sess->anchor_set && !tbt_addr_eq(src, &sess->anchor))
    {
        if (tbt_addr_set(&t->cfg.peer) || !owner)
        {
            atomic64_inc(&sess->join_src_reject);
            *why = "source address";
            return -EPROTO;
        }
        /* Phase one only: this check runs before tbt_rx_hello_crypto(), so
         * no state has moved yet. Phase two (commit) runs after the crypto
         * step adopted this HELLO's nonce/epoch, where equality is expected
         * for the very HELLO that performed the rotation; re-testing here
         * would reject every legitimate restarted peer that changed source.
         * A replay never reaches phase two: phase one rejects it. */
        if (!commit &&
            (!g->psk || !sess->crypto_epoch_set ||
             g->epoch <= sess->crypto_epoch))
        {
            atomic64_inc(&sess->join_src_reject);
            *why = "source epoch";
            return -EPROTO;
        }
    }
    if (sess->nonce_pinned || sess->nonce_set)
    {
        if (memcmp(body + 2, sess->nonce, sizeof(sess->nonce)))
        {
            /* A restarted near side may name a new session nonce; the PSK
             * tag is what makes that name trustworthy now, so a pinned
             * nonce= stays absolute and a stranger never renames it. */
            if (sess->nonce_pinned || !owner)
            {
                atomic64_inc(&sess->session_join_bad);
                *why = "session nonce";
                return -EPROTO;
            }
            rename_nonce = true;
        }
    }
    else
    {
        /* Only the configured near peer may define the session nonce on its
         * first carrier; otherwise any connection could name a session
         * without the secret. A far tunnel has no configured peer (0), so
         * the first HELLO defines it and every later one must match; a
         * configured peer must match its source address. */
        if (tbt_addr_set(&t->cfg.peer) && !tbt_addr_eq(src, &t->cfg.peer))
        {
            atomic64_inc(&sess->join_nonce_reject);
            *why = "nonce authority";
            return -EPROTO;
        }
        adopt = true;
    }
    if (!commit)
        return 0;
    if (adopt || rename_nonce)
    {
        memcpy(sess->nonce, body + 2, sizeof(sess->nonce));
        sess->nonce_set = true;
    }
    /* Only a HELLO that passed every check may fix or move the anchor. */
    if (!sess->anchor_set || !tbt_addr_eq(src, &sess->anchor))
    {
        sess->anchor = *src;
        sess->anchor_set = true;
    }
    WRITE_ONCE(c->peer_gen, gen);
    return 0;
}

/* Validate HELLO, adopt or check the nonce, and join the far side's slot.
 *
 * Two phases keep a refused HELLO from leaving state behind. The stateless
 * checks (path id, source anchor, session nonce, nonce authority) run before
 * tbt_rx_hello_crypto(), so a wrong nonce can neither adopt the peer's key
 * nor poison the RX context. Every mutation -- the session nonce, the anchor,
 * peer_gen, peer_hello and join_retries -- happens only after the crypto step
 * accepted the HELLO. The commit repeats the anchor and nonce checks under
 * sess->lock because another carrier may have fixed them in between. */
int tbt_rx_hello(struct tbt_carrier *c, const struct tbt_hdr *h, const u8 *body)
{
    struct tbt_session *sess = c->sess;
    struct tbt_tunnel *t = sess->tunnel;
    bool far = t->cfg.role == TBT_ROLE_FAR;
    u8 want_role = far ? TBT_ROLE_NEAR : TBT_ROLE_FAR;
    struct tbt_addr src;
    struct tbt_hello_gen hg;
    u8 gen = 0;

    if (!tbt_hello_admit(sess->fec, h->body_len, body, want_role))
        return tbt_hello_bad(c, "length/version/role");
    /* The v11 generation tail. Only a configured PSK can authenticate it: the
     * gate and the crypto step both use `psk`, and with key_id 1 the key
     * travels in the body, so a tag over it would prove nothing. */
    memset(&hg, 0, sizeof(hg));
    if (sess->fec)
    {
        hg.nonce = body + TBT_CRYPTO_HELLO_NONCE;
        hg.epoch = tbt_le64(body + TBT_CRYPTO_HELLO_EPOCH);
        hg.psk = t->cfg.crypto_key_set &&
                 body[TBT_CRYPTO_HELLO_KEY_ID] == TBT_CRYPTO_KEY_ID_NONE &&
                 tbt_crypto_hello_auth_ok_path(body, h->body_len,
                                               t->cfg.crypto_key,
                                               h->path_id);
    }
    /* The authenticated generation is a session-identity high-water. A
     * pre-1970 realtime clock has two wrap classes: the old kernel cast the
     * negative ktime to u64 before dividing by NSEC_PER_USEC, landing in
     * [limit, 2*limit) with bit 63 clear, and the old client cast a negative
     * seconds count to ~U64_MAX. Both are at or above the largest value a
     * fixed emitter can name, so the limit -- not the top bit -- identifies
     * them. Refuse before the gate or the crypto step can move any state.
     * Without a PSK-proved tag the epoch authorizes nothing, so the check
     * follows the client's psk-only rule. */
    if (hg.psk && hg.epoch >= TBT_RX_HELLO_EPOCH_LIMIT)
        return tbt_hello_bad(c, "epoch range");
    /* The last HELLO byte names the container. Both ends derive the expected
     * value from the carrier index (spares of an http-kal tunnel drip), so a
     * restart with a different profile is refused here instead of being
     * mis-parsed as binary records. */
    {
        bool spare = h->path_id >= sess->n_pri;
        u8 want = tbt_carrier_profile(sess, h->path_id, spare);
        u8 prof = body[22] & 3;

        gen = (body[22] >> 2) & 0xf;
        if (prof != want)
        {
            atomic64_inc(&sess->profile_mismatch);
            return tbt_hello_bad(c, "profile");
        }
    }
    if (tbt_le16(body + 18) != TBT_PAYLOAD_MAX || tbt_le16(body + 20) != TBT_CHUNK_MAX)
        return tbt_hello_bad(c, "payload/chunk ceiling");

    /* Phase one: admission. Nothing here writes session or carrier state. */
    if (far)
    {
        struct sock *csk = tbt_carrier_sk_get(c);
        const char *why = NULL;
        int gerr;

        if (h->path_id >= sess->n_car || !csk)
        {
            if (csk)
                tbt_carrier_sk_put(csk);
            return tbt_hello_bad(c, "path id");
        }
        tbt_sk_peer_addr(csk, &src);
        tbt_carrier_sk_put(csk);
        spin_lock(&sess->lock);
        gerr = tbt_hello_far_gate(sess, c, body, &hg, &src, gen, false, &why);
        spin_unlock(&sess->lock);
        if (gerr)
            return tbt_hello_bad(c, why);
    }
    else
    {
        if (memcmp(body + 2, sess->nonce, sizeof(sess->nonce)))
            return tbt_hello_bad(c, "session nonce");
        if (h->path_id != c->index)
            return tbt_hello_bad(c, "path id");
    }

    if (sess->fec)
    {
        const char *why = "crypto fields";
        int cerr = tbt_rx_hello_crypto(sess, body, h->body_len, h->path_id,
                                       &hg, &why);

        if (cerr)
            return tbt_hello_bad(c, why);
    }

    /* Phase two: commit. Every check that guards a one-time write is repeated
     * under the lock the write itself takes. */
    if (far)
    {
        const char *why = NULL;
        int gerr;
        int err;

        spin_lock(&sess->lock);
        gerr = tbt_hello_far_gate(sess, c, body, &hg, &src, gen, true, &why);
        spin_unlock(&sess->lock);
        if (gerr)
            return tbt_hello_bad(c, why);
        /* The join reads this epoch under sess->lock; only a PSK-proved
         * HELLO may raise the path high-water. */
        c->hello_epoch = hg.epoch;
        err = tbt_session_join(sess, c, h->path_id, hg.psk);
        if (err)
            return err;
    }
    else
    {
        WRITE_ONCE(c->peer_gen, gen);
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

/* Drop the consumed prefix from the carrier's partial-record buffer. Both
 * parsers decode a whole 8 KiB run through a local read cursor and compact
 * once on return: a per-record memmove of the tail made one slice of K small
 * records O(K^2) bytes copied (a 32-byte-record flood is ~128x amplification
 * per slice). */
static void tbt_parser_compact(struct tbt_parser *p, size_t consumed)
{
    if (!consumed || consumed > p->have)
        return;
    p->have -= consumed;
    if (p->have)
        memmove(p->buf, p->buf + consumed, p->have);
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
    size_t pos = 0;
    int ret = 0;

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
            {
                ret = -EAGAIN;
                break;
            }
            c->http_pending = false;
            if (err)
            {
                ret = err;
                break;
            }
        }
        if (!c->http_hdr_done)
        {
            size_t take, hlen, avail;
            int perr = 0;

            avail = p->have - pos;
            if (!avail)
                break;
            take = min_t(size_t, avail, TBT_HTTP_HDR_MAX - c->http_have);
            if (!take)
            {
                atomic64_inc(&sess->http_parse_err);
                ret = tbt_http_bad(c, "header full", -EPROTO);
                break;
            }
            memcpy(c->http_hdr + c->http_have, p->buf + pos, take);
            c->http_have += take;
            pos += take;

            if (near)
            {
                struct tbt_http_resp resp;

                hlen = tbt_http_resp_parse(c->http_hdr, c->http_have, &resp, &perr);
                if (perr == TBT_HTTP_NEED_MORE)
                {
                    if (c->http_have == TBT_HTTP_HDR_MAX)
                    {
                        atomic64_inc(&sess->http_parse_err);
                        ret = tbt_http_bad(c, "response header full",
                                           -EPROTO);
                    }
                    break;
                }
                if (perr == TBT_HTTP_PROTO)
                {
                    atomic64_inc(&sess->http_parse_err);
                    ret = tbt_http_bad(c, "response header proto", -EPROTO);
                    break;
                }
                if (resp.status != 200 || !resp.chunked || !resp.keep_alive)
                {
                    atomic64_inc(&sess->http_status_err);
                    ret = tbt_http_bad(c, "response status", -EPROTO);
                    break;
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
                        ret = tbt_http_bad(c, "request header full",
                                           -EPROTO);
                    }
                    break;
                }
                if (perr == TBT_HTTP_PROTO)
                {
                    atomic64_inc(&sess->http_parse_err);
                    ret = tbt_http_bad(c, "request header proto", -EPROTO);
                    break;
                }
                if (!req.chunked || !req.keep_alive)
                {
                    atomic64_inc(&sess->http_parse_err);
                    ret = tbt_http_bad(c, "request framing", -EPROTO);
                    break;
                }
                c->http_req_done = true;
                tbt_tx_wake(sess);
            }
            if (ret)
                break;
            if (hlen > c->http_have)
            {
                atomic64_inc(&sess->http_parse_err);
                ret = tbt_http_bad(c, "header length", -EPROTO);
                break;
            }
            {
                size_t extra = c->http_have - hlen;

                if (extra)
                {
                    size_t remaining = p->have - pos;

                    memmove(p->buf + extra, p->buf + pos, remaining);
                    memcpy(p->buf, c->http_hdr + hlen, extra);
                    p->have = remaining + extra;
                    pos = 0;
                }
            }
            c->http_have = 0;
            c->http_hdr_done = true;
            continue;
        }
        while (p->have > pos)
        {
            const u8 *buf = p->buf + pos;
            size_t avail = p->have - pos;
            struct tbt_rec rec;
            int derr = 0;
            size_t n;

            if (sess->fec)
            {
                const u8 *frec = NULL;
                size_t flen = 0;
                int ferr = TBT_RX_NEED_MORE;

                n = tbt_http_fec_frame_decode(c->http_rx, buf, avail, &frec,
                                              &flen, &ferr);
                pos += n;
                if (ferr == TBT_RX_OK)
                {
                    int err;

                    /* The record-type gate lives in one place: the FEC range
                     * 0x30..0x35 must not fall into the old-frame scanner. */
                    if (tbt_fec_record_type(frec[0]))
                    {
                        if (!c->saw_hello)
                        {
                            ret = tbt_http_bad(c, "fec before hello",
                                               -EPROTO);
                            break;
                        }
                        err = tbt_fec_rx_record(c, (u8 *)frec, flen);
                    }
                    else
                    {
                        struct tbt_rec orec;
                        int rerr = TBT_REC_PROTO;
                        size_t used = tbt_rec_scan(frec, flen, &orec, &rerr);

                        if (rerr != TBT_REC_OK || used != flen)
                        {
                            ret = tbt_http_bad(c, "record scan", -EPROTO);
                            break;
                        }
                        err = tbt_http_record(c, &orec);
                    }
                    if (err)
                    {
                        ret = err;
                        break;
                    }
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
                ret = tbt_http_bad(c, "fec chunk", -EPROTO);
                break;
            }

            n = tbt_http_frame_decode(c->http_rx, buf, avail, &rec, &derr);
            pos += n;
            if (derr == TBT_RX_OK)
            {
                int err = tbt_http_record(c, &rec);

                if (err == -EAGAIN)
                {
                    c->http_pending = true;
                    c->http_pending_hdr = rec.hdr;
                    c->http_pending_len = rec.body_len;
                    ret = -EAGAIN;
                    break;
                }
                if (err)
                {
                    ret = err;
                    break;
                }
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
            ret = tbt_http_bad(c, "chunk", -EPROTO);
            break;
        }
        if (ret)
            break;
        if (c->http_hdr_done)
            break;
    }
    tbt_parser_compact(p, pos);
    return ret;
}

static int tbt_rx_parse_raw(struct tbt_carrier *c)
{
    struct tbt_parser *p = c->parser;
    size_t pos = 0;
    int ret = 0;

    for (;;)
    {
        const u8 *buf = p->buf + pos;
        size_t avail = p->have - pos;
        struct tbt_rec rec;
        int st = TBT_REC_NEED_MORE;
        size_t used;

        /* A FEC record's first byte is accepted by tbt_fec_record_type()
         * (DATA..FBK); the old 32-byte header never matches it. Parse the
         * record here and leave the old scanner untouched, so a mixed
         * carrier (HELLO/OPEN/FIN + FEC shares) works. */
        if (avail && tbt_fec_record_type(buf[0]))
        {
            size_t need = 0;
            int fst = tbt_fec_record_len(buf, avail, &need);
            int ferr;

            if (fst == TBT_REC_NEED_MORE)
                break;
            if (fst == TBT_REC_PROTO)
            {
                ret = -EPROTO;
                break;
            }
            if (!c->saw_hello)
            {
                ret = -EPROTO;
                break;
            }
            ferr = tbt_fec_rx_record(c, (u8 *)buf, need);
            /* The record was handed to the FEC layer, which may have
             * decrypted it in place. Consume it even on error: retrying the
             * same bytes would XOR the plaintext again and turn one real
             * error into a stream of payload-CRC failures. */
            pos += need;
            if (ferr)
            {
                ret = ferr;
                break;
            }
            continue;
        }

        used = tbt_rec_scan(buf, avail, &rec, &st);
        if (st == TBT_REC_NEED_MORE)
            break;
        if (st == TBT_REC_PROTO)
        {
            if (debug)
            {
                struct tbt_hdr bad;

                tbt_hdr_decode(buf, &bad);
                pr_info_ratelimited("tbtun: carrier proto error: "
                                    "type=%u flags=%u path=%u ackc=%u ackm=%u len=%u "
                                    "seq=%llu sid=%u:%u r2=%u conv=%u\n",
                                    bad.type, bad.flags, bad.path_id,
                                    bad.path_ack_count, bad.path_ack_mask,
                                    bad.body_len, bad.seq, bad.sid_hi,
                                    bad.sid_lo, bad.reserved2, bad.conv);
                if (debug >= 3)
                    print_hex_dump(KERN_INFO, "tbtun rx: ", DUMP_PREFIX_OFFSET,
                                   16, 1, buf,
                                   min_t(size_t, avail, 64), false);
            }
            atomic64_inc(&c->sess->rx_parse_proto);
            ret = -EPROTO;
            break;
        }

        atomic64_inc(&c->sess->rx_records);

        /* The first frame on a carrier must be HELLO: it carries the session
         * nonce and the path index, and it is what authorises the peer to name
         * streams on this connection. */
        if (!c->saw_hello)
        {
            int err;

            if (rec.hdr.type != TBT_F_HELLO)
            {
                ret = -EPROTO;
                break;
            }
            err = tbt_rx_hello(c, &rec.hdr, rec.body);
            if (err)
            {
                ret = err;
                break;
            }
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
            {
                ret = -EAGAIN; // record stays buffered; carrier pauses
                break;
            }
            if (err)
            {
                ret = err;
                break;
            }
        }
        pos += used;
    }
    tbt_parser_compact(p, pos);
    return ret;
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
    /* The byte budget is the effective reorder bound (or 64 KiB, whichever is
     * larger): a hole that lets the tree reach the reorder bound has already
     * failed. In auto-window mode the sender may legitimately run ahead by
     * tbt_tx_reorder_floor(n_car), so use the same effective value as
     * tbt_rx_data() rather than the raw configured bytes. */
    {
        u64 eff = tbt_rx_reorder_budget(sess);

        budget_bytes = eff > U32_MAX ? U32_MAX : (u32)eff;
    }
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
