/* tbt_tx.c - public bytes -> frames -> carriers, and the feedback that makes
 * the scheduler and the retention window work.
 *
 * C3 scope: one stream per session, up to 16 carriers, no duplication and no
 * prediction (C5/C6). Every DATA frame is assigned a path when the pump is
 * about to write it, using the lowest per-path inflight with a 64-frame
 * striping window; the frame is kept in a per-stream retention list until the
 * peer's delivered byte offset has passed it, so a lost carrier or a
 * ppm-dropped record can be re-sent on another path.
 *
 * Owns:
 *   - byte-offset sequence assignment (seq is the stream byte offset, not a
 *     frame ordinal, so the sender's window and the receiver's reorder bound
 *     are both measured in bytes).
 *   - the session frame queue and its memory accounting.
 *   - the pump: HELLO/OPEN handshakes, path selection, the delivered-offset
 *     send window, retention GC, and the retransmit timer.
 *   - FBK construction (one entry per live path) and the ack update it feeds.
 *
 * Must never:
 *   - call a carrier function while holding tx_lock; the pump takes the frames
 *     out of the queue, drops the lock, writes, then re-takes the lock.
 *   - free a retained frame while a retransmit copy of it is still queued: the
 *     copy is a separate object, and the pump drops any DATA frame the peer
 *     has already delivered.
 *   - grow a queue past TBT_SQ_MAX bytes or the retention past
 *     tx_retain_bytes (0 = unlimited).
 */
#include <linux/ktime.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <net/sock.h>
#include <net/tcp.h>

#include "tbtun.h"

/* A frame's payload length travels as u16 and is bounded by TBT_PAYLOAD_MAX;
 * the two must not drift apart. */
static_assert(TBT_PAYLOAD_MAX <= U16_MAX, "payload length must fit u16");

/* The send queue's hard bound. TBT_SQ_MAX is the capture backpressure line;
 * retransmit copies share the same queue and must never grow it without
 * bound. TBT_RX_RING_MAX is the *receive* delivery ring (64 MiB) and must
 * never be used here: it let the queue reach 13.8-27 MB and starved capture
 * (measured: Q7 baseline, txq_max 13.8 MB, app 1.31 Mbit/s).
 */
#define TBT_TXQ_HARD_MAX (TBT_SQ_MAX + 4 * TBT_CHUNK_MAX)

static u64 tbt_now_ns(void)
{
    return ktime_get_ns();
}

static bool tbt_retain_received(struct tbt_session *sess, struct tbt_stream *st,
                                struct tbt_retain *r);
static bool tbt_retain_tcp_done(struct tbt_session *sess, struct tbt_retain *r,
                                u64 now, u64 *earliest);
static bool tbt_retain_paths_stalled(struct tbt_session *sess, struct tbt_retain *r,
                                     u64 now, u64 rto);
static bool tbt_hole_copy_inflight(struct tbt_session *sess, struct tbt_retain *r);

static void tbt_frame_free(struct tbt_frame *fr)
{
    struct tbt_stream *st = fr->st;

    fr->st = NULL;
    if (st)
        tbt_stream_put(st); /* outside tx_lock */
    kfree(fr);
}

static struct tbt_frame *tbt_frame_alloc(u16 len, gfp_t gfp)
{
    struct tbt_frame *fr;

    fr = kmalloc(struct_size(fr, data, len), gfp);
    if (!fr)
        return NULL;
    INIT_LIST_HEAD(&fr->list);
    fr->len = len;
    fr->off = 0;
    fr->open_pending = 0;
    fr->pinned = false;
    fr->dup_copy = false;
    fr->retx_copy = false;
    fr->path = 0xff;
    fr->avoid_mask = 0;
    fr->ret = NULL;
    fr->st = NULL;
    fr->gen = 0;
    fr->retries = 0;
    fr->retry_at_ns = 0;
    return fr;
}

static struct tbt_retain *tbt_retain_alloc(u16 len, gfp_t gfp)
{
    struct tbt_retain *r;

    r = kmalloc(struct_size(r, data, len), gfp);
    if (!r)
        return NULL;
    INIT_LIST_HEAD(&r->list);
    r->len = len;
    return r;
}

/* Ask every connected carrier's writer to drain the session queue. Safe from
 * any context: it only schedules work. The pump itself must never run while a
 * caller holds a socket lock that the opposite direction needs. */
void tbt_tx_wake(struct tbt_session *sess)
{
    struct tbt_carrier *c;
    int i;

    for (i = 0; i < sess->n_car; i++)
    {
        c = READ_ONCE(sess->car[i]);
        if (c && atomic_read(&c->connected) && !atomic_read(&c->stopping))
        {
            sess->tx_wake_calls++;
            if (!schedule_work(&c->w_tx))
                sess->tx_wake_busy++;
        }
    }
}

/* Append one frame to the session queue. `head` puts it in front of every
 * queued DATA frame, which is how an OPEN overtakes bytes that were captured
 * before the carrier became usable. */
static void tbt_tx_enqueue(struct tbt_session *sess, struct tbt_frame *fr, bool head)
{
    spin_lock(&sess->tx_lock);
    if (head)
        list_add(&fr->list, &sess->tx_q);
    else
        list_add_tail(&fr->list, &sess->tx_q);
    sess->tx_queued += fr->len;
    spin_unlock(&sess->tx_lock);
}

/* True when this stream's OPEN was written on this exact carrier
 * (index, generation). OPEN announce, the pump's OPEN duplicate check and the
 * DATA gate use it; the scheduler still uses c->open_sent until S4. */
static bool tbt_stream_announced(const struct tbt_stream *st,
                                 const struct tbt_carrier *c)
{
    if (!st || !c)
        return false;
    if (!(READ_ONCE(st->announced_mask) & BIT(c->index)))
        return false;
    return READ_ONCE(st->announced_gen[c->index]) ==
           (READ_ONCE(c->gen) & 0xf);
}

/* Allocate this stream's declared-hole ring on first use. Called with
 * tx_lock held, so it must not sleep; a NULL result only skips hole
 * accounting, never the retransmit itself. */
static struct tbt_hole *tbt_stream_holes_get_locked(struct tbt_stream *st)
{
    if (!st->holes)
        st->holes = kcalloc(TBT_HOLES_MAX, sizeof(*st->holes), GFP_ATOMIC);
    return st->holes;
}

/* Queue a frame pinned to one carrier. Used for HELLO, OPEN and the per-path
 * copies of FIN/RST. The path must be a valid index, or 0xff to mean "the
 * carrier that is being replaced", which the pump never sees. `st` is the
 * owning stream for OPEN/FIN/RST/SACK/FBK, or NULL for session-level HELLO,
 * PAD and RETIRE frames. */
int tbt_tx_control_pinned(struct tbt_session *sess, struct tbt_stream *st,
                          u8 path, u8 type, u64 sid, u64 seq,
                          const u8 *body, u16 len, bool head, gfp_t gfp)
{
    struct tbt_frame *fr = tbt_frame_alloc(len, gfp);

    if (!fr)
        return -ENOMEM;
    fr->st = st;
    if (st)
        refcount_inc(&st->refs);
    fr->pinned = true;
    fr->path = path;
    fr->gen = (path < sess->n_car && READ_ONCE(sess->car[path])) ?
                  READ_ONCE(sess->car[path])->gen : 0;
    fr->hdr = (struct tbt_hdr){
        .type = type,
        .body_len = len,
        .sid_lo = (u32)sid,
        .seq = seq,
        .conv = TBT_CONV,
        .path_id = path,
    };
    fr->hdr.sid_hi = (u8)(sid >> 32);
    if (len && body)
        memcpy(fr->data, body, len);
    /* The control queue has a hard bound. A producer that gets -ENOSPC must
     * retry after the pump drains; no FIN/RST is dropped here. */
    spin_lock(&sess->tx_lock);
    if (sess->ctl_queued >= TBT_CTL_QUEUE_MAX)
    {
        spin_unlock(&sess->tx_lock);
        tbt_frame_free(fr);
        return -ENOSPC;
    }
    if (head)
        list_add(&fr->list, &sess->tx_q);
    else
        list_add_tail(&fr->list, &sess->tx_q);
    sess->tx_queued += fr->len;
    sess->ctl_queued++;
    spin_unlock(&sess->tx_lock);
    tbt_tx_wake(sess);
    return 0;
}

/* Queue this stream's OPEN on one carrier if it has not been written there
 * for the carrier's current generation. */
static void tbt_tx_announce_carrier(struct tbt_session *sess,
                                    struct tbt_stream *st,
                                    struct tbt_carrier *c, gfp_t gfp)
{
    if (!c || !atomic_read(&c->connected) || !READ_ONCE(c->hello_sent) ||
        atomic_read(&c->stopping))
        return;
    if (tbt_stream_announced(st, c))
        return;
    WRITE_ONCE(st->tx_open_sent, true);
    atomic_set(&c->up, 0);
    if (tbt_tx_control_pinned(sess, st, c->index, TBT_F_OPEN, st->sid, 0,
                              NULL, 0, true, gfp))
        atomic_set(&c->up, 1);
}

/* Announce the stream on every carrier that has sent its HELLO and has not
 * yet written this stream's OPEN. OPEN is per stream and per carrier. */
void tbt_tx_announce(struct tbt_session *sess, struct tbt_stream *st, gfp_t gfp)
{
    int i;

    WRITE_ONCE(st->tx_open_sent, true);
    for (i = 0; i < sess->n_car; i++)
        tbt_tx_announce_carrier(sess, st, READ_ONCE(sess->car[i]), gfp);
    tbt_tx_wake(sess);
}

/* FIN is stream-ordered: its seq is the byte offset after the last DATA byte,
 * so a receiver that still has a hole does not shut the write side early. One
 * copy per usable carrier, at most one per (stream, index). A second call is
 * idempotent: the per-index mask already names every copy. */
int tbt_tx_fin(struct tbt_session *sess, struct tbt_stream *st, gfp_t gfp)
{
    u64 seq;
    int i, queued = 0, err = 0;

    spin_lock(&sess->tx_lock);
    /* Freeze the offset once: a retry must name the same byte, even if a late
     * capture submitted more DATA after the first request. */
    if (!st->tx_fin_seq_set)
    {
        st->tx_fin_seq = st->tx_seq;
        st->tx_fin_seq_set = true;
    }
    seq = st->tx_fin_seq;
    spin_unlock(&sess->tx_lock);
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);

        /* A carrier that has sent HELLO can carry this stream.  It may be
         * momentarily up==0 while its OPEN is being enqueued; the pump leaves
         * a pinned frame queued until the carrier is up again, so queue the
         * FIN anyway instead of dropping it silently. */
        if (!c || !READ_ONCE(c->hello_sent) || atomic_read(&c->stopping))
            continue;
        if (test_and_set_bit(i, &st->tx_fin_mask))
            continue;
        /* Set the in-flight bit before the frame can be seen by the pump: the
         * pump clears it on the write, and a stale bit would hide the loss of
         * the last copy. */
        set_bit(i, &st->tx_fin_inflight);
        err = tbt_tx_control_pinned(sess, st, (u8)i, TBT_F_FIN, st->sid,
                                    seq, NULL, 0, false, gfp);
        if (err)
        {
            clear_bit(i, &st->tx_fin_inflight);
            clear_bit(i, &st->tx_fin_mask);
            if (err == -ENOMEM)
                sess->dbg_alloc_fail++;
            continue;
        }
        queued++;
    }
    if (queued)
        WRITE_ONCE(st->tx_fin_queued, true);
    tbt_tx_wake(sess);
    /* The number of copies actually enqueued: 0 means no carrier was usable,
     * and the caller must not treat the FIN as sent. */
    return queued ? queued : err;
}

/* RST is not stream-ordered; one copy per live carrier is enough for one of
 * them to arrive. The per-index mask makes a second tbt_tx_rst() a no-op. */
