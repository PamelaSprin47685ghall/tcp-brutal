/* tbt_tx.c - public bytes -> frames -> carriers, and the feedback that makes
 * the scheduler and the retention window work.
 *
 * C3 scope: one stream per session, up to 16 carriers for the legacy ARQ,
 * duplication up to TBT_DUP_MAX copies, and no prediction (C5/C6). Every DATA
 * frame is assigned a path when the pump is about to write it, using the
 * lowest per-path inflight with a 64-frame striping window; the frame is kept
 * in a per-stream retention list until the peer's delivered byte offset has
 * passed it, so a lost carrier or a ppm-dropped record can be re-sent on
 * another path.
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
#include "tbt_tx_acct.h"

/* A frame's payload length travels as u16 and is bounded by TBT_PAYLOAD_MAX;
 * the two must not drift apart. */
static_assert(TBT_PAYLOAD_MAX <= U16_MAX, "payload length must fit u16");

/* Pure bound test, no warning: a caller may legally walk a FEC session's 256
 * slots. */
static inline bool tbt_arq_path_ok(const struct tbt_session *sess,
                                   unsigned int idx)
{
    return tbt_tx_path_in_range(idx, READ_ONCE(sess->n_car));
}

/* Index into the 16-slot retain arrays. Any out-of-range index is a code
 * error: warn once and refuse the write. The warning lives here, not at each
 * write site, so a FEC path with 256 slots never trips it. */
static inline bool tbt_retain_path_ok(struct tbt_session *sess,
                                      unsigned int idx)
{
    bool warned = false;
    bool ok = tbt_tx_retain_path_in_range(idx, READ_ONCE(sess->n_car),
                                          &warned);

    if (warned)
        WARN_ON_ONCE(1);
    return ok;
}

/* The send queue's bound is derived from the sender window (sess->txq_max):
 * the queue only has to hold the frames the window admits while the carriers
 * drain. The old fixed 96 KiB (TBT_SQ_MAX + 4 chunks) was calibrated when the
 * window itself was 128 KiB; with a 1-8 MiB window it paused capture every
 * 64 KiB and left the carriers idle between bursts. TBT_SQ_MAX remains the
 * floor. The memory bound is window (retention) + txq_max (queue) + the
 * carriers' own buffers, all bounded by the same window. TBT_RX_RING_MAX is
 * the *receive* delivery ring (64 MiB) and must never be used here: it let
 * the queue reach 13.8-27 MB and starved capture (measured: Q7 baseline,
 * txq_max 13.8 MB, app 1.31 Mbit/s). */
#define TBT_TXQ_MIN (TBT_SQ_MAX + 4 * TBT_CHUNK_MAX)

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
static void tbt_tx_ctl_arm(struct tbt_session *sess, struct tbt_carrier *pref,
                           unsigned int ms);

/* Defined below; tbt_session.c also calls it from the purge path, so it is
 * not static and carries the same contract there. */
void tbt_tx_frame_drop_account(struct tbt_session *sess, struct tbt_frame *fr);

/* A stream with at most two records outstanding is control-like: its next
 * bytes are what the peer's application needs to make progress, not bulk.
 * Both carrier admission gates exempt it, so a bulk stream that fills the
 * peer window or the socket memory cannot starve it (measured: the 172nd
 * byte of an iperf3 control stream sat in tx_q while the bulk used the whole
 * 785 KB window, and the sender's stall timer killed the stream). */
static bool tbt_stream_small_backlog(const struct tbt_stream *st)
{
    u64 seq, ack;

    if (!st)
        return false;
    seq = READ_ONCE(st->tx_seq);
    ack = READ_ONCE(st->tx_ack);
    return seq >= ack && seq - ack <= 2 * TBT_PAYLOAD_MAX;
}

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
    /* The pump reads this on the write path to pick first_sent_seq; a
     * kmalloc()ed frame must not inherit the previous slab occupant's value. */
    fr->dropped = false;
    fr->retx_src = TBT_RETX_NONE;
    fr->path = TBT_PATH_NONE;
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

    r = kzalloc(struct_size(r, data, len), gfp);
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
            atomic64_inc(&sess->tx_wake_calls);
            if (!schedule_work(&c->w_tx))
                atomic64_inc(&sess->tx_wake_busy);
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

/* The one way a frame names a carrier. A frame without a path (or with a
 * stale index past the configured count) resolves to NULL; it is never
 * allowed to index sess->car[] with a value that could be TBT_PATH_NONE or a
 * number the session does not own. */
static struct tbt_carrier *tbt_frame_carrier(struct tbt_session *sess,
                                             const struct tbt_frame *fr)
{
    if (!sess || fr->path >= sess->n_car)
        return NULL;
    return READ_ONCE(sess->car[fr->path]);
}

/* True when this stream's OPEN was written on this exact carrier
 * (index, generation). OPEN announce, the pump's OPEN duplicate check and the
 * DATA gate use it; the scheduler still uses c->open_sent until S4. */
