/* tbt_fec_data.c - FEC v6 data plane: packetizer, grouped block codec,
 * record writer, receiver reassembly.
 *
 * Scope (T2):
 *   - carriers = groups * (n + m), one share per carrier;
 *   - carrier c carries group c/(n+m) share c%(n+m);
 *   - global packet seq is striped relative to the shape origin: group =
 *     (seq - base_pkt) % groups, block_id = base_block + ((seq - base_pkt) /
 *     groups) / n, share = ((seq - base_pkt) / groups) % n. The 0.2.x
 *     mapping is the base_pkt = base_block = 0 case; a shape switch only
 *     moves the origin (tbt_fec.h helpers).
 *   - a block is n DATA packets followed by m parity packets; the codec is the
 *     host-tested GF(2) RLNC in tbt_fec.c, reused with a per-group, per-block
 *     nonce;
 *   - both containers: raw records, or one 48-byte record per HTTP chunk.
 *
 * The capture actor runs in softirq: tbt_fec_tx_submit only copies bytes into
 * a ring and schedules a work item. Packetization, encoding, and socket writes
 * run in tbt_fec_tx_work (process context), so no sleeping call is reachable
 * from the actor.
 *
 * The receiver runs inside the carrier parser, which may also be softirq. It
 * copies shares into preallocated block buffers, decodes with preallocated
 * workspaces, and queues ACKs to the same work item; it never allocates a
 * codec or calls a socket from that path.
 *
 * Delivery is by super-block: block b of every group covers the contiguous
 * global range [base_pkt + (b-base_block)*groups*n,
 * base_pkt + (b+1-base_block)*groups*n). A super-block is handed to the
 * stream only when every group's block b is decoded, so the byte stream has
 * no hole and the ACK frontier is a real global frontier, not a per-group
 * guess.
 */
#include <linux/ktime.h>
#include <linux/random.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <net/tcp.h>

#include "tbtun.h"
#include "tbt_fec.h"
#include "tbt_fec_adapt.h"
#include "tbt_fec_adapt_sclc.h"
#include "tbt_sclc_uplink.h"
#include "tbt_rec.h"

/* Defined next to the codec constructors; the shape-switch paths that run
 * earlier in this file need the declaration. */
static const struct tbt_fec_ops *tbt_fec_ops_of_shape(
    const struct tbt_fec_shape *sh);
static struct tbt_fec_grouped *tbt_fec_codec_new(
    const struct tbt_fec_sclc_shape *sclc, u16 groups, u16 n, u16 m, u16 k,
    u16 w, u8 d, u64 nonce);
static void tbt_fec_tx_init_tick(struct tbt_session *sess, struct tbt_fec_tx *tx);
static void tbt_fec_alloc_tx_tick(struct tbt_session *sess,
                                  struct tbt_fec_flow *f);
static void tbt_fec_alloc_rx_release_old(struct tbt_session *sess,
                                         struct tbt_fec_flow *f);

/* Give a pending switch shape its wire identity before it is stored or
 * copied. An RLNC shape may arrive with variant 0 while its group count
 * says grouped; the CFG and the peer's FBK both name variant 1. */
static void tbt_fec_shape_wire_norm(struct tbt_fec_shape *sh)
{
    if (!sh)
        return;
    sh->codec_variant = tbt_fec_variant_norm(sh->codec_variant, sh->groups);
    if (sh->codec_variant != 2)
    {
        sh->L = 0;
        sh->block = 0;
        sh->rate_ppm = 0;
    }
}

/* The source ring is the capture's pause granularity: tbt_fec_tx_submit()
 * sets tx_paused only when the ring cannot take the next chunk. 256 KiB
 * paused capture every 32 packets while the credit window allowed 1024, so
 * the pump had to resume capture repeatedly. Size the ring from the credit
 * window, with the old 256 KiB floor and a 1 MiB ceiling. */
#define TBT_FEC_TX_RING (256 * 1024)
#define TBT_FEC_TX_RING_MAX (1024 * 1024)

/* tx->run / rx->run ownership bits. The sched bit in the session owns the
 * reference; this word owns the shared buffers and cursors. A work entry that
 * finds bit 0 set must only raise bit 1 and return: workqueue can run a
 * re-queued work_struct concurrently with its previous instance, and two
 * packetizers share tx->pkt/tx->rec, while two delivery walks share
 * next_super/deliver_bytes. */
#define TBT_FEC_RUN_BIT   0u
#define TBT_FEC_AGAIN_BIT 1u

/* How long a partial packet may wait for company. The packetizer emits a
 * share only when its packet is full, so an application that writes a small
 * request and then waits for the answer -- an HTTP request, iperf3's control
 * cookie -- would wait for 8192 bytes that never come, and the tunnel would
 * look connected and silent. 2 ms is above the gap between two writes of a
 * saturating sender, so bulk traffic still fills its packets, and far below
 * any round trip, so an interactive exchange pays one tick. */
#define TBT_FEC_SHORT_MS 2
/* A super-block that has not become ready for this long cannot become ready:
 * this wire has no NACK and no retransmission, so a share that never arrived
 * is never fetched. The bound is well above any recovery the parity can do
 * (all shares of one block arrive within one carrier burst) and below curl's
 * 120 s no-progress limit, so the application sees a reset instead of a
 * silent stall. */
#define TBT_FEC_STALL_MS 30000
#define TBT_FEC_NONCE_GOLDEN 0x9e3779b97f4a7c15ULL
/* The RX arena holds at most 16 MiB of super-blocks: slots is the per-group
 * block count, and the ACK credit is additionally capped to
 * TBT_FEC_CREDIT_MAX packets (8 MiB at S=8192) and to K-2 super-blocks. */
#define TBT_FEC_RX_BUDGET (16u * 1024 * 1024)
/* One FEC drain run sends at most this many batches before yielding to the
 * packetizer and the other carriers. A full 256-carrier burst is served in
 * four such runs, each of which re-arms with no delay while fresh bits
 * remain; the cap bounds the worker hold time, not the delivery latency. */
#define TBT_FEC_DRAIN_PASS_MAX 64
/* The carrier's short-write tail holds a whole batch: TBT_CAR_PART_MAX is
 * TBT_FEC_BATCH_MAX frames and part_len is wide enough for it. A batch's
 * worst-case framed total is at most that (one full record is
 * TBT_HTTP_FEC_FRAME_MAX), so one http_begin/lock_sock/tcp_sendmsg can carry
 * up to TBT_FEC_BATCH_MAX full records without overflowing the tail. */
#define TBT_FEC_DRAIN_BYTES_MAX TBT_CAR_PART_MAX

/* A block codec decodes a whole block per attempt, so an attempt made while
 * the block is still receiving shares both fails and runs the expensive
 * erasure-heavy path (the uplink GF(16) peeling is ~3.6x slower at 70%
 * erasures than at 2%, and the downlink runs all 128 batches regardless).
 * Wake the decoder only after the block has been quiet for this long.  The
 * window is longer than the gap between a block's shares on the field path
 * (the uplink's 32 shares span ~0.8 s, so ~25 ms apart; the downlink's 256
 * span ~0.35 s), which makes a burst coalesce into one decode at its end.
 * A failed decode then backs off; a new share restarts the quiet window
 * because it is new evidence.  Without this the field measured 7.7
 * downlink decodes per delivered 1 MiB block and 2 uplink decodes per
 * 64 KiB group block. */
#define TBT_FEC_DECODE_SETTLE_MS 50
#define TBT_FEC_DECODE_RETRY_MS 50
#define TBT_FEC_DECODE_RETRY_MAX_MS 400

/* No carrier TCP is left to deliver a share: a retransmit count at the
 * default tcp_retries2 (15) is the point where TCP itself gives up. */
#define TBT_FEC_TCP_RETX_MAX 15

/* Raise an atomic maximum without losing a concurrent larger sample. */
static void tbt_fec_stat_max(atomic64_t *max, u64 v)
{
    u64 cur = atomic64_read(max);

    while (v > cur)
    {
        u64 prev = atomic64_cmpxchg(max, cur, v);

        if (prev == cur)
            break;
        cur = prev;
    }
}

/* True while at least one carrier TCP can still deliver a byte: ESTABLISHED
 * and below the default tcp_retries2. Only when every carrier fails this
 * test may the FEC receive side reset the stream; a merely slow or stalled
 * carrier keeps the repair (NACK, replay, TCP retransmission) alive. */
static bool tbt_fec_any_tcp_alive(struct tbt_session *sess)
{
    int i;

    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);
        struct sock *sk;
        bool alive = false;

        if (!c)
            continue;
        if (!atomic_read(&c->stopping))
        {
            sk = tbt_carrier_sk_get(c);
            if (sk)
            {
                alive = READ_ONCE(sk->sk_state) == TCP_ESTABLISHED &&
                        READ_ONCE(inet_csk(sk)->icsk_retransmits) <
                            TBT_FEC_TCP_RETX_MAX;
                tbt_carrier_sk_put(sk);
            }
        }
        tbt_carrier_put(c);
        if (alive)
            return true;
    }
    return false;
}

/* Both ends derive the same coefficients from the configured parameters. The
 * session nonce is not available on the far side until HELLO, and FEC state is
 * built before that; a fixed base keeps sender and receiver aligned. FEC_CFG
 * can carry a negotiated seed later without changing the 48-byte header. */
static u64 tbt_fec_base_nonce(const struct tbt_session *sess)
{
    (void)sess;
    return 0x5a5a5a5a5a5a5a5aULL;
}

/* Group g uses seed_g = base ^ (g << 32); the block generation then mixes in
 * the same golden ratio as the single-group cut, so two blocks never reuse a
 * coefficient set. Sender and receiver derive this independently. */
static u64 tbt_fec_group_nonce(u64 base, u16 group, u64 block_id)
{
    return base ^ ((u64)group << 32) ^ (block_id * TBT_FEC_NONCE_GOLDEN);
}

static struct tbt_stream *tbt_fec_stream(struct tbt_session *sess)
{
    struct tbt_stream *st;
    u64 sid;
    int i;

    sid = READ_ONCE(sess->far_sid);
    if (sid)
    {
        st = tbt_stream_get(sess, sid, false, false);
        if (st &&
            !tbt_fec_tx_stream_usable(
                READ_ONCE(st->state) == TBT_ST_DEAD,
                test_bit(TBT_STREAM_F_ACTIVE, &st->attached),
                READ_ONCE(st->tombstone_until_ns)))
        {
            tbt_stream_put(st);
            return NULL;
        }
        return st;
    }
    spin_lock(&sess->lock);
    for (i = 0; i < TBT_STREAM_HT; i++)
    {
        hlist_for_each_entry(st, &sess->streams[i], node)
        {
            /* A gracefully closed stream is detached and tombstoned while
             * its state is still OPEN; returning it makes the TX work arm
             * itself at 0 delay until the reaper runs.  Only a live,
             * attached stream is a usable fallback. */
            if (!tbt_fec_tx_stream_usable(
                    READ_ONCE(st->state) == TBT_ST_DEAD,
                    test_bit(TBT_STREAM_F_ACTIVE, &st->attached),
                    READ_ONCE(st->tombstone_until_ns)))
                continue;
            if (refcount_inc_not_zero(&st->refs))
            {
                spin_unlock(&sess->lock);
                return st;
            }
        }
    }
    spin_unlock(&sess->lock);
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Output queue
 * ------------------------------------------------------------------ */

/* Drop one record reference. The record owns a flow reference for as long as
 * it exists, so a detached flow's planes cannot be freed under an in-flight
 * flush. */
static void tbt_fec_out_put(struct tbt_fec_out *o)
{
    if (!o)
        return;
    if (refcount_dec_and_test(&o->refs))
    {
        tbt_fec_flow_put(o->flow);
        kfree(o);
    }
}

/* Take one reader reference while the record is still on its queue. The
 * caller holds fec_out_lock. */
static struct tbt_fec_out *tbt_fec_out_get(struct tbt_fec_out *o)
{
    if (o)
        refcount_inc(&o->refs);
    return o;
}

/* Unlink one queued record and drop the queue's reference. `q` is its carrier
 * queue, or NULL for a control or replay record. The caller holds
 * fec_out_lock. A record already unlinked by a flow purge is left alone, so
 * purge and an in-flight flush can race without a double list_del. */
static void tbt_fec_out_free(struct tbt_session *sess, struct tbt_fec_outq *q,
                             struct tbt_fec_out *o)
{
    if (!tbt_fec_out_unlink_once(&o->queued))
    {
        /* The queue reference is already gone (a flow purge won the race);
         * the caller still owns whatever reader reference it took. */
        return;
    }
    list_del(&o->node);
    if (o->replay)
    {
        if (sess->fec_replay_queued)
            sess->fec_replay_queued--;
        if (sess->fec_replay_bytes >= o->len)
            sess->fec_replay_bytes -= o->len;
    }
    if (o->control)
    {
        if (sess->fec_out_ctl_depth)
            sess->fec_out_ctl_depth--;
        if (sess->fec_out_ctl_bytes >= o->len)
            sess->fec_out_ctl_bytes -= o->len;
    }
    if (q)
    {
        /* Guard both counters: a double free or a mismatched queue must not
         * wrap them. A wrapped byte count makes every later push look full
         * (the 1 GiB field run dropped 22 shares while its depth high-water
         * stayed at 99, which is exactly that signature). */
        if (q->depth)
            q->depth--;
        if (q->bytes >= o->len)
            q->bytes -= o->len;
        else
            q->bytes = 0;
        if (sess->fec_outq_queued)
            sess->fec_outq_queued--;
        if (sess->fec_outq_bytes >= o->len)
            sess->fec_outq_bytes -= o->len;
        else
            sess->fec_outq_bytes = 0;
    }
    if (sess->fec_out_queued)
        sess->fec_out_queued--;
    if (o->owner && o->owner->queued)
        o->owner->queued--;
    tbt_fec_out_put(o);
}

void tbt_fec_out_kick(struct tbt_session *sess, u16 carrier)
{
    if (!sess->fec || READ_ONCE(sess->fec_stopping) || !sess->fec_ndrain ||
        carrier >= sess->fec_ndrain)
        return;
    set_bit(carrier, sess->fec_drain_ready);
    mod_delayed_work(TBT_WQ, &sess->fec_drain_work, 0);
}

static int tbt_fec_out_push(struct tbt_session *sess, u16 carrier,
                            const u8 *buf, u16 len, bool control, bool replay,
                            u64 stamp_ns, struct tbt_fec_flow *flow)
{
    struct tbt_fec_out *o;
    struct tbt_fec_tx *owner = flow ? flow->tx : NULL;
    struct tbt_fec_outq *q = NULL;
    bool first = false;

    /* A record is per-flow work. Refuse one whose flow is already dying:
     * purge may have run, and this would otherwise reach a carrier after the
     * flow left the table. A NULL flow is reserved for session-wide records
     * (v7 has none). */
    if (flow && !tbt_fec_flow_live(READ_ONCE(flow->state)))
        return -ESHUTDOWN;
    if (control)
    {
        /* The session control queue is carrier-agnostic; the flush picks an
         * up carrier for each record. */
    }
    else if (replay)
    {
        /* The replay queue is also carrier-agnostic: the record's header
         * names its slot, and the flush picks the least-loaded carrier. */
        carrier = 0;
    }
    else
    {
        if (carrier >= sess->n_car || !sess->fec_outq)
            return -EINVAL;
        q = &sess->fec_outq[carrier];
    }
    o = kmalloc(struct_size(o, data, len), GFP_ATOMIC);
    if (!o)
        return -ENOMEM;
    refcount_set(&o->refs, 1); /* the queue reference */
    o->queued = false;
    o->flow = flow;
    if (flow && !refcount_inc_not_zero(&flow->refs))
    {
        /* The caller raced the final put. Do not publish a record that
         * outlives its flow. */
        kfree(o);
        return -ESHUTDOWN;
    }
    o->carrier = carrier;
    o->len = len;
    o->control = control ? 1 : 0;
    o->replay = replay ? 1 : 0;
    o->block_id = 0;
    o->sid = 0;
    o->owner = NULL;
    {
        struct tbt_fec_hdr h;

        /* The record is complete here, header included; unpack is a plain
         * byte copy. The block id lets the flush drop a repair the peer has
         * already delivered. */
        if (!tbt_fec_hdr_unpack(buf, &h))
        {
            o->block_id = h.block_id;
            o->sid = h.sid;
        }
    }
    o->stamp_ns = stamp_ns;
    memcpy(o->data, buf, len);
    spin_lock(&sess->fec_out_lock);
    if (replay &&
        sess->fec_replay_bytes + len >
            tbt_fec_replay_cap(READ_ONCE(sess->fec_closing),
                               TBT_FEC_REPLAY_BYTES_MAX))
    {
        spin_unlock(&sess->fec_out_lock);
        tbt_fec_out_put(o);
        return -ENOSPC;
    }
    /* These are the hard caps, not the normal stop: the packetizer pauses at
     * TBT_FEC_OUTQ_HIGH_* and pushes the pressure into the capture ring, so a
     * record reaches this branch only if the pause was bypassed (a parity
     * burst larger than the margin, or a shape switch). The replay ring still
     * holds the share and a NACK repairs it, but every hit is a multi-second
     * repair, so it stays counted and visible. */
    if (q && (q->depth >= TBT_FEC_OUTQ_DEPTH_MAX ||
              q->bytes + len > TBT_FEC_OUTQ_BYTES_MAX ||
              sess->fec_outq_queued >= TBT_FEC_OUTQ_QUEUE_MAX ||
              sess->fec_outq_bytes + len > TBT_FEC_OUTQ_QUEUE_BYTES_MAX))
    {
        spin_unlock(&sess->fec_out_lock);
        tbt_fec_out_put(o);
        atomic64_inc(&sess->fec_outq_drop);
        return -ENOSPC;
    }
    if (replay)
    {
        sess->fec_replay_queued++;
        sess->fec_replay_bytes += len;
        if (sess->fec_replay_bytes >
            atomic64_read(&sess->fec_replay_queue_high))
            atomic64_set(&sess->fec_replay_queue_high,
                         sess->fec_replay_bytes);
    }
    if (control)
    {
        list_add(&o->node, &sess->fec_out_ctl);
        sess->fec_out_ctl_depth++;
        sess->fec_out_ctl_bytes += len;
    }
    else if (replay)
    {
        struct tbt_fec_out *pos;
        bool placed = false;

        /* Frontier first: a later super-block's repair must not sit ahead
         * of the block that holds the peer's ACK frontier. fix24's far had
         * an 8 MiB replay queue and the frontier's repair waited 6.13 s, so
         * the TX stall timer reset the stream. Within one block the NACK
         * order is kept. */
        list_for_each_entry(pos, &sess->fec_out_replay, node)
        {
            if (pos->block_id > o->block_id)
            {
                list_add_tail(&o->node, &pos->node);
                placed = true;
                break;
            }
        }
        if (!placed)
            list_add_tail(&o->node, &sess->fec_out_replay);
    }
    else
    {
        first = q->depth == 0;
        o->owner = owner;
        if (owner)
            owner->queued++;
        list_add_tail(&o->node, &q->queue);
        q->depth++;
        q->bytes += len;
        sess->fec_outq_queued++;
        sess->fec_outq_bytes += len;
        if (q->depth > sess->fec_outq_depth_max)
            sess->fec_outq_depth_max = q->depth;
    }
    o->queued = true;
    sess->fec_out_queued++;
    spin_unlock(&sess->fec_out_lock);
    if (!control && !replay)
    {
        /* Data drains in its carrier's own work. Wake it only when the queue
         * turned non-empty: a queue that is already draining or backing off
         * has its own schedule and must not be re-timed by every new share. */
        if (first)
            tbt_fec_out_kick(sess, carrier);
    }
    else
    {
        /* Control and replay are session-wide: the TX work picks a carrier. */
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    }
    return 0;
}

/* No usable carrier: ask the session's replacement path for one and keep the
 * record queued. The 2 ms flush arm retries; a source share is never dropped
 * here. The first wait in an episode is counted once, and the source ring is
 * the backpressure of last resort. */
static void tbt_fec_wait_carrier(struct tbt_session *sess,
                                 struct tbt_fec_tx *tx)
{
    if (tx && !tx->carrier_wait)
    {
        tx->carrier_wait = true;
        tx->carrier_wait_since_ns = ktime_get_ns();
        tx->backpressure_counted = false;
        atomic64_inc(&sess->fec_wait_carrier);
    }
    if (!READ_ONCE(sess->closing))
        tbt_session_arm(sess, TBT_SW_REPLACE, 0);
}

static u16 tbt_fec_replay_usable(struct tbt_session *sess, u8 *usable);
static u16 tbt_fec_replay_pick(struct tbt_session *sess, const u8 *usable,
                               const u32 *pace);

/* Write one replay record. The carrier is not part of the record's identity:
 * start at the emptiest usable queue, then rotate so one full socket cannot
 * hold the repair. `usable` is a per-flush snapshot; every candidate is
 * rechecked under its reference before the write. */
static int tbt_fec_out_try_replay(struct tbt_session *sess,
                                  struct tbt_fec_out *o, bool *sent,
                                  const u8 *usable, u16 ncand, u32 *pace)
{
    struct tbt_fec_tx *tx = o->flow ? o->flow->tx : NULL;
    u16 start, i;
    int rc = -ENODEV;

    start = ncand ? tbt_fec_replay_pick(sess, usable, pace) : U16_MAX;
    if (start == U16_MAX)
    {
        tbt_fec_wait_carrier(sess, tx);
        return -EAGAIN;
    }
    for (i = 0; i < sess->n_car; i++)
    {
        u16 idx = (u16)((start + i) % sess->n_car);
        struct tbt_carrier *c;
        int err;

        if (!usable[idx])
            continue;
        c = tbt_carrier_get(sess, (u8)idx);
        if (!c)
            continue;
        if (!atomic_read(&c->up) || atomic_read(&c->stopping) ||
            !atomic_read(&c->connected) || READ_ONCE(c->stalled))
        {
            tbt_carrier_put(c);
            continue;
        }
        err = tbt_carrier_send_fec(c, o->data, o->len);
        tbt_carrier_put(c);
        if (!err)
        {
            if (tx)
            {
                tx->carrier_wait = false;
                tx->backpressure_counted = false;
            }
            if (sent)
                *sent = true;
            pace[idx] += o->len;
            return 0;
        }
        if (err == -EAGAIN)
        {
            rc = -EAGAIN;
            continue;
        }
        if (err == -ENOTCONN || err == -ENODEV)
            continue;
        rc = err;
    }
    if (rc == -ENODEV)
    {
        tbt_fec_wait_carrier(sess, tx);
        return -EAGAIN;
    }
    return rc;
}

/* Try to write one record. `sent` is true only when a carrier accepted bytes;
 * a deliberate drop (debug loss, a stalled share) and -EAGAIN both leave it
 * false, which keeps the NACK->wire latency honest. */
static int tbt_fec_out_try(struct tbt_session *sess, struct tbt_fec_out *o,
                           bool *sent, const u8 *usable, u16 ncand,
                           u32 *pace)
{
    struct tbt_carrier *c;
    int i, rc;

    if (sent)
        *sent = false;
    /* The flow may have detached after this record was queued. Drop it
     * instead of putting a dead sid on the wire; the receiver would discard
     * it as an unknown flow. */
    if (o->flow && !tbt_fec_flow_live(READ_ONCE(o->flow->state)))
        return 0;
    if (o->replay)
        return tbt_fec_out_try_replay(sess, o, sent, usable, ncand, pace);
    if (o->control)
    {
        /* Control records (ACK and NACK) may ride any up carrier. Start after
         * the carrier that last accepted one and try every up carrier:
         * pinning the queue to the lowest up carrier lets one congested
         * socket hold every ACK behind its data, and a late ACK is a stalled
         * credit window. */
        struct tbt_fec_tx *tx = o->flow ? o->flow->tx : NULL;
        u16 start = tx ? tx->ctl_rot : 0;
        int rc = -ENODEV;
        int err;

        for (i = 0; i < sess->n_car; i++)
        {
            u16 idx = (u16)((start + i) % sess->n_car);

            c = tbt_carrier_get(sess, (u8)idx);
            if (!c)
                continue;
            if (!atomic_read(&c->up) || atomic_read(&c->stopping))
            {
                tbt_carrier_put(c);
                continue;
            }
            if (READ_ONCE(c->stalled))
            {
                /* A stalled carrier sends no new record, ACKs included. */
                tbt_carrier_put(c);
                continue;
            }
            err = tbt_carrier_send_fec(c, o->data, o->len);
            tbt_carrier_put(c);
            if (!err)
            {
                if (debug && o->data[0] == TBT_FEC_ALLOC)
                    pr_info("tbtun: fec alloc wire carrier=%u len=%u\n",
                            idx, o->len);
                if (tx)
                {
                    tx->ctl_rot = (u16)((idx + 1) % sess->n_car);
                    tx->carrier_wait = false;
                    tx->backpressure_counted = false;
                }
                if (sent)
                    *sent = true;
                return 0;
            }
            /* A dead socket must not end the scan; a full one must not stop
             * it either, because the next carrier may have room. A full
             * carrier anywhere keeps the record queued even if every later
             * one is dead: -EAGAIN wins over -ENODEV. */
            if (err == -EAGAIN)
                rc = -EAGAIN;
        }
        /* No usable carrier. Keep the record queued and ask the replacement
         * path for a carrier: the receiver sends an ACK only when delivery
         * advances, and delivery cannot advance again until this ACK reopens
         * the sender's credit window. The flush re-arms in 2 ms, so the
         * record goes out when a carrier recovers or a replacement appears. */
        if (rc == -ENODEV)
        {
            tbt_fec_wait_carrier(sess, tx);
            rc = -EAGAIN;
        }
        if (debug && o->data[0] == TBT_FEC_ALLOC)
            pr_info_ratelimited("tbtun: fec alloc no carrier rc=%d depth=%u\n",
                                rc, READ_ONCE(sess->fec_out_ctl_depth));
        return rc;
    }
    if (o->carrier >= sess->n_car)
        return -ENODEV;
    c = tbt_carrier_get(sess, (u8)o->carrier);
    /* Debug-only loss injection for a FEC share. It is keyed by the carrier
     * the share was scheduled for, before the fallback below: dropping here
     * models the share the sender handed to a carrier that then died, and it
     * must not eat records that merely fell back to this carrier. The session
     * copy outlives the carrier object, so the injector keeps dropping the
     * same share after a replacement. The sender returns success and frees
     * the record, so only the codec can recover it. */
    {
        u32 drop = READ_ONCE(sess->fec_drop_ppm[o->carrier]);

        if (drop && get_random_u32() % TBT_DROP_PPM_MAX < drop)
        {
            pr_warn_once("tbtun: dropping FEC records: debug_loss=1 and drop_ppm=%u\n",
                         drop);
            sess->dropped_by_ppm++;
            if (c)
                tbt_carrier_put(c);
            return 0;
        }
    }
    if (c && READ_ONCE(c->stalled))
    {
        /* A stalled carrier is paused, not dead: it sends no new record until
         * snd_una moves again. Treat this share as lost. Do not retry it (the
         * queue head would hold every later share behind it) and do not move
         * it to another carrier here (a stall is not a repath, and the share
         * keeps its carrier). The block buffer and the replay ring already
         * hold the real payload, so the receiver's NACK can fetch the share
         * and the parity rows recover it if the NACK is lost. The receiver
         * keeps repairing while a carrier TCP is alive. */
        tbt_carrier_put(c);
        return 0;
    }
    if (c && atomic_read(&c->up) && !atomic_read(&c->stopping))
    {
        struct tbt_fec_tx *tx = o->flow ? o->flow->tx : NULL;
        int err = tbt_carrier_send_fec(c, o->data, o->len);

        tbt_carrier_put(c);
        /* A socket that vanished between the state check and the write is
         * the replacement case: fall through to a live carrier or wait. */
        if (err == -ENOTCONN || err == -ENODEV)
            goto fallback;
        if (tx && (err == 0 || err == -EAGAIN))
        {
            tx->carrier_wait = false;
            tx->backpressure_counted = false;
        }
        if (!err && sent)
            *sent = true;
        return err;
    }
    if (c)
        tbt_carrier_put(c);
fallback:
    /* The share's own carrier is gone or not usable yet. Send it on another
     * carrier instead of dropping it: the receiver places a share by its
     * header, and a dropped share is a block the codec has to recover -- which
     * it cannot always do (measured: 3 of 4 shares plus all 4 parity rows
     * stayed undecodable, and the whole stream stopped behind that block).
     *
     * `up` is the only proof this stream's OPEN is already on that carrier:
     * the ARQ pump writes OPEN, not this FEC drain, and tbt_tx_announce_carrier
     * keeps `up` clear until that write completes. The far drops DATA that
     * arrives before its stream exists (tbt_session_bind_sid), and a share
     * dropped there is consumed here and never NACKed: it must wait. */
    rc = -ENODEV;

    for (i = 0; i < sess->n_car; i++)
    {
        int err;

        if (i == o->carrier)
            continue;
        c = tbt_carrier_get(sess, (u8)i);
        if (!c)
            continue;
        if (!READ_ONCE(c->hello_sent) || !atomic_read(&c->up) ||
            atomic_read(&c->stopping) || READ_ONCE(c->stalled) ||
            !atomic_read(&c->connected))
        {
            tbt_carrier_put(c);
            continue;
        }
        err = tbt_carrier_send_fec(c, o->data, o->len);
        tbt_carrier_put(c);
        if (err == -ENOTCONN || err == -ENODEV)
            continue;
        {
            struct tbt_fec_tx *ftx = o->flow ? o->flow->tx : NULL;

            if (ftx && (err == 0 || err == -EAGAIN))
            {
                ftx->carrier_wait = false;
                ftx->backpressure_counted = false;
            }
        }
        if (!err)
        {
            if (sent)
                *sent = true;
            return 0;
        }
        /* A full carrier must not hide a later one with room. */
        if (err == -EAGAIN)
            rc = -EAGAIN;
        else
            rc = err;
    }
    /* No live carrier to carry this share now. Keep it queued and ask the
     * replacement path for one; the same index carries the same share mapping
     * after a replacement. Never drop it and never move it to another
     * carrier from this path. */
    if (rc == -ENODEV)
    {
        tbt_fec_wait_carrier(sess, o->flow ? o->flow->tx : NULL);
        return -EAGAIN;
    }
    return rc;
}

/* Record the replay bytes one flush pass wrote and publish a decaying
 * bytes/s gauge. The flush runs under pump_lock, so the accumulators have a
 * single writer; /proc reads them with READ_ONCE. */
static void tbt_fec_replay_rate_sample(struct tbt_session *sess, u64 bytes)
{
    u64 now = ktime_get_ns();
    u64 dt;

    sess->fec_replay_drain_bytes += bytes;
    if (!sess->fec_replay_drain_ns)
    {
        sess->fec_replay_drain_ns = now;
        return;
    }
    dt = now - sess->fec_replay_drain_ns;
    if (dt < 100000000ULL) /* one sample per 100 ms */
        return;
    {
        u64 rate = sess->fec_replay_drain_bytes * 1000000000ULL / dt;
        u64 old = atomic64_read(&sess->fec_replay_drain_bps);

        /* EWMA: a stopped drain decays instead of showing the last burst. */
        atomic64_set(&sess->fec_replay_drain_bps,
                     old ? (old * 7 + rate) / 8 : rate);
    }
    sess->fec_replay_drain_bytes = 0;
    sess->fec_replay_drain_ns = now;
}

/* Drain one carrier's bound data queue one record at a time. This is the
 * fallback when the carrier is down or the batch write fails: a record the
 * socket refuses stays at the head and sets *again, and tbt_fec_out_try()
 * can move a dead carrier's share to a live one. No session lock is needed;
 * the carrier write takes its own tx_lock. */
static u32 tbt_fec_out_flush_carrier(struct tbt_session *sess, u16 idx,
                                     bool *again)
{
    struct tbt_fec_outq *q;
    u32 wrote = 0;

    *again = false;
    if (idx >= sess->n_car || !sess->fec_outq)
        return 0;
    q = &sess->fec_outq[idx];
    for (;;)
    {
        struct tbt_fec_out *o;
        u32 depth = 0;
        bool sent = false;
        int err;

        spin_lock(&sess->fec_out_lock);
        o = list_first_entry_or_null(&q->queue, struct tbt_fec_out, node);
        if (o)
        {
            refcount_inc(&o->refs);
            depth = q->depth;
        }
        spin_unlock(&sess->fec_out_lock);
        if (!o)
            break;
        err = tbt_fec_out_try(sess, o, &sent, NULL, 0, NULL);
        if (err == -EAGAIN)
        {
            /* This carrier's socket is full. Only its own work retries. */
            atomic64_inc(&sess->fec_flush_skip);
            atomic64_add(depth, &sess->fec_hol_wait);
            *again = true;
            tbt_fec_out_put(o);
            break;
        }
        spin_lock(&sess->fec_out_lock);
        tbt_fec_out_free(sess, q, o);
        spin_unlock(&sess->fec_out_lock);
        tbt_fec_out_put(o);
        if (sent)
            wrote++;
    }
    if (wrote)
        atomic64_add(wrote, &sess->fec_flush_pass);
    return wrote;
}

/* Collect the head records whose worst-case framed total still fits the
 * carrier's short-write tail. The estimate uses the largest HTTP chunk
 * header, so it is an upper bound for the raw profile too. Returns 0 when
 * the queue is empty. */
static u16 tbt_fec_out_batch_peek(struct tbt_session *sess, u16 idx,
                                  const u8 **recs, u16 *lens,
                                  struct tbt_fec_out **outs)
{
    struct tbt_fec_outq *q;
    struct tbt_fec_out *o;
    size_t bytes = 0;
    u16 n = 0;

    if (idx >= sess->n_car || !sess->fec_outq)
        return 0;
    q = &sess->fec_outq[idx];
    spin_lock(&sess->fec_out_lock);
    list_for_each_entry(o, &q->queue, node)
    {
        size_t framed = (size_t)o->len + TBT_HTTP_CHUNK_HDR_MAX + 2;

        if (n >= TBT_FEC_BATCH_MAX)
            break;
        if (bytes + framed > TBT_FEC_DRAIN_BYTES_MAX)
            break;
        recs[n] = o->data;
        lens[n] = o->len;
        outs[n] = tbt_fec_out_get(o);
        bytes += framed;
        n++;
    }
    spin_unlock(&sess->fec_out_lock);
    return n;
}

/* Remove the records the carrier accepted and release every peek reference.
 * `n` is the peeked count and `sent` the accepted prefix; the caller must not
 * reuse outs[] afterwards. */
static void tbt_fec_out_batch_commit(struct tbt_session *sess, u16 idx,
                                     struct tbt_fec_out **outs, u16 n,
                                     u16 sent)
{
    struct tbt_fec_outq *q = &sess->fec_outq[idx];
    u16 i;

    spin_lock(&sess->fec_out_lock);
    for (i = 0; i < n; i++)
    {
        struct tbt_fec_out *o = outs[i];

        if (i < sent)
        {
            struct tbt_fec_out *head = list_first_entry_or_null(
                &q->queue, struct tbt_fec_out, node);

            if (head == o)
                tbt_fec_out_free(sess, q, o);
        }
        tbt_fec_out_put(o);
    }
    spin_unlock(&sess->fec_out_lock);
}

/* Release peek references when no commit runs. */
static void tbt_fec_out_batch_put(struct tbt_fec_out **outs, u16 n)
{
    u16 i;

    for (i = 0; i < n; i++)
        tbt_fec_out_put(outs[i]);
}

/* A carrier the batch writer may use without falling back. */
static struct tbt_carrier *tbt_fec_out_carrier_ready(struct tbt_session *sess,
                                                     u16 idx)
{
    struct tbt_carrier *c = tbt_carrier_get(sess, (u8)idx);

    if (!c)
        return NULL;
    if (!atomic_read(&c->up) || atomic_read(&c->stopping) ||
        !atomic_read(&c->connected) || READ_ONCE(c->stalled))
    {
        tbt_carrier_put(c);
        return NULL;
    }
    return c;
}

static bool tbt_fec_out_queue_empty(struct tbt_session *sess, u16 idx)
{
    struct tbt_fec_outq *q;
    bool empty;

    if (idx >= sess->n_car || !sess->fec_outq)
        return true;
    q = &sess->fec_outq[idx];
    spin_lock(&sess->fec_out_lock);
    empty = list_empty(&q->queue);
    spin_unlock(&sess->fec_out_lock);
    return empty;
}

/* Drain one batch from one carrier. Returns true when the queue still has a
 * record; *blocked is true when the socket refused the batch and the carrier
 * needs a later retry. Every non-commit path releases the peek references. */
static bool tbt_fec_out_drain_one(struct tbt_session *sess, u16 idx,
                                  bool *blocked)
{
    const u8 *recs[TBT_FEC_BATCH_MAX];
    u16 lens[TBT_FEC_BATCH_MAX];
    struct tbt_fec_out *outs[TBT_FEC_BATCH_MAX];
    struct tbt_carrier *c;
    u16 n, sent = 0;
    bool again = false;
    int err;

    *blocked = false;
    n = tbt_fec_out_batch_peek(sess, idx, recs, lens, outs);
    if (!n)
        return false;
    /* The batch writer hands records straight to the carrier socket, so it
     * bypasses the per-record drop injector in tbt_fec_out_try(). With debug
     * loss armed on this carrier, take the fallback path: the injector must
     * see every share, or a 25% run silently carries zero loss (and the
     * adaptive law never sees the loss it is asked to track). */
    if (READ_ONCE(sess->fec_drop_ppm[idx]))
    {
        tbt_fec_out_batch_put(outs, n);
        tbt_fec_out_flush_carrier(sess, idx, &again);
        *blocked = again;
        return !tbt_fec_out_queue_empty(sess, idx);
    }
    c = tbt_fec_out_carrier_ready(sess, idx);
    if (!c)
    {
        /* The carrier is down or stalled: the per-record path can fall back
         * to another carrier and owns the drop policy. */
        tbt_fec_out_batch_put(outs, n);
        tbt_fec_out_flush_carrier(sess, idx, &again);
        *blocked = again;
        return !tbt_fec_out_queue_empty(sess, idx);
    }
    err = tbt_carrier_send_fec_batch(c, recs, lens, n, &sent);
    tbt_carrier_put(c);
    if (!err)
    {
        atomic64_add(sent, &sess->fec_flush_pass);
        tbt_fec_out_batch_commit(sess, idx, outs, n, sent);
        if (sent != n)
        {
            /* All-or-nothing is the batch contract; keep the rest for a
             * bounded retry instead of exiting with a non-empty queue. */
            *blocked = true;
            return true;
        }
        return !tbt_fec_out_queue_empty(sess, idx);
    }
    if (err == -EAGAIN)
    {
        atomic64_inc(&sess->fec_flush_skip);
        atomic64_add(n, &sess->fec_hol_wait);
        tbt_fec_out_batch_put(outs, n);
        *blocked = true;
        return true;
    }
    /* A dead socket or a local framing error: the per-record path owns the
     * fallback and the drop policy. */
    tbt_fec_out_batch_put(outs, n);
    tbt_fec_out_flush_carrier(sess, idx, &again);
    *blocked = again;
    return !tbt_fec_out_queue_empty(sess, idx);
}

/* Serve up to `cap` carriers from the ready bitmap, round-robin. A carrier
 * that refused its batch moves to the retry bitmap; one that still has data
 * keeps its ready bit and is served again by the next run without delay.
 * Fresh and blocked bits never share a bitmap, so a full socket cannot push
 * a fresh carrier into its retry timer. */
static void tbt_fec_out_drain_ready(struct tbt_session *sess, u16 cap)
{
    u16 pass;

    for (pass = 0; pass < cap; pass++)
    {
        unsigned long idx;
        bool one_blocked, one_more;

        idx = find_next_bit(sess->fec_drain_ready, sess->n_car,
                            sess->fec_drain_cursor);
        if (idx >= sess->n_car)
        {
            sess->fec_drain_cursor = 0;
            idx = find_first_bit(sess->fec_drain_ready, sess->n_car);
            if (idx >= sess->n_car)
                break;
        }
        clear_bit(idx, sess->fec_drain_ready);
        sess->fec_drain_cursor = (u16)((idx + 1) % sess->n_car);
        one_more = tbt_fec_out_drain_one(sess, (u16)idx, &one_blocked);
        if (one_blocked)
        {
            /* The first refusal sets the deadline; later ones in the same
             * run share it, and a fresh run never shortens it. */
            if (bitmap_empty(sess->fec_drain_retry, sess->n_car))
                sess->fec_retry_at = jiffies + msecs_to_jiffies(2);
            set_bit(idx, sess->fec_drain_retry);
        }
        else if (one_more)
        {
            set_bit(idx, sess->fec_drain_ready);
        }
    }
}

/* The retry timer gates blocked carriers only. When it fires, their bits
 * rejoin the ready bitmap and the bounded loop serves them; while it has
 * not fired, fresh bits are served immediately. A full set of carriers
 * therefore retries once per timer tick instead of spinning. */
static void tbt_fec_out_drain_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, fec_drain_work);
    u16 cap;

    if (!sess->fec || !sess->n_car || READ_ONCE(sess->fec_stopping))
        return;
    if (!bitmap_empty(sess->fec_drain_retry, sess->n_car) &&
        time_after_eq(jiffies, sess->fec_retry_at))
    {
        unsigned long idx;

        /* Move bit by bit: kick() sets a ready bit concurrently, and a
         * whole-word bitmap_or() could overwrite that atomic update. */
        for_each_set_bit(idx, sess->fec_drain_retry, sess->n_car)
        {
            set_bit(idx, sess->fec_drain_ready);
            clear_bit(idx, sess->fec_drain_retry);
        }
        sess->fec_retry_at = 0;
    }

    cap = (u16)min_t(unsigned int, sess->n_car, TBT_FEC_DRAIN_PASS_MAX);
    tbt_fec_out_drain_ready(sess, cap);

    if (!bitmap_empty(sess->fec_drain_ready, sess->n_car))
        mod_delayed_work(TBT_WQ, &sess->fec_drain_work, 0);
    else if (!bitmap_empty(sess->fec_drain_retry, sess->n_car))
    {
        mod_delayed_work(TBT_WQ, &sess->fec_drain_work,
                         max_t(long, 1,
                               (long)(sess->fec_retry_at - jiffies)));
        /* A kick can land between the ready check and this arm, and its
         * own 0-delay mod would lose to the later retry arm. Re-check so
         * the fresh bit always wins; a later kick re-arms itself after
         * this point and needs no help. */
        if (!bitmap_empty(sess->fec_drain_ready, sess->n_car))
            mod_delayed_work(TBT_WQ, &sess->fec_drain_work, 0);
    }
}

/* Write the session-wide queues that can make progress: control (ACK/NACK,
 * any up carrier) and replay (any up carrier, paced by queue bytes). The
 * bound data queues drain in their per-carrier works, so a full socket here
 * never holds an ACK, a NACK, a repair, or the packetizer. The session pump
 * lock still serialises these writes with the old control path
 * (HELLO/OPEN/FIN/PAD), so a record is never spliced into another record's
 * bytes on one carrier. */
static int tbt_fec_out_flush(struct tbt_session *sess, u32 *written)
{
    int ret = 0;
    u32 pass = 0;
    u32 replay_sent = 0;
    u64 replay_bytes = 0;
    bool replay_more = false;
    u16 i;

    if (written)
        *written = 0;
    mutex_lock(&sess->pump_lock);
    /* Pass 1: control. */
    if (READ_ONCE(sess->fec_out_ctl_depth))
    {
        for (;;)
        {
            struct tbt_fec_out *o;
            bool sent = false;
            int err;

            spin_lock(&sess->fec_out_lock);
            o = list_first_entry_or_null(&sess->fec_out_ctl,
                                         struct tbt_fec_out, node);
            if (o)
                refcount_inc(&o->refs);
            spin_unlock(&sess->fec_out_lock);
            if (!o)
                break;
            err = tbt_fec_out_try(sess, o, &sent, NULL, 0, NULL);
            if (err == -EAGAIN)
            {
                ret = -EAGAIN;
                tbt_fec_out_put(o);
                break;
            }
            spin_lock(&sess->fec_out_lock);
            tbt_fec_out_free(sess, NULL, o);
            spin_unlock(&sess->fec_out_lock);
            tbt_fec_out_put(o);
            if (sent)
                pass++;
        }
    }
    /* Pass 2: replay. The usable set is built once; every write still
     * rechecks its carrier, so a socket that dies mid-pass is skipped. */
    if (READ_ONCE(sess->fec_replay_queued))
    {
        u8 *usable = sess->fec_replay_usable;
        u32 *pace = sess->fec_replay_pace;
        u16 ncand = tbt_fec_replay_usable(sess, usable);
        u32 cap = tbt_fec_replay_cap(READ_ONCE(sess->fec_closing),
                                     TBT_FEC_REPLAY_BYTES_MAX);
        u32 high = tbt_fec_replay_high_water(cap);
        u32 bytes, limit;
        bool over;

        spin_lock(&sess->fec_out_lock);
        for (i = 0; i < sess->n_car; i++)
            pace[i] = sess->fec_outq[i].bytes;
        bytes = sess->fec_replay_bytes;
        spin_unlock(&sess->fec_out_lock);
        over = bytes > high;
        if (over != READ_ONCE(sess->fec_replay_over_high))
        {
            WRITE_ONCE(sess->fec_replay_over_high, over);
            if (over)
                atomic64_inc(&sess->fec_replay_high_water);
        }
        /* Above the mark one replay record per pass keeps new DATA moving;
         * replay still drains, and the TX work is re-armed immediately. */
        limit = tbt_fec_replay_pass_limit(over, TBT_FEC_REPLAY_PASS_MAX);
        if (over)
            atomic64_inc(&sess->fec_replay_pass_throttled);
        for (;;)
        {
            struct tbt_fec_out *o;
            u32 depth = 0;
            bool sent = false;
            u64 stamp_ns = 0;
            u64 block_id = 0;
            u16 olen = 0;
            int err;

            spin_lock(&sess->fec_out_lock);
            o = list_first_entry_or_null(&sess->fec_out_replay,
                                         struct tbt_fec_out, node);
            if (o)
            {
                refcount_inc(&o->refs);
                depth = sess->fec_replay_queued;
            }
            stamp_ns = o ? o->stamp_ns : 0;
            block_id = o ? o->block_id : 0;
            olen = o ? o->len : 0;
            spin_unlock(&sess->fec_out_lock);
            if (!o)
                break;
            if (block_id && o->flow && o->flow->tx)
            {
                struct tbt_fec_tx *rtx = o->flow->tx;
                bool stale = tbt_fec_block_delivered_base(
                    block_id, READ_ONCE(rtx->peer_pkts), rtx->base_pkt,
                    rtx->base_block, rtx->groups, rtx->n);

                if (stale)
                {
                /* The delivered frontier passed this block while the repair
                 * waited in the queue: every share in it is a duplicate of
                 * data the peer already has. Free it instead of writing it,
                 * so the tail repair and the FIN are not stuck behind it. */
                    spin_lock(&sess->fec_out_lock);
                    tbt_fec_out_free(sess, NULL, o);
                    spin_unlock(&sess->fec_out_lock);
                    tbt_fec_out_put(o);
                    atomic64_inc(&sess->fec_replay_stale);
                    continue;
                }
            }
            err = tbt_fec_out_try(sess, o, &sent, usable, ncand, pace);
            if (err == -EAGAIN)
            {
                /* No carrier has room for this repair now. It keeps its
                 * place; the bound data queues still drain below. */
                atomic64_inc(&sess->fec_replay_skip);
                atomic64_add(depth, &sess->fec_hol_wait);
                ret = -EAGAIN;
                tbt_fec_out_put(o);
                break;
            }
            spin_lock(&sess->fec_out_lock);
            tbt_fec_out_free(sess, NULL, o);
            spin_unlock(&sess->fec_out_lock);
            tbt_fec_out_put(o);
            if (sent)
            {
                replay_bytes += olen;
                /* Charge NACK->wire, not NACK->enqueue: the repair is
                 * complete only when a carrier accepted the record. */
                u64 now = ktime_get_ns();
                u64 dt = now > stamp_ns ? now - stamp_ns : 0;

                atomic64_add(dt, &sess->fec_nack_lat_ns);
                tbt_fec_stat_max(&sess->fec_nack_lat_max_ns, dt);
                pass++;
                if (++replay_sent >= limit)
                {
                    /* Give the packetizer a turn; the TX work is re-armed
                     * below while replay records remain. */
                    replay_more = true;
                    break;
                }
            }
            else
            {
                atomic64_inc(&sess->fec_replay_drop);
            }
        }
    }
    else if (READ_ONCE(sess->fec_replay_over_high))
    {
        /* The queue emptied: the next burst counts as a new episode. */
        WRITE_ONCE(sess->fec_replay_over_high, false);
    }
    tbt_fec_replay_rate_sample(sess, replay_bytes);
    mutex_unlock(&sess->pump_lock);
    if (replay_more && READ_ONCE(sess->fec_replay_queued))
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    if (pass)
        atomic64_add(pass, &sess->fec_flush_pass);
    if (written)
        *written = pass;
    return ret;
}

/* Queue one complete record. Control records may ride any up carrier; data
 * records are bound to their share's carrier. Returns 0 when queued. */
static int tbt_fec_send_record_flow(struct tbt_session *sess, u16 carrier,
                                    u8 *buf, u16 len, bool control,
                                    bool replay, struct tbt_fec_flow *flow)
{
    return tbt_fec_out_push(sess, carrier, buf, len, control, replay, 0,
                            flow);
}

static int tbt_fec_send_record(struct tbt_session *sess, u16 carrier, u8 *buf,
                               u16 len, bool control, bool replay,
                               struct tbt_fec_flow *flow)
{
    return tbt_fec_send_record_flow(sess, carrier, buf, len, control, replay,
                                    flow);
}

/* Carriers that may carry a replay record right now. The share header names
 * its slot, so a replay is not bound to the carrier that would carry the
 * source share; the set is built once per flush pass instead of once per
 * record. */
static u16 tbt_fec_replay_usable(struct tbt_session *sess, u8 *usable)
{
    u16 i, count = 0;

    memset(usable, 0, TBT_MAX_CARRIERS_LIMIT);
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);

        if (!c)
            continue;
        if (atomic_read(&c->up) && atomic_read(&c->connected) &&
            !atomic_read(&c->stopping) && !READ_ONCE(c->stalled))
        {
            usable[i] = 1;
            count++;
        }
        tbt_carrier_put(c);
    }
    return count;
}

/* The usable carrier with the fewest queued bytes, or U16_MAX when none is
 * usable. `pace` starts as each carrier's bound-queue bytes and grows by the
 * replay bytes already written in this pass, so one NACK's 256 shares spread
 * over the carriers instead of filling the first socket. */
static u16 tbt_fec_replay_pick(struct tbt_session *sess, const u8 *usable,
                               const u32 *pace)
{
    u16 best = U16_MAX;
    u32 best_bytes = U32_MAX;
    u16 i;

    for (i = 0; i < sess->n_car; i++)
    {
        u32 bytes;

        if (!usable[i])
            continue;
        bytes = pace[i];
        if (bytes < best_bytes)
        {
            best_bytes = bytes;
            best = i;
        }
    }
    return best;
}

/* Queue one replay record on the session replay queue. The carrier is chosen
 * at write time by queue bytes; `nack_ns` is when the NACK arrived and the
 * flush charges NACK->wire when a carrier accepts the record. */
static int tbt_fec_replay_enqueue(struct tbt_session *sess, const u8 *buf,
                                  u16 len, u64 nack_ns,
                                  struct tbt_fec_flow *flow)
{
    int err = tbt_fec_out_push(sess, 0, buf, len, false, true, nack_ns,
                               flow);

    if (!err)
        WRITE_ONCE(sess->fec_repair_last_ns, ktime_get_ns());
    return err;
}

/* Replay bytes the 8 MiB queue can still take. The NACK handler checks this
 * before queueing a record, so a full queue stops the reply at a clean point
 * instead of letting tbt_fec_out_push() refuse it with -ENOSPC: the receiver
 * retries after its hold, and no share is counted as an allocation failure. */
static u32 tbt_fec_replay_space(struct tbt_session *sess)
{
    u32 cap = tbt_fec_replay_cap(READ_ONCE(sess->fec_closing),
                                 TBT_FEC_REPLAY_BYTES_MAX);
    u32 space;

    spin_lock(&sess->fec_out_lock);
    space = sess->fec_replay_bytes < cap ? cap - sess->fec_replay_bytes : 0;
    spin_unlock(&sess->fec_out_lock);
    return space;
}

static int tbt_fec_make_record(struct tbt_session *sess, struct tbt_fec_hdr *h,
                               const u8 *payload, u8 *out, u16 *out_len)
{
    if (tbt_fec_hdr_seal(h, payload))
        return -EINVAL;
    if (tbt_fec_hdr_pack(h, out))
        return -EINVAL;
    if (h->payload_len)
        memcpy(out + TBT_FEC_HDR_LEN, payload, h->payload_len);
    /* The payload CRC above covers the clear bytes (T0 §2.2). Only DATA and
     * PARITY are transformed; the 48-byte header stays clear. The TX work is
     * serialized and only runs once the TX context is ready. */
    if (sess->crypto &&
        (h->type == TBT_FEC_DATA || h->type == TBT_FEC_PARITY))
    {
        if (!sess->crypto_ctx.tx.ready)
            return -EINVAL;
        if (tbt_crypto_body_xor_flow(&sess->crypto_ctx.tx, h->sid, h->seq,
                                     h->type == TBT_FEC_PARITY,
                                     h->share_index, out + TBT_FEC_HDR_LEN,
                                     h->payload_len))
            return -EINVAL;
    }
    *out_len = (u16)(TBT_FEC_HDR_LEN + h->payload_len);
    return 0;
}

/* ------------------------------------------------------------------ *
 * TX
 * ------------------------------------------------------------------ */

static u32 tbt_fec_ring_put(struct tbt_fec_tx *tx, struct sk_buff *skb,
                            unsigned int off, u32 len)
{
    u32 free = tx->ring_size - (tx->head - tx->tail);
    u32 take = min(len, free);
    u32 first = min(take, tx->ring_size - (tx->head % tx->ring_size));
    u32 pos = tx->head % tx->ring_size;

    if (!take)
        return 0;
    if (skb_copy_bits(skb, off, tx->ring + pos, first))
        return 0;
    if (take > first &&
        skb_copy_bits(skb, off + first, tx->ring, take - first))
        return 0;
    tx->head += take;
    return take;
}

static u32 tbt_fec_ring_get(struct tbt_fec_tx *tx, u8 *dst, u32 len)
{
    u32 avail = tx->head - tx->tail;
    u32 take = min(avail, len);
    u32 first = min(take, tx->ring_size - (tx->tail % tx->ring_size));
    u32 pos = tx->tail % tx->ring_size;

    if (!take)
        return 0;
    memcpy(dst, tx->ring + pos, first);
    if (take > first)
        memcpy(dst + first, tx->ring, take - first);
    tx->tail += take;
    return take;
}

/* Encode one group block and send the parity shares the codec needs.
 *
 * win_len is the number of real DATA packets in the block: n for a full
 * block, less for a block the short-write path closed early. The unused
 * positions are already zero-filled. A full block sends all m rows; a short
 * SC-LDPC block sends row 0 alone, which states win_len/epoch so the receiver
 * stops waiting for shares the sender never wrote, and leaves the few real
 * shares to the NACK replay (see tbt_fec_tx_parity_count()). */
static void tbt_fec_tx_emit_parity(struct tbt_session *sess,
                                   struct tbt_fec_tx *tx, u16 group,
                                   u64 block_id, u16 win_len)
{
    struct tbt_fec_gtx *g = &tx->g[group];
    struct tbt_fec_hdr h;
    bool block_code = tbt_fec_grouped_mode(tx->codec) == 3;
    u64 last_seq;
    u16 count, rec_len, i;
    int st;
    int err;

    if (!tx->codec || !win_len || win_len > tx->n)
        return;
    count = tbt_fec_tx_parity_count(win_len, tx->n, tx->m, block_code);
    if (!count)
        return;
    tbt_fec_grouped_set_nonce(tx->codec, group,
                              tbt_fec_group_nonce(tx->nonce, group, block_id));
    for (i = 0; i < tx->n; i++)
        tx->data_ptrs[i] = g->blk + (size_t)i * TBT_FEC_S;
    for (i = 0; i < tx->m; i++)
        tx->parity_ptrs[i] = g->par + (size_t)i * TBT_FEC_S;
    if (tbt_fec_grouped_encode(tx->codec, group, tx->data_ptrs, tx->n,
                               tx->parity_ptrs) < 0)
    {
        /* No parity means the peer can only repair this block by NACKing the
         * shares themselves; name it instead of failing silently. */
        atomic64_inc(&sess->fec_encode_fail);
        pr_warn_ratelimited("tbtun: fec parity encode failed block=%llu group=%u n=%u m=%u mode=%u\n",
                            block_id, group, tx->n, tx->m,
                            tbt_fec_grouped_mode(tx->codec));
        return;
    }
    last_seq = tbt_fec_seq_of_block_pkt_base(
        block_id, group, (u16)(win_len - 1), tx->base_pkt, tx->base_block,
        tx->groups, tx->n);
    for (i = 0; i < count; i++)
    {
        memset(&h, 0, sizeof(h));
        h.type = TBT_FEC_PARITY;
        h.share_index = (u16)(tx->n + i);
        h.n = (u8)tx->n;
        h.m = (u8)tx->m;
        h.block_id = block_id;
        h.seq = last_seq;
        h.win_len = win_len;
        h.payload_len = TBT_FEC_S;
        h.epoch = (u16)(g->last_len && g->last_len < TBT_FEC_S ? g->last_len : 0);
        h.conv = TBT_FEC_CONV;
        h.sid = tx->sid;
        if (tbt_fec_make_record(sess, &h, tx->parity_ptrs[i], tx->rec, &rec_len))
            continue;
        WRITE_ONCE(tx->last_group, group);
        WRITE_ONCE(tx->last_share, (u16)(tx->n + i));
        st = -EINVAL;
        if (tx->replay)
        {
            spin_lock_bh(&tx->replay_lock);
            st = tbt_fec_replay_store(tx->replay, block_id, group,
                                      (u16)(tx->n + i), tx->rec, rec_len);
            spin_unlock_bh(&tx->replay_lock);
        }
        err = tbt_fec_send_record_flow(
            sess,
            (u16)(tx->group_map[group] * (tx->n + tx->m) + tx->n + i),
            tx->rec, rec_len, false, false, tx->flow);
        if (debug >= 2)
            pr_info("tbtun: fec parity tx block=%llu group=%u parity=%u win_len=%u epoch=%u store=%d send=%d\n",
                    block_id, group, i, win_len, h.epoch, st, err);
        if (err)
        {
            /* The ring keeps the parity; the peer's NACK probe can replay it.
             * Re-arm so a transient queue failure is not silent. */
            tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
        }
    }
    if (count < tx->m)
        atomic64_add((u64)(tx->m - count), &sess->fec_short_parity_saved);
}

/* One PARITY record that says "this group has no packet in this block".
 * Without it the receiver waits forever for a block that was never sent: a
 * group with no real share emits no parity, and the super-block walk requires
 * every group's block to be ready. The marker carries no payload, so crypto
 * has nothing to transform and the record is header-only. */
static void tbt_fec_tx_emit_empty(struct tbt_session *sess,
                                  struct tbt_fec_tx *tx, u16 group,
                                  u64 block_id)
{
    struct tbt_fec_hdr h;
    u16 rec_len;
    int err;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_PARITY;
    h.share_index = tx->n; /* parity 0 */
    h.n = (u8)tx->n;
    h.m = (u8)tx->m;
    h.block_id = block_id;
    h.seq = tbt_fec_seq_of_block_pkt_base(block_id, group, 0, tx->base_pkt,
                                          tx->base_block, tx->groups, tx->n);
    h.win_len = 0;
    h.payload_len = 0;
    h.epoch = 0;
    h.conv = TBT_FEC_CONV;
    h.sid = tx->sid;
    if (tbt_fec_make_record(sess, &h, NULL, tx->rec, &rec_len))
        return;
    WRITE_ONCE(tx->last_group, group);
    WRITE_ONCE(tx->last_share, tx->n);
    /* A marker is not stored in the replay ring: it is a statement about a
     * block, not a share of it, and replaying an old marker for a block the
     * receiver has already settled was one source of the duplicate-marker
     * protocol errors. A NACK for share n gets a fresh marker from
     * tbt_fec_send_empty_marker() instead. */
    if (debug >= 2)
        pr_info("tbtun: fec empty tx block=%llu group=%u share=%u\n",
                block_id, group, tx->n);
    /* A marker is control-like: it must ride any up carrier. The parity-0
     * carrier may be stalled or full, and the data path would drop the
     * marker as a stalled share. */
    err = tbt_fec_send_record(sess, 0, tx->rec, rec_len, true, false,
                              tx->flow);
    if (err)
    {
        if (debug >= 2)
            pr_info("tbtun: fec empty tx block=%llu group=%u send=%d; retry\n",
                    block_id, group, err);
        /* The ring keeps the marker; the peer's NACK probe can replay it.
         * Re-arm the TX work so a transient queue failure gets another
         * pass instead of staying silent. */
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
    }
    atomic64_inc(&sess->fec_empty_block);
}

/* End the block under construction. Every group that wrote packets into it
 * gets parity over its zero-filled tail with win_len = its real packet count,
 * which is how the receiver learns the block is short; the parity also states
 * the last real share's length in `epoch`, so a receiver that has to recover
 * that share still knows how long it was.
 *
 * A group with no packet in this block emits an empty-block marker instead of
 * silence, so the super-block walk can complete. A group whose block already
 * filled emitted its parity when the last share was queued: nreal == 0 and
 * blk_id == block_id is that state, not "empty".
 *
 * `advance` moves the packet numbering to the first packet of the next block,
 * for a block closed before it filled. The jump is block-aligned: every group
 * starts the next block at share 0, which keeps `nreal = share + 1` and the
 * parity's last_seq valid. The old jump kept `seq % groups` and landed in the
 * middle of a block, so a group before the landing point skipped share 0
 * while packetize() still counted the skipped slots as real packets. */
static void tbt_fec_tx_close_block(struct tbt_session *sess,
                                   struct tbt_fec_tx *tx, bool advance)
{
    u64 block_id = tx->cur_block;
    u16 group;

    for (group = 0; group < tx->groups; group++)
    {
        struct tbt_fec_gtx *g = &tx->g[group];
        u16 len = g->nreal;

        if (debug >= 2)
            pr_info("tbtun: fec close block=%llu group=%u gblk=%llu nreal=%u path=%s\n",
                    block_id, group, g->blk_id, len,
                    g->blk_id != block_id ? "empty" :
                    (len && len < tx->n ? "parity" : "skip"));
        if (g->blk_id != block_id)
        {
            tbt_fec_tx_emit_empty(sess, tx, group, block_id);
            continue;
        }
        if (!len || len >= tx->n)
            continue; /* full: parity already sent */
        memset(g->blk + (size_t)len * TBT_FEC_S, 0,
               (size_t)(tx->n - len) * TBT_FEC_S);
        tbt_fec_tx_emit_parity(sess, tx, group, block_id, len);
        g->nreal = 0;
    }
    if (advance)
    {
        tx->seq = tbt_fec_seq_of_block_pkt_base(
            block_id + 1, 0, 0, tx->base_pkt, tx->base_block, tx->groups,
            tx->n);
        tx->cur_block = block_id + 1;
    }
}

/* Flush the stream: every group whose final block is short gets parity over
 * its zero-filled tail, so a lost real share is still recoverable and the
 * receiver learns the real length from win_len. Groups whose final block is
 * full already sent their parity when the block completed; a group with no
 * packet in the final block sends a marker.
 *
 * pkt_end is frozen before the jump: it names the packet after the stream's
 * last DATA packet, and the FIN body carries it so a receiver whose LAST or
 * parity was lost still knows where the stream ends. */
static void tbt_fec_tx_finish(struct tbt_session *sess,
                              struct tbt_fec_tx *tx, struct tbt_stream *st)
{
    u64 block_start;

    if (READ_ONCE(st->fec_finished))
        return;
    WRITE_ONCE(st->fec_pkt_end, tx->seq);
    /* close_block() advances tx->seq to the next block even when the last
     * block was already closed by the short-packet path. Calling it again on
     * an empty next block would emit a full set of empty markers for a block
     * that never carried a packet (field: fec_empty_block=63 for a 198-byte
     * request, i.e. 31 group markers plus a second all-empty block). Only
     * close a block that actually received a packet. */
    block_start = tbt_fec_seq_of_block_pkt_base(
        tx->cur_block, 0, 0, tx->base_pkt, tx->base_block, tx->groups,
        tx->n);
    if (st->fec_pkt_base_set && tx->seq > block_start)
        tbt_fec_tx_close_block(sess, tx, true);
    smp_store_release(&st->fec_finished, true);
    /* The close path's tbt_tx_fin() retry is driven by w_fbk. If the tick
     * slept while the session had no stream, wake it now that the final block
     * is finished and the FIN can be named. */
    tbt_fbk_kick(sess, false);
}

/* The replay ring is the largest per-flow object: on the 256-carrier shape K
 * lands on 7 and the record plane alone is ~14.1 MiB. Only a sender needs it.
 * Allocating it with the first DATA packet keeps a receive-only session from
 * reserving a repair window it will never read. The packetizer runs in
 * w_fec_tx (process context), so vzalloc() may sleep here. Failure is not
 * fatal: the caller stops and retries, preserving the old "no repair ring,
 * no DATA" contract instead of quietly losing repair ability. */
static bool tbt_fec_tx_replay_ensure(struct tbt_session *sess,
                                     struct tbt_fec_tx *tx)
{
    struct tbt_fec_replay *r;

    if (READ_ONCE(tx->replay))
        return true;
    r = tbt_fec_replay_new_k(tx->groups, tx->n, tx->m, TBT_FEC_S,
                             sess->fec_replay_k);
    if (!r)
    {
        atomic64_inc(&sess->fec_replay_miss_no_ring);
        return false;
    }
    spin_lock_bh(&tx->replay_lock);
    if (tx->replay)
    {
        spin_unlock_bh(&tx->replay_lock);
        tbt_fec_replay_free(r);
        return true;
    }
    WRITE_ONCE(tx->replay, r);
    spin_unlock_bh(&tx->replay_lock);
    return true;
}

static void tbt_fec_packetize(struct tbt_session *sess, struct tbt_fec_tx *tx,
                              struct tbt_stream *st, bool last)
{
    struct tbt_fec_hdr h;
    u16 rec_len, group, share, len, carrier;
    u64 seq, block_id;
    u8 *rec = tx->rec;

    if (!tx->fill && !last)
        return;
    /* An FEC_ALLOC switch owns the base from P0 until its swap or abort. */
    if (tx->alloc_hold && tx->seq >= tx->alloc_target.pkt)
        return;
    /* DRAIN stops new-shape bytes at P0. The old block was already closed
     * when the switch began, so tx->seq == P0 here; a retry after a failed
     * CFG keeps the same stop. */
    if ((tx->switch_state == TBT_FEC_SW_CFG_SENT ||
         tx->switch_state == TBT_FEC_SW_DRAIN) &&
        tx->seq >= tx->switch_pkt)
        return;
    len = tx->fill;
    seq = tx->seq++;
    group = tbt_fec_group_of_seq_base(seq, tx->base_pkt, tx->groups);
    share = tbt_fec_share_of_seq_base(seq, tx->base_pkt, tx->groups, tx->n);
    block_id = tbt_fec_block_of_seq_base(seq, tx->base_pkt, tx->base_block,
                                         tx->groups, tx->n);
    carrier = (u16)(tx->group_map[group] * (tx->n + tx->m) + share);

    /* The first packet of a stream fixes its absolute base; the FIN body
     * carries it so the peer can turn its global packet frontier into a
     * stream-relative byte offset. */
    if (!st->fec_pkt_base_set)
    {
        WRITE_ONCE(st->fec_pkt_base, seq);
        /* Publish the value before the flag: the ACK path reads the flag with
         * acquire and then the base, so it never sees a set flag with a stale
         * zero base. */
        smp_store_release(&st->fec_pkt_base_set, true);
    }
    /* A group's nreal/last_len belong to one block. Entering a new block
     * resets them; a share that is not the next one means the sender skipped
     * a slot (the early-close jump used to do exactly that), and the parity
     * would then describe a hole. */
    if (tx->g[group].blk_id != block_id)
    {
        tx->g[group].blk_id = block_id;
        tx->g[group].nreal = 0;
        tx->g[group].last_len = 0;
    }
    if (share != tx->g[group].nreal)
        atomic64_inc(&sess->fec_tx_share_gap);

    tx->short_at_ns = 0;
    if (len < TBT_FEC_S)
        atomic64_inc(&sess->fec_short_tx);
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_DATA;
    h.flags = last ? TBT_FEC_FF_LAST : 0;
    h.share_index = share;
    h.n = (u8)tx->n;
    h.m = (u8)tx->m;
    h.block_id = block_id;
    h.seq = seq;
    h.win_len = 0;
    h.payload_len = len;
    /* epoch is block-level and belongs to PARITY only: a mid-block short
     * share that carried it overwrote the block's last-share length. */
    h.epoch = 0;
    h.conv = TBT_FEC_CONV;
    h.sid = tx->sid;
    if (!tbt_fec_make_record(sess, &h, tx->pkt, rec, &rec_len))
    {
        WRITE_ONCE(tx->last_group, group);
        WRITE_ONCE(tx->last_share, share);
        if (tx->replay)
        {
            spin_lock_bh(&tx->replay_lock);
            tbt_fec_replay_store(tx->replay, block_id, group, share,
                                 rec, rec_len);
            spin_unlock_bh(&tx->replay_lock);
        }
        (void)tbt_fec_send_record_flow(sess, carrier, rec, rec_len, false,
                                       false, tx->flow);
        tx->data_pkts_sent++;
    }
    /* The block buffer must hold the real byte even when the share did not
     * reach its carrier: parity then describes the truth, and the receiver
     * can recover the dropped share instead of decoding zeros. */
    memcpy(tx->g[group].blk + (size_t)share * TBT_FEC_S, tx->pkt, len);
    if (len < TBT_FEC_S)
        memset(tx->g[group].blk + (size_t)share * TBT_FEC_S + len, 0,
               TBT_FEC_S - len);
    tx->fill = 0;
    tx->cur_block = block_id;
    tx->g[group].nreal = (u16)(share + 1);
    tx->g[group].last_len = len;

    if (share + 1 == tx->n)
    {
        tbt_fec_tx_emit_parity(sess, tx, group, block_id, tx->g[group].nreal);
        tx->g[group].nreal = 0;
    }
    if (last)
        tbt_fec_tx_finish(sess, tx, st);
}

/* The peer's ACK grants `credit` packets beyond the frontier it has delivered.
 * The receiver buffers `slots * groups * n` packets ahead of delivery, and the
 * credit is the same number, so a sender that respects it can never outrun the
 * buffer. Before this was enforced the packetizer ran as far ahead as the
 * carriers would carry it, the receiver dropped 1527 shares as "too far ahead"
 * (fec_drop_window), and the blocks they belonged to could never complete: the
 * stream stalled with the application's bytes still in the ring. */
static bool tbt_fec_tx_credit_ok(const struct tbt_fec_tx *tx)
{
    u64 acked = READ_ONCE(tx->peer_pkts);
    u64 seq = READ_ONCE(tx->seq);
    u32 credit = READ_ONCE(tx->credit);

    /* Zero credit is a closed window, not an open one. The receiver always
     * grants at least one packet, so zero means no grant has arrived (or a
     * bug); sending on it would overrun the receiver's block slots and the
     * shares would come back as fec_drop_window. */
    if (!credit)
        return false;
    /* An ACK names packets the receiver has delivered, so it can never be
     * ahead of what was sent. Clamp a nonsensical frontier instead of letting
     * the unsigned subtraction wrap into a huge outstanding count. */
    if (acked >= seq)
        return true;
    return seq - acked < (u64)credit;
}

/* Any bound data queue at or above its high-water mark. The packetizer
 * checks this before it builds the next packet: pausing there pushes the
 * pressure into the capture ring, which pauses the socket once full
 * (tbt_fec_tx_submit), so the sender never drops a share it would have to
 * repair with a NACK round trip. The hard caps in tbt_fec_out_push() remain
 * as the last line of defence. */
static bool tbt_fec_tx_outq_high(struct tbt_session *sess)
{
    bool high = false;
    u16 i;

    if (!sess->fec_outq)
        return false;
    spin_lock(&sess->fec_out_lock);
    if (sess->fec_outq_queued >= TBT_FEC_OUTQ_QUEUE_HIGH ||
        sess->fec_outq_bytes >= TBT_FEC_OUTQ_QUEUE_BYTES_HIGH)
        high = true;
    else if (sess->fec_replay_bytes >=
             tbt_fec_replay_cap(READ_ONCE(sess->fec_closing),
                                TBT_FEC_REPLAY_BYTES_MAX) / 2)
        /* Half the ring is a repair backlog the receiver is waiting on; more
         * new DATA only competes with the repair for the same sockets. Stop
         * the packetizer until the queue drains below the mark. The flush
         * high-water (75%) still throttles the drain itself. */
        high = true;
    else
    {
        for (i = 0; i < sess->n_car; i++)
        {
            struct tbt_fec_outq *q = &sess->fec_outq[i];

            if (q->depth >= TBT_FEC_OUTQ_HIGH_DEPTH ||
                q->bytes >= TBT_FEC_OUTQ_HIGH_BYTES)
            {
                high = true;
                break;
            }
        }
    }
    spin_unlock(&sess->fec_out_lock);
    return high;
}

static bool tbt_fec_tx_is_idle(struct tbt_session *sess,
                               struct tbt_fec_tx *tx)
{
    bool pkt_idle;
    u32 queued;

    spin_lock(&tx->lock);
    pkt_idle = tx->head == tx->tail && tx->fill == 0 &&
               !READ_ONCE(tx->flush_req);
    spin_unlock(&tx->lock);
    spin_lock(&sess->fec_out_lock);
    queued = tx->queued;
    spin_unlock(&sess->fec_out_lock);
    /* Only new DATA orders the FIN. Replay records are duplicates of shares
     * already sent once and control records carry no stream bytes, so an
     * 8 MiB replay backlog must not hold the close (fix20: close_timeouts=1
     * with 3.98 MB left). */
    return tbt_fec_fin_ready(pkt_idle, queued);
}

/* The bounded TX reset in tbt_tx.c fires after 30 s without an ACK advance.
 * Acceptance runs debug=0, so its TXSTALLKILL line is invisible; this
 * ratelimited line names the same state from the FEC side before the reset:
 * the ack/txseq gap, the frontier block, the replay backlog and the repair
 * age. It returns without printing while the ack is inside its budget. */
static void tbt_fec_tx_stall_diag(struct tbt_session *sess,
                                  struct tbt_fec_tx *tx,
                                  struct tbt_stream *st)
{
    u64 now = ktime_get_ns();
    u64 last = READ_ONCE(st->last_ack_ns);
    u64 budget = tbt_tx_wait_budget_ns(sess, st);
    u64 frontier = tbt_fec_block_of_seq_base(
        READ_ONCE(tx->peer_pkts), tx->base_pkt, tx->base_block, tx->groups,
        tx->n);
    u32 rq, rb;

    /* An idle stream has nothing outstanding: ack == txseq is not a stall.
     * The near's uplink GET sat at ack=txseq=91 and printed this line for
     * 954 s, drowning the log (fix25). */
    if (READ_ONCE(st->tx_ack) >= READ_ONCE(st->tx_seq))
        return;
    if (!last || now <= last || now - last <= budget)
        return;
    spin_lock(&sess->fec_out_lock);
    rq = sess->fec_replay_queued;
    rb = sess->fec_replay_bytes;
    spin_unlock(&sess->fec_out_lock);
    pr_info_ratelimited("tbtun: fec TX stall diag ack=%llu txseq=%llu peer_pkts=%llu frontier=%llu cur=%llu g=%u s=%u replay_q=%u replay_bytes=%u high=%d throttled=%llu age_us=%llu\n",
                        READ_ONCE(st->tx_ack), READ_ONCE(st->tx_seq),
                        READ_ONCE(tx->peer_pkts), frontier,
                        READ_ONCE(tx->cur_block),
                        READ_ONCE(tx->last_group), READ_ONCE(tx->last_share),
                        rq, rb, READ_ONCE(sess->fec_replay_over_high),
                        atomic64_read(&sess->fec_replay_pass_throttled),
                        (now - last) / NSEC_PER_USEC);
}

/* Kept out of line: inlining it into the work body doubled that frame, which
 * already holds the 128-entry flow snapshot. */
static noinline void tbt_fec_tx_pump(struct tbt_session *sess,
                                     struct tbt_fec_flow *f)
{
    struct tbt_fec_tx *tx = f->tx;
    struct tbt_stream *st;
    bool paused = false;
    bool short_ready;
    bool block_dirty;

    if (!tx || f->state != 1 || READ_ONCE(sess->closing) ||
        READ_ONCE(sess->fec_stopping))
        return;
    st = tbt_stream_get(sess, f->sid, false, false);
    if (!st)
        return;

    /* SC-LDPC announces its transmit shape with a per-flow INIT CFG, and the
     * receiver installs it from w_fec_rx asynchronously. Pumping shares
     * first makes the peer drop every one of them as a stale shape; the
     * repair burst then looks like channel loss to the brand-new control law
     * and pins the first rate decision low. Hold the packetizer until the
     * peer acknowledged INIT, or the bounded retry gave up. Grouped flows
     * never run the INIT handshake, so they are exempt. */
    if (sess->fec_sclc && sess->n_fec_flows <= 1 && tx->sid &&
        !tx->init_done)
    {
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
        tbt_stream_put(st);
        return;
    }

    /* A closing stream caps replay admission at
     * TBT_FEC_REPLAY_CLOSE_BYTES_MAX: the last block's repair must fit the
     * close budget, so an older backlog must not refill the queue. */
    WRITE_ONCE(tx->closing, READ_ONCE(st->close_since_ns) != 0);
    tbt_fec_tx_stall_diag(sess, tx, st);
    for (;;)
    {
        u32 got;

        if ((tx->switch_state == TBT_FEC_SW_CFG_SENT ||
             tx->switch_state == TBT_FEC_SW_DRAIN) &&
            tx->seq >= tx->switch_pkt)
            break;
        if (tx->alloc_hold && tx->seq >= tx->alloc_target.pkt)
            break;
        if (tbt_fec_tx_outq_high(sess))
        {
            if (!tx->outq_paused)
            {
                tx->outq_paused = true;
                atomic64_inc(&sess->fec_outq_high_wait);
            }
            tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
            break;
        }
        tx->outq_paused = false;
        spin_lock(&tx->lock);
        got = tbt_fec_ring_get(tx, tx->pkt + tx->fill, TBT_FEC_S - tx->fill);
        if (got)
            tx->fill += (u16)got;
        paused = tx->head == tx->tail && READ_ONCE(st->tx_paused);
        spin_unlock(&tx->lock);
        /* Allocate the repair ring only when this flow actually has bytes to
         * send. A receive-only session's TX ring stays empty, so it never
         * reserves the ~14 MiB window. No ring means no DATA: stop and retry
         * rather than send shares a NACK could never recover. */
        if ((got || tx->fill) && !tbt_fec_tx_replay_ensure(sess, tx))
        {
            tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
            break;
        }
        if (got)
        {
            if (tx->fill == TBT_FEC_S && tbt_fec_tx_credit_ok(tx))
                tbt_fec_packetize(sess, tx, st, false);
            continue;
        }
        if (tx->fill == TBT_FEC_S && tbt_fec_tx_credit_ok(tx))
        {
            tbt_fec_packetize(sess, tx, st, false);
            continue;
        }
        if (tx->fill == TBT_FEC_S)
        {
            atomic64_inc(&sess->fec_credit_wait);
            break;
        }
        if (READ_ONCE(tx->flush_req) && tx->head == tx->tail)
        {
            if (tx->fill && !tbt_fec_tx_credit_ok(tx))
                break;
            if (tx->fill)
                tbt_fec_packetize(sess, tx, st, true);
            else
                tbt_fec_tx_finish(sess, tx, st);
            WRITE_ONCE(tx->flush_req, false);
            continue;
        }
        break;
    }
    /* A block must be sealed after this much silence even when its last share
     * filled a packet exactly. The receiver learns the cut only from the
     * PARITY's win_len, so a stream that stops on a block boundary -- an
     * HTTP request of exactly N*S bytes, a sender waiting for the response --
     * would otherwise leave the block open until FIN and never be delivered.
     * The same 2 ms idle rule that already cuts a partial packet now cuts
     * the block; any share that arrives first clears the timer. */
    short_ready = false;
    block_dirty = false;
    if (tx->seq > tbt_fec_seq_of_block_pkt_base(
                      READ_ONCE(tx->cur_block), 0, 0, tx->base_pkt,
                      tx->base_block, tx->groups, tx->n))
        block_dirty = true;
    spin_lock(&tx->lock);
    if ((tx->fill || block_dirty) && tx->head == tx->tail)
        short_ready = true;
    spin_unlock(&tx->lock);
    if (short_ready && !READ_ONCE(tx->flush_req) &&
        !tbt_fec_tx_outq_high(sess) &&
        !tx->alloc_hold &&
        tx->switch_state != TBT_FEC_SW_CFG_SENT &&
        tx->switch_state != TBT_FEC_SW_DRAIN)
    {
        u64 now = ktime_get_ns();

        if (!tx->short_at_ns)
        {
            tx->short_at_ns = now + (u64)TBT_FEC_SHORT_MS * NSEC_PER_MSEC;
            tbt_session_arm(sess, TBT_SW_FEC_TX,
                            msecs_to_jiffies(TBT_FEC_SHORT_MS));
        }
        else if (now >= tx->short_at_ns)
        {
            if (tbt_fec_tx_credit_ok(tx))
            {
                if (tx->fill)
                    tbt_fec_packetize(sess, tx, st, false);
                tbt_fec_tx_close_block(sess, tx, true);
                tx->short_at_ns = 0;
            }
            /* Credit is a closed window, not a lost deadline: the ACK that
             * reopens it is the wake this wait needs. */
        }
        else
        {
            /* An earlier wake consumed this instance before the deadline: a
             * 0 ms flush, ACK or submit ran the pump with now < short_at_ns.
             * Re-arm the short cut here or it would wait for the next
             * unrelated wake. */
            tbt_session_arm(sess, TBT_SW_FEC_TX,
                            msecs_to_jiffies(TBT_FEC_SHORT_MS));
        }
    }
    else
    {
        tx->short_at_ns = 0;
    }
    if (paused)
    {
        WRITE_ONCE(st->tx_paused, false);
        tbt_stream_schedule(st, &st->w_read, TBT_STREAM_F_SCHED_READ);
    }
    tbt_stream_put(st);
}

/* Take a reference on the session's primary flow. The pointer is a borrowed
 * view, so a work body that keeps it across an unlock must own a reference:
 * detach can swap it and drop the last table reference at any time. */
static struct tbt_fec_flow *tbt_fec_primary_get(struct tbt_session *sess)
{
    struct tbt_fec_flow *f;

    if (!sess || !sess->fec)
        return NULL;
    spin_lock(&sess->lock);
    f = sess->fec_primary;
    if (f && !refcount_inc_not_zero(&f->refs))
        f = NULL;
    spin_unlock(&sess->lock);
    return f;
}

/* Snapshot the live flow table with one reference per entry. A flow owns at
 * least one group and every flow has a stream, so TBT_STREAMS_MAX covers
 * every possible entry. The snapshot is taken under sess->lock, which
 * is also where admit, detach and abort link/unlink; processing runs after
 * the unlock with the references held, so a flow can be detached but not
 * freed under the walk. */
static u16 tbt_fec_flow_snapshot(struct tbt_session *sess,
                                 struct tbt_fec_flow **out, u16 max)
{
    struct tbt_fec_flow *f;
    u16 n = 0, b;

    if (!sess || !sess->fec || !out || !max)
        return 0;
    spin_lock(&sess->lock);
    for (b = 0; b < TBT_STREAM_HT && n < max; b++)
        hlist_for_each_entry(f, &sess->fec_flows[b], node)
            if (tbt_fec_flow_live(f->state) && f->sid &&
                refcount_inc_not_zero(&f->refs))
                out[n++] = f;
    spin_unlock(&sess->lock);
    return n;
}

static void tbt_fec_flow_put_all(struct tbt_fec_flow **flows, u16 n)
{
    u16 i;

    for (i = 0; i < n; i++)
        tbt_fec_flow_put(flows[i]);
}

/* Publish the primary compatibility pointers for the RCU readers outside the
 * data plane (tbt_tx.c and tbt_carrier.c). The caller holds sess->lock. */
static void tbt_fec_primary_publish(struct tbt_session *sess,
                                    struct tbt_fec_flow *f)
{
    rcu_assign_pointer(sess->fec_primary, f);
    rcu_assign_pointer(sess->fec_tx, f ? f->tx : NULL);
    rcu_assign_pointer(sess->fec_rx, f ? f->rx : NULL);
}

static void tbt_fec_alloc_sort_flows(struct tbt_fec_flow **order, u16 n)
{
    u16 i, j;

    for (i = 1; i < n; i++)
    {
        struct tbt_fec_flow *key = order[i];

        j = i;
        while (j > 0 && order[j - 1]->sid > key->sid)
        {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
}

/* Prepare one flow's initiator exchange. The target mask and count are
 * fixed here; P0/B0 are filled by the TX tick after the current block is
 * closed. Returns false when the flow is already switching. */
static bool tbt_fec_alloc_prepare_tx(struct tbt_session *sess,
                                     struct tbt_fec_flow *f,
                                     const u8 *mask, u16 groups, u32 epoch,
                                     u64 now)
{
    struct tbt_fec_tx *tx = f->tx;
    u16 map[TBT_FEC_MAX_GROUPS];

    if (!tx || !groups ||
        tbt_fec_group_map_build(mask, sess->fec_groups_total, map) != groups)
        return false;
    /* One base-moving operation per flow: a shape switch must finish before
     * a group switch can start. */
    if (tx->switch_state != TBT_FEC_SW_STEADY)
        return false;
    spin_lock_bh(&tx->alloc_lock);
    if (tx->alloc_role)
    {
        spin_unlock_bh(&tx->alloc_lock);
        return false;
    }
    tx->alloc_target.epoch = epoch;
    tx->alloc_target.groups = groups;
    memcpy(tx->alloc_target.mask, mask, TBT_FEC_BITMAP_BYTES);
    memcpy(tx->alloc_target.map, map, sizeof(map));
    tx->alloc_target.pkt = 0;
    tx->alloc_target.block = 0;
    tbt_fec_alloc_sm_init(&tx->alloc);
    if (!tbt_fec_alloc_sm_start(&tx->alloc, now))
    {
        spin_unlock_bh(&tx->alloc_lock);
        return false;
    }
    tx->alloc_role = 1;
    tx->alloc_hold = false;
    tx->alloc_fail = false;
    spin_unlock_bh(&tx->alloc_lock);
    return true;
}

/* Fail a round under construction: clear the request state and let the
 * admission wait see the failure. */
static void tbt_fec_alloc_round_fail(struct tbt_session *sess)
{
    bool retry;

    spin_lock(&sess->lock);
    retry = !sess->fec_alloc_req && sess->fec_alloc_grow &&
            sess->fec_alloc_grow_tries < TBT_FEC_ALLOC_RETRIES;
    sess->fec_alloc_round = false;
    sess->fec_alloc_req = false;
    sess->fec_alloc_has_pending = false;
    sess->fec_alloc_pending_valid = false;
    sess->fec_alloc_failed = true;
    if (retry)
    {
        sess->fec_alloc_grow = true;
        sess->fec_alloc_grow_tries++;
    }
    else
    {
        sess->fec_alloc_grow = false;
        sess->fec_alloc_grow_tries = 0;
    }
    spin_unlock(&sess->lock);
    /* A peer that still lists the departed flow as live rejects the mask;
     * its teardown follows the close handshake, so retry briefly. */
    if (retry)
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(500));
}

/* Build one rebalance round. The near side recomputes an equal split over
 * every live bound flow, with the pending admission (if any) inserted at its
 * sid order. Existing flows shrink first, then grow from the free pool, so
 * every planned set stays disjoint while the switches run. */
/* Kept out of line: it owns the flow snapshot and the split workspace, and
 * the TX work body must stay inside the kernel's 2 KiB frame budget. */
static noinline void tbt_fec_alloc_plan_start(struct tbt_session *sess)
{
    struct tbt_fec_flow *flows[TBT_STREAMS_MAX];
    struct tbt_fec_flow **order = NULL;
    u8 (*plan)[TBT_FEC_BITMAP_BYTES] = NULL;
    u16 *target = NULL;
    u8 newmask[TBT_FEC_BITMAP_BYTES];
    u16 n, n_total, i, k, pos, new_target = 0, affected = 0;
    u16 total = sess->fec_groups_total;
    bool req, grow, has_new, admit;
    u64 sid = 0, now = ktime_get_ns();
    u32 epoch;
    int err = -ENOMEM;

    spin_lock(&sess->lock);
    if (sess->fec_alloc_round)
    {
        spin_unlock(&sess->lock);
        return;
    }
    req = sess->fec_alloc_req;
    grow = sess->fec_alloc_grow;
    admit = sess->fec_alloc_admit;
    sid = sess->fec_alloc_pending_sid;
    epoch = sess->fec_alloc_epoch + 1;
    spin_unlock(&sess->lock);
    if (!req && !grow)
        return;
    /* An admission that took no round is creating its flow from the free
     * pool. Let it finish; the grow round then includes the new flow. */
    if (!req && admit)
        return;
    n = tbt_fec_flow_snapshot(sess, flows, ARRAY_SIZE(flows));
    if (!n || !total)
    {
        tbt_fec_flow_put_all(flows, n);
        tbt_fec_alloc_round_fail(sess);
        return;
    }
    has_new = req && sid != 0;
    n_total = (u16)(n + (has_new ? 1 : 0));
    if (n_total > total)
    {
        tbt_fec_flow_put_all(flows, n);
        tbt_fec_alloc_round_fail(sess);
        return;
    }
    order = kvzalloc((size_t)n_total * sizeof(*order), GFP_KERNEL);
    plan = kvzalloc((size_t)n_total * TBT_FEC_BITMAP_BYTES, GFP_KERNEL);
    target = kvzalloc((size_t)n_total * sizeof(*target), GFP_KERNEL);
    if (!order || !plan || !target)
        goto out;
    for (i = 0; i < n; i++)
        order[i] = flows[i];
    tbt_fec_alloc_sort_flows(order, n);
    if (has_new)
    {
        for (pos = 0; pos < n; pos++)
            if (sid < order[pos]->sid)
                break;
        memmove(&order[pos + 1], &order[pos],
                (size_t)(n - pos) * sizeof(*order));
        order[pos] = NULL;
    }
    /* Compact the existing flows and compute their targets in sid order; the
     * pending flow's position only decides its count. */
    k = 0;
    for (pos = 0; pos < n_total; pos++)
    {
        u16 t = tbt_fec_alloc_count(total, n_total, pos);

        if (!order[pos])
        {
            new_target = t;
            continue;
        }
        memcpy(plan[k], order[pos]->group_mask, TBT_FEC_BITMAP_BYTES);
        target[k] = t;
        order[k] = order[pos];
        k++;
    }
    memset(newmask, 0, sizeof(newmask));
    if (!tbt_fec_alloc_split(total, n, plan, target,
                             has_new ? newmask : NULL, new_target))
        goto out;
    /* Refuse the round before starting any flow: a prepared flow cannot be
     * rolled back without a peer exchange. */
    for (k = 0; k < n; k++)
    {
        struct tbt_fec_tx *tx = order[k]->tx;

        if (!memcmp(plan[k], order[k]->group_mask, TBT_FEC_BITMAP_BYTES))
            continue;
        if (!tx || tx->alloc_role ||
            tx->switch_state != TBT_FEC_SW_STEADY)
            goto out;
    }
    for (k = 0; k < n; k++)
    {
        if (!memcmp(plan[k], order[k]->group_mask, TBT_FEC_BITMAP_BYTES))
            continue;
        if (!tbt_fec_alloc_prepare_tx(sess, order[k], plan[k], target[k],
                                      epoch, now))
            goto out;
        affected++;
    }

    spin_lock(&sess->lock);
    sess->fec_alloc_epoch = epoch;
    sess->fec_alloc_round = affected != 0;
    sess->fec_alloc_flows = affected;
    sess->fec_alloc_finished = 0;
    sess->fec_alloc_failed = false;
    sess->fec_alloc_has_pending = has_new;
    sess->fec_alloc_deadline_ns = now +
        (u64)TBT_FEC_ALLOC_TIMEOUT_MS * TBT_FEC_NSEC_PER_MSEC +
        500ULL * TBT_FEC_NSEC_PER_MSEC;
    if (has_new)
    {
        sess->fec_alloc_pending.epoch = epoch;
        sess->fec_alloc_pending.groups = new_target;
        memcpy(sess->fec_alloc_pending.mask, newmask,
               TBT_FEC_BITMAP_BYTES);
        tbt_fec_group_map_build(newmask, total,
                                sess->fec_alloc_pending.map);
        sess->fec_alloc_pending.pkt = 0;
        sess->fec_alloc_pending.block = 0;
    }
    sess->fec_alloc_pending_valid = has_new && affected == 0;
    if (!sess->fec_alloc_round)
    {
        sess->fec_alloc_req = false;
        sess->fec_alloc_grow = false;
    }
    spin_unlock(&sess->lock);
    atomic64_inc(&sess->fec_alloc_begin);
    if (sess->fec_alloc_round)
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    err = 0;

out:
    if (err)
    {
        tbt_fec_alloc_round_fail(sess);
        atomic64_inc(&sess->fec_alloc_bad);
    }
    kvfree(target);
    kvfree(plan);
    kvfree(order);
    tbt_fec_flow_put_all(flows, n);
}

/* Complete or time out the active round. A flow that was killed mid-switch
 * is counted by the deadline, not by the table scan. */
static void tbt_fec_alloc_round_tick(struct tbt_session *sess)
{
    bool active, has_pending, failed, req, grow, retry = false;
    u16 expected, finished;
    u8 tries;
    u64 deadline, now = ktime_get_ns();

    spin_lock(&sess->lock);
    active = sess->fec_alloc_round;
    if (!active)
    {
        bool want = sess->fec_alloc_req || sess->fec_alloc_grow;

        spin_unlock(&sess->lock);
        if (want)
            tbt_fec_alloc_plan_start(sess);
        return;
    }
    expected = sess->fec_alloc_flows;
    finished = sess->fec_alloc_finished;
    deadline = sess->fec_alloc_deadline_ns;
    failed = sess->fec_alloc_failed;
    has_pending = sess->fec_alloc_has_pending;
    req = sess->fec_alloc_req;
    grow = sess->fec_alloc_grow;
    tries = sess->fec_alloc_grow_tries;
    spin_unlock(&sess->lock);
    if (finished < expected && now < deadline)
    {
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(5));
        return;
    }
    if (finished < expected)
        failed = true;
    spin_lock(&sess->lock);
    if (sess->fec_alloc_round)
    {
        sess->fec_alloc_round = false;
        sess->fec_alloc_req = false;
        sess->fec_alloc_has_pending = false;
        if (failed)
        {
            sess->fec_alloc_failed = true;
            sess->fec_alloc_pending_valid = false;
            atomic64_inc(&sess->fec_alloc_abort);
            retry = !req && grow && tries < TBT_FEC_ALLOC_RETRIES;
            sess->fec_alloc_grow = retry;
            sess->fec_alloc_grow_tries = retry ? (u8)(tries + 1) : 0;
        }
        else if (has_pending)
        {
            sess->fec_alloc_pending_valid = true;
            sess->fec_alloc_grow_tries = 0;
        }
        else
        {
            sess->fec_alloc_grow = false;
            sess->fec_alloc_grow_tries = 0;
        }
    }
    spin_unlock(&sess->lock);
    if (retry)
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(500));
}

static void tbt_fec_tx_work_body(struct tbt_session *sess)
{
    struct tbt_fec_flow *primary;
    struct tbt_fec_flow **flows = sess->fec_tx_snap;
    u16 n, i;

    if (READ_ONCE(sess->closing) || READ_ONCE(sess->fec_stopping))
        return;
    primary = tbt_fec_primary_get(sess);
    if (!primary)
        return;
    /* Control and replay are session-wide and flush here; the bound data
     * queues drain in their per-carrier works, so a full socket never stops
     * the packetizer. */
    (void)tbt_fec_out_flush(sess, NULL);
    /* The initial shape is a session property; with one flow it is also the
     * flow's shape. N>1 freezes CFG/FBK (design §3.8). */
    if (sess->n_fec_flows <= 1)
        tbt_fec_tx_init_tick(sess, primary->tx);

    if (sess->crypto && !smp_load_acquire(&sess->crypto_tx_ready))
    {
        tbt_session_arm(sess, TBT_SW_FEC_TICK, msecs_to_jiffies(10));
        goto out;
    }

    n = tbt_fec_flow_snapshot(sess, flows, TBT_STREAMS_MAX);
    for (i = 0; i < n; i++)
        tbt_fec_alloc_tx_tick(sess, flows[i]);
    for (i = 0; i < n; i++)
        tbt_fec_tx_pump(sess, flows[i]);
    tbt_fec_flow_put_all(flows, n);
    /* Admission and detach ask for a new split through this work item. The
     * round runs after the pumps so a newly stopped flow is already gated. */
    tbt_fec_alloc_round_tick(sess);
    if (tbt_fec_out_flush(sess, NULL) == -EAGAIN)
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
out:
    tbt_fec_flow_put(primary);
}

/* Send one FEC_CFG. APPLY announces the pending shape; ABORT cancels one the
 * receiver may have armed. Control records are plaintext and ride the
 * control queue, so a dead share carrier does not lose them. */
static void tbt_fec_tx_send_cfg(struct tbt_session *sess,
                                struct tbt_fec_tx *tx, u8 flags)
{
    struct tbt_fec_cfg c;
    struct tbt_fec_hdr h;
    u8 body[TBT_FEC_CFG_LEN];
    u8 rec[TBT_FEC_HDR_LEN + TBT_FEC_CFG_LEN];
    u16 len;

    if (!tx)
        return;
    memset(&c, 0, sizeof(c));
    c.epoch = tx->switch_epoch;
    c.n = tx->switch_shape.n;
    c.m = tx->switch_shape.m;
    c.w = tx->switch_shape.w;
    c.k = tx->switch_shape.k;
    c.d = tx->switch_shape.d;
    c.flags = flags;
    c.groups = tx->switch_shape.groups;
    c.switch_pkt = tx->switch_pkt;
    c.block_base = tx->switch_block;
    c.seed = tx->nonce;
    c.codec_variant = tx->switch_shape.codec_variant;
    c.L = tx->switch_shape.L;
    c.block = tx->switch_shape.block;
    c.rate_ppm = tx->switch_shape.rate_ppm;
    if (tbt_fec_cfg_pack(&c, body))
        return;
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_CFG;
    h.payload_len = TBT_FEC_CFG_LEN;
    h.conv = TBT_FEC_CONV;
    h.sid = tx->sid;
    if (tbt_fec_make_record(sess, &h, body, rec, &len))
        return;
    (void)tbt_fec_send_record(sess, 0, rec, len, true, false, tx->flow);
}

/* The peer's receive shape is this side's transmit shape, and the wire
 * carries no codec field, so a session whose directions differ (downlink
 * 1x256, uplink 8x32) has to announce it. INIT is not a switch: the receiver
 * installs it before its first DATA and the sender keeps its own codec. It is
 * retried until the peer's CFG_ACK names epoch 1. */
static void tbt_fec_tx_send_init(struct tbt_session *sess,
                                 struct tbt_fec_tx *tx)
{
    struct tbt_fec_cfg c;
    struct tbt_fec_hdr h;
    u8 body[TBT_FEC_CFG_LEN];
    u8 rec[TBT_FEC_HDR_LEN + TBT_FEC_CFG_LEN];
    u16 len;

    if (!tx)
        return;
    memset(&c, 0, sizeof(c));
    c.epoch = 1;
    c.n = tx->n;
    c.m = tx->m;
    c.w = tx->shape_w;
    c.k = tx->shape_k;
    c.d = tx->shape_d;
    c.flags = TBT_FEC_CFG_F_APPLY | TBT_FEC_CFG_F_INIT;
    c.groups = tx->groups;
    c.switch_pkt = 0;
    c.block_base = 0;
    c.seed = tx->nonce;
    c.codec_variant = tx->codec_variant;
    c.L = tx->sclc_L;
    c.block = tx->sclc_block;
    c.rate_ppm = tx->rate_ppm;
    if (tbt_fec_cfg_pack(&c, body))
        return;
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_CFG;
    h.payload_len = TBT_FEC_CFG_LEN;
    h.conv = TBT_FEC_CONV;
    h.sid = tx->sid;
    if (tbt_fec_make_record(sess, &h, body, rec, &len))
        return;
    (void)tbt_fec_send_record(sess, 0, rec, len, true, false, tx->flow);
}

/* One FEC_ALLOC record. The body is symmetric: switch_pkt/block_base always
 * name the sender's own TX origin, so the peer can arm the reverse RX at the
 * same point. Control records ride the carrier-agnostic control queue. */
static int tbt_fec_send_alloc(struct tbt_session *sess, struct tbt_fec_flow *f,
                              u8 flags, u32 epoch, const u8 *mask,
                              u64 switch_pkt, u64 block_base)
{
    struct tbt_fec_alloc a;
    struct tbt_fec_hdr h;
    u8 body[TBT_FEC_ALLOC_LEN];
    u8 rec[TBT_FEC_HDR_LEN + TBT_FEC_ALLOC_LEN];
    u16 len;

    if (!sess || !f || !mask)
        return -EINVAL;
    if (debug)
        pr_info("tbtun: fec alloc tx sid=%llu flags=%#x epoch=%u groups=%u pkt=%llu blk=%llu\n",
                f->sid, flags, epoch, tbt_fec_bitmap_count(mask), switch_pkt,
                block_base);
    memset(&a, 0, sizeof(a));
    a.epoch = epoch;
    memcpy(a.group_mask, mask, TBT_FEC_BITMAP_BYTES);
    a.flags = flags;
    a.switch_pkt = switch_pkt;
    a.block_base = block_base;
    /* pack() returns the body length on success, not 0: comparing it with
     * true silently dropped every ALLOC record, so no exchange ever
     * started. The host test pins the same contract. */
    if (tbt_fec_alloc_pack(&a, body) != TBT_FEC_ALLOC_LEN)
        return -EINVAL;
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_ALLOC;
    h.payload_len = TBT_FEC_ALLOC_LEN;
    h.conv = TBT_FEC_CONV;
    h.sid = f->sid;
    if (tbt_fec_make_record(sess, &h, body, rec, &len))
        return -EINVAL;
    {
        int err = tbt_fec_send_record(sess, 0, rec, len, true, false, f);

        if (debug)
            pr_info("tbtun: fec alloc queued sid=%llu flags=%#x err=%d\n",
                    f->sid, flags, err);
        return err;
    }
}

/* Publish a committed flow mask: the group pool and the OPEN body both read
 * it. Called only after both directions of the exchange are consistent. */
static void tbt_fec_flow_mask_commit(struct tbt_session *sess,
                                     struct tbt_fec_flow *f,
                                     const struct tbt_fec_alloc_flow *a)
{
    spin_lock(&sess->lock);
    if (sess->fec_groups_used >= f->groups)
        sess->fec_groups_used -= f->groups;
    else
        sess->fec_groups_used = 0;
    f->groups = a->groups;
    memcpy(f->group_mask, a->mask, TBT_FEC_BITMAP_BYTES);
    memcpy(f->group_map, a->map, sizeof(f->group_map));
    sess->fec_groups_used += f->groups;
    if (sess->fec_groups_used > sess->fec_groups_total)
        sess->fec_groups_used = sess->fec_groups_total;
    spin_unlock(&sess->lock);
}

/* The per-group scratch array is sized for the session's whole pool at plane
 * creation, so a late NACK that still names an old local group reads a live
 * struct. Only the buffers outside the new count are released. */
static bool tbt_fec_tx_groups_resize(struct tbt_fec_tx *tx, u16 groups)
{
    u16 shares = (u16)(tx->n + tx->m);
    u16 from = tx->groups;
    u16 i, j;

    if (!shares || groups > tx->g_cap)
        return false;
    for (i = from; i < groups; i++)
    {
        if (!tx->g[i].buf)
        {
            tx->g[i].buf = kvzalloc((size_t)shares * TBT_FEC_S,
                                    GFP_KERNEL);
            if (!tx->g[i].buf)
            {
                /* Release only what this call took; the flow keeps the
                 * buffers of its current mask. */
                for (j = from; j < i; j++)
                {
                    kvfree(tx->g[j].buf);
                    memset(&tx->g[j], 0, sizeof(tx->g[j]));
                }
                return false;
            }
        }
        tx->g[i].blk = tx->g[i].buf;
        tx->g[i].par = tx->g[i].buf + (size_t)tx->n * TBT_FEC_S;
    }
    for (i = groups; i < tx->groups; i++)
    {
        kvfree(tx->g[i].buf);
        memset(&tx->g[i], 0, sizeof(tx->g[i]));
    }
    return true;
}

/* Install the new group set on a stopped TX. The packetizer is paused at
 * alloc_target.pkt, every old block is closed, and the peer has applied its
 * RX at that point, so the replay ring and the group scratch can be replaced
 * without losing a repairable block. */
static bool tbt_fec_tx_alloc_swap(struct tbt_session *sess,
                                  struct tbt_fec_flow *f)
{
    struct tbt_fec_tx *tx = f->tx;
    struct tbt_fec_alloc_flow *a = &tx->alloc_target;
    struct tbt_fec_grouped *codec;
    struct tbt_fec_replay *replay, *old_replay;
    struct tbt_fec_grouped *old_codec;
    u16 i, rk = sess->fec_replay_k;
    u32 cap;

    if (!a->groups || a->groups > sess->fec_groups_total ||
        a->groups != tbt_fec_bitmap_count(a->mask))
        return false;
    codec = tbt_fec_codec_new(NULL, a->groups, tx->n, tx->m, tx->n,
                              tx->shape_w, tx->shape_d, tx->nonce);
    if (!codec)
        return false;
    replay = tbt_fec_replay_new_k(a->groups, tx->n, tx->m, TBT_FEC_S, rk);
    if (!replay)
    {
        tbt_fec_grouped_free(codec);
        return false;
    }
    if (!tbt_fec_tx_groups_resize(tx, a->groups))
    {
        tbt_fec_grouped_free(codec);
        tbt_fec_replay_free(replay);
        return false;
    }

    old_codec = tx->codec;
    tx->codec = codec;
    if (tx->old_codec)
        tbt_fec_grouped_free(tx->old_codec);
    tx->old_codec = old_codec;
    spin_lock_bh(&tx->replay_lock);
    old_replay = tx->replay;
    tx->replay = replay;
    spin_unlock_bh(&tx->replay_lock);
    tbt_fec_replay_free(old_replay);

    spin_lock_bh(&tx->shape_lock);
    tx->groups = a->groups;
    memcpy(tx->group_mask, a->mask, TBT_FEC_BITMAP_BYTES);
    memcpy(tx->group_map, a->map, sizeof(tx->group_map));
    tx->base_pkt = a->pkt;
    tx->base_block = a->block;
    spin_unlock_bh(&tx->shape_lock);
    tx->seq = a->pkt;
    tx->cur_block = a->block;
    if (debug)
        pr_info("tbtun: alloc swap t=%llu sid=%llu role=%u grp=%u base=%llu/%llu\n",
                sess->tunnel ? sess->tunnel->cfg.id : 0, f->sid,
                tx->alloc_role, a->groups, a->pkt, a->block);
    for (i = 0; i < a->groups; i++)
    {
        tx->g[i].blk = tx->g[i].buf;
        tx->g[i].par = tx->g[i].buf + (size_t)tx->n * TBT_FEC_S;
        tx->g[i].blk_id = ~0ULL;
        tx->g[i].nreal = 0;
        tx->g[i].last_len = 0;
    }
    cap = tbt_fec_credit_packets(a->groups, tx->n, rk, sess->fec_slots,
                                 TBT_FEC_CREDIT_MAX);
    WRITE_ONCE(tx->credit_cap, cap);
    WRITE_ONCE(tx->credit, cap);
    tx->short_at_ns = 0;
    tx->alloc_hold = false;
    tbt_fec_flow_mask_commit(sess, f, a);
    return true;
}

/* Stop a flow's TX on the next super-block boundary and remember P0/B0. The
 * block under construction is closed first, so every group's parity and
 * empty marker describe the old mask before the switch record is sent. */
static bool tbt_fec_alloc_tx_stop(struct tbt_session *sess,
                                  struct tbt_fec_flow *f)
{
    struct tbt_fec_tx *tx = f->tx;
    u64 block_start, pkt, block;

    if (!tx || tx->switch_state != TBT_FEC_SW_STEADY)
        return false;
    block_start = tbt_fec_seq_of_block_pkt_base(tx->cur_block, 0, 0,
                                                tx->base_pkt, tx->base_block,
                                                tx->groups, tx->n);
    if (tx->seq > block_start)
        tbt_fec_tx_close_block(sess, tx, true);
    tbt_fec_alloc_p0(tx->seq, tx->cur_block, tx->base_pkt, tx->base_block,
                     tx->groups, tx->n, &pkt, &block);
    spin_lock_bh(&tx->alloc_lock);
    tx->alloc_target.pkt = pkt;
    tx->alloc_target.block = block;
    spin_unlock_bh(&tx->alloc_lock);
    tx->alloc_hold = true;
    if (debug)
        pr_info("tbtun: alloc stop t=%llu sid=%llu role=%u grp=%u n=%u seq=%llu cur=%llu base=%llu/%llu start=%llu -> pkt=%llu blk=%llu peerpkts=%llu\n",
                sess->tunnel ? sess->tunnel->cfg.id : 0, f->sid,
                tx->alloc_role, tx->groups, tx->n, tx->seq, tx->cur_block,
                tx->base_pkt, tx->base_block, block_start, pkt, block,
                READ_ONCE(tx->peer_pkts));
    return true;
}

/* Pace the announcement: first pass immediately, then every 500 ms, at most
 * six times. A peer that never acks keeps the two-sided shape it configured
 * itself; the mismatch then shows as a shape drop, not as silence. */
static void tbt_fec_tx_init_tick(struct tbt_session *sess,
                                 struct tbt_fec_tx *tx)
{
    u64 now = ktime_get_ns();

    if (!tx || tx->init_done || !tx->sid)
        return;
    if (tx->peer_est_valid && tx->peer_epoch == 1 &&
        (tx->peer_flags & TBT_FEC_FBK_F_CFG_ACK))
    {
        tx->init_done = true;
        return;
    }
    if (tx->peer_est_valid && tx->peer_epoch == 1 &&
        (tx->peer_flags & TBT_FEC_FBK_F_CFG_NACK))
    {
        /* The peer cannot build this shape. Retrying cannot help; keep the
         * two-sided configured shape and name the mismatch once. */
        tx->init_done = true;
        pr_warn_ratelimited("tbtun: tunnel %llu: peer refused the INIT shape sid=%llu n=%u m=%u groups=%u w=%u k=%u d=%u\n",
                            (unsigned long long)sess->tunnel->cfg.id,
                            (unsigned long long)tx->sid, tx->n, tx->m,
                            tx->groups, tx->shape_w, tx->shape_k,
                            tx->shape_d);
        return;
    }
    if (tx->init_tries >= 6)
    {
        tx->init_done = true;
        return;
    }
    if (tx->init_tries && now - tx->init_ns < 500000000ULL)
    {
        tbt_session_arm(sess, TBT_SW_FEC_TICK, msecs_to_jiffies(500));
        return;
    }
    tbt_fec_tx_send_init(sess, tx);
    tx->init_tries++;
    tx->init_ns = now;
    tbt_session_arm(sess, TBT_SW_FEC_TICK, msecs_to_jiffies(500));
}

static void tbt_fec_tx_switch_abort(struct tbt_session *sess, struct tbt_fec_tx *tx, bool tell_peer);
static void tbt_fec_tx_swap(struct tbt_session *sess, struct tbt_fec_tx *tx);
static void tbt_fec_tx_switch_tick(struct tbt_session *sess, struct tbt_fec_tx *tx);

static void tbt_fec_tx_clear_data_queues(struct tbt_session *sess)
{
    struct tbt_fec_out *o, *tmp;
    u16 i;

    spin_lock(&sess->fec_out_lock);
    list_for_each_entry_safe(o, tmp, &sess->fec_out_replay, node)
        tbt_fec_out_free(sess, NULL, o);
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_fec_outq *q = &sess->fec_outq[i];

        list_for_each_entry_safe(o, tmp, &q->queue, node)
            tbt_fec_out_free(sess, q, o);
    }
    spin_unlock(&sess->fec_out_lock);
}

static bool tbt_fec_sess_sclc(const struct tbt_session *sess)
{
    return sess->fec_sclc;
}

static void tbt_fec_tx_switch_abort(struct tbt_session *sess,
                                    struct tbt_fec_tx *tx, bool tell_peer)
{

    if (!tx)
        return;
    if (tell_peer)
        tbt_fec_tx_send_cfg(sess, tx, TBT_FEC_CFG_F_ABORT);
    tx->switch_state = TBT_FEC_SW_COOLDOWN;
    tx->switch_start_ns = ktime_get_ns();
    if (tbt_fec_sess_sclc(sess) && tx->adapt_sclc)
        tbt_fec_adapt_sclc_ctl_abort(tx->adapt_sclc);
    else
        tbt_fec_adapt_abort(tx->adapt);
    atomic64_inc(&sess->fec_switch_abort);
    spin_lock(&sess->lock);
    sess->adapt.state = TBT_FEC_ADAPT_ABORT;
    sess->adapt.reason = TBT_FEC_ADAPT_CTL_R_NONE;
    spin_unlock(&sess->lock);
}

/* Commit the new shape on the sender: the old block is closed at P0, every
 * replay and data queue belongs to the old shape, and the packetizer resumes
 * at B0 with a fresh codec. The old codec is kept until the next swap so a
 * late NACK cannot dereference freed state. */
static void tbt_fec_tx_swap(struct tbt_session *sess, struct tbt_fec_tx *tx)
{
    struct tbt_fec_grouped *codec;
    struct tbt_stream *st;
    u16 i, rk;

    codec = tbt_fec_grouped_new_ops(tbt_fec_ops_of_shape(&tx->switch_shape),
                                    tx->switch_shape.groups,
                                    tx->switch_shape.n, tx->switch_shape.m,
                                    tx->switch_shape.k, tx->switch_shape.w,
                                    TBT_FEC_S, tx->switch_shape.d,
                                    tx->nonce);
    if (!codec)
    {
        tbt_fec_tx_switch_abort(sess, tx, true);
        return;
    }
    if (tx->old_codec)
        tbt_fec_grouped_free(tx->old_codec);
    tx->old_codec = tx->codec;
    tx->codec = codec;
    spin_lock(&tx->shape_lock);
    tx->band = tbt_fec_grouped_band(codec, 0);
    tx->n = tx->switch_shape.n;
    tx->m = tx->switch_shape.m;
    tx->groups = tx->switch_shape.groups;
    tx->base_pkt = tx->switch_pkt;
    tx->base_block = tx->switch_block;
    spin_unlock(&tx->shape_lock);
    tx->cur_block = tx->switch_block;
    tx->seq = tx->switch_pkt;
    for (i = 0; i < tx->groups; i++)
    {
        tx->g[i].blk = tx->g[i].buf;
        tx->g[i].par = tx->g[i].buf + (size_t)tx->n * TBT_FEC_S;
        tx->g[i].blk_id = ~0ULL;
        tx->g[i].nreal = 0;
        tx->g[i].last_len = 0;
    }
    rk = tx->replay ? tbt_fec_replay_k(tx->replay) : sess->fec_replay_k;
    tx->credit_cap = tbt_fec_credit_packets(tx->groups, tx->n, rk, 0,
                                            TBT_FEC_CREDIT_MAX);
    tx->credit = tx->credit_cap;
    spin_lock_bh(&tx->replay_lock);
    tbt_fec_replay_reset(tx->replay);
    spin_unlock_bh(&tx->replay_lock);
    tbt_fec_tx_clear_data_queues(sess);
    tx->short_at_ns = 0;
    tx->carrier_wait = false;
    tx->backpressure_counted = false;
    tx->outq_paused = false;
    tbt_fec_shape_wire_norm(&tx->switch_shape);
    tx->shape_w = tx->switch_shape.w;
    tx->shape_k = tx->switch_shape.k;
    tx->shape_d = tx->switch_shape.d;
    tx->codec_variant = tx->switch_shape.codec_variant;
    tx->sclc_L = tx->switch_shape.L;
    tx->sclc_block = tx->switch_shape.block;
    tx->rate_ppm = tx->switch_shape.rate_ppm;
    tx->switch_state = TBT_FEC_SW_STEADY;
    if (tbt_fec_sess_sclc(sess) && tx->adapt_sclc)
        tbt_fec_adapt_sclc_ctl_applied(tx->adapt_sclc, ktime_get_ns());
    else
        tbt_fec_adapt_applied(tx->adapt, ktime_get_ns());
    atomic64_inc(&sess->fec_switch_done);
    spin_lock(&sess->lock);
    sess->adapt.epoch = tx->switch_epoch;
    sess->adapt.n = tx->n;
    sess->adapt.m = tx->m;
    sess->adapt.groups = tx->groups;
    sess->adapt.class = (tbt_fec_sess_sclc(sess) && tx->adapt_sclc)
                            ? tx->adapt_sclc->cur
                            : tx->adapt->cur;
    sess->adapt.state = TBT_FEC_ADAPT_STEADY;
    sess->adapt.reason = TBT_FEC_ADAPT_CTL_R_NONE;
    spin_unlock(&sess->lock);
    st = tbt_fec_stream(sess);
    if (st)
    {
        WRITE_ONCE(st->tx_paused, false);
        tbt_stream_schedule(st, &st->w_read, TBT_STREAM_F_SCHED_READ);
        tbt_stream_put(st);
    }
    tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
}

/* Close the current old-shape block at P0 and send the first CFG. The
 * packetizer is paused from here until the swap or the abort. */
static void tbt_fec_tx_switch_begin_shape(struct tbt_session *sess,
                                          struct tbt_fec_tx *tx,
                                          const struct tbt_fec_shape *shape,
                                          u32 epoch)
{
    struct tbt_stream *st;
    u64 block_start;

    if (!tx || tx->switch_state != TBT_FEC_SW_STEADY)
        return;
    /* A group rebalance owns the base until both directions commit. */
    if (tx->alloc_role || READ_ONCE(sess->fec_alloc_round))
        return;
    /* Phase 1 keeps the carrier geometry: n+m and groups must not change. */
    if (shape->groups != tx->groups ||
        (u16)(shape->n + shape->m) != (u16)(tx->n + tx->m) ||
        !shape->n || !shape->m)
        return;
    st = tbt_fec_stream(sess);
    if (!st)
        return;
    if (READ_ONCE(st->fec_finished) || READ_ONCE(sess->closing))
    {
        tbt_stream_put(st);
        return;
    }
    block_start = tbt_fec_seq_of_block_pkt_base(
        tx->cur_block, 0, 0, tx->base_pkt, tx->base_block, tx->groups,
        tx->n);
    if (tx->seq > block_start)
        tbt_fec_tx_close_block(sess, tx, true);
    tx->switch_pkt = tx->seq;
    tx->switch_block = tx->cur_block;
    tx->switch_epoch = epoch;
    tx->switch_shape = *shape;
    tbt_fec_shape_wire_norm(&tx->switch_shape);
    tx->switch_shape.base_pkt = tx->switch_pkt;
    tx->switch_shape.base_block = tx->switch_block;
    tx->switch_shape.slots = 0;
    tx->switch_retries = 1;
    tx->switch_start_ns = ktime_get_ns();
    tx->switch_cfg_ns = tx->switch_start_ns;
    tx->switch_state = TBT_FEC_SW_CFG_SENT;
    tbt_fec_tx_send_cfg(sess, tx, TBT_FEC_CFG_F_APPLY);
    atomic64_inc(&sess->fec_switch_begin);
    spin_lock(&sess->lock);
    sess->adapt.state = TBT_FEC_ADAPT_CFG_SENT;
    sess->adapt.reason = TBT_FEC_ADAPT_CTL_R_SWITCHING;
    sess->adapt.epoch = tx->switch_epoch;
    spin_unlock(&sess->lock);
    tbt_stream_put(st);
}

static void tbt_fec_tx_switch_begin(struct tbt_session *sess,
                                    struct tbt_fec_tx *tx,
                                    const struct tbt_fec_adapt_out *sw)
{
    tbt_fec_tx_switch_begin_shape(sess, tx, &sw->shape, sw->epoch);
}

static void tbt_fec_tx_switch_tick(struct tbt_session *sess,
                                   struct tbt_fec_tx *tx)
{
    u64 now = ktime_get_ns();

    if (!tx)
        return;
    switch (tx->switch_state)
    {
    case TBT_FEC_SW_CFG_SENT:
        if (tx->peer_est_valid && tx->peer_epoch == tx->switch_epoch &&
            (tx->peer_flags & TBT_FEC_FBK_F_CFG_NACK))
        {
            if (tbt_fec_sess_sclc(sess) && tx->adapt_sclc)
                tbt_fec_adapt_sclc_ctl_rejected(tx->adapt_sclc, now);
            tbt_fec_tx_switch_abort(sess, tx, false);
            return;
        }
        if (tx->peer_est_valid && tx->peer_epoch == tx->switch_epoch &&
            (tx->peer_flags &
             (TBT_FEC_FBK_F_CFG_ACK | TBT_FEC_FBK_F_SWITCH_DONE)))
        {
            tx->switch_state = TBT_FEC_SW_DRAIN;
            tx->switch_start_ns = now;
            spin_lock(&sess->lock);
            sess->adapt.state = TBT_FEC_ADAPT_DRAIN;
            spin_unlock(&sess->lock);
            return;
        }
        if (now - tx->switch_start_ns >= 500000000ULL)
        {
            tbt_fec_tx_switch_abort(sess, tx, true);
            return;
        }
        if (tx->switch_retries < 3 &&
            now - tx->switch_cfg_ns >= 50000000ULL)
        {
            tbt_fec_tx_send_cfg(sess, tx, TBT_FEC_CFG_F_APPLY);
            tx->switch_retries++;
            tx->switch_cfg_ns = now;
        }
        break;
    case TBT_FEC_SW_DRAIN:
        if (tx->peer_est_valid && tx->peer_epoch == tx->switch_epoch &&
            (tx->peer_flags &
             (TBT_FEC_FBK_F_CFG_NACK | TBT_FEC_FBK_F_ROLLBACK)))
        {
            tbt_fec_tx_switch_abort(sess, tx, false);
            return;
        }
        if (tx->peer_est_valid && tx->peer_epoch == tx->switch_epoch &&
            (tx->peer_flags & TBT_FEC_FBK_F_SWITCH_DONE) &&
            READ_ONCE(tx->peer_pkts) >= tx->switch_pkt)
        {
            tbt_fec_tx_swap(sess, tx);
            return;
        }
        if (now - tx->switch_start_ns >= 2000000000ULL)
        {
            tbt_fec_tx_switch_abort(sess, tx, true);
            return;
        }
        break;
    case TBT_FEC_SW_COOLDOWN:
        if (now - tx->switch_start_ns >= 5000000000ULL)
            tx->switch_state = TBT_FEC_SW_STEADY;
        break;
    default:
        break;
    }
}

/* One flow leaves the switching role. The round counter is advanced exactly
 * once here, so a flow that is killed mid-exchange cannot hold the round. */
static void tbt_fec_alloc_flow_done(struct tbt_session *sess,
                                    struct tbt_fec_flow *f, u32 epoch,
                                    bool ok)
{
    struct tbt_fec_tx *tx = f->tx;

    spin_lock(&sess->lock);
    if (sess->fec_alloc_round && epoch == sess->fec_alloc_epoch)
    {
        sess->fec_alloc_finished++;
        if (!ok)
            sess->fec_alloc_failed = true;
    }
    spin_unlock(&sess->lock);
    if (tx)
    {
        spin_lock_bh(&tx->alloc_lock);
        tx->alloc_role = 0;
        tx->alloc_hold = false;
        tx->alloc_fail = !ok;
        spin_unlock_bh(&tx->alloc_lock);
    }
}

/* Far side: stop the TX at the peer's request, send ACK, then swap when the
 * peer reports its RX applied at our P0. All allocation happens here, in the
 * TX work, never in carrier RX. */
static void tbt_fec_alloc_responder_tick(struct tbt_session *sess,
                                         struct tbt_fec_flow *f)
{
    struct tbt_fec_tx *tx = f->tx;
    struct tbt_fec_alloc_flow target;
    bool abort, req, commit, ack_resend, committed_resend;
    bool role;

    if (!tx)
        return;
    spin_lock_bh(&tx->alloc_lock);
    abort = tx->alloc_abort_pending;
    tx->alloc_abort_pending = false;
    req = tx->alloc_remote_req;
    commit = tx->alloc_commit_pending;
    tx->alloc_commit_pending = false;
    ack_resend = tx->alloc_ack_resend;
    tx->alloc_ack_resend = false;
    committed_resend = tx->alloc_committed_resend;
    tx->alloc_committed_resend = false;
    target = tx->alloc_target;
    role = tx->alloc_role;
    spin_unlock_bh(&tx->alloc_lock);

    if (abort)
    {
        /* The RX work restores its plane and sends the ABORT ACK. Release
         * the TX hold here without touching the old mask. */
        spin_lock_bh(&tx->alloc_lock);
        tx->alloc_hold = false;
        tx->alloc_remote_req = false;
        tx->alloc_ack_sent = false;
        tx->alloc_committed = false;
        tx->alloc_role = 0;
        spin_unlock_bh(&tx->alloc_lock);
        atomic64_inc(&sess->fec_alloc_abort);
        return;
    }
    if (req && !tx->alloc_ack_sent)
    {
        if (!tx->alloc_hold && !tbt_fec_alloc_tx_stop(sess, f))
        {
            if (debug)
                pr_info_ratelimited("tbtun: fec alloc responder waiting to stop sid=%llu switch=%u hold=%u\n",
                                    f->sid, tx->switch_state,
                                    tx->alloc_hold);
            tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(5));
            return;
        }
        spin_lock_bh(&tx->alloc_lock);
        target = tx->alloc_target;
        tx->alloc_remote_req = false;
        tx->alloc_ack_sent = true;
        spin_unlock_bh(&tx->alloc_lock);
        (void)tbt_fec_send_alloc(sess, f, TBT_FEC_ALLOC_F_ACK,
                                 target.epoch, target.mask, target.pkt,
                                 target.block);
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(5));
    }
    else if (ack_resend && tx->alloc_ack_sent)
    {
        (void)tbt_fec_send_alloc(sess, f, TBT_FEC_ALLOC_F_ACK,
                                 target.epoch, target.mask, target.pkt,
                                 target.block);
    }
    if (commit && tx->alloc_ack_sent)
    {
        if (!tbt_fec_tx_alloc_swap(sess, f))
        {
            pr_warn_ratelimited("tbtun: fec alloc responder swap failed sid=%llu groups=%u\n",
                                f->sid, target.groups);
            tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
            return;
        }
        spin_lock_bh(&tx->alloc_lock);
        target = tx->alloc_target;
        tx->alloc_committed = true;
        tx->alloc_commit_pending = false;
        tx->alloc_role = 0;
        spin_unlock_bh(&tx->alloc_lock);
        (void)tbt_fec_send_alloc(sess, f, TBT_FEC_ALLOC_F_COMMITTED,
                                 target.epoch, target.mask, target.pkt,
                                 target.block);
        atomic64_inc(&sess->fec_alloc_done);
        return;
    }
    if (committed_resend && tx->alloc_committed)
        (void)tbt_fec_send_alloc(sess, f, TBT_FEC_ALLOC_F_COMMITTED,
                                 target.epoch, target.mask, target.pkt,
                                 target.block);
    if (role)
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(10));
}

/* Near side: drive one flow's exchange. The FSM is the only writer of the
 * state; carrier RX only sets event flags. Keep it out of line: the TX work
 * body's flow snapshot already takes half of the 2 KiB frame budget. */
static noinline void tbt_fec_alloc_tx_tick(struct tbt_session *sess,
                                           struct tbt_fec_flow *f)
{
    struct tbt_fec_tx *tx = f->tx;
    struct tbt_fec_alloc_flow target, peer;
    struct tbt_fec_alloc_sm sm;
    u64 now = ktime_get_ns();
    u32 act;
    u8 role;
    bool ok;

    if (!tx)
        return;
    if (!tbt_fec_flow_live(READ_ONCE(f->state)))
        return;
    spin_lock_bh(&tx->alloc_lock);
    role = tx->alloc_role;
    spin_unlock_bh(&tx->alloc_lock);
    if (!role)
        return;
    if (role == 2)
    {
        tbt_fec_alloc_responder_tick(sess, f);
        return;
    }
    /* The exchange is owned by the per-flow state machine, never by the
     * session round that started it. An ABORT while the peer already applied
     * (or committed) its TX would resume the old mask against a new plane,
     * and clearing alloc_role before ABORT|ACK lets the pump send old-mask
     * records the peer can no longer place. The SM aborts on its own only in
     * REQ, before any ACK, where the peer has not armed anything. */
    if (tx->alloc.state == TBT_FEC_ALLOC_FAILED)
    {
        /* Safety net for an engine that reached FAILED without running the
         * FINISH action: release the role/hold, or every later plan is
         * refused and the regrow stage stalls again. */
        tbt_fec_alloc_flow_done(sess, f, tx->alloc_target.epoch, false);
        return;
    }
    if (!tx->alloc_hold && tx->alloc.state == TBT_FEC_ALLOC_REQ)
    {
        if (!tbt_fec_alloc_tx_stop(sess, f))
        {
            if (debug)
                pr_info_ratelimited("tbtun: fec alloc initiator waiting to stop sid=%llu switch=%u\n",
                                    f->sid, tx->switch_state);
            tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(5));
            return;
        }
    }
    spin_lock_bh(&tx->alloc_lock);
    /* The exchange is not stalled while our RX is still draining the old
     * plane toward the peer's switch point. On a high-BDP WAN that drain can
     * outlast the round timeout (5 MiB at 300 ms RTT took 3 s), so renew the
     * deadline on real delivery progress. A silent peer never advances
     * next_deliver, so the timeout still bounds it. */
    if (f->rx)
    {
        u64 next = READ_ONCE(f->rx->next_deliver);

        if (next > tx->alloc.rx_progress)
        {
            tx->alloc.rx_progress = next;
            if (tx->alloc.state == TBT_FEC_ALLOC_WAIT ||
                tx->alloc.state == TBT_FEC_ALLOC_COMMIT)
                tx->alloc.start_ns = now;
        }
    }
    act = tbt_fec_alloc_sm_step(&tx->alloc, now,
                                READ_ONCE(sess->closing) ||
                                    READ_ONCE(sess->fec_stopping));
    target = tx->alloc_target;
    peer = tx->alloc_peer;
    sm = tx->alloc;
    spin_unlock_bh(&tx->alloc_lock);

    if (debug && act)
        pr_info("tbtun: alloc fsm t=%llu sid=%llu role=%u state=%u act=%#x ack=%u pdone=%u pcomm=%u swapped=%u dsent=%u target=%llu/%llu\n",
                sess->tunnel ? sess->tunnel->cfg.id : 0, f->sid, role, sm.state,
                act, sm.ack, sm.peer_done, sm.peer_committed, sm.tx_swapped,
                sm.done_sent, target.pkt, target.block);
    if (act & TBT_FEC_ALLOC_ACT_REQ)
    {
        (void)tbt_fec_send_alloc(sess, f, 0, target.epoch, target.mask,
                                 target.pkt, target.block);
        atomic64_inc(&sess->fec_alloc_retry);
    }
    if (act & TBT_FEC_ALLOC_ACT_ABORT)
    {
        /* No peer state has committed: restore the old mask at the same
         * P0. The far rolls its armed RX back on this record. A failed
         * enqueue must not be counted as sent; keep the SM in ABORT and
         * let the next tick retry, or the peer's TX stays stopped. */
        if (tbt_fec_send_alloc(sess, f, TBT_FEC_ALLOC_F_ABORT,
                               target.epoch, f->group_mask, target.pkt,
                               target.block))
        {
            tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
            return;
        }
        atomic64_inc(&sess->fec_alloc_abort);
    }
    if (act & TBT_FEC_ALLOC_ACT_SWAP)
    {
        if (!tbt_fec_tx_alloc_swap(sess, f))
        {
            pr_warn_ratelimited("tbtun: fec alloc initiator swap failed sid=%llu groups=%u\n",
                                f->sid, target.groups);
            tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
            return;
        }
        spin_lock_bh(&tx->alloc_lock);
        tbt_fec_alloc_sm_tx_swapped(&tx->alloc, now);
        spin_unlock_bh(&tx->alloc_lock);
    }
    if (act & TBT_FEC_ALLOC_ACT_DONE)
        (void)tbt_fec_send_alloc(sess, f, TBT_FEC_ALLOC_F_DONE,
                                 target.epoch, target.mask, peer.pkt,
                                 peer.block);
    if (act & TBT_FEC_ALLOC_ACT_FINISH)
    {
        ok = sm.state == TBT_FEC_ALLOC_DONE;
        /* Both directions committed: the RX plane's rollback copy is dead
         * even if no new-plane record has arrived yet. */
        if (ok)
            tbt_fec_alloc_rx_release_old(sess, f);
        tbt_fec_alloc_flow_done(sess, f, target.epoch, ok);
        if (ok)
            atomic64_inc(&sess->fec_alloc_done);
        return;
    }
    if (role)
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(10));
}

/* Build the next SC-LDPC wire shape from a controller suggestion. The
 * current block/L/variant stay fixed; only n/m and the rate move. */
static bool tbt_fec_tx_sclc_next_shape(
    struct tbt_fec_tx *tx, const struct tbt_fec_adapt_sclc_out *sw,
    struct tbt_fec_shape *out)
{
    struct tbt_fec_cfg curcfg, nextcfg;
    struct tbt_fec_adapt_sclc_wire curw, nextw;
    struct tbt_fec_adapt_sclc_shape s;
    u16 shares = (u16)(tx->n + tx->m);

    memset(&curcfg, 0, sizeof(curcfg));
    curcfg.n = tx->n;
    curcfg.m = tx->m;
    curcfg.groups = tx->groups;
    curcfg.w = tx->shape_w;
    curcfg.k = tx->shape_k;
    curcfg.d = tx->shape_d;
    curcfg.codec_variant = tx->codec_variant;
    curcfg.L = tx->sclc_L;
    curcfg.block = tx->sclc_block;
    curcfg.rate_ppm = tx->rate_ppm;
    if (tbt_fec_adapt_sclc_wire_of_cfg(&curcfg,
                                      (u16)(tx->groups * shares), &curw) ||
        tbt_fec_adapt_sclc_shape(sw->shape.rate_pct, shares, tx->groups, &s))
        return false;
    nextw = curw;
    nextw.n = s.n;
    nextw.m = s.m;
    nextw.groups = s.groups;
    nextw.rate_pct = s.rate_pct;
    nextw.rate_ppm = s.rate_ppm;
    if (tbt_fec_adapt_sclc_wire_to_cfg(&nextw, sw->epoch,
                                       TBT_FEC_CFG_F_APPLY, &nextcfg))
        return false;
    memset(out, 0, sizeof(*out));
    out->n = nextcfg.n;
    out->m = nextcfg.m;
    out->groups = nextcfg.groups;
    out->w = nextcfg.w;
    out->k = nextcfg.k;
    out->d = nextcfg.d;
    out->codec_variant = nextcfg.codec_variant;
    out->L = nextcfg.L;
    out->block = nextcfg.block;
    out->rate_ppm = nextcfg.rate_ppm;
    return true;
}

/* The 250 ms control-law tick. The receiver measures p and reports it in
 * FEC_FBK; this side measures the residual repair rate from its own NACKs.
 * Mode 3 runs the SC-LDPC law; every other mode keeps the RLNC law. */
static void tbt_fec_tx_adapt_tick(struct tbt_session *sess,
                                  struct tbt_fec_tx *tx)
{
    struct tbt_fec_est_out o;
    struct tbt_fec_adapt_out sw;
    struct tbt_fec_adapt_sclc_est_out so;
    struct tbt_fec_adapt_sclc_out sws;
    u64 now = ktime_get_ns();
    u64 req, pkts;
    u32 r_ppm = 0;
    bool r_valid = false;
    bool sc = tx && tx->adapt_sclc && tbt_fec_sess_sclc(sess);

    if (!tx || (!tx->adapt && !tx->adapt_sclc) || READ_ONCE(tx->stopping) ||
        READ_ONCE(sess->closing))
        return;
    if (!READ_ONCE(sess->adaptive))
    {
        if (sc)
            tbt_fec_adapt_sclc_ctl_set_enabled(tx->adapt_sclc, false, now);
        if (tx->switch_state == TBT_FEC_SW_CFG_SENT ||
            tx->switch_state == TBT_FEC_SW_DRAIN)
            tbt_fec_tx_switch_abort(sess, tx, true);
        else if (tx->switch_state == TBT_FEC_SW_COOLDOWN)
            tx->switch_state = TBT_FEC_SW_STEADY;
        return;
    }
    if (sc)
        tbt_fec_adapt_sclc_ctl_set_enabled(tx->adapt_sclc, true, now);
    tbt_fec_tx_switch_tick(sess, tx);
    if (tx->switch_state != TBT_FEC_SW_STEADY)
        return;

    req = tx->nack_req_shares;
    pkts = tx->data_pkts_sent;
    if (!tx->fail_win_ns)
    {
        tx->fail_win_ns = now;
        tx->fail_req_last = req;
        tx->fail_pkts_last = pkts;
    }
    else if (now - tx->fail_win_ns >= 1000000000ULL)
    {
        u64 dreq = req - tx->fail_req_last;
        u64 dpkts = pkts - tx->fail_pkts_last;

        if (dpkts)
        {
            u64 r = dreq * 1000000ull / dpkts;

            r_ppm = r > 1000000ull ? 1000000u : (u32)r;
            r_valid = true;
        }
        tx->fail_win_ns = now;
        tx->fail_req_last = req;
        tx->fail_pkts_last = pkts;
    }

    if (sc)
    {
        memset(&so, 0, sizeof(so));
        so.p_ppm = tx->peer_p_ppm;
        so.p_ewma_ppm = tx->peer_p_ppm;
        so.p_win_ppm = tx->peer_p_ppm;
        so.p_fast_ppm = tx->peer_p_fast_ppm;
        so.r_fail_ppm = r_ppm;
        so.r_valid = r_valid;
        so.codec_fail_ppm = tx->peer_codec_fail_ppm;
        so.sent_win = tx->peer_expected;
        so.miss_win = tx->peer_expected >= tx->peer_received
                          ? tx->peer_expected - tx->peer_received
                          : 0;
        so.supers_win = (tx->peer_flags & TBT_FEC_FBK_F_P_VALID) ? 8 : 0;
        so.p_valid = tx->peer_est_valid &&
                     (tx->peer_flags & TBT_FEC_FBK_F_P_VALID) != 0;
        so.pause_idle = !tx->peer_est_valid ||
                        now - tx->peer_est_ns > 500000000ULL;
        so.burst = so.p_fast_ppm && so.p_fast_ppm > so.p_ppm &&
                   so.p_fast_ppm > TBT_FEC_ADAPT_SCLC_BURST_PPM &&
                   so.p_fast_ppm >
                       so.p_ewma_ppm + TBT_FEC_ADAPT_SCLC_BURST_GAP_PPM;
        if (debug)
            pr_info_ratelimited("tbtun-diag: tx adapt tunnel=%llu p=%u fast=%u r=%u flags=%#x valid=%d idle=%d cur=%u busy=%u sw=%u exp=%u recv=%u\n",
                                (unsigned long long)sess->tunnel->cfg.id,
                                so.p_ppm, so.p_fast_ppm, so.r_fail_ppm,
                                tx->peer_flags, so.p_valid, so.pause_idle,
                                tx->adapt_sclc->cur, tx->adapt_sclc->busy,
                                tx->switch_state, so.sent_win, so.miss_win);
        if (!so.pause_idle)
            tbt_fec_adapt_sclc_ctl_commit_failed(tx->adapt_sclc, &so, now);
        tbt_fec_adapt_sclc_ctl_eval(tx->adapt_sclc, &so, now, &sws);
        if (sws.switch_now)
        {
            struct tbt_fec_shape shape;

            if (tbt_fec_tx_sclc_next_shape(tx, &sws, &shape))
            {
                tbt_fec_tx_switch_begin_shape(sess, tx, &shape, sws.epoch);
                if (tx->switch_state == TBT_FEC_SW_STEADY)
                    tbt_fec_adapt_sclc_ctl_abort(tx->adapt_sclc);
            }
            else
                tbt_fec_adapt_sclc_ctl_abort(tx->adapt_sclc);
        }
        spin_lock(&sess->lock);
        sess->adapt.class = tx->adapt_sclc->cur;
        sess->adapt.r_fail_ppm = r_ppm;
        spin_unlock(&sess->lock);
        return;
    }

    memset(&o, 0, sizeof(o));
    o.p_ppm = tx->peer_p_ppm;
    o.p_ewma_ppm = tx->peer_p_ppm;
    o.sent_win = tx->peer_expected;
    o.miss_win = tx->peer_expected >= tx->peer_received ?
                     tx->peer_expected - tx->peer_received :
                     0;
    o.r_fail_ppm = r_ppm;
    o.r_valid = r_valid;
    o.p_valid = tx->peer_est_valid &&
                (tx->peer_flags & TBT_FEC_FBK_F_P_VALID) &&
                tx->peer_expected >= TBT_FEC_EST_MIN_SHARES;
    o.pause_idle = !tx->peer_est_valid ||
                   now - tx->peer_est_ns > 500000000ULL;
    tbt_fec_adapt_eval(tx->adapt, &o, now, &sw);
    if (sw.switch_now)
    {
        /* The control law names the 256-carrier family (groups=32); the data
         * plane owns the session's actual geometry (32/64/256 carriers). */
        sw.shape.groups = tx->groups;
        tbt_fec_tx_switch_begin(sess, tx, &sw);
        /* The law marked itself busy when it issued the suggestion. A begin
         * that could not start (no stream, closing, geometry) must release
         * it, or every later evaluation returns R_BUSY forever. */
        if (tx->switch_state == TBT_FEC_SW_STEADY)
            tbt_fec_adapt_abort(tx->adapt);
    }
    spin_lock(&sess->lock);
    sess->adapt.class = tx->adapt->cur;
    sess->adapt.r_fail_ppm = r_ppm;
    spin_unlock(&sess->lock);
}

/* Slow FEC cadence: the 500 ms INIT retry, the 250 ms adaptation tick and
 * the 10 ms wait for the crypto TX key. The entry owns no data-plane state;
 * it asks w_fec_tx for one pass, and that body arms this work again while
 * the cadence is still needed. Keeping every delay above the short cut on a
 * separate work leaves w_fec_tx free for the 0 ms wake and the 2 ms block
 * cut, which tbt_session_arm() must never place behind a queued slow timer
 * (measured: a 200-byte echo waited out the 500 ms INIT retry in each
 * direction, 1113 ms round trip). */
static void tbt_fec_tick_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_tick);
    bool owned = test_and_clear_bit(TBT_SW_FEC_TICK, &sess->sched);

    if (!READ_ONCE(sess->closing) && !READ_ONCE(sess->fec_stopping))
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    if (owned)
        tbt_session_put(sess);
}

void tbt_fec_tx_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_tx);
    bool owned = test_and_clear_bit(TBT_SW_FEC_TX, &sess->sched);

    /* The run word lives on the session, not on a flow's TX plane: detach can
     * swap the primary while a body is running, and two bodies over the same
     * flow would corrupt its packetizer. */
    if (atomic_fetch_or(1u << TBT_FEC_RUN_BIT, &sess->fec_tx_run) &
        (1u << TBT_FEC_RUN_BIT))
    {
        atomic_fetch_or(1u << TBT_FEC_AGAIN_BIT, &sess->fec_tx_run);
        goto out;
    }
    tbt_fec_tx_work_body(sess);
    if (sess->n_fec_flows <= 1)
    {
        struct tbt_fec_flow *primary = tbt_fec_primary_get(sess);

        if (primary)
        {
            tbt_fec_tx_adapt_tick(sess, primary->tx);
            tbt_fec_flow_put(primary);
        }
    }
    atomic_fetch_and(~(1u << TBT_FEC_RUN_BIT), &sess->fec_tx_run);
    if (atomic_fetch_and(~(1u << TBT_FEC_AGAIN_BIT), &sess->fec_tx_run) &
        (1u << TBT_FEC_AGAIN_BIT))
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    /* The control law needs a fixed 250 ms cadence even when no share or ACK
     * wakes the packetizer. */
    if (READ_ONCE(sess->adaptive) && !READ_ONCE(sess->fec_stopping) &&
        !READ_ONCE(sess->closing) && sess->n_fec_flows <= 1)
        tbt_session_arm(sess, TBT_SW_FEC_TICK, msecs_to_jiffies(250));
out:
    if (owned)
        tbt_session_put(sess);
}

size_t tbt_fec_tx_submit(struct tbt_stream *st, struct sk_buff *skb,
                         unsigned int off, size_t len)
{
    struct tbt_session *sess = st->sess;
    struct tbt_fec_tx *tx = st->fec_flow ? st->fec_flow->tx : NULL;
    u32 take;

    if (!tx)
        return 0;
    spin_lock(&tx->lock);

    take = tbt_fec_ring_put(tx, skb, off, (u32)min_t(size_t, len, U32_MAX));
    if (take)
    {
        /* The FIN path freezes tx_seq after the ring is drained; keeping the
         * accepted byte count here is what makes that FIN name the real end
         * of the stream and lets the ACK clamp stay exact. */
        WRITE_ONCE(st->tx_seq, READ_ONCE(st->tx_seq) + take);
    }
    if (take < len)
    {
        WRITE_ONCE(st->tx_paused, true);
        /* The source ring is full while the sender waits for a carrier:
         * this is the last-resort backpressure, not a dropped share. */
        if (READ_ONCE(tx->carrier_wait) &&
            !READ_ONCE(tx->backpressure_counted))
        {
            WRITE_ONCE(tx->backpressure_counted, true);
            atomic64_inc(&sess->fec_backpressure);
        }
    }
    spin_unlock(&tx->lock);
    if (take)
        atomic64_add(take, &sess->tx_submit_bytes);
    tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    return take;
}

bool tbt_fec_tx_idle(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;
    struct tbt_fec_flow *f = st->fec_flow;
    bool idle;

    if (!f || !f->tx)
        return true;
    idle = tbt_fec_tx_is_idle(sess, f->tx);

    /* The FIN may proceed while replay records are still queued; count how
     * often the old all-queues gate would have held it. */
    if (idle && READ_ONCE(sess->fec_replay_queued))
        atomic64_inc(&sess->fec_fin_wait_replay);
    return idle;
}

int tbt_fec_tx_flush(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;
    struct tbt_fec_flow *f = st->fec_flow;
    struct tbt_fec_tx *tx = f ? f->tx : NULL;

    if (!tx)
        return -ENODEV;
    if (READ_ONCE(sess->fec_stopping))
        return -ESHUTDOWN;
    /* The packetizer is owned by tbt_fec_tx_work; a flush from the stream
     * close path only raises a flag, so two CPUs never touch tx->pkt. */
    WRITE_ONCE(tx->flush_req, true);
    tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    return 0;
}

/* ------------------------------------------------------------------ *
 * RX
 * ------------------------------------------------------------------ */

int tbt_fec_record_len(const u8 *buf, size_t have, size_t *need)
{
    u16 len;

    if (have < TBT_FEC_HDR_LEN)
        return TBT_REC_NEED_MORE;
    len = (u16)(buf[TBT_FEC_H_PAYLEN] | ((u16)buf[TBT_FEC_H_PAYLEN + 1] << 8));
    if (len > TBT_FEC_PAYLOAD_MAX)
        return TBT_REC_PROTO;
    *need = TBT_FEC_HDR_LEN + len;
    /* Defence in depth: the parser buffer must hold the whole record, or it
     * would fill up and never complete (p->have == sizeof(p->buf)). */
    if (*need > TBT_PARSER_BUF)
        return TBT_REC_PROTO;
    return have < *need ? TBT_REC_NEED_MORE : TBT_REC_OK;
}

/* Re-slice the shape-invariant buffer for the current n/m. A switch keeps the
 * allocation and moves only these two pointers. */
static void tbt_fec_blk_slice(const struct tbt_fec_rx *rx,
                              struct tbt_fec_blk *b)
{
    b->data = b->buf;
    b->parity = b->buf + (size_t)rx->n * rx->s;
}

static void tbt_fec_blk_reset(struct tbt_session *sess,
                              struct tbt_fec_flow *f, struct tbt_fec_blk *b,
                              u64 block_id)
{
    struct tbt_fec_rx *rx = f->rx;

    /* A block that never became ready is dropped with its slot: its DATA
     * shares are not evidence for p (they would understate it), but a high
     * abandoned rate freezes the controller. Carrier RX counts here under
     * rx->lock; w_fec_rx folds the delta into the estimator. */
    if (rx && b->used && !b->ready && !b->empty && !b->judged)
        atomic64_add(b->nreal ? b->nreal : rx->n, &rx->abandoned_shares);
    b->used = true;
    b->ready = false;
    b->nreal = 0;
    b->ndata = 0;
    b->nparity = 0;
    b->count_real = 0xffff;
    b->decoding = false;
    b->decode_tries = 0;
    b->decode_next_ns = 0;
    b->empty = false;
    b->win_len_set = false;
    b->short_len = 0;
    b->short_unknown = false;
    b->first_gap_ns = 0;
    b->miss_mask = 0;
    memset(b->miss_bits, 0, sizeof(b->miss_bits));
    memset(b->codec_fail_bits, 0, sizeof(b->codec_fail_bits));
    b->judged = false;
    b->block_id = block_id;
    b->sid = 0;
    memset(b->dpresent, 0, TBT_FEC_MAX_SHARES);
    memset(b->ppresent, 0, TBT_FEC_MAX_SHARES);
    memset(b->dlen, 0, TBT_FEC_MAX_SHARES * sizeof(u16));
}

/* Real length of this block, or 0 when the group has no packet in it. The
 * parity's win_len is authoritative for the block; the stream's total packet
 * count is the second source, used when every parity record of this block was
 * lost. A known value never grows: an inconsistent peer cannot turn padding
 * back into data. */
static u16 tbt_fec_blk_real_len(const struct tbt_fec_rx *rx, u16 group,
                                const struct tbt_fec_blk *b)
{
    bool has_data = false;
    u16 i;

    /* A marker says the group has no packet in this block; nothing in it is
     * ever delivered, whatever the stream-length fallback would say. */
    if (b->empty)
        return 0;
    for (i = 0; i < rx->n; i++)
        if (b->dpresent[i])
        {
            has_data = true;
            break;
        }
    return tbt_fec_blk_real_len_from_tail_base(
        b->nreal, rx->n, rx->total_known, rx->total_pkts, rx->base_pkt,
        rx->base_block, rx->groups, b->block_id, group, has_data);
}

/* Lazy recount: ndata is the number of present shares in [0, real). It only
 * changes when the block's real length changes (the first PARITY, or a FIN
 * that names the stream end), so the O(n) scan runs on those edges only. */
static void tbt_fec_blk_counts(const struct tbt_fec_rx *rx, u16 group,
                               struct tbt_fec_blk *b)
{
    u16 real = tbt_fec_blk_real_len(rx, group, b);
    u16 i, nd = 0;

    if (b->count_real == real)
        return;
    for (i = 0; i < real; i++)
        nd = (u16)(nd + (b->dpresent[i] ? 1 : 0));
    b->ndata = nd;
    b->count_real = real;
}

/* Decode one claimed block outside rx->lock. Present slots point into
 * b->data; missing slots point into rx->rec; padding points at rx->zero. The
 * codec only writes the missing slots, so an arrived share is never
 * overwritten. Returns 0 when ready, -EAGAIN when more shares are needed.
 *
 * The caller has snapshotted dpresent/ppresent and set b->decoding, so no
 * carrier can place another share into this block while the codec runs. */
static int tbt_fec_blk_decode(struct tbt_session *sess,
                              struct tbt_fec_flow *f, u16 group,
                              struct tbt_fec_blk *b)
{
    struct tbt_fec_rx *rx = f->rx;
    bool block_code = tbt_fec_grouped_mode(rx->codec) == 3;
    u16 real, i, k, missing = 0, have_parity = 0;
    int rc;

    real = tbt_fec_blk_real_len(rx, group, b);
    if (!real)
        return 0; /* this group has no packet in this block */
    for (i = 0; i < rx->n; i++)
        if (!rx->dpresent_snap[i])
            missing++;
    for (i = 0; i < rx->m; i++)
        if (rx->ppresent_snap[i])
            have_parity++;
    if (!missing)
        return 0;
    /* RLNC needs one independent row per missing share; an SC-LDPC block code
     * needs any parity at all and decides itself how much it can recover, so
     * the count test would both reject decodable blocks and admit blocks the
     * parity indices do not cover. */
    if (!rx->codec || (!block_code && have_parity < missing) ||
        (block_code && !have_parity))
        return -EAGAIN;
    /* A recovered share has no payload_len of its own, so the block's shape
     * must name every length the delivery walk will use. Only the last real
     * position may be shorter than S, and the parity's epoch must state that
     * same length; every earlier position is S. A short share anywhere else,
     * or an epoch that disagrees with the share it describes, leaves a
     * recovered slot without a length, and a guess would hand the application
     * the block's zero padding. Refuse the block instead of guessing. */
    for (i = 0; i < real; i++)
    {
        u16 dl = b->dlen[i];

        if (!rx->dpresent_snap[i] || !dl || dl >= rx->s)
            continue;
        if (i + 1 != real || b->short_len != dl)
        {
            if (!b->short_unknown)
            {
                b->short_unknown = true;
                atomic64_inc(&sess->fec_short_unknown);
            }
            return -EAGAIN;
        }
    }
    for (i = 0, k = 0; i < real; i++)
        rx->data_ptrs[i] = rx->dpresent_snap[i] ?
            b->data + (size_t)i * rx->s :
            rx->rec + (size_t)(k++) * rx->s;
    for (i = real; i < rx->n; i++)
        rx->data_ptrs[i] = rx->zero; /* padding is known zero */
    for (i = 0; i < rx->m; i++)
        rx->parity_ptrs[i] = rx->ppresent_snap[i] ?
            b->parity + (size_t)i * rx->s : rx->zero;

    tbt_fec_grouped_set_nonce(rx->codec, group,
                              tbt_fec_group_nonce(tbt_fec_base_nonce(sess),
                                                  group, b->block_id));
    atomic64_inc(&sess->fec_decode_blocks);
    rc = tbt_fec_grouped_decode(rx->codec, group, rx->data_ptrs,
                                rx->parity_ptrs, rx->dpresent_snap,
                                rx->ppresent_snap, rx->n);
    if (rc == TBT_FEC_RLNC_ERR_NOMEM)
        atomic64_inc(&sess->fec_decode_nomem);
    if (rc > 0)
        atomic64_add(rc, &sess->fec_decode_recovered);
    if (rc < 0)
        return -EAGAIN;
    /* A block codec returns how much it recovered, not whether the block is
     * whole. The commit path checks is_known() per share, so a partial
     * recovery is committed and the block stays open for the rest. Refusing
     * the whole block here threw the parity recovery away and forced a NACK
     * for every share the codec had already restored: the 0.3.0 first-connect
     * repair took 102 s on one block because of it. */
    return 0;
}

/* next_deliver is the packet frontier the sender's credit window uses.
 * deliver_bytes is the stream-relative byte count this receiver has actually
 * handed over; it is the only value the sender may use as tx_ack, because a
 * packet frontier can name bytes that no delivered share carried. */
static void tbt_fec_ack(struct tbt_session *sess, struct tbt_fec_flow *f,
                        struct tbt_stream *st, u64 next_deliver,
                        u64 deliver_bytes)
{
    struct tbt_fec_hdr h;
    struct tbt_fec_ack a;
    u8 body[TBT_FEC_ACK_LEN];
    u8 rec[TBT_FEC_HDR_LEN + TBT_FEC_ACK_LEN];
    u16 len;

    memset(&a, 0, sizeof(a));
    a.next_deliver = next_deliver;
    a.deliver_bytes = deliver_bytes;
    /* The credit is K_eff-2 super-blocks: the ring holds K, and two are
     * slack so a block the receiver can still NACK is never evicted. The
     * receiver's arena `slots` is the other bound (a sender inside the
     * credit cannot overrun it), and TBT_FEC_CREDIT_MAX caps the packets
     * (8 MiB at S=8192). The sender applies the same cap. */
    {
        struct tbt_fec_rx *rx = f->rx;

        a.credit_packets = tbt_fec_credit_packets(
            rx->groups, rx->n, rx->replay_k, rx->slots, TBT_FEC_CREDIT_MAX);
        /* The peer uses this as the replay round trip: a share asked again
         * before it can arrive would only duplicate the repair. The sample
         * is the NACK scan's carrier RTT gauge, 0 before the first one. */
        a.rtt_us = READ_ONCE(rx->rtt_us);
    }
    if (tbt_fec_ack_pack(&a, body))
        return;
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_ACK;
    h.seq = next_deliver;
    h.payload_len = TBT_FEC_ACK_LEN;
    h.conv = TBT_FEC_CONV;
    h.sid = f ? f->sid : (st ? st->sid : 0);
    if (tbt_fec_make_record(sess, &h, body, rec, &len))
        return;
    if (tbt_fec_send_record(sess, 0, rec, len, true, false, f))
        return;
    atomic64_inc(&sess->fec_ack_tx);
}

/* Ask the peer to replay the shares missing from one super-block. The NACK
 * is a control record: it may ride any up, non-stalled carrier and it is
 * queued at the head, ahead of new shares. */
static int tbt_fec_send_nack(struct tbt_session *sess,
                             struct tbt_fec_flow *f, u64 block_id,
                             const struct tbt_fec_nack *n)
{
    struct tbt_fec_hdr h;
    u8 body[TBT_FEC_NACK_LEN];
    u8 rec[TBT_FEC_HDR_LEN + TBT_FEC_NACK_LEN];
    u16 len;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_NACK;
    h.block_id = block_id;
    h.payload_len = TBT_FEC_NACK_LEN;
    h.conv = TBT_FEC_CONV;
    h.sid = f->sid;
    /* tbt_fec_nack_pack() returns the positive body length on success. Only
     * a negative value is an error; treating 64 as an error kept every NACK
     * off the wire (fec_nack_tx=0, fec_nack_drop climbing). */
    if (tbt_fec_nack_pack(n, body) != TBT_FEC_NACK_LEN)
        return -EINVAL;
    if (tbt_fec_make_record(sess, &h, body, rec, &len))
        return -EINVAL;
    {
        int err = tbt_fec_send_record(sess, 0, rec, len, true, false, f);

        if (!err)
            WRITE_ONCE(sess->fec_repair_last_ns, ktime_get_ns());
        return err;
    }
}

/* A closed block has no packet in `group`: its empty marker is the truth.
 * Answer a NACK for a share that was never stored with that marker, so the
 * receiver can mark the group empty instead of waiting for a share that will
 * never exist. The marker is also stored in the replay ring, so a later NACK
 * for share n hits it. */
static int tbt_fec_send_empty_marker(struct tbt_session *sess,
                                     struct tbt_fec_tx *tx, u16 group,
                                     u64 block_id)
{
    struct tbt_fec_hdr h;
    u8 rec[TBT_FEC_HDR_LEN];
    u16 len;

    if (!tx || !tx->n || !tx->m)
        return -EINVAL;
    if (debug >= 2)
        pr_info("tbtun: fec NACK empty reply block=%llu group=%u share=%u win_len=0\n",
                block_id, group, tx->n);
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_PARITY;
    h.share_index = tx->n;
    h.n = (u8)tx->n;
    h.m = (u8)tx->m;
    h.block_id = block_id;
    h.seq = tbt_fec_seq_of_block_pkt_base(block_id, group, 0, tx->base_pkt,
                                          tx->base_block, tx->groups, tx->n);
    h.win_len = 0;
    h.payload_len = 0;
    h.epoch = 0;
    h.conv = TBT_FEC_CONV;
    h.sid = tx->sid;
    if (tbt_fec_make_record(sess, &h, NULL, rec, &len))
        return -EINVAL;
    /* The reply is built fresh every time and is never stored: a marker for
     * an old block id shares its ring slot with block_id + K, so storing it
     * would reclaim a block the receiver may still be waiting for. */
    if (debug >= 2)
        pr_info("tbtun: fec empty reply tx block=%llu group=%u share=%u cur=%llu\n",
                block_id, group, tx->n, READ_ONCE(tx->cur_block));
    return tbt_fec_send_record(sess, 0, rec, len, true, false, tx->flow);
}

/* One failed NACK, split by error code. The masks are logged once per rate
 * window so a field run can tell a bad shape from an allocation failure. */
static void tbt_fec_nack_drop(struct tbt_session *sess, int err, u64 block_id,
                              const struct tbt_fec_nack *n)
{
    switch (err)
    {
    case -ENOMEM:
        atomic64_inc(&sess->fec_nack_enomem);
        break;
    case -EINVAL:
        atomic64_inc(&sess->fec_nack_einval);
        break;
    case -ENOSPC:
        atomic64_inc(&sess->fec_nack_enospc);
        break;
    default:
        atomic64_inc(&sess->fec_nack_eother);
        break;
    }
    atomic64_inc(&sess->fec_nack_drop);
    pr_info_ratelimited("tbtun: fec NACK drop err=%d block=%llu group=%*phN share=%*phN\n",
                        err, block_id, TBT_FEC_BITMAP_BYTES, n->group_mask,
                        TBT_FEC_BITMAP_BYTES, n->share_mask);
}

/* A NACK must not name a group or share the session shape does not have. */
static bool tbt_fec_bitmap_clear_from(const u8 bm[TBT_FEC_BITMAP_BYTES],
                                      u16 from)
{
    u16 i;

    for (i = from; i < TBT_FEC_MAX_SHARES; i++)
        if (tbt_fec_bitmap_test(bm, i))
            return false;
    return true;
}

static struct tbt_fec_blk *tbt_fec_rx_blk(struct tbt_fec_rx *rx, u16 group,
                                          u64 block_id)
{
    return &rx->blk[(size_t)group * rx->slots + (block_id % rx->slots)];
}

/* Every group must have decoded block s before any byte of it is delivered.
 * In the final super-block a group with no packet is complete by definition.
 * The caller holds rx->lock: block slots are reset by a carrier as soon as
 * the window advances, and reading used/ready/block_id without the lock races
 * that reset. */
static bool tbt_fec_super_ready_locked(const struct tbt_fec_rx *rx, u64 s)
{
    u16 group;

    for (group = 0; group < rx->groups; group++)
    {
        struct tbt_fec_blk *b;

        b = tbt_fec_rx_blk((struct tbt_fec_rx *)rx, group, s);
        if (rx->total_known)
        {
            u64 final = tbt_fec_final_block_of_total_base(
                rx->total_pkts, rx->base_pkt, rx->base_block, rx->groups,
                rx->n);

            if (s == final &&
                !tbt_fec_group_tail_len_base(rx->total_pkts, rx->base_pkt,
                                             rx->base_block, rx->groups,
                                             rx->n, group))
            {
                /* The final super-block may leave a group with no packet.
                 * Skip it only when no block was ever placed for it; a block
                 * that does exist must still be ready, or its bytes would be
                 * skipped while the frontier advanced. */
                if (!b->used || b->block_id != s)
                    continue;
            }
        }
        /* A block placed for the old stream must not make the new stream's
         * super-block look ready: the delivery walk would advance its cursor
         * without handing over any byte. */
        if (!b->used || !b->ready || b->block_id != s ||
            !tbt_fec_blk_sid_match(b->sid, rx->stream_sid))
            return false;
    }
    return true;
}

/* Missing shares in the frontier super-block, for the reset log. The block
 * may not exist at all (a whole-block loss): every DATA share of every group
 * is then missing. The caller must not hold rx->lock. */
static u16 tbt_fec_frontier_missing(struct tbt_fec_rx *rx, u64 s)
{
    u16 g, i, missing = 0;

    spin_lock_bh(&rx->lock);
    for (g = 0; g < rx->groups; g++)
    {
        struct tbt_fec_blk *b = tbt_fec_rx_blk(rx, g, s);
        u16 real;

        if (!b->used || b->block_id != s)
        {
            missing = (u16)(missing + rx->n);
            continue;
        }
        if (b->ready)
            continue;
        real = tbt_fec_blk_real_len(rx, g, b);
        for (i = 0; i < real; i++)
            if (!b->dpresent[i])
                missing++;
        for (i = 0; i < rx->m; i++)
            if (!b->ppresent[i])
                missing++;
    }
    spin_unlock_bh(&rx->lock);
    return missing;
}

/* Name the state a FEC hole reset is about to throw away: the frontier, the
 * stream's known end, the missing share count, how long the walk has made no
 * progress, and the replay queue depth. The acceptance runs debug=0, so the
 * per-block dumps are off; this line is unconditional (ratelimited). */
static void tbt_fec_reset_diag(struct tbt_session *sess,
                               struct tbt_fec_flow *f, u64 s,
                               const char *why)
{
    struct tbt_fec_rx *rx = f->rx;
    u64 now = ktime_get_ns();
    u32 rq;

    spin_lock(&sess->fec_out_lock);
    rq = sess->fec_replay_queued;
    spin_unlock(&sess->fec_out_lock);
    pr_info_ratelimited("tbtun: fec reset diag why=%s super=%llu next=%llu total_known=%d total_pkts=%llu missing=%u repair_age_us=%llu replay_q=%u\n",
                        why, s, READ_ONCE(rx->next_super),
                        READ_ONCE(rx->total_known),
                        READ_ONCE(rx->total_pkts),
                        tbt_fec_frontier_missing(rx, s),
                        rx->progress_ns && now > rx->progress_ns
                            ? (now - rx->progress_ns) / NSEC_PER_USEC
                            : 0,
                        rq);
}

/* Deliver every ready super-block, in order, from the exact cursor the last
 * attempt stopped at. Only w_fec_rx calls this, so it is the only writer of
 * next_super / deliver_bytes / next_deliver / ack_deliver.
 *
 * -ENOMEM is a per-packet stop, not a lost packet: the successful prefix is
 * committed, (deliver_p, deliver_g) names the first packet that failed, and
 * next_super / next_deliver / ack_deliver do not move. The work item retries
 * in 2 ms and resumes from the cursor; no byte is skipped and no ACK runs
 * past a byte the application never received. */
static noinline void tbt_fec_deliver_est_fold(struct tbt_session *sess,
                                              struct tbt_fec_rx *rx, u64 s)
{
    bool sc_mode = rx->est_sclc && tbt_fec_grouped_mode(rx->codec) == 3 &&
                   rx->groups <= TBT_FEC_ADAPT_GROUPS;
    bool rlnc_mode = READ_ONCE(sess->adaptive) && rx->est &&
                     rx->groups <= TBT_FEC_ADAPT_GROUPS &&
                     (u16)(rx->n + rx->m) == TBT_FEC_ADAPT_SHARES;

    if (sc_mode || rlnc_mode)
    {
        u64 est_now = ktime_get_ns();
        u16 eg;

        for (eg = 0; eg < rx->groups; eg++)
        {
            struct tbt_fec_blk *eb;
            u16 real = 0;
            u32 miss = 0, fail = 0;
            u8 sc_miss[TBT_FEC_BITMAP_BYTES];
            bool unknown = false;

            spin_lock_bh(&rx->lock);
            eb = tbt_fec_rx_blk(rx, eg, s);
            if (!eb->used || eb->block_id != s || eb->judged)
            {
                spin_unlock_bh(&rx->lock);
                continue;
            }
            eb->judged = true;
            real = tbt_fec_blk_real_len(rx, eg, eb);
            miss = eb->miss_mask;
            if (sc_mode)
            {
                u16 si;

                memcpy(sc_miss, eb->miss_bits, TBT_FEC_BITMAP_BYTES);
                for (si = 0; si < real; si++)
                {
                    if (eb->dpresent[si])
                        continue;
                    tbt_fec_adapt_bitmap_set(sc_miss, si);
                    if (tbt_fec_adapt_bitmap_test(eb->codec_fail_bits,
                                                  si))
                        fail++;
                }
            }
            /* Without a parity cut and without the stream end, a block
             * that never filled n slots has an unknown real length: do
             * not estimate p from it. */
            unknown = !eb->win_len_set && !rx->total_known &&
                      eb->ndata < rx->n;
            spin_unlock_bh(&rx->lock);
            if (unknown)
            {
                if (sc_mode)
                    tbt_fec_adapt_sclc_est_unknown(rx->est_sclc,
                                                   est_now);
                else
                    tbt_fec_est_unknown(rx->est, est_now);
            }
            else if (real)
            {
                if (sc_mode)
                {
                    tbt_fec_adapt_sclc_est_block(
                        rx->est_sclc, est_now, eg, real, sc_miss,
                        rx->alive_bm);
                    if (fail)
                        tbt_fec_adapt_sclc_est_codec_fail(
                            rx->est_sclc, est_now, fail);
                }
                else
                    tbt_fec_est_block(rx->est, est_now, eg, real, miss,
                                      rx->alive_bm);
            }
        }
        if (sc_mode)
            tbt_fec_adapt_sclc_est_super(rx->est_sclc, est_now);
        else
            tbt_fec_est_super(rx->est, est_now);
    }
}

static int tbt_fec_deliver_ready(struct tbt_session *sess,
                                 struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_stream *st;
    bool delivered = false;
    bool blocked = false;
    const char *blocked_why = "?";

    if (READ_ONCE(rx->rebase_pending))
        return 0;
    if (READ_ONCE(rx->total_known) && !READ_ONCE(rx->total_pkts) &&
        !rx->deliver_partial)
        return 0;

    st = tbt_stream_get(sess, f->sid, false, false);
    if (!st)
        return 0;
    for (;;)
    {
        u64 s = rx->next_super;
        u64 base = tbt_fec_seq_of_block_pkt_base(
            s, 0, 0, rx->base_pkt, rx->base_block, rx->groups, rx->n);
        u64 off = rx->deliver_bytes;
        u16 p, group, start_p, start_g;
        bool ready;
        bool known = READ_ONCE(rx->total_known);
        u64 total_pkts = READ_ONCE(rx->total_pkts);

        blocked = false;
        /* Teardown: stop handing bytes to a stream that is being destroyed.
         * The ACK below still covers what was delivered before the flag. */
        if (READ_ONCE(rx->stopping))
            break;
        if (known)
        {
            u64 final = tbt_fec_final_block_of_total_base(
                total_pkts, rx->base_pkt, rx->base_block, rx->groups,
                rx->n);

            if (s > final)
                break;
        }
        spin_lock_bh(&rx->lock);
        ready = tbt_fec_super_ready_locked(rx, s);
        if (!ready)
        {
            /* Name the group whose block holds the whole stream. This is a
             * per-frontier diagnostic: a v7 flow is per stream, and a detached
             * or aborted flow must not fill the log (0.2.2 saw 20-40 lines per
             * abort). Keep it behind debug. */
            u64 now_diag = ktime_get_ns();

            for (group = 0; group < rx->groups; group++)
            {
                struct tbt_fec_blk *sb = tbt_fec_rx_blk(rx, group, s);
                u16 k, have = 0;

                if (sb->used && sb->ready && sb->block_id == s)
                    continue;
                for (k = 0; k < rx->n; k++)
                    have = (u16)(have + (sb->dpresent[k] ? 1 : 0));
                if (debug)
                    pr_info_ratelimited("tbtun: fec super %llu stuck: next=%llu total_known=%d total_pkts=%llu next_deliver=%llu deliver_bytes=%llu group=%u/%u used=%d ready=%d id=%llu nreal=%u shares=%u/%u parity=%u wset=%d empty=%d short=%u cnt=%d gap_us=%llu\n",
                                        s, READ_ONCE(rx->next_super), known,
                                        total_pkts,
                                        READ_ONCE(rx->next_deliver),
                                        READ_ONCE(rx->deliver_bytes), group,
                                        rx->groups, sb->used, sb->ready,
                                        sb->block_id, sb->nreal, have, rx->n,
                                        (unsigned)rx->m, sb->win_len_set,
                                        sb->empty, sb->short_len,
                                        (int)sb->count_real,
                                        sb->first_gap_ns &&
                                                now_diag > sb->first_gap_ns
                                            ? (now_diag - sb->first_gap_ns) /
                                                  NSEC_PER_USEC
                                            : 0);
                break;
            }
        }
        spin_unlock_bh(&rx->lock);
        if (!ready)
        {
            /* The walk stopped at a super-block that is not ready. The NACK
             * scan above asks the peer for the missing shares while they are
             * in its replay window; the carrier TCP still holds the rest.
             * Reset only when every carrier TCP has failed: an independent
             * timer here would make the tunnel reset in cases where a direct
             * TCP stream would keep retransmitting. */
            u64 now = ktime_get_ns();
            u64 budget = (u64)TBT_FEC_STALL_MS * NSEC_PER_MSEC;
            u64 left_ms;

            if (!rx->progress_ns)
                rx->progress_ns = now;
            if (now - rx->progress_ns >= budget &&
                !tbt_fec_any_tcp_alive(sess))
            {
                spin_lock_bh(&rx->lock);
                rx->stopping = true;
                spin_unlock_bh(&rx->lock);
                pr_info_ratelimited("tbtun: fec reset: super %llu not ready and no carrier TCP alive (total_pkts=%llu)\n",
                                    s, READ_ONCE(rx->total_pkts));
                tbt_fec_reset_diag(sess, f, s, "no tcp alive");
                atomic64_inc(&sess->fec_reset_tcp_failed);
                atomic64_inc(&sess->fec_reset_hole);
                atomic64_inc(&sess->holes_unrecoverable);
                tbt_stream_kill(st, TBT_ST_HOLE);
                break;
            }
            if (now - rx->progress_ns >= budget)
            {
                /* At least one carrier TCP is alive. Keep the stream waiting
                 * and repairing: the NACK/replay path and the carrier's own
                 * retransmission own the recovery, and only the user's own
                 * timeout may end the stream. */
                rx->progress_ns = now;
                left_ms = TBT_FEC_STALL_MS;
            }
            else
            {
                left_ms = (budget - (now - rx->progress_ns)) / NSEC_PER_MSEC + 1;
            }
            tbt_session_arm(sess, TBT_SW_FEC_RX_POLL,
                            msecs_to_jiffies(tbt_fec_rx_poll_ms(
                                left_ms, now - rx->progress_ns)));
            break;
        }

        start_p = rx->deliver_partial ? rx->deliver_p : 0;
        start_g = rx->deliver_partial ? rx->deliver_g : 0;
        for (p = start_p; p < rx->n; p++)
        {
            for (group = start_g; group < rx->groups; group++)
            {
                struct tbt_fec_blk *b;
                u64 seq = base + (u64)p * rx->groups + group;
                u16 len, real;
                int err;

                b = tbt_fec_rx_blk(rx, group, s);
                if (!tbt_fec_blk_deliverable(b->used, b->ready, b->block_id,
                                             s) ||
                    !tbt_fec_blk_sid_match(b->sid, st->sid))
                    continue;
                /* A block that was closed early has fewer real shares than n;
                 * the rest are the encoder's zero padding and carry no stream
                 * bytes. A marker block has no real share at all. */
                real = tbt_fec_blk_real_len(rx, group, b);
                if (p >= real)
                {
                    /* Padding carries no stream byte, so skipping it is
                     * correct -- unless a share is actually present here. Then
                     * the sender's cut and its DATA records disagree, and the
                     * skip would delete that share's payload while the packet
                     * frontier and the ACK ran past it. Refuse the block and
                     * name the reason instead. */
                    if (b->dpresent[p])
                    {
                        atomic64_inc(&sess->fec_slot_past_cut);
                        blocked = true;
                        blocked_why = "cut";
                        break;
                    }
                    continue;
                }
                if (known && seq >= total_pkts)
                {
                    /* Past the stream's last packet: no byte exists here.
                     * A present share would mean the stream end is wrong. */
                    if (b->dpresent[p])
                    {
                        atomic64_inc(&sess->fec_slot_past_cut);
                        blocked = true;
                        blocked_why = "past_end";
                        break;
                    }
                    continue;
                }
                len = tbt_fec_rx_share_len(b->dlen[p], b->short_len, p, real,
                                           rx->s);
                if (!len)
                {
                    /* A recovered share whose length no record stated, in a
                     * block that also holds a short share. Guessing S would
                     * hand the application the block's zero padding. */
                    atomic64_inc(&sess->fec_short_unknown);
                    blocked = true;
                    blocked_why = "short";
                    break;
                }
                if (known && rx->total_bytes &&
                    seq + 1 == total_pkts)
                {
                    u64 done = off;
                    u64 tail = rx->total_bytes - done;

                    if (tail > 0 && tail <= rx->s)
                        len = (u16)tail;
                }
                /* Defence in depth: hdr_check() already refuses an epoch
                 * larger than S, but a delivery length that named more than
                 * one slot would read past this block's buffer. */
                if (len > rx->s)
                    len = rx->s;
                err = tbt_rx_fec_deliver(st, off,
                                         b->data + (size_t)p * rx->s, len);
                if (err == -ENOMEM)
                {
                    /* Commit only the successful prefix and keep the cursor.
                     * next_super, next_deliver and ack_deliver stay put. */
                    spin_lock_bh(&rx->lock);
                    rx->deliver_bytes = off;
                    rx->deliver_p = p;
                    rx->deliver_g = group;
                    rx->deliver_partial = true;
                    spin_unlock_bh(&rx->lock);
                    atomic64_inc(&sess->fec_deliver_nomem);
                    tbt_session_arm(sess, TBT_SW_FEC_RX,
                                    msecs_to_jiffies(2));
                    tbt_stream_put(st);
                    return -ENOMEM;
                }
                if (err == 1)
                {
                    /* The share's bytes are already in the stream (a duplicate
                     * or an expired retry). Advancing off here would count
                     * them twice and the ACK would acknowledge bytes the
                     * application never received. Realign the cursor to what
                     * the stream socket has actually accepted, then continue
                     * with the next share. */
                    u64 frontier;

                    spin_lock_bh(&st->rx_lock);
                    frontier = READ_ONCE(st->rx_next);
                    spin_unlock_bh(&st->rx_lock);
                    atomic64_inc(&sess->fec_deliver_dup);
                    off = frontier;
                    spin_lock_bh(&rx->lock);
                    rx->deliver_bytes = off;
                    spin_unlock_bh(&rx->lock);
                    delivered = true; /* the ACK may now name the real bytes */
                    continue;
                }
                if (err == 2)
                {
                    /* The ordered walk found a hole: the caller's cursor ran
                     * ahead of the stream socket. Queueing that share in the
                     * reorder tree would deliver bytes out of order, which is
                     * the silent corruption this walk exists to prevent.
                     * Stop the FEC receiver and reset the stream instead. */
                    u64 frontier;

                    spin_lock_bh(&st->rx_lock);
                    frontier = READ_ONCE(st->rx_next);
                    spin_unlock_bh(&st->rx_lock);
                    atomic64_inc(&sess->fec_deliver_ooo);
                    pr_info_ratelimited("tbtun: fec ordered walk hole: super %llu p=%u group=%u off=%llu frontier=%llu\n",
                                        s, p, group,
                                        off, frontier);
                    tbt_fec_reset_diag(sess, f, s, "ordered walk hole");
                    spin_lock_bh(&rx->lock);
                    rx->stopping = true;
                    spin_unlock_bh(&rx->lock);
                    atomic64_inc(&sess->fec_reset_hole);
                    atomic64_inc(&sess->holes_unrecoverable);
                    tbt_stream_kill(st, TBT_ST_HOLE);
                    tbt_stream_put(st);
                    return 0;
                }
                if (err == -EPROTO)
                    atomic64_inc(&sess->fec_deliver_bad);
                off += len;
                spin_lock_bh(&rx->lock);
                rx->deliver_bytes = off; /* per-packet commit */
                spin_unlock_bh(&rx->lock);
                delivered = true;
            }
            if (blocked)
            {
                /* Commit the successful prefix and keep the cursor: a retry
                 * resumes at the same share. next_super and next_deliver do
                 * not move, so the ACK cannot name a byte that was never
                 * delivered. */
                spin_lock_bh(&rx->lock);
                rx->deliver_bytes = off;
                rx->deliver_p = p;
                rx->deliver_g = group;
                rx->deliver_partial = true;
                spin_unlock_bh(&rx->lock);
                if (debug >= 2)
                    pr_info("tbtun: fec deliver partial: s=%llu p=%u g=%u off=%llu blocked=%s next_deliver=%llu ack=%llu\n",
                            s, p, group, off, blocked_why,
                            rx->next_deliver, rx->ack_deliver);
                break;
            }
            start_g = 0;
        }
        /* A blocked walk did not deliver this super-block. Advancing here
         * would clear the cursor the commit above just saved and move the
         * frontier past bytes the application never received. */
        if (blocked)
            break;
        /* Fold this super-block's repair evidence into the estimator before
         * next_super moves: a carrier reuses a slot as soon as the window
         * advances, and the mask belongs to the block just delivered. */
        tbt_fec_deliver_est_fold(sess, rx, s);
        rx->deliver_partial = false;
        rx->deliver_p = 0;
        rx->deliver_g = 0;
        rx->next_super++;
        rx->progress_ns = 0; /* progress: the stall budget restarts */
        rx->next_deliver = tbt_fec_seq_of_block_pkt_base(
            s + 1, 0, 0, rx->base_pkt, rx->base_block, rx->groups, rx->n);
        if (known && rx->next_deliver > total_pkts)
            rx->next_deliver = total_pkts;
        if (debug >= 2)
            pr_info("tbtun: fec deliver super: s=%llu next=%llu next_deliver=%llu ack=%llu\n",
                    s, rx->next_super, rx->next_deliver, rx->ack_deliver);
    }
    if (delivered && rx->next_deliver > rx->ack_deliver)
    {
        u64 stream_bytes = rx->deliver_bytes > rx->stream_base ?
                           rx->deliver_bytes - rx->stream_base : 0;

        rx->ack_deliver = rx->next_deliver;
        /* The ACK names the frontier and the bytes. The frontier keeps the
         * sender's credit window in packet units; the byte count is what
         * tx_ack and every retention decision use. They are not
         * interchangeable: a block closed early, or a share whose payload is
         * shorter than S, leaves the frontier ahead of the bytes. */
        tbt_fec_ack(sess, f, st, rx->next_deliver, stream_bytes);
    }
    if (blocked)
    {
        u64 stream_off = rx->deliver_bytes > rx->stream_base ?
                         rx->deliver_bytes - rx->stream_base : 0;

        /* A ready block cannot change, so a share at or past its cut, a
         * share past the stream end, or a recovered share with no stated
         * length can never be reconciled. Retrying the same cursor would
         * spin forever while the ACK stayed behind it. Fail the stream
         * instead of stalling silently. */
        spin_lock_bh(&rx->lock);
        rx->stopping = true;
        spin_unlock_bh(&rx->lock);
        atomic64_inc(&sess->fec_reset_hole);
        atomic64_inc(&sess->holes_unrecoverable);
        pr_info_ratelimited("tbtun: fec deliver conflict: super %llu p=%u group=%u reason=%s off=%llu; stream reset\n",
                            rx->next_super, rx->deliver_p, rx->deliver_g,
                            blocked_why, stream_off);
        tbt_fec_reset_diag(sess, f, rx->next_super, blocked_why);
        tbt_stream_kill(st, TBT_ST_HOLE);
    }
    tbt_stream_put(st);
    return 0;
}

/* Refresh rx->rtt_us from the carriers' own RTT estimators, at most every
 * 100 ms. A replay may ride any up carrier, so the largest RTT is the bound
 * a re-request must respect. */
static void tbt_fec_rx_sample_rtt(struct tbt_session *sess,
                                  struct tbt_fec_flow *f, u64 now)
{
    struct tbt_fec_rx *rx = f->rx;
    u32 max_rtt = 0;
    u16 i;

    if (now - rx->rtt_sample_ns < 100000000ULL)
        return;
    rx->rtt_sample_ns = now;
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);

        if (!c)
            continue;
        if (READ_ONCE(c->rtt_us) > max_rtt)
            max_rtt = READ_ONCE(c->rtt_us);
        tbt_carrier_put(c);
    }
    if (max_rtt)
        rx->rtt_us = max_rtt;
}

/* Last NACK request time for one share, 0 when the slice belongs to another
 * block or the share was never asked. The caller holds rx->lock. */
static u64 tbt_fec_req_ns(const struct tbt_fec_rx *rx, u16 group, u64 block_id,
                          u16 share)
{
    size_t bs = (size_t)group * rx->slots + (size_t)(block_id % rx->slots);

    if (rx->req_block[bs] != block_id)
        return 0;
    return rx->req_ns[bs * (size_t)(rx->n + rx->m) + share];
}

/* Add one missing share unless its replay is still inside the hold window.
 * Returns 1 when the bit was set. The caller holds rx->lock. */
static u16 tbt_fec_nack_add(struct tbt_session *sess, struct tbt_fec_rx *rx,
                            struct tbt_fec_nack *n, u16 group, u64 block_id,
                            u16 share, u64 now, u64 hold, u16 *count)
{
    struct tbt_fec_blk *b;

    if (tbt_fec_nack_share_skip(false, now,
                                tbt_fec_req_ns(rx, group, block_id, share),
                                hold) == TBT_FEC_NACK_SKIP_INFLIGHT)
    {
        atomic64_inc(&sess->fec_nack_filtered_inflight);
        return 0;
    }
    if (*count >= TBT_FEC_NACK_SHARES_MAX)
        return 0;
    tbt_fec_bitmap_set(n->group_mask, group);
    tbt_fec_bitmap_set(n->share_mask, share);
    /* A DATA share the NACK names needed repair. The estimator reads the
     * block's mask when the super-block completes. */
    if (share < rx->n)
    {
        b = tbt_fec_rx_blk(rx, group, block_id);
        if (b->used && b->block_id == block_id)
        {
            if (share < 32)
                b->miss_mask |= 1u << share;
            tbt_fec_adapt_bitmap_set(b->miss_bits, share);
        }
    }
    (*count)++;
    return 1;
}

/* Stamp every share a NACK named, so the next scan leaves it alone for one
 * round trip, and advance the shape-probe pacing when this NACK was only a
 * probe. */
static void tbt_fec_nack_stamp(struct tbt_fec_rx *rx, u64 block_id,
                               const struct tbt_fec_nack *n, u64 now,
                               bool probed)
{
    u16 g, i, shares = (u16)(rx->n + rx->m);

    spin_lock_bh(&rx->lock);
    for (g = 0; g < rx->groups; g++)
    {
        size_t bs;
        size_t base;

        if (!tbt_fec_bitmap_test(n->group_mask, g))
            continue;
        bs = (size_t)g * rx->slots + (size_t)(block_id % rx->slots);
        base = bs * shares;
        if (rx->req_block[bs] != block_id)
        {
            memset(&rx->req_ns[base], 0, shares * sizeof(u64));
            rx->req_block[bs] = block_id;
        }
        for (i = 0; i < shares; i++)
            if (tbt_fec_bitmap_test(n->share_mask, i))
                rx->req_ns[base + i] = now;
    }
    spin_unlock_bh(&rx->lock);
    if (probed)
    {
        rx->probe_last_ns = now;
        if (rx->probe_tries < TBT_FEC_NACK_PROBE_MAX)
            rx->probe_tries++;
    }
}

/* Ask the peer for the shares missing from the oldest super-block that is
 * still inside the replay window. One NACK per pass, rate-limited and
 * retried without a total cap: a live carrier TCP keeps the repair going.
 * A block outside the window is left to that TCP's own retransmission; the
 * stream is reset only when every carrier TCP has failed.
 *
 * Three filters keep the request rate below what the sender can serve:
 *   - the first NACK waits one round trip from the block's first gap (the
 *     carrier TCP and the parity may still repair it);
 *   - a share is not named again inside max(RTT, 2 x cadence), so a reply
 *     that is still in flight is not duplicated;
 *   - one NACK names at most TBT_FEC_NACK_SHARES_MAX shares, so a burst
 *     loss cannot fill the sender's 8 MiB replay queue with one request.
 * A shape probe for an unbuilt block runs at TBT_FEC_NACK_PROBE_MS, not at
 * the repair cadence, and probes only shares whose hold has expired. */
static void tbt_fec_nack_scan(struct tbt_session *sess,
                              struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_nack n;
    u64 now = ktime_get_ns();
    u64 hold;
    u64 s, hi;
    u16 g, i;
    bool found = false;
    bool young = false;
    bool any_probe = false; /* the NACK being built names probe shares */
    bool probe_only = false; /* the request being sent is only a shape probe */
    bool block_code;

    if (!rx || !rx->nack || READ_ONCE(rx->stopping) ||
        READ_ONCE(sess->closing))
        return;
    if (READ_ONCE(rx->rebase_pending))
        return;
    /* The shape must support a ring. Both ends derive the same effective K,
     * so the receiver NACKs exactly the blocks the sender can replay; the
     * sender counts fec_replay_miss_no_ring if its own allocation failed. */
    if (!rx->replay_k)
        return;
    block_code = tbt_fec_grouped_mode(rx->codec) == 3;

    tbt_fec_rx_sample_rtt(sess, f, now);

    /* A previous NACK's block is now ready or delivered: that interval is
     * the repair latency the sender's cost model needs. */
    if (rx->nack->valid)
    {
        bool repaired;

        spin_lock_bh(&rx->lock);
        repaired = rx->nack->block_id < rx->next_super ||
                   tbt_fec_super_ready_locked(rx, rx->nack->block_id);
        spin_unlock_bh(&rx->lock);
        if (repaired)
        {
            u64 dt = now - (rx->repair_first_ns &&
                                    rx->repair_block == rx->nack->block_id
                                ? rx->repair_first_ns
                                : rx->nack->first_ns);

            atomic64_add(dt, &sess->fec_nack_repair_ns);
            tbt_fec_stat_max(&sess->fec_nack_repair_max_ns, dt);
            /* A slow repair means the sender's replay queue is behind;
             * asking faster only adds duplicates. */
            rx->nack_cadence_ms =
                tbt_fec_nack_cadence_ms(rx->nack_cadence_ms, dt);
            rx->repair_first_ns = 0;
            tbt_fec_nack_reset(rx->nack);
        }
    }

    /* How long a requested share is left alone: one round trip, never less
     * than two repair intervals. Computed after the cadence update so this
     * pass uses the latency the last repair taught. */
    hold = tbt_fec_nack_hold_ns(rx->rtt_us, rx->nack_cadence_ms);

    hi = READ_ONCE(rx->highest_block);
    /* The delivery frontier may be ahead of the highest block seen: every
     * share of the next super-block was lost. Probe it anyway (parity 0 for
     * each group) so the sender's replay ring can state its shape; otherwise
     * the receiver waits for a share that never arrives and the sender's TX
     * stall fires. */
    if (hi < rx->next_super)
        hi = rx->next_super;
    /* The ring covers [highest - K + 1, highest]. An older block is outside
     * the window: do not NACK it, and do not reset it early. The sender's
     * credit is capped to K-2 super-blocks, so this window always covers the
     * block the sender can still be waiting on. */
    if (hi - rx->next_super >= rx->replay_k)
        return;

    if (debug >= 2)
    {
        spin_lock_bh(&rx->lock);
        for (g = 0; g < rx->groups; g++)
        {
            struct tbt_fec_blk *db = tbt_fec_rx_blk(rx, g, rx->next_super);

            /* Rate-limited: the walk now polls at the NACK cadence while a
             * super-block is not ready, and four unthrottled lines per poll
             * would evict the delivery trace from the kernel ring buffer. */
            pr_info_ratelimited("tbtun: fec super %llu group %u used=%d ready=%d id=%llu empty=%d nparity=%u ndata=%u\n",
                                rx->next_super, g, db->used, db->ready,
                                db->block_id, db->empty, db->nparity,
                                db->ndata);
        }
        spin_unlock_bh(&rx->lock);
    }

    spin_lock_bh(&rx->lock);
    for (s = rx->next_super; s <= hi; s++)
    {
        bool missing = false;
        bool started = false;
        u64 first_gap = 0;
        u16 nmissing = 0;

        probe_only = true; /* every request so far is a shape probe */
        any_probe = false;

        /* The stream ends in `final`: a super-block past it holds no packet
         * of this stream, so requesting it would send a NACK every retry
         * interval until the connection closed. The delivery walk already
         * refuses to deliver past the end; stop asking here too. */
        if (READ_ONCE(rx->total_known) &&
            s > tbt_fec_final_block_of_total_base(
                    READ_ONCE(rx->total_pkts), rx->base_pkt, rx->base_block,
                    rx->groups, rx->n))
            break;
        for (g = 0; g < rx->groups; g++)
        {
            struct tbt_fec_blk *b = tbt_fec_rx_blk(rx, g, s);

            if (b->used && b->block_id == s)
            {
                started = true;
                break;
            }
        }
        /* A super-block ahead of the delivery frontier with no block at all
         * has not started: its shares may simply not be sent yet. Do not
         * request them. The delivery frontier itself is probed even when no
         * share of it arrived: a whole-block loss is invisible to `hi`. */
        if (!started && s != rx->next_super)
            continue;
        memset(&n, 0, sizeof(n));
        for (g = 0; g < rx->groups; g++)
        {
            struct tbt_fec_blk *b = tbt_fec_rx_blk(rx, g, s);
            u16 real;

            /* Do not ask during the first settle window: the pass above has
             * not attempted this block yet, and a replay would restart the
             * window.  After the pass has tried and failed, decode_tries is
             * set and the scan asks normally; gating that backoff here would
             * suppress the only repair, because the next pass re-arms it
             * before this scan runs. */
            if (tbt_fec_nack_wait_decode(block_code, b->used, b->block_id,
                                         s, b->nparity, b->decode_tries,
                                         now,
                                         b->decode_next_ns))
                continue;
            if (!b->used || b->block_id != s)
            {
                /* A block that holds only parity is not "unstarted": its
                 * DATA shares are the missing ones. Request them instead of
                 * probing parity 0 again (which is already present). */
                if (tbt_fec_nack_parity_only(b->used, b->nparity, b->block_id,
                                             s, b->ndata, b->ready))
                {
                    u16 dlimit, plimit, seen_hi = 0;

                    missing = true;
                    probe_only = false;
                    if (!b->first_gap_ns)
                        b->first_gap_ns = now;
                    if (!first_gap || b->first_gap_ns < first_gap)
                        first_gap = b->first_gap_ns;
                    real = tbt_fec_blk_real_len(rx, g, b);
                    for (i = rx->n; i > 0; i--)
                        if (b->dpresent[i - 1])
                        {
                            seen_hi = i;
                            break;
                        }
                    dlimit = tbt_fec_nack_data_limit(b->win_len_set, real,
                                                     seen_hi);
                    plimit = tbt_fec_nack_parity_limit(
                        b->win_len_set, real, rx->n, rx->m, block_code);
                    for (i = 0; i < dlimit; i++)
                        if (!b->dpresent[i])
                            tbt_fec_nack_add(sess, rx, &n, g, s, i, now,
                                             hold, &nmissing);
                    for (i = 0; i < plimit; i++)
                        if (!b->ppresent[i])
                            tbt_fec_nack_add(sess, rx, &n, g, s,
                                             (u16)(rx->n + i), now, hold,
                                             &nmissing);
                    continue;
                }
                /* Before the stream end is known, a group with no block may
                 * be legitimately empty: a short/sparse block. Wait for the
                 * parity win_len, the empty marker or FIN to state the real
                 * share count. Requesting the whole range here produced a
                 * NACK storm the sender could not serve and left the block
                 * unready until the reset. */
                if (tbt_fec_nack_unstarted_group_base(
                        READ_ONCE(rx->total_pkts), rx->base_pkt,
                        rx->base_block, rx->groups, rx->n, s, g,
                        READ_ONCE(rx->total_known)))
                {
                    /* Total is known and this group has packets in the
                     * block: a wholly missing block is a real loss. A block
                     * code has no present share to prove any DATA slot real,
                     * so ask for parity 0 alone: it states win_len, and the
                     * next pass names the real DATA range. Asking for all n
                     * here named padding on every short block. RLNC has no
                     * cut to learn and keeps its full range. */
                    missing = true;
                    probe_only = false;
                    if (rx->gap_block != s)
                    {
                        rx->gap_block = s;
                        rx->gap_ns = now;
                    }
                    if (!first_gap || rx->gap_ns < first_gap)
                        first_gap = rx->gap_ns;
                    if (!block_code)
                        for (i = 0; i < rx->n; i++)
                            tbt_fec_nack_add(sess, rx, &n, g, s, i, now,
                                             hold, &nmissing);
                    for (i = 0; i < (block_code ? 1 : rx->m); i++)
                        tbt_fec_nack_add(sess, rx, &n, g, s,
                                         (u16)(rx->n + i), now, hold,
                                         &nmissing);
                    continue;
                }
                /* Before total is known, probe only parity 0 of the groups.
                 * A block the sender never built answers nothing; after
                 * TBT_FEC_NACK_PROBE_MAX unanswered probes stop asking and
                 * leave it to the stall timer. Probing an idle frontier
                 * forever asked for the same absent shares 274k times on the
                 * fix22 run. */
                if (rx->probe_block != s)
                {
                    rx->probe_block = s;
                    rx->probe_tries = 0;
                }
                if (!READ_ONCE(rx->total_known) &&
                    tbt_fec_nack_probe_due(now, rx->probe_last_ns) &&
                    tbt_fec_nack_probe_open(hi > s, rx->probe_tries) &&
                    tbt_fec_nack_add(sess, rx, &n, g, s, rx->n, now, hold,
                                     &nmissing))
                {
                    missing = true;
                    any_probe = true;
                    atomic64_inc(&sess->fec_nack_sparse_probe);
                }
                continue;
            }
            if (b->ready)
                continue;
            real = tbt_fec_blk_real_len(rx, g, b);
            if (!real)
                continue;
            /* The decode pass ran before this scan. A block that is still
             * not ready cannot be completed by the shares it already holds:
             * "ndata + nparity >= real" is a count test, not a rank test,
             * and a rank-deficient block was skipped forever (fix25: the
             * frontier never got the parity row it needed, the ACK stopped
             * and the download stalled for 120 s). Request the missing DATA
             * and parity shares; the per-share hold keeps repeats down.
             *
             * A DATA share is named only when it is proven real: the parity's
             * win_len states the cut, or a present share after it proves the
             * block has that many shares. The tail of a short block is zero
             * padding the sender never wrote; naming it bought a round trip
             * and an absent-share miss per share. */
            {
                u16 dlimit, plimit, seen_hi = 0;

                for (i = rx->n; i > 0; i--)
                    if (b->dpresent[i - 1])
                    {
                        seen_hi = i;
                        break;
                    }
                dlimit = tbt_fec_nack_data_limit(b->win_len_set, real,
                                                 seen_hi);
                plimit = tbt_fec_nack_parity_limit(
                    b->win_len_set, real, rx->n, rx->m, block_code);
                missing = true;
                probe_only = false;
                if (!b->first_gap_ns)
                    b->first_gap_ns = now;
                if (!first_gap || b->first_gap_ns < first_gap)
                    first_gap = b->first_gap_ns;
                for (i = 0; i < dlimit; i++)
                    if (!b->dpresent[i])
                        tbt_fec_nack_add(sess, rx, &n, g, s, i, now, hold,
                                         &nmissing);
                for (i = 0; i < plimit; i++)
                    if (!b->ppresent[i])
                        tbt_fec_nack_add(sess, rx, &n, g, s,
                                         (u16)(rx->n + i), now, hold,
                                         &nmissing);
            }
        }
        if (!(missing && tbt_fec_bitmap_count(n.share_mask)))
        {
            /* Every missing share of the frontier is inside its hold (or
             * already present). Never move on to a later block: it cannot be
             * delivered before the frontier, and asking for it leaves the
             * ACK stuck (fix26: next=185 was skipped for s=186 563 times).
             * Arm and wait for the hold to expire. */
            if (tbt_fec_nack_frontier_only(s, rx->next_super))
                break;
            continue;
        }
        /* A real request waits one round trip from the first gap: the
         * carrier TCP still holds the share and the block's parity may still
         * decode it. A NACK sent before that returns a replay that is almost
         * always late (field: 338k late shares, 853 ms mean repair). A shape
         * probe has its own pacing and never waits here. */
        if (!probe_only && first_gap &&
            !tbt_fec_nack_first_due(now, first_gap, hold))
        {
            atomic64_add(tbt_fec_bitmap_count(n.share_mask),
                         &sess->fec_nack_filtered_young);
            young = true;
            break; /* later blocks are younger: nothing to send this pass */
        }
        found = true;
        break;
    }
    spin_unlock_bh(&rx->lock);

    if (found && s > rx->next_super)
        pr_info_ratelimited("tbtun: fec NACK skips frontier next=%llu for s=%llu hi=%llu\n",
                            rx->next_super, s, hi);
    if (!found)
    {
        struct tbt_fec_nack last;
        bool started = false;
        bool blocked;
        u64 last_block;
        u16 nmissing = 0;

        if (!rx->nack->valid)
        {
            /* A young block has nothing to ask yet; wake when its hold may
             * have expired instead of waiting for the next share. */
            if (young)
                tbt_session_arm(sess, TBT_SW_FEC_RX_POLL,
                                msecs_to_jiffies(TBT_FEC_NACK_MIN_MS));
            return;
        }
        last_block = rx->nack->block_id;
        if (last_block > rx->next_super)
        {
            /* A NACK for a later block cannot move the ACK. Drop it and let
             * the next scan target the frontier. */
            tbt_fec_nack_reset(rx->nack);
            return;
        }
        if (last_block < rx->next_super)
        {
            /* The block was delivered while its NACK was in flight: nothing
             * in that bitmap needs a replay. */
            atomic64_add(tbt_fec_bitmap_count(rx->nack->share_mask),
                         &sess->fec_nack_filtered_ready);
            tbt_fec_nack_reset(rx->nack);
            return;
        }
        if (READ_ONCE(rx->total_known) &&
            rx->next_super >
                tbt_fec_final_block_of_total_base(
                    rx->total_pkts, rx->base_pkt, rx->base_block, rx->groups,
                    rx->n))
        {
            tbt_fec_nack_reset(rx->nack);
            return;
        }
        spin_lock_bh(&rx->lock);
        for (g = 0; g < rx->groups; g++)
        {
            struct tbt_fec_blk *b = tbt_fec_rx_blk(rx, g, rx->next_super);

            if (b->used && b->block_id == rx->next_super)
            {
                started = true;
                break;
            }
        }
        blocked = started &&
                  !tbt_fec_super_ready_locked(rx, rx->next_super);
        if (!blocked)
        {
            spin_unlock_bh(&rx->lock);
            tbt_fec_nack_reset(rx->nack);
            return;
        }
        /* The resend path below rebuilds the previous bitmap.  It must leave
         * a settling block alone too, or the same storm returns through the
         * retry path instead of a fresh request. */
        if (block_code)
        {
            bool settling = false;

            for (g = 0; g < rx->groups; g++)
            {
                struct tbt_fec_blk *sb = tbt_fec_rx_blk(rx, g, last_block);

                if (tbt_fec_nack_wait_decode(block_code, sb->used,
                                             sb->block_id, last_block,
                                             sb->nparity,
                                             sb->decode_tries, now,
                                             sb->decode_next_ns))
                {
                    settling = true;
                    break;
                }
            }
            if (settling)
            {
                spin_unlock_bh(&rx->lock);
                tbt_session_arm(sess, TBT_SW_FEC_RX_POLL,
                                msecs_to_jiffies(TBT_FEC_NACK_MIN_MS));
                return;
            }
        }
        /* The scan found no fresh missing set, but the blocked super-block
         * is still not ready. Rebuild the previous bitmap from the block's
         * current state: shares that arrived are dropped, and a share whose
         * replay is still inside its round trip is not asked again. */
        memset(&last, 0, sizeof(last));
        for (g = 0; g < rx->groups; g++)
        {
            struct tbt_fec_blk *b;
            bool have;
            u16 shares = (u16)(rx->n + rx->m);

            if (!tbt_fec_bitmap_test(rx->nack->group_mask, g))
                continue;
            b = tbt_fec_rx_blk(rx, g, last_block);
            have = b->used && b->block_id == last_block;
            for (i = 0; i < shares; i++)
            {
                bool present;
                enum tbt_fec_nack_skip skip;

                if (!tbt_fec_bitmap_test(rx->nack->share_mask, i))
                    continue;
                present = have &&
                          (i < rx->n ? b->dpresent[i] :
                                       b->ppresent[i - rx->n]);
                skip = tbt_fec_nack_share_skip(
                    present, now,
                    tbt_fec_req_ns(rx, g, last_block, i), hold);
                if (skip != TBT_FEC_NACK_KEEP)
                {
                    if (skip == TBT_FEC_NACK_SKIP_PRESENT)
                        atomic64_inc(&sess->fec_nack_filtered_dup);
                    else
                        atomic64_inc(&sess->fec_nack_filtered_inflight);
                    continue;
                }
                if (nmissing >= TBT_FEC_NACK_SHARES_MAX)
                    continue;
                tbt_fec_bitmap_set(last.group_mask, g);
                tbt_fec_bitmap_set(last.share_mask, i);
                nmissing++;
            }
        }
        spin_unlock_bh(&rx->lock);
        if (!tbt_fec_bitmap_count(last.share_mask))
        {
            /* Every share arrived or its reply is still in flight. Nothing
             * to put on the wire; the next pass decodes the block or the
             * hold expires. */
            tbt_session_arm(sess, TBT_SW_FEC_RX_POLL,
                            msecs_to_jiffies(TBT_FEC_NACK_MIN_MS));
            return;
        }
        if (tbt_fec_nack_due_at(rx->nack, now, last_block, &last,
                                rx->nack_cadence_ms))
        {
            int err = tbt_fec_send_nack(sess, f, last_block, &last);

            if (debug >= 2)
                pr_info("tbtun: fec NACK retry block=%llu next=%llu group=%*phN share=%*phN\n",
                        last_block, rx->next_super, TBT_FEC_BITMAP_BYTES,
                        last.group_mask, TBT_FEC_BITMAP_BYTES,
                        last.share_mask);
            if (!err)
            {
                atomic64_inc(&sess->fec_nack_tx);
                tbt_fec_nack_stamp(rx, last_block, &last, now, false);
                if (!rx->repair_first_ns ||
                    rx->repair_block != last_block)
                {
                    rx->repair_block = last_block;
                    rx->repair_first_ns = now;
                }
            }
            else
                tbt_fec_nack_drop(sess, err, last_block, &last);
        }
        tbt_session_arm(sess, TBT_SW_FEC_RX_POLL,
                        msecs_to_jiffies(TBT_FEC_NACK_MIN_MS));
        return;
    }
    if (probe_only && rx->nack->valid &&
        now - rx->nack->last_ns < (u64)TBT_FEC_NACK_PROBE_MS * 1000000ULL)
    {
        /* The sender may not have sent this super-block at all: an idle
         * connection would put a probe on the wire every retry interval for
         * as long as it stays open. Pace it; a real repair keeps the retry
         * cadence. */
        tbt_session_arm(sess, TBT_SW_FEC_RX_POLL,
                        msecs_to_jiffies(TBT_FEC_RX_POLL_SLOW_MS));
        return;
    }
    if (tbt_fec_nack_due_at(rx->nack, now, s, &n, rx->nack_cadence_ms))
    {
        int err;

        if (debug >= 2)
            pr_info("tbtun: fec NACK tx block=%llu next=%llu hi=%llu group=%*phN share=%*phN\n",
                    s, rx->next_super, hi, TBT_FEC_BITMAP_BYTES,
                    n.group_mask, TBT_FEC_BITMAP_BYTES, n.share_mask);
        err = tbt_fec_send_nack(sess, f, s, &n);
        if (!err)
        {
            atomic64_inc(&sess->fec_nack_tx);
            tbt_fec_nack_stamp(rx, s, &n, now, any_probe);
            if (!rx->repair_first_ns || rx->repair_block != s)
            {
                rx->repair_block = s;
                rx->repair_first_ns = now;
            }
        }
        else
            tbt_fec_nack_drop(sess, err, s, &n);
    }
    /* The NACK itself may be lost; retry while the block is in the window.
     * No total attempt cap: only the window and the carrier TCP lifetime
     * bound the repair. */
    tbt_session_arm(sess, TBT_SW_FEC_RX_POLL,
                    msecs_to_jiffies(TBT_FEC_NACK_MIN_MS));
}

/* Claim one block under rx->lock, decode it outside, commit under the lock.
 * The snapshot is what the codec sees; a share that arrives during the decode
 * is placed into b->data as usual, and the commit only writes slots that are
 * still missing. */
static bool tbt_fec_rx_decode_one(struct tbt_session *sess,
                                  struct tbt_fec_flow *f, u16 group,
                                  u16 slot)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_blk *b = &rx->blk[(size_t)group * rx->slots + slot];
    u16 real = 0, i, k;
    bool claimed = false;
    bool block_code;
    u64 now = ktime_get_ns();
    int rc;

    spin_lock_bh(&rx->lock);
    block_code = tbt_fec_grouped_mode(rx->codec) == 3;
    /* Only the delivery frontier can use a decoded block.  Decoding a later
     * slot speculatively costs a whole block of CPU and delays the frontier
     * it cannot overtake; the field body ran up to eight 46 ms decodes
     * before it delivered the one block the application was waiting for. */
    if (b->used && !b->ready && !b->decoding && now >= b->decode_next_ns &&
        (!block_code || b->block_id == rx->next_super))
    {
        real = tbt_fec_blk_real_len(rx, group, b);
        tbt_fec_blk_counts(rx, group, b);
        if (b->empty || b->ndata >= real)
        {
            b->ready = true; /* complete without the codec */
        }
        else if (block_code ? b->nparity > 0
                            : b->nparity >= (u16)(real - b->ndata))
        {
            /* An SC-LDPC block codec needs any parity share and reports what
             * it could recover; the RLNC path needs one row per missing
             * share. */
            memcpy(rx->dpresent_snap, b->dpresent, rx->n);
            memcpy(rx->ppresent_snap, b->ppresent, rx->m);
            for (i = real; i < rx->n; i++)
                rx->dpresent_snap[i] = 1; /* padding is known zero */
            b->decoding = true;
            claimed = true;
        }
    }
    spin_unlock_bh(&rx->lock);
    if (!claimed)
        return false;

    rc = tbt_fec_blk_decode(sess, f, group, b);
    spin_lock_bh(&rx->lock);
    if (rc == 0)
    {
        bool commit_block_code = tbt_fec_grouped_mode(rx->codec) == 3;
        u8 fail[TBT_FEC_BITMAP_BYTES];

        memset(fail, 0, sizeof(fail));
        for (i = 0, k = 0; i < real; i++)
        {
            if (rx->dpresent_snap[i])
                continue;
            /* Belt and braces for a block codec: copy a recovered slot only
             * when the codec says that share is complete. A partial decode
             * must not turn stale rx->rec bytes into delivered data. */
            if (commit_block_code &&
                !tbt_fec_grouped_is_known(rx->codec, group, i))
            {
                tbt_fec_adapt_bitmap_set(fail, i);
                k++;
                continue;
            }
            if (!b->dpresent[i])
            {
                memcpy(b->data + (size_t)i * rx->s,
                       rx->rec + (size_t)k * rx->s, rx->s);
                b->dpresent[i] = 1;
                b->dlen[i] = 0; /* a recovered share has no own length */
                b->ndata++;
                /* Parity recovered this DATA share: it is a repair, the
                 * estimator's miss evidence for this block. */
                if (i < 32)
                    b->miss_mask |= 1u << i;
                tbt_fec_adapt_bitmap_set(b->miss_bits, i);
            }
            k++;
        }
        memcpy(b->codec_fail_bits, fail, sizeof(fail));
        /* Ready only when every real share is present; a skipped share keeps
         * the block decodable on the next pass. */
        b->ready = b->ndata >= real;
    }
    if (b->ready)
    {
        b->decode_tries = 0;
        b->decode_next_ns = 0;
    }
    else
    {
        /* Exponential backoff, 50 ms doubling to 400 ms.  A share that
         * arrives after this attempt is new evidence and sets its own quiet
         * deadline in the placement path; keep whichever deadline is later,
         * so a repair is not decoded before its burst has settled. */
        u64 wait = (u64)TBT_FEC_DECODE_RETRY_MS * NSEC_PER_MSEC;
        u64 deadline;

        if (b->decode_tries < 4)
            wait <<= b->decode_tries;
        else
            wait = (u64)TBT_FEC_DECODE_RETRY_MAX_MS * NSEC_PER_MSEC;
        if (b->decode_tries < 0xff)
            b->decode_tries++;
        deadline = ktime_get_ns() + wait;
        if (b->decode_next_ns < deadline)
            b->decode_next_ns = deadline;
        atomic64_inc(&sess->fec_decode_backoff);
    }
    b->decoding = false;
    spin_unlock_bh(&rx->lock);
    return rc == 0;
}

/* One FEC_FBK report. Control records are plaintext and ride the control
 * queue; the sender's control law consumes the fields. `extra` carries the
 * handshake flags (CFG_ACK / SWITCH_DONE / ROLLBACK / CFG_NACK). */
static bool tbt_fec_rx_sc_mode(const struct tbt_fec_rx *rx)
{
    return rx->est_sclc && tbt_fec_grouped_mode(rx->codec) == 3;
}

static u64 tbt_fec_rx_est_activity(const struct tbt_fec_rx *rx)
{
    if (tbt_fec_rx_sc_mode(rx))
        return rx->est_sclc->last_activity_ns;
    return rx->est ? rx->est->last_activity_ns : 0;
}

/* Sample whichever estimator owns this shape. The RLNC-shaped out is the
 * common view the c13 snapshot already consumes; the SC out carries the
 * p_fast/codec_fail values the FBK overlay needs. */
static void tbt_fec_rx_sample(struct tbt_fec_rx *rx, u64 now,
                              struct tbt_fec_est_out *o,
                              struct tbt_fec_adapt_sclc_est_out *so)
{
    if (tbt_fec_rx_sc_mode(rx))
    {
        tbt_fec_adapt_sclc_est_sample(rx->est_sclc, now, so);
        memset(o, 0, sizeof(*o));
        o->p_ppm = so->p_ppm;
        o->p_ewma_ppm = so->p_ewma_ppm;
        o->p_win_ppm = so->p_win_ppm;
        o->r_fail_ppm = so->r_fail_ppm;
        o->sent_win = so->sent_win;
        o->miss_win = so->miss_win;
        o->supers_win = so->supers_win;
        o->abandoned_win = so->abandoned_win;
        o->unknown_win = so->unknown_win;
        o->requested_win = so->requested_win;
        o->pkts_win = so->pkts_win;
        o->abandoned_total = so->abandoned_total;
        o->unknown_total = so->unknown_total;
        o->alive = so->alive;
        o->stalled = so->stalled;
        o->carriers = so->carriers;
        o->p_valid = so->p_valid;
        o->r_valid = so->r_valid;
        o->pause_idle = so->pause_idle;
        o->pause_carrier = so->pause_carrier;
        o->pause_abandoned = so->pause_abandoned;
    }
    else
    {
        memset(so, 0, sizeof(*so));
        if (rx->est)
            tbt_fec_est_sample(rx->est, now, o);
        else
            memset(o, 0, sizeof(*o));
    }
}

static void tbt_fec_rx_fbk_send(struct tbt_session *sess,
                                struct tbt_fec_flow *flow, u8 extra)
{
    struct tbt_fec_rx *rx = flow->rx;
    struct tbt_fec_est_out o;
    struct tbt_fec_adapt_sclc_est_out so;
    struct tbt_fec_fbk f;
    struct tbt_fec_hdr h;
    struct tbt_fec_shape suggest;
    u8 body[TBT_FEC_FBK_LEN];
    u8 rec[TBT_FEC_HDR_LEN + TBT_FEC_FBK_LEN];
    u16 len;
    u64 now = ktime_get_ns();
    u32 expected;
    bool sc;

    if (!rx || (!rx->est && !rx->est_sclc))
        return;
    sc = tbt_fec_rx_sc_mode(rx);
    tbt_fec_rx_sample(rx, now, &o, &so);
    memset(&f, 0, sizeof(f));
    tbt_fec_fbk_set_epoch(&f, rx->fbk_epoch ? rx->fbk_epoch : rx->epoch);
    rx->fbk_epoch = 0;
    if (sc)
    {
        /* v8 keeps m_current/m_target as parity share counts; the rate now
         * has its own extension words and is no longer folded into them. */
        f.m_current = (u8)rx->m;
        f.m_target = READ_ONCE(rx->cfg_pending)
                         ? (u8)rx->pending_cfg.m
                         : f.m_current; /* the sender owns the control law */
    }
    else
    {
        f.m_current = (u8)rx->m;
        tbt_fec_adapt_shape(tbt_fec_adapt_class_of_p(o.p_ppm), &suggest);
        f.m_target = (u8)suggest.m;
    }
    /* The field keeps its 0x1006 name, but the control law consumes the same
     * p_used the receiver used: max(EWMA, 1 s window). */
    f.p_ewma_ppm = o.p_ppm;
    expected = o.sent_win;
    f.expected_shares = expected > 0xffffu ? 0xffffu : (u16)expected;
    f.received_shares =
        (u16)(f.expected_shares -
              (o.miss_win > f.expected_shares ? f.expected_shares
                                              : o.miss_win));
    f.n_current = (u8)rx->n;
    if (o.p_valid && !o.pause_idle && !o.pause_carrier &&
        !o.pause_abandoned)
        extra |= TBT_FEC_FBK_F_P_VALID;
    f.flags = extra;
    memcpy(f.carrier_bitmap, rx->alive_bm, TBT_FEC_BITMAP_BYTES);
    f.codec_variant = READ_ONCE(rx->shape.codec_variant);
    f.L = READ_ONCE(rx->shape.L);
    f.block = READ_ONCE(rx->shape.block);
    if (sc)
    {
        /* p_fast moved to the extension; codec_fail stays local (procfs). */
        f.p_fast_ppm = so.p_fast_ppm;
        f.rate_ppm_current = READ_ONCE(rx->shape.rate_ppm);
        f.rate_ppm_target = READ_ONCE(rx->cfg_pending)
                                ? READ_ONCE(rx->pending_cfg.rate_ppm)
                                : f.rate_ppm_current;
    }
    if (tbt_fec_fbk_pack(&f, body))
        return;
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_FBK;
    h.payload_len = TBT_FEC_FBK_LEN;
    h.conv = TBT_FEC_CONV;
    h.sid = flow->sid;
    if (tbt_fec_make_record(sess, &h, body, rec, &len))
        return;
    if (!tbt_fec_send_record(sess, 0, rec, len, true, false, flow))
        rx->fbk_last_ns = now;
}

static void tbt_fec_rx_switch_tick(struct tbt_session *sess,
                                   struct tbt_fec_flow *f);
static void tbt_fec_rx_init_tick(struct tbt_session *sess,
                                 struct tbt_fec_flow *f);

/* Refresh the alive bitmap, fold the abandoned delta and publish the c13
 * snapshot. Called only from w_fec_rx, the estimator's single writer. */
static void tbt_fec_rx_adapt_tick(struct tbt_session *sess,
                                  struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_adapt_ctl *a = &sess->adapt;
    struct tbt_fec_est_out o;
    struct tbt_fec_adapt_sclc_est_out so;
    u64 now = ktime_get_ns();
    u64 abandoned;
    u16 i, alive = 0, stalled = 0;
    u8 flags;
    bool paused, sc;

    if (!rx || (!rx->est && !rx->est_sclc) || READ_ONCE(rx->stopping) ||
        READ_ONCE(sess->closing))
        return;
    sc = tbt_fec_rx_sc_mode(rx);
    if (!READ_ONCE(sess->adaptive))
    {
        /* The initial shape is not a switch: install it even with the
         * control law off, which is the 0.3.0 SC-LDPC configuration. */
        if (rx->init_pending)
            tbt_fec_rx_init_tick(sess, f);
        /* adaptive=off keeps the shape in force but abandons any pending
         * switch. The installed codec is never freed here. */
        if (rx->switch_state != TBT_FEC_SW_STEADY)
        {
            struct tbt_fec_grouped *drop = rx->pending_codec;

            rx->pending_codec = NULL;
            if (rx->switch_state == TBT_FEC_SW_APPLIED)
            {
                drop = rx->old_codec;
                rx->old_codec = NULL;
            }
            rx->switch_state = TBT_FEC_SW_STEADY;
            rx->cfg_pending = false;
            rx->commit_pending = false;
            rx->rollback_pending = false;
            tbt_fec_grouped_free(drop);
        }
        return;
    }
    /* INIT is not a switch: it installs the peer's asymmetric shape (the
     * downlink 1x256 and uplink 8x32 differ) before the first share can be
     * placed. Living only in the adaptive=off branch left every adaptive
     * pair decoding the peer's shares with its own configured codec, so no
     * block ever completed and no FBK could start the control law. */
    if (rx->init_pending)
        tbt_fec_rx_init_tick(sess, f);
    tbt_fec_rx_switch_tick(sess, f);
    memset(rx->alive_bm, 0, sizeof(rx->alive_bm));
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);

        if (!c)
            continue;
        if (tbt_fec_adapt_carrier_alive(atomic_read(&c->up),
                                        atomic_read(&c->stopping),
                                        atomic_read(&c->connected), now,
                                        READ_ONCE(c->last_rx_ns),
                                        READ_ONCE(c->rtt_us)))
        {
            tbt_fec_adapt_bitmap_set(rx->alive_bm, i);
            alive++;
        }
        if (READ_ONCE(c->stalled))
            stalled++;
        tbt_carrier_put(c);
    }
    if (rx->est)
        tbt_fec_est_carriers(rx->est, alive, stalled, sess->n_car);
    if (rx->est_sclc)
        tbt_fec_adapt_sclc_est_carriers(rx->est_sclc, alive, stalled,
                                        sess->n_car);
    abandoned = (u64)atomic64_read(&rx->abandoned_shares);
    if (abandoned > rx->est_abandoned_last)
    {
        if (rx->est)
            tbt_fec_est_abandoned(rx->est, now,
                                  (u32)(abandoned -
                                        rx->est_abandoned_last));
        if (rx->est_sclc)
            tbt_fec_adapt_sclc_est_abandoned(
                rx->est_sclc, now,
                (u32)(abandoned - rx->est_abandoned_last));
        rx->est_abandoned_last = abandoned;
    }
    tbt_fec_rx_sample(rx, now, &o, &so);
    if (debug && sc)
        pr_info_ratelimited("tbtun-diag: rx adapt tunnel=%llu valid=%d idle=%d carrier=%d abandoned=%d sent=%u supers=%u p=%u\n",
                            (unsigned long long)sess->tunnel->cfg.id,
                            so.p_valid, so.pause_idle, so.pause_carrier,
                            so.pause_abandoned, so.sent_win, so.supers_win,
                            so.p_ppm);

    a->epoch = rx->epoch;
    a->n = rx->n;
    a->m = rx->m;
    a->groups = rx->groups;
    if (sc)
    {
        u8 idx = tbt_fec_adapt_sclc_rate_index_for_shape(rx->n, rx->m);

        a->class = idx == 0xff ? 0 : idx;
    }
    else
    {
        a->class = tbt_fec_adapt_class_of_shape(rx->n, rx->m);
        if (a->class == 0xff)
            a->class = 3;
    }
    a->p_ppm = o.p_ppm;
    a->p_win_ppm = o.p_win_ppm;
    a->r_fail_ppm = 0; /* the sender measures the residual */
    a->sent = o.sent_win;
    a->miss = o.miss_win;
    a->sample_unknown = o.unknown_total;
    a->abandoned = o.abandoned_total;

    paused = o.pause_idle || o.pause_carrier || o.pause_abandoned;
    if (paused && !rx->est_paused)
    {
        rx->est_paused = true;
        rx->pause_start_ns = now;
        a->pauses++;
    }
    else if (!paused && rx->est_paused)
    {
        rx->est_paused = false;
        a->pause_ns += now - rx->pause_start_ns;
    }

    if (rx->switch_state == TBT_FEC_SW_PREP ||
        rx->switch_state == TBT_FEC_SW_ARMED)
    {
        a->state = TBT_FEC_ADAPT_CFG_SENT;
        a->reason = TBT_FEC_ADAPT_CTL_R_SWITCHING;
    }
    else if (rx->switch_state == TBT_FEC_SW_APPLIED)
    {
        a->state = TBT_FEC_ADAPT_SWAP;
        a->reason = TBT_FEC_ADAPT_CTL_R_SWITCHING;
    }
    else
    {
        a->state = TBT_FEC_ADAPT_STEADY;
        if (o.pause_idle)
            a->reason = TBT_FEC_ADAPT_CTL_R_IDLE;
        else if (o.pause_carrier)
            a->reason = TBT_FEC_ADAPT_CTL_R_CARRIER;
        else if (o.pause_abandoned)
            a->reason = TBT_FEC_ADAPT_CTL_R_ABANDONED;
        else if (!o.p_valid)
            a->reason = TBT_FEC_ADAPT_CTL_R_SAMPLE;
        else
            a->reason = TBT_FEC_ADAPT_CTL_R_NONE;
    }

    /* Repeat the handshake while it is the receiver's state: an FBK may be
     * lost, and the sender's retry window is only 500 ms. */
    if (rx->switch_state == TBT_FEC_SW_ARMED)
    {
        rx->fbk_flags |= TBT_FEC_FBK_F_CFG_ACK;
        rx->fbk_epoch = rx->pending_cfg.epoch;
    }
    else if (rx->switch_state == TBT_FEC_SW_APPLIED)
    {
        rx->fbk_flags |= TBT_FEC_FBK_F_SWITCH_DONE;
        rx->fbk_epoch = rx->epoch;
    }
    flags = rx->fbk_flags;
    rx->fbk_flags = 0;
    {
        u64 activity = tbt_fec_rx_est_activity(rx);

        if (flags ||
            (activity && now - activity < 1000000000ULL &&
             now - rx->fbk_last_ns >= 100000000ULL))
            tbt_fec_rx_fbk_send(sess, f, flags);
    }
}

static void tbt_fec_rx_fbk_kick(struct tbt_session *sess, u8 flags)
{
    struct tbt_fec_flow **flows;
    bool owner;
    u16 n, i;

    if (!sess->fec)
        return;
    /* w_fec_rx and a carrier's CFG path can both kick at once. The slot has
     * one owner; a second kick merges its flags and returns, and the owner
     * drains them before it gives the slot back. Coalescing is safe because
     * every flag applies to every live flow. */
    spin_lock_bh(&sess->fec_fbk_lock);
    owner = !sess->fec_fbk_busy;
    if (owner)
        sess->fec_fbk_busy = true;
    else
        sess->fec_fbk_pending |= flags;
    spin_unlock_bh(&sess->fec_fbk_lock);
    if (!owner)
        return;
    flows = sess->fec_fbk_snap;
    for (;;)
    {
        n = tbt_fec_flow_snapshot(sess, flows, TBT_STREAMS_MAX);
        for (i = 0; i < n; i++)
        {
            struct tbt_fec_flow *f = flows[i];
            struct tbt_fec_rx *rx = f->rx;

            if (!rx)
                continue;
            rx->fbk_flags |= flags;
            if (flags & (TBT_FEC_FBK_F_CFG_ACK | TBT_FEC_FBK_F_CFG_NACK |
                         TBT_FEC_FBK_F_SWITCH_DONE))
                rx->fbk_epoch = rx->switch_state == TBT_FEC_SW_APPLIED
                                    ? rx->epoch
                                    : rx->pending_cfg.epoch;
            tbt_fec_rx_fbk_send(sess, f, rx->fbk_flags);
            rx->fbk_flags = 0;
        }
        tbt_fec_flow_put_all(flows, n);
        spin_lock_bh(&sess->fec_fbk_lock);
        flags = sess->fec_fbk_pending;
        sess->fec_fbk_pending = 0;
        if (!flags)
        {
            sess->fec_fbk_busy = false;
            spin_unlock_bh(&sess->fec_fbk_lock);
            break;
        }
        spin_unlock_bh(&sess->fec_fbk_lock);
    }
}

/* Clear the reorder tree and the NACK state for a new shape. The caller
 * holds rx->lock and has already set n/m/groups/base. */
static void tbt_fec_rx_reset_blocks_locked(struct tbt_fec_rx *rx)
{
    u32 total = (u32)rx->groups * rx->slots;
    u32 i;

    for (i = 0; i < total; i++)
    {
        struct tbt_fec_blk *b = &rx->blk[i];

        tbt_fec_blk_slice(rx, b);
        b->used = false;
        b->ready = false;
        b->decoding = false;
        b->empty = false;
        b->win_len_set = false;
        b->short_len = 0;
        b->short_unknown = false;
        b->first_gap_ns = 0;
        b->block_id = ~0ULL;
        b->sid = 0;
        b->nreal = 0;
        b->ndata = 0;
        b->nparity = 0;
        b->count_real = 0xffff;
        b->miss_mask = 0;
        memset(b->miss_bits, 0, sizeof(b->miss_bits));
        memset(b->codec_fail_bits, 0, sizeof(b->codec_fail_bits));
        b->judged = false;
        memset(b->dpresent, 0, TBT_FEC_MAX_SHARES);
        memset(b->ppresent, 0, TBT_FEC_MAX_SHARES);
        memset(b->dlen, 0, TBT_FEC_MAX_SHARES * sizeof(u16));
    }
    memset(rx->req_ns, 0,
           (size_t)total * (rx->n + rx->m) * sizeof(*rx->req_ns));
    memset(rx->req_block, 0, (size_t)total * sizeof(*rx->req_block));
    tbt_fec_nack_reset(rx->nack);
}

/* Install the armed shape at P0. The old codec is kept until the first
 * new-shape record, so an abort can still roll back. */
static void tbt_fec_rx_switch_apply(struct tbt_session *sess,
                                    struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_cfg *c = &rx->pending_cfg;
    struct tbt_stream *st;

    spin_lock_bh(&rx->lock);
    if (rx->switch_state != TBT_FEC_SW_ARMED || !rx->pending_codec)
    {
        spin_unlock_bh(&rx->lock);
        return;
    }
    rx->prev_epoch = rx->epoch;
    rx->prev_shape = rx->shape;
    rx->prev_base_pkt = rx->base_pkt;
    rx->prev_base_block = rx->base_block;
    rx->old_codec = rx->codec;
    rx->codec = rx->pending_codec;
    rx->pending_codec = NULL;
    rx->n = c->n;
    rx->m = c->m;
    rx->groups = c->groups;
    rx->shape.n = c->n;
    rx->shape.m = c->m;
    rx->shape.groups = c->groups;
    rx->shape.w = c->w;
    rx->shape.k = c->k;
    rx->shape.d = c->d;
    rx->shape.codec_variant = c->codec_variant;
    rx->shape.L = c->L;
    rx->shape.block = c->block;
    rx->shape.rate_ppm = c->rate_ppm;
    rx->shape.base_pkt = rx->switch_pkt;
    rx->shape.base_block = rx->switch_block;
    rx->base_pkt = rx->switch_pkt;
    rx->base_block = rx->switch_block;
    rx->epoch = c->epoch;
    tbt_fec_rx_reset_blocks_locked(rx);
    rx->next_super = rx->switch_block;
    rx->highest_block = rx->switch_block;
    rx->switch_state = TBT_FEC_SW_APPLIED;
    rx->switch_start_ns = ktime_get_ns();
    rx->cfg_pending = false;
    rx->fbk_flags |= TBT_FEC_FBK_F_SWITCH_DONE;
    tbt_fec_est_init(rx->est, (u16)(rx->n + rx->m));
    rx->est_abandoned_last = (u64)atomic64_read(&rx->abandoned_shares);
    spin_unlock_bh(&rx->lock);

    /* The sender's DRAIN waits for this ACK: it carries the new frontier
     * and the new credit. */
    st = tbt_stream_get(sess, f->sid, false, false);
    if (st)
    {
        tbt_fec_ack(sess, f, st, rx->next_deliver, rx->deliver_bytes);
        tbt_stream_put(st);
    }
    tbt_fec_rx_fbk_kick(sess, TBT_FEC_FBK_F_SWITCH_DONE);
}

/* Undo ARMED (sender never reached P0) or APPLIED (sender aborted). ARMED
 * only drops the pending codec; APPLIED restores the previous shape at the
 * same P0, so the old-shape stream continues without a byte gap. */
static void tbt_fec_rx_switch_rollback(struct tbt_session *sess,
                                       struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_grouped *drop = NULL;
    bool applied = false;

    spin_lock_bh(&rx->lock);
    if (rx->switch_state == TBT_FEC_SW_ARMED)
    {
        drop = rx->pending_codec;
        rx->pending_codec = NULL;
    }
    else if (rx->switch_state == TBT_FEC_SW_APPLIED)
    {
        applied = true;
        drop = rx->codec;
        rx->codec = rx->old_codec;
        rx->old_codec = NULL;
        rx->n = rx->prev_shape.n;
        rx->m = rx->prev_shape.m;
        rx->groups = rx->prev_shape.groups;
        rx->shape = rx->prev_shape;
        rx->base_pkt = rx->prev_base_pkt;
        rx->base_block = rx->prev_base_block;
        rx->epoch = rx->prev_epoch;
        tbt_fec_rx_reset_blocks_locked(rx);
        rx->next_super = rx->switch_block;
        rx->highest_block = rx->switch_block;
        rx->fbk_flags |= TBT_FEC_FBK_F_ROLLBACK;
        tbt_fec_est_init(rx->est, (u16)(rx->n + rx->m));
        rx->est_abandoned_last = (u64)atomic64_read(&rx->abandoned_shares);
    }
    rx->switch_state = TBT_FEC_SW_STEADY;
    rx->cfg_pending = false;
    rx->rollback_pending = false;
    spin_unlock_bh(&rx->lock);
    tbt_fec_grouped_free(drop);
    atomic64_inc(&sess->fec_switch_rollback);
    if (applied)
        tbt_fec_rx_fbk_kick(sess, TBT_FEC_FBK_F_ROLLBACK);
}

static void tbt_fec_rx_switch_tick(struct tbt_session *sess,
                                   struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_grouped *codec;
    u64 now = ktime_get_ns();

    if (!rx)
        return;
    if (rx->commit_pending)
    {
        struct tbt_fec_grouped *drop = NULL;

        spin_lock_bh(&rx->lock);
        if (rx->commit_pending && rx->switch_state == TBT_FEC_SW_APPLIED)
        {
            rx->commit_pending = false;
            drop = rx->old_codec;
            rx->old_codec = NULL;
            rx->switch_state = TBT_FEC_SW_STEADY;
        }
        else
            rx->commit_pending = false;
        spin_unlock_bh(&rx->lock);
        tbt_fec_grouped_free(drop);
    }
    if (rx->rollback_pending)
    {
        tbt_fec_rx_switch_rollback(sess, f);
        return;
    }
    switch (rx->switch_state)
    {
    case TBT_FEC_SW_PREP:
    {
        struct tbt_fec_shape sh;

        memset(&sh, 0, sizeof(sh));
        sh.n = rx->pending_cfg.n;
        sh.m = rx->pending_cfg.m;
        sh.groups = rx->pending_cfg.groups;
        sh.w = rx->pending_cfg.w;
        sh.k = rx->pending_cfg.k;
        sh.d = rx->pending_cfg.d;
        sh.codec_variant = rx->pending_cfg.codec_variant;
        sh.L = rx->pending_cfg.L;
        sh.block = rx->pending_cfg.block;
        sh.rate_ppm = rx->pending_cfg.rate_ppm;
        codec = tbt_fec_grouped_new_ops(tbt_fec_ops_of_shape(&sh),
                                        sh.groups, sh.n, sh.m, sh.k, sh.w,
                                        TBT_FEC_S, sh.d,
                                        rx->pending_cfg.seed);
    }
        if (!codec)
        {
            rx->switch_state = TBT_FEC_SW_STEADY;
            rx->cfg_pending = false;
            tbt_fec_rx_fbk_kick(sess, TBT_FEC_FBK_F_CFG_NACK);
            return;
        }
        rx->pending_codec = codec;
        rx->switch_state = TBT_FEC_SW_ARMED;
        rx->switch_start_ns = now;
        tbt_fec_rx_fbk_kick(sess, TBT_FEC_FBK_F_CFG_ACK);
        break;
    case TBT_FEC_SW_ARMED:
        if (rx->next_deliver >= rx->switch_pkt)
            tbt_fec_rx_switch_apply(sess, f);
        else if (now - rx->switch_start_ns >= 2000000000ULL)
            tbt_fec_rx_switch_rollback(sess, f);
        break;
    case TBT_FEC_SW_APPLIED:
        /* Wait for the first new-shape record or for an old-shape record
         * that proves the sender aborted. A timeout here would roll back a
         * switch the sender already committed and drop every new-shape
         * record as a foreign epoch. */
        break;
    default:
        break;
    }
}

/* Free one block array allocated by tbt_fec_rx_blk_alloc(). */
static void tbt_fec_rx_blk_free(struct tbt_fec_blk *blk, u32 total)
{
    u32 i;

    if (!blk)
        return;
    for (i = 0; i < total; i++)
    {
        kvfree(blk[i].buf);
        kvfree(blk[i].dpresent);
        kvfree(blk[i].ppresent);
        kvfree(blk[i].dlen);
    }
    kvfree(blk);
}

/* `total` block slots of `shares` shares each, sliced for a block of `n` DATA
 * shares. Returns NULL on the first failure and releases what it took. */
static struct tbt_fec_blk *tbt_fec_rx_blk_alloc(u32 total, u16 shares, u16 n)
{
    struct tbt_fec_blk *blk = tbt_fec_vzalloc((size_t)total * sizeof(*blk));
    u32 i;

    if (!blk)
        return NULL;
    for (i = 0; i < total; i++)
    {
        struct tbt_fec_blk *b = &blk[i];

        b->buf = tbt_fec_vzalloc((size_t)shares * TBT_FEC_S);
        b->dpresent = kvzalloc(TBT_FEC_MAX_SHARES, GFP_KERNEL);
        b->ppresent = kvzalloc(TBT_FEC_MAX_SHARES, GFP_KERNEL);
        b->dlen = kvzalloc(TBT_FEC_MAX_SHARES * sizeof(u16), GFP_KERNEL);
        if (!b->buf || !b->dpresent || !b->ppresent || !b->dlen)
        {
            tbt_fec_rx_blk_free(blk, total);
            return NULL;
        }
        b->data = b->buf;
        b->parity = b->buf + (size_t)n * TBT_FEC_S;
    }
    return blk;
}

/* Install the peer's initial shape (TBT_FEC_CFG_F_INIT). This is the only
 * path that changes the receive geometry outside a switch, and it is accepted
 * only before the first share: a carrier may be placing a share under
 * rx->lock, so the pointer swap happens there and the old arena is freed
 * after the unlock. Runs in w_fec_rx (process context) because it allocates.
 *
 * The peer's L is not on the wire; the local session's L is the only source.
 * Both ends must configure the same codec, so a mismatch is a configuration
 * error the caller reports rather than a shape this path can detect. */
/* Scratch slots for recovered DATA shares. A rate switch changes n/m but not
 * the arena, so size for the largest supported split once, at INIT. */
static u16 tbt_fec_sclc_rec_slots(const struct tbt_fec_cfg *cfg, u16 shares)
{
    struct tbt_fec_adapt_sclc_wire w;
    u16 side = cfg->n > cfg->m ? cfg->n : cfg->m;
    u16 sc;
    u8 mask;

    if (!cfg->groups || !shares ||
        (u32)cfg->groups * shares > TBT_FEC_MAX_CARRIERS)
        return side;
    if (tbt_fec_adapt_sclc_wire_of_cfg(cfg, (u16)(cfg->groups * shares), &w))
        return side;
    /* A rate switch changes n/m but not the arena, so size the recovered
     * slots for the largest side over every ladder point the codec builds.
     * The downlink now builds all of them. */
    mask = (u8)((1u << TBT_FEC_ADAPT_SCLC_LEVELS) - 1u);
    sc = tbt_fec_adapt_sclc_max_side(shares, mask);
    return sc > side ? sc : side;
}

static int tbt_fec_rx_apply_init(struct tbt_session *sess,
                                 struct tbt_fec_flow *f,
                                 const struct tbt_fec_cfg *cfg)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_sclc_shape sclc;
    struct tbt_fec_grouped *codec, *old_codec;
    struct tbt_fec_blk *blk, *old_blk;
    u64 *req_ns, *req_block, *old_req_ns, *old_req_block;
    u8 *rec, *old_rec;
    u32 per, slots, total, old_total;
    u16 shares = (u16)(cfg->n + cfg->m);
    bool same, fresh, block_code = false;

    if (!rx)
        return -EINVAL;
    /* On the far side the flow mask came from the peer's OPEN, so the INIT
     * shape must name the same peer group count. The near side's flow mask is
     * its own TX partition, not this RX direction. */
    if (sess->fec_sclc && sess->tunnel->cfg.role == TBT_ROLE_FAR &&
        !tbt_fec_open_groups_match(true, f->rx->group_mask, cfg->groups))
        return -EINVAL;
    /* The shape names every share the session's carriers carry: groups is
     * carriers/(n+m), checked against the actual carrier count rather than
     * a fixed 256. */
    if ((u32)cfg->groups * shares != sess->n_car)
        return -EINVAL;
    memset(&sclc, 0, sizeof(sclc));
    spin_lock_bh(&rx->lock);
    same = rx->n == cfg->n && rx->m == cfg->m &&
           rx->groups == cfg->groups && rx->shape.w == cfg->w &&
           rx->shape.k == cfg->k && rx->shape.d == cfg->d &&
           rx->shape.codec_variant == cfg->codec_variant &&
           rx->shape.L == cfg->L && rx->shape.block == cfg->block &&
           rx->shape.rate_ppm == cfg->rate_ppm;
    fresh = rx->next_deliver == 0 && rx->next_super == 0 &&
            rx->highest_block == 0 && rx->epoch == 0;
    spin_unlock_bh(&rx->lock);
    if (same)
        return 0;
    if (!fresh)
    {
        atomic64_inc(&sess->fec_epoch_bad);
        return -EBUSY;
    }

    /* Every graph parameter comes from the announced shape, including L:
     * this side's own L describes its transmit codec, and the two directions
     * legitimately differ (downlink L 8, uplink L 16). Using the local L here
     * built the peer's codec with the wrong coupling length, so every share
     * was placed by a graph the sender never encoded with. */
    if (cfg->codec_variant == 2)
    {
        if (tbt_fec_sclc_shape_of_cfg(cfg, (u16)(cfg->groups * shares),
                                      &sclc))
            return -EINVAL;
        /* With adaptive on, the peer's INIT must name a shipped profile on
         * the rate ladder; an off-ladder split cannot be switched later. */
        if (READ_ONCE(sess->adaptive))
        {
            struct tbt_fec_adapt_sclc_wire w;

            if (tbt_fec_adapt_sclc_wire_of_cfg(
                    cfg, (u16)(cfg->groups * shares), &w))
                return -EINVAL;
        }
        codec = tbt_fec_sclc_new(&sclc, TBT_FEC_S, cfg->seed);
        block_code = true;
    }
    else if (sess->fec_sclc)
    {
        /* Mode 3 has no RLNC fallback. A body that is not a buildable
         * SC-LDPC shape is a permanent profile mismatch: refuse it here so
         * the caller NACKs the INIT, instead of building the wrong codec
         * and reporting the factory's NULL as a transient -ENOMEM. */
        return -EINVAL;
    }
    else
    {
        codec = tbt_fec_grouped_new_ops(&tbt_fec_ops_rlnc, cfg->groups,
                                        cfg->n, cfg->m, cfg->k, cfg->w,
                                        TBT_FEC_S, cfg->d, cfg->seed);
    }
    if (!codec)
        return -ENOMEM;

    per = (u32)cfg->groups * shares * TBT_FEC_S;
    slots = TBT_FEC_RX_BUDGET / per;
    if (slots < 4)
        slots = 4;
    if (slots > 256)
        slots = 256;
    total = (u32)cfg->groups * slots;
    blk = tbt_fec_rx_blk_alloc(total, shares, cfg->n);
    req_ns = kvzalloc((size_t)total * shares * sizeof(*req_ns), GFP_KERNEL);
    req_block = kvzalloc((size_t)total * sizeof(*req_block), GFP_KERNEL);
    {
        u16 slots_per_share = cfg->n > cfg->m ? cfg->n : cfg->m;

        if (sess->fec_sclc)
            slots_per_share = tbt_fec_sclc_rec_slots(cfg, shares);
        if (slots_per_share < TBT_FEC_ADAPT_CLASSES)
            slots_per_share = TBT_FEC_ADAPT_CLASSES;
        rec = tbt_fec_vzalloc((size_t)slots_per_share * TBT_FEC_S);
    }
    if (!blk || !req_ns || !req_block || !rec)
    {
        tbt_fec_rx_blk_free(blk, total);
        kvfree(req_ns);
        kvfree(req_block);
        kvfree(rec);
        tbt_fec_grouped_free(codec);
        return -ENOMEM;
    }

    spin_lock_bh(&rx->lock);
    old_blk = rx->blk;
    old_req_ns = rx->req_ns;
    old_req_block = rx->req_block;
    old_rec = rx->rec;
    old_codec = rx->codec;
    old_total = (u32)rx->groups * rx->slots;
    rx->n = cfg->n;
    rx->m = cfg->m;
    rx->groups = cfg->groups;
    rx->slots = (u16)slots;
    if (sess->fec_sclc)
    {
        u16 g;

        memset(rx->group_mask, 0, TBT_FEC_BITMAP_BYTES);
        for (g = 0; g < cfg->groups && g < TBT_FEC_MAX_GROUPS; g++)
            tbt_fec_bitmap_set(rx->group_mask, g);
        tbt_fec_group_map_build(rx->group_mask, TBT_FEC_MAX_GROUPS,
                                rx->group_map);
    }
    rx->blk = blk;
    rx->req_ns = req_ns;
    rx->req_block = req_block;
    rx->rec = rec;
    rx->codec = codec;
    rx->shape.n = cfg->n;
    rx->shape.m = cfg->m;
    rx->shape.groups = cfg->groups;
    rx->shape.slots = (u16)slots;
    rx->shape.w = cfg->w;
    rx->shape.k = cfg->k;
    rx->shape.d = cfg->d;
    rx->shape.codec_variant = cfg->codec_variant;
    rx->shape.L = cfg->L;
    rx->shape.block = cfg->block;
    rx->shape.rate_ppm = cfg->rate_ppm;
    rx->shape.base_pkt = 0;
    rx->shape.base_block = 0;
    rx->prev_shape = rx->shape;
    rx->base_pkt = 0;
    rx->base_block = 0;
    rx->epoch = cfg->epoch;
    rx->replay_k = tbt_fec_replay_k_for(cfg->groups, cfg->n, cfg->m,
                                        TBT_FEC_S);
    rx->nack_cadence_ms = TBT_FEC_NACK_RETRY_MS;
    spin_unlock_bh(&rx->lock);

    tbt_fec_rx_blk_free(old_blk, old_total);
    kvfree(old_req_ns);
    kvfree(old_req_block);
    kvfree(old_rec);
    tbt_fec_grouped_free(old_codec);
    if (rx->est)
        tbt_fec_est_init(rx->est, shares);
    if (rx->est_sclc)
        tbt_fec_adapt_sclc_est_init(rx->est_sclc, shares);
    pr_info_ratelimited("tbtun: tunnel %llu: fec rx shape from peer: n=%u m=%u groups=%u slots=%u block=%u L=%u rate=%u\n",
                        (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id
                                                          : 0),
                        cfg->n, cfg->m, cfg->groups, slots,
                        block_code ? sclc.block : 0,
                        block_code ? sclc.L : 0,
                        block_code
                            ? (sclc.variant == TBT_FEC_SCLC_DOWN
                                   ? sclc.rate_ppm
                                   : sclc.rate_256)
                            : 0);
    return 0;
}

/* w_fec_rx side of TBT_FEC_CFG_F_INIT: apply and acknowledge. The ACK names
 * epoch 1, which is what stops the sender's retry. */
static void tbt_fec_rx_init_tick(struct tbt_session *sess,
                                 struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_cfg cfg;
    int err;

    if (!rx)
        return;
    spin_lock_bh(&rx->lock);
    if (!rx->init_pending)
    {
        spin_unlock_bh(&rx->lock);
        return;
    }
    cfg = rx->init_cfg;
    rx->init_pending = false;
    spin_unlock_bh(&rx->lock);
    err = tbt_fec_rx_apply_init(sess, f, &cfg);
    if (err)
    {
        atomic64_inc(&sess->fec_init_bad);
        pr_warn_ratelimited("tbtun: tunnel %llu: fec init shape rejected: %d n=%u m=%u groups=%u w=%u k=%u d=%u\n",
                            (unsigned long long)sess->tunnel->cfg.id, err,
                            cfg.n, cfg.m, cfg.groups, cfg.w, cfg.k, cfg.d);
        /* A permanent profile mismatch is refused once; a transient
         * allocation failure is left to the sender's retry window. */
        if (err == -EINVAL)
        {
            rx->fbk_epoch = cfg.epoch;
            tbt_fec_rx_fbk_send(sess, f, TBT_FEC_FBK_F_CFG_NACK);
            rx->fbk_epoch = 0;
        }
        return;
    }
    atomic64_inc(&sess->fec_init_rx);
    rx->fbk_flags |= TBT_FEC_FBK_F_CFG_ACK;
    rx->fbk_epoch = cfg.epoch;
    tbt_fec_rx_fbk_send(sess, f, rx->fbk_flags);
    rx->fbk_flags = 0;
}

/* FEC_CFG from the sender. The body is already validated; this function
 * decides whether the shape can be armed at all. Allocation and the ACK run
 * in w_fec_rx, never in the carrier's RX context. */
static void tbt_fec_rx_on_cfg(struct tbt_session *sess,
                              struct tbt_fec_flow *f,
                              const struct tbt_fec_cfg *cfg)
{
    struct tbt_fec_rx *rx = f->rx;
    u64 span;
    bool ack = false;

    if (!rx)
        return;
    /* A group rebalance and a shape switch both move the base. Let the
     * rebalance finish; the controller retries the shape later. */
    if (rx->alloc_state || rx->alloc_abort_pending)
    {
        atomic64_inc(&sess->fec_epoch_bad);
        return;
    }
    /* v7.0 freezes the shape while more than one flow is live: the CFG/FBK
     * handshake still names one session shape, and a switch would move the
     * origin under every flow. A per-flow switch is v7.1. */
    if (sess->n_fec_flows > 1)
    {
        atomic64_inc(&sess->fec_epoch_bad);
        return;
    }
    if (cfg->flags & TBT_FEC_CFG_F_INIT)
    {
        /* The arena swap allocates, and a carrier parser can run in softirq:
         * record the shape here and let w_fec_rx install it. */
        spin_lock_bh(&rx->lock);
        if (READ_ONCE(rx->stopping) || rx->init_pending)
        {
            spin_unlock_bh(&rx->lock);
            return;
        }
        rx->init_cfg = *cfg;
        rx->init_pending = true;
        spin_unlock_bh(&rx->lock);
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
        return;
    }
    if (!READ_ONCE(sess->adaptive))
        return;
    /* Phase 1 keeps the carrier geometry: only n/m move inside n+m=8. */
    if ((u16)(cfg->n + cfg->m) != (u16)(rx->n + rx->m) ||
        cfg->groups != rx->groups)
        return;
    spin_lock_bh(&rx->lock);
    if (cfg->flags & TBT_FEC_CFG_F_ABORT)
    {
        if (rx->cfg_pending || rx->switch_state != TBT_FEC_SW_STEADY)
        {
            rx->cfg_pending = false;
            rx->rollback_pending = true;
        }
        spin_unlock_bh(&rx->lock);
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
        return;
    }
    if (!(cfg->flags & TBT_FEC_CFG_F_APPLY))
    {
        spin_unlock_bh(&rx->lock);
        return;
    }
    if (sess->fec_sclc)
    {
        struct tbt_fec_cfg cur;
        struct tbt_fec_adapt_sclc_wire nw;
        u16 carriers = (u16)(rx->groups * (rx->n + rx->m));

        memset(&cur, 0, sizeof(cur));
        cur.n = rx->n;
        cur.m = rx->m;
        cur.groups = rx->groups;
        cur.w = rx->shape.w;
        cur.k = rx->shape.k;
        cur.d = rx->shape.d;
        cur.codec_variant = rx->shape.codec_variant;
        cur.L = rx->shape.L;
        cur.block = rx->shape.block;
        cur.rate_ppm = rx->shape.rate_ppm;
        if (!tbt_fec_adapt_sclc_wire_compatible(&cur, cfg, carriers) ||
            tbt_fec_adapt_sclc_wire_of_cfg(cfg, carriers, &nw))
        {
            /* A variant/block/L change or an off-ladder split is not a rate
             * switch: refuse it with the CFG's own epoch. The lock is held
             * here; release it before the FBK path. */
            spin_unlock_bh(&rx->lock);
            rx->fbk_epoch = cfg->epoch;
            tbt_fec_rx_fbk_send(sess, f, TBT_FEC_FBK_F_CFG_NACK);
            rx->fbk_epoch = 0;
            return;
        }
    }
    if (cfg->epoch <= rx->epoch && !rx->cfg_pending)
    {
        spin_unlock_bh(&rx->lock);
        return; /* stale generation */
    }
    if (rx->cfg_pending)
    {
        /* A retransmission of the same CFG: repeat the ACK, never re-arm. */
        ack = rx->pending_cfg.epoch == cfg->epoch &&
              rx->switch_state == TBT_FEC_SW_ARMED;
        spin_unlock_bh(&rx->lock);
        if (ack)
            tbt_fec_rx_fbk_kick(sess, TBT_FEC_FBK_F_CFG_ACK);
        return;
    }
    if (rx->switch_state != TBT_FEC_SW_STEADY || rx->rebase_pending)
    {
        spin_unlock_bh(&rx->lock);
        return; /* one switch at a time; a retry after APPLIED is stale */
    }
    span = (u64)rx->groups * rx->n;
    if (cfg->switch_pkt < rx->next_deliver ||
        cfg->switch_pkt < rx->base_pkt ||
        (cfg->switch_pkt - rx->base_pkt) % span ||
        cfg->block_base !=
            rx->base_block + (cfg->switch_pkt - rx->base_pkt) / span)
    {
        atomic64_inc(&sess->fec_epoch_bad);
        spin_unlock_bh(&rx->lock);
        return;
    }
    rx->pending_cfg = *cfg;
    rx->cfg_pending = true;
    rx->switch_pkt = cfg->switch_pkt;
    rx->switch_block = cfg->block_base;
    rx->switch_start_ns = ktime_get_ns();
    rx->switch_state = TBT_FEC_SW_PREP;
    spin_unlock_bh(&rx->lock);
    tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
}

/* A new mask may not collide with a live flow on either side. Dead flows are
 * skipped: their groups are already on the way back to the pool, and their
 * records are demultiplexed by sid, not by group. */
static bool tbt_fec_alloc_mask_overlap(struct tbt_session *sess,
                                       struct tbt_fec_flow *self,
                                       const u8 *mask)
{
    struct tbt_fec_flow *it;
    u16 b, p;
    bool overlap = false;

    spin_lock(&sess->lock);
    for (b = 0; b < TBT_STREAM_HT && !overlap; b++)
        hlist_for_each_entry(it, &sess->fec_flows[b], node)
        {
            if (it == self || !tbt_fec_flow_live(READ_ONCE(it->state)))
                continue;
            for (p = 0; p < sess->fec_groups_total; p++)
                if (tbt_fec_bitmap_test(mask, p) &&
                    tbt_fec_bitmap_test(it->group_mask, p))
                {
                    overlap = true;
                    break;
                }
            if (overlap)
                break;
        }
    spin_unlock(&sess->lock);
    return overlap;
}

/* Drop the RX plane a completed switch no longer needs. The old plane is
 * kept from apply until commit so an ABORT can restore it; a new epoch (or a
 * locally finished round, `force`) proves the exchange completed. The old
 * state stayed `applied` when the reverse direction carried no further data,
 * because the commit event is the first new-plane record; the next ACK arm
 * then failed forever. Called without rx->lock. */
/* Detach and free the rollback copy of the defeated RX plane. Process
 * context only (w_fec_rx commit or w_fec_tx FINISH): the carrier RX handler
 * marks it with tbt_fec_alloc_rx_retire_locked() instead, because vfree and
 * the arena walk must not run under the carrier's sk_lock/BH-disabled
 * section. `state` becomes idle only when it was applied; a newer arm may
 * already own the new plane by the time this runs. */
static void tbt_fec_alloc_rx_release_old(struct tbt_session *sess,
                                         struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f ? f->rx : NULL;
    struct tbt_fec_grouped *codec = NULL;
    struct tbt_fec_blk *blk = NULL;
    u64 *ns = NULL, *rb = NULL;
    u32 total = 0;

    if (!rx)
        return;
    spin_lock_bh(&rx->lock);
    codec = rx->alloc_old_codec;
    blk = rx->alloc_old_blk;
    ns = rx->alloc_old_req_ns;
    rb = rx->alloc_old_req_block;
    total = (u32)rx->alloc_old_groups * rx->slots;
    rx->alloc_old_codec = NULL;
    rx->alloc_old_blk = NULL;
    rx->alloc_old_req_ns = NULL;
    rx->alloc_old_req_block = NULL;
    rx->alloc_old_groups = 0;
    if (rx->alloc_state == 3)
        rx->alloc_state = 0;
    spin_unlock_bh(&rx->lock);
    if (blk)
        tbt_fec_rx_blk_free(blk, total);
    tbt_fec_grouped_free(codec);
    kvfree(ns);
    kvfree(rb);
    (void)sess;
}

/* Carrier RX side: a strictly newer epoch proves the previous exchange
 * completed, but the buffers are freed later by w_fec_rx. Clearing the
 * applied state here is what lets the new ACK arm; leaving it was the
 * "applied then the reverse direction stayed idle" stall. */
static void tbt_fec_alloc_rx_retire_locked(struct tbt_fec_rx *rx,
                                           u32 newer_epoch)
{
    if (rx->alloc_epoch < newer_epoch &&
        (rx->alloc_state == 3 || rx->alloc_old_codec))
    {
        if (rx->alloc_state == 3)
            rx->alloc_state = 0;
        rx->alloc_commit_pending = true;
    }
}

/* FEC_ALLOC reception. Carrier RX only records the event and arms a work
 * item: the RX plane may need allocations and the TX plane may need to close
 * a block, and neither may run in softirq. */
static void tbt_fec_rx_on_alloc(struct tbt_session *sess,
                                struct tbt_fec_flow *f,
                                const struct tbt_fec_alloc *a)
{
    struct tbt_fec_tx *tx;
    struct tbt_fec_rx *rx;
    struct tbt_fec_alloc_flow peer;
    u16 count = 0;
    u64 rbase, rblock, rnext;
    u16 rg, rn;
    bool near = sess->fec_allocator;

    if (!f || !f->tx || !f->rx || sess->fec_sclc || !a->epoch)
    {
        atomic64_inc(&sess->fec_alloc_bad);
        return;
    }
    tx = f->tx;
    rx = f->rx;
    if (!tbt_fec_group_mask_valid(a->group_mask, sess->fec_groups_total,
                                  &count))
    {
        atomic64_inc(&sess->fec_alloc_bad);
        return;
    }
    if (debug)
        pr_info("tbtun: fec alloc rx sid=%llu flags=%#x epoch=%u groups=%u near=%u\n",
                f->sid, a->flags, a->epoch, count, near);
    if (a->flags == 0)
    {
        /* REQUEST: the near TX names its P0; the far side owns this end. */
        u16 map[TBT_FEC_MAX_GROUPS];

        if (near)
        {
            atomic64_inc(&sess->fec_alloc_bad);
            return;
        }
        if (tx->switch_state != TBT_FEC_SW_STEADY ||
            rx->switch_state != TBT_FEC_SW_STEADY)
        {
            if (debug)
                pr_info("tbtun: fec alloc request rejected sid=%llu tx_switch=%u rx_switch=%u\n",
                        f->sid, tx->switch_state, rx->switch_state);
            atomic64_inc(&sess->fec_alloc_bad);
            return;
        }
        if (tbt_fec_group_map_build(a->group_mask,
                                    sess->fec_groups_total, map) != count)
        {
            atomic64_inc(&sess->fec_alloc_bad);
            return;
        }
        spin_lock_bh(&rx->lock);
        rbase = rx->base_pkt;
        rblock = rx->base_block;
        rg = rx->groups;
        rn = rx->n;
        rnext = rx->next_deliver;
        if (a->epoch <= rx->alloc_epoch)
        {
            if (rx->alloc_state == 3 && rx->alloc.epoch == a->epoch)
                rx->alloc_done_resend = true;
            spin_unlock_bh(&rx->lock);
            tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
            spin_lock_bh(&tx->alloc_lock);
            if (tx->alloc_target.epoch == a->epoch)
            {
                if (tx->alloc_committed)
                    tx->alloc_committed_resend = true;
                else if (tx->alloc_ack_sent)
                    tx->alloc_ack_resend = true;
                else
                {
                    /* The role was released before this epoch's ACK went
                     * out (a stale abort, or a stop that failed). Re-run
                     * the responder: stop the TX and answer this REQUEST. */
                    tx->alloc_abort_pending = false;
                    tx->alloc_role = 2;
                    tx->alloc_remote_req = true;
                }
            }
            spin_unlock_bh(&tx->alloc_lock);
            tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
            return;
        }
        spin_unlock_bh(&rx->lock);
        if (!tbt_fec_alloc_switch_valid(a->switch_pkt, a->block_base, rbase,
                                        rblock, rg, rn) ||
            a->switch_pkt < rnext)
        {
            /* A retransmission of a switch we already applied: repeat the
             * answer instead of rejecting it. */
            spin_lock_bh(&rx->lock);
            if (rx->alloc_state == 3 && rx->alloc.epoch == a->epoch)
            {
                rx->alloc_done_resend = true;
                spin_unlock_bh(&rx->lock);
                tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
                spin_lock_bh(&tx->alloc_lock);
                if (tx->alloc_committed &&
                    tx->alloc_target.epoch == a->epoch)
                    tx->alloc_committed_resend = true;
                else if (tx->alloc_ack_sent)
                    tx->alloc_ack_resend = true;
                else if (tx->alloc_target.epoch == a->epoch)
                {
                    /* Same recovery as above for a REQUEST that named a
                     * switch point this side already applied. */
                    tx->alloc_abort_pending = false;
                    tx->alloc_role = 2;
                    tx->alloc_remote_req = true;
                }
                spin_unlock_bh(&tx->alloc_lock);
                tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
                return;
            }
            spin_unlock_bh(&rx->lock);
            atomic64_inc(&sess->fec_alloc_bad);
            return;
        }
        if (tbt_fec_alloc_mask_overlap(sess, f, a->group_mask))
        {
            atomic64_inc(&sess->fec_alloc_overlap);
            return;
        }
        spin_lock_bh(&tx->alloc_lock);
        if (tx->alloc_target.epoch == a->epoch && tx->alloc_ack_sent)
        {
            tx->alloc_ack_resend = true;
            spin_unlock_bh(&tx->alloc_lock);
            tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
            return;
        }
        /* Only an older epoch is stale. A newer REQUEST must take over a
         * responder still holding the previous round (its ABORT may have
         * been lost); the assignment below resets the whole target. */
        if (tx->alloc_role == 2 && a->epoch < tx->alloc_target.epoch)
        {
            spin_unlock_bh(&tx->alloc_lock);
            atomic64_inc(&sess->fec_alloc_bad);
            return;
        }
        tx->alloc_target.epoch = a->epoch;
        tx->alloc_target.groups = count;
        memcpy(tx->alloc_target.mask, a->group_mask, TBT_FEC_BITMAP_BYTES);
        memcpy(tx->alloc_target.map, map, sizeof(map));
        tx->alloc_target.pkt = 0;
        tx->alloc_target.block = 0;
        tx->alloc_role = 2;
        tx->alloc_remote_req = true;
        tx->alloc_ack_sent = false;
        tx->alloc_committed = false;
        tx->alloc_commit_pending = false;
        tx->alloc_hold = false;
        /* A stale abort from the previous epoch must not kill this REQUEST
         * in responder_tick before the TX stops and the ACK goes out. */
        tx->alloc_abort_pending = false;
        spin_unlock_bh(&tx->alloc_lock);
        spin_lock_bh(&rx->lock);
        tbt_fec_alloc_rx_retire_locked(rx, a->epoch);
        rx->alloc.epoch = a->epoch;
        rx->alloc.groups = count;
        memcpy(rx->alloc.mask, a->group_mask, TBT_FEC_BITMAP_BYTES);
        memcpy(rx->alloc.map, map, sizeof(map));
        rx->alloc.pkt = a->switch_pkt;
        rx->alloc.block = a->block_base;
        rx->alloc_state = 2; /* armed */
        rx->alloc_epoch = a->epoch;
        rx->alloc_abort_pending = false;
        spin_unlock_bh(&rx->lock);
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
        return;
    }
    if (a->flags == TBT_FEC_ALLOC_F_ABORT ||
        a->flags == (TBT_FEC_ALLOC_F_ABORT | TBT_FEC_ALLOC_F_ACK))
    {
        if (near)
        {
            bool peer_committed;

            /* ABORT ACK: the far restored its TX to the old mask. Our own RX
             * may have armed (2) or applied (3) that switch; an armed state
             * left behind blocks every later ACK arm. If the far already
             * committed its TX (peer_committed), the new plane is what it
             * still sends on: release the old buffers and keep it. */
            spin_lock_bh(&rx->lock);
            peer_committed = READ_ONCE(tx->alloc.peer_committed);
            if (peer_committed &&
                (rx->alloc_state == 3 || rx->alloc_old_codec))
            {
                if (rx->alloc_state == 3)
                    rx->alloc_state = 0;
                /* Let w_fec_rx free a rollback copy that a newer arm kept
                 * past its epoch; state 2 alone owns no old plane. */
                rx->alloc_commit_pending = true;
            }
            if (rx->alloc.epoch == a->epoch)
            {
                if (rx->alloc_state == 3)
                {
                    rx->alloc_abort_pending = true;
                    rx->alloc_abort_epoch = a->epoch;
                }
                else if (rx->alloc_state == 2 && !peer_committed)
                {
                    /* The peer did not commit: its TX is back on the old
                     * mask, so an armed RX must be disarmed too. When it
                     * did commit, the armed plane is the one its TX now
                     * uses and has to stay. */
                    rx->alloc_state = 0;
                    rx->alloc_epoch = a->epoch; /* tombstone */
                }
            }
            spin_unlock_bh(&rx->lock);
            spin_lock_bh(&tx->alloc_lock);
            if (a->epoch == tx->alloc_target.epoch)
                tbt_fec_alloc_sm_abort_ack(&tx->alloc);
            else
                atomic64_inc(&sess->fec_alloc_bad);
            spin_unlock_bh(&tx->alloc_lock);
            tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
            tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
            return;
        }
        spin_lock_bh(&rx->lock);
        /* Only an ABORT for the plane actually armed or applied may roll it
         * back. A late ABORT from the previous epoch must not disarm the
         * new one. */
        if ((rx->alloc_state == 2 || rx->alloc_state == 3) &&
            rx->alloc.epoch == a->epoch)
        {
            rx->alloc_abort_pending = true;
            rx->alloc_abort_epoch = a->epoch;
        }
        spin_unlock_bh(&rx->lock);
        spin_lock_bh(&tx->alloc_lock);
        if (tx->alloc_role == 2 && tx->alloc_target.epoch == a->epoch)
            tx->alloc_abort_pending = true;
        spin_unlock_bh(&tx->alloc_lock);
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
        return;
    }
    if (near)
    {
        u8 late_state;
        u8 late_role;

        spin_lock_bh(&tx->alloc_lock);
        /* The round is over on this side: ABORT is unwinding it or the SM
         * already released. A late ACK must not arm rx->alloc_state and
         * apply a mask the peer is no longer driving. */
        if ((!tx->alloc_role ||
             tx->alloc.state == TBT_FEC_ALLOC_ABORT ||
             tx->alloc.state == TBT_FEC_ALLOC_FAILED) &&
            (a->flags == TBT_FEC_ALLOC_F_ACK ||
             a->flags == TBT_FEC_ALLOC_F_DONE ||
             a->flags == TBT_FEC_ALLOC_F_COMMITTED))
        {
            late_state = tx->alloc.state;
            late_role = tx->alloc_role;
            spin_unlock_bh(&tx->alloc_lock);
            if (debug)
                pr_info_ratelimited("tbtun: alloc late %#x sid=%llu epoch=%u ignored state=%u role=%u\n",
                                    a->flags, f->sid, a->epoch,
                                    late_state, late_role);
            return;
        }
        if (a->epoch != tx->alloc_target.epoch)
        {
            spin_unlock_bh(&tx->alloc_lock);
            atomic64_inc(&sess->fec_alloc_bad);
            return;
        }
        if (a->flags == TBT_FEC_ALLOC_F_COMMITTED)
            tbt_fec_alloc_sm_peer_committed(&tx->alloc);
        else if (a->flags == TBT_FEC_ALLOC_F_DONE)
            tbt_fec_alloc_sm_peer_done(&tx->alloc);
        else if (a->flags == TBT_FEC_ALLOC_F_ACK)
        {
            /* ACK names the far TX's switch point; that becomes our RX
             * origin. The mask must be the one we asked for. */
            u16 map[TBT_FEC_MAX_GROUPS];

            if (memcmp(a->group_mask, tx->alloc_target.mask,
                       TBT_FEC_BITMAP_BYTES) ||
                tbt_fec_group_map_build(a->group_mask,
                                        sess->fec_groups_total, map) !=
                    tx->alloc_target.groups)
            {
                spin_unlock_bh(&tx->alloc_lock);
                atomic64_inc(&sess->fec_alloc_bad);
                return;
            }
            tx->alloc_peer.epoch = a->epoch;
            tx->alloc_peer.groups = tx->alloc_target.groups;
            memcpy(tx->alloc_peer.mask, a->group_mask,
                   TBT_FEC_BITMAP_BYTES);
            memcpy(tx->alloc_peer.map, map, sizeof(map));
            tx->alloc_peer.pkt = a->switch_pkt;
            tx->alloc_peer.block = a->block_base;
            peer = tx->alloc_peer;
            tbt_fec_alloc_sm_ack(&tx->alloc);
            spin_unlock_bh(&tx->alloc_lock);
            spin_lock_bh(&rx->lock);
            tbt_fec_alloc_rx_retire_locked(rx, a->epoch);
            if (debug)
                pr_info("tbtun: alloc ack t=%llu sid=%llu pkt=%llu blk=%llu grp=%u rx(alloc=%u ep=%u/%u sw=%u base=%llu/%llu grp=%u next=%llu nexts=%llu valid=%d ahead=%d)\n",
                        sess->tunnel ? sess->tunnel->cfg.id : 0, f->sid,
                        a->switch_pkt, a->block_base, count,
                        rx->alloc_state, rx->alloc_epoch, a->epoch,
                        rx->switch_state, rx->base_pkt, rx->base_block,
                        rx->groups, rx->next_deliver, rx->next_super,
                        (int)tbt_fec_alloc_switch_valid(
                            a->switch_pkt, a->block_base, rx->base_pkt,
                            rx->base_block, rx->groups, rx->n),
                        (int)(a->switch_pkt >= rx->next_deliver));
            if (rx->alloc_state == 0 &&
                rx->alloc_epoch < a->epoch &&
                rx->switch_state == TBT_FEC_SW_STEADY &&
                tbt_fec_alloc_switch_valid(a->switch_pkt, a->block_base,
                                           rx->base_pkt, rx->base_block,
                                           rx->groups, rx->n) &&
                a->switch_pkt >= rx->next_deliver)
            {
                rx->alloc = peer;
                rx->alloc_state = 2;
                /* The epoch is armed now: a duplicate of this ACK must not
                 * look like an unknown future epoch (and be counted bad). */
                rx->alloc_epoch = a->epoch;
            }
            else if (rx->alloc_state == 3 &&
                     rx->alloc.epoch == a->epoch)
            {
                /* A repeated ACK means the peer is still waiting for DONE.
                 * Our plane is already applied, so answer again; the resend
                 * is consumed by the RX work for the initiator as well. */
                rx->alloc_done_resend = true;
            }
            else if (a->epoch > rx->alloc_epoch)
                atomic64_inc(&sess->fec_alloc_bad);
            /* A duplicate ACK for an armed epoch is the peer repeating its
             * last response; the applied case above repeats our DONE. */
            spin_unlock_bh(&rx->lock);
            tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
            tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
            return;
        }
        else
            atomic64_inc(&sess->fec_alloc_bad);
        spin_unlock_bh(&tx->alloc_lock);
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
        return;
    }
    /* Far side: DONE from the near means its RX applied at our P0. */
    if (a->flags == TBT_FEC_ALLOC_F_DONE)
    {
        spin_lock_bh(&tx->alloc_lock);
        if (a->epoch == tx->alloc_target.epoch && tx->alloc_ack_sent)
            tx->alloc_commit_pending = true;
        else if (a->epoch == tx->alloc_target.epoch && tx->alloc_committed)
            tx->alloc_committed_resend = true;
        else
            atomic64_inc(&sess->fec_alloc_bad);
        spin_unlock_bh(&tx->alloc_lock);
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
        return;
    }
    atomic64_inc(&sess->fec_alloc_bad);
}

/* Apply, commit or roll back the pending RX switch. Called from w_fec_rx
 * after the delivery walk, so next_deliver has reached the sender's P0. */
static void tbt_fec_alloc_rx_tick(struct tbt_session *sess,
                                  struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_tx *tx = f->tx;
    struct tbt_fec_alloc_flow a;
    struct tbt_fec_grouped *new_codec;
    struct tbt_fec_blk *new_blk;
    u64 *new_req_ns, *new_req_block;
    u32 total;
    u16 shares;
    bool ready;

    if (!rx)
        return;
    if (READ_ONCE(rx->stopping))
        return;
    if (rx->alloc_abort_pending)
    {
        struct tbt_fec_grouped *drop_codec = NULL;
        struct tbt_fec_blk *drop_blk = NULL;
        u64 *drop_req_ns = NULL, *drop_req_block = NULL;
        u32 drop_total = 0;
        u32 epoch;

        spin_lock_bh(&rx->lock);
        rx->alloc_abort_pending = false;
        epoch = rx->alloc_abort_epoch;
        if (rx->alloc_state == 3 && rx->alloc_old_codec)
        {
            /* No new-mask record can exist: the sender aborts before it
             * ever swaps its TX. Restore the old plane at the same P0. */
            drop_codec = rx->codec;
            drop_blk = rx->blk;
            drop_req_ns = rx->req_ns;
            drop_req_block = rx->req_block;
            drop_total = (u32)rx->groups * rx->slots;
            rx->codec = rx->alloc_old_codec;
            rx->blk = rx->alloc_old_blk;
            rx->req_ns = rx->alloc_old_req_ns;
            rx->req_block = rx->alloc_old_req_block;
            rx->groups = rx->alloc_old_groups;
            memcpy(rx->group_mask, rx->alloc_old_mask,
                   TBT_FEC_BITMAP_BYTES);
            memcpy(rx->group_map, rx->alloc_old_map,
                   sizeof(rx->group_map));
            rx->base_pkt = rx->alloc_old_base_pkt;
            rx->base_block = rx->alloc_old_base_block;
            rx->alloc_old_codec = NULL;
            rx->alloc_old_blk = NULL;
            rx->alloc_old_req_ns = NULL;
            rx->alloc_old_req_block = NULL;
            rx->alloc_old_groups = 0;
            /* next_super/highest_block are block ids, not packet seqs.
             * Writing the switch packet here made every record of the
             * restored blocks look older than the cursor, so an ABORT
             * silently discarded the sender's resumed stream (observed as
             * fec_late_share in the thousands and a frozen frontier).
             * alloc.pkt lies on the restored grid: P0 was computed from the
             * old mask, and the initiator never swapped its TX. */
            {
                u64 resume = rx->alloc.pkt;

                if (resume < rx->base_pkt)
                    resume = rx->base_pkt; /* defensive: never go backwards */
                rx->next_super = tbt_fec_block_of_seq_base(
                    resume, rx->base_pkt, rx->base_block, rx->groups, rx->n);
                rx->highest_block = rx->next_super;
                rx->next_deliver = resume;
            }
            tbt_fec_rx_reset_blocks_locked(rx);
            rx->work_pending = true;
        }
        rx->alloc_state = 0;
        rx->alloc_done_sent = false;
        rx->alloc_epoch = epoch; /* tombstone: never re-arm this epoch */
        rx->rebase_pending = false;
        spin_unlock_bh(&rx->lock);
        if (drop_blk)
            tbt_fec_rx_blk_free(drop_blk, drop_total);
        tbt_fec_grouped_free(drop_codec);
        kvfree(drop_req_ns);
        kvfree(drop_req_block);
        if (!sess->fec_allocator)
        {
            /* Only the responder owns the answer. An initiator rolls its
             * applied plane back silently after receiving ABORT|ACK;
             * answering would bounce the record between the two ends. A
             * failed send keeps the abort pending so the next RX pass
             * repeats it instead of dropping the peer's only release. */
            if (tbt_fec_send_alloc(sess, f,
                                   TBT_FEC_ALLOC_F_ABORT |
                                       TBT_FEC_ALLOC_F_ACK,
                                   epoch, rx->group_mask, rx->base_pkt,
                                   rx->base_block))
            {
                spin_lock_bh(&rx->lock);
                rx->alloc_abort_pending = true;
                rx->alloc_abort_epoch = epoch;
                spin_unlock_bh(&rx->lock);
                tbt_session_arm(sess, TBT_SW_FEC_RX,
                                msecs_to_jiffies(10));
                return;
            }
            atomic64_inc(&sess->fec_alloc_abort);
        }
        return;
    }
    if (rx->alloc_commit_pending)
    {
        spin_lock_bh(&rx->lock);
        rx->alloc_commit_pending = false;

        spin_unlock_bh(&rx->lock);
        /* First record on the new plane: the old plane can go and the next
         * epoch may arm. */
        tbt_fec_alloc_rx_release_old(sess, f);
    }
    if (rx->alloc_done_resend && rx->alloc_state == 3)
    {
        spin_lock_bh(&rx->lock);
        a = rx->alloc;
        rx->alloc_done_resend = false;
        spin_unlock_bh(&rx->lock);
        (void)tbt_fec_send_alloc(sess, f, TBT_FEC_ALLOC_F_DONE,
                                 a.epoch, a.mask, a.pkt, a.block);
        return;
    }
    if (rx->alloc_state != 2)
        return;
    spin_lock_bh(&rx->lock);
    a = rx->alloc;
    /* A reset left the reorder window empty for the next record; it must not
     * hold the allocation switch back. The apply below installs the new base
     * and clears the flag, and the rollback path clears it too. */
    ready = !rx->deliver_partial && rx->next_deliver == a.pkt &&
            rx->switch_state == TBT_FEC_SW_STEADY;
    spin_unlock_bh(&rx->lock);
    if (!ready)
    {
        if (debug)
            pr_info_ratelimited("tbtun: alloc apply-wait t=%llu sid=%llu next=%llu pkt=%llu blk=%llu partial=%d sw=%u state=%u rebase=%d\n",
                                sess->tunnel ? sess->tunnel->cfg.id : 0,
                                f->sid, READ_ONCE(rx->next_deliver), a.pkt,
                                a.block, (int)READ_ONCE(rx->deliver_partial),
                                READ_ONCE(rx->switch_state),
                                READ_ONCE(rx->alloc_state),
                                (int)READ_ONCE(rx->rebase_pending));
        tbt_session_arm(sess, TBT_SW_FEC_RX, msecs_to_jiffies(10));
        return;
    }
    if (debug)
        pr_info("tbtun: alloc apply t=%llu sid=%llu next=%llu pkt=%llu blk=%llu grp=%u\n",
                sess->tunnel ? sess->tunnel->cfg.id : 0, f->sid,
                READ_ONCE(rx->next_deliver), a.pkt, a.block, a.groups);
    total = (u32)a.groups * rx->slots;
    shares = (u16)(rx->n + rx->m);
    new_blk = tbt_fec_rx_blk_alloc(total, shares, rx->n);
    new_req_ns = kvzalloc((size_t)total * shares * sizeof(*new_req_ns),
                          GFP_KERNEL);
    new_req_block = kvzalloc((size_t)total * sizeof(*new_req_block),
                             GFP_KERNEL);
    new_codec = tbt_fec_codec_new(NULL, a.groups, rx->n, rx->m, rx->n,
                                  rx->shape.w, rx->shape.d,
                                  tbt_fec_base_nonce(sess));
    if (!new_blk || !new_req_ns || !new_req_block || !new_codec)
    {
        if (new_blk)
            tbt_fec_rx_blk_free(new_blk, total);
        tbt_fec_grouped_free(new_codec);
        kvfree(new_req_ns);
        kvfree(new_req_block);
        tbt_session_arm(sess, TBT_SW_FEC_RX, msecs_to_jiffies(10));
        return;
    }
    spin_lock_bh(&rx->lock);
    if (rx->alloc_state != 2 || rx->next_deliver != a.pkt ||
        rx->deliver_partial || rx->switch_state != TBT_FEC_SW_STEADY)
    {
        spin_unlock_bh(&rx->lock);
        tbt_fec_rx_blk_free(new_blk, total);
        tbt_fec_grouped_free(new_codec);
        kvfree(new_req_ns);
        kvfree(new_req_block);
        tbt_session_arm(sess, TBT_SW_FEC_RX, msecs_to_jiffies(10));
        return;
    }
    rx->alloc_old_codec = rx->codec;
    rx->alloc_old_blk = rx->blk;
    rx->alloc_old_req_ns = rx->req_ns;
    rx->alloc_old_req_block = rx->req_block;
    rx->alloc_old_groups = rx->groups;
    memcpy(rx->alloc_old_mask, rx->group_mask, TBT_FEC_BITMAP_BYTES);
    memcpy(rx->alloc_old_map, rx->group_map, sizeof(rx->group_map));
    rx->alloc_old_base_pkt = rx->base_pkt;
    rx->alloc_old_base_block = rx->base_block;
    rx->codec = new_codec;
    rx->blk = new_blk;
    rx->req_ns = new_req_ns;
    rx->req_block = new_req_block;
    rx->groups = a.groups;
    memcpy(rx->group_mask, a.mask, TBT_FEC_BITMAP_BYTES);
    memcpy(rx->group_map, a.map, sizeof(rx->group_map));
    rx->base_pkt = a.pkt;
    rx->base_block = a.block;
    rx->next_super = a.block;
    rx->highest_block = a.block;
    rx->next_deliver = a.pkt;
    rx->alloc_epoch = a.epoch;
    rx->alloc_state = 3; /* applied */
    rx->alloc_done_sent = false;
    rx->rebase_pending = false;
    tbt_fec_rx_reset_blocks_locked(rx);
    rx->work_pending = true;
    spin_unlock_bh(&rx->lock);
    if (sess->fec_allocator)
    {
        bool dead;

        spin_lock_bh(&tx->alloc_lock);
        dead = tx->alloc.state == TBT_FEC_ALLOC_FAILED ||
               !tx->alloc_role;
        if (!dead)
            tbt_fec_alloc_sm_rx_done(&tx->alloc);
        spin_unlock_bh(&tx->alloc_lock);
        if (dead)
        {
            /* The round timeout released the state machine before this RX
             * reached the peer's P0. The far responder still holds its TX
             * at P0 waiting for DONE; send it from the plane just applied so
             * the peer can commit and resume instead of holding forever. */
            (void)tbt_fec_send_alloc(sess, f, TBT_FEC_ALLOC_F_DONE,
                                     a.epoch, a.mask, a.pkt, a.block);
        }
        else
            tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    }
    else
    {
        (void)tbt_fec_send_alloc(sess, f, TBT_FEC_ALLOC_F_DONE,
                                 a.epoch, a.mask, a.pkt, a.block);
        spin_lock_bh(&rx->lock);
        rx->alloc_done_sent = true;
        spin_unlock_bh(&rx->lock);
    }
    tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
}

/* One pass over this flow's groups*slots. Called only by w_fec_rx. Returns
 * the number of blocks decoded, so the caller can yield after a whole-block
 * decode instead of running several of them before it delivers the first
 * one. */
static int tbt_fec_rx_pass_flow(struct tbt_session *sess,
                                struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    u16 group, slot;
    int decoded = 0;

    for (group = 0; group < rx->groups; group++)
        for (slot = 0; slot < rx->slots; slot++)
        {
            if (tbt_fec_rx_decode_one(sess, f, group, slot))
                decoded++;
            /* A grouped block code decodes one group per slot and a group
             * block can run tens of milliseconds on the target CPU.  Let
             * the carrier works and the timer softirq run between them. */
            cond_resched();
        }
    return decoded;
}

static void tbt_fec_rx_work_body(struct tbt_session *sess)
{
    bool again;
    int rounds = 0;
    int decoded = 0;

    if (!sess->fec || READ_ONCE(sess->closing) ||
        READ_ONCE(sess->fec_stopping))
        return;
    do
    {
        struct tbt_fec_flow **flows = sess->fec_rx_snap;
        u16 n, i;

        again = false;
        n = tbt_fec_flow_snapshot(sess, flows, TBT_STREAMS_MAX);
        for (i = 0; i < n; i++)
        {
            struct tbt_fec_flow *f = flows[i];
            struct tbt_fec_rx *rx = f->rx;

            if (!rx || READ_ONCE(rx->stopping))
                continue;
            decoded += tbt_fec_rx_pass_flow(sess, f);
            /* Ask for the missing shares before the delivery walk arms
             * its own (possibly 30 s) stall timer: the earlier 10 ms arm
             * wins and keeps the NACK retry alive. */
            tbt_fec_nack_scan(sess, f);
            if (tbt_fec_deliver_ready(sess, f) == -ENOMEM)
                continue; /* a retry is already armed */
            /* The delivery walk may have reached an FEC_ALLOC switch point;
             * apply, commit or roll back the RX plane before the next pass. */
            tbt_fec_alloc_rx_tick(sess, f);
            spin_lock_bh(&rx->lock);
            if (rx->work_pending)
            {
                rx->work_pending = false;
                again = true;
            }
            spin_unlock_bh(&rx->lock);
        }
        tbt_fec_flow_put_all(flows, n);
        /* A block decode is the expensive unit (tens of milliseconds on the
         * target).  Deliver and re-arm after one such round instead of
         * stacking up to four of them, which starved the ACK and NACK walk
         * behind the decode and held the delivery cursor for hundreds of
         * milliseconds.  The re-arm below runs the next round at once. */
        if (decoded)
            break;
    } while (again && ++rounds < 4);
    if (again)
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
}

/* Slow RX cadence: a NACK retry, the estimator sample or the stall poll.
 * The delays range from 10 ms to the 30 s stall budget; on w_fec_rx they
 * would hold a ready block's 0 ms wake for the whole delay. The entry asks
 * w_fec_rx for one pass, and that pass arms this work again when the next
 * super-block is still not ready. */
static void tbt_fec_rx_poll_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_rx_poll);
    bool owned = test_and_clear_bit(TBT_SW_FEC_RX_POLL, &sess->sched);

    if (!READ_ONCE(sess->closing) && !READ_ONCE(sess->fec_stopping) &&
        READ_ONCE(sess->fec))
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
    if (owned)
        tbt_session_put(sess);
}

void tbt_fec_rx_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_rx);
    bool owned = test_and_clear_bit(TBT_SW_FEC_RX, &sess->sched);

    if (atomic_fetch_or(1u << TBT_FEC_RUN_BIT, &sess->fec_rx_run) &
        (1u << TBT_FEC_RUN_BIT))
    {
        atomic_fetch_or(1u << TBT_FEC_AGAIN_BIT, &sess->fec_rx_run);
        goto out;
    }
    tbt_fec_rx_work_body(sess);
    if (sess->n_fec_flows <= 1)
    {
        struct tbt_fec_flow *primary = tbt_fec_primary_get(sess);

        if (primary)
        {
            tbt_fec_rx_adapt_tick(sess, primary);
            tbt_fec_flow_put(primary);
        }
    }
    atomic_fetch_and(~(1u << TBT_FEC_RUN_BIT), &sess->fec_rx_run);
    if (atomic_fetch_and(~(1u << TBT_FEC_AGAIN_BIT), &sess->fec_rx_run) &
        (1u << TBT_FEC_AGAIN_BIT))
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
    /* The estimator needs a periodic sample even on an idle queue: the 1 s
     * window expires and the FBK cadence is 100 ms. */
    if (READ_ONCE(sess->adaptive) && !READ_ONCE(sess->fec_stopping) &&
        !READ_ONCE(sess->closing) && sess->n_fec_flows <= 1)
        tbt_session_arm(sess, TBT_SW_FEC_RX_POLL, msecs_to_jiffies(100));
out:
    if (owned)
        tbt_session_put(sess);
}

static int tbt_fec_rx_record_body(struct tbt_session *sess,
                                  struct tbt_fec_flow *f,
                                  struct tbt_fec_hdr *hp, u8 *rec, size_t len)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_hdr h = *hp;
    struct tbt_fec_blk *b;
    const u8 *payload;
    u16 shares, group, share, slot;
    bool wake = false, rebase;
    bool fresh = false;

    if (!rx || len < TBT_FEC_HDR_LEN)
        return -EPROTO;
    {
        /* Teardown may start between the carrier parse and this record. Stop
         * before decryption so a non-zero return leaves the bytes untouched;
         * the raw parser consumes the record on any return, so a retry can
         * never decrypt the same buffer twice. */
        if (READ_ONCE(sess->fec_stopping) || READ_ONCE(rx->stopping))
            return -ESHUTDOWN;
        /* A duplicate record must be dropped before decryption. The record
         * buffer is decrypted in place; a second pass XORs the plaintext and
         * fails the payload CRC, which looks like wire corruption and hides
         * the real duplicate. The header CRC above makes these names
         * trustworthy. */
        if (h.type == TBT_FEC_DATA || h.type == TBT_FEC_PARITY)
        {
            u16 cg = 0xffff;
            u16 csh = h.share_index;
            u16 cshares;
            u64 next = READ_ONCE(rx->next_super);
            bool dup = false, late = false;

            spin_lock_bh(&rx->lock);
            /* The shape fields are read under rx->lock: w_fec_rx may apply a
             * switch between the header check and this prefilter. A record
             * whose shape is not the current one is left to the placement
             * path, which decides between late, epoch_bad and rollback. */
            cshares = (u16)(rx->n + rx->m);
            if (h.n == (u8)rx->n && h.m == (u8)rx->m &&
                h.seq >= rx->base_pkt)
                cg = tbt_fec_group_of_seq_base(h.seq, rx->base_pkt,
                                               rx->groups);
            if (cg < rx->groups && csh < cshares &&
                h.block_id >= next && h.block_id < next + rx->slots)
            {
                b = &rx->blk[(size_t)cg * rx->slots +
                             (h.block_id % rx->slots)];
                if (b->used && b->block_id == h.block_id)
                {
                    if (b->empty)
                    {
                        /* A repeated marker is a duplicate; a DATA or real
                         * PARITY for an empty block stays -EPROTO below. */
                        if (h.type == TBT_FEC_PARITY && h.win_len == 0)
                            dup = true;
                    }
                    else if (b->ready)
                    {
                        late = true;
                    }
                    else if (h.type == TBT_FEC_DATA && b->dpresent[csh])
                    {
                        dup = true;
                    }
                    else if (h.type == TBT_FEC_PARITY && h.win_len &&
                             csh >= rx->n &&
                             b->ppresent[csh - rx->n])
                    {
                        dup = true;
                    }
                }
            }
            spin_unlock_bh(&rx->lock);
            if (debug >= 2 && (dup || late))
                pr_info("tbtun: fec %s share block=%llu group=%u share=%u type=%u win_len=%u epoch=%u next=%llu\n",
                        late ? "late" : "dup", h.block_id, cg, csh, h.type,
                        h.win_len, h.epoch, next);
            if (dup)
            {
                atomic64_inc(&sess->fec_dup_share);
                return 0;
            }
            if (late)
            {
                atomic64_inc(&sess->fec_late_share);
                return 0;
            }
        }
        /* Header framing is trusted here; decrypt DATA/PARITY before the
         * payload CRC, which covers the clear bytes (T0 §1.3, §2.2). The rx
         * context is shared by carriers and body_xor is read-only. */
        if (sess->crypto &&
            (h.type == TBT_FEC_DATA || h.type == TBT_FEC_PARITY))
        {
            if (!smp_load_acquire(&sess->crypto_ready))
            {
                atomic64_inc(&sess->fec_drop_crypto);
                return 0;
            }
            if (tbt_crypto_body_xor_flow(&sess->crypto_ctx.rx, h.sid, h.seq,
                                         h.type == TBT_FEC_PARITY,
                                         h.share_index,
                                         rec + TBT_FEC_HDR_LEN,
                                         h.payload_len))
                return 0; /* counter overflow: drop this share */
        }
        if (tbt_fec_crc32c(0, rec + TBT_FEC_HDR_LEN, h.payload_len) !=
            h.payload_crc32c)
        {
            /* The framing was sound, so only this share is lost. */
            if (debug)
                pr_info_ratelimited("tbtun: fec payload crc fail: type=%u seq=%llu share=%u len=%u block=%llu hdr_crc=%08x\n",
                                    h.type, h.seq, h.share_index,
                                    h.payload_len, h.block_id,
                                    h.header_crc32c);
            atomic64_inc(&sess->fec_payload_crc_fail);
            return 0;
        }
    }
    if (h.type == TBT_FEC_ACK)
    {
        struct tbt_fec_ack a;
        struct tbt_stream *st;
        struct tbt_fec_tx *tx;

        if (tbt_fec_ack_unpack(rec + TBT_FEC_HDR_LEN, &a))
            return 0;
        atomic64_inc(&sess->fec_ack_rx);
        tx = f->tx;
        st = tbt_stream_get(sess, f->sid, false, false);
        if (st && tx)
        {
            /* A peer-controlled u64 must never become a byte count by a bare
             * multiply: next_deliver * S wraps, and a frontier past tx->seq
             * would acknowledge bytes this side never sent. Clamp the packet
             * frontier first; it only gates credit. The byte count comes from
             * the body's deliver_bytes, which names what the receiver really
             * handed over. Deriving it from the frontier acknowledged bytes a
             * short share or an early-closed block never delivered. */
            u64 sent = READ_ONCE(tx->seq);
            u64 acked;

            if (a.next_deliver > sent)
            {
                a.next_deliver = sent;
                atomic64_inc(&sess->fec_ack_bad);
            }
            acked = a.deliver_bytes;
            if (acked > READ_ONCE(st->tx_seq))
            {
                /* The ACK belongs to another stream of this session, or its
                 * byte count is hostile. Never acknowledge more than this
                 * stream submitted. */
                acked = READ_ONCE(st->tx_seq);
                atomic64_inc(&sess->fec_ack_bad);
            }
            spin_lock(&sess->lock);
            if (acked > READ_ONCE(st->tx_ack))
            {
                WRITE_ONCE(st->tx_ack, acked);
                st->last_ack_ns = ktime_get_ns();
                sess->tx_ack = acked;
                sess->last_ack_ns = st->last_ack_ns;
            }
            spin_unlock(&sess->lock);
            /* Flow control: the peer has delivered up to this packet, and
             * grants the credit it sent. Wake the packetizer, which may have
             * stopped at the window. */
            /* ACKs now rotate over the carriers, so they can arrive out of
             * order; the frontier is cumulative and must only move forward.
             * A stale ACK that moved it back would shrink the sender's
             * window and stall it until the next ACK caught up. */
            {
                u64 cur = READ_ONCE(tx->peer_pkts);

                /* Carriers process records concurrently, so two ACKs can
                 * race here; a plain write would let the older one win. */
                while (a.next_deliver > cur)
                {
                    u64 prev = cmpxchg(&tx->peer_pkts, cur, a.next_deliver);

                    if (prev == cur)
                        break;
                    cur = prev;
                }
            }
            /* Zero credit is not "unlimited": it would stop the packetizer
             * forever if it replaced a real window, and accepting it as a
             * grant would overrun the receiver's arena. Ignore it and keep
             * the last good credit. A peer cannot widen the window past what
             * the receiver can buffer. */
            if (!a.credit_packets)
                atomic64_inc(&sess->fec_ack_bad);
            else
            {
                /* The peer cannot widen the window past the replay ring:
                 * the oldest block it may still NACK must stay in the ring. */
                WRITE_ONCE(tx->credit,
                           min_t(u32, a.credit_packets,
                                 READ_ONCE(tx->credit_cap)));
            }
            /* The peer's measured RTT is how long a replay needs to come
             * back; the NACK reply deduplicates inside it. A zero sample is
             * "not measured yet", not a zero-latency path. */
            if (a.rtt_us)
                WRITE_ONCE(tx->rtt_us, a.rtt_us);
            tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
            /* The FIN pump holds the close until tx_ack covers tx_seq. */
            tbt_tx_wake(sess);
        }
        if (st)
            tbt_stream_put(st);
        return 0;
    }
    if (h.type == TBT_FEC_NACK)
    {
        struct tbt_fec_nack n;
        struct tbt_fec_tx *tx = f->tx;
        u64 t0 = ktime_get_ns();
        u64 hold;
        u8 *buf;
        u8 marker_done[TBT_FEC_BITMAP_BYTES];
        u16 g, s, shares, tn, tm, tg;
        u64 tbase;
        u16 tband;
        u32 requested = 0;
        u32 nack_limit = TBT_FEC_NACK_REQ_MAX;
        bool full = false;
        bool never = false; /* a requested share was not resident */

        if (tbt_fec_nack_unpack(rec + TBT_FEC_HDR_LEN, &n))
            return -EPROTO;
        if (!tx || !READ_ONCE(tx->replay))
        {
            atomic64_inc(&sess->fec_replay_miss_no_ring);
            atomic64_inc(&sess->fec_replay_miss);
            return 0;
        }
        /* Snapshot the shape once: a switch may run between this NACK and
         * the replay lookup. A NACK for a block below the new base is from
         * the old epoch; the ring has been reset, so it cannot be served. */
        spin_lock_bh(&tx->shape_lock);
        tn = tx->n;
        tm = tx->m;
        tg = tx->groups;
        tbase = tx->base_block;
        tband = tx->band;
        spin_unlock_bh(&tx->shape_lock);
        if (h.block_id < tbase)
        {
            atomic64_inc(&sess->fec_nack_stale_epoch);
            return 0;
        }
        shares = (u16)(tn + tm);
        if (!tbt_fec_bitmap_clear_from(n.group_mask, tg) ||
            !tbt_fec_bitmap_clear_from(n.share_mask, shares))
        {
            /* A NACK naming a share outside the shape in force is a record
             * from before the initial-shape install or from the other
             * direction's geometry. Drop it: returning -EPROTO here counted
             * against the carrier, and a join-time burst of stale NACKs is
             * not a reason to kill a carrier. */
            atomic64_inc(&sess->fec_nack_bad);
            return 0;
        }
        atomic64_inc(&sess->fec_nack_rx);
        atomic64_add(tbt_fec_bitmap_count(n.share_mask),
                     &sess->fec_nack_req_shares);
        /* Repair is active from this point: the sender is about to build and
         * queue replay records, and the TX stall check must not kill the
         * stream while that work is in flight. */
        WRITE_ONCE(sess->fec_repair_last_ns, t0);
        /* The block is already delivered: a replay cannot help, and every
         * share of it would be a duplicate on the wire. This races a NACK
         * sent before the ACK reached the peer. */
        if (tbt_fec_block_delivered_base(
                h.block_id, READ_ONCE(tx->peer_pkts), tx->base_pkt,
                tx->base_block, tx->groups, tx->n))
        {
            atomic64_add(tbt_fec_bitmap_count(n.share_mask),
                         &sess->fec_replay_dedup);
            return 0;
        }
        /* A replayed share needs one round trip to arrive. Naming it again
         * inside that window only puts a duplicate record ahead of the
         * repair already queued. */
        hold = tbt_fec_nack_hold_ns(READ_ONCE(tx->rtt_us),
                                    TBT_FEC_NACK_RETRY_MS);
        buf = kmalloc(TBT_FEC_REC_MAX, GFP_ATOMIC);
        if (!buf)
        {
            atomic64_inc(&sess->fec_replay_miss_alloc);
            atomic64_inc(&sess->fec_replay_miss);
            return -ENOMEM;
        }
        memset(marker_done, 0, sizeof(marker_done));
        {
            u32 cap = tbt_fec_replay_cap(READ_ONCE(sess->fec_closing),
                                         TBT_FEC_REPLAY_BYTES_MAX);
            u32 limit = TBT_FEC_NACK_REQ_MAX;

            spin_lock(&sess->fec_out_lock);
            if (sess->fec_replay_bytes >= cap / 2)
                limit = TBT_FEC_NACK_REQ_THROTTLED;
            spin_unlock(&sess->fec_out_lock);
            if (limit > TBT_FEC_NACK_REQ_MAX)
                limit = TBT_FEC_NACK_REQ_MAX;
            nack_limit = limit;
        }
        for (g = 0; g < tg && !full; g++)
        {
            if (!tbt_fec_bitmap_test(n.group_mask, g))
                continue;
            if (requested >= nack_limit)
                break;
            for (s = 0; s < shares; s++)
            {
                struct tbt_carrier *pc;
                u16 cidx;
                bool replaced = false;
                int err, len;

                if (!tbt_fec_bitmap_test(n.share_mask, s))
                    continue;
                /* r_fail counts DATA shares the peer had to name, not the
                 * parity rows it also asked for. */
                if (s < tn)
                    tx->nack_req_shares++;
                /* A NACK may name a large bitmap; bound the replay work per
                 * record. The receiver retries the remainder. */
                if (++requested >= nack_limit)
                    break;
                spin_lock_bh(&tx->replay_lock);
                len = tbt_fec_replay_get(tx->replay, h.block_id, g, s, buf,
                                         TBT_FEC_REC_MAX);
                spin_unlock_bh(&tx->replay_lock);
                if (len > 0)
                {
                    u64 last;
                    bool dup;

                    spin_lock_bh(&tx->replay_lock);
                    last = tbt_fec_replay_sent_ns(tx->replay, h.block_id,
                                                  g, s);
                    dup = tbt_fec_replay_dup(t0, last, hold);
                    if (!dup)
                        tbt_fec_replay_mark_sent(tx->replay, h.block_id, g,
                                                 s, t0);
                    spin_unlock_bh(&tx->replay_lock);
                    if (dup)
                    {
                        atomic64_inc(&sess->fec_replay_dedup);
                        continue;
                    }
                    if (tbt_fec_replay_space(sess) < (u32)len)
                    {
                        atomic64_inc(&sess->fec_nack_backpressure);
                        full = true;
                        break;
                    }
                }
                if (len == -ENODATA)
                {
                    bool replied = false;
                    u16 band = tband;
                    u16 first;
                    u16 ri;

                    if (debug >= 2)
                        pr_info("tbtun: fec NACK absent block=%llu group=%u share=%u cur=%llu gblk=%llu\n",
                                h.block_id, g, s, READ_ONCE(tx->cur_block),
                                READ_ONCE(tx->g[g].blk_id));
                    if (h.block_id >= READ_ONCE(tx->cur_block))
                        continue; /* still being built: the share may appear */
                    /* A block code stores parity 0 for every block it wrote.
                     * If the named DATA share sits at or past the cut that
                     * row states, it is padding the sender never wrote: not
                     * a replay miss. Answer with the row (the receiver then
                     * learns win_len and stops naming padding), and count
                     * the skipped request. A request for a real share that
                     * was never stored is a miss and takes the same row as
                     * its answer. */
                    if (s < tn && !band)
                    {
                        struct tbt_fec_hdr ph0;
                        int plen0;

                        spin_lock_bh(&tx->replay_lock);
                        plen0 = tbt_fec_replay_get(tx->replay, h.block_id, g,
                                                   tn, buf, TBT_FEC_REC_MAX);
                        spin_unlock_bh(&tx->replay_lock);
                        if (plen0 > 0 && !tbt_fec_hdr_unpack(buf, &ph0) &&
                            ph0.win_len && s >= ph0.win_len)
                            atomic64_inc(&sess->fec_nack_pad_skip);
                        else
                        {
                            atomic64_inc(&sess->fec_replay_miss_never_stored);
                            atomic64_inc(&sess->fec_replay_miss_get);
                            atomic64_inc(&sess->fec_replay_miss);
                        }
                        if (plen0 > 0)
                        {
                            u64 last;
                            bool dup;

                            spin_lock_bh(&tx->replay_lock);
                            last = tbt_fec_replay_sent_ns(tx->replay,
                                                          h.block_id, g, tn);
                            dup = tbt_fec_replay_dup(t0, last, hold);
                            if (!dup)
                                tbt_fec_replay_mark_sent(tx->replay,
                                                         h.block_id, g, tn,
                                                         t0);
                            spin_unlock_bh(&tx->replay_lock);
                            if (dup)
                            {
                                atomic64_inc(&sess->fec_replay_dedup);
                                continue;
                            }
                            if (tbt_fec_replay_space(sess) < (u32)plen0)
                            {
                                atomic64_inc(&sess->fec_nack_backpressure);
                                full = true;
                                break;
                            }
                            if (tbt_fec_replay_enqueue(sess, buf, (u16)plen0,
                                                       t0, tx->flow))
                                continue;
                            atomic64_inc(&sess->fec_nack_retx);
                            atomic64_add((u64)plen0,
                                         &sess->fec_nack_retx_bytes);
                            atomic64_inc(&sess->fec_nack_parity_reply);
                            atomic64_inc(&sess->fec_nack_absent_reply);
                            continue;
                        }
                        atomic64_inc(&sess->fec_nack_req_invalid);
                        continue;
                    }
                    atomic64_inc(&sess->fec_replay_miss_never_stored);
                    atomic64_inc(&sess->fec_replay_miss_get);
                    atomic64_inc(&sess->fec_replay_miss);
                    /* With a parity band, row r covers DATA share s when
                     * (s + m - (r % m)) % m < band. r = s % m always covers
                     * it (band >= 1); a fixed row 0 does not cover s with
                     * s % m >= band, and repeating it cannot decode the
                     * block. Start at r = s % m, then try the other covering
                     * rows if that row is not in the ring. A no-band DATA
                     * share was answered above. */
                    first = s < tn ? (u16)(s % tm) : (u16)(s - tn);
                    for (ri = 0; ri < tm && !replied; ri++)
                    {
                        u16 r = (u16)((first + ri) % tm);
                        u64 last;
                        bool dup;
                        int plen;

                        if (s < tn &&
                            (u16)((s + tm - (r % tm)) % tm) >= band)
                            continue;
                        spin_lock_bh(&tx->replay_lock);
                        plen = tbt_fec_replay_get(tx->replay, h.block_id, g,
                                                  (u16)(tn + r), buf,
                                                  TBT_FEC_REC_MAX);
                        spin_unlock_bh(&tx->replay_lock);
                        if (plen <= 0)
                            continue;
                        spin_lock_bh(&tx->replay_lock);
                        last = tbt_fec_replay_sent_ns(
                            tx->replay, h.block_id, g, (u16)(tn + r));
                        dup = tbt_fec_replay_dup(t0, last, hold);
                        if (!dup)
                            tbt_fec_replay_mark_sent(tx->replay, h.block_id,
                                                     g, (u16)(tn + r),
                                                     t0);
                        spin_unlock_bh(&tx->replay_lock);
                        if (dup)
                        {
                            /* This row is already on its way; it answers the
                             * request, so no empty marker is needed. */
                            atomic64_inc(&sess->fec_replay_dedup);
                            replied = true;
                            break;
                        }
                        if (tbt_fec_replay_space(sess) < (u32)plen)
                        {
                            atomic64_inc(&sess->fec_nack_backpressure);
                            full = true;
                            break;
                        }
                        if (tbt_fec_replay_enqueue(sess, buf, (u16)plen, t0,
                                                   tx->flow))
                            continue;
                        atomic64_inc(&sess->fec_nack_retx);
                        atomic64_add((u64)plen, &sess->fec_nack_retx_bytes);
                        atomic64_inc(&sess->fec_nack_parity_reply);
                        atomic64_inc(&sess->fec_nack_absent_reply);
                        if (debug >= 2)
                        {
                            struct tbt_fec_hdr ph;

                            if (!tbt_fec_hdr_unpack(buf, &ph))
                                pr_info("tbtun: fec NACK parity reply block=%llu group=%u parity=%u win_len=%u epoch=%u\n",
                                        h.block_id, g, r, ph.win_len,
                                        ph.epoch);
                        }
                        replied = true;
                    }
                    /* The block is resident, so this group did write into
                     * it: the group is not empty and a marker would falsely
                     * mark real data as padding. The share can never exist
                     * (padding beyond the block's cut, or a parity row that
                     * was never stored); no answer exists, and asking again
                     * cannot change that. */
                    if (!replied && !full)
                        atomic64_inc(&sess->fec_nack_req_invalid);
                    continue;
                }
                if (len == -ESTALE)
                {
                    u64 cur = READ_ONCE(tx->cur_block);
                    u16 rk = tbt_fec_replay_k(tx->replay);

                    /* -ESTALE means "this block is not resident". A block
                     * the sender never built reads the same as one the ring
                     * has wrapped past; counting both as evictions makes a
                     * probe for a block that does not exist yet look like a
                     * ring that is too shallow. Only a block the ring has
                     * moved at least K ids past is really evicted. */
                    if (cur >= h.block_id && cur - h.block_id >= rk)
                    {
                        atomic64_inc(&sess->fec_replay_miss_evicted);
                        if (debug >= 2)
                            pr_info("tbtun: fec NACK evicted block=%llu group=%u share=%u cur=%llu k=%u\n",
                                    h.block_id, g, s, cur, rk);
                    }
                    else
                    {
                        atomic64_inc(&sess->fec_replay_miss_never_stored);
                        never = true;
                        if (debug >= 2)
                            pr_info("tbtun: fec NACK unbuilt block=%llu group=%u share=%u cur=%llu k=%u\n",
                                    h.block_id, g, s, cur, rk);
                    }
                    atomic64_inc(&sess->fec_replay_miss_get);
                    atomic64_inc(&sess->fec_replay_miss);
                    /* The block is closed, still inside the ring, and no
                     * record of it exists for this group: the group is empty
                     * in that block. Markers are not stored, so this is the
                     * only path that answers such a NACK. */
                    if (tbt_fec_nack_empty_group(
                            cur > h.block_id,
                            cur >= h.block_id && cur - h.block_id < rk,
                            READ_ONCE(tx->g[g].blk_id), h.block_id) &&
                        !tbt_fec_bitmap_test(marker_done, g))
                    {
                        tbt_fec_bitmap_set(marker_done, g);
                        if (!tbt_fec_send_empty_marker(sess, tx, g, h.block_id))
                        {
                            atomic64_inc(&sess->fec_nack_empty_reply);
                            atomic64_inc(&sess->fec_nack_absent_reply);
                        }
                    }
                    continue;
                }
                if (len <= 0)
                {
                    atomic64_inc(&sess->fec_replay_miss_get);
                    atomic64_inc(&sess->fec_replay_miss);
                    continue;
                }
                atomic64_inc(&sess->fec_replay_hit);
                /* Attribute the replay to a carrier replacement when the
                 * share's own slot has lost its carrier. */
                cidx = (u16)(g * (tn + tm) + s);
                if (cidx < sess->n_car)
                {
                    pc = tbt_carrier_get(sess, (u8)cidx);
                    if (pc)
                    {
                        replaced = atomic_read(&pc->stopping) ||
                                   !atomic_read(&pc->connected);
                        tbt_carrier_put(pc);
                    }
                }
                err = tbt_fec_replay_enqueue(sess, buf, (u16)len, t0,
                                             tx->flow);
                if (err)
                {
                    if (err == -ENOMEM)
                        atomic64_inc(&sess->fec_replay_miss_alloc);
                    else
                        atomic64_inc(&sess->fec_replay_miss_enospc);
                    atomic64_inc(&sess->fec_replay_miss);
                    continue;
                }
                atomic64_inc(&sess->fec_nack_retx);
                atomic64_add((u64)len, &sess->fec_nack_retx_bytes);
                if (replaced)
                    atomic64_add((u64)len,
                                 &sess->fec_replace_retx_bytes);
            }
        }
        kfree(buf);
        if (never)
            atomic64_inc(&sess->fec_nack_never_stored);
        return 0;
    }
    if (h.type == TBT_FEC_FBK)
    {
        struct tbt_fec_fbk fb;
        struct tbt_fec_tx *tx = f->tx;

        if (tbt_fec_fbk_unpack(rec + TBT_FEC_HDR_LEN, &fb))
            return -EPROTO;
        if (!tx)
            return 0;
        /* The report names the receive shape; a mismatch means the two
         * ends disagree about the committed codec, so the values would be
         * meaningless. Drop it and count, as §7 requires. */
        if (!tbt_fec_bitmap_tail_zero(fb.carrier_bitmap, sess->n_car) ||
            !tbt_fec_fbk_variant_match(fb.codec_variant, fb.L, fb.block,
                                       tx->codec_variant, tx->sclc_L,
                                       tx->sclc_block, tx->groups))
        {
            atomic64_inc(&sess->fec_epoch_bad);
            return 0;
        }
        tx->peer_p_ppm = fb.p_ewma_ppm;
        if (sess->fec_sclc)
            tx->peer_p_fast_ppm = fb.p_fast_ppm;
        tx->peer_expected = fb.expected_shares;
        tx->peer_received = fb.received_shares;
        tx->peer_epoch = tbt_fec_fbk_epoch(&fb);
        tx->peer_flags = fb.flags;
        tx->peer_est_ns = ktime_get_ns();
        tx->peer_est_valid = true;
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
        return 0;
    }
    if (h.type == TBT_FEC_ALLOC)
    {
        struct tbt_fec_alloc a;

        if (tbt_fec_alloc_unpack(rec + TBT_FEC_HDR_LEN, &a))
            return -EPROTO;
        tbt_fec_rx_on_alloc(sess, f, &a);
        return 0;
    }
    if (h.type == TBT_FEC_CFG)
    {
        struct tbt_fec_cfg cfg;
        u16 carriers = sess->tunnel->cfg.carriers;

        if (tbt_fec_cfg_unpack(rec + TBT_FEC_HDR_LEN, carriers, &cfg))
            return -EPROTO;
        tbt_fec_rx_on_cfg(sess, f, &cfg);
        return 0;
    }
    if (h.type != TBT_FEC_DATA && h.type != TBT_FEC_PARITY)
        return 0;

    payload = rec + TBT_FEC_HDR_LEN;
    spin_lock_bh(&rx->lock);
    if (READ_ONCE(rx->stopping))
    {
        spin_unlock_bh(&rx->lock);
        return -ESHUTDOWN;
    }
    /* Shape validation belongs under rx->lock: w_fec_rx applies a switch
     * here. A record of the previous shape after APPLIED means the sender
     * aborted; a record of any other shape is a stale epoch, not a broken
     * carrier, so it is dropped and counted. */
    {
        u16 cur_n = rx->n, cur_m = rx->m, cur_groups = rx->groups;
        u64 cur_base = rx->base_pkt, cur_block = rx->base_block;
        bool prev_match = rx->prev_shape.n == h.n &&
                          rx->prev_shape.m == h.m &&
                          rx->switch_state == TBT_FEC_SW_APPLIED &&
                          h.seq >= rx->switch_pkt;

        if (h.n != (u8)cur_n || h.m != (u8)cur_m)
        {
            spin_unlock_bh(&rx->lock);
            if (prev_match)
            {
                rx->rollback_pending = true;
                tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
            }
            else
                atomic64_inc(&sess->fec_epoch_bad);
            return 0;
        }
        if (h.seq < cur_base)
        {
            atomic64_inc(&sess->fec_late_share);
            spin_unlock_bh(&rx->lock);
            return 0;
        }
        shares = (u16)(cur_n + cur_m);
        group = tbt_fec_group_of_seq_base(h.seq, cur_base, cur_groups);
        share = h.share_index;
        if (!shares || group >= cur_groups || share >= shares ||
            h.block_id != tbt_fec_block_of_seq_base(h.seq, cur_base,
                                                    cur_block, cur_groups,
                                                    cur_n))
        {
            atomic64_inc(&sess->fec_epoch_bad);
            spin_unlock_bh(&rx->lock);
            return 0;
        }
        if (rx->switch_state == TBT_FEC_SW_APPLIED)
            rx->commit_pending = true;
        /* First record on the new group set: the old arena and codec can be
         * released by w_fec_rx. A record below the new base cannot exist:
         * the sender only resumes after this side's DONE. */
        if (rx->alloc_state == 3 && h.seq >= rx->base_pkt)
        {
            rx->alloc_commit_pending = true;
            rx->work_pending = true;
        }
    }
    /* Freeze the shape the stream started in before any switch can move
     * base_pkt. FIN validates its pkt_base (the stream's first packet, which
     * predates every later switch) against this origin and span. Capturing
     * them lazily at FIN time measured the post-switch origin, so every FIN
     * of a stream that had adapted failed alignment and the peer reset the
     * stream instead of delivering its tail. */
    if (!rx->stream_span)
    {
        rx->stream_span = (u64)rx->groups * rx->n;
        rx->stream_base_pkt = rx->base_pkt;
    }
    /* The reorder window belongs to one stream. A reset leaves it empty
     * (rebase_pending), and a record of another sid may only move it once the
     * delivery walk has drained the stream it holds; the first such record
     * rebases it to its own block. A lower block before the stream's first
     * delivery lowers the window to itself, so an out-of-order first record
     * costs a NACK instead of skipping the blocks below it.
     *
     * The session's first stream is not rebased: it starts at packet 0, so its
     * window is already at the base and a first block whose records were lost
     * must be NACKed, not skipped. `rebase_sid` is bound here so a later
     * switch can tell whose blocks the tree holds. */
    rebase = READ_ONCE(rx->rebase_pending);
    switch (tbt_fec_rx_window_step(rx->next_super, h.block_id, rx->slots,
                                   rebase,
                                   rx->deliver_bytes != 0 ||
                                       rx->deliver_partial))
    {
    case TBT_FEC_RX_WIN_REBASE:
    {
        /* The first rebase uses the smallest block seen for this sid: a
         * record that arrived before the reset kept its minimum, so the
         * shares the reset wiped stay inside the window and the NACK scan
         * asks for them. */
        u64 base = h.block_id;

        if (rx->min_block_seen && rx->min_block_seen < base)
            base = rx->min_block_seen;
        if (debug)
            pr_info("tbtun: fec rx rebase sid=%llu block=%llu base=%llu next=%llu highest=%llu\n",
                    h.sid, h.block_id, base, rx->next_super,
                    rx->highest_block);
        rx->rebase_sid = h.sid;
        rx->min_block_seen = base;
        rx->next_super = base;
        if (base > rx->highest_block)
            rx->highest_block = base;
        rx->next_deliver = tbt_fec_seq_of_block_pkt_base(
            base, 0, 0, rx->base_pkt, rx->base_block, rx->groups, rx->n);
        if (!rx->stream_pkt_base)
            rx->stream_pkt_base = rx->next_deliver;
        rx->progress_super = 0;
        rx->progress_ns = 0;
        rx->rebase_pending = false;
        break;
    }
    case TBT_FEC_RX_WIN_LATE:
        atomic64_inc(&sess->fec_late_share);
        spin_unlock_bh(&rx->lock);
        return 0; /* already delivered */
    case TBT_FEC_RX_WIN_FAR:
        atomic64_inc(&sess->fec_drop_window);
        spin_unlock_bh(&rx->lock);
        return 0; /* too far ahead to buffer */
    case TBT_FEC_RX_WIN_PLACE:
        break;
    }
    if (h.block_id >= rx->next_super + rx->slots)
    {
        atomic64_inc(&sess->fec_drop_window);
        spin_unlock_bh(&rx->lock);
        return 0; /* the kept minimum from before the reset left this record
                   * beyond the window; the NACK scan recovers the rest */
    }
    slot = (u16)((size_t)group * rx->slots + (h.block_id % rx->slots));
    b = &rx->blk[slot];
    if (!b->used || b->block_id != h.block_id)
    {
        tbt_fec_blk_reset(sess, f, b, h.block_id);
        b->sid = f->sid;
    }
    /* Defensive: a parity record is a share of this block. A path that
     * placed parity without the reset must still make the block visible to
     * the decode/NACK walk. */
    b->used = true;
    if (h.block_id > rx->highest_block)
        rx->highest_block = h.block_id;
    if (debug >= 2)
        pr_info("tbtun: fec marker rx block=%llu next=%llu used=%d ready=%d empty=%d type=%u win_len=%u\n",
                h.block_id, READ_ONCE(rx->next_super), b->used, b->ready,
                b->empty, h.type, h.win_len);
    /* A settled block is immutable. Classify the record before anything
     * mutates it: a replayed empty marker, a duplicate marker for an empty
     * block, and a real share that lost the race are all normal on a NACK
     * path, not protocol errors. Returning -EPROTO here made the carrier
     * parser kill the carrier that carried the record, which dropped every
     * share in flight on it (field: rx_proto=93, 102 replacements). */
    switch (tbt_fec_arrival_kind(b->ready, b->empty, h.type, h.win_len,
                                 b->ndata || b->nparity || b->win_len_set ||
                                     b->count_real != 0xffff))
    {
    case TBT_FEC_ARR_LATE:
        /* The block is immutable once ready: a duplicate or late share must
         * not change a byte that may already be on its way to the stream. */
        atomic64_inc(&sess->fec_late_share);
        /* A DATA share for a real slot that is still absent would mean the
         * block became ready without it. The ready rule (ndata >= real) makes
         * this impossible; count it separately so a field run can tell it
         * from the normal parity-after-complete case. */
        if (h.type == TBT_FEC_DATA &&
            share < tbt_fec_blk_real_len(rx, group, b) &&
            !b->dpresent[share])
            atomic64_inc(&sess->fec_late_data);
        spin_unlock_bh(&rx->lock);
        return 0;
    case TBT_FEC_ARR_DUP_MARKER:
        /* A replayed marker for a block that is already empty. It carries no
         * payload, so placing it again changes nothing; a NACK retry can
         * deliver it several times. */
        atomic64_inc(&sess->fec_late_share);
        atomic64_inc(&sess->fec_dup_marker);
        spin_unlock_bh(&rx->lock);
        return 0;
    case TBT_FEC_ARR_CONFLICT:
        /* Marker and real shares disagree. The block is settled, so the
         * record is ignored and named; killing the carrier would punish every
         * share in flight on it for one shape disagreement. */
        atomic64_inc(&sess->fec_marker_conflict);
        spin_unlock_bh(&rx->lock);
        return 0;
    case TBT_FEC_ARR_PLACE:
        break;
    }

    if (h.type == TBT_FEC_DATA)
    {
        /* A PARITY record may have fixed this block's cut before this share
         * arrived. If the cut says this slot holds no stream byte, placing the
         * share would make it invisible to the delivery walk (which skips
         * every slot at or past the cut) while the packet frontier and the
         * ACK ran past it: the exact silent 8192-byte deletion this check
         * exists to refuse. */
        if (tbt_fec_share_past_cut(b->win_len_set, b->nreal, share))
        {
            atomic64_inc(&sess->fec_slot_past_cut);
            spin_unlock_bh(&rx->lock);
            return -EPROTO;
        }
        if (b->dpresent[share])
        {
            atomic64_inc(&sess->fec_dup_share);
        }
        else
        {
            fresh = true;
            memcpy(b->data + (size_t)share * rx->s, payload, h.payload_len);
            /* The sender zero-pads a short share before encoding parity. The
             * decoder XORs the whole S-byte slot into the right-hand side, so
             * the receiver must zero the same tail; otherwise stale bytes
             * from the slot's previous block corrupt every share recovered
             * from this one. The tail is never delivered: dlen names the real
             * payload. */
            if (h.payload_len < rx->s)
                memset(b->data + (size_t)share * rx->s + h.payload_len, 0,
                       rx->s - h.payload_len);
            b->dlen[share] = h.payload_len;
            b->dpresent[share] = 1;
            /* ndata counts present shares inside the block's current real
             * range. A share outside it is padding for a shape the block no
             * longer has, and the next counts pass drops it. */
            if (share < tbt_fec_blk_real_len(rx, group, b))
                b->ndata++;
            if (h.flags & TBT_FEC_FF_LAST)
            {
                if (!rx->total_known)
                {
                    rx->total_pkts = h.seq + 1;
                    rx->total_known = true;
                }
            }
        }
    }
    else if (h.win_len == 0)
    {
        /* Empty-block marker. It fixes the block as having no real share, so
         * a later DATA or PARITY for it is a mixed-source protocol error. */
        if (b->ndata || b->nparity || b->win_len_set ||
            b->count_real != 0xffff)
        {
            spin_unlock_bh(&rx->lock);
            return -EPROTO;
        }
        b->empty = true;
        b->win_len_set = true;
        b->ready = true;
        atomic64_inc(&sess->fec_pad_share);
        rx->work_pending = true;
        if (debug >= 2)
            pr_info("tbtun: fec marker placed block=%llu group=%u next=%llu empty=%d ready=%d\n",
                    h.block_id, group, READ_ONCE(rx->next_super), b->empty,
                    b->ready);
    }
    else
    {
        u16 idx = (u16)(h.share_index - rx->n);

        if (b->ppresent[idx])
        {
            atomic64_inc(&sess->fec_dup_share);
        }
        else
        {
            fresh = true;
            memcpy(b->parity + (size_t)idx * rx->s, payload, h.payload_len);
            b->ppresent[idx] = 1;
            b->nparity++;
        }
        if (!b->win_len_set)
        {
            u16 i;

            /* The first PARITY fixes the block's cut. A DATA share at or past
             * win_len means the peer used a different block shape for the
             * same block; recovering it would deliver padding as data. */
            for (i = h.win_len; i < rx->n; i++)
            {
                if (b->dpresent[i])
                {
                    atomic64_inc(&sess->fec_epoch_conflict);
                    spin_unlock_bh(&rx->lock);
                    return -EPROTO;
                }
            }
            b->nreal = h.win_len;
            b->short_len = h.epoch;
            b->win_len_set = true;
        }
        else if (b->nreal != h.win_len || b->short_len != h.epoch)
        {
            atomic64_inc(&sess->fec_epoch_conflict);
            spin_unlock_bh(&rx->lock);
            return -EPROTO;
        }
    }
    tbt_fec_blk_counts(rx, group, b);
    if (b->empty || b->ndata >= tbt_fec_blk_real_len(rx, group, b))
    {
        b->ready = true;
        rx->work_pending = true; /* deliver_ready() has a block to hand over */
    }
    else if (tbt_fec_grouped_mode(rx->codec) == 3
                 ? b->nparity > 0
                 : b->nparity >=
                       (u16)(tbt_fec_blk_real_len(rx, group, b) - b->ndata))
    {
        /* RLNC needs one row per missing share before a decode can succeed;
         * a block codec needs any parity and recovers what it can, so one
         * arriving parity share must wake the decoder. Waiting for the RLNC
         * count left SC-LDPC blocks asleep until the next unrelated pass. */
        if (fresh && tbt_fec_grouped_mode(rx->codec) == 3)
        {
            /* The decoder runs once the block has been quiet for a while.
             * Every new share pushes the deadline out, so a burst coalesces
             * into one decode at the end instead of one per share.  A
             * duplicate changes no block state and must not postpone the
             * decode or clear the backoff. */
            b->decode_next_ns =
                ktime_get_ns() +
                (u64)TBT_FEC_DECODE_SETTLE_MS * NSEC_PER_MSEC;
            b->decode_tries = 0;
        }
        rx->work_pending = true;
    }
    wake = rx->work_pending;
    spin_unlock_bh(&rx->lock);
    if (wake)
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
    return 0;
}

/* Every per-stream record names its flow in h.sid. The wrapper parses and
 * verifies the header once, takes a flow reference, and runs the body; a late
 * record for a detached flow finds no entry and is dropped, never delivered
 * into another flow's arena. */
int tbt_fec_rx_record(struct tbt_carrier *c, u8 *rec, size_t len)
{
    struct tbt_session *sess = c->sess;
    struct tbt_fec_hdr h;
    struct tbt_fec_flow *f;
    int rc;

    if (!sess || !sess->fec)
        return -EPROTO;
    rc = tbt_fec_rec_check(rec, len, TBT_FEC_S, &h);
    if (rc)
    {
        if (rc == -EBADMSG)
            atomic64_inc(&sess->fec_hdr_crc_fail);
        else
            atomic64_inc(&sess->fec_proto_fail);
        return -EPROTO;
    }
    if (READ_ONCE(sess->fec_stopping))
        return -ESHUTDOWN;
    if (!h.sid)
    {
        /* v7 requires a routing key on every FEC record. A zero sid is a
         * pre-v7 or hostile record: drop it rather than guess a flow. */
        atomic64_inc(&sess->fec_drop_sid);
        return 0;
    }
    f = tbt_fec_flow_lookup(sess, h.sid);
    if (!f)
    {
        atomic64_inc(&sess->fec_drop_sid);
        return 0;
    }
    rc = tbt_fec_rx_record_body(sess, f, &h, rec, len);
    tbt_fec_flow_put(f);
    return rc;
}

/* FIN names the stream's absolute packet range: pkt_end is the packet after
 * its last DATA packet, pkt_base the block-aligned packet of its first one.
 * The packet count is therefore pkt_end itself, not ceil(bytes / S): a short
 * share makes the byte count an upper bound on the packet count, and the
 * second stream's pkt_end must not be compared with the first stream's
 * next_super base.
 *
 * A FIN that arrives before its stream is adopted is parked in next_fin_*;
 * deliver_ready() consumes it when the stream identity settles. */
void tbt_fec_rx_fin(struct tbt_session *sess, struct tbt_stream *st,
                    u64 total_bytes, u64 pkt_end, u64 pkt_base)
{
    struct tbt_fec_flow *f;
    struct tbt_fec_rx *rx;
    u64 span;
    bool wake = false;

    if (!sess || !st)
        return;
    f = st->fec_flow;
    if (!f || !f->rx)
        return;
    rx = f->rx;
    spin_lock_bh(&rx->lock);
    if (READ_ONCE(rx->stopping))
    {
        spin_unlock_bh(&rx->lock);
        return;
    }
    span = (u64)rx->groups * rx->n;
    if (!rx->stream_span)
    {
        rx->stream_span = span;
        rx->stream_base_pkt = rx->base_pkt;
    }
    if (!tbt_fec_fin_valid_base(pkt_end, pkt_base, rx->stream_base_pkt,
                                rx->stream_span))
    {
        atomic64_inc(&sess->fec_fin_bad);
        spin_unlock_bh(&rx->lock);
        return;
    }
    if (READ_ONCE(rx->rebase_pending))
    {
        u64 base = tbt_fec_block_of_seq_base(pkt_base, rx->base_pkt,
                                             rx->base_block, rx->groups,
                                             rx->n);

        rx->next_super = base;
        rx->next_deliver = pkt_base;
        rx->stream_pkt_base = pkt_base;
        rx->highest_block = base;
        rx->progress_super = 0;
        rx->progress_ns = 0;
        rx->rebase_pending = false;
    }
    if (!rx->total_known)
    {
        rx->total_bytes = total_bytes;
        rx->total_pkts = pkt_end;
        rx->total_known = true;
    }
    else
    {
        if (pkt_end != rx->total_pkts)
            atomic64_inc(&sess->fec_fin_bad); /* keep LAST's value */
        if (!rx->total_bytes)
            rx->total_bytes = total_bytes;
    }
    rx->work_pending = true;
    wake = true;
    spin_unlock_bh(&rx->lock);
    if (wake)
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
}

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

/* Flow frees run on a module-owned workqueue, not the system-wide one: the
 * last tbt_fec_flow_put() often runs inside tbt_session_destroy(), and rmmod
 * must be able to wait for every queued free before the module text is
 * unmapped. synchronize_rcu()/rcu_barrier() only cover RCU callbacks.
 * flush_scheduled_work() is not an option either: it is deprecated and warns
 * at runtime since 6.6, it waits for every unrelated item on a system-wide
 * queue (the deadlock its deprecation exists to prevent), and its target
 * queue changed from system_wq to system_percpu_wq in 7.0. A private queue
 * gives a barrier that covers exactly our frees on every supported release;
 * destroy_workqueue() is that barrier. */
static struct workqueue_struct *tbt_fec_free_wq;

int tbt_fec_data_module_init(void)
{
    tbt_fec_free_wq = alloc_workqueue("tbt_fec_free",
                                      WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
    return tbt_fec_free_wq ? 0 : -ENOMEM;
}

void tbt_fec_data_module_exit(void)
{
    if (!tbt_fec_free_wq)
        return;
    /* tbt_tunnel_shutdown() has returned, so no session, stream or work item
     * can take another flow reference. Draining here waits for queued and
     * running frees; each free's own synchronize_rcu() completes first. */
    destroy_workqueue(tbt_fec_free_wq);
    tbt_fec_free_wq = NULL;
}

void tbt_fec_data_free(struct tbt_session *sess)
{
    struct tbt_fec_flow *f;
    struct tbt_fec_out *o, *tmp;
    u16 i, b;

    /* The work items are cancelled before this point. The session drain work
     * is session-owned and must be stopped before its queues go. ndrain is
     * zero until INIT_DELAYED_WORK has run, so a partial init is safe. */
    if (sess->fec_ndrain)
    {
        cancel_delayed_work_sync(&sess->fec_drain_work);
        bitmap_zero(sess->fec_drain_ready, TBT_MAX_CARRIERS_LIMIT);
        bitmap_zero(sess->fec_drain_retry, TBT_MAX_CARRIERS_LIMIT);
        sess->fec_retry_at = 0;
        sess->fec_drain_cursor = 0;
        sess->fec_ndrain = 0;
    }

    /* Clear the RCU-published views before the table reference can be the
     * last put. free_work waits one grace period before releasing the
     * planes; a reader that loaded fec_tx/fec_rx must be ordered before
     * that grace period, not after it. This also covers a partially
     * initialised session (fec == false). */
    tbt_fec_primary_publish(sess, NULL);

    if (sess->fec)
    {
        spin_lock(&sess->fec_out_lock);
        list_for_each_entry_safe(o, tmp, &sess->fec_out_ctl, node)
            tbt_fec_out_free(sess, NULL, o);
        list_for_each_entry_safe(o, tmp, &sess->fec_out_replay, node)
            tbt_fec_out_free(sess, NULL, o);
        for (i = 0; i < sess->n_car; i++)
        {
            struct tbt_fec_outq *q = &sess->fec_outq[i];

            list_for_each_entry_safe(o, tmp, &q->queue, node)
                tbt_fec_out_free(sess, q, o);
        }
        spin_unlock(&sess->fec_out_lock);

        /* Drop the table's reference. A flow whose stream still holds one is
         * freed by tbt_fec_flow_detach(); session destroy detaches every
         * stream before this call, so a non-empty remainder is a bug. */
        for (b = 0; b < TBT_STREAM_HT; b++)
        {
            while (!hlist_empty(&sess->fec_flows[b]))
            {
                f = hlist_entry(sess->fec_flows[b].first,
                                struct tbt_fec_flow, node);
                hlist_del_init(&f->node);
                if (sess->n_fec_flows)
                    sess->n_fec_flows--;
                tbt_fec_flow_put(f);
            }
        }
    }
    kvfree(sess->fec_outq);
    sess->fec_outq = NULL;
    sess->fec = false;
}

/* One grouped container for the configured shape. The RLNC path keeps the
 * caller's window; mode 3 resolves the variant to its ops table and the
 * parameter permutation that table documents (see tbt_fec.h). */
struct tbt_fec_grouped *tbt_fec_sclc_new(const struct tbt_fec_sclc_shape *sh,
                                         size_t s, u64 nonce)
{
    const struct tbt_fec_ops *ops;
    u16 k, w, d;

    if (!sh)
        return NULL;
    if (sh->variant == TBT_FEC_SCLC_DOWN)
    {
        ops = &tbt_fec_ops_sclc;
        k = sh->rate_ppm;
        w = sh->block;
        d = 0;
    }
    else if (sh->variant == TBT_FEC_SCLC_UP)
    {
        ops = &tbt_fec_ops_sclc_gf16;
        k = sh->block;
        w = sh->L;
        d = sh->rate_256;
    }
    else
    {
        return NULL;
    }
    return tbt_fec_grouped_new_ops(ops, sh->groups, sh->n, sh->m, k, w, s, d,
                                   nonce);
}

/* v8 names the codec in the CFG extension. The legacy w/k/d mirror is not a
 * second classifier: a shape with variant 0/1 is RLNC whatever its window
 * looks like. */
static const struct tbt_fec_ops *tbt_fec_ops_of_shape(
    const struct tbt_fec_shape *sh)
{
    if (!sh)
        return &tbt_fec_ops_rlnc;
    if (sh->codec_variant == 2)
        return sh->groups == 1 ? &tbt_fec_ops_sclc
                               : &tbt_fec_ops_sclc_gf16;
    return &tbt_fec_ops_rlnc;
}

static struct tbt_fec_grouped *tbt_fec_codec_new(
    const struct tbt_fec_sclc_shape *sclc, u16 groups, u16 n, u16 m, u16 k,
    u16 w, u8 d, u64 nonce)
{
    if (sclc)
        return tbt_fec_sclc_new(sclc, TBT_FEC_S, nonce);
    return tbt_fec_grouped_new(groups, n, m, k, w, TBT_FEC_S, d, nonce);
}

/* ------------------------------------------------------------------ *
 * Flow table and per-flow planes (TBT_FEC_CONV 0x1008)
 *
 * A flow owns a subset of the session's physical groups. Its TX and RX planes
 * are full copies of the old session planes, sized by G_i groups; the session
 * keeps only the carrier queues, the work items, the shape and the replay K.
 * With sum(G_i) == total_groups the memory and the aggregate credit equal the
 * old single-stream allocation (design §3.1).
 * ------------------------------------------------------------------ */

static struct tbt_fec_flow *
tbt_fec_flow_find_locked(struct tbt_session *sess, u64 sid)
{
    struct tbt_fec_flow *f;

    hlist_for_each_entry(f, &sess->fec_flows[TBT_STREAM_BUCKET(sid)], node)
        if (f->sid == sid)
            return f;
    return NULL;
}

struct tbt_fec_flow *tbt_fec_flow_lookup(struct tbt_session *sess, u64 sid)
{
    struct tbt_fec_flow *f;

    if (!sess || !sess->fec || !sid)
        return NULL;
    spin_lock(&sess->lock);
    f = tbt_fec_flow_find_locked(sess, sid);
    if (f && !refcount_inc_not_zero(&f->refs))
        f = NULL;
    spin_unlock(&sess->lock);
    return f;
}

static void tbt_fec_tx_plane_free(struct tbt_fec_tx *tx)
{
    u16 i;

    if (!tx)
        return;
    if (tx->g)
    {
        for (i = 0; i < tx->g_cap; i++)
            kvfree(tx->g[i].buf);
        kvfree(tx->g);
    }
    tbt_fec_replay_free(tx->replay);
    tbt_fec_grouped_free(tx->codec);
    tbt_fec_grouped_free(tx->old_codec);
    kfree(tx->adapt);
    kfree(tx->adapt_sclc);
    kvfree(tx->ring);
    kvfree(tx->pkt);
    kvfree(tx->rec);
    kvfree(tx);
}

static void tbt_fec_rx_plane_free(struct tbt_fec_rx *rx)
{
    u32 total;

    if (!rx)
        return;
    total = (u32)rx->groups * rx->slots;
    tbt_fec_rx_blk_free(rx->blk, total);
    if (rx->alloc_old_blk)
        tbt_fec_rx_blk_free(rx->alloc_old_blk,
                            (u32)rx->alloc_old_groups * rx->slots);
    kvfree(rx->alloc_old_req_ns);
    kvfree(rx->alloc_old_req_block);
    tbt_fec_grouped_free(rx->alloc_old_codec);
    kvfree(rx->rec);
    kvfree(rx->zero);
    kvfree(rx->nack);
    kvfree(rx->req_ns);
    kvfree(rx->req_block);
    tbt_fec_grouped_free(rx->codec);
    tbt_fec_grouped_free(rx->old_codec);
    tbt_fec_grouped_free(rx->pending_codec);
    kfree(rx->est);
    kfree(rx->est_sclc);
    kvfree(rx);
}

static void tbt_fec_flow_free_work(struct work_struct *w)
{
    struct tbt_fec_flow *f = container_of(w, struct tbt_fec_flow, free_work);

    /* The primary compatibility pointers are RCU-published and read by
     * tbt_tx.c/tbt_carrier.c without a flow reference. Wait one grace period
     * so no reader can still hold a pointer into these planes. */
    synchronize_rcu();
    tbt_fec_tx_plane_free(f->tx);
    tbt_fec_rx_plane_free(f->rx);
    kfree(f);
}

void tbt_fec_flow_put(struct tbt_fec_flow *f)
{
    if (!f)
        return;
    if (refcount_dec_and_test(&f->refs))
    {
        /* The zero transition happens once, so the queue cannot already hold
         * this work. If the module is past tbt_fec_data_module_exit(), the
         * missing WQ is a lifecycle bug; leaking one flow is safer than
         * queueing into freed module state. */
        if (WARN_ON_ONCE(!tbt_fec_free_wq))
            return;
        WARN_ON_ONCE(!queue_work(tbt_fec_free_wq, &f->free_work));
    }
}

static struct tbt_fec_tx *
tbt_fec_tx_plane_new(struct tbt_session *sess, struct tbt_fec_flow *flow,
                     u16 groups, u64 sid, const u8 *mask, u64 nonce)
{
    const struct tbt_fec_sclc_shape *sclc =
        sess->fec_sclc ? &sess->fec_sclc_shape : NULL;
    struct tbt_fec_tx *tx;
    u16 n = sess->fec_n, m = sess->fec_m;
    u16 shares = (u16)(n + m);
    u32 i;

    tx = kzalloc(sizeof(*tx), GFP_KERNEL);
    if (!tx)
        return NULL;
    tx->sid = sid;
    tx->flow = flow;
    tx->nonce = nonce;
    memcpy(tx->group_mask, mask, TBT_FEC_BITMAP_BYTES);
    if (tbt_fec_group_map_build(mask, sess->fec_groups_total, tx->group_map) !=
        groups)
        goto fail;
    spin_lock_init(&tx->lock);
    spin_lock_init(&tx->replay_lock);
    spin_lock_init(&tx->shape_lock);
    spin_lock_init(&tx->alloc_lock);
    tbt_fec_alloc_sm_init(&tx->alloc);
    tx->alloc_role = 0;
    tx->base_pkt = 0;
    tx->base_block = 0;
    tx->switch_state = TBT_FEC_SW_STEADY;
    tx->adapt = kzalloc(sizeof(*tx->adapt), GFP_KERNEL);
    if (tx->adapt)
        tbt_fec_adapt_init(tx->adapt, sess->adapt.class);
    if (sess->fec_sclc)
    {
        u8 idx, sc_mask;

        if (tbt_fec_adapt_sclc_init_plan(
                n, m, groups, sess->fec_sclc_shape.variant, &idx,
                &sc_mask) == 0)
        {
            tx->adapt_sclc = kzalloc(sizeof(*tx->adapt_sclc), GFP_KERNEL);
            if (tx->adapt_sclc &&
                tbt_fec_adapt_sclc_ctl_init(
                    tx->adapt_sclc, READ_ONCE(sess->adaptive),
                    tbt_fec_adapt_sclc_rate_at(idx)->pct, shares, groups))
            {
                kfree(tx->adapt_sclc);
                tx->adapt_sclc = NULL;
            }
            if (tx->adapt_sclc)
            {
                /* INIT uses generation 1; the first switch must be 2. */
                tbt_fec_adapt_sclc_ctl_set_epoch(tx->adapt_sclc, 1);
                tbt_fec_adapt_sclc_ctl_set_rate_mask(tx->adapt_sclc,
                                                     sc_mask);
                TBT_DIAG("fec sclc adapt tunnel=%llu sid=%llu rate=%u mask=0x%02x shares=%u groups=%u",
                         (unsigned long long)sess->tunnel->cfg.id,
                         (unsigned long long)sid,
                         tbt_fec_adapt_sclc_rate_at(idx)->pct, sc_mask,
                         shares, groups);
            }
        }
        else if (READ_ONCE(sess->adaptive))
        {
            pr_warn_ratelimited("tbtun: tunnel %llu: fec rejected: adaptive mode=3 needs a supported ladder profile (n=%u m=%u groups=%u)\n",
                                (unsigned long long)sess->tunnel->cfg.id,
                                n, m, groups);
            goto fail;
        }
    }
    /* tx->replay is allocated lazily by tbt_fec_tx_replay_ensure() when the
     * packetizer builds the first DATA packet. A receive-only session never
     * pays the ~14 MiB repair window. */
    tx->groups = groups;
    tx->n = n;
    tx->m = m;
    tx->s = TBT_FEC_S;
    tx->shape_w = sess->fec_w;
    tx->shape_k = sess->fec_k;
    tx->shape_d = sess->fec_d;
    if (sclc)
    {
        /* The v8 CFG extension names the codec and its graph. The rate is
         * derived from the announced split; cfg_unpack recomputes n from it
         * with the same floor. */
        tx->codec_variant = 2;
        tx->sclc_L = sclc->L;
        tx->sclc_block = sclc->block;
        tx->rate_ppm = (u32)((((u64)n * 1000000u) + shares - 1) / shares);
    }
    else
    {
        tx->codec_variant = groups > 1 ? 1 : 0;
    }
    tx->credit_cap = tbt_fec_credit_packets(groups, n, sess->fec_replay_k,
                                            sess->fec_slots,
                                            TBT_FEC_CREDIT_MAX);
    tx->credit = tx->credit_cap;
    {
        u64 want = (u64)tx->credit * TBT_FEC_S;

        if (want > TBT_FEC_TX_RING_MAX)
            want = TBT_FEC_TX_RING_MAX;
        if (want < TBT_FEC_TX_RING)
            want = TBT_FEC_TX_RING;
        tx->ring_size = (u32)want;
    }
    tx->ring = tbt_fec_vzalloc(tx->ring_size);
    tx->pkt = tbt_fec_vzalloc(TBT_FEC_S);
    tx->rec = tbt_fec_vzalloc(TBT_FEC_REC_MAX);
    /* The per-group structs live for the flow's lifetime: a rebalance may
     * grow the flow to the session's whole pool, and a late NACK can still
     * name an old local group index. */
    tx->g_cap = sess->fec_groups_total;
    tx->g = tbt_fec_vzalloc((size_t)tx->g_cap * sizeof(*tx->g));
    tx->codec = tbt_fec_codec_new(sclc, groups, n, m, n, sess->fec_w,
                                  sess->fec_d, nonce);
    if (tx->codec)
        tx->band = tbt_fec_grouped_band(tx->codec, 0);
    if (!tx->ring || !tx->pkt || !tx->rec || !tx->g || !tx->codec ||
        !tx->adapt ||
        (sess->fec_sclc &&
         tbt_fec_adapt_sclc_rate_index_for_shape(n, m) != 0xff &&
         !tx->adapt_sclc))
        goto fail;
    for (i = 0; i < groups; i++)
    {
        tx->g[i].buf = tbt_fec_vzalloc((size_t)shares * TBT_FEC_S);
        if (!tx->g[i].buf)
            goto fail;
        tx->g[i].blk = tx->g[i].buf;
        tx->g[i].par = tx->g[i].buf + (size_t)n * TBT_FEC_S;
        tx->g[i].blk_id = ~0ULL;
    }
    return tx;

fail:
    tbt_fec_tx_plane_free(tx);
    return NULL;
}

static struct tbt_fec_rx *
tbt_fec_rx_plane_new(struct tbt_session *sess, struct tbt_fec_flow *flow,
                     u16 groups, u64 sid, const u8 *mask)
{
    const struct tbt_fec_sclc_shape *sclc =
        sess->fec_sclc ? &sess->fec_sclc_shape : NULL;
    struct tbt_fec_rx *rx;
    u16 n = sess->fec_n, m = sess->fec_m;
    u16 shares = (u16)(n + m);
    u32 total = (u32)groups * sess->fec_slots;

    rx = kzalloc(sizeof(*rx), GFP_KERNEL);
    if (!rx)
        return NULL;
    rx->sid = sid;
    rx->stream_sid = sid;
    rx->flow = flow;
    /* The stream's first geometry is frozen by its first DATA/PARITY
     * record, after an INIT may have replaced the local shape. A group
     * rebalance changes the packet mapping but never moves the stream's
     * first packet, so that origin stays valid for the whole flow. */
    rx->alloc_epoch = 0;
    rx->alloc_state = 0;
    memcpy(rx->group_mask, mask, TBT_FEC_BITMAP_BYTES);
    {
        u16 map_count = tbt_fec_group_map_build(
            mask, sess->fec_sclc ? TBT_FEC_MAX_GROUPS
                                 : sess->fec_groups_total,
            rx->group_map);

        /* SC-LDPC may start with the local TX group count while the peer
         * bitmap already names the RX groups; CFG INIT installs the matching
         * RX geometry later. */
        if (!map_count || (!sess->fec_sclc && map_count != groups))
            goto fail;
    }
    spin_lock_init(&rx->lock);
    rx->n = n;
    rx->m = m;
    rx->s = TBT_FEC_S;
    rx->groups = groups;
    rx->slots = sess->fec_slots;
    rx->base_pkt = 0;
    rx->base_block = 0;
    rx->epoch = 0;
    rx->switch_state = TBT_FEC_SW_STEADY;
    rx->shape.n = n;
    rx->shape.m = m;
    rx->shape.groups = groups;
    rx->shape.slots = sess->fec_slots;
    rx->shape.w = sess->fec_w;
    rx->shape.k = sess->fec_k;
    rx->shape.d = sess->fec_d;
    if (sess->fec_sclc)
    {
        rx->shape.codec_variant = 2;
        rx->shape.L = sess->fec_sclc_shape.L;
        rx->shape.block = sess->fec_sclc_shape.block;
        rx->shape.rate_ppm =
            (u32)((((u64)n * 1000000u) + (n + m) - 1) / (n + m));
    }
    else
    {
        rx->shape.codec_variant = groups > 1 ? 1 : 0;
    }
    rx->prev_shape = rx->shape;
    rx->replay_k = sess->fec_replay_k;
    rx->est = kzalloc(sizeof(*rx->est), GFP_KERNEL);
    if (rx->est)
        tbt_fec_est_init(rx->est, shares);
    if (sess->fec_sclc)
    {
        rx->est_sclc = kzalloc(sizeof(*rx->est_sclc), GFP_KERNEL);
        if (rx->est_sclc)
            tbt_fec_adapt_sclc_est_init(rx->est_sclc, shares);
    }
    rx->nack = tbt_fec_vzalloc(sizeof(*rx->nack));
    rx->blk = tbt_fec_rx_blk_alloc(total, shares, n);
    rx->req_ns = tbt_fec_vzalloc((size_t)total * shares * sizeof(*rx->req_ns));
    rx->req_block = tbt_fec_vzalloc((size_t)total * sizeof(*rx->req_block));
    rx->nack_cadence_ms = TBT_FEC_NACK_RETRY_MS;
    {
        u16 rec_slots = n > m ? n : m;

        if (rec_slots < TBT_FEC_ADAPT_CLASSES)
            rec_slots = TBT_FEC_ADAPT_CLASSES;
        if (sess->fec_sclc)
        {
            u8 idx, sc_mask;

            if (tbt_fec_adapt_sclc_init_plan(
                    n, m, groups, sess->fec_sclc_shape.variant, &idx,
                    &sc_mask) == 0)
            {
                u16 sc_slots = tbt_fec_adapt_sclc_max_side(shares,
                                                           sc_mask);

                if (sc_slots > rec_slots)
                    rec_slots = sc_slots;
            }
        }
        rx->rec = tbt_fec_vzalloc((size_t)rec_slots * TBT_FEC_S);
    }
    rx->zero = tbt_fec_vzalloc(TBT_FEC_S);
    rx->codec = tbt_fec_codec_new(sclc, groups, n, m, n, sess->fec_w,
                                  sess->fec_d, tbt_fec_base_nonce(sess));
    if (!rx->blk || !rx->req_ns || !rx->req_block || !rx->codec ||
        !rx->rec || !rx->zero || !rx->est || !rx->nack ||
        (sess->fec_sclc && !rx->est_sclc))
        goto fail;
    return rx;

fail:
    tbt_fec_rx_plane_free(rx);
    return NULL;
}

static u16 tbt_fec_free_groups_locked(struct tbt_session *sess, u16 want,
                                      u8 mask[TBT_FEC_BITMAP_BYTES])
{
    struct tbt_fec_flow *f;
    u16 p, free_bits = 0, take;

    memset(mask, 0, TBT_FEC_BITMAP_BYTES);
    for (p = 0; p < sess->fec_groups_total; p++)
    {
        bool used = false;
        u16 b;

        for (b = 0; b < TBT_STREAM_HT && !used; b++)
            hlist_for_each_entry(f, &sess->fec_flows[b], node)
            {
                if (tbt_fec_bitmap_test(f->group_mask, p))
                {
                    used = true;
                    break;
                }
            }
        if (!used)
            free_bits++;
    }
    take = want && want < free_bits ? want : free_bits;
    if (!take)
        return 0;
    for (p = 0; p < sess->fec_groups_total && take; p++)
    {
        bool used = false;
        u16 b;

        for (b = 0; b < TBT_STREAM_HT && !used; b++)
            hlist_for_each_entry(f, &sess->fec_flows[b], node)
            {
                if (tbt_fec_bitmap_test(f->group_mask, p))
                {
                    used = true;
                    break;
                }
            }
        if (!used)
        {
            tbt_fec_bitmap_set(mask, p);
            take--;
        }
    }
    return (u16)(tbt_fec_bitmap_count(mask));
}

u16 tbt_fec_flow_mask_alloc(struct tbt_session *sess, u16 want,
                            u8 mask[TBT_FEC_BITMAP_BYTES])
{
    u16 got;

    if (!sess || !sess->fec || !mask)
        return 0;
    mutex_lock(&sess->fec_alloc_lock);
    spin_lock(&sess->lock);
    got = tbt_fec_free_groups_locked(sess, want, mask);
    spin_unlock(&sess->lock);
    mutex_unlock(&sess->fec_alloc_lock);
    return got;
}

bool tbt_fec_flow_can_admit(struct tbt_session *sess)
{
    bool can;

    if (!sess || !sess->fec)
        return false;
    spin_lock(&sess->lock);
    can = sess->fec_groups_used < sess->fec_groups_total;
    spin_unlock(&sess->lock);
    return can;
}

/* Allocate and publish a flow. The caller holds fec_alloc_lock, which
 * serialises admission and detach; the spinlock is taken only for the table
 * check/reserve and for the final publish. The plane allocations sleep and
 * must never run under sess->lock. */
static struct tbt_fec_flow *
tbt_fec_flow_create(struct tbt_session *sess, u64 sid,
                    const u8 mask[TBT_FEC_BITMAP_BYTES], u16 count,
                    const u8 *rx_mask)
{
    struct tbt_fec_flow *f, *it;
    u16 p, b;
    u64 nonce;
    int err = -ENOMEM;

    spin_lock(&sess->lock);
    err = tbt_fec_flow_create_code(
        sid && tbt_fec_flow_find_locked(sess, sid) != NULL,
        sess->fec_sclc, sess->n_fec_flows, sess->fec_groups_total, count);
    if (err)
    {
        spin_unlock(&sess->lock);
        return ERR_PTR(err);
    }
    if (!tbt_fec_groups_fit(sess->fec_groups_total,
                            sess->fec_groups_used, count))
    {
        spin_unlock(&sess->lock);
        return ERR_PTR(-ENOSPC);
    }
    for (b = 0; b < TBT_STREAM_HT; b++)
        hlist_for_each_entry(it, &sess->fec_flows[b], node)
            for (p = 0; p < sess->fec_groups_total; p++)
                if (tbt_fec_bitmap_test(mask, p) &&
                    tbt_fec_bitmap_test(it->group_mask, p))
                {
                    spin_unlock(&sess->lock);
                    return ERR_PTR(-EPROTO);
                }
    /* Reserve the groups before the sleeping allocation. fec_alloc_lock
     * prevents another admission from stealing them; a failure below puts
     * the reservation back. */
    sess->fec_groups_used += count;
    spin_unlock(&sess->lock);

    f = kzalloc(sizeof(*f), GFP_KERNEL);
    if (!f)
        goto fail_unreserve;
    f->sess = sess;
    f->sid = sid;
    f->groups = count;
    f->state = 1;
    memcpy(f->group_mask, mask, TBT_FEC_BITMAP_BYTES);
    if (tbt_fec_group_map_build(mask, sess->fec_groups_total, f->group_map) !=
        count)
    {
        kfree(f);
        err = -EPROTO;
        goto fail_unreserve;
    }
    nonce = tbt_fec_base_nonce(sess);
    f->tx = tbt_fec_tx_plane_new(sess, f, count, sid, mask, nonce);
    f->rx = tbt_fec_rx_plane_new(sess, f, count, sid,
                                 rx_mask ? rx_mask : mask);
    if (!f->tx || !f->rx)
    {
        tbt_fec_tx_plane_free(f->tx);
        tbt_fec_rx_plane_free(f->rx);
        kfree(f);
        goto fail_unreserve;
    }
    refcount_set(&f->refs, 2); /* one table reference, one caller reference */
    INIT_WORK(&f->free_work, tbt_fec_flow_free_work);

    spin_lock(&sess->lock);
    hlist_add_head(&f->node, &sess->fec_flows[TBT_STREAM_BUCKET(sid)]);
    sess->n_fec_flows++;
    if (!sess->fec_primary)
    {
        tbt_fec_primary_publish(sess, f);
    }
    spin_unlock(&sess->lock);
    return f;

fail_unreserve:
    spin_lock(&sess->lock);
    sess->fec_groups_used -= count;
    spin_unlock(&sess->lock);
    return ERR_PTR(err);
}

/* Bind the unbound primary flow to the first stream. Its TX plane keeps the
 * local shape; for SC-LDPC the OPEN bitmap names the peer's TX groups, which
 * are this side's RX groups, so the RX plane's mask moves to that bitmap. The
 * RX plane is reallocated later by FEC_CFG INIT, which also checks that the
 * INIT group count matches this bitmap. The caller holds fec_alloc_lock and
 * the spinlock. */
static struct tbt_fec_flow *
tbt_fec_flow_bind_primary_locked(struct tbt_session *sess,
                                 struct tbt_fec_flow *f, u64 sid,
                                 const u8 mask[TBT_FEC_BITMAP_BYTES])
{
    hlist_del_init(&f->node);
    f->sid = sid;
    if (f->tx)
        f->tx->sid = sid;
    if (f->rx)
    {
        f->rx->sid = sid;
        f->rx->stream_sid = sid;
    }
    if (sess->fec_sclc && f->rx)
    {
        /* The flow mask is this side's TX partition (used by OPEN announce);
         * the peer bitmap belongs to the RX plane. The TX plane keeps its
         * local shape, and CFG INIT later installs the RX geometry. */
        memcpy(f->rx->group_mask, mask, TBT_FEC_BITMAP_BYTES);
        tbt_fec_group_map_build(mask, TBT_FEC_MAX_GROUPS, f->rx->group_map);
    }
    hlist_add_head(&f->node, &sess->fec_flows[TBT_STREAM_BUCKET(sid)]);
    refcount_inc(&f->refs); /* caller reference */
    return f;
}

struct tbt_fec_flow *tbt_fec_flow_admit_mask(struct tbt_session *sess, u64 sid,
                                             const u8 mask[TBT_FEC_BITMAP_BYTES])
{
    struct tbt_fec_flow *f;
    u16 count;
    bool bound = false;

    if (!sess || !sess->fec || !sid)
        return ERR_PTR(-EINVAL);
    if (!tbt_fec_open_mask_valid(sess->fec_sclc, sess->fec_groups_total,
                                 mask, &count))
        return ERR_PTR(-EPROTO);
    mutex_lock(&sess->fec_alloc_lock);
    spin_lock(&sess->lock);
    f = sess->fec_primary;
    if (f && !f->sid && sess->n_fec_flows == 1)
    {
        if (!tbt_fec_primary_mask_match(sess->fec_sclc, f->group_mask, mask))
            f = ERR_PTR(-EPROTO);
        else
        {
            f = tbt_fec_flow_bind_primary_locked(sess, f, sid, mask);
            bound = true;
        }
    }
    spin_unlock(&sess->lock);
    if (bound || (f && IS_ERR(f)))
    {
        mutex_unlock(&sess->fec_alloc_lock);
        return f;
    }
    if (sess->fec_sclc)
    {
        u8 local[TBT_FEC_BITMAP_BYTES];
        u16 local_count = sess->fec_groups_total;
        u16 g;

        memset(local, 0, sizeof(local));
        for (g = 0; g < local_count && g < TBT_FEC_MAX_GROUPS; g++)
            tbt_fec_bitmap_set(local, g);
        f = tbt_fec_flow_create(sess, sid, local, local_count, mask);
    }
    else
    {
        f = tbt_fec_flow_create(sess, sid, mask, count, NULL);
    }
    mutex_unlock(&sess->fec_alloc_lock);
    return f;
}

/* Fair group count for a new flow whose sid sorts at `pos`, and that position
 * itself. Only bound live flows take part: the unbound primary has no stream
 * and is either the flow this admission binds or not part of the split. */
static u16 tbt_fec_alloc_share_for_sid(struct tbt_session *sess, u64 sid,
                                       u16 *pos)
{
    struct tbt_fec_flow *f;
    u16 b, n = 0, idx = 0;

    spin_lock(&sess->lock);
    for (b = 0; b < TBT_STREAM_HT; b++)
        hlist_for_each_entry(f, &sess->fec_flows[b], node)
        {
            if (!f->sid || !tbt_fec_flow_live(READ_ONCE(f->state)))
                continue;
            n++;
            if (f->sid < sid)
                idx++;
        }
    spin_unlock(&sess->lock);
    *pos = idx;
    return tbt_fec_alloc_count(sess->fec_groups_total, (u16)(n + 1), idx);
}

/* Wait until no allocation round is running. Growth after a detach starts
 * from w_fec_tx; it must finish before the new split is computed, or the
 * admission would hand out groups the survivors are about to claim. */
static bool tbt_fec_alloc_wait_idle(struct tbt_session *sess, u64 deadline)
{
    while (ktime_get_ns() < deadline)
    {
        bool busy, admit;

        spin_lock(&sess->lock);
        admit = sess->fec_alloc_admit;
        busy = sess->fec_alloc_round || (!admit && sess->fec_alloc_grow);
        spin_unlock(&sess->lock);
        if (!busy)
            return true;
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
        msleep(1);
    }
    return false;
}

/* Release the admission's claim on the free pool. A grow request that arrived
 * while the admission held it now runs and includes the new flow. */
static void tbt_fec_alloc_admit_end(struct tbt_session *sess)
{
    bool grow;

    spin_lock(&sess->lock);
    sess->fec_alloc_admit = false;
    sess->fec_alloc_req = false;
    grow = sess->fec_alloc_grow;
    spin_unlock(&sess->lock);
    if (grow)
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
}

/* Ask w_fec_tx for one round that includes this sid, then wait for its
 * reserved mask. The caller holds fec_rebalance_lock, so there is at most one
 * waiter; async growth cannot start a second round meanwhile. */
static bool tbt_fec_alloc_request(struct tbt_session *sess, u64 sid,
                                  u64 deadline, u8 mask[TBT_FEC_BITMAP_BYTES],
                                  u16 *count)
{
    spin_lock(&sess->lock);
    if (sess->fec_alloc_round || sess->fec_alloc_req)
    {
        spin_unlock(&sess->lock);
        return false;
    }
    sess->fec_alloc_req = true;
    sess->fec_alloc_pending_sid = sid;
    sess->fec_alloc_pending_valid = false;
    sess->fec_alloc_failed = false;
    spin_unlock(&sess->lock);
    tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    while (ktime_get_ns() < deadline)
    {
        bool ready, failed;

        spin_lock(&sess->lock);
        ready = sess->fec_alloc_pending_valid;
        failed = sess->fec_alloc_failed && !sess->fec_alloc_round;
        if (ready)
        {
            memcpy(mask, sess->fec_alloc_pending.mask,
                   TBT_FEC_BITMAP_BYTES);
            *count = sess->fec_alloc_pending.groups;
            sess->fec_alloc_pending_valid = false;
        }
        spin_unlock(&sess->lock);
        if (ready)
            return true;
        if (failed)
            return false;
        msleep(1);
    }
    spin_lock(&sess->lock);
    if (!sess->fec_alloc_round)
        sess->fec_alloc_req = false;
    spin_unlock(&sess->lock);
    return false;
}

/* Near-side admission with the dynamic split. The first stream binds the
 * pre-created primary; later streams take their fair share from the free
 * pool or wait for the donors to shrink. */
static struct tbt_fec_flow *tbt_fec_flow_admit_dynamic(struct tbt_session *sess,
                                                       u64 sid)
{
    struct tbt_fec_flow *f;
    u8 mask[TBT_FEC_BITMAP_BYTES];
    u16 target, pos = 0, got = 0;
    u64 deadline = ktime_get_ns() +
                   (u64)TBT_FEC_ALLOC_ADMIT_WAIT_MS *
                       TBT_FEC_NSEC_PER_MSEC;

    mutex_lock(&sess->fec_rebalance_lock);
    /* The primary flow is created by data_init without a sid. The first
     * stream adopts it; no round is needed and its full mask stays. */
    mutex_lock(&sess->fec_alloc_lock);
    spin_lock(&sess->lock);
    f = sess->fec_primary;
    if (f && !f->sid && sess->n_fec_flows == 1)
    {
        memset(mask, 0, sizeof(mask));
        f = tbt_fec_flow_bind_primary_locked(sess, f, sid, mask);
        spin_unlock(&sess->lock);
        mutex_unlock(&sess->fec_alloc_lock);
        mutex_unlock(&sess->fec_rebalance_lock);
        return f;
    }
    spin_unlock(&sess->lock);
    mutex_unlock(&sess->fec_alloc_lock);

    spin_lock(&sess->lock);
    sess->fec_alloc_admit = true;
    sess->fec_alloc_pending_sid = sid;
    spin_unlock(&sess->lock);
    if (!tbt_fec_alloc_wait_idle(sess, deadline))
        goto fail;
    target = tbt_fec_alloc_share_for_sid(sess, sid, &pos);
    if (!target)
        goto fail;
    {
        u16 free;

        spin_lock(&sess->lock);
        free = sess->fec_groups_total > sess->fec_groups_used
                   ? (u16)(sess->fec_groups_total - sess->fec_groups_used)
                   : 0;
        spin_unlock(&sess->lock);
        if (free >= target)
        {
            mutex_lock(&sess->fec_alloc_lock);
            spin_lock(&sess->lock);
            got = tbt_fec_free_groups_locked(sess, target, mask);
            spin_unlock(&sess->lock);
            if (got)
                f = tbt_fec_flow_create(sess, sid, mask, got, NULL);
            else
                f = ERR_PTR(-ENOSPC);
            mutex_unlock(&sess->fec_alloc_lock);
            if (!IS_ERR(f))
            {
                tbt_fec_alloc_admit_end(sess);
                mutex_unlock(&sess->fec_rebalance_lock);
                return f;
            }
            if (PTR_ERR(f) != -ENOSPC)
            {
                tbt_fec_alloc_admit_end(sess);
                mutex_unlock(&sess->fec_rebalance_lock);
                return f;
            }
        }
    }
    /* No room without shrinking a donor: one round reserves this flow's
     * mask and moves every affected flow. */
    if (!tbt_fec_alloc_request(sess, sid, deadline, mask, &got))
        goto fail;
    mutex_lock(&sess->fec_alloc_lock);
    f = tbt_fec_flow_create(sess, sid, mask, got, NULL);
    mutex_unlock(&sess->fec_alloc_lock);
    tbt_fec_alloc_admit_end(sess);
    mutex_unlock(&sess->fec_rebalance_lock);
    return f;

fail:
    tbt_fec_alloc_admit_end(sess);
    mutex_unlock(&sess->fec_rebalance_lock);
    return ERR_PTR(-ENOSPC);
}

struct tbt_fec_flow *tbt_fec_flow_admit(struct tbt_session *sess, u64 sid,
                                        u16 want)
{
    struct tbt_fec_flow *f;
    u8 mask[TBT_FEC_BITMAP_BYTES];
    u16 got;

    if (!sess || !sess->fec || !sid)
        return ERR_PTR(-EINVAL);
    /* Default quota: the near side is the allocator and splits the pool
     * dynamically. A configured fec_groups_per_flow keeps the static quota
     * semantics for A/B tests and for a peer that does not speak ALLOC. */
    if (!want && !sess->fec_sclc && sess->fec_allocator)
        return tbt_fec_flow_admit_dynamic(sess, sid);
    /* A quota above the session's geometry means "all groups"; without this
     * clamp the primary's bind rejects the first stream as a mismatch. */
    if (want > sess->fec_groups_total)
        want = sess->fec_groups_total;
    mutex_lock(&sess->fec_alloc_lock);
    spin_lock(&sess->lock);
    f = sess->fec_primary;
    if (f && !f->sid && sess->n_fec_flows == 1)
    {
        if (want && want != f->groups)
            f = ERR_PTR(-EPROTO);
        else
            f = tbt_fec_flow_bind_primary_locked(sess, f, sid, mask);
        spin_unlock(&sess->lock);
        mutex_unlock(&sess->fec_alloc_lock);
        return f;
    }
    got = tbt_fec_free_groups_locked(sess, want, mask);
    if (!got)
    {
        spin_unlock(&sess->lock);
        mutex_unlock(&sess->fec_alloc_lock);
        return ERR_PTR(-ENOSPC);
    }
    spin_unlock(&sess->lock);
    f = tbt_fec_flow_create(sess, sid, mask, got, NULL);
    mutex_unlock(&sess->fec_alloc_lock);
    return f;
}

/* Drop every record this flow still has queued. The owner pointer in each
 * bound record charges tx->queued, so the records must go before the planes. */
static void tbt_fec_flow_purge(struct tbt_session *sess,
                               struct tbt_fec_flow *f)
{
    struct tbt_fec_out *o, *tmp;
    u16 i;

    spin_lock(&sess->fec_out_lock);
    list_for_each_entry_safe(o, tmp, &sess->fec_out_ctl, node)
        if (o->flow == f)
            tbt_fec_out_free(sess, NULL, o);
    list_for_each_entry_safe(o, tmp, &sess->fec_out_replay, node)
        if (o->flow == f)
            tbt_fec_out_free(sess, NULL, o);
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_fec_outq *q = &sess->fec_outq[i];

        list_for_each_entry_safe(o, tmp, &q->queue, node)
            if (o->flow == f)
                tbt_fec_out_free(sess, q, o);
    }
    spin_unlock(&sess->fec_out_lock);
    /* The flow is already unlinked and its group count returned. Let the TX
     * work grow the survivors into the freed slice; during teardown there is
     * nothing to grow. */
    if (sess->fec_allocator && !sess->fec_sclc &&
        !READ_ONCE(sess->closing) && !READ_ONCE(sess->fec_stopping))
    {
        spin_lock(&sess->lock);
        sess->fec_alloc_grow = true;
        spin_unlock(&sess->lock);
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    }
}

/* Undo an admission whose stream could not be created. `f` is the caller's
 * admission reference; the table holds another. This function consumes both,
 * so a failed stream start cannot leave the flow unlinked but alive. */
void tbt_fec_flow_abort(struct tbt_session *sess, struct tbt_fec_flow *f)
{
    struct tbt_fec_flow *next;

    if (!sess || !sess->fec || !f)
        return;
    mutex_lock(&sess->fec_alloc_lock);
    spin_lock(&sess->lock);
    /* Publish the death before unlinking: a snapshot or a send that already
     * has a reference must see a dead flow and stop producing work for it. */
    WRITE_ONCE(f->state, 3);
    if (!hlist_unhashed(&f->node))
        hlist_del_init(&f->node);
    if (sess->n_fec_flows)
        sess->n_fec_flows--;
    if (sess->fec_groups_used >= f->groups)
        sess->fec_groups_used -= f->groups;
    if (sess->fec_primary == f)
    {
        u16 b;

        tbt_fec_primary_publish(sess, NULL);
        for (b = 0; b < TBT_STREAM_HT && !sess->fec_primary; b++)
            hlist_for_each_entry(next, &sess->fec_flows[b], node)
            {
                tbt_fec_primary_publish(sess, next);
                break;
            }
    }
    spin_unlock(&sess->lock);
    mutex_unlock(&sess->fec_alloc_lock);
    if (f->rx)
    {
        spin_lock_bh(&f->rx->lock);
        WRITE_ONCE(f->rx->stopping, true);
        spin_unlock_bh(&f->rx->lock);
    }
    /* Purge after the flow is unreachable: a racing producer now refuses at
     * tbt_fec_out_push(), and an in-flight flush owns a record reference. */
    tbt_fec_flow_purge(sess, f);
    tbt_fec_flow_put(f); /* table reference */
    tbt_fec_flow_put(f); /* caller's admission reference */
}

void tbt_fec_flow_attach(struct tbt_stream *st, struct tbt_fec_flow *f)
{
    if (!st || !f)
        return;
    st->fec_flow = f; /* consumes the caller's reference */
}

void tbt_fec_flow_detach(struct tbt_stream *st)
{
    struct tbt_session *sess;
    struct tbt_fec_flow *f, *next;

    if (!st || !st->fec_flow)
        return;
    f = st->fec_flow;
    st->fec_flow = NULL;
    sess = f->sess;
    mutex_lock(&sess->fec_alloc_lock);
    spin_lock(&sess->lock);
    WRITE_ONCE(f->state, 3);
    if (!hlist_unhashed(&f->node))
        hlist_del_init(&f->node);
    if (sess->n_fec_flows)
        sess->n_fec_flows--;
    if (sess->fec_groups_used >= f->groups)
        sess->fec_groups_used -= f->groups;
    if (sess->fec_primary == f)
    {
        u16 b;

        tbt_fec_primary_publish(sess, NULL);
        for (b = 0; b < TBT_STREAM_HT && !sess->fec_primary; b++)
            hlist_for_each_entry(next, &sess->fec_flows[b], node)
            {
                tbt_fec_primary_publish(sess, next);
                break;
            }
    }
    spin_unlock(&sess->lock);
    mutex_unlock(&sess->fec_alloc_lock);
    if (f->rx)
    {
        spin_lock_bh(&f->rx->lock);
        WRITE_ONCE(f->rx->stopping, true);
        spin_unlock_bh(&f->rx->lock);
    }
    tbt_fec_flow_purge(sess, f);
    tbt_fec_flow_put(f); /* table reference */
    tbt_fec_flow_put(f); /* stream reference */
}

int tbt_fec_data_init(struct tbt_session *sess)
{
    struct tbt_tunnel *t = sess->tunnel;
    struct tbt_fec_sclc_shape sclc;
    struct tbt_fec_flow *f;
    bool is_sclc = t->cfg.mode == 3;
    u16 n = t->cfg.n, m = t->cfg.m, shares, groups, w, d;
    u32 per, slots, total, i;
    u8 mask[TBT_FEC_BITMAP_BYTES];
    u16 primary_groups;
    u8 sclc_idx, sclc_mask;

    /* Every record the parsers accept must fit the buffer that assembles it.
     * Without these, raising TBT_FEC_PAYLOAD_MAX would make a full record
     * impossible to complete and the parser would stick at p->have ==
     * sizeof(p->buf) with no error path. */
    BUILD_BUG_ON(TBT_PARSER_BUF < TBT_FEC_REC_MAX);
    BUILD_BUG_ON(TBT_FEC_REC_MAX != TBT_FEC_HDR_LEN + TBT_FEC_PAYLOAD_MAX);
    BUILD_BUG_ON(TBT_PARSER_BUF < TBT_REC_MAX);
    BUILD_BUG_ON(TBT_CAR_PART_MAX < TBT_FEC_REC_MAX);
    BUILD_BUG_ON(sizeof(((struct tbt_http_rx *)0)->rec) != TBT_FEC_REC_MAX);
    /* The estimator fold uses TBT_FEC_ADAPT_GROUPS entries on the stack; the
     * SC-LDPC codec's own group limit must not outgrow that scratch. */
    BUILD_BUG_ON(TBT_FEC_ADAPT_GROUPS < TBT_SCLC_MAX_GROUPS);

    if (!n || !m || (u16)n + (u16)m > TBT_FEC_MAX_SHARES)
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec rejected: n=%u m=%u exceeds %u shares\n",
                            (unsigned long long)t->cfg.id, n, m,
                            TBT_FEC_MAX_SHARES);
        return -EINVAL;
    }
    if (is_sclc && tbt_fec_sclc_shape_of(n, m, t->cfg.carriers, t->cfg.w,
                                         t->cfg.L, &sclc))
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec rejected: not an SC-LDPC shape n=%u m=%u carriers=%u w=%u L=%u\n",
                            (unsigned long long)t->cfg.id, n, m,
                            t->cfg.carriers, t->cfg.w, t->cfg.L);
        return -EINVAL;
    }
    shares = (u16)(n + m);
    if (!t->cfg.carriers || t->cfg.carriers % shares)
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec rejected: carriers=%u is not a multiple of n+m=%u\n",
                            (unsigned long long)t->cfg.id, t->cfg.carriers,
                            shares);
        return -EINVAL;
    }
    groups = (u16)(t->cfg.carriers / shares);
    if (!groups || groups > TBT_FEC_MAX_GROUPS)
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec rejected: groups=%u outside 1..%u\n",
                            (unsigned long long)t->cfg.id, groups,
                            TBT_FEC_MAX_GROUPS);
        return -EINVAL;
    }
    if (is_sclc)
    {
        if (sclc.variant == TBT_FEC_SCLC_DOWN)
        {
            w = sclc.block;
            d = 0;
        }
        else
        {
            w = sclc.L;
            d = sclc.rate_256;
        }
        /* The controller starts at the configured ladder point and may move
         * only to the points this codec can build. Both the binary downlink
         * and the GF(16) uplink build every ladder split. */
        if (tbt_fec_adapt_sclc_init_plan(n, m, groups, sclc.variant,
                                         &sclc_idx, &sclc_mask) &&
            READ_ONCE(sess->adaptive))
        {
            pr_warn_ratelimited("tbtun: tunnel %llu: fec rejected: adaptive mode=3 needs an n/m split on the rate ladder (n=%u m=%u)\n",
                                (unsigned long long)t->cfg.id, n, m);
            return -EINVAL;
        }
    }
    else
    {
        w = n;
        if (groups > 1)
        {
            w = t->cfg.w ? t->cfg.w : (u16)(2 * n);
            if (w < n)
                w = n;
        }
        d = (u16)(w / 2);
        if (!d)
            d = 1;
        if (w > 1 && d >= w)
            d = (u16)(w - 1);
    }

    /* One super-block across every group is groups*(n+m)*S bytes. Keep the
     * whole RX arena at or below 16 MiB: 32x8 gets 8 slots, one group keeps
     * the historical 256. The slots value is per session: every flow's arena
     * is G_i*slots blocks, so sum(G_i)*slots is the single-stream total. */
    per = (u32)groups * shares * TBT_FEC_S;
    slots = TBT_FEC_RX_BUDGET / per;
    if (slots < 4)
        slots = 4;
    if (slots > 256)
        slots = 256;
    total = (u32)groups * slots;
    TBT_DIAG("fec init start tunnel=%llu n=%u m=%u carriers=%u mode=%u groups=%u w=%u d=%u slots=%u total_blk=%u arena_kb=%u",
             (unsigned long long)t->cfg.id, n, m, t->cfg.carriers,
             t->cfg.mode, groups, w, d, slots, total,
             (u32)(((size_t)total * shares * TBT_FEC_S) >> 10));

    /* Session scratch: the carrier queues and the drain works belong to the
     * session, not to one flow. */
    sess->fec_outq = tbt_fec_vzalloc((size_t)t->cfg.carriers *
                                     sizeof(*sess->fec_outq));
    if (!sess->fec_outq)
        goto fail;
    for (i = 0; i < t->cfg.carriers; i++)
        INIT_LIST_HEAD(&sess->fec_outq[i].queue);

    INIT_DELAYED_WORK(&sess->fec_drain_work, tbt_fec_out_drain_work);
    bitmap_zero(sess->fec_drain_ready, TBT_MAX_CARRIERS_LIMIT);
    bitmap_zero(sess->fec_drain_retry, TBT_MAX_CARRIERS_LIMIT);
    sess->fec_retry_at = 0;
    sess->fec_drain_cursor = 0;
    /* Publish the gate last: tbt_fec_out_kick() refuses while ndrain is 0,
     * so it cannot arm an uninitialised work during a partial init. */
    sess->fec_ndrain = sess->n_car;

    for (i = 0; i < TBT_STREAM_HT; i++)
        INIT_HLIST_HEAD(&sess->fec_flows[i]);
    mutex_init(&sess->fec_alloc_lock);
    spin_lock_init(&sess->fec_fbk_lock);
    sess->fec_fbk_busy = false;
    sess->fec_fbk_pending = 0;
    mutex_init(&sess->fec_rebalance_lock);
    sess->fec_allocator = t->cfg.role == TBT_ROLE_NEAR;
    sess->fec_alloc_round = false;
    sess->fec_alloc_req = false;
    sess->fec_alloc_failed = false;
    sess->fec_alloc_grow = false;
    sess->fec_alloc_epoch = 0;
    sess->fec_alloc_flows = 0;
    sess->fec_alloc_finished = 0;
    sess->fec_alloc_has_pending = false;
    sess->fec_alloc_pending_valid = false;
    sess->n_fec_flows = 0;
    sess->fec_groups_total = groups;
    sess->fec_groups_used = 0;
    sess->fec_slots = (u16)slots;
    sess->fec_replay_k = tbt_fec_replay_k_for(groups, n, m, TBT_FEC_S);
    sess->fec_n = n;
    sess->fec_m = m;
    sess->fec_w = w;
    sess->fec_k = is_sclc ? (u16)(sclc.variant == TBT_FEC_SCLC_DOWN
                                      ? sclc.rate_ppm
                                      : sclc.block)
                          : n;
    sess->fec_d = (u8)d;
    sess->fec_sclc = is_sclc;
    if (is_sclc)
        sess->fec_sclc_shape = sclc;
    sess->fec_rr = 0;
    sess->fec_ctl_rot = 0;
    sess->fec_stopping = false;
    atomic_set(&sess->fec_tx_run, 0);
    atomic_set(&sess->fec_rx_run, 0);
    tbt_fec_primary_publish(sess, NULL);

    INIT_LIST_HEAD(&sess->fec_out_ctl);
    INIT_LIST_HEAD(&sess->fec_out_replay);
    spin_lock_init(&sess->fec_out_lock);
    INIT_DELAYED_WORK(&sess->w_fec_tx, tbt_fec_tx_work);
    INIT_DELAYED_WORK(&sess->w_fec_rx, tbt_fec_rx_work);
    INIT_DELAYED_WORK(&sess->w_fec_tick, tbt_fec_tick_work);
    INIT_DELAYED_WORK(&sess->w_fec_rx_poll, tbt_fec_rx_poll_work);
    sess->fec = true;

    /* The first flow owns one fair share from the start. fec_groups_per_flow
     * = 0 keeps the old single-stream geometry; 16 on 256 carriers leaves 16
     * groups for a second flow. A far-side OPEN later replaces this mask. */
    primary_groups = groups;
    /* SC-LDPC keeps the whole carrier geometry in its single flow: a partial
     * group set would make the CFG groups field disagree with the session's
     * carriers/(n+m) and the rate switch would be refused. */
    if (!is_sclc && tbt_fec_groups_per_flow &&
        tbt_fec_groups_per_flow < primary_groups)
        primary_groups = (u16)tbt_fec_groups_per_flow;
    if (!primary_groups)
        primary_groups = 1;
    memset(mask, 0, sizeof(mask));
    for (i = 0; i < primary_groups; i++)
        tbt_fec_bitmap_set(mask, (u16)i);

    mutex_lock(&sess->fec_alloc_lock);
    f = tbt_fec_flow_create(sess, 0, mask, primary_groups, NULL);
    mutex_unlock(&sess->fec_alloc_lock);
    if (IS_ERR(f))
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec primary flow failed: %ld\n",
                            (unsigned long long)t->cfg.id, PTR_ERR(f));
        goto fail;
    }
    /* The table keeps one reference; the primary pointer is a borrowed view
     * and is cleared by tbt_fec_flow_detach(). */
    tbt_fec_flow_put(f);

    tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    TBT_DIAG("fec init done tunnel=%llu groups=%u slots=%u replay_k=%u primary_groups=%u mode=%u",
             (unsigned long long)t->cfg.id, groups, slots,
             sess->fec_replay_k, primary_groups, t->cfg.mode);
    return 0;

fail:
    pr_warn_ratelimited("tbtun: tunnel %llu: fec init failed\n",
                        (unsigned long long)t->cfg.id);
    tbt_fec_data_free(sess);
    return -ENOMEM;
}