int tbt_tx_rst(struct tbt_session *sess, struct tbt_stream *st, gfp_t gfp)
{
    int i, queued = 0, err = 0;

    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);

        if (!c || !READ_ONCE(c->hello_sent) || atomic_read(&c->stopping))
            continue;
        if (test_and_set_bit(i, &st->tx_rst_mask))
            continue;
        set_bit(i, &st->tx_rst_inflight);
        err = tbt_tx_control_pinned(sess, st, (u8)i, TBT_F_RST, st->sid, 0, NULL, 0,
                                    false, gfp);
        if (err)
        {
            clear_bit(i, &st->tx_rst_inflight);
            clear_bit(i, &st->tx_rst_mask);
            if (err == -ENOMEM)
                sess->dbg_alloc_fail++;
            continue;
        }
        queued++;
    }
    tbt_tx_wake(sess);
    return queued ? queued : err;
}

// ---------------------------------------------------------------------------
// Retention and the send window

/* Free retained frames the peer has delivered in full. Runs only from the
 * pump, which holds pump_lock; the list itself is under tx_lock. */
/* Every copy of this frame on a path whose generation still exists must have
 * been received (the path's high-water past the frame's end). A path that has
 * been replaced is skipped: its old copy's fate is the path-loss rule's, and
 * the retransmit's new path bit speaks for the frame. Keeping an acked frame
 * until its *per-path* copies are received is what makes the confirmed-loss
 * ratio exact: evicting on the global ack alone let a copy still queued in
 * the receiver's parser look like a drop (measured: probe I, 600-8000 ppm on
 * clean carriers). */
static bool tbt_retain_all_received(struct tbt_session *sess, struct tbt_retain *r)
{
    int i;

    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);

        if (!c || !(r->path_mask & BIT(i)))
            continue;
        if ((c->gen & 0xf) != r->path_gen[i])
            continue;
        if (READ_ONCE(c->uack) < r->path_end[i])
            return false;
    }
    return true;
}

static void tbt_tx_retain_gc(struct tbt_session *sess, struct tbt_stream *st)
{
    struct tbt_retain *r, *tmp;
    LIST_HEAD(free);
    u64 ack = READ_ONCE(st->tx_ack);
    u64 now = tbt_now_ns();
    u64 replay_ns = (u64)TBT_REPLAY_MS * NSEC_PER_MSEC;
    u32 cap = sess->tunnel->cfg.replay;

    /* The ring keeps an acked frame for replay_ms so a late hole can still be
     * filled from real bytes, and evicts the oldest acked frames when the
     * configured byte budget is exceeded. Unacked frames are never evicted. */
    spin_lock(&sess->tx_lock);
    list_for_each_entry_safe(r, tmp, &st->retain, list)
    {
        bool acked = r->seq + r->len <= ack &&
                     tbt_retain_all_received(sess, r);
        bool old = r->first_sent_ns && now - r->first_sent_ns >= replay_ns;
        bool over = cap && st->retain_bytes > cap;

        if (!acked)
            break;
        if (!old && !over)
            break;
        if (over && !old)
            sess->retain_drop++;
        /* A queued copy may still point at this entry; clear it before the
         * entry is freed. Such a copy is acked and the selection drops it on
         * seq alone, but the path-mask read happens first. */
        {
            struct tbt_frame *fr;

            list_for_each_entry(fr, &sess->tx_q, list)
                if (fr->ret == r)
                    fr->ret = NULL;
        }
        list_move_tail(&r->list, &free);
        st->retain_bytes -= r->len;
    }
    spin_unlock(&sess->tx_lock);

    list_for_each_entry_safe(r, tmp, &free, list)
    {
        list_del(&r->list);
        kfree(r);
    }
}

/* The retention cap is a guard for a configured bound, not a policy: with the
 * default 0 nothing is dropped. When it does bite, the oldest frame is
 * released first, which is the one closest to being acked anyway.
 *
 * This runs in the pump, under pump_lock, and nowhere else. The pump is the
 * only owner of frames outside sess->tx_q (its local batch and its retransmit
 * candidates hold fr->ret across a dropped tx_lock), so freeing an entry
 * anywhere else would let one of them dereference it after the free. */
static void tbt_tx_retain_trim(struct tbt_session *sess, struct tbt_stream *st)
{
    struct tbt_retain *r, *tmp;
    LIST_HEAD(free);

    if (st->retain_max && st->retain_bytes > st->retain_max)
    {
        u32 over = st->retain_bytes - st->retain_max;

        spin_lock(&sess->tx_lock);
        list_for_each_entry_safe(r, tmp, &st->retain, list)
        {
            struct tbt_frame *fr;

            if (!over)
                break;
            /* A queued copy may still point at this entry; the pump writes
             * fr->ret->path_mask after the free (UAF). */
            list_for_each_entry(fr, &sess->tx_q, list)
                if (fr->ret == r)
                    fr->ret = NULL;
            list_move_tail(&r->list, &free);
            st->retain_bytes -= r->len;
            over = over > r->len ? over - r->len : 0;
            sess->retain_drop++;
        }
        spin_unlock(&sess->tx_lock);
    }
    list_for_each_entry_safe(r, tmp, &free, list)
    {
        list_del(&r->list);
        kfree(r);
    }
}

/* How far this frame may be from the peer's delivered offset. Half the
 * receiver's reorder budget leaves room for the frames already inside the
 * carrier pipes; the floor keeps a tiny configured budget from deadlocking. */
static u64 tbt_tx_window(struct tbt_session *sess)
{
    u64 cap = sess->tunnel->cfg.rx_reorder_bytes / TBT_TX_WINDOW_DIV;

    return cap < 16384 ? 16384 : cap;
}

static bool tbt_tx_window_ok(struct tbt_stream *st, const struct tbt_frame *fr)
{
    return fr->hdr.seq + fr->len <=
           READ_ONCE(st->tx_ack) + tbt_tx_window(st->sess);
}

/* The carrier's own TCP stack measures the path RTT from its ACKs; the FBK
 * estimate is the peer's view and can be stale or zero. Use the larger, or a
 * shaped 40 ms path looks like a 1 ms one and every delayed frame is declared
 * lost (measured: probe I retransmitted the whole payload). */
static u32 tbt_path_rtt_us(struct tbt_carrier *c)
{
    u32 rtt = READ_ONCE(c->rtt_us);
    u32 srtt = 0;

    if (c->sk)
        srtt = READ_ONCE(tcp_sk(c->sk)->srtt_us) >> 3;
    return srtt > rtt ? srtt : rtt;
}

/* Retransmit timeout: four RTTs, floored at two feedback intervals so ordinary
 * ack latency never looks like loss, capped so a dead path is noticed. */
static u64 tbt_tx_rto_ns(struct tbt_session *sess, struct tbt_stream *st)
{
    u32 rtt = 0;
    int i;

    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);
        u32 r;

        if (!c)
            continue;
        r = tbt_path_rtt_us(c);
        if (r > rtt)
            rtt = r;
    }
    if (!rtt)
        rtt = TBT_RETX_MIN_US / 4;
    /* Bootstrap: before the first tunnel-level ack the only RTT we can read is
     * the carrier's own, and where only the peer's egress is shaped that
     * sample is ~0.1 ms while the feedback path (peer's egress + FBK cadence)
     * is ~22 ms. An RTO built from it (the 20 ms floor) retransmits every
     * frame before its ack arrives; the duplicates then drown the very
     * feedback that would end the bootstrap (measured: probe J, far
     * rto_us=20000, srtt_us=80-238, c_rtt_us=0, a 131 KB stream inflated to
     * 7.7 MB). Hold 200 ms until a tunnel ack lands; after that, 4xRTT. */
    if (!READ_ONCE(st->last_ack_ns))
        return 200 * NSEC_PER_MSEC;
    /* The RTO must exceed the end-to-end feedback delay, not just four RTTs:
     * a frame whose uack is still in flight (tx_q + carrier buffers + FBK
     * cadence + the peer's delivery) is not lost. With a 1 MiB carrier buffer
     * that delay reached 2-4 s and every frame was retransmitted ~16x before
     * its ack arrived (measured: bench n=4, retx_ppm 558k, wire/app 11.1).
     * The cap moves to 500 ms for the same reason: 2 x a 200 ms delay does
     * not fit under 250 ms. */
    {
        u32 fb = READ_ONCE(sess->fb_delay_us);
        u64 rto_us = (u64)rtt * 4;

        if (fb && (u64)fb * 2 > rto_us)
            rto_us = (u64)fb * 2;
        return clamp_t(u64, rto_us * NSEC_PER_USEC,
                       TBT_RETX_MIN_US * NSEC_PER_USEC,
                       TBT_RETX_MAX_US * NSEC_PER_USEC);
    }
}

/* A carrier died: drop its bits from every stream's retained frames and mark
 * the frames it last carried for immediate retransmit. Runs with a stream
 * reference from the pump/FBK snapshot, so it may take tx_lock but must not
 * take sess->lock or call back into the carrier. */
static void tbt_tx_path_lost_stream(struct tbt_session *sess,
                                    struct tbt_stream *st,
                                    unsigned long mask)
{
    struct tbt_retain *r;

    if (!st)
        return;
    spin_lock(&sess->tx_lock);
    list_for_each_entry(r, &st->retain, list)
    {
        /* The index is about to name a different carrier. A later carrier's
         * per-path high-water says nothing about frames the old one carried,
         * so drop the bit; otherwise tbt_retain_received() skips exactly the
         * frames that must be re-sent (measured: probe C's receiver hole
         * waited out its 500 ms budget after carrier 0 was replaced). */
        r->path_mask &= ~(u16)mask;
        r->reserve_mask &= ~(u16)mask;
        if (r->last_path < 16 && (mask & BIT(r->last_path)) && r->sends &&
            r->seq + r->len > READ_ONCE(st->tx_ack))
            r->retx_now = true;
    }
    spin_unlock(&sess->tx_lock);
}

/* Called from softirq/atomic carrier-loss context: only publish the lost
 * carrier bits and wake the FBK worker. The per-stream retain lists are
 * walked later, outside the atomic context. */
void tbt_tx_path_lost(struct tbt_session *sess, u8 index)
{
    if (index >= sess->n_car)
        return;
    set_bit(index, &sess->path_lost_mask);
    tbt_fbk_kick(sess, true);
}

/* Drain the lost-carrier bits once, outside atomic context, for every stream
 * in the caller's snapshot. */
static void tbt_tx_path_lost_drain(struct tbt_session *sess,
                                   struct tbt_stream **snap, int n)
{
    unsigned long mask = xchg(&sess->path_lost_mask, 0);
    int i;

    if (!mask)
        return;
    for (i = 0; i < n; i++)
        tbt_tx_path_lost_stream(sess, snap[i], mask);
}

/* Queue copies of the oldest retained frames whose last write is overdue.
 * Called from the pump before path selection, so the copies are written in the
 * same run. The candidates are collected under tx_lock and copied after it is
 * dropped: only the pump frees retention entries, so the pointers stay valid
 * for this run. */