static bool tbt_stream_announced(const struct tbt_stream *st,
                                 const struct tbt_carrier *c)
{
    if (!st || !c || c->index >= TBT_MAX_CARRIERS_LIMIT)
        return false;
    if (!test_bit(c->index, st->announced_mask))
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
 * copies of FIN/RST. `path` is a carrier index below sess->n_car, or
 * TBT_PATH_NONE for a frame the pump must refuse rather than misroute; the
 * pump resolves it through tbt_frame_carrier(). `st` is the owning stream for
 * OPEN/FIN/RST/SACK/FBK, or NULL for session-level HELLO, PAD and RETIRE
 * frames. */
int tbt_tx_control_pinned(struct tbt_session *sess, struct tbt_stream *st,
                          u16 path, u8 type, u64 sid, u64 seq,
                          const u8 *body, u16 len, bool head, gfp_t gfp)
{
    struct tbt_frame *fr = tbt_frame_alloc(len, gfp);

    if (!fr)
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: control frame alloc failed: path=%u type=%u len=%u (-ENOMEM)\n",
                            (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
                            path, type, len);
        return -ENOMEM;
    }
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
        .path_id = (u8)path,
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
        TBT_DIAG_WARN("ctl queue full tunnel=%llu path=%u type=%u len=%u ctl_queued=%u: -ENOSPC",
                      (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
                      path, type, len, READ_ONCE(sess->ctl_queued));
        return -ENOSPC;
    }
    if (head)
        list_add(&fr->list, &sess->tx_q);
    else
        list_add_tail(&fr->list, &sess->tx_q);
    sess->tx_queued += fr->len;
    sess->ctl_queued++;
    spin_unlock(&sess->tx_lock);
    if (type == TBT_F_HELLO)
        TBT_DIAG("HELLO queued tunnel=%llu carrier=%u gen=%u len=%u ctl_queued=%u tx_queued=%u",
                 (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
                 path, fr->gen, len, READ_ONCE(sess->ctl_queued),
                 READ_ONCE(sess->tx_queued));
    tbt_tx_wake(sess);
    /* schedule_work(&c->w_tx) is lost when that work is already running and
     * has passed the queue scan. A control frame that is the last thing a
     * dying stream sends (the RST) then waits for a wake that never comes:
     * the stream is dead, so the feedback tick stops re-arming. The bounded
     * pump is a delayed work, so mod_delayed_work always leaves one run
     * pending (measured: near.rst_eagain, the RST queued at 12.63 s and never
     * written while the far drained for 60 s). */
    tbt_tx_ctl_arm(sess, NULL, 0);
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
    u8 body[TBT_FIN_BODY_LEN] = {0};
    int i, queued = 0, err = 0;

    /* FEC: the packetizer must finish the stream's last block before FIN
     * names its end. The first call only requests the flush and returns 0;
     * the close path retries. Once finish has run, pkt_end is frozen and the
     * body can name it. */
    if (sess->fec)
    {
        if (!smp_load_acquire(&st->fec_finished))
        {
            tbt_fec_tx_flush(st);
            return 0;
        }
        if (!tbt_fec_tx_idle(st))
            return 0;
    }
    spin_lock(&sess->tx_lock);
    /* Freeze the offset once: a retry must name the same byte, even if a late
     * capture submitted more DATA after the first request. The FEC packet
     * bounds are frozen with it: pkt_end is the packet after the last DATA
     * packet, pkt_base the stream's first packet (or pkt_end when the stream
     * sent no DATA). */
    if (!st->tx_fin_seq_set)
    {
        st->tx_fin_seq = st->tx_seq;
        if (sess->fec)
        {
            st->tx_fin_pkts = READ_ONCE(st->fec_pkt_end);
            st->tx_fin_pkt_base = st->fec_pkt_base_set
                                      ? READ_ONCE(st->fec_pkt_base)
                                      : READ_ONCE(st->fec_pkt_end);
        }
        st->tx_fin_seq_set = true;
    }
    seq = st->tx_fin_seq;
    spin_unlock(&sess->tx_lock);
    tbt_put_le64(body, st->tx_fin_pkts);
    tbt_put_le64(body + 8, st->tx_fin_pkt_base);
    /* The close timer starts at the first request, not at the write: a FIN
     * that never gets a carrier, or whose preceding DATA the peer never
     * acknowledges, must end in a bounded reset instead of a pinned stream. */
    if (!READ_ONCE(st->close_since_ns))
        WRITE_ONCE(st->close_since_ns, tbt_now_ns());
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);

        /* A carrier that has sent HELLO can carry this stream.  It may be
         * momentarily up==0 while its OPEN is being enqueued; the pump leaves
         * a pinned frame queued until the carrier is up again, so queue the
         * FIN anyway instead of dropping it silently. */
        if (!c || !READ_ONCE(c->hello_sent) || atomic_read(&c->stopping))
            continue;
        if (test_and_set_bit(i, st->tx_fin_mask))
            continue;
        /* Set the in-flight bit before the frame can be seen by the pump: the
         * pump clears it on the write, and a stale bit would hide the loss of
         * the last copy. */
        set_bit(i, st->tx_fin_inflight);
        err = tbt_tx_control_pinned(sess, st, (u8)i, TBT_F_FIN, st->sid,
                                    seq, body, sizeof(body), false, gfp);
        if (err)
        {
            clear_bit(i, st->tx_fin_inflight);
            clear_bit(i, st->tx_fin_mask);
            if (err == -ENOMEM)
                atomic64_inc(&sess->dbg_alloc_fail);
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
        if (test_and_set_bit(i, st->tx_rst_mask))
            continue;
        set_bit(i, st->tx_rst_inflight);
        err = tbt_tx_control_pinned(sess, st, (u8)i, TBT_F_RST, st->sid, 0, NULL, 0,
                                    false, gfp);
        if (err)
        {
            clear_bit(i, st->tx_rst_inflight);
            clear_bit(i, st->tx_rst_mask);
            if (err == -ENOMEM)
                atomic64_inc(&sess->dbg_alloc_fail);
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

    for (i = 0; i < sess->n_car && i < TBT_ARQ_MAX_PATHS; i++)
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
 * anywhere else would let one of them dereference it after the free.
 *
 * Contract with tbt_tx_submit(): a DATA frame's retain entry is published
 * under tx_lock together with the frame and every duplicate, and submit never
 * touches r after publish returns. That is why clearing fr->ret here is
 * enough; no capture-side reference survives the publish. */
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

/* The carrier's own TCP stack measures the path RTT from its ACKs; the FBK
 * estimate is the peer's view and can be stale or zero. Use the larger, or a
 * shaped 40 ms path looks like a 1 ms one and every delayed frame is declared
 * lost (measured: probe I retransmitted the whole payload). */
static u32 tbt_path_rtt_us(struct tbt_carrier *c)
{
    u32 rtt = READ_ONCE(c->rtt_us);
    u32 srtt = 0;
    struct sock *sk;

    sk = tbt_carrier_sk_get(c);
    if (sk)
    {
        srtt = READ_ONCE(tcp_sk(sk)->srtt_us) >> 3;
        tbt_carrier_sk_put(sk);
    }
    return srtt > rtt ? srtt : rtt;
}

/* One carrier's BBR bandwidth sample, in bytes per second. This kernel does
 * not keep a `delivery_rate` field on struct tcp_sock: tcp_rate_gen() stores
 * the raw sample BBR's bw filter consumes, rate_delivered packets over
 * rate_interval_us. Use that, rather than inventing a second estimator.
 * rate_app_limited says the sender, not the path, limited the sample; it is
 * still an upper bound on the rate, which is what a window needs. */
static u64 tbt_car_delivery_bps(const struct tcp_sock *tp)
{
    u32 delivered = READ_ONCE(tp->rate_delivered);
    u32 interval = READ_ONCE(tp->rate_interval_us);
    u32 mss = READ_ONCE(tp->mss_cache);

    if (!delivered || !interval || !mss)
        return 0;
    return (u64)delivered * mss * 1000000ULL / interval;
}

/* The byte budget behind the auto window: W = k x B.
 *
 * B is the aggregate BDP, sum(rate_i x (srtt_i + feedback_delay)): the bytes
 * an average carrier can have in flight while one round trip plus the tunnel
 * ack elapses. rate_i and srtt_i are each carrier's own TCP BBR estimator
 * (rate_delivered/rate_interval_us, srtt_us); this kernel has no
 * tcp_sock.delivery_rate field, and tcp_rate_gen publishes exactly the sample
 * BBR's bandwidth filter consumes. The feedback delay is the measured uack
 * lag after the carrier TCP ack, floored at the 10 ms FBK cadence. The rate
 * is an EWMA by construction, and a carrier with no sample contributes
 * nothing, so a new carrier cannot distort the aggregate.
 *
 * k is the loss-decision time in units of the mean RTT: k = RTO / mean_srtt,
 * clamped to 2..5. The RTO is SRTT + 4 x RTTVAR (the same shape TCP uses),
 * floored at 200 ms, so the jitter is absorbed by k rather than by using the
 * slowest carrier's absolute RTT as the expected demand. This is a target,
 * not a bound: the ceiling is the receiver's reorder budget R (or a WINDOW
 * credit), never sum(2 x BDP_i), a single mean sample, or
 * sum(snd_cwnd)/sum(snd_wnd). A carrier TCP releases cwnd when its own ACK
 * arrives while those bytes stay in the receiver's tree, and a cwnd sum can
 * exceed the tree and overflow it.
 *
 * The capacity identity is R >= W = k x B: the loss clock starts when a frame
 * is sent, so the receiver's peak reorder occupancy is the whole window (the
 * last BDP is still in flight and arrives before the repair). A configuration
 * with R < k x B must lower k or raise R; the sender never exceeds R. */
static u64 tbt_tx_bdp_window(struct tbt_session *sess, u32 *mean_out,
                             u32 *var_out, u32 *max_out)
{
    u64 sum_rate = 0, sum_rtt_rate = 0;
    u32 rtt_max = 0, var_max = 0;
    int i;

    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);
        struct sock *sk;
        u32 srtt = 0, mdev = 0;
        u64 rate = 0;

        if (!c)
            continue;
        sk = tbt_carrier_sk_get(c);
        if (sk)
        {
            struct tcp_sock *tp = tcp_sk(sk);

            srtt = READ_ONCE(tp->srtt_us) >> 3;
            mdev = READ_ONCE(tp->mdev_us);
            rate = tbt_car_delivery_bps(tp);
            tbt_carrier_sk_put(sk);
        }
        if (!srtt)
            srtt = READ_ONCE(c->rtt_us);
        tbt_carrier_put(c);
        if (!srtt || !rate)
            continue;
        sum_rate += rate;
        sum_rtt_rate += (u64)srtt * rate;
        if (srtt > rtt_max)
            rtt_max = srtt;
        if (mdev > var_max)
            var_max = mdev;
    }
    if (mean_out)
        *mean_out = sum_rate ? (u32)(sum_rtt_rate / sum_rate) : 0;
    if (var_out)
        *var_out = var_max;
    if (max_out)
        *max_out = rtt_max;
    if (!sum_rate)
        return 0;
    {
        u32 mean = (u32)(sum_rtt_rate / sum_rate);
        u64 fb = READ_ONCE(sess->fb_delay_us);
        u64 rtt_eff;
        u64 bdp;
        u64 rto;
        u64 k;

        fb = max_t(u64, fb, (u64)TBT_FBK_INTERVAL_MS * 1000);
        rtt_eff = (u64)mean + fb;
        bdp = rtt_eff * sum_rate / 1000000ULL;
        sess->bdp_bytes = bdp > U32_MAX ? U32_MAX : (u32)bdp;
        /* The same RTO shape as tbt_tx_rto_ns(), without a stream's
         * bootstrap state: the worst carrier sets it, floored at 200 ms. */
        rto = max_t(u64, (u64)rtt_max + 4ULL * var_max, 200000ULL);
        k = rto / mean;
        k = clamp_t(u64, k, 2, 5);
        sess->k_x100 = (u32)(k * 100);
        return k * bdp;
    }
}

/* Effective sender window: how far a frame may be ahead of the peer's
 * delivered offset. Explicit config wins; auto uses the BBR-derived BDP with
 * a 1 MiB floor (a path with no sample yet still gets a window that can
 * carry a 218 ms, 17 Mbit BDP). The peer's reorder budget is the ceiling:
 * a frame beyond it is a stream kill, not a retransmit. */
static u32 tbt_tx_window(struct tbt_session *sess)
{
    const struct tbt_cfg *cfg = &sess->tunnel->cfg;
    u64 cap = cfg->rx_reorder_bytes;
    u32 mean = 0, var = 0, max = 0;
    u64 win;

    /* Always refresh the RTT gauges, so /proc shows the estimator inputs even
     * when the window is configured explicitly. */
    win = tbt_tx_bdp_window(sess, &mean, &var, &max);
    sess->rtt_mean_us = mean;
    sess->rttvar_us = var;
    if (max)
        sess->rtt_max_us = max;
    if (cfg->tx_window_bytes)
        win = cfg->tx_window_bytes;
    else if (!win)
        /* Bootstrap only: no carrier has a rate/RTT sample yet, so k x B is
         * undefined. 1 MiB lets the first frames establish the samples. */
        win = TBT_TX_WINDOW_AUTO_FLOOR;
    /* Liveness: two maximum frames must fit, or no frame can ever pass and
     * the stream deadlocks. This is a frame-granularity floor, not a 1 MiB
     * one: once samples exist, W = k x B and the floor must not override it
     * (a 70 KB BDP at k=2 would otherwise become a 15x-B window whose 3.4 s
     * drain breaks the hole budget). */
    if (win < 2 * TBT_PAYLOAD_MAX)
        win = 2 * TBT_PAYLOAD_MAX;
    if (win > cap)
        win = cap;
    return win ? (u32)win : 1;
}

/* Recompute once per pump pass, not per frame. Grow at once; shrink only
 * after a 25% overshoot, so one stale rate sample cannot collapse a window
 * that is already filling the path. */
static void tbt_tx_window_update(struct tbt_session *sess)
{
    u32 old = sess->tx_window;
    u32 win = tbt_tx_window(sess);
    u64 now = tbt_now_ns();
    bool recent_loss = sess->last_loss_ns &&
                       now - sess->last_loss_ns < NSEC_PER_SEC;

    /* Grow at once: the anti-self-lock rule is that a full window with no
     * loss for 1 s must reach k x B, and growing at once dominates the
     * 25%/s ramp. Shrink only after a 25% overshoot and only when no loss
     * was seen in the last second: a hole is evidence about the path, not a
     * reason to collapse the window over and over. The 1 MiB floor lives in
     * tbt_tx_window(). */
    if (!old || win > old || (!recent_loss && win + win / 4 < old))
        sess->tx_window = win;
    sess->txq_max = sess->tx_window + 8 * TBT_CHUNK_MAX;
}

/* Session birth: no carrier has a rate sample yet, so this lands on the 1 MiB
 * floor (or the configured value) and gives capture and the carriers a bound
 * before the first ACK. The pump recomputes it from then on. */
void tbt_tx_window_init(struct tbt_session *sess)
{
    tbt_tx_window_update(sess);
}

static bool tbt_tx_window_ok(struct tbt_stream *st, const struct tbt_frame *fr)
{
    return fr->hdr.seq + fr->len <=
           READ_ONCE(st->tx_ack) + READ_ONCE(st->sess->tx_window);
}

/* Retransmit timeout, from the same two terms TCP uses: SRTT + 4 x RTTVAR.
 * The worst carrier sets it, because a frame is only lost when the path that
 * carried it is silent. The floor is TCP's 200 ms minimum; the feedback delay
 * (uack lag after the carrier TCP ack) can raise it, and the 500 ms cap makes
 * a dead path noticeable. A fixed 4 x SRTT was wrong on a jittery path: it
 * fired on frames whose ack was still inside the jitter band. */
static u64 tbt_tx_rto_ns(struct tbt_session *sess, struct tbt_stream *st)
{
    u32 rto_us = 0;
    int i;

    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);
        struct sock *sk;
        u32 srtt = 0, mdev = 0, r;

        if (!c)
            continue;
        sk = tbt_carrier_sk_get(c);
        if (sk)
        {
            srtt = READ_ONCE(tcp_sk(sk)->srtt_us) >> 3;
            mdev = READ_ONCE(tcp_sk(sk)->mdev_us);
            tbt_carrier_sk_put(sk);
        }
        if (!srtt)
        {
            srtt = READ_ONCE(c->rtt_us);
            mdev = 0;
        }
        tbt_carrier_put(c);
        r = srtt + 4 * mdev;
        if (r > rto_us)
            rto_us = r;
    }
    if (!rto_us)
        rto_us = TBT_RETX_MIN_US / 4;
    /* Bootstrap: before the first tunnel-level ack the only RTT we can read is
     * the carrier's own, and where only the peer's egress is shaped that
     * sample is ~0.1 ms while the feedback path (peer's egress + FBK cadence)
     * is ~22 ms. An RTO built from it retransmits every frame before its ack
     * arrives; the duplicates then drown the very feedback that would end the
     * bootstrap (measured: probe J, far rto_us=20000, srtt_us=80-238,
     * c_rtt_us=0, a 131 KB stream inflated to 7.7 MB). Hold 200 ms until a
     * tunnel ack lands. */
    if (!READ_ONCE(st->last_ack_ns))
        return 200 * NSEC_PER_MSEC;
    {
        u32 fb = READ_ONCE(sess->fb_delay_us);
        u64 r = rto_us;

        if (fb && (u64)fb * 2 > r)
            r = (u64)fb * 2;
        return clamp_t(u64, r * NSEC_PER_USEC,
                       (u64)TBT_RETX_MIN_US * NSEC_PER_USEC,
                       (u64)TBT_RETX_MAX_US * NSEC_PER_USEC);
    }
}

/* A carrier died: drop its bits from every stream's retained frames and mark
 * the frames it last carried for immediate retransmit. Runs with a stream
 * reference from the pump/FBK snapshot, so it may take tx_lock but must not
 * take sess->lock or call back into the carrier. */
static void tbt_tx_path_lost_stream(struct tbt_session *sess,
                                    struct tbt_stream *st,
                                    const unsigned long *mask)
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
        /* The retention masks are 16 bits wide and only the old ARQ uses
         * them; the parser refuses `fec=off` above 16 carriers, so a lost path
         * index is always inside them. */
        r->path_mask &= ~(u16)(mask[0] & 0xffff);
        r->reserve_mask &= ~(u16)(mask[0] & 0xffff);
        if (r->last_path < 16 && test_bit(r->last_path, mask) && r->sends &&
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
    if (index >= sess->n_car || index >= TBT_MAX_CARRIERS_LIMIT)
        return;
    set_bit(index, sess->path_lost_mask);
    tbt_fbk_kick(sess, true);
}