static void tbt_tx_retx(struct tbt_session *sess, struct tbt_stream *st)
{
    struct tbt_retain *cand[8];
    struct tbt_retain *r, *first = NULL;
    int n = 0, i;
    u64 now = tbt_now_ns();
    u64 rto = tbt_tx_rto_ns(sess, st);
    u64 ack = READ_ONCE(st->tx_ack);

    WRITE_ONCE(sess->rto_us, (u32)(rto / NSEC_PER_USEC));
    {
        u64 stall = 0;

        for (i = 0; i < sess->n_car; i++)
        {
            struct tbt_carrier *pc = READ_ONCE(sess->car[i]);

            if (pc && pc->last_una_move_ns &&
                now - pc->last_una_move_ns > stall)
                stall = now - pc->last_una_move_ns;
        }
        WRITE_ONCE(sess->una_stall_us, (u32)(stall / NSEC_PER_USEC));
    }

    /* Delivery backpressure is not loss. While the peer reports a full
     * delivery queue (FBK rx_state=2, published as peer_paused) or our own is
     * full, nothing we re-send can advance the ack: the peer is not reading.
     * Retransmitting there only burns the per-flow budget and deepens the
     * queue the feedback has to cross (measured: bench n=4 strict, peer rxq
     * 26.5 MB, retx_ppm 642k, wire/app 7.5). The frame at the hole is repaired
     * as soon as the peer drains and the ack moves again; the receiver's own
     * hole budget is discounted while it is paused, so no reset races it. */
    if (READ_ONCE(st->peer_paused) ||
        READ_ONCE(st->paused))
        return;

    /* Delivery backpressure is not loss, but the frame at the received ack is
     * genuinely missing; retransmitting it is the only way the hole can fill
     * once the peer resumes reading. Suppressing this deadlocked the strict
     * send-then-read case (probe G reset at 500 ms). */
    spin_lock(&sess->tx_lock);
    list_for_each_entry(r, &st->retain, list)
    {
        if (r->seq + r->len <= ack)
            continue;
        if (!r->sends)
            continue;
        /* The global ack can be stale by a feedback interval. A frame that
         * the peer's per-path high-water says it received is not a hole; the
         * real hole is later in the ring. Chasing the stale ack re-sent a
         * received frame until the receiver's budget reset the stream. */
        if (tbt_retain_received(sess, st, r))
            continue;
        /* The frame at the peer's delivered offset has explicit receiver
         * evidence: the contiguous uack stopped exactly at this seq. That is
         * the only signal that covers both halves of the dead zone -- a copy
         * on a blackholed carrier whose snd_una never passes, and a copy the
         * peer's TCP took but whose parser never saw. So it outranks the
         * heuristic gates. It still pays the RTO throttle (one copy per RTO)
         * and the total queue bound; only the copy share and the path gate
         * are waived. */
        {
            /* A brief tx_ack stall is feedback latency, not a hole: the frame
             * may simply be in flight. Only a stall longer than two RTOs, or
             * the bootstrap window where no tunnel ack has ever landed, is
             * receiver evidence that the bytes at the ack are missing. */
            u64 ack_ns = READ_ONCE(st->last_ack_ns);
            bool hole_evident = r->seq == ack &&
                                (!ack_ns || now - ack_ns >= 2 * rto);

            if (hole_evident)
            {
                if (r->last_sent_ns && now - r->last_sent_ns < rto)
                    continue;
            }
            else if (!r->retx_now)
        {
            u64 done_ns = 0;

            /* Slow path: a record TCP delivered but the parser never saw.
             * Path progress proves nothing there. Bounded to one copy per
             * 2xRTO so it cannot become a per-RTO flood. */
            if (READ_ONCE(st->last_ack_ns) &&
                tbt_retain_tcp_done(sess, r, now, &done_ns))
            {
                if (now - done_ns < 2 * rto)
                    continue;
            }
            else if (!tbt_retain_paths_stalled(sess, r, now, rto))
            {
                continue;
            }
            /* Retransmit copies are inserted at the head, ahead of new data.
             * Cap them at half the queue so the frames that carry a fresh
             * uack always have room; a dead path and the hole above are
             * exempt because they are the only ways forward. */
            if (READ_ONCE(sess->tx_queued) + r->len > TBT_TXQ_HARD_MAX / 2)
                continue;
            }
        }
        if (!first)
            first = r;
        /* A pending copy is normally left alone, but a copy that has not been
         * written for a whole RTO is stuck behind a full queue; waiting for it
         * forever left a hole until the peer's budget reset the stream
         * (measured: probe F, one run in three). */
        if (r->pending && r->last_sent_ns && now - r->last_sent_ns < rto)
            continue;
        {
            /* Only the frame at the hole is re-sent on the timer: everything
             * behind it may simply be waiting in the peer's reorder tree, and
             * re-sending it would turn one drop into a burst of duplicates.
             * last_sent_ns is stamped here as well, so a stuck copy is retried
             * once per RTO rather than once per pump run. */
            if (r == first && !r->declared &&
                !tbt_hole_copy_inflight(sess, r))
            {
                /* A duplication tunnel's holes are the duplication's own
                 * business: every frame there has a copy by design, and the
                 * copies' work is counted by dup_tx/dup_used. A frame already
                 * written on two paths is likewise a copy hole, not a
                 * primary-path gap. The retransmit still runs; the hole
                 * counter stays with single-copy tunnels. */
                bool copy_hole = sess->tunnel->cfg.dup || r->sends > 1;

                r->declared = true;
                if (copy_hole)
                    goto retx_queued;
                sess->holes_declared++;
                if (st->hole_tail - st->hole_head < TBT_HOLES_MAX)
                {
                    struct tbt_hole *holes =
                        tbt_stream_holes_get_locked(st);
                    u16 idx = st->hole_tail % TBT_HOLES_MAX;

                    if (!holes)
                        goto retx_queued;
                    holes[idx].sid = st->sid;
                    holes[idx].seq = r->seq;
                    holes[idx].len = r->len;
                    holes[idx].declared_ns = now;
                    st->hole_tail++;
                }
                if (debug >= 2)
                    pr_info_ratelimited("tbtun: hole declare seq=%llu timer age_us=%llu "
                                        "mask=%#x sends=%u dup_pend=%u\n",
                                        r->seq,
                                        r->first_sent_ns
                                            ? (now - r->first_sent_ns) / NSEC_PER_USEC
                                            : 0,
                                        r->path_mask, r->sends, r->dup_pending);
            }
retx_queued:
            r->last_sent_ns = now;
            r->pending = true;
            cand[n++] = r;
        }
        if (n == ARRAY_SIZE(cand))
            break;
    }
    spin_unlock(&sess->tx_lock);
    if (!n)
        return;

    for (i = 0; i < n; i++)
    {
        struct tbt_retain *r = cand[i];
        struct tbt_frame *fr;

        if (r->seq + r->len <= READ_ONCE(st->tx_ack))
        {
            r->pending = false;
            continue;
        }
        /* A retransmit must be able to enter a queue that is full of
         * window-blocked new data: it is the frame that advances the window.
         * Head insertion and a bounded overage keep the queue finite while
         * letting the hole filler through first. */
        if (READ_ONCE(sess->tx_queued) > TBT_TXQ_HARD_MAX)
        {
            for (; i < n; i++)
                cand[i]->pending = false;
            break;
        }
        fr = tbt_frame_alloc(r->len, GFP_KERNEL);
        if (!fr)
        {
            for (; i < n; i++)
                cand[i]->pending = false;
            sess->dbg_alloc_fail++;
            break;
        }
        fr->hdr = r->hdr;
        fr->hdr.body_len = r->len;
        fr->hdr.seq = r->seq;
        fr->avoid_mask = r->path_mask;
        fr->ret = r;
        fr->st = st;
        refcount_inc(&st->refs);
        memcpy(fr->data, r->data, r->len);
        /* Head insertion is what lets a hole filler overtake window-blocked
         * new data, but when the queue is already over half full it does the
         * opposite: the copies take the space the new frames need, and only a
         * new frame can carry a fresh uack. Above the line the copy goes to
         * the tail instead. */
        tbt_tx_enqueue(sess, fr,
                       READ_ONCE(sess->tx_queued) <= TBT_TXQ_HARD_MAX / 2);
        /* Counted when the carrier accepts the write, not here: a copy the
         * pump drops because the ack passed never reaches the wire, and
         * counting it made retx_ppm read 25% while wire/app was 1.01. The
         * enqueue is kept as a separate diagnostic. */
        sess->retx_queued++;
        fr->retx_copy = true;
    }
}

/* 0.919^(g-1) in millionths, g = 1..17 (DESIGN.md §4.3). */
static const u32 tbt_late_tab[17] = {
    1000000, 919000, 844561, 776152, 713283, 655507, 602411, 553616, 508773,
    467562, 429690, 394885, 362899, 333504, 306491, 281665, 258850,
};

/* Was this frame received on any path it was sent on? The per-path uack is a
 * cumulative high-water mark: TCP delivers in order on one carrier, so a path
 * that has seen a later byte has seen this frame too. */
static bool tbt_retain_received(struct tbt_session *sess, struct tbt_stream *st,
                                struct tbt_retain *r)
{
    u64 ack = READ_ONCE(st->tx_ack);
    int i;

    /* The peer's contiguous delivered offset has passed this frame: every
     * byte below tx_ack reached the application. This proof does not depend
     * on any per-path accounting. */
    if (r->seq + r->len <= ack)
        return true;
    /* The frame at the hole is the one place a per-path high-water cannot be
     * trusted. A record dropped at the sender before the write leaves no gap
     * in the carrier's byte stream, so the next record's bytes are contiguous
     * and that path's high-water jumps over the missing frame. Trusting it
     * would make the hole permanently invisible and the stream would die on
     * the receiver's budget (measured: probe I, hole at 1.47 MB, 5 s reset).
     * Only the retransmit timer and the geometric test may repair it; a stale
     * tx_ack costs one duplicate the receiver drops. */
    if (r->seq == ack)
        return false;
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);

        if (!c || !(r->path_mask & BIT(i)))
            continue;
        /* The path index may now name a replacement carrier; only the
         * generation that actually wrote this frame can speak for it. */
        if ((c->gen & 0xf) != r->path_gen[i])
            continue;
        /* A later byte on this path proves the frame arrived: TCP delivers in
         * order, and the hole case above already covers the one exception.
         * recv_lag stays a loss *signal* for the hole gate; it must not
         * disable the receipt proof, or a 2% counting bias retransmits the
         * whole window (measured: rx_lag_ppm 22k-40k, retx_ppm 65-85%). */
        if (READ_ONCE(c->uack) >= r->path_end[i])
            return true;
    }
    return false;
}

/* Has every copy of this frame been acked by its carrier's TCP? Returns the
 * earliest crossing observation when it has, and stamps una_observed_ns as it
 * goes. Called with tx_lock held.
 *
 * "Every" was tried as "at least one" to break the blackholed-carrier dead
 * zone, and it made probe J worse (1/3 vs 2/3): a frame whose un-delivered
 * copy is merely slow then looks lost and its copies add load without filling
 * the hole. Kept as "every" until a better discriminator exists. */
static bool tbt_retain_tcp_done(struct tbt_session *sess, struct tbt_retain *r,
                                u64 now, u64 *earliest)
{
    bool any = false;
    int i;

    *earliest = 0;
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);

        if (!c || !(r->path_mask & BIT(i)))
            continue;
        if ((c->gen & 0xf) != r->path_gen[i])
            continue;
        if (!c->sk)
            return false;
        if ((s32)(READ_ONCE(tcp_sk(c->sk)->snd_una) - r->first_sent_seq[i]) < 0)
            return false;
        if (!r->una_observed_ns[i])
            r->una_observed_ns[i] = now;
        any = true;
        if (!*earliest || r->una_observed_ns[i] < *earliest)
            *earliest = r->una_observed_ns[i];
    }
    return any;
}

/* Is every path that wrote this frame without snd_una progress for a whole
 * RTO? A frame is not lost while a path that wrote it is still moving its ack
 * clock: under cwnd collapse snd_nxt is already past the record while snd_una
 * sits 1-2 s behind, and a write clock cannot tell that apart from loss. Only
 * a path that has not moved snd_una for an RTO is evidence. Called with
 * tx_lock held. */
static bool tbt_retain_paths_stalled(struct tbt_session *sess, struct tbt_retain *r,
                                     u64 now, u64 rto)
{
    int i;

    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);

        if (!c || !(r->path_mask & BIT(i)))
            continue;
        if ((c->gen & 0xf) != r->path_gen[i])
            continue;
        if (c->last_una_move_ns && now - c->last_una_move_ns < rto)
            return false; /* still in flight on a path that is making progress */
    }
    /* No live copy left (all replaced) counts as stalled: the frame has no
     * path that could still deliver it. */
    return true;
}

/* A copy of this frame is still in flight on a path that wrote it: that
 * path's high-water has not reached the frame's end, and a record is atomic,
 * so the bytes cannot have arrived there yet. Declaring the hole now would
 * count a repair the duplication is already doing. This is a sound in-flight
 * test even on a path with record loss, unlike the high-water >= end test.
 * The wait is bounded by the receiver's hole budget; if the copy never
 * arrives, a later record on that path moves the high-water past the end and
 * the hole is declared normally. Called with tx_lock held. */
static bool tbt_hole_copy_inflight(struct tbt_session *sess, struct tbt_retain *r)
{
    int i;

    if (r->dup_pending)
        return true;
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);

        if (!c || !(r->path_mask & BIT(i)))
            continue;
        if ((c->gen & 0xf) != r->path_gen[i])
            continue;
        if (READ_ONCE(c->uack) < r->path_end[i])
            return true;
    }
    return false;
}

/* Queue true-byte copies of a declared hole, avoiding every path the frame
 * already used. Called with tx_lock held; allocation is atomic. */
static void tbt_tx_inject_locked(struct tbt_session *sess, struct tbt_stream *st, struct tbt_retain *r, int g)
{
    int d2 = sess->tunnel->cfg.dup + (g == 1 ? 1 : 0);
    int k;

    if (d2 < 1)
        d2 = 1;
    for (k = 0; k < d2; k++)
    {
        struct tbt_frame *fr;

        /* Called with tx_lock held: this is the authoritative bound. A hole
         * filler that cannot enter is not lost; the retransmit timer offers
         * it again on the next pump. */
        if (sess->tx_queued + r->len > TBT_TXQ_HARD_MAX)
        {
            sess->inject_skip++;
            break;
        }
        fr = tbt_frame_alloc(r->len, GFP_ATOMIC);

        if (!fr)
        {
            sess->dbg_alloc_fail++;
            break;
        }
        fr->hdr = r->hdr;
        fr->hdr.body_len = r->len;
        fr->hdr.seq = r->seq;
        fr->avoid_mask = r->path_mask;
        fr->ret = r;
        fr->st = st;
        refcount_inc(&st->refs);
        memcpy(fr->data, r->data, r->len);
        list_add(&fr->list, &sess->tx_q);
        sess->tx_queued += r->len;
        sess->retx_queued++;
        fr->retx_copy = true;
    }
}

/* Pop this stream's repaired holes in seq order. Each stream owns its ring
 * and counters, so one stream's unrepaired head cannot block another. */
static void tbt_tx_holes_repaired(struct tbt_session *sess,
                                   struct tbt_stream *st)
{
    struct tbt_hole *holes;
    u64 ack;

    if (!st)
        return;
    holes = READ_ONCE(st->holes);
    if (!holes)
        return;
    ack = READ_ONCE(st->tx_ack);
    spin_lock(&sess->tx_lock);
    while (st->hole_head != st->hole_tail)
    {
        u16 idx = st->hole_head % TBT_HOLES_MAX;

        if (holes[idx].seq + holes[idx].len > ack)
            break;
        if (debug >= 2)
            pr_info_ratelimited("tbtun: hole repair seq=%llu age_us=%llu\n",
                                holes[idx].seq,
                                (tbt_now_ns() - holes[idx].declared_ns) /
                                    NSEC_PER_USEC);
        st->hole_head++;
        sess->holes_repaired++;
    }
    spin_unlock(&sess->tx_lock);
}

/* The geometric hole test, run once per FBK. The gate is the important part:
 * the FBK must cover every path the frame used, no such path may have seen the
 * frame, and every other live path must have moved past it. `g` is 1 plus the
 * number of later frames the peer has actually received, i.e. the depth the
 * hole has reached; p_late(g) < 0.919^k_max_hole declares it lost. */
void tbt_tx_hole_check(struct tbt_session *sess, struct tbt_stream *st)
{
    struct tbt_retain *r, *hole = NULL;
    u64 ack, now;
    u32 rtt = 0;
    bool gate = true, wake = false;
    int g = 1, i;

    if (!st)
        return;
    ack = READ_ONCE(st->tx_ack);
    now = tbt_now_ns();
    spin_lock(&sess->tx_lock);
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);

        if (!c || !atomic_read(&c->up))
            continue;
        if (tbt_path_rtt_us(c) > rtt)
            rtt = tbt_path_rtt_us(c);
    }
    list_for_each_entry(r, &st->retain, list)
    {
        if (r->seq + r->len <= ack)
            continue;
        hole = r;
        break;
    }
    if (!hole || hole->seq > ack)
    {
        /* The bytes at the ack are no longer in the ring. There is no
         * placeholder frame: delivery never advances across a gap, and the
         * receiver's bounded hole budget resets the stream if the true bytes
         * never arrive (DESIGN.md §4.6 case B). */
        goto unlock;
    }
    if (hole->declared || !hole->sends)
        goto unlock;
    sess->hole_checks++;
    /* Gate: a path the hole was sent on must show a received-count lag, and
     * every other live path must have moved past the hole's seq. The lossy
     * path's own uack is not required to be fresh: while it is dropping, no
     * fresh uack from it can exist. Its recv_cnt is the receiver-published
     * evidence, and the lag is "records we wrote below its high-water that the
     * peer's count does not account for". */
    {
        bool lost = false;

        for (i = 0; i < sess->n_car; i++)
        {
            struct tbt_carrier *c = READ_ONCE(sess->car[i]);
            struct tbt_retain *q;
            u64 tail = 0, sent_below, lag;

            if (!c || !atomic_read(&c->up))
                continue;
            /* A warm spare is connected but carries no DATA; its empty
             * high-water must not hold the gate closed for the paths that
             * do. */
            if (READ_ONCE(c->state) == TBT_CAR_SPARE)
                continue;
            list_for_each_entry(q, &st->retain, list)
            {
                if ((q->path_mask & BIT(i)) &&
                    (c->gen & 0xf) == q->path_gen[i] &&
                    q->path_end[i] <= READ_ONCE(c->uack))
                    continue;
                if ((q->path_mask & BIT(i)) &&
                    (c->gen & 0xf) == q->path_gen[i])
                    tail++;
            }
            sent_below = READ_ONCE(c->data_frames) >= tail
                             ? READ_ONCE(c->data_frames) - tail
                             : 0;
            lag = sent_below > READ_ONCE(c->recv_cnt)
                      ? sent_below - READ_ONCE(c->recv_cnt)
                      : 0;
            WRITE_ONCE(c->recv_lag, lag);
            /* Confirmed loss ratio: records we wrote below the peer's
             * high-water that its recv_cnt does not account for, over those
             * records. A clean carrier reads exactly 0; a frame in our own
             * queue is not in the denominator (it is above the high-water). */
            c->loss_ppm = sent_below
                              ? (s32)(lag * 1000000ULL / sent_below)
                              : 0;
            if (!(hole->path_mask & BIT(i)))
            {
                if (READ_ONCE(c->uack) < hole->path_end[i])
                {
                    gate = false;
                    break;
                }
                continue;
            }
            /* Old generation: the path loss already re-sends these frames. */
            if ((c->gen & 0xf) != hole->path_gen[i])
                continue;
            if (lag)
                lost = true;
        }
        if (!lost)
            gate = false;
    }
    /* Depth over records: the consecutive run of retained frames starting at
     * the hole that are not known received on their own (index, generation).
     * A hole of depth 1 is depth 1 no matter what later writes did. */
    if (gate)
    {
        struct tbt_retain *q;

        g = 0;
        list_for_each_entry(q, &st->retain, list)
        {
            if (q->seq < hole->seq)
                continue;
            if (!q->sends)
                continue;
            if (tbt_retain_received(sess, st, q))
                break;
            g++;
            if (g >= 17)
                break;
        }
        if (g < 1)
            g = 1;
    }
    if (!gate)
    {
        sess->hole_gate_fail++;
        if (debug >= 2)
            pr_info_ratelimited("tbtun: hole gatefail seq=%llu mask=%#x ack=%llu\n",
                                hole->seq, hole->path_mask, ack);
    }
    else if (tbt_late_tab[g] >= tbt_late_tab[TBT_K_MAX_HOLE])
    {
        sess->hole_depth_fail++;
        if (debug >= 2)
            pr_info_ratelimited("tbtun: hole depthfail seq=%llu g=%d mask=%#x\n",
                                hole->seq, g, hole->path_mask);
    }
    else if (!hole->first_sent_ns ||
             now - hole->first_sent_ns <
                 (u64)(rtt > 500 ? rtt : 500) * 2 * NSEC_PER_USEC)
    {
        sess->hole_time_fail++;
        if (debug >= 2)
            pr_info_ratelimited("tbtun: hole timefail seq=%llu age_us=%llu rtt_us=%u\n",
                                hole->seq,
                                hole->first_sent_ns ? (now - hole->first_sent_ns) / NSEC_PER_USEC : 0,
                                rtt);
    }
    else if (tbt_hole_copy_inflight(sess, hole))
    {
        /* The duplication path is still carrying this frame. */
    }
    else if (sess->tunnel->cfg.dup || hole->sends > 1)
    {
        /* Duplication hole: counted by the dup counters, not here. */
        hole->declared = true;
    }
    else
    {
        hole->declared = true;
        sess->holes_declared++;
        if (debug >= 2)
        {
            u64 idle = 0;

            for (i = 0; i < sess->n_car; i++)
            {
                struct tbt_carrier *pc = READ_ONCE(sess->car[i]);
                u64 d;

                if (!pc || !(hole->path_mask & BIT(i)))
                    continue;
                d = pc->last_rx_ns ? now - pc->last_rx_ns : 0;
                if (d > idle)
                    idle = d;
            }
            pr_info_ratelimited("tbtun: hole declare seq=%llu g=%d age_us=%llu "
                                "mask=%#x path_idle_us=%llu\n",
                                hole->seq, g, (now - hole->first_sent_ns) / NSEC_PER_USEC,
                                hole->path_mask, idle / NSEC_PER_USEC);
        }
        if (st->hole_tail - st->hole_head < TBT_HOLES_MAX)
        {
            struct tbt_hole *holes = tbt_stream_holes_get_locked(st);
            u16 idx;

            if (!holes)
                goto unlock;
            idx = st->hole_tail % TBT_HOLES_MAX;
            holes[idx].sid = st->sid;
            holes[idx].seq = hole->seq;
            holes[idx].len = hole->len;
            holes[idx].declared_ns = now;
            st->hole_tail++;
        }
        tbt_tx_inject_locked(sess, st, hole, g);
        wake = true;
    }
unlock:
    spin_unlock(&sess->tx_lock);
    if (wake)
        tbt_tx_wake(sess);
}

/* Sender-side bounded reset. It must fire on *no progress*, not on the age of
 * the oldest frame: one dead path leaves an old frame at the ack while the
 * other paths keep repairing holes, and resetting there would turn a
 * recoverable two-second outage into a failure (measured in probe H). The
 * timer therefore runs from the last time tx_ack advanced. Total loss leaves
 * it untouched, which is the case the reset exists for. */
static void tbt_tx_stall_check(struct tbt_session *sess,
                               struct tbt_stream *st)
{
    struct tbt_retain *r;
    u64 now = tbt_now_ns();
    u64 ack, start = 0;
    bool kill = false;

    if (!st || READ_ONCE(st->state) == TBT_ST_DEAD)
        return;
    /* Backpressure, local or remote, is not loss. While this stream's own
     * delivery queue is full its carriers are not read, so the peer's feedback
     * cannot arrive either; a reset here turns ordinary TCP backpressure into
     * ECONNRESET (measured: probe I reset at ~14 MiB). */
    if (READ_ONCE(st->peer_paused) ||
        READ_ONCE(st->paused))
        return;
    ack = READ_ONCE(st->tx_ack);
    spin_lock(&sess->tx_lock);
    list_for_each_entry(r, &st->retain, list)
    {
        if (r->seq + r->len <= ack)
            continue;
        start = st->last_ack_ns ? st->last_ack_ns
                                : (r->first_sent_ns ? r->first_sent_ns
                                                    : st->tx_start_ns);
        if (st->pause_cleared_ns > start)
            start = st->pause_cleared_ns;
        break;
    }
    spin_unlock(&sess->tx_lock);
    /* A timestamp stamped on another CPU can be a few nanoseconds ahead of
     * this CPU's ktime_get_ns(); the unsigned subtraction then looks like a
     * 584-year stall and kills a healthy stream (measured: probe G, one run in
     * three, age_us=18446744073709519). */
    if (start && now > start && ack < READ_ONCE(st->tx_seq) &&
        now - start > (u64)sess->tunnel->cfg.hole_budget_ms * NSEC_PER_MSEC)
        kill = true;
    if (kill)
    {
        if (debug >= 2)
            pr_info("tbtun: TXSTALLKILL sid=%llu ack=%llu txseq=%llu age_us=%llu\n",
                    (unsigned long long)st->sid, ack, READ_ONCE(st->tx_seq),
                    (now - start) / NSEC_PER_USEC);
        sess->holes_unrecoverable++;
        tbt_stream_kill(st, TBT_ST_HOLE);
    }
}