/* ARQ entry for "a stalled path pauses, it does not repath". It has no caller
 * today: ARQ still takes tbt_carrier_lost() (kill the carrier and repath its
 * frames), and FEC has no struct tbt_retain entries for this function to drop.
 * Keep it for the day ARQ switches to the pause semantics; the FEC container
 * deliberately does not call it.
 *
 * When that switch happens, this drops the frames only the stalled path
 * carried, together with their queued copies, so the loss does not turn into a
 * repath through the back door. It never sets retx_now (that is
 * tbt_tx_path_lost()'s rule) and never advances tx_ack: the peer may still
 * deliver those bytes from another copy, and the receiver's own ack is the only
 * thing allowed to move the window.
 *
 * Must run from the 50 ms carrier poll, never from atomic context. The pump's
 * retransmit candidates and write batch hold struct tbt_retain pointers
 * outside tx_lock, so pump_lock serialises this drop with them; the entries
 * are unlinked under tx_lock and freed after it is dropped. */
void tbt_tx_path_stalled(struct tbt_session *sess, u8 index)
{
    struct tbt_stream *snap[TBT_STREAM_SNAP_MAX];
    struct tbt_retain *r, *rtmp;
    struct tbt_frame *fr, *ftmp;
    LIST_HEAD(free);
    LIST_HEAD(drop);
    u16 bit;
    int n, i;

    if (!sess || index >= READ_ONCE(sess->n_car) ||
        index >= TBT_MAX_CARRIERS_LIMIT)
        return;
    if (READ_ONCE(sess->closing))
        return;

    /* The retention arrays are 16 slots wide; a FEC v6 path has no ARQ
     * retention entries to drop. */
    bit = index < TBT_ARQ_MAX_PATHS ? BIT(index) : 0;

    mutex_lock(&sess->pump_lock);
    n = tbt_session_stream_snapshot(sess, snap, ARRAY_SIZE(snap));
    spin_lock(&sess->tx_lock);
    for (i = 0; i < n; i++)
    {
        struct tbt_stream *st = snap[i];
        u64 ack = READ_ONCE(st->tx_ack);

        list_for_each_entry_safe(r, rtmp, &st->retain, list)
        {
            bool had = bit && (r->path_mask & bit);

            if (bit)
            {
                r->path_mask &= ~bit;
                r->reserve_mask &= ~bit;
            }
            /* Keep a frame that still has another copy, and one the peer has
             * already delivered (the retention GC owns that one). A fresh
             * entry that never reached this path has no bit and is untouched:
             * its frame is still owned by tx_q. */
            if (!had || r->path_mask || r->seq + r->len <= ack)
                continue;

            /* The frame had no copy on another path. Its queued copies must
             * die with the entry: writing them on another path would be the
             * repath this rule forbids. */
            list_for_each_entry_safe(fr, ftmp, &sess->tx_q, list)
            {
                if (fr->ret != r)
                    continue;
                list_del(&fr->list);
                sess->tx_queued -= fr->len;
                tbt_tx_frame_drop_account(sess, fr);
                list_add_tail(&fr->list, &drop);
            }
            list_move_tail(&r->list, &free);
            st->retain_bytes -= r->len;
            sess->stall_drop++;
        }
    }
    spin_unlock(&sess->tx_lock);
    mutex_unlock(&sess->pump_lock);

    list_for_each_entry_safe(r, rtmp, &free, list)
    {
        list_del(&r->list);
        kfree(r);
    }
    list_for_each_entry_safe(fr, ftmp, &drop, list)
    {
        list_del(&fr->list);
        tbt_frame_free(fr);
    }
    for (i = 0; i < n; i++)
        tbt_stream_put(snap[i]);
    tbt_fbk_kick(sess, true);
}

/* Drain the lost-carrier bits once, outside atomic context, for every stream
 * in the caller's snapshot. */
static void tbt_tx_path_lost_drain(struct tbt_session *sess,
                                   struct tbt_stream **snap, int n)
{
    DECLARE_BITMAP(mask, TBT_MAX_CARRIERS_LIMIT);
    unsigned long flags;
    int i;

    /* Snapshot and clear in one critical section: `set_bit` in the loss path is
     * atomic, so a bit that lands between the copy and the clear would be
     * lost, and the frames it protects would wait for the next FBK tick. */
    spin_lock_irqsave(&sess->tx_lock, flags);
    bitmap_copy(mask, sess->path_lost_mask, TBT_MAX_CARRIERS_LIMIT);
    bitmap_zero(sess->path_lost_mask, TBT_MAX_CARRIERS_LIMIT);
    spin_unlock_irqrestore(&sess->tx_lock, flags);
    if (bitmap_empty(mask, TBT_MAX_CARRIERS_LIMIT))
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
    u8 srcs[ARRAY_SIZE(cand)];
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
        u8 src;