/* Apply one ack value (a byte offset below which the peer has delivered
 * everything) to this session's TX direction. `c` is the path that reported
 * it, used only to attribute an RTT sample. */
void tbt_tx_ack(struct tbt_session *sess, struct tbt_carrier *c,
                u64 uack, u64 sid)
{
    struct tbt_stream *st;
    struct tbt_retain *r;
    u64 sample = 0, fb_sample = 0;

    if (!uack || !sid)
        return;
    st = tbt_stream_get(sess, sid, false, false);
    if (!st)
        return;
    spin_lock(&sess->lock);
    if (uack > READ_ONCE(st->tx_seq))
        uack = READ_ONCE(st->tx_seq);
    if (uack <= READ_ONCE(st->tx_ack))
    {
        spin_unlock(&sess->lock);
        tbt_stream_put(st);
        return;
    }
    WRITE_ONCE(st->tx_ack, uack);
    st->last_ack_ns = tbt_now_ns();
    sess->tx_ack = uack; /* control/legacy mirror; st->tx_ack is authoritative */
    sess->last_ack_ns = st->last_ack_ns;
    spin_unlock(&sess->lock);

    /* The frame that contains the last delivered byte, when it went out on the
     * path that reported the ack, gives an RTT sample. */
    spin_lock(&sess->tx_lock);
    list_for_each_entry(r, &st->retain, list)
    {
        if (uack > r->seq && uack <= r->seq + r->len)
        {
            if (r->sends && r->last_path == c->index && r->last_sent_ns)
            {
                u64 us = (tbt_now_ns() - r->last_sent_ns) / NSEC_PER_USEC;

                sample = us > U32_MAX ? U32_MAX : us;
            }
            /* Feedback delay: how long the application-level ack took after
             * the carrier TCP had taken the bytes. That is the delay the RTO
             * must cover (tx_q + carrier buffers + FBK cadence + peer
             * delivery), and it is not the RTT. Use the copy on the reporting
             * carrier; fall back to the last one that wrote the frame. Only a
             * copy whose snd_una crossing was observed is a real sample. */
            if (r->sends)
            {
                int i = -1;

                if (c && (r->path_mask & BIT(c->index)) &&
                    (c->gen & 0xf) == r->path_gen[c->index])
                    i = c->index;
                else if (r->last_path < sess->n_car)
                    i = r->last_path;
                if (i >= 0 && r->una_observed_ns[i])
                {
                    u64 now = tbt_now_ns();

                    if (now > r->una_observed_ns[i])
                    {
                        u64 us = (now - r->una_observed_ns[i]) / NSEC_PER_USEC;

                        fb_sample = us > 5000 ? 5000 : us;
                    }
                }
            }
            break;
        }
    }
    spin_unlock(&sess->tx_lock);
    if (sample)
    {
        u32 us = (u32)sample;

        if (!c->rtt_min_us || us < c->rtt_min_us)
            c->rtt_min_us = us;
        c->rtt_us = c->rtt_us ? (c->rtt_us * 7 + us) / 8 : us;
    }
    if (fb_sample)
    {
        u32 us = (u32)fb_sample;

        sess->fb_delay_us = sess->fb_delay_us
                                ? (sess->fb_delay_us * 7 + us) / 8
                                : us;
    }
    tbt_tx_holes_repaired(sess, st);
    tbt_stream_put(st);
    tbt_tx_wake(sess);
}

// ---------------------------------------------------------------------------
// Scheduler: lowest inflight, striped across a 64-frame window

static u32 tbt_path_inflight(struct tbt_session *sess, struct tbt_stream *st,
                             struct tbt_carrier *c)
{
    u64 ack = st ? READ_ONCE(st->tx_ack) : READ_ONCE(sess->tx_ack);
    u64 high = READ_ONCE(c->payload_bytes_sent);

    if (high <= ack)
        return 0;
    return high - ack > U32_MAX ? U32_MAX : (u32)(high - ack);
}

static bool tbt_sched_better(struct tbt_session *sess, struct tbt_stream *st,
                             const struct tbt_sched *s, u8 a, u8 b)
{
    struct tbt_carrier *ca = READ_ONCE(sess->car[a]);
    struct tbt_carrier *cb = READ_ONCE(sess->car[b]);
    u32 ia = tbt_path_inflight(sess, st, ca);
    u32 ib = tbt_path_inflight(sess, st, cb);

    if (ia != ib)
        return ia < ib;
    /* Ties rotate, so a burst is striped instead of pinned to path 0. */
    return ((a + s->rot) % sess->n_car) < ((b + s->rot) % sess->n_car);
}

/* Rebuild one stream's striped order (or the session's control order when
 * st is NULL). Only the pump calls this (under pump_lock), so the scheduler
 * state needs no lock of its own. */
static void tbt_sched_rebuild(struct tbt_session *sess, struct tbt_stream *st,
                              bool need_open)
{
    struct tbt_sched *s = st ? &st->sched : &sess->ctrl_sched;
    u8 order[TBT_MAX_CARRIERS_LIMIT];
    int n = 0, i, j;

    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);

        if (!c || atomic_read(&c->stopping) || !atomic_read(&c->up))
            continue;
        /* An idle spare is warm but not a DATA path; it still carries FBK. */
        if (need_open && c->spare && !c->active)
            continue;
        /* A DATA path must have written this stream's OPEN on this exact
         * (index, generation). c->open_sent alone is not enough: it may name
         * an older stream. */
        if (need_open && (!st || !tbt_stream_announced(st, c)))
            continue;
        order[n++] = (u8)i;
    }
    for (i = 1; i < n; i++)
    {
        u8 key = order[i];

        for (j = i; j > 0 &&
             tbt_sched_better(sess, need_open ? st : NULL, s, key,
                              order[j - 1]); j--)
            order[j] = order[j - 1];
        order[j] = key;
    }
    memcpy(s->order, order, n);
    s->n = (u8)n;
    s->need_open = need_open;
    s->sort_ns = tbt_now_ns();
    s->rot++;
    sess->sched_sorts++;
}

static struct tbt_carrier *tbt_sched_pick(struct tbt_session *sess,
                                          struct tbt_stream *st, u16 blocked,
                                          bool need_open)
{
    struct tbt_sched *s = st ? &st->sched : &sess->ctrl_sched;
    u64 now = tbt_now_ns();
    int tries;

    if (!s->n || s->need_open != need_open ||
        now - s->sort_ns > TBT_FBK_INTERVAL_MS * NSEC_PER_MSEC)
        tbt_sched_rebuild(sess, st, need_open);
    for (tries = 0; tries < s->n; tries++)
    {
        u8 idx;

        if (s->win >= 64)
        {
            s->win = 0;
            tbt_sched_rebuild(sess, st, need_open);
        }
        idx = s->order[s->win % (s->n ? s->n : 1)];
        s->win++;
        if (blocked & BIT(idx))
            continue;
        return READ_ONCE(sess->car[idx]);
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// The pump

/* A pinned frame left tx_q (selected into a batch, written or dropped). The
 * session's control-frame count includes frames in a batch, so it is
 * decremented only when the frame is finished. */
static void tbt_tx_ctl_put(struct tbt_session *sess, struct tbt_frame *fr)
{
    if (!fr->pinned)
        return;
    spin_lock(&sess->tx_lock);
    if (sess->ctl_queued)
        sess->ctl_queued--;
    spin_unlock(&sess->tx_lock);
}

/* Wake a carrier's 2 ms pump. A stopping carrier's w_pump returns early, so
 * prefer any live one; when none exists the replacement/FBK work owns the
 * next attempt. */
static void tbt_tx_ctl_arm(struct tbt_session *sess, struct tbt_carrier *pref,
                           unsigned int ms)
{
    struct tbt_carrier *c = pref;
    int i;

    if (!c || atomic_read(&c->stopping) || !atomic_read(&c->connected))
        c = NULL;
    for (i = 0; !c && i < sess->n_car; i++)
    {
        struct tbt_carrier *it = READ_ONCE(sess->car[i]);

        if (it && !atomic_read(&it->stopping) &&
            atomic_read(&it->connected))
            c = it;
    }
    if (c)
        mod_delayed_work(system_dfl_wq, &c->w_pump, msecs_to_jiffies(ms));
}

/* Put a pinned frame back on the queue after a transient failure. The bound is
 * per frame: after TBT_CTL_MAX_RETRIES the caller must fail it explicitly.
 * A dead carrier slot waits at the poll cadence; a full write buffer waits
 * 2 ms. */
static bool tbt_tx_ctl_retry(struct tbt_session *sess, struct tbt_frame *fr,
                             bool path_wait)
{
    unsigned int ms = path_wait ? TBT_CTL_PATH_RETRY_MS : TBT_CTL_RETRY_MS;
    struct tbt_carrier *c = NULL;

    if (fr->retries >= TBT_CTL_MAX_RETRIES)
        return false;
    fr->retries++;
    sess->ctl_retry++;
    sess->ctl_requeue++;
    fr->retry_at_ns = tbt_now_ns() + (u64)ms * NSEC_PER_MSEC;
    if (fr->path < sess->n_car)
        c = READ_ONCE(sess->car[fr->path]);
    tbt_tx_enqueue(sess, fr, true);
    tbt_tx_ctl_arm(sess, c, ms);
    sess->dbg_pump_hiwat++;
    return true;
}

/* Explicit failure after the retry bound. A FIN/RST copy is redundant when
 * another copy was written or is still in flight; only the last one with no
 * written copy makes the stream terminal. */
static void tbt_tx_ctl_fail(struct tbt_session *sess, struct tbt_frame *fr)
{
    u8 type = fr->hdr.type;

    sess->ctl_lost++;
    if (fr->st)
    {
        if (type == TBT_F_FIN)
        {
            if (fr->path < TBT_MAX_CARRIERS_LIMIT)
                clear_bit(fr->path, &fr->st->tx_fin_inflight);
            if (!READ_ONCE(fr->st->tx_fin_written) &&
                !fr->st->tx_fin_inflight)
            {
                WRITE_ONCE(fr->st->tx_fin_queued, false);
                tbt_stream_kill(fr->st, TBT_ST_DEAD);
            }
        }
        else if (type == TBT_F_RST)
        {
            if (fr->path < TBT_MAX_CARRIERS_LIMIT)
                clear_bit(fr->path, &fr->st->tx_rst_inflight);
            if (!READ_ONCE(fr->st->tx_rst_written) &&
                !fr->st->tx_rst_inflight)
                tbt_stream_kill(fr->st, TBT_ST_DEAD);
        }
    }
    tbt_tx_ctl_put(sess, fr);
    tbt_frame_free(fr);
}

/* A pinned frame reached the wire. The per-index mask stays set: a duplicate
 * FIN/RST on the same carrier is never wanted. */
static void tbt_tx_ctl_written(struct tbt_session *sess, struct tbt_frame *fr)
{
    u8 type = fr->hdr.type;

    if (fr->st)
    {
        if (type == TBT_F_FIN)
        {
            WRITE_ONCE(fr->st->tx_fin_written, true);
            if (fr->path < TBT_MAX_CARRIERS_LIMIT)
                clear_bit(fr->path, &fr->st->tx_fin_inflight);
        }
        else if (type == TBT_F_RST)
        {
            WRITE_ONCE(fr->st->tx_rst_written, true);
            if (fr->path < TBT_MAX_CARRIERS_LIMIT)
                clear_bit(fr->path, &fr->st->tx_rst_inflight);
        }
    }
    tbt_tx_ctl_put(sess, fr);
}

/* A RETIRE names its victim (index, generation) in the body; the carrier that
 * sends the notice is incidental. After that sender is replaced, move the
 * notice to another live carrier instead of dropping it. */
static int tbt_tx_retire_alt(struct tbt_session *sess, u8 path)
{
    int i;

    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);

        if (i == path || !c || atomic_read(&c->stopping) ||
            !atomic_read(&c->up))
            continue;
        return i;
    }
    return -1;
}

/* Take every frame that can be written right now out of the session queue,
 * then write them with no lock held. A frame is left queued when its path is
 * at the write-queue high-water mark, when the send window is closed, or when
 * its pinned carrier is not ready yet; the next pass retries it. */
int tbt_tx_pump(struct tbt_session *sess)
{
    struct tbt_stream *snap[TBT_STREAM_SNAP_MAX];
    int nsnap, i;
    int pass, moved = 0;

    might_sleep();
    sess->dbg_pump_runs++;
    if (!sess->tunnel)
    {
        sess->dbg_pump_nocar++;
        return 0;
    }
    mutex_lock(&sess->pump_lock);
    if (!sess->tunnel)
    {
        mutex_unlock(&sess->pump_lock);
        return 0;
    }

    {
        u32 rate = sess->tunnel->cfg.tunnel_rate;

        if (rate)
        {
            u64 now = tbt_now_ns();
            u64 burst = (u64)2 * sess->n_car * TBT_CHUNK_MAX;

            if (!sess->last_refill_ns)
            {
                sess->tokens = burst;
            }
            else
            {
                sess->tokens += (u64)rate * (now - sess->last_refill_ns) / NSEC_PER_SEC;
                if (sess->tokens > burst)
                    sess->tokens = burst;
            }
            sess->last_refill_ns = now;
        }
    }
    nsnap = tbt_session_stream_snapshot(sess, snap, ARRAY_SIZE(snap));
    tbt_tx_path_lost_drain(sess, snap, nsnap);
    for (i = 0; i < nsnap; i++)
    {
        tbt_tx_retain_gc(sess, snap[i]);
        tbt_tx_retain_trim(sess, snap[i]);
        tbt_tx_retx(sess, snap[i]);
    }

    for (pass = 0; pass < 4; pass++)
    {
        LIST_HEAD(batch);
        LIST_HEAD(drop);
        LIST_HEAD(fail);
        struct tbt_frame *fr, *tmp;
        u16 blocked = 0;
        bool progress = false;

        spin_lock(&sess->tx_lock);
        {
        unsigned long guard = 0;
        list_for_each_entry_safe(fr, tmp, &sess->tx_q, list)
        {
            if (++guard > 200000) {
                pr_err("tbtun: pump tx_q loop guard\n");
                break;
            }
            struct tbt_carrier *c = NULL;
            u8 type = fr->hdr.type;

            if (fr->pinned)
            {
                bool stream_ctl = (type == TBT_F_FIN || type == TBT_F_RST);
                u64 now = tbt_now_ns();

                if (fr->path >= sess->n_car)
                    goto drop;
                c = READ_ONCE(sess->car[fr->path]);
                /* A replacement may be ready before the retry timer fires.
                 * Re-pin FIN/RST immediately: the generation change is the
                 * event that matters, not the timer. */
                if (c && !atomic_read(&c->stopping) && fr->gen &&
                    fr->gen != c->gen && stream_ctl)
                {
                    fr->gen = c->gen;
                    fr->retry_at_ns = 0;
                    sess->ctl_requeue++;
                    continue;
                }
                if (fr->retry_at_ns && now < fr->retry_at_ns)
                    continue;
                if (!c || atomic_read(&c->stopping))
                {
                    /* The slot is being replaced. HELLO/OPEN/RETIRE belong to
                     * the old generation and are obsolete; FIN/RST belong to
                     * the stream and wait for the replacement. */
                    if (!stream_ctl)
                        goto drop;
                    if (fr->retries >= TBT_CTL_MAX_RETRIES)
                        goto ctl_fail;
                    fr->retries++;
                    sess->ctl_retry++;
                    sess->ctl_requeue++;
                    fr->retry_at_ns = now +
                        (u64)TBT_CTL_PATH_RETRY_MS * NSEC_PER_MSEC;
                    tbt_tx_ctl_arm(sess, c, TBT_CTL_PATH_RETRY_MS);
                    continue;
                }
                /* A pinned frame from a carrier that has since been replaced
                 * must never be written on the replacement, with one exception:
                 * FIN/RST are stream-level and re-pin to the new generation.
                 * RETIRE names its victim in the body, so its sender may move
                 * to another live carrier. */
                if (fr->gen && fr->gen != c->gen)
                {
                    if (stream_ctl)
                    {
                        fr->gen = c->gen;
                        fr->retry_at_ns = 0;
                        sess->ctl_requeue++;
                        continue;
                    }
                    if (type == TBT_F_RETIRE)
                    {
                        int alt = tbt_tx_retire_alt(sess, fr->path);

                        if (alt >= 0)
                        {
                            fr->path = (u8)alt;
                            fr->gen = READ_ONCE(sess->car[alt])->gen;
                            fr->retry_at_ns = 0;
                            sess->ctl_requeue++;
                            continue;
                        }
                    }
                    goto drop;
                }
                if (type == TBT_F_HELLO)
                {
                    if (READ_ONCE(c->hello_sent))
                        goto drop;
                    if (!atomic_read(&c->connected))
                        continue;
                }
                else if (type == TBT_F_OPEN)
                {
                    if (!READ_ONCE(c->hello_sent))
                        goto drop;
                    /* An OPEN already written for this stream is a duplicate;
                     * another stream's OPEN does not make this one stale. */
                    if (!fr->st || tbt_stream_announced(fr->st, c))
                        goto drop;
                }
                else if (type == TBT_F_RETIRE && !atomic_read(&c->up))
                {
                    /* The carrier that carried the notice is not usable yet.
                     * Another live carrier can send it now. */
                    int alt = tbt_tx_retire_alt(sess, fr->path);

                    if (alt >= 0)
                    {
                        fr->path = (u8)alt;
                        fr->gen = READ_ONCE(sess->car[alt])->gen;
                        fr->retry_at_ns = 0;
                        sess->ctl_requeue++;
                        continue;
                    }
                    continue;
                }
                else if (!atomic_read(&c->up))
                {
                    continue;
                }
                /* A FIN is stream-ordered: it may not leave before every byte
                 * it orders has reached the peer.  tx_ack is the peer's
                 * delivered offset and tbt_tx_ack() clamps it to tx_seq, so
                 * equality means no DATA for this stream is still outstanding.
                 * A FIN that overtook gated DATA would make the receiver's
                 * rx_next stop below rx_fin_seq, and its drain would never
                 * reach EOF.  RST is not ordered and is exempt.  The FBK tick
                 * re-runs the pump, so waiting here is bounded.  A stream that
                 * has already left the session has no ack left to wait for;
                 * let its FIN go rather than pin a frame to a dead stream. */
                if (type == TBT_F_FIN && fr->st &&
                    test_bit(TBT_STREAM_F_ACTIVE, &fr->st->attached) &&
                    READ_ONCE(fr->st->tx_ack) != READ_ONCE(fr->st->tx_seq))
                    continue;
            }
            else
            {
                u16 avoid = fr->avoid_mask;
                /* DATA and stream-owned control use this stream's scheduler;
                 * FBK stays on the session-level control scheduler. */
                struct tbt_stream *sst =
                    type == TBT_F_FBK ? NULL : fr->st;

                /* No copy of a sequence may reuse a path another copy went
                 * out on, whichever copy is selected first. A retransmit has
                 * the same rule. If every path is used, fall back rather than
                 * stall, and count it (dup_same_path must stay 0 in the
                 * probe). */
                if (type == TBT_F_DATA && fr->ret)
                    avoid |= fr->ret->path_mask | fr->ret->reserve_mask;
                c = tbt_sched_pick(sess, sst, blocked | avoid,
                                   type == TBT_F_DATA);
                if (!c && avoid && fr->dup_copy)
                {
                    /* No path free of the primary: a duplicate on the same
                     * path is worse than none. The harness asserts
                     * dup_same_path == 0; suppress instead of falling back. */
                    sess->dup_suppressed++;
                    goto drop;
                }
                if (!c && avoid)
                    c = tbt_sched_pick(sess, sst, blocked,
                                       type == TBT_F_DATA);
                if (!c)
                    continue;
                if (!atomic_read(&c->up))
                    continue;
                if (type == TBT_F_DATA &&
                    (!fr->st || !tbt_stream_announced(fr->st, c)))
                    continue;
            }
            /* The real admission test tcp_sendmsg_locked() uses: the
             * socket's send memory, not an arbitrary 64 KiB line. When it is
             * full, the kernel will not call sk_write_space once the send
             * queue drains, so arm the bounded pump timer as the wake.
             *
             * The FBK is exempt. Control feedback must not share the DATA
             * admission gate: when every carrier is at its memory line the
             * feedback would die first, the peer's ack would freeze at its
             * last value, and the session would kill itself five seconds
             * later with no way to learn anything (measured: probe K, far
             * fbk_rx=0, tx_ack=0, both directions stuck at 64 KB). An FBK is
             * a few dozen bytes and tcp_sendmsg_locked still accepts it at
             * the line; a failure just re-queues it as -EAGAIN. */
            if (c->sk && type != TBT_F_FBK && !sk_stream_memory_free(c->sk))
            {
                blocked |= BIT(c->index);
                if (c->sk->sk_socket)
                    set_bit(SOCK_NOSPACE, &c->sk->sk_socket->flags);
                mod_delayed_work(system_dfl_wq, &c->w_pump,
                                 msecs_to_jiffies(2));
                sess->pump_hw_armed++;
                sess->dbg_pump_hiwat++;
                continue;
            }
            if (type == TBT_F_DATA)
            {
                if (!fr->st)
                    goto drop;
                if (fr->hdr.seq + fr->len <= READ_ONCE(fr->st->tx_ack))
                    goto drop; /* a retransmit copy the peer already has */
                if (!tbt_tx_window_ok(fr->st, fr))
                    continue;
                if (sess->tunnel->cfg.tunnel_rate && sess->tokens < fr->len)
                    continue;
            }
            /* Reserve the path now, not on write success: the duplicate copy
             * of this seq is selected later in the same pass and must see the
             * primary's path in the mask. */
            if (type == TBT_F_DATA && fr->ret)
            {
                struct tbt_retain *r = fr->ret;

                if (fr->dup_copy && ((r->path_mask | r->reserve_mask) & BIT(c->index)))
                    sess->dup_same_path++;
                r->reserve_mask |= BIT(c->index);
            }
            fr->path = c->index;
            list_move_tail(&fr->list, &batch);
            sess->tx_queued -= fr->len;
            if (type == TBT_F_DATA && sess->tunnel->cfg.tunnel_rate)
                sess->tokens -= fr->len;
            continue;
drop:
            /* FIN/RST never disappear here: a stale path is an explicit
             * failure, not a silent free. */
            if (fr->pinned && (type == TBT_F_FIN || type == TBT_F_RST))
                goto ctl_fail;
            if (fr->pinned)
                sess->ctl_obsolete++;
            list_del(&fr->list);
            sess->tx_queued -= fr->len;
            if (fr->dup_copy)
            {
                if (sess->dup_bytes >= fr->len)
                    sess->dup_bytes -= fr->len;
                if (fr->ret && fr->ret->dup_pending)
                    fr->ret->dup_pending--;
            }
            list_add_tail(&fr->list, &drop);
            progress = true;
            continue;
ctl_fail:
            list_del(&fr->list);
            sess->tx_queued -= fr->len;
            list_add_tail(&fr->list, &fail);
            progress = true;
        }
        }
        spin_unlock(&sess->tx_lock);
        list_for_each_entry_safe(fr, tmp, &drop, list)
        {
            list_del(&fr->list);
            if (fr->pinned)
                tbt_tx_ctl_put(sess, fr);
            tbt_frame_free(fr);
        }
        list_for_each_entry_safe(fr, tmp, &fail, list)
        {
            list_del(&fr->list);
            tbt_tx_ctl_fail(sess, fr);
        }

        list_for_each_entry_safe(fr, tmp, &batch, list)
        {
            struct tbt_carrier *c = READ_ONCE(sess->car[fr->path]);
            u8 type = fr->hdr.type;
            int err;

            list_del(&fr->list);
            if (!c)
            {
                /* The carrier vanished between selection and write. A
                 * FIN/RST keeps its promise and waits for the replacement;
                 * other pinned frames fail explicitly after the bound. */
                if (fr->pinned)
                {
                    if (tbt_tx_ctl_retry(sess, fr, true))
                        continue;
                    tbt_tx_ctl_fail(sess, fr);
                    continue;
                }
                tbt_frame_free(fr);
                continue;
            }
            /* Debug assertion, not a gate: a DATA frame remembers whether the
             * stream had been announced when it was enqueued. */
            if (fr->open_pending && type == TBT_F_DATA)
                WARN_ON_ONCE(1);
            fr->hdr.path_id = c->index;
            /* A frame with a stream reports that stream's contiguous RX
             * offset; FBK is path-level and stays on the session mirror. */
            /* Every frame that names a stream reports that stream's own
             * contiguous RX offset.  The FBK is sent per stream and its
             * header uack is applied to that sid by the receiver; the
             * session-wide mirror belongs to whichever stream last advanced
             * and would falsely ack a different stream's bytes. */
            if (fr->st)
                fr->hdr.uack = READ_ONCE(fr->st->rx_next);
            else
                fr->hdr.uack = READ_ONCE(sess->rx_ack);
            err = tbt_carrier_xmit(sess, c, fr);
            if (err)
            {
                /* -ENOMEM is a transient allocation failure (the HTTP header
                 * kmalloc), not a dead path. Treat it like backpressure:
                 * retry the frame on another path and leave the carrier
                 * alone. */
                if (err == -EAGAIN || err == -ENOMEM)
                {
                    /* The carrier's TCP buffer is full (or the ppm injector
                     * dropped the record): this is not a write. Release the
                     * path reservation, or the per-path high-water would later
                     * "prove" a frame that never reached the wire. */
                    if (fr->ret)
                    {
                        spin_lock(&sess->tx_lock);
                        fr->ret->reserve_mask &= ~BIT(c->index);
                        spin_unlock(&sess->tx_lock);
                    }
                    if (fr->pinned)
                    {
                        /* Never free a pinned frame on backpressure: put it
                         * back and let the 2 ms w_pump retry it. */
                        if (tbt_tx_ctl_retry(sess, fr, false))
                            continue;
                        tbt_tx_ctl_fail(sess, fr);
                        continue;
                    }
                    fr->path = 0xff;
                    fr->avoid_mask = BIT(c->index);
                    sess->dbg_pump_hiwat++;
                    tbt_tx_enqueue(sess, fr, false);
                    continue;
                }
                sess->dbg_pump_dead++;
                if (fr->ret)
                {
                    spin_lock(&sess->tx_lock);
                    fr->ret->reserve_mask &= ~BIT(c->index);
                    spin_unlock(&sess->tx_lock);
                }
                tbt_carrier_lost(sess, c);
                if (fr->pinned)
                {
                    /* -EIO may have written a prefix; the carrier dies, but
                     * the frame is still owed to the peer. It re-pins to the
                     * replacement or fails explicitly at the bound. */
                    if (tbt_tx_ctl_retry(sess, fr, true))
                        continue;
                    tbt_tx_ctl_fail(sess, fr);
                    continue;
                }
                /* Unpinned: give it another path. A short send may have put a
                 * partial record on the dead carrier, which is harmless because
                 * that carrier is closed; the receiver gets the whole frame
                 * from the new path. */
                fr->path = 0xff;
                fr->avoid_mask = BIT(c->index);
                tbt_tx_enqueue(sess, fr, false);
                continue;
            }
            progress = true;
            moved++;
            switch (type)
            {
            case TBT_F_HELLO:
                WRITE_ONCE(c->hello_sent, true);
                if (nsnap)
                {
                    /* The OPEN goes in front of every queued DATA frame, and
                     * `up` stays clear until it is written. */
                    atomic_set(&c->up, 0);
                    for (i = 0; i < nsnap; i++)
                        tbt_tx_announce_carrier(sess, snap[i], c,
                                                GFP_KERNEL);
                }
                else
                {
                    atomic_set(&c->up, 1);
                }
                /* An idle spare stays warm but out of the DATA scheduler. */
                c->state = (c->spare && !c->active) ? TBT_CAR_SPARE : TBT_CAR_UP;
                break;
            case TBT_F_OPEN:
                WRITE_ONCE(c->open_sent, true);
                if (fr->st)
                {
                    u8 idx = c->index;

                    WRITE_ONCE(fr->st->announced_gen[idx],
                               (u8)(c->gen & 0xf));
                    WRITE_ONCE(fr->st->announced_mask,
                               (u16)(READ_ONCE(fr->st->announced_mask) |
                                     BIT(idx)));
                    if (WARN_ON_ONCE(!tbt_stream_announced(fr->st, c)))
                        sess->dbg_announce_mismatch++;
                }
                else
                {
                    WARN_ON_ONCE(1);
                    sess->dbg_announce_mismatch++;
                }
                atomic_set(&c->up, 1);
                c->state = (c->spare && !c->active) ? TBT_CAR_SPARE : TBT_CAR_UP;
                break;
            case TBT_F_FIN:
            case TBT_F_RST:
                /* The old stream's close control is on the wire.  Give the
                 * far side a moment to process it before the next stream's
                 * OPEN (which is inserted at the queue head) can race it. */
                if (!READ_ONCE(sess->far_sid) && READ_ONCE(sess->n_pending))
                    queue_delayed_work(system_dfl_wq, &sess->w_pending,
                                       msecs_to_jiffies(1));
                break;
            case TBT_F_DATA:
            {
                struct tbt_retain *r = fr->ret;
                u64 now = tbt_now_ns();

                if (r)
                {
                    spin_lock(&sess->tx_lock);
                    r->path_mask |= BIT(c->index);
                    r->reserve_mask &= ~BIT(c->index);
                    r->path_gen[c->index] = c->gen & 0xf;
                    /* The mark is the byte just past this record. A short
                     * write left its tail in c->part, and xmit still reported
                     * success, so those bytes are not in TCP yet: count them,
                     * or the record looks acknowledged before it was even
                     * handed over and the RTO fires on a frame still in our
                     * own buffer. The injector wrote nothing, so its mark is
                     * where snd_nxt stood when it dropped the frame. */
                    r->first_sent_seq[c->index] = fr->dropped
                        ? c->drop_snd_nxt
                        : READ_ONCE(tcp_sk(c->sk)->write_seq) + c->part_len;
                    r->una_observed_ns[c->index] = 0;
                    if (fr->retx_copy)
                    {
                        sess->retx_frames++;
                        sess->retx_bytes += fr->len;
                        if (r->last_path < sess->n_car)
                        {
                            struct tbt_carrier *pc = READ_ONCE(sess->car[r->last_path]);

                            if (pc)
                                pc->lost_frames++;
                        }
                    }
                    r->last_path = c->index;
                    r->last_sent_ns = now;
                    r->pending = false;
                    r->retx_now = false;
                    if (!r->first_sent_ns)
                        r->first_sent_ns = now;
                    if (r->sends < U8_MAX)
                        r->sends++;
                    if (fr->dup_copy && r->dup_pending)
                        r->dup_pending--;
                    spin_unlock(&sess->tx_lock);
                }
                if (fr->dup_copy)
                    sess->dup_tx++;
                c->payload_bytes_sent += fr->len;
                if (r)
                    r->path_end[c->index] = c->payload_bytes_sent;
                sess->stats_tx += fr->len;
                break;
            }
            default:
                break;
            }
            if (fr->pinned)
                tbt_tx_ctl_written(sess, fr);
            tbt_frame_free(fr);
        }

        if (!progress)
            break;
    }

    /* Space is available again: stop asking TCP for a wake on every ACK, and
     * let a capture paused on the queue cap refill it. Disarm the pump timers
     * once no carrier is full. */
    for (int i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);

        if (!c)
            continue;
        if (c->sk && sk_stream_memory_free(c->sk))
        {
            if (c->sk->sk_socket)
                clear_bit(SOCK_NOSPACE, &c->sk->sk_socket->flags);
            if (cancel_delayed_work(&c->w_pump))
                sess->pump_hw_disarmed++;
        }
    }
    if (READ_ONCE(sess->tx_queued) <= TBT_SQ_MAX / 2)
    {
        struct tbt_stream *it;
        int i;

        rcu_read_lock();
        for (i = 0; i < TBT_STREAM_HT; i++)
        {
            hlist_for_each_entry_rcu(it, &sess->streams[i], node)
            {
                if (!READ_ONCE(it->tx_paused))
                    continue;
                WRITE_ONCE(it->tx_paused, false);
                schedule_work(&it->w_read);
            }
        }
        rcu_read_unlock();
    }
    sess->dbg_pump_moved += moved;
    mutex_unlock(&sess->pump_lock);
    for (i = 0; i < nsnap; i++)
        tbt_stream_put(snap[i]);
    return moved;
}