        if (r->seq + r->len <= ack)
            continue;
        if (!r->sends)
        {
            atomic64_inc(&sess->dbg_retx_nosends);
            continue;
        }
        /* The frame containing the peer's contiguous delivered offset has
         * explicit receiver evidence: the uack stopped inside it. That
         * outranks the per-path high-water, which counts every record that
         * arrived on the path -- including the ones behind the gap -- and is
         * therefore not proof that this particular record was parsed. It also
         * covers the two dead zones: a copy on a blackholed carrier whose
         * snd_una never passes, and a copy the peer's TCP took but whose
         * parser never saw. The wait is one RTO, not two: the receiver's hole
         * budget is 2-3 x RTO, so a copy at 2 x RTO plus one one-way can
         * arrive after the stream was already reset (measured: the 2.4 Mbit
         * slirp carrier reset at 1.5 s with retx_frames=0). */
        {
            u64 ack_ns = READ_ONCE(st->last_ack_ns);
            bool hole_evident = r->seq <= ack && ack < r->seq + r->len &&
                                (!ack_ns || now - ack_ns >= rto);

            if (!hole_evident && tbt_retain_received(sess, st, r))
                continue;
            if (hole_evident)
            {
                atomic64_inc(&sess->dbg_retx_evident);
                src = TBT_RETX_HOLE;
                if (r->last_sent_ns && now - r->last_sent_ns < rto)
                    continue;
            }
            else if (!r->retx_now)
            {
                u64 done_ns = 0;

                src = TBT_RETX_SLOW;
                /* Slow path: a record TCP delivered but the parser never saw.
                 * Path progress proves nothing there. Bounded to one copy per
                 * 3xRTO so it cannot become a per-RTO flood. Two RTOs sat
                 * inside the measured feedback lag: the peer's delivery queue
                 * was tens of MB, so the uack crossed a TCP-acked frame only
                 * seconds later, and every frame in that band was copied
                 * again every 2xRTO (measured: 310 of 313 copies in one
                 * bench run came from this branch, no loss injected). */
                if (READ_ONCE(st->last_ack_ns) &&
                    tbt_retain_tcp_done(sess, r, now, &done_ns))
                {
                    if (now - done_ns < 3 * rto)
                        continue;
                }
                else if (!tbt_retain_paths_stalled(sess, r, now, rto))
                {
                    continue;
                }
                /* Retransmit copies are inserted at the head, ahead of new
                 * data. Cap them at half the queue so the frames that carry a
                 * fresh uack always have room; a dead path and the hole above
                 * are exempt because they are the only ways forward. */
                if (READ_ONCE(sess->tx_queued) + r->len > sess->txq_max / 2)
                    continue;
            }
            else
            {
                src = TBT_RETX_PATHLOSS;
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
            srcs[n] = src;
            cand[n++] = r;
            atomic64_inc(&sess->dbg_retx_cand);
            sess->last_loss_ns = now;
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
        /* A hole filler enters even when the queue is far past its bound.
         * The capture path keeps the frames the window cannot carry in this
         * queue, so it is deepest exactly when a hole exists; the old bound
         * refused the one frame that advances the ack (measured:
         * retx_evident=256, retx_cand=7, retx_txqdrop=7, tx_queued=1.1 MB,
         * txq_max=200 KB, retx_frames=0, far holes_unrecoverable=1). One
         * 8 KiB copy per RTO is the whole exposure, and it is freed as soon
         * as the pump writes it. Slow and path-loss copies keep the bound:
         * they are not the only way forward. */
        if (srcs[i] != TBT_RETX_HOLE &&
            READ_ONCE(sess->tx_queued) > sess->txq_max)
        {
            sess->dbg_retx_txqdrop++;
            if (debug >= 1)
                pr_info_ratelimited("tbtun: retx txqdrop seq=%llu len=%u src=%u tx_queued=%llu txq_max=%u\n",
                                    (unsigned long long)r->seq,
                                    (unsigned int)r->len,
                                    (unsigned int)srcs[i],
                                    (unsigned long long)READ_ONCE(sess->tx_queued),
                                    (unsigned int)sess->txq_max);
            r->pending = false;
            continue;
        }
        fr = tbt_frame_alloc(r->len, GFP_KERNEL);
        if (!fr)
        {
            for (; i < n; i++)
                cand[i]->pending = false;
            atomic64_inc(&sess->dbg_alloc_fail);
            break;
        }
        fr->hdr = r->hdr;
        fr->hdr.body_len = r->len;
        fr->hdr.seq = r->seq;
        fr->avoid_mask = r->path_mask;
        fr->ret = r;
        fr->st = st;
        fr->retx_src = srcs[i];
        refcount_inc(&st->refs);
        memcpy(fr->data, r->data, r->len);
        /* A retransmit copy always goes to the head. The old "over half the
         * queue -> tail" rule assumed the copies compete with new frames for
         * a small queue; with a large txq and a small carrier window it put
         * the only frame that can advance the ack behind megabytes of new
         * data, so it was never written before the receiver's budget expired
         * (measured: retx_evident=255, retx_cand=6, retx_frames=0, far
         * holes_unrecoverable=1). */
        /* Before the frame is visible to the pump, which reads retx_copy to
         * pick the stalled-path mask and to account the copy. Today the pump
         * is the only consumer and this runs inside it under pump_lock, so
         * the old store-after-enqueue window was not reachable; the frame was
         * still published with its state incomplete, and a second consumer
         * would have read it as new data. */
        fr->retx_copy = true;
        tbt_tx_enqueue(sess, fr, true);
        /* Counted when the carrier accepts the write, not here: a copy the
         * pump drops because the ack passed never reaches the wire, and
         * counting it made retx_ppm read 25% while wire/app was 1.01. The
         * enqueue is kept as a separate diagnostic. */
        atomic64_inc(&sess->retx_queued);
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
    for (i = 0; i < sess->n_car && i < TBT_ARQ_MAX_PATHS; i++)
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
    for (i = 0; i < sess->n_car && i < TBT_ARQ_MAX_PATHS; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);
        struct sock *sk;

        if (!c)
            continue;
        if (!(r->path_mask & BIT(i)) || (c->gen & 0xf) != r->path_gen[i] ||
            !tbt_retain_path_ok(sess, i))
        {
            tbt_carrier_put(c);
            continue;
        }
        sk = tbt_carrier_sk_get(c);
        if (!sk)
        {
            tbt_carrier_put(c);
            return false;
        }
        if ((s32)(READ_ONCE(tcp_sk(sk)->snd_una) - r->first_sent_seq[i]) < 0)
        {
            tbt_carrier_sk_put(sk);
            tbt_carrier_put(c);
            return false;
        }
        tbt_carrier_sk_put(sk);
        if (!r->una_observed_ns[i])
            r->una_observed_ns[i] = now;
        any = true;
        if (!*earliest || r->una_observed_ns[i] < *earliest)
            *earliest = r->una_observed_ns[i];
        tbt_carrier_put(c);
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

    for (i = 0; i < sess->n_car && i < TBT_ARQ_MAX_PATHS; i++)
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
    for (i = 0; i < sess->n_car && i < TBT_ARQ_MAX_PATHS; i++)
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
        if (sess->tx_queued + r->len > sess->txq_max)
        {
            sess->inject_skip++;
            break;
        }
        fr = tbt_frame_alloc(r->len, GFP_ATOMIC);

        if (!fr)
        {
            atomic64_inc(&sess->dbg_alloc_fail);
            break;
        }
        fr->hdr = r->hdr;
        fr->hdr.body_len = r->len;
        fr->hdr.seq = r->seq;
        fr->avoid_mask = r->path_mask;
        fr->ret = r;
        fr->st = st;
        fr->retx_src = TBT_RETX_HOLE; /* a declared-hole filler */
        refcount_inc(&st->refs);
        memcpy(fr->data, r->data, r->len);
        list_add(&fr->list, &sess->tx_q);
        sess->tx_queued += r->len;
        atomic64_inc(&sess->retx_queued);
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
    now = tbt_now_ns();
    spin_lock(&sess->tx_lock);
    /* tx_ack moves under sess->lock, not tx_lock: reading it before the lock
     * lets the hole walk use a stale value while tbt_retain_received() reads
     * a newer one, which logged a depth failure for a hole that was already
     * repaired. Take the snapshot inside the lock. */
    ack = READ_ONCE(st->tx_ack);
    for (i = 0; i < sess->n_car && i < TBT_ARQ_MAX_PATHS; i++)
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
    atomic64_inc(&sess->hole_checks);
    /* Gate: a path the hole was sent on must show a received-count lag, and
     * every other live path must have moved past the hole's seq. The lossy
     * path's own uack is not required to be fresh: while it is dropping, no
     * fresh uack from it can exist. Its recv_cnt is the receiver-published
     * evidence, and the lag is "records we wrote below its high-water that the
     * peer's count does not account for". */
    {
        bool lost = false;

        for (i = 0; i < sess->n_car && i < TBT_ARQ_MAX_PATHS; i++)
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
    /* The ack snapshot above can still be passed by a concurrent
     * tbt_tx_ack() (it takes sess->lock, not tx_lock). If the hole has been
     * repaired while this walk ran, do not log a stale gate/depth/time
     * failure and do not inject another copy the peer already has. */
    if (hole->seq + hole->len <= READ_ONCE(st->tx_ack))
        goto unlock;
    if (!gate)
    {
        atomic64_inc(&sess->hole_gate_fail);
        if (debug >= 2)
            pr_info_ratelimited("tbtun: hole gatefail seq=%llu mask=%#x ack=%llu\n",
                                hole->seq, hole->path_mask, ack);
    }
    else if (tbt_late_tab[g] >= tbt_late_tab[TBT_K_MAX_HOLE])
    {
        atomic64_inc(&sess->hole_depth_fail);
        if (debug >= 2)
            pr_info_ratelimited("tbtun: hole depthfail seq=%llu g=%d mask=%#x\n",
                                hole->seq, g, hole->path_mask);
    }
    else if (!hole->first_sent_ns ||
             now - hole->first_sent_ns <
                 (u64)(rtt > 500 ? rtt : 500) * 2 * NSEC_PER_USEC)
    {
        atomic64_inc(&sess->hole_time_fail);
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

            for (i = 0; i < sess->n_car && i < TBT_ARQ_MAX_PATHS; i++)
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

/* How long a stream may wait on the peer before it is reset. One rule serves
 * the sender stall timer and the close FIN: the configured budget is a floor
 * for a low-RTT path; it must never fire before this session's own repair can
 * arrive. One RTO finds the loss, and one more RTO plus a one-way covers the
 * retransmit and its feedback, so the wait is at least 2.5 RTOs. This is the
 * same 2-3 x RTO rule the receiver's hole budget follows, applied where the
 * RTO is actually known. */
u64 tbt_tx_wait_budget_ns(struct tbt_session *sess, struct tbt_stream *st)
{
    u64 rto = tbt_tx_rto_ns(sess, st);
    u64 budget;

    if (sess->tunnel->cfg.hole_budget_ms)
        budget = (u64)sess->tunnel->cfg.hole_budget_ms * NSEC_PER_MSEC;
    else
    {
        /* Auto must not fall back to one second. The receiver's own auto
         * hole budget is max(2 s, 5*mean_srtt), and the sender has to add
         * its RTO plus one one-way on top; one second is shorter than
         * that, and it killed a 172-byte control stream whose last byte
         * waited behind a bulk window (measured: TXSTALLKILL ack=171
         * txseq=172 age=3.17 s while carrier_stall_ms=15000 was
         * ignored). The operator's dead-path bound wins when set; 5 s or
         * 5 RTOs otherwise. An explicit hole_budget_ms keeps its exact
         * meaning (probe H asserts 500 ms). */
        budget = max_t(u64, 5 * NSEC_PER_SEC, 5 * rto);
        if (sess->tunnel->cfg.carrier_stall_ms)
            budget = max_t(u64, budget,
                           (u64)sess->tunnel->cfg.carrier_stall_ms *
                               NSEC_PER_MSEC);
    }
    if (budget < 5 * rto / 2)
        budget = 5 * rto / 2;
    return budget;
}

/* Sender-side bounded reset. It must fire on *no progress*, not on the age of
 * the oldest frame: one dead path leaves an old frame at the ack while the
 * other paths keep repairing holes, and resetting there would turn a
 * recoverable two-second outage into a failure (measured in probe H). The
 * timer therefore runs from the last time tx_ack advanced. Total loss leaves
 * it untouched, which is the case the reset exists for. */
/* True while the FEC packetizer is stopped by the peer's credit window: the
 * receiver has not delivered enough of its buffer for the sender to emit the
 * next packet. That is flow control, not a TX-path stall; only the receiver's
 * bounded reset or the next ACK can move it. Mirrors the FEC-owned
 * tbt_fec_tx_credit_ok() without touching that file. */
static bool tbt_fec_credit_waiting(const struct tbt_session *sess)
{
    const struct tbt_fec_tx *tx = READ_ONCE(sess->fec_tx);
    u64 acked, seq;
    u32 credit;

    if (!tx)
        return false;
    acked = READ_ONCE(tx->peer_pkts);
    seq = READ_ONCE(tx->seq);
    credit = READ_ONCE(tx->credit);
    /* Zero credit is a closed window: no grant has arrived yet. */
    if (!credit)
        return true;
    /* An ACK cannot name packets that were not sent; clamp instead of letting
     * the unsigned subtraction wrap into a huge outstanding count. */
    if (acked >= seq)
        return false;
    return seq - acked >= (u64)credit;
}

/* A FEC repair is still in progress: replay records are queued, or the FEC
 * data plane touched the repair clock inside the grace window. The grace is
 * two wait budgets (clamped to 5 s..30 s), so a NACK/replay burst is covered
 * without hiding a dead ACK path forever. */
#define TBT_FEC_REPAIR_MAX_NS (30ULL * NSEC_PER_SEC)

static bool tbt_fec_repair_active(struct tbt_session *sess, u64 now,
                                  u64 wait_budget_ns)
{
    u64 grace = clamp_t(u64, 2 * wait_budget_ns,
                        5ULL * NSEC_PER_SEC, TBT_FEC_REPAIR_MAX_NS);
    u64 last = READ_ONCE(sess->fec_repair_last_ns);

    if (READ_ONCE(sess->fec_replay_queued))
        return true;
    return last && now > last && now - last < grace;
}

/* The FEC packetizer's source ring has data waiting; only then is waking the
 * public-socket capture useful. `tx_queued` is the ARQ queue and stays near
 * zero in FEC mode, so it cannot be the wake test. */
static bool tbt_fec_ring_busy(struct tbt_session *sess)
{
    struct tbt_fec_tx *tx = READ_ONCE(sess->fec_tx);

    if (!tx)
        return false;
    return READ_ONCE(tx->head) != READ_ONCE(tx->tail);
}

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
    if (!start && sess->fec)
    {
        /* FEC data never enters retain: the packetizer keeps it in its own
         * ring, so the loop above finds nothing and this reset could never
         * fire. The FEC ACK is the only progress signal; time it the same
         * way. */
        start = st->last_ack_ns ? st->last_ack_ns : st->tx_start_ns;
        if (st->pause_cleared_ns > start)
            start = st->pause_cleared_ns;
        /* A closed credit window is peer flow control, not a lost ACK: the
         * packetizer is waiting for the receiver to drain. Do not race the
         * receiver's own bounded reset. When the window opens again, the ACK
         * that opened it also advances last_ack_ns, so a genuinely dead ACK
         * path still reaches the budget below. */
        if (tbt_fec_credit_waiting(sess))
            start = now;
    }
    {
        u64 budget = tbt_tx_wait_budget_ns(sess, st);
        bool would_kill;

        /* A timestamp stamped on another CPU can be a few nanoseconds ahead
         * of this CPU's ktime_get_ns(); the unsigned subtraction then looks
         * like a 584-year stall and kills a healthy stream (measured: probe G,
         * one run in three, age_us=18446744073709519). */
        would_kill = start && now > start && ack < READ_ONCE(st->tx_seq) &&
                     now - start > budget;
        /* FEC repair is progress even when the tunnel ack has not moved: the
         * receiver is asking for and receiving replay shares. Hold the kill,
         * but only inside the hard no-progress cap; a repair that never
         * completes must still end the stream. */
        if (sess->fec && would_kill &&
            now - start <= TBT_FEC_REPAIR_MAX_NS &&
            tbt_fec_repair_active(sess, now, budget))
        {
            atomic64_inc(&sess->tx_stall_suppressed_fec);
            start = now;
            would_kill = false;
        }
        kill = would_kill;
    }
    if (kill)
    {
        if (debug >= 2)
        {
            struct tbt_fec_tx *tx =
                sess->fec ? READ_ONCE(sess->fec_tx) : NULL;
            u16 g = tx ? READ_ONCE(tx->last_group) : 0;

            if (tx && tx->g && g < READ_ONCE(tx->groups))
                pr_info("tbtun: TXSTALLKILL sid=%llu ack=%llu txseq=%llu age_us=%llu "
                        "fec_block=%llu fec_group=%u fec_share=%u fec_nreal=%u "
                        "fec_last_len=%u fec_seq=%llu fec_peer_pkts=%llu\n",
                        (unsigned long long)st->sid, ack,
                        READ_ONCE(st->tx_seq),
                        (now - start) / NSEC_PER_USEC,
                        (unsigned long long)READ_ONCE(tx->cur_block), g,
                        (unsigned int)READ_ONCE(tx->last_share),
                        (unsigned int)READ_ONCE(tx->g[g].nreal),
                        (unsigned int)READ_ONCE(tx->g[g].last_len),
                        (unsigned long long)READ_ONCE(tx->seq),
                        (unsigned long long)READ_ONCE(tx->peer_pkts));
            else
                pr_info("tbtun: TXSTALLKILL sid=%llu ack=%llu txseq=%llu age_us=%llu\n",
                        (unsigned long long)st->sid, ack,
                        READ_ONCE(st->tx_seq),
                        (now - start) / NSEC_PER_USEC);
        }
        atomic64_inc(&sess->holes_unrecoverable);
        if (sess->fec)
            atomic64_inc(&sess->fec_reset_hole);
        tbt_stream_kill(st, TBT_ST_HOLE);
    }
}

/* Bounded close. The FIN is queued only after the capture work has submitted
 * every byte the producer sent (tbt_pub_capture_work), and the pump holds it
 * until tx_ack covers tx_seq. Both waits must end: a producer that closed
 * while the path died cannot pin the stream and its socket forever. The timer
 * runs from the later of the FIN request and the last ack advance, so a slow
 * but progressing drain is never cut at the budget; only no progress is. Once
 * all data is acked the only remaining wait is the FIN copy itself, and that
 * is bounded from the request. */
static void tbt_tx_close_check(struct tbt_session *sess, struct tbt_stream *st)
{
    u64 since = READ_ONCE(st->close_since_ns);
    u64 start, budget, now;

    if (!since)
        return;
    if (READ_ONCE(st->tx_fin_written))
    {
        /* Our half is on the wire and every byte before it was acked.  If the
         * peer never closes its half and there is nothing left to deliver to
         * the application, the stream must still not pin a session slot and a
         * socket forever.  Detach, do not reset: the peer may yet send data,
         * it is dropped, and the peer's own stall timer resets it. */
        u64 fw = READ_ONCE(st->close_fin_written_ns);
        u64 bound = max_t(u64, 60 * NSEC_PER_SEC,
                          2 * tbt_tx_wait_budget_ns(sess, st));

        now = tbt_now_ns();
        if (!fw || READ_ONCE(st->rx_fin_done) || READ_ONCE(st->rx_queued))
            return;
        if (now <= fw || now - fw <= bound)
            return;
        atomic64_inc(&sess->close_detaches);
        tbt_stream_detach_from_session(st);
        return;
    }
    start = since;
    if (READ_ONCE(st->tx_ack) < READ_ONCE(st->tx_seq))
    {
        u64 ack_ns = READ_ONCE(st->last_ack_ns);

        if (ack_ns > start)
            start = ack_ns;
    }
    if (READ_ONCE(st->pause_cleared_ns) > start)
        start = READ_ONCE(st->pause_cleared_ns);
    budget = tbt_tx_wait_budget_ns(sess, st);
    now = tbt_now_ns();
    if (now <= start || now - start <= budget)
        return;
    /* FEC tail repair: the FIN gate waits for tx_ack == tx_seq, and the last
     * bytes may still be moving through NACK/replay or a closed credit
     * window. Killing here sends RST and truncates the response at the
     * target (measured: fix20 1 GiB, far close_timeouts=1 with 3.98 MB left
     * while repair was active). Let the repair path finish; the receiver's
     * own bounded reset is the authority for giving up. A credit window that
     * stays closed with no repair traffic is still bounded: after the same
     * 30 s hard cap the close timer fires. */
    if (sess->fec &&
        tbt_tx_close_hold_fec(true,
                              tbt_fec_repair_active(sess, now, budget),
                              tbt_fec_credit_waiting(sess),
                              now - start, TBT_FEC_REPAIR_MAX_NS))
    {
        atomic64_inc(&sess->close_hold_fec);
        if (debug >= 2)
            pr_info_ratelimited("tbtun: CLOSEWAITFEC sid=%llu ack=%llu txseq=%llu age_us=%llu\n",
                                (unsigned long long)st->sid,
                                READ_ONCE(st->tx_ack),
                                READ_ONCE(st->tx_seq),
                                (now - start) / NSEC_PER_USEC);
        return;
    }
    if (debug >= 2)
        pr_info("tbtun: CLOSETIMEOUT sid=%llu ack=%llu txseq=%llu fin=%llu age_us=%llu\n",
                (unsigned long long)st->sid, READ_ONCE(st->tx_ack),
                READ_ONCE(st->tx_seq), READ_ONCE(st->tx_fin_seq),
                (now - start) / NSEC_PER_USEC);
    atomic64_inc(&sess->close_timeouts);
    tbt_stream_kill(st, TBT_ST_DEAD);
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

                if (c && tbt_retain_path_ok(sess, c->index) &&
                    (r->path_mask & BIT(c->index)) &&
                    (c->gen & 0xf) == r->path_gen[c->index])
                    i = c->index;
                else if (r->last_path < TBT_ARQ_MAX_PATHS &&
                         r->last_path < sess->n_car)
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

/* Paths that have bytes TCP has not acked and have not moved snd_una for an
 * RTO. A retransmit copy must not be scheduled there: a blackholed path stops
 * being written, so its payload_bytes_sent - tx_ack falls to zero and the
 * lowest-inflight scheduler picks it exactly when it can deliver nothing.
 * An idle path (snd_una == snd_nxt) is not stalled: a copy sent there only
 * waits for the next new frame to move the clock. Measured: probe K, the hole
 * copy landed on the blackholed carrier on every timer tick and the sender's
 * 500 ms budget reset the stream. */
static u16 tbt_path_stalled_mask(struct tbt_session *sess)
{
    u64 now = tbt_now_ns();
    u64 rto = (u64)READ_ONCE(sess->rto_us) * NSEC_PER_USEC;
    u16 mask = 0;
    int i;

    if (!rto)
        return 0;
    for (i = 0; i < sess->n_car && i < TBT_ARQ_MAX_PATHS; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);
        struct sock *sk;

        if (!c)
            continue;
        if (!c->last_una_move_ns || now - c->last_una_move_ns < rto)
        {
            tbt_carrier_put(c);
            continue;
        }
        sk = tbt_carrier_sk_get(c);
        if (!sk)
        {
            tbt_carrier_put(c);
            continue;
        }
        if (READ_ONCE(tcp_sk(sk)->snd_una) != READ_ONCE(tcp_sk(sk)->snd_nxt))
            mask |= BIT(i);
        tbt_carrier_sk_put(sk);
        tbt_carrier_put(c);
    }
    return mask;
}

/* Carriers the poll has paused. New frames must not enter them: the path is
 * alive, but anything written there only waits for a recovery that may not
 * come before the receiver's hole budget. This is the scheduler-side mask;
 * tbt_path_stalled_mask() above stays the retransmit-side clock test, which
 * deliberately keeps its own (snd_una based) meaning. */
static void tbt_path_stall_now_mask(struct tbt_session *sess,
                                    unsigned long *mask)
{
    int i;

    /* A FEC session has up to 256 carriers; the scheduler mask must cover
     * every slot, not just the 16 legacy retain slots. */
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = READ_ONCE(sess->car[i]);

        if (c && READ_ONCE(c->stalled))
            set_bit(i, mask);
    }
}

/* Fold the legacy 16-bit avoid/stalled masks into the 256-bit scheduler
 * mask. Those two are properties of a retained DATA copy, and only the
 * legacy ARQ creates one, so their width is correct. */
static void tbt_blocked_add_u16(unsigned long *mask, u16 bits)
{
    int i;

    for (i = 0; i < TBT_ARQ_MAX_PATHS; i++)
        if (bits & BIT(i))
            set_bit(i, mask);
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
        /* A stalled carrier is paused, not dead: keep it out of the stripe
         * until the poll sees snd_una move again. */
        if (READ_ONCE(c->stalled))
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
                                          struct tbt_stream *st,
                                          const unsigned long *blocked,
                                          u16 avoid, u16 stalled,
                                          bool need_open)
{
    struct tbt_sched *s = st ? &st->sched : &sess->ctrl_sched;
    DECLARE_BITMAP(mask, TBT_MAX_CARRIERS_LIMIT);
    u64 now = tbt_now_ns();
    int tries;

    memcpy(mask, blocked, sizeof(mask));
    tbt_blocked_add_u16(mask, avoid);
    tbt_blocked_add_u16(mask, stalled);
    /* The order may predate the stall (it is rebuilt at most every 10 ms);
     * skip the paused slots even then. */
    tbt_path_stall_now_mask(sess, mask);
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
        if (test_bit(idx, mask))
            continue;
        return READ_ONCE(sess->car[idx]);
    }
    return NULL;
}

/* No schedulable carrier is not user backpressure yet: the replacement worker
 * may be about to install one. Ask it now, and let the capture side wait this
 * long before it is told to stop reading. A dial that fails or a peer that
 * never answers simply outlives the window; that is the point where the queue
 * stops being a short wait and becomes real backpressure. */
#define TBT_TX_WAIT_CARRIER_MS 500

static void tbt_tx_request_carrier(struct tbt_session *sess)
{
    if (READ_ONCE(sess->closing))
        return;
    if (!sess->tx_wait_since_ns)
        sess->tx_wait_since_ns = tbt_now_ns();
    /* Coalesced by tbt_session_arm(); 20 ms is one dial retry, not a poll
     * storm. The replace work supplies empty slots and replaces stopping
     * carriers; a stalled carrier is already out of the stripe. */
    tbt_session_arm(sess, TBT_SW_REPLACE, msecs_to_jiffies(20));
}

/* Called from capture when the queue refused a chunk. If the refusal is a
 * carrier wait, the caller must not stop reading yet; once the wait outlives
 * the window (dial failed, max_carriers reached, peer unreachable), or the
 * queue is full for another reason, the caller enters user backpressure. */
static bool tbt_tx_should_backpressure(struct tbt_session *sess)
{
    u64 since = READ_ONCE(sess->tx_wait_since_ns);

    if (READ_ONCE(sess->closing))
        return true;
    if (!READ_ONCE(sess->n_car))
        return true;
    if (!since)
        return true;
    return tbt_now_ns() - since >=
           (u64)TBT_TX_WAIT_CARRIER_MS * NSEC_PER_MSEC;
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
        mod_delayed_work(TBT_WQ, &c->w_pump, msecs_to_jiffies(ms));
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

    /* A FIN's deadline is the close timer, not this per-frame bound: a
     * replay/data burst may reject the same copy many times while the tail
     * is repaired. Other pinned frames keep the bound. */
    if (fr->hdr.type != TBT_F_FIN &&
        fr->retries >= TBT_CTL_MAX_RETRIES)
        return false;
    fr->retries++;
    sess->ctl_retry++;
    sess->ctl_requeue++;
    fr->retry_at_ns = tbt_now_ns() + (u64)ms * NSEC_PER_MSEC;
    c = tbt_frame_carrier(sess, fr);
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

    atomic64_inc(&sess->ctl_lost);
    if (type == TBT_F_HELLO)
        pr_warn_ratelimited("tbtun: tunnel %llu: HELLO abandoned: carrier=%u gen=%u retries=%u (retry bound reached)\n",
                            (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
                            fr->path, fr->gen, fr->retries);
    if (fr->st)
    {
        if (type == TBT_F_FIN)
        {
            struct tbt_stream *st = fr->st;
            u64 since = READ_ONCE(st->close_since_ns);
            u64 budget = tbt_tx_wait_budget_ns(sess, st);

            if (fr->path < TBT_MAX_CARRIERS_LIMIT)
            {
                clear_bit(fr->path, st->tx_fin_inflight);
                /* Let tbt_tx_fin() queue a fresh copy on this or another
                 * hello_sent carrier; the mask would otherwise suppress it. */
                clear_bit(fr->path, st->tx_fin_mask);
            }
            if (!READ_ONCE(st->tx_fin_written) &&
                bitmap_empty(st->tx_fin_inflight, TBT_MAX_CARRIERS_LIMIT))
            {
                WRITE_ONCE(st->tx_fin_queued, false);
                if (tbt_tx_fin_retry_allowed(tbt_now_ns(), since, budget))
                {
                    /* The FIN was starved, not lost: requeue it under the
                     * close timer and count the starvation. */
                    atomic64_inc(&sess->fin_starved);
                    tbt_tx_fin(sess, st, GFP_ATOMIC);
                }
                else
                {
                    tbt_stream_kill(st, TBT_ST_DEAD);
                }
            }
        }
        else if (type == TBT_F_RST)
        {
            if (fr->path < TBT_MAX_CARRIERS_LIMIT)
                clear_bit(fr->path, fr->st->tx_rst_inflight);
            if (!READ_ONCE(fr->st->tx_rst_written) &&
                bitmap_empty(fr->st->tx_rst_inflight, TBT_MAX_CARRIERS_LIMIT))
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
            atomic64_inc(&sess->fin_written);
            if (!READ_ONCE(fr->st->close_fin_written_ns))
                WRITE_ONCE(fr->st->close_fin_written_ns, tbt_now_ns());
            if (fr->path < TBT_MAX_CARRIERS_LIMIT)
                clear_bit(fr->path, fr->st->tx_fin_inflight);
            /* Both halves may already be done: the peer's FIN can arrive
             * before ours reaches the wire.  This is the moment the stream
             * may leave the session. */
            tbt_stream_maybe_detach(fr->st);
        }
        else if (type == TBT_F_RST)
        {
            WRITE_ONCE(fr->st->tx_rst_written, true);
            if (fr->path < TBT_MAX_CARRIERS_LIMIT)
                clear_bit(fr->path, fr->st->tx_rst_inflight);
        }
    }
    tbt_tx_ctl_put(sess, fr);
}

/* A RETIRE names its victim (index, generation) in the body; the carrier that
 * sends the notice is incidental. After that sender is replaced, move the
 * notice to another live carrier instead of dropping it. */
static int tbt_tx_retire_alt(struct tbt_session *sess, u16 path)
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
    bool wait_carrier = false;
    bool close_priority = false;

    might_sleep();
    atomic64_inc(&sess->dbg_pump_runs);
    if (!sess->tunnel)
    {
        atomic64_inc(&sess->dbg_pump_nocar);
        return 0;
    }
    mutex_lock(&sess->pump_lock);
    if (!sess->tunnel)
    {
        mutex_unlock(&sess->pump_lock);
        return 0;
    }
    if (debug >= 1 && sess->n_car > 0xff && !sess->dbg_path_selftest_done)
    {
        struct tbt_frame probe = { .path = TBT_PATH_NONE };

        /* At 256 slots 0xff names a real carrier. The sentinel must resolve
         * to none, while 255 must still resolve to its own slot and the
         * first out-of-range index to none: this is the exact bound every
         * pinned FIN/RST copy relies on. Probe N treats any new warning as a
         * failure, so a regression is visible instead of silently misrouting
         * a frame to carrier 255. */
        sess->dbg_path_selftest_done = true;
        WARN_ON_ONCE(tbt_frame_carrier(sess, &probe) != NULL);
        probe.path = 0xff;
        WARN_ON_ONCE(tbt_frame_carrier(sess, &probe) !=
                     READ_ONCE(sess->car[0xff]));
        probe.path = sess->n_car;
        WARN_ON_ONCE(tbt_frame_carrier(sess, &probe) != NULL);
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
    tbt_tx_window_update(sess);
    nsnap = tbt_session_stream_snapshot(sess, snap, ARRAY_SIZE(snap));
    for (i = 0; i < nsnap; i++)
    {
        if (READ_ONCE(snap[i]->close_since_ns) ||
            READ_ONCE(snap[i]->tx_fin_queued))
        {
            close_priority = true;
            break;
        }
    }
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
        /* 256 bits: c->index ranges over TBT_MAX_CARRIERS_LIMIT, and a u16
         * shift at index >= 64 is undefined (UBSAN) while index >= 16 was
         * silently truncated. */
        DECLARE_BITMAP(blocked, TBT_MAX_CARRIERS_LIMIT);
        bool progress = false;
        u32 close_data = 0, close_ctl = 0;

        memset(blocked, 0, sizeof(blocked));
        spin_lock(&sess->tx_lock);
        if (close_priority)
        {
            /* A closing stream's FIN is the last record the peer needs. Move
             * every queued FIN ahead of the FIFO so replay/data copies cannot
             * starve it; the per-pass quotas below bound the rest. */
            list_for_each_entry_safe(fr, tmp, &sess->tx_q, list)
                if (fr->hdr.type == TBT_F_FIN)
                    list_move(&fr->list, &sess->tx_q);
        }
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

            if (!tbt_tx_close_frame_allowed(close_priority, type,
                                            &close_data, &close_ctl))
                continue;
            if (fr->pinned)
            {
                bool stream_ctl = (type == TBT_F_FIN || type == TBT_F_RST);
                u64 now = tbt_now_ns();

                if (fr->path >= sess->n_car)
                {
                    sess->dbg_pinned_bad_path++;
                    goto drop;
                }
                c = tbt_frame_carrier(sess, fr);
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
                 * must never be written on the replacement, with two
                 * exceptions: FIN/RST are stream-level and re-pin to the new
                 * generation, and RETIRE names its victim in the body, so its
                 * sender may move to another live carrier. DATA never reaches
                 * this branch: every DATA copy (capture, duplicate and
                 * retransmit alike) is queued unpinned, and the scheduler
                 * below picks its path. */
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
                            fr->path = (u16)alt;
                            fr->gen = READ_ONCE(sess->car[alt])->gen;
                            fr->retry_at_ns = 0;
                            sess->ctl_requeue++;
                            continue;
                        }
                    }
                    if (type == TBT_F_HELLO)
                        TBT_DIAG("HELLO dropped tunnel=%llu carrier=%u frame_gen=%u cur_gen=%u: generation changed",
                                 (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
                                 fr->path, fr->gen, c->gen);
                    goto drop;
                }
                if (type == TBT_F_HELLO)
                {
                    if (READ_ONCE(c->hello_sent))
                    {
                        TBT_DIAG("HELLO dropped tunnel=%llu carrier=%u gen=%u: hello already sent",
                                 (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
                                 c->index, c->gen);
                        goto drop;
                    }
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
                        fr->path = (u16)alt;
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
                 * re-runs the pump and the close deadline bounds the wait.  A
                 * killed stream has no ack left to wait for: let its FIN go
                 * rather than pin a frame to a dead stream.  A graceful stream
                 * that merely left the session still waits: ACTIVE is cleared
                 * only after its FIN was written (tbt_stream_maybe_detach). */
                if (type == TBT_F_FIN && fr->st &&
                    READ_ONCE(fr->st->state) != TBT_ST_DEAD &&
                    READ_ONCE(fr->st->tx_ack) != READ_ONCE(fr->st->tx_seq))
                    continue;
            }
            else
            {
                u16 avoid = fr->avoid_mask;
                u16 stalled = fr->retx_copy ? tbt_path_stalled_mask(sess) : 0;
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
                c = tbt_sched_pick(sess, sst, blocked, avoid, stalled,
                                   type == TBT_F_DATA);
                if (!c && avoid && fr->dup_copy)
                {
                    /* No path free of the primary: a duplicate on the same
                     * path is worse than none. The harness asserts
                     * dup_same_path == 0; suppress instead of falling back. */
                    atomic64_inc(&sess->dup_suppressed);
                    goto drop;
                }
                if (!c && avoid)
                    c = tbt_sched_pick(sess, sst, blocked, 0, stalled,
                                       type == TBT_F_DATA);
                /* A stalled path is one whose snd_una has not moved for an
                 * RTO with bytes still unacked: the copy is better sent
                 * elsewhere. When every remaining path is stalled -- and with
                 * N=1 that is always the case -- refusing to schedule means
                 * the hole can never fill, and the peer's hole budget resets
                 * the stream with the bytes still in our queue (measured:
                 * retx_evident=255, retx_cand=6, retx_frames=0). A copy on a
                 * stalled path is no worse than no copy: the path may be
                 * slow, but the record is what the receiver is waiting for. */
                if (!c && stalled)
                    c = tbt_sched_pick(sess, sst, blocked, avoid, 0,
                                       type == TBT_F_DATA);
                if (!c && stalled)
                    c = tbt_sched_pick(sess, sst, blocked, 0, 0,
                                       type == TBT_F_DATA);
                if (!c)
                {
                    if (fr->retx_copy)
                    {
                        sess->dbg_retx_nopick++;
                        if (stalled)
                            sess->dbg_retx_stalled++;
                    }
                    /* No usable carrier is a wait, not backpressure: ask the
                     * replacement worker for one and let capture keep the
                     * bytes in the public socket until the wait expires. */
                    tbt_tx_request_carrier(sess);
                    wait_carrier = true;
                    continue;
                }
                if (!atomic_read(&c->up))
                {
                    if (fr->retx_copy)
                        sess->dbg_retx_notup++;
                    continue;
                }
                if (type == TBT_F_DATA &&
                    (!fr->st || !tbt_stream_announced(fr->st, c)))
                {
                    if (fr->retx_copy)
                        sess->dbg_retx_notann++;
                    continue;
                }
            }
            /* Keep at most one peer window in the carrier's TCP send buffer.
             * Filling sndbuf past the advertised window only parks the tail in
             * zero-window persist, where recovery is a slow timer rather than
             * the next ACK (measured: near.rst_eagain, the last ~282 bytes did
             * not move for 60 s while the far's window was open).  DATA stays
             * in the module queue; control frames bypass this gate, so a
             * pinned RST can still leave after the in-window bytes.  snd_wnd
             * is a heuristic read, and 0 means "one chunk" rather than
             * flooding an unknown window.  The queued cursor is write_seq,
             * not snd_nxt: snd_nxt counts bytes already sent, so a sndbuf
             * filled past the window would still show a small in-flight
             * value and the gate would never fire. */
            /* A retransmit copy is exempt from the peer-window gate: it is
             * the only frame that can fill the hole, and queueing it in the
             * carrier's TCP send buffer is exactly what makes the peer's next
             * window update send it. Gating it behind the 64 KiB window left
             * the hole unfilled while the receiver's budget expired
             * (measured: far holes_unrecoverable=1, near retx_hole=0). */
            if (type == TBT_F_DATA && !fr->retx_copy)
            {
                struct sock *sk = tbt_carrier_sk_get(c);

                if (sk)
                {
                    struct tcp_sock *tp = tcp_sk(sk);
                    u32 una = READ_ONCE(tp->snd_una);
                    u32 queued = READ_ONCE(tp->write_seq);
                    u32 wnd = READ_ONCE(tp->snd_wnd);
                    u32 limit = wnd ? wnd : TBT_CHUNK_MAX;

                    /* Per-stream order is kept: the queue is FIFO and the
                     * small stream's earlier frames are already ahead, so
                     * exempting it here cannot reorder its bytes. */
                    if ((u32)(queued - una) >= limit &&
                        !tbt_stream_small_backlog(fr->st))
                    {
                        c->win_paused = true;
                        sess->win_block++;
                        if (sk->sk_socket)
                            set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
                        /* No timer here. SOCK_NOSPACE makes tcp_check_space()
                         * call sk_write_space when the peer's window opens,
                         * and the 10 ms FBK tick pumps anyway. Arming the
                         * 2 ms fallback once per blocked frame only to have
                         * the pump's own disarm cancel it was 3-44k
                         * schedule/cancel pairs per second (measured:
                         * pump_hw_armed 84380 in a 1.9 s run, fired 0). */
                        sess->dbg_pump_hiwat++;
                        tbt_carrier_sk_put(sk);
                        continue;
                    }
                    if (c->win_paused)
                    {
                        c->win_paused = false;
                        sess->win_resume++;
                    }
                    tbt_carrier_sk_put(sk);
                }
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
            if (type != TBT_F_FBK &&
                !(type == TBT_F_DATA && tbt_stream_small_backlog(fr->st)))
            {
                struct sock *sk = tbt_carrier_sk_get(c);

                if (sk && !sk_stream_memory_free(sk))
                {
                    set_bit(c->index, blocked);
                    if (sk->sk_socket)
                        set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
                    schedule_delayed_work(&c->w_pump, msecs_to_jiffies(2));
                    sess->pump_hw_armed++;
                    sess->dbg_pump_hiwat++;
                    tbt_carrier_sk_put(sk);
                    continue;
                }
                tbt_carrier_sk_put(sk);
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

                if (c->index < 16)
                {
                    if (fr->dup_copy &&
                        ((r->path_mask | r->reserve_mask) & BIT(c->index)))
                        sess->dup_same_path++;
                    r->reserve_mask |= BIT(c->index);
                }
            }
            /* A carrier became usable: close the current wait event. Both
             * L_wait gauges keep the last event's values (bytes queued and
             * how long the wait ran), so R_c x wait can be read directly;
             * tx_wait_carrier counts the events. */
            if (sess->tx_wait_since_ns)
            {
                sess->tx_wait_carrier_ns =
                    tbt_now_ns() - sess->tx_wait_since_ns;
                sess->tx_wait_since_ns = 0;
            }
            fr->path = c->index;
            if (fr->retx_copy)
                sess->dbg_retx_batch++;
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
            /* Only this pump subtracts (under pump_lock); captures only add,
             * so the guard cannot underflow between read and sub. */
            tbt_tx_frame_drop_account(sess, fr);
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
            struct tbt_carrier *c = tbt_frame_carrier(sess, fr);
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
                /* The slot vanished between selection and write: the frame
                 * was already taken out of tx_queued, so only the dup
                 * accounting is still outstanding. */
                spin_lock(&sess->tx_lock);
                tbt_tx_frame_drop_account(sess, fr);
                spin_unlock(&sess->tx_lock);
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
                if (type == TBT_F_HELLO && err != -EAGAIN)
                    pr_warn_ratelimited("tbtun: tunnel %llu: HELLO write failed: carrier=%u gen=%u ret=%d retries=%u ctl_queued=%u tx_queued=%u\n",
                                        (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
                                        c->index, c->gen, err, fr->retries,
                                        READ_ONCE(sess->ctl_queued),
                                        READ_ONCE(sess->tx_queued));
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
                    fr->path = TBT_PATH_NONE;
                    fr->avoid_mask = BIT(c->index);
                    sess->dbg_pump_hiwat++;
                    if (fr->retx_copy)
                    {
                        sess->dbg_retx_eagain++;
                        if (debug >= 1)
                            pr_info_ratelimited("tbtun: retx eagain seq=%llu len=%u path=%u src=%u tx_queued=%llu txq_max=%u\n",
                                                (unsigned long long)fr->hdr.seq,
                                                (unsigned int)fr->len,
                                                (unsigned int)c->index,
                                                (unsigned int)fr->retx_src,
                                                (unsigned long long)READ_ONCE(sess->tx_queued),
                                                (unsigned int)sess->txq_max);
                    }
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
                fr->path = TBT_PATH_NONE;
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
                TBT_DIAG("HELLO written tunnel=%llu carrier=%u gen=%u up=%d spare=%d state=%u",
                         (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
                         c->index, c->gen, atomic_read(&c->up), c->spare,
                         c->state);
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

                    if (idx < TBT_MAX_CARRIERS_LIMIT)
                    {
                        WRITE_ONCE(fr->st->announced_gen[idx],
                                   (u8)(c->gen & 0xf));
                        set_bit(idx, fr->st->announced_mask);
                    }
                    if (WARN_ON_ONCE(!tbt_stream_announced(fr->st, c)))
                    {
                        sess->dbg_announce_mismatch++;
                        pr_info_ratelimited("tbtun: announce mismatch idx=%u gen=%u stored=%u bit=%d path=%u st=%llu\n",
                                            idx, (unsigned)(c->gen & 0xf),
                                            fr->st->announced_gen[idx],
                                            test_bit(idx, fr->st->announced_mask),
                                            fr->path,
                                            (unsigned long long)fr->st->sid);
                    }
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
                    tbt_session_arm(sess, TBT_SW_PENDING,
                                    msecs_to_jiffies(1));
                break;
            case TBT_F_DATA:
            {
                struct tbt_retain *r = fr->ret;
                u64 now = tbt_now_ns();

                if (r)
                {
                    bool path_ok;

                    spin_lock(&sess->tx_lock);
                    /* Every DATA record the carrier accepted, copies
                     * included: the honest retransmit denominator. */
                    sess->data_tx++;
                    /* The retention bookkeeping is 16 slots wide and only the
                     * old ARQ fills it (the parser refuses fec=off above 16
                     * carriers). Guard the index anyway: a FEC v6 frame that
                     * ever reached this path with carrier 16..255 would write
                     * past path_gen[] into the neighbouring fields. */
                    path_ok = tbt_arq_path_ok(sess, c->index);
                    if (WARN_ON_ONCE(!path_ok))
                        path_ok = false;
                    if (path_ok)
                    {
                        r->path_mask |= BIT(c->index);
                        r->reserve_mask &= ~BIT(c->index);
                        r->path_gen[c->index] = c->gen & 0xf;
                    }
                    /* The mark is the byte just past this record. A short
                     * write left its tail in c->part, and xmit still reported
                     * success, so those bytes are not in TCP yet: count them,
                     * or the record looks acknowledged before it was even
                     * handed over and the RTO fires on a frame still in our
                     * own buffer. The injector wrote nothing, so its mark is
                     * where snd_nxt stood when it dropped the frame. */
                    if (path_ok)
                    {
                        struct sock *sk = tbt_carrier_sk_get(c);

                        r->first_sent_seq[c->index] = fr->dropped || !sk
                            ? c->drop_snd_nxt
                            : READ_ONCE(tcp_sk(sk)->write_seq) + c->part_len;
                        tbt_carrier_sk_put(sk);
                    }
                    if (path_ok)
                        r->una_observed_ns[c->index] = 0;
                    if (fr->retx_copy)
                    {
                        sess->retx_frames++;
                        sess->retx_bytes += fr->len;
                        c->retx_data++;
                        switch (fr->retx_src)
                        {
                        case TBT_RETX_HOLE:
                            sess->retx_hole++;
                            break;
                        case TBT_RETX_SLOW:
                            sess->retx_slow++;
                            break;
                        case TBT_RETX_PATHLOSS:
                            sess->retx_pathloss++;
                            break;
                        default:
                            sess->retx_other++;
                            break;
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
                {
                    /* Same lock as the path_mask update above: a reader in
                     * tbt_retain_all_received() must see the end offset and
                     * the mask bit together. */
                    spin_lock(&sess->tx_lock);
                    if (tbt_retain_path_ok(sess, c->index))
                        r->path_end[c->index] = c->payload_bytes_sent;
                    spin_unlock(&sess->tx_lock);
                }
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
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);
        struct sock *sk;

        if (!c)
            continue;
        sk = tbt_carrier_sk_get(c);
        if (sk)
        {
            if (sk_stream_memory_free(sk))
            {
                if (sk->sk_socket)
                    clear_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
                if (cancel_delayed_work(&c->w_pump))
                    sess->pump_hw_disarmed++;
            }
            tbt_carrier_sk_put(sk);
        }
        tbt_carrier_put(c);
    }
    /* L_wait inputs: the last wait event's queue depth and duration. These
     * are gauges, not sums: tx_queued at the wait, and the elapsed wait. The
     * event count is tx_wait_carrier; a sum would grow with the pump rate
     * instead of measuring R_c x wait. */
    if (wait_carrier)
    {
        sess->tx_wait_carrier++;
        sess->tx_wait_carrier_bytes = READ_ONCE(sess->tx_queued);
        if (sess->tx_wait_since_ns)
            sess->tx_wait_carrier_ns =
                tbt_now_ns() - sess->tx_wait_since_ns;
    }
    {
        bool wait_expired = false;

        if (sess->tx_wait_since_ns)
            wait_expired = tbt_now_ns() - sess->tx_wait_since_ns >=
                           (u64)TBT_TX_WAIT_CARRIER_MS * NSEC_PER_MSEC;
        /* In FEC mode tx_queued is the ARQ queue and stays near zero; waking
         * capture on every pump because of that alone spun w_read at ~1849/s.
         * Wake only when there is real work: an ARQ frame, a pinned control
         * frame, or data in the FEC packetizer's source ring. */
        if (READ_ONCE(sess->tx_queued) <= sess->txq_max / 2 &&
            (!sess->fec || READ_ONCE(sess->tx_queued) ||
             READ_ONCE(sess->ctl_queued) || tbt_fec_ring_busy(sess)))
        {
            struct tbt_stream *it;
            int i;

            rcu_read_lock();
            for (i = 0; i < TBT_STREAM_HT; i++)
            {
                hlist_for_each_entry_rcu(it, &sess->streams[i], node)
                {
                    /* Wake every stream, not only a paused one: a capture
                     * that returned short while waiting for a carrier left
                     * bytes in the public socket without setting tx_paused,
                     * and only this pump knows the queue is draining again. */
                    if (READ_ONCE(it->tx_paused))
                        WRITE_ONCE(it->tx_paused, false);
                    tbt_stream_schedule(it, &it->w_read,
                                        TBT_STREAM_F_SCHED_READ);
                }
            }
            rcu_read_unlock();
        }
        else if (wait_expired)
        {
            /* The wait outlived the dial window: wake capture once so its
             * next submit sees tbt_tx_should_backpressure() and stops
             * reading. A stream already in backpressure is skipped; arming
             * it every 10 ms would only repeat the same failed submit. */
            struct tbt_stream *it;
            int i;

            rcu_read_lock();
            for (i = 0; i < TBT_STREAM_HT; i++)
            {
                hlist_for_each_entry_rcu(it, &sess->streams[i], node)
                {
                    if (READ_ONCE(it->tx_paused))
                        continue;
                    tbt_stream_schedule(it, &it->w_read,
                                        TBT_STREAM_F_SCHED_READ);
                }
            }
            rcu_read_unlock();
        }
    }
    sess->dbg_pump_moved += moved;
    mutex_unlock(&sess->pump_lock);
    for (i = 0; i < nsnap; i++)
        tbt_stream_put(snap[i]);
    return moved;
}

// ---------------------------------------------------------------------------
// Capture

/* Build the duplicate copies of one DATA frame. Everything this reads is
 * private to the caller: `hdr` is a stack copy and `r` is not yet in the
 * retain list, so no lock is needed and no concurrent trim/gc can free it.
 * Returns how many copies were built (0..TBT_DUP_MAX); a failure only lowers
 * the count and never publishes a partial frame. */
static unsigned int tbt_tx_build_dups(struct tbt_session *sess,
                                      struct tbt_retain *r,
                                      const struct tbt_hdr *hdr,
                                      u16 chunk,
                                      struct tbt_frame *dup[TBT_DUP_MAX])
{
    unsigned int n = 0;

    for (u32 k = 0; k < sess->tunnel->cfg.dup && n < TBT_DUP_MAX; k++)
    {
        u64 goodput = tbt_stat_read(&sess->tx_submit_bytes) + chunk;
        u64 dup_after = tbt_stat_read(&sess->dup_bytes) + chunk;
        struct tbt_frame *d;

        /* Projected ratio, so the measured inflation cannot exceed the cap.
         * Headers and retransmits are not duplication and stay excluded. */
        if ((goodput + dup_after) * 1000 >
            goodput * sess->tunnel->cfg.max_inflate_milli)
        {
            atomic64_inc(&sess->dup_suppressed);
            break;
        }
        d = tbt_frame_alloc(chunk, GFP_ATOMIC);
        if (!d)
        {
            atomic64_inc(&sess->dbg_alloc_fail);
            break;
        }
        d->hdr = *hdr;
        d->hdr.flags |= TBT_FF_LOW_PRIO;
        d->dup_copy = true;
        d->ret = r;
        memcpy(d->data, r->data, chunk);
        dup[n++] = d;
    }
    return n;
}

/* Publish the primary frame, its retain entry and every duplicate in one
 * tx_lock critical section. Returns false when the queue cannot hold the
 * whole set; the caller still owns fr, r and dup[] then. On success the
 * ownership of all three moves to tx_q / st->retain, and the caller must not
 * touch them again. */
static bool tbt_tx_publish_data(struct tbt_session *sess,
                                struct tbt_stream *st,
                                struct tbt_frame *fr,
                                struct tbt_frame *const dup[],
                                unsigned int ndup,
                                struct tbt_retain *r,
                                u16 chunk,
                                const struct tbt_hdr *hdr)
{
    u64 seq;
    struct tbt_tx_acct acct;

    if (WARN_ON_ONCE(ndup > U8_MAX))
        return false;

    spin_lock(&sess->tx_lock);
    if (!tbt_tx_acct_fits(sess->tx_queued, sess->txq_max, chunk, ndup))
    {
        spin_unlock(&sess->tx_lock);
        return false;
    }

    if (!st->tx_start_ns)
        st->tx_start_ns = tbt_now_ns();
    seq = st->tx_seq;
    st->tx_seq = seq + chunk;

    r->seq = seq;
    r->hdr = *hdr;
    r->hdr.seq = seq;

    fr->hdr = *hdr;
    fr->hdr.seq = seq;
    fr->open_pending = st->near && !READ_ONCE(st->tx_open_sent);
    fr->ret = r;
    fr->st = st;
    refcount_inc(&st->refs);

    for (unsigned int k = 0; k < ndup; k++)
    {
        dup[k]->hdr.seq = seq;
        dup[k]->st = st;
        refcount_inc(&st->refs);
    }

    acct.tx_queued = sess->tx_queued;
    acct.dup_bytes = 0;
    acct.retain_bytes = st->retain_bytes;
    tbt_tx_acct_publish(&acct, chunk, ndup, &r->dup_pending);
    sess->tx_queued = acct.tx_queued;
    atomic64_add(acct.dup_bytes, &sess->dup_bytes);
    st->retain_bytes = acct.retain_bytes;

    list_add_tail(&fr->list, &sess->tx_q);
    list_add_tail(&r->list, &st->retain);
    for (unsigned int k = 0; k < ndup; k++)
        list_add_tail(&dup[k]->list, &sess->tx_q);

    spin_unlock(&sess->tx_lock);
    return true;
}

/* Give back the accounting of a duplicate DATA frame removed from tx_q
 * without being written. Every path that frees such a frame calls this under
 * sess->tx_lock; the caller subtracts tx_queued itself. A non-dup frame (the
 * primary copy, control) owns no dup accounting and returns untouched. */
void tbt_tx_frame_drop_account(struct tbt_session *sess, struct tbt_frame *fr)
{
    u64 sub;

    if (!tbt_tx_acct_is_dup(fr->hdr.type, fr->dup_copy))
        return;
    sub = tbt_tx_acct_drop(tbt_stat_read(&sess->dup_bytes), fr->len,
                           fr->ret ? &fr->ret->dup_pending : NULL);
    if (sub)
        atomic64_sub(sub, &sess->dup_bytes);
}

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

    atomic64_inc(&sess->tx_submit_calls);
    if (sess->fec)
        return tbt_fec_tx_submit(st, skb, off, len);

    while (done < len)
    {
        u16 chunk = min_t(size_t, len - done, (size_t)TBT_PAYLOAD_MAX);
        struct tbt_retain *r;
        struct tbt_frame *fr;
        struct tbt_frame *dup[TBT_DUP_MAX];
        struct tbt_hdr hdr;
        unsigned int ndup;

        /* ---- A. Outside tx_lock: only this call's private objects. ---- */
        r = tbt_retain_alloc(chunk, GFP_ATOMIC);
        fr = tbt_frame_alloc(chunk, GFP_ATOMIC);
        if (!r || !fr)
        {
            kfree(r);
            kfree(fr);
            atomic64_inc(&sess->dbg_alloc_fail);
            break;
        }
        if (skb_copy_bits(skb, off + done, r->data, chunk))
        {
            kfree(r);
            kfree(fr);
            atomic64_inc(&sess->dbg_copy_fail);
            break;
        }
        memcpy(fr->data, r->data, chunk);

        /* The header is a stack copy: seq is filled in under tx_lock, every
         * other field is already final. dup[] reads only this copy and
         * r->data; r is not yet reachable by the pump or by trim/gc. */
        hdr = (struct tbt_hdr){
            .type = TBT_F_DATA,
            .body_len = chunk,
            .sid_lo = (u32)st->sid,
            .conv = TBT_CONV,
        };
        hdr.sid_hi = (u8)(st->sid >> 32);
        ndup = tbt_tx_build_dups(sess, r, &hdr, chunk, dup);

        /* ---- B. One tx_lock: publish or roll back everything. ---- */
        if (!tbt_tx_publish_data(sess, st, fr, dup, ndup, r, chunk, &hdr))
        {
            /* publish failed, so fr, r and dup[] are still ours. */
            for (u32 k = 0; k < ndup; k++)
                kfree(dup[k]);
            kfree(fr);
            kfree(r);
            /* Backpressure, not a loss: the bytes stay in the public socket's
             * receive queue, its window closes, and the application's send()
             * blocks. seq did not move. While a carrier wait is still inside
             * its dial window, leave tx_paused alone: the pump wakes capture
             * when the wait closes or expires. */
            /* The pump may not have run since the last carrier died. Ask for
             * one before the first refusal becomes user backpressure; the
             * wait window starts here, not at the pump's next pass. */
            if (!READ_ONCE(sess->tx_wait_since_ns))
                tbt_tx_request_carrier(sess);
            if (tbt_tx_should_backpressure(sess) &&
                !READ_ONCE(st->tx_paused))
            {
                u64 since = READ_ONCE(sess->tx_wait_since_ns);
                u64 now = tbt_now_ns();

                WRITE_ONCE(st->tx_paused, true);
                atomic64_inc(&sess->tx_backpressure);
                /* L_wait evidence for the "last resort": how much was
                 * queued, and how long the carrier wait had already run. */
                atomic64_set(&sess->tx_backpressure_bytes,
                             READ_ONCE(sess->tx_queued));
                atomic64_set(&sess->tx_backpressure_ns,
                             since ? now - since : 0);
            }
            atomic64_inc(&sess->dbg_over_qmax);
            break;
        }

        /* Published: fr, dup[] and r belong to tx_q / st->retain now. */
        done += chunk;
        atomic64_add(chunk, &sess->tx_submit_bytes);
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
    if (sess->fec)
        return 0; /* FEC has its own ACK/FBK records */
    /* The FBK body has exactly TBT_LEGACY_MAX_CARRIERS entries. A non-FEC
     * session with more carriers is a configuration-chain failure; refuse it
     * rather than write past the stack body. */
    if (WARN_ON_ONCE(sess->n_car > TBT_LEGACY_MAX_CARRIERS))
        return -EINVAL;
    u8 body[TBT_FBK_MAX];
    u32 nr = 0;
    u16 mask = 0;
    u8 state = tbt_rx_state(st);
    struct tbt_frame *fr;
    int i;

    for (i = 0; i < sess->n_car && i < TBT_LEGACY_MAX_CARRIERS &&
                nr < TBT_LEGACY_MAX_CARRIERS; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);
        struct sock *sk;
        u8 *e;
        u32 rtt = 0;

        if (!c)
            continue;
        if (!atomic_read(&c->up))
        {
            tbt_carrier_put(c);
            continue;
        }
        /* The reference makes the socket safe outside the RCU section; the
         * read is a short scalar snapshot, not a lock_sock. */
        sk = tbt_carrier_sk_get(c);
        if (sk)
        {
            rtt = READ_ONCE(tcp_sk(sk)->srtt_us) >> 3;
            tbt_carrier_sk_put(sk);
        }
        WRITE_ONCE(c->rx_state, state);
        e = body + nr * TBT_FBK_ENTRY_LEN;
        /* The generation in feedback is the *peer's* generation for this path
         * (learned from its HELLO), because the entry describes the frames the
         * peer sent on it. The peer compares it to its own carrier gen. */
        e[TBT_FBK_PATH] = (u8)i | ((READ_ONCE(c->peer_gen) & 0xf) << 4);
        e[TBT_FBK_RXSTATE] = state;
        /* Per-path cumulative high water: a later byte on this carrier proves
         * every earlier frame on it arrived (TCP is ordered per path). The
         * FBK header's uack stays the global contiguous offset used by the
         * send window and retention. */
        tbt_put_le64(e + TBT_FBK_UACK, READ_ONCE(c->rx_bytes));
        tbt_put_le32(e + TBT_FBK_RECVCNT, (u32)READ_ONCE(c->rx_frames));
        tbt_put_le32(e + TBT_FBK_RTT, rtt);
        mask |= BIT(i);
        nr++;
        tbt_carrier_put(c);
    }
    if (!nr)
        return 0;
    if (WARN_ON_ONCE(nr > U8_MAX))
        return -EINVAL;
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
        struct tbt_carrier *sp = tbt_carrier_get(sess, (u8)i);
        bool pin = sp && sp->spare && atomic_read(&sp->up) &&
                   !atomic_read(&sp->stopping);

        if (pin)
        {
            fr->pinned = true;
            fr->path = (u16)i;
            fr->gen = sp->gen;
            tbt_carrier_put(sp);
            break;
        }
        tbt_carrier_put(sp);
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
        tbt_session_arm(sess, TBT_SW_FBK, 0);
    else
        tbt_session_arm(sess, TBT_SW_FBK,
                        msecs_to_jiffies(TBT_FBK_INTERVAL_MS));
}

int tbt_tx_sack(struct tbt_session *sess, struct tbt_stream *st, gfp_t gfp)
{
    if (sess->fec)
        return 0;
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
    bool owned = test_and_clear_bit(TBT_SW_FBK, &sess->sched);
    bool alive = false;
    int nsnap, i;

    if (!sess->tunnel)
    {
        if (owned)
            tbt_session_put(sess);
        return;
    }
    nsnap = tbt_session_stream_snapshot(sess, snap, ARRAY_SIZE(snap));
    tbt_tx_path_lost_drain(sess, snap, nsnap);
    for (i = 0; i < nsnap; i++)
    {
        struct tbt_stream *st = snap[i];
        u8 state = tbt_rx_state(st);
        bool one_alive;

        st->fbk_frames = 0;
        tbt_tx_close_check(sess, st);
        {
            u64 sig = sess->n_car;
            u64 now = tbt_now_ns();
            int ci;

            /* The FBK body is the per-path receive high-water and count plus
             * the stream rx state. Re-sending it every 10 ms with none of
             * those changed is pure overhead: a 37-byte ping measured 259 to
             * 442 FBKs against 4 DATA frames, and every one of them ran the
             * peer's geometric hole check. The signature includes the carrier
             * set (index and generation), so a replacement or a new carrier
             * also forces one. The 10 ms tick itself stays: the pump below
             * still drives retransmission, close and hole timers. */
            for (ci = 0; ci < sess->n_car; ci++)
            {
                struct tbt_carrier *fc = READ_ONCE(sess->car[ci]);

                if (!fc || !atomic_read(&fc->up))
                    continue;
                sig = sig * 131 + (u64)ci;
                sig = sig * 131 + READ_ONCE(fc->gen);
                sig = sig * 131 + READ_ONCE(fc->rx_bytes);
                sig = sig * 131 + READ_ONCE(fc->rx_frames);
                sig = sig * 131 + READ_ONCE(fc->peer_gen);
            }
            if (READ_ONCE(st->rx_next) != st->fbk_last_rx_next ||
                sig != st->fbk_sig || state != st->fbk_last_state ||
                !st->fbk_last_ns ||
                now - st->fbk_last_ns >=
                    (u64)TBT_FBK_IDLE_MS * NSEC_PER_MSEC)
            {
                if (tbt_tx_fbk(sess, st, GFP_KERNEL) == 0)
                {
                    st->fbk_last_rx_next = READ_ONCE(st->rx_next);
                    st->fbk_sig = sig;
                    st->fbk_last_state = state;
                    st->fbk_last_ns = now;
                }
            }
        }
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
        tbt_rx_deliver_stall_check(st);
        tbt_tx_stall_check(sess, st);
        /* The capture work emits a FIN once.  If no carrier was usable then,
         * the request stays set and is retried here.  The stream leaves the
         * session only after that FIN is written and the peer's FIN has been
         * delivered (tbt_stream_maybe_detach), never on the queued copy. */
        if (one_alive && READ_ONCE(st->tx_fin_sent) &&
            !READ_ONCE(st->tx_fin_queued) &&
            test_bit(TBT_STREAM_F_ACTIVE, &st->attached) &&
            tbt_tx_fin(sess, st, GFP_KERNEL) > 0)
            WRITE_ONCE(st->tx_fin_queued, true);
        tbt_stream_maybe_detach(st);
        alive |= one_alive;
    }
    for (i = 0; i < nsnap; i++)
        tbt_stream_put(snap[i]);
    tbt_tx_pump(sess);
    if (alive && !READ_ONCE(sess->closing))
        tbt_session_arm(sess, TBT_SW_FBK,
                        msecs_to_jiffies(TBT_FBK_INTERVAL_MS));
    if (owned)
        tbt_session_put(sess);
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
        /* Session teardown: keep dup_bytes/dup_pending consistent with the
         * frames that are about to be freed, even though the session is
         * dying. */
        tbt_tx_frame_drop_account(sess, fr);
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
        /* Contract: tbt_tx_free_queue() has already dropped every queued
         * copy, so no retain entry can still owe a duplicate. */
        WARN_ON_ONCE(r->dup_pending);
        list_del(&r->list);
        kfree(r);
    }
    st->retain_bytes = 0;
}