// ---------------------------------------------------------------------------
// Capture

// Take `len` bytes at `off` in `skb` and queue them for the carrier.
//
// Allocation is GFP_ATOMIC on purpose: the same function is reached from a
// tcp_read_sock() actor, and tcp_read_sock is also called from sk_data_ready
// in softirq. A GFP_KERNEL allocation there is a "sleeping in atomic context"
// splat. Returns the number of bytes consumed, which is what the actor
// reports; a short return leaves the rest in the public socket.
size_t tbt_tx_submit(struct tbt_stream *st, struct sk_buff *skb, unsigned int off, size_t len)
{
    struct tbt_session *sess = st->sess;
    size_t done = 0;

    sess->tx_submit_calls++;

    while (done < len)
    {
        u16 chunk = min_t(size_t, len - done, (size_t)TBT_PAYLOAD_MAX);
        struct tbt_retain *r;
        struct tbt_frame *fr;
        u64 seq;

        spin_lock(&sess->tx_lock);
        if (sess->tx_queued + chunk > TBT_TXQ_HARD_MAX)
        {
            spin_unlock(&sess->tx_lock);
            /* Backpressure, not a loss: the bytes stay in the public socket's
             * receive queue, its window closes, and the application's send()
             * blocks. */
            WRITE_ONCE(st->tx_paused, true);
            sess->dbg_over_qmax++;
            break;
        }
        spin_unlock(&sess->tx_lock);

        r = tbt_retain_alloc(chunk, GFP_ATOMIC);
        fr = tbt_frame_alloc(chunk, GFP_ATOMIC);
        if (!r || !fr)
        {
            kfree(r);
            kfree(fr);
            sess->dbg_alloc_fail++;
            break;
        }
        if (skb_copy_bits(skb, off + done, r->data, chunk))
        {
            kfree(r);
            kfree(fr);
            sess->dbg_copy_fail++;
            break;
        }
        memcpy(fr->data, r->data, chunk);
        r->hdr = (struct tbt_hdr){
            .type = TBT_F_DATA,
            .body_len = chunk,
            .sid_lo = (u32)st->sid,
            .conv = TBT_CONV,
        };
        r->hdr.sid_hi = (u8)(st->sid >> 32);

        spin_lock(&sess->tx_lock);
        if (sess->tx_queued + chunk > TBT_TXQ_HARD_MAX)
        {
            spin_unlock(&sess->tx_lock);
            kfree(r);
            kfree(fr);
            WRITE_ONCE(st->tx_paused, true);
            sess->dbg_over_qmax++;
            break;
        }
        if (!st->tx_start_ns)
            st->tx_start_ns = tbt_now_ns();
        seq = st->tx_seq;
        st->tx_seq = seq + chunk;
        r->seq = seq;
        r->hdr.seq = seq;
        fr->hdr = r->hdr;
        fr->hdr.seq = seq;
        fr->open_pending = st->near && !READ_ONCE(st->tx_open_sent);
        fr->ret = r;
        fr->st = st;
        refcount_inc(&st->refs);
        sess->tx_queued += chunk;
        list_add_tail(&fr->list, &sess->tx_q);
        list_add_tail(&r->list, &st->retain);
        st->retain_bytes += chunk;
        spin_unlock(&sess->tx_lock);

        /* Duplicates go on paths the primary has not used. The inflation cap
         * is checked against the *projected* ratio, so the measured value
         * cannot exceed max_inflate (headers and retransmits excluded: they
         * are not duplication). */
        for (u32 k = 0; k < sess->tunnel->cfg.dup; k++)
        {
            u64 goodput = READ_ONCE(sess->tx_submit_bytes) + chunk;
            u64 dup_after = READ_ONCE(sess->dup_bytes) + chunk;
            struct tbt_frame *dup;

            if ((goodput + dup_after) * 1000 >
                goodput * sess->tunnel->cfg.max_inflate_milli)
            {
                sess->dup_suppressed++;
                break;
            }
            dup = tbt_frame_alloc(chunk, GFP_ATOMIC);
            if (!dup)
            {
                sess->dbg_alloc_fail++;
                break;
            }
            dup->hdr = fr->hdr;
            dup->hdr.flags |= TBT_FF_LOW_PRIO;
            dup->dup_copy = true;
            dup->ret = r;
            memcpy(dup->data, r->data, chunk);
            spin_lock(&sess->tx_lock);
            if (sess->tx_queued + chunk > TBT_TXQ_HARD_MAX)
            {
                spin_unlock(&sess->tx_lock);
                kfree(dup);
                break;
            }
            sess->tx_queued += chunk;
            sess->dup_bytes += chunk;
            if (r->dup_pending < U8_MAX)
                r->dup_pending++;
            dup->st = st;
            refcount_inc(&st->refs);
            list_add_tail(&dup->list, &sess->tx_q);
            spin_unlock(&sess->tx_lock);
        }

        done += chunk;
        sess->tx_submit_bytes += chunk;
    }

    /* Always wake the carrier writer, including on the short return that hit
     * the queue cap. Skipping the wake there left a full queue with no
     * scheduled pump. */
    tbt_tx_wake(sess);
    return done;
}

// ---------------------------------------------------------------------------
// Feedback

/* Our receive state as one nibble per path: 0 ok, 1 hole outstanding,
 * 2 delivery queue full, 3 stream dead. */
static u8 tbt_rx_state(struct tbt_stream *st)
{
    if (READ_ONCE(st->state) == TBT_ST_DEAD)
        return 3;
    if (READ_ONCE(st->rx_queued) >= TBT_RX_PAUSED_MARK)
        return 2;
    if (READ_ONCE(st->rx_ooo_frames))
        return 1;
    return 0;
}

int tbt_tx_fbk(struct tbt_session *sess, struct tbt_stream *st, gfp_t gfp)
{
    u8 body[TBT_FBK_MAX];
    u32 nr = 0;
    u16 mask = 0;
    u8 state = tbt_rx_state(st);
    struct tbt_frame *fr;
    int i;

    for (i = 0; i < sess->n_car && nr < TBT_MAX_CARRIERS_LIMIT; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);
        u8 *e;
        u32 rtt = 0;

        if (!c || !atomic_read(&c->up))
            continue;
        if (c->sk)
            rtt = READ_ONCE(tcp_sk(c->sk)->srtt_us) >> 3;
        WRITE_ONCE(c->rx_state, state);
        e = body + nr * TBT_FBK_ENTRY_LEN;
        /* The generation in feedback is the *peer's* generation for this path
         * (learned from its HELLO), because the entry describes the frames the
         * peer sent on it. The peer compares it to its own carrier gen. */
        e[TBT_FBK_PATH] = (u8)i | ((c->peer_gen & 0xf) << 4);
        e[TBT_FBK_RXSTATE] = state;
        /* Per-path cumulative high water: a later byte on this carrier proves
         * every earlier frame on it arrived (TCP is ordered per path). The
         * FBK header's uack stays the global contiguous offset used by the
         * send window and retention. */
        tbt_put_le64(e + TBT_FBK_UACK, READ_ONCE(c->rx_bytes));
        tbt_put_le32(e + TBT_FBK_RECVCNT, (u32)c->rx_frames);
        tbt_put_le32(e + TBT_FBK_RTT, rtt);
        mask |= BIT(i);
        nr++;
    }
    if (!nr)
        return 0;
    fr = tbt_frame_alloc(nr * TBT_FBK_ENTRY_LEN, gfp);
    if (!fr)
        return -ENOMEM;
    memcpy(fr->data, body, nr * TBT_FBK_ENTRY_LEN);
    /* Pin the feedback to a spare carrier when there is one. The FBK and the
     * DATA records share a carrier's ordered byte stream, so one record that
     * the peer's parser cannot finish blocks every FBK behind it -- and the
     * feedback is exactly what would unblock it (measured: probe J, far
     * fbk_rx=0 with tx_ack frozen at 0 for 5 s). A spare carries no DATA by
     * design; it is the control plane. Fall back to normal scheduling when
     * no spare is usable. */
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *sp = READ_ONCE(sess->car[i]);

        if (sp && sp->spare && atomic_read(&sp->up) &&
            !atomic_read(&sp->stopping))
        {
            fr->pinned = true;
            fr->path = (u8)i;
            fr->gen = sp->gen;
            break;
        }
    }
    fr->hdr = (struct tbt_hdr){
        .type = TBT_F_FBK,
        .body_len = nr * TBT_FBK_ENTRY_LEN,
        .path_ack_count = nr,
        .path_ack_mask = mask,
        .sid_lo = (u32)st->sid,
        .seq = 0,
        .conv = TBT_CONV,
    };
    fr->hdr.sid_hi = (u8)(st->sid >> 32);
    fr->st = st;
    refcount_inc(&st->refs);
    /* Head, not tail: the FBK carries the receiver's recv_cnt and per-path
     * uack, and the retransmit copies are inserted at the head. Queued behind
     * them, the feedback goes stale exactly during a storm, recv_lag grows,
     * tbt_retain_received() loses the receipt proof, and the timer re-sends
     * even more -- a loop that feeds itself (measured: near sent 1153 FBKs,
     * far received 896; per-path rx_lag_ppm 171k-526k with dropped=0). One
     * small control frame ahead of data costs at most one record of delay. */
    tbt_tx_enqueue(sess, fr, true);
    sess->fbk_tx++;
    tbt_tx_wake(sess);
    return 0;
}

/* Feedback tick: one FBK per session every TBT_FBK_INTERVAL_MS while a
 * stream exists, or immediately after TBT_FBK_FRAMES received DATA frames. It
 * is also the retransmit timer's driver: a pump run here re-queues frames whose
 * last write is older than the RTO, even when no other event arrives. */
void tbt_fbk_kick(struct tbt_session *sess, bool now)
{
    if (!sess->tunnel)
        return;
    /* The feedback tick carries every ack the sender has. Sharing system_wq
     * with the data-path works let it slip hundreds of milliseconds under
     * load, which starves the retransmit timer's view of the ack. */
    if (now)
        mod_delayed_work(system_dfl_wq, &sess->w_fbk, 0);
    else if (!work_pending(&sess->w_fbk.work))
        queue_delayed_work(system_dfl_wq, &sess->w_fbk,
                           msecs_to_jiffies(TBT_FBK_INTERVAL_MS));
}

int tbt_tx_sack(struct tbt_session *sess, struct tbt_stream *st, gfp_t gfp)
{
    struct tbt_frame *fr;
    u8 state = tbt_rx_state(st);

    fr = tbt_frame_alloc(1, gfp);
    if (!fr)
        return -ENOMEM;
    fr->data[0] = state;
    fr->hdr = (struct tbt_hdr){
        .type = TBT_F_SACK,
        .body_len = 1,
        .seq = 0,
        .uack = READ_ONCE(st->rx_next),
        .conv = TBT_CONV,
    };
    tbt_hdr_set_sid(&fr->hdr, st->sid);
    fr->st = st;
    refcount_inc(&st->refs);
    tbt_tx_enqueue(sess, fr, true);
    tbt_tx_wake(sess);
    sess->sack_tx++;
    return 0;
}

void tbt_fbk_work(struct work_struct *w)
{
    struct tbt_session *sess = container_of(to_delayed_work(w), struct tbt_session, w_fbk);
    struct tbt_stream *snap[TBT_STREAM_SNAP_MAX];
    bool alive = false;
    int nsnap, i;

    if (!sess->tunnel)
        return;
    nsnap = tbt_session_stream_snapshot(sess, snap, ARRAY_SIZE(snap));
    tbt_tx_path_lost_drain(sess, snap, nsnap);
    for (i = 0; i < nsnap; i++)
    {
        struct tbt_stream *st = snap[i];
        u8 state = tbt_rx_state(st);
        bool one_alive;

        st->fbk_frames = 0;
        tbt_tx_fbk(sess, st, GFP_KERNEL);
        one_alive = READ_ONCE(st->state) != TBT_ST_DEAD;
        if (READ_ONCE(st->sack_dirty) ||
            READ_ONCE(st->sack_next) != READ_ONCE(st->rx_next) ||
            st->sack_state != state)
        {
            tbt_tx_sack(sess, st, GFP_KERNEL);
            st->sack_next = READ_ONCE(st->rx_next);
            st->sack_state = state;
            WRITE_ONCE(st->sack_dirty, false);
        }
        if (one_alive)
            tbt_rx_check_hole(st);
        tbt_tx_stall_check(sess, st);
        /* The capture work emits a FIN once.  If no carrier was usable then,
         * the request stays set and is retried here; only a FIN that actually
         * entered the queue may trigger the detach. */
        if (one_alive && READ_ONCE(st->tx_fin_sent) &&
            !READ_ONCE(st->tx_fin_queued) &&
            test_bit(TBT_STREAM_F_ACTIVE, &st->attached) &&
            tbt_tx_fin(sess, st, GFP_KERNEL) > 0)
        {
            WRITE_ONCE(st->tx_fin_queued, true);
            if (READ_ONCE(st->rx_fin_done))
                tbt_stream_detach_from_session(st);
        }
        alive |= one_alive;
    }
    for (i = 0; i < nsnap; i++)
        tbt_stream_put(snap[i]);
    tbt_tx_pump(sess);
    if (alive && !READ_ONCE(sess->closing))
        queue_delayed_work(system_dfl_wq, &sess->w_fbk,
                           msecs_to_jiffies(TBT_FBK_INTERVAL_MS));
}

void tbt_tx_frame_free(struct tbt_frame *fr)
{
    tbt_frame_free(fr);
}

void tbt_tx_free_queue(struct tbt_session *sess)
{
    LIST_HEAD(free);
    struct tbt_frame *fr, *tmp;

    spin_lock(&sess->tx_lock);
    list_for_each_entry_safe(fr, tmp, &sess->tx_q, list)
    {
        list_del(&fr->list);
        list_add_tail(&fr->list, &free);
    }
    sess->tx_queued = 0;
    sess->ctl_queued = 0;
    spin_unlock(&sess->tx_lock);
    list_for_each_entry_safe(fr, tmp, &free, list)
    {
        list_del(&fr->list);
        tbt_frame_free(fr);
    }
}

void tbt_tx_free_stream(struct tbt_stream *st)
{
    struct tbt_retain *r, *tmp;

    /* Called from the stream's RCU callback: the stream is unreachable, all of
     * its work items are cancelled, and the session may already be gone, so
     * this walks the list without the session lock. */
    list_for_each_entry_safe(r, tmp, &st->retain, list)
    {
        list_del(&r->list);
        kfree(r);
    }
    st->retain_bytes = 0;
}
