/* tbt_fec_data.c - FEC v6 data plane: packetizer, grouped block codec,
 * record writer, receiver reassembly.
 *
 * Scope (T2):
 *   - carriers = groups * (n + m), one share per carrier;
 *   - carrier c carries group c/(n+m) share c%(n+m);
 *   - global packet seq is striped: group = seq % groups, block_id =
 *     (seq / groups) / n, share = (seq / groups) % n;
 *   - a block is n DATA packets followed by m parity packets; the codec is the
 *     host-tested GF(2) RLNC in tbt_fec.c, reused with a per-group, per-block
 *     nonce;
 *   - both containers: raw records, or one 40-byte record per HTTP chunk.
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
 * global range [b*groups*n, (b+1)*groups*n). A super-block is handed to the
 * stream only when every group's block b is decoded, so the byte stream has no
 * hole and the ACK frontier is a real global frontier, not a per-group guess.
 */
#include <linux/ktime.h>
#include <linux/random.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <net/tcp.h>

#include "tbtun.h"
#include "tbt_fec.h"
#include "tbt_rec.h"

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
/* The RX arena keeps at most one window of 1024 packets, and the ACK grants
 * exactly that: slots * groups * n reaches 1024 for every supported shape
 * (the 16 MiB arena divided by (n + m) * S is 256 super-blocks). */
#define TBT_FEC_RX_BUDGET (16u * 1024 * 1024)
#define TBT_FEC_CREDIT_MAX 1024u

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
 * can carry a negotiated seed later without changing the 40-byte header. */
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
        return tbt_stream_get(sess, sid, false, false);
    spin_lock(&sess->lock);
    for (i = 0; i < TBT_STREAM_HT; i++)
    {
        hlist_for_each_entry(st, &sess->streams[i], node)
        {
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

/* Unlink and free one record. `q` is its carrier queue, or NULL for a control
 * or replay record. The caller holds fec_out_lock. */
static void tbt_fec_out_free(struct tbt_session *sess, struct tbt_fec_outq *q,
                             struct tbt_fec_out *o)
{
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
        q->depth--;
        q->bytes -= o->len;
        if (sess->fec_outq_queued)
            sess->fec_outq_queued--;
    }
    if (sess->fec_out_queued)
        sess->fec_out_queued--;
    kfree(o);
}

static int tbt_fec_out_push(struct tbt_session *sess, u16 carrier, const u8 *buf,
                            u16 len, bool control, bool replay, u64 stamp_ns)
{
    struct tbt_fec_out *o;
    struct tbt_fec_outq *q = NULL;

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
    o->carrier = carrier;
    o->len = len;
    o->control = control ? 1 : 0;
    o->replay = replay ? 1 : 0;
    o->stamp_ns = stamp_ns;
    memcpy(o->data, buf, len);
    spin_lock(&sess->fec_out_lock);
    if (replay &&
        sess->fec_replay_bytes + len > TBT_FEC_REPLAY_BYTES_MAX)
    {
        spin_unlock(&sess->fec_out_lock);
        kfree(o);
        return -ENOSPC;
    }
    /* A full carrier queue drops this record only: the replay ring already
     * holds the share, so the receiver's NACK repairs it. Blocking the
     * packetizer here would let one congested carrier stall every other
     * carrier, which is the defect this queue split exists to remove. */
    if (q && (q->depth >= TBT_FEC_OUTQ_DEPTH_MAX ||
              q->bytes + len > TBT_FEC_OUTQ_BYTES_MAX))
    {
        spin_unlock(&sess->fec_out_lock);
        kfree(o);
        atomic64_inc(&sess->fec_outq_drop);
        return -ENOSPC;
    }
    if (replay)
    {
        sess->fec_replay_queued++;
        sess->fec_replay_bytes += len;
    }
    if (control)
    {
        list_add(&o->node, &sess->fec_out_ctl);
        sess->fec_out_ctl_depth++;
        sess->fec_out_ctl_bytes += len;
    }
    else if (replay)
    {
        /* Repairs are drained ahead of new shares, FIFO across NACKs. */
        list_add_tail(&o->node, &sess->fec_out_replay);
    }
    else
    {
        list_add_tail(&o->node, &q->queue);
        q->depth++;
        q->bytes += len;
        sess->fec_outq_queued++;
        if (q->depth > sess->fec_outq_depth_max)
            sess->fec_outq_depth_max = q->depth;
    }
    sess->fec_out_queued++;
    spin_unlock(&sess->fec_out_lock);
    tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    return 0;
}

/* No usable carrier: ask the session's replacement path for one and keep the
 * record queued. The 2 ms flush arm retries; a source share is never dropped
 * here. The first wait in an episode is counted once, and the source ring is
 * the backpressure of last resort. */
static void tbt_fec_wait_carrier(struct tbt_session *sess)
{
    struct tbt_fec_tx *tx = sess->fec_tx;

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
    struct tbt_fec_tx *tx = sess->fec_tx;
    u16 start, i;
    int rc = -ENODEV;

    start = ncand ? tbt_fec_replay_pick(sess, usable, pace) : U16_MAX;
    if (start == U16_MAX)
    {
        tbt_fec_wait_carrier(sess);
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
        tbt_fec_wait_carrier(sess);
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
    if (o->replay)
        return tbt_fec_out_try_replay(sess, o, sent, usable, ncand, pace);
    if (o->control)
    {
        /* Control records (ACK and NACK) may ride any up carrier. Start after
         * the carrier that last accepted one and try every up carrier:
         * pinning the queue to the lowest up carrier lets one congested
         * socket hold every ACK behind its data, and a late ACK is a stalled
         * credit window. */
        struct tbt_fec_tx *tx = sess->fec_tx;
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
            tbt_fec_wait_carrier(sess);
            rc = -EAGAIN;
        }
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
        struct tbt_fec_tx *tx = sess->fec_tx;
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
    /* The share's own carrier is gone or not usable yet. Send it on a live one
     * instead of dropping it: the receiver places a share by its header, and a
     * dropped share is a block the codec has to recover -- which it cannot
     * always do (measured: 3 of 4 shares plus all 4 parity rows stayed
     * undecodable, and the whole stream stopped behind that block). A carrier
     * that has sent HELLO can carry bytes even before this stream's OPEN is
     * written on it, because the OPEN precedes any DATA on that carrier only
     * for frames the pump pins to it. */
    rc = -ENODEV;

    for (i = 0; i < sess->n_car; i++)
    {
        int err;

        if (i == o->carrier)
            continue;
        c = tbt_carrier_get(sess, (u8)i);
        if (!c)
            continue;
        if (!READ_ONCE(c->hello_sent) || atomic_read(&c->stopping) ||
            READ_ONCE(c->stalled) || !atomic_read(&c->connected))
        {
            tbt_carrier_put(c);
            continue;
        }
        err = tbt_carrier_send_fec(c, o->data, o->len);
        tbt_carrier_put(c);
        if (err == -ENOTCONN || err == -ENODEV)
            continue;
        if (sess->fec_tx && (err == 0 || err == -EAGAIN))
        {
            sess->fec_tx->carrier_wait = false;
            sess->fec_tx->backpressure_counted = false;
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
        tbt_fec_wait_carrier(sess);
        return -EAGAIN;
    }
    return rc;
}

/* Write every queue that can make progress. The session pump lock serialises
 * these writes with the old control path (HELLO/OPEN/FIN/PAD), so a record is
 * never spliced into another record's bytes on one carrier.
 *
 * Three independent passes run in priority order: control (ACK/NACK, any up
 * carrier), replay (any up carrier, paced by queue bytes), then the bound
 * data queues. A congested data queue therefore never holds an ACK, a NACK,
 * or a repair behind it. Within the data pass a carrier whose socket returns
 * -EAGAIN keeps its head and the sweep moves to the next carrier: only an
 * empty pass means the pipeline itself is backpressured. */
static int tbt_fec_out_flush(struct tbt_session *sess, u32 *written)
{
    int ret = 0;
    u32 pass = 0;
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
            spin_unlock(&sess->fec_out_lock);
            if (!o)
                break;
            err = tbt_fec_out_try(sess, o, &sent, NULL, 0, NULL);
            if (err == -EAGAIN)
            {
                ret = -EAGAIN;
                break;
            }
            spin_lock(&sess->fec_out_lock);
            tbt_fec_out_free(sess, NULL, o);
            spin_unlock(&sess->fec_out_lock);
            if (sent)
                pass++;
        }
    }
    /* Pass 2: replay. The usable set is built once; every write still
     * rechecks its carrier, so a socket that dies mid-pass is skipped. */
    if (READ_ONCE(sess->fec_replay_queued))
    {
        u8 usable[TBT_MAX_CARRIERS_LIMIT];
        u32 pace[TBT_MAX_CARRIERS_LIMIT];
        u16 ncand = tbt_fec_replay_usable(sess, usable);

        spin_lock(&sess->fec_out_lock);
        for (i = 0; i < sess->n_car; i++)
            pace[i] = sess->fec_outq[i].bytes;
        spin_unlock(&sess->fec_out_lock);
        for (;;)
        {
            struct tbt_fec_out *o;
            u32 depth = 0;
            bool sent = false;
            u64 stamp_ns = 0;
            int err;

            spin_lock(&sess->fec_out_lock);
            o = list_first_entry_or_null(&sess->fec_out_replay,
                                         struct tbt_fec_out, node);
            if (o)
                depth = sess->fec_replay_queued;
            stamp_ns = o ? o->stamp_ns : 0;
            spin_unlock(&sess->fec_out_lock);
            if (!o)
                break;
            err = tbt_fec_out_try(sess, o, &sent, usable, ncand, pace);
            if (err == -EAGAIN)
            {
                /* No carrier has room for this repair now. It keeps its
                 * place; the bound data queues still drain below. */
                atomic64_inc(&sess->fec_replay_skip);
                atomic64_add(depth, &sess->fec_hol_wait);
                ret = -EAGAIN;
                break;
            }
            spin_lock(&sess->fec_out_lock);
            tbt_fec_out_free(sess, NULL, o);
            spin_unlock(&sess->fec_out_lock);
            if (sent)
            {
                /* Charge NACK->wire, not NACK->enqueue: the repair is
                 * complete only when a carrier accepted the record. */
                u64 now = ktime_get_ns();
                u64 dt = now > stamp_ns ? now - stamp_ns : 0;

                atomic64_add(dt, &sess->fec_nack_lat_ns);
                tbt_fec_stat_max(&sess->fec_nack_lat_max_ns, dt);
                pass++;
            }
            else
            {
                atomic64_inc(&sess->fec_replay_drop);
            }
        }
    }
    /* Pass 3: bound data queues, one carrier at a time. */
    if (READ_ONCE(sess->fec_outq_queued))
    {
        for (i = 0; i < sess->n_car; i++)
        {
            struct tbt_fec_outq *q = &sess->fec_outq[i];

            for (;;)
            {
                struct tbt_fec_out *o;
                u32 depth = 0;
                bool sent = false;
                int err;

                spin_lock(&sess->fec_out_lock);
                o = list_first_entry_or_null(&q->queue, struct tbt_fec_out,
                                             node);
                if (o)
                    depth = q->depth;
                spin_unlock(&sess->fec_out_lock);
                if (!o)
                    break;
                err = tbt_fec_out_try(sess, o, &sent, NULL, 0, NULL);
                if (err == -EAGAIN)
                {
                    /* This carrier's head cannot be written now. Its records
                     * wait, but every later carrier still gets its turn. */
                    atomic64_inc(&sess->fec_flush_skip);
                    atomic64_add(depth, &sess->fec_hol_wait);
                    ret = -EAGAIN;
                    break;
                }
                spin_lock(&sess->fec_out_lock);
                tbt_fec_out_free(sess, q, o);
                spin_unlock(&sess->fec_out_lock);
                if (sent)
                    pass++;
            }
        }
    }
    mutex_unlock(&sess->pump_lock);
    if (pass)
        atomic64_add(pass, &sess->fec_flush_pass);
    if (written)
        *written = pass;
    return ret;
}

/* Queue one complete record. Control records may ride any up carrier; data
 * records are bound to their share's carrier. Returns 0 when queued. */
static int tbt_fec_send_record(struct tbt_session *sess, u16 carrier, u8 *buf,
                               u16 len, bool control, bool replay)
{
    return tbt_fec_out_push(sess, carrier, buf, len, control, replay, 0);
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
                                  u16 len, u64 nack_ns)
{
    int err = tbt_fec_out_push(sess, 0, buf, len, false, true, nack_ns);

    if (!err)
        WRITE_ONCE(sess->fec_repair_last_ns, ktime_get_ns());
    return err;
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
     * PARITY are transformed; the 40-byte header stays clear. The TX work is
     * serialized and only runs once the TX context is ready. */
    if (sess->crypto &&
        (h->type == TBT_FEC_DATA || h->type == TBT_FEC_PARITY))
    {
        if (!sess->crypto_ctx.tx.ready)
            return -EINVAL;
        if (tbt_crypto_body_xor_share(&sess->crypto_ctx.tx, h->seq,
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

/* Encode and send the m parity shares of one group block. win_len is the
 * number of real DATA packets in the block: n for a full block, less for the
 * stream's final block. The unused positions are already zero-filled. */
static void tbt_fec_tx_emit_parity(struct tbt_session *sess, u16 group,
                                   u64 block_id, u16 win_len)
{
    struct tbt_fec_tx *tx = sess->fec_tx;
    struct tbt_fec_gtx *g = &tx->g[group];
    struct tbt_fec_rlnc *ctx = tbt_fec_grouped_ctx(tx->codec, group);
    struct tbt_fec_hdr h;
    u64 last_seq;
    u16 rec_len, i;

    if (!ctx || !win_len || win_len > tx->n)
        return;
    tbt_fec_rlnc_set_nonce(ctx, tbt_fec_group_nonce(tx->nonce, group, block_id));
    for (i = 0; i < tx->n; i++)
        tx->data_ptrs[i] = g->blk + (size_t)i * TBT_FEC_S;
    for (i = 0; i < tx->m; i++)
        tx->parity_ptrs[i] = g->par + (size_t)i * TBT_FEC_S;
    if (tbt_fec_rlnc_encode(ctx, tx->data_ptrs, tx->n, tx->parity_ptrs) < 0)
        return;
    last_seq = tbt_fec_seq_of_block_pkt(block_id, group, (u16)(win_len - 1),
                                        tx->groups, tx->n);
    for (i = 0; i < tx->m; i++)
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
        if (tbt_fec_make_record(sess, &h, tx->parity_ptrs[i], tx->rec, &rec_len))
            continue;
        WRITE_ONCE(tx->last_group, group);
        WRITE_ONCE(tx->last_share, (u16)(tx->n + i));
        if (tx->replay)
        {
            spin_lock_bh(&tx->replay_lock);
            tbt_fec_replay_store(tx->replay, block_id, group,
                                 (u16)(tx->n + i), tx->rec, rec_len);
            spin_unlock_bh(&tx->replay_lock);
        }
        tbt_fec_send_record(sess,
                            (u16)(group * (tx->n + tx->m) + tx->n + i),
                            tx->rec, rec_len, false, false);
    }
}

/* One PARITY record that says "this group has no packet in this block".
 * Without it the receiver waits forever for a block that was never sent: a
 * group with no real share emits no parity, and the super-block walk requires
 * every group's block to be ready. The marker carries no payload, so crypto
 * has nothing to transform and the record is header-only. */
static void tbt_fec_tx_emit_empty(struct tbt_session *sess, u16 group,
                                  u64 block_id)
{
    struct tbt_fec_tx *tx = sess->fec_tx;
    struct tbt_fec_hdr h;
    u16 rec_len;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_PARITY;
    h.share_index = tx->n; /* parity 0 */
    h.n = (u8)tx->n;
    h.m = (u8)tx->m;
    h.block_id = block_id;
    h.seq = tbt_fec_seq_of_block_pkt(block_id, group, 0, tx->groups, tx->n);
    h.win_len = 0;
    h.payload_len = 0;
    h.epoch = 0;
    h.conv = TBT_FEC_CONV;
    if (tbt_fec_make_record(sess, &h, NULL, tx->rec, &rec_len))
        return;
    WRITE_ONCE(tx->last_group, group);
    WRITE_ONCE(tx->last_share, tx->n);
    if (tx->replay)
    {
        spin_lock_bh(&tx->replay_lock);
        tbt_fec_replay_store(tx->replay, block_id, group, tx->n,
                             tx->rec, rec_len);
        spin_unlock_bh(&tx->replay_lock);
    }
    tbt_fec_send_record(sess,
                        (u16)(group * (tx->n + tx->m) + tx->n),
                        tx->rec, rec_len, false, false);
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
static void tbt_fec_tx_close_block(struct tbt_session *sess, bool advance)
{
    struct tbt_fec_tx *tx = sess->fec_tx;
    u64 block_id = tx->cur_block;
    u16 group;

    for (group = 0; group < tx->groups; group++)
    {
        struct tbt_fec_gtx *g = &tx->g[group];
        u16 len = g->nreal;

        if (g->blk_id != block_id)
        {
            tbt_fec_tx_emit_empty(sess, group, block_id);
            continue;
        }
        if (!len || len >= tx->n)
            continue; /* full: parity already sent */
        memset(g->blk + (size_t)len * TBT_FEC_S, 0,
               (size_t)(tx->n - len) * TBT_FEC_S);
        tbt_fec_tx_emit_parity(sess, group, block_id, len);
        g->nreal = 0;
    }
    if (advance)
    {
        tx->seq = (block_id + 1) * (u64)tx->n * tx->groups;
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
static void tbt_fec_tx_finish(struct tbt_session *sess, struct tbt_stream *st)
{
    struct tbt_fec_tx *tx = sess->fec_tx;
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
    block_start = tx->cur_block * (u64)tx->n * tx->groups;
    if (st->fec_pkt_base_set && tx->seq > block_start)
        tbt_fec_tx_close_block(sess, true);
    smp_store_release(&st->fec_finished, true);
}

static void tbt_fec_packetize(struct tbt_session *sess, struct tbt_stream *st,
                              bool last)
{
    struct tbt_fec_tx *tx = sess->fec_tx;
    struct tbt_fec_hdr h;
    u16 rec_len, group, share, len, carrier;
    u64 seq, local, block_id;
    u8 *rec = tx->rec;

    if (!tx->fill && !last)
        return;
    len = tx->fill;
    seq = tx->seq++;
    group = (u16)(seq % tx->groups);
    local = seq / tx->groups;
    share = (u16)(local % tx->n);
    block_id = local / tx->n;
    carrier = (u16)(group * (tx->n + tx->m) + share);

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
        (void)tbt_fec_send_record(sess, carrier, rec, rec_len, false, false);
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
        tbt_fec_tx_emit_parity(sess, group, block_id, tx->g[group].nreal);
        tx->g[group].nreal = 0;
    }
    if (last)
        tbt_fec_tx_finish(sess, st);
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

static bool tbt_fec_tx_is_idle(struct tbt_session *sess)
{
    struct tbt_fec_tx *tx = sess->fec_tx;
    bool idle;

    spin_lock(&tx->lock);
    idle = tx->head == tx->tail && tx->fill == 0 && !READ_ONCE(tx->flush_req);
    spin_unlock(&tx->lock);
    if (!idle)
        return false;
    spin_lock(&sess->fec_out_lock);
    idle = !sess->fec_out_queued;
    spin_unlock(&sess->fec_out_lock);
    return idle;
}

static void tbt_fec_tx_work_body(struct tbt_session *sess)
{
    struct tbt_fec_tx *tx = sess->fec_tx;
    struct tbt_stream *st;
    bool paused = false;
    bool short_ready;

    if (!tx || READ_ONCE(sess->closing) || READ_ONCE(tx->stopping))
        return;
    {
        u32 wrote = 0;

        /* Drain first, but do not let one backpressured carrier stop the
         * packetizer: continue while any carrier in the pass accepted a
         * record. Only a pass that wrote nothing at all means every carrier
         * is blocked, and then a 2 ms retry is the correct response. */
        if (tbt_fec_out_flush(sess, &wrote) == -EAGAIN && !wrote)
        {
            tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
            return;
        }
    }

    /* crypto=on: the far side cannot packetize until the near's HELLO has
     * delivered the session key. The ring keeps the bytes; retry shortly. */
    if (sess->crypto && !smp_load_acquire(&sess->crypto_tx_ready))
    {
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(10));
        return;
    }

    st = tbt_fec_stream(sess);
    if (!st)
        return;
    for (;;)
    {
        u32 got;

        spin_lock(&tx->lock);
        got = tbt_fec_ring_get(tx, tx->pkt + tx->fill, TBT_FEC_S - tx->fill);
        if (got)
            tx->fill += (u16)got;
        paused = tx->head == tx->tail && READ_ONCE(st->tx_paused);
        spin_unlock(&tx->lock);
        if (got)
        {
            if (tx->fill == TBT_FEC_S && tbt_fec_tx_credit_ok(tx))
                tbt_fec_packetize(sess, st, false);
            continue;
        }
        if (tx->fill == TBT_FEC_S && tbt_fec_tx_credit_ok(tx))
        {
            tbt_fec_packetize(sess, st, false);
            continue;
        }
        if (tx->fill == TBT_FEC_S)
        {
            atomic64_inc(&sess->fec_credit_wait);
            break; /* the peer's credit is exhausted: wait for its ACK */
        }
        if (READ_ONCE(tx->flush_req) && tx->head == tx->tail)
        {
            /* A partial tail still consumes one packet of the peer's window.
             * Emitting it on an exhausted window would place it in a block
             * the receiver has already dropped as too far ahead, and the
             * stream could never end. Wait for the ACK that reopens the
             * window; the flush request survives. */
            if (tx->fill && !tbt_fec_tx_credit_ok(tx))
                break;
            if (tx->fill)
                tbt_fec_packetize(sess, st, true);
            else
                tbt_fec_tx_finish(sess, st);
            WRITE_ONCE(tx->flush_req, false);
            continue;
        }
        break;
    }
    spin_lock(&tx->lock);
    short_ready = tx->fill && tx->head == tx->tail;
    spin_unlock(&tx->lock);
    if (short_ready && !READ_ONCE(tx->flush_req))
    {
        /* The ring is dry and the packet is short: emit it once the
         * application has been quiet for TBT_FEC_SHORT_MS. The share carries
         * its real payload_len and its block slot stays zero-padded, so parity
         * still describes the truth, and the receiver places the next share
         * right after it (the delivery offset is the sum of the payload
         * lengths, not the packet index times S).
         *
         * Closing the block is always safe now: every group that has no
         * packet in it sends an empty-block marker, so the receiver's
         * super-block walk never waits for a share that was never written.
         * The old `flushable` scan could never tell "full and already
         * emitted" from "empty", so a short write after a completed block
         * waited forever. */
        u64 now = ktime_get_ns();

        if (!tx->short_at_ns)
        {
            tx->short_at_ns = now + (u64)TBT_FEC_SHORT_MS * NSEC_PER_MSEC;
            tbt_session_arm(sess, TBT_SW_FEC_TX,
                            msecs_to_jiffies(TBT_FEC_SHORT_MS));
        }
        else if (now >= tx->short_at_ns && tbt_fec_tx_credit_ok(tx))
        {
            tbt_fec_packetize(sess, st, false);
            tbt_fec_tx_close_block(sess, true);
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
    if (tbt_fec_out_flush(sess, NULL) == -EAGAIN)
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
}

void tbt_fec_tx_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_tx);
    struct tbt_fec_tx *tx = sess->fec_tx;
    bool owned = test_and_clear_bit(TBT_SW_FEC_TX, &sess->sched);

    if (!tx)
        goto out;
    if (atomic_fetch_or(1u << TBT_FEC_RUN_BIT, &tx->run) &
        (1u << TBT_FEC_RUN_BIT))
    {
        /* Another instance owns the packetizer buffers. Record the wake; the
         * owner re-arms once when it finishes. Never run the body here. */
        atomic_fetch_or(1u << TBT_FEC_AGAIN_BIT, &tx->run);
        goto out;
    }
    tbt_fec_tx_work_body(sess);
    atomic_fetch_and(~(1u << TBT_FEC_RUN_BIT), &tx->run);
    if (atomic_fetch_and(~(1u << TBT_FEC_AGAIN_BIT), &tx->run) &
        (1u << TBT_FEC_AGAIN_BIT))
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
out:
    if (owned)
        tbt_session_put(sess);
}

size_t tbt_fec_tx_submit(struct tbt_stream *st, struct sk_buff *skb,
                         unsigned int off, size_t len)
{
    struct tbt_session *sess = st->sess;
    struct tbt_fec_tx *tx = sess->fec_tx;
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
    return tbt_fec_tx_is_idle(st->sess);
}

int tbt_fec_tx_flush(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;
    struct tbt_fec_tx *tx = sess->fec_tx;

    if (!tx)
        return -ENODEV;
    if (READ_ONCE(tx->stopping))
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

bool tbt_fec_record_type(u8 type)
{
    return type >= TBT_FEC_DATA && type <= TBT_FEC_FBK;
}

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

static void tbt_fec_blk_reset(struct tbt_fec_blk *b, u64 block_id)
{
    b->used = true;
    b->ready = false;
    b->nreal = 0;
    b->ndata = 0;
    b->nparity = 0;
    b->count_real = 0xffff;
    b->decoding = false;
    b->empty = false;
    b->win_len_set = false;
    b->short_len = 0;
    b->short_unknown = false;
    b->block_id = block_id;
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
    return tbt_fec_blk_real_len_from_tail(b->nreal, rx->n, rx->total_known,
                                          rx->total_pkts, rx->groups,
                                          b->block_id, group, has_data);
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
static int tbt_fec_blk_decode(struct tbt_session *sess, u16 group,
                              struct tbt_fec_blk *b)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    struct tbt_fec_rlnc *ctx = tbt_fec_grouped_ctx(rx->codec, group);
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
    if (!ctx || have_parity < missing)
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

    tbt_fec_rlnc_set_nonce(ctx, tbt_fec_group_nonce(tbt_fec_base_nonce(sess),
                                                    group, b->block_id));
    atomic64_inc(&sess->fec_decode_blocks);
    rc = tbt_fec_rlnc_decode(ctx, rx->data_ptrs, rx->parity_ptrs,
                             rx->dpresent_snap, rx->ppresent_snap, rx->n);
    if (rc == TBT_FEC_RLNC_ERR_NOMEM)
        atomic64_inc(&sess->fec_decode_nomem);
    if (rc > 0)
        atomic64_add(rc, &sess->fec_decode_recovered);
    return rc < 0 ? -EAGAIN : 0;
}

/* next_deliver is the packet frontier the sender's credit window uses.
 * deliver_bytes is the stream-relative byte count this receiver has actually
 * handed over; it is the only value the sender may use as tx_ack, because a
 * packet frontier can name bytes that no delivered share carried. */
static void tbt_fec_ack(struct tbt_session *sess, u64 next_deliver,
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
    /* The receiver buffers `slots` super-blocks; the credit must not exceed
     * that window, or the sender would overrun it and its shares would be
     * dropped as "too far ahead". It also must not exceed K-2 super-blocks:
     * the replay ring holds K, so the oldest block the receiver can still
     * NACK is never evicted. The sender applies the same cap. */
    {
        struct tbt_fec_rx *rx = sess->fec_rx;
        u32 blocks = rx->replay_k > 2 ? (u32)(rx->replay_k - 2) : 1;

        a.credit_packets = (u32)min_t(u64,
                                      (u64)rx->slots * rx->groups * rx->n,
                                      (u64)blocks * rx->groups * rx->n);
    }
    a.credit_packets = min(a.credit_packets, TBT_FEC_CREDIT_MAX);
    if (!a.credit_packets)
        a.credit_packets = 1;
    if (tbt_fec_ack_pack(&a, body))
        return;
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_ACK;
    h.seq = next_deliver;
    h.payload_len = TBT_FEC_ACK_LEN;
    h.conv = TBT_FEC_CONV;
    if (tbt_fec_make_record(sess, &h, body, rec, &len))
        return;
    if (tbt_fec_send_record(sess, 0, rec, len, true, false))
        return;
    atomic64_inc(&sess->fec_ack_tx);
}

/* Ask the peer to replay the shares missing from one super-block. The NACK
 * is a control record: it may ride any up, non-stalled carrier and it is
 * queued at the head, ahead of new shares. */
static int tbt_fec_send_nack(struct tbt_session *sess, u64 block_id,
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
    /* tbt_fec_nack_pack() returns the positive body length on success. Only
     * a negative value is an error; treating 64 as an error kept every NACK
     * off the wire (fec_nack_tx=0, fec_nack_drop climbing). */
    if (tbt_fec_nack_pack(n, body) != TBT_FEC_NACK_LEN)
        return -EINVAL;
    if (tbt_fec_make_record(sess, &h, body, rec, &len))
        return -EINVAL;
    {
        int err = tbt_fec_send_record(sess, 0, rec, len, true, false);

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
static int tbt_fec_send_empty_marker(struct tbt_session *sess, u16 group,
                                     u64 block_id)
{
    struct tbt_fec_tx *tx = sess->fec_tx;
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
    h.seq = tbt_fec_seq_of_block_pkt(block_id, group, 0, tx->groups, tx->n);
    h.win_len = 0;
    h.payload_len = 0;
    h.epoch = 0;
    h.conv = TBT_FEC_CONV;
    if (tbt_fec_make_record(sess, &h, NULL, rec, &len))
        return -EINVAL;
    if (tx->replay)
    {
        spin_lock_bh(&tx->replay_lock);
        tbt_fec_replay_store(tx->replay, block_id, group, tx->n, rec, len);
        spin_unlock_bh(&tx->replay_lock);
    }
    return tbt_fec_send_record(sess, 0, rec, len, true, false);
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

/* The stream this receiver delivers for. A bound stream sticks: a new OPEN in
 * the same session must not steal the previous stream's tail block, so the
 * lookup returns rx->stream_sid whenever that stream still exists. */
static struct tbt_stream *tbt_fec_rx_stream(struct tbt_session *sess,
                                            bool *is_new)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    struct tbt_stream *st;
    u64 far = READ_ONCE(sess->far_sid);

    /* A new stream is already named by the session. Keep the old one only
     * while its final super-block is still undelivered; otherwise the new
     * stream's bytes would wait behind a stream object that has nothing left
     * to deliver (the second connection of an HTTP keep-alive or iperf3
     * control+data pair). */
    if (rx->stream_sid && far && far != rx->stream_sid)
    {
        bool drained = true;

        if (READ_ONCE(rx->total_known) && READ_ONCE(rx->total_pkts))
        {
            u64 final = tbt_fec_final_block_of_total(
                READ_ONCE(rx->total_pkts), rx->groups, rx->n);

            drained = rx->next_super > final;
        }
        if (!drained)
        {
            st = tbt_stream_get(sess, rx->stream_sid, false, false);
            if (st)
                return st;
        }
        st = tbt_stream_get(sess, far, false, false);
        if (st)
        {
            *is_new = true;
            return st;
        }
    }

    if (rx->stream_sid)
    {
        st = tbt_stream_get(sess, rx->stream_sid, false, false);
        if (st)
            return st;
    }
    st = tbt_fec_stream(sess);
    if (st && st->sid != rx->stream_sid)
        *is_new = true;
    return st;
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
            u64 final = tbt_fec_final_block_of_total(rx->total_pkts,
                                                     rx->groups, rx->n);

            if (s == final &&
                !tbt_fec_group_tail_len(rx->total_pkts, rx->groups, rx->n,
                                        group))
            {
                /* The final super-block may leave a group with no packet.
                 * Skip it only when no block was ever placed for it; a block
                 * that does exist must still be ready, or its bytes would be
                 * skipped while the frontier advanced. */
                if (!b->used || b->block_id != s)
                    continue;
            }
        }
        if (!b->used || !b->ready || b->block_id != s)
            return false;
    }
    return true;
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
static int tbt_fec_deliver_ready(struct tbt_session *sess)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    struct tbt_stream *st;
    bool delivered = false;
    bool is_new = false;
    bool blocked = false;

    if (READ_ONCE(rx->total_known) && !READ_ONCE(rx->total_pkts) &&
        !rx->deliver_partial)
        return 0;

    st = tbt_fec_rx_stream(sess, &is_new);
    if (!st)
        return 0;
    /* A new connection in the same session: the session's packet and byte
     * counters keep running, but the stream starts at byte 0 and its own FIN
     * names its own end. A FIN that arrived before this adoption was parked
     * in next_fin_*; consume it now, while the stream identity is settled. */
    if (is_new)
    {
        spin_lock_bh(&rx->lock);
        if (rx->stream_sid != st->sid)
        {
            rx->stream_sid = st->sid;
            rx->stream_base = rx->deliver_bytes;
            rx->stream_pkt_base = 0;
            rx->total_known = false;
            rx->total_pkts = 0;
            rx->total_bytes = 0;
            rx->deliver_partial = false;
            rx->deliver_p = 0;
            rx->deliver_g = 0;
            if (rx->next_fin_valid && rx->next_fin_sid == st->sid)
            {
                rx->stream_pkt_base = rx->next_fin_base;
                rx->total_pkts = rx->next_fin_end;
                rx->total_bytes = rx->next_fin_bytes;
                rx->total_known = true;
                rx->next_fin_valid = false;
            }
        }
        spin_unlock_bh(&rx->lock);
    }
    for (;;)
    {
        u64 s = rx->next_super;
        u64 base = s * (u64)rx->groups * rx->n;
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
            u64 final = tbt_fec_final_block_of_total(total_pkts,
                                                     rx->groups, rx->n);

            if (s > final)
                break;
        }
        spin_lock_bh(&rx->lock);
        ready = tbt_fec_super_ready_locked(rx, s);
        if (!ready && debug)
        {
            /* Name the group whose block holds the whole stream: without it
             * the frontier cannot move and the sender's credit is never
             * renewed. */
            for (group = 0; group < rx->groups; group++)
            {
                struct tbt_fec_blk *sb = tbt_fec_rx_blk(rx, group, s);
                u16 k, have = 0;

                if (sb->used && sb->ready && sb->block_id == s)
                    continue;
                for (k = 0; k < rx->n; k++)
                    have = (u16)(have + (sb->dpresent[k] ? 1 : 0));
                pr_info_ratelimited("tbtun: fec super %llu stuck: group %u/%u used=%d ready=%d id=%llu nreal=%u shares=%u/%u parity=%u\n",
                                    s, group, rx->groups, sb->used, sb->ready,
                                    sb->block_id, sb->nreal, have, rx->n,
                                    (unsigned)rx->m);
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
            tbt_session_arm(sess, TBT_SW_FEC_RX,
                            msecs_to_jiffies(left_ms));
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
                                             s))
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
                    break;
                }
                if (known && rx->total_bytes &&
                    seq + 1 == total_pkts)
                {
                    u64 done = off - rx->stream_base;
                    u64 tail = rx->total_bytes - done;

                    if (tail > 0 && tail <= rx->s)
                        len = (u16)tail;
                }
                /* Defence in depth: hdr_check() already refuses an epoch
                 * larger than S, but a delivery length that named more than
                 * one slot would read past this block's buffer. */
                if (len > rx->s)
                    len = rx->s;
                err = tbt_rx_fec_deliver(st, off - rx->stream_base,
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
                    off = rx->stream_base + frontier;
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
                                        off - rx->stream_base, frontier);
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
                break;
            }
            start_g = 0;
        }
        rx->deliver_partial = false;
        rx->deliver_p = 0;
        rx->deliver_g = 0;
        rx->next_super++;
        rx->progress_ns = 0; /* progress: the stall budget restarts */
        rx->next_deliver = (s + 1) * (u64)rx->groups * rx->n;
        if (known && rx->next_deliver > total_pkts)
            rx->next_deliver = total_pkts;
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
        tbt_fec_ack(sess, rx->next_deliver, stream_bytes);
    }
    tbt_stream_put(st);
    return 0;
}

/* Ask the peer for the shares missing from the oldest super-block that is
 * still inside the replay window. One NACK per pass, rate-limited and
 * retried without a total cap: a live carrier TCP keeps the repair going.
 * A block outside the window is left to that TCP's own retransmission; the
 * stream is reset only when every carrier TCP has failed. */
static void tbt_fec_nack_scan(struct tbt_session *sess)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    struct tbt_fec_nack n;
    u64 now = ktime_get_ns();
    u64 s, hi;
    u16 g, i;
    bool found = false;

    if (!rx || !rx->nack || READ_ONCE(rx->stopping) ||
        READ_ONCE(sess->closing))
        return;
    /* The shape must support a ring. Both ends derive the same effective K,
     * so the receiver NACKs exactly the blocks the sender can replay; the
     * sender counts fec_replay_miss_no_ring if its own allocation failed. */
    if (!rx->replay_k)
        return;

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
            u64 dt = now - rx->nack->first_ns;

            atomic64_add(dt, &sess->fec_nack_repair_ns);
            tbt_fec_stat_max(&sess->fec_nack_repair_max_ns, dt);
            tbt_fec_nack_reset(rx->nack);
        }
    }

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

    spin_lock_bh(&rx->lock);
    for (s = rx->next_super; s <= hi; s++)
    {
        bool missing = false;
        bool started = false;

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

            if (!b->used || b->block_id != s)
            {
                /* Before the stream end is known, a group with no block may
                 * be legitimately empty: a short/sparse block. Wait for the
                 * parity win_len, the empty marker or FIN to state the real
                 * share count. Requesting the whole range here produced a
                 * NACK storm the sender could not serve and left the block
                 * unready until the reset. */
                if (tbt_fec_nack_unstarted_group(READ_ONCE(rx->total_pkts),
                                                 rx->groups, rx->n, s, g,
                                                 READ_ONCE(rx->total_known)))
                {
                    /* Total is known and this group has packets in the
                     * block: a wholly missing block is a real loss. Request
                     * its range; the ring returns only what it has. */
                    missing = true;
                    tbt_fec_bitmap_set(n.group_mask, g);
                    for (i = 0; i < (u16)(rx->n + rx->m); i++)
                        tbt_fec_bitmap_set(n.share_mask, i);
                    continue;
                }
                /* Before total is known, probe only parity 0. The empty
                 * marker or the parity's win_len then states the real shape;
                 * a full-range request here was the sparse-block NACK storm. */
                if (!READ_ONCE(rx->total_known))
                {
                    missing = true;
                    tbt_fec_bitmap_set(n.group_mask, g);
                    tbt_fec_bitmap_set(n.share_mask, rx->n);
                    atomic64_inc(&sess->fec_nack_sparse_probe);
                }
                continue;
            }
            if (b->ready)
                continue;
            real = tbt_fec_blk_real_len(rx, g, b);
            if (!real)
                continue;
            if (b->nparity >= (u16)(real - b->ndata))
                continue; /* the next pass can decode it */
            missing = true;
            tbt_fec_bitmap_set(n.group_mask, g);
            for (i = 0; i < real; i++)
                if (!b->dpresent[i])
                    tbt_fec_bitmap_set(n.share_mask, i);
            for (i = 0; i < rx->m; i++)
                if (!b->ppresent[i])
                    tbt_fec_bitmap_set(n.share_mask, rx->n + i);
        }
        if (missing && tbt_fec_bitmap_count(n.share_mask))
        {
            found = true;
            break;
        }
    }
    spin_unlock_bh(&rx->lock);

    if (!found)
    {
        struct tbt_fec_nack last;
        bool started = false;
        bool blocked;
        u64 last_block;

        if (!rx->nack->valid)
            return;
        last_block = rx->nack->block_id;
        if (last_block < rx->next_super)
        {
            tbt_fec_nack_reset(rx->nack);
            return;
        }
        if (READ_ONCE(rx->total_known) &&
            rx->next_super >
                tbt_fec_final_block_of_total(rx->total_pkts, rx->groups,
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
        spin_unlock_bh(&rx->lock);
        if (!blocked)
        {
            tbt_fec_nack_reset(rx->nack);
            return;
        }
        /* The scan found no new missing set, but the blocked super-block is
         * still not ready. Resend the last NACK: a replayed share or a parity
         * reply may still be in flight, and a never-generated padding share
         * may need the parity reply the previous NACK triggered. */
        memcpy(last.group_mask, rx->nack->group_mask, TBT_FEC_BITMAP_BYTES);
        memcpy(last.share_mask, rx->nack->share_mask, TBT_FEC_BITMAP_BYTES);
        if (tbt_fec_nack_due(rx->nack, now, last_block, &last))
        {
            int err = tbt_fec_send_nack(sess, last_block, &last);

            if (debug >= 2)
                pr_info("tbtun: fec NACK retry block=%llu next=%llu group=%*phN share=%*phN\n",
                        last_block, rx->next_super, TBT_FEC_BITMAP_BYTES,
                        last.group_mask, TBT_FEC_BITMAP_BYTES,
                        last.share_mask);
            if (!err)
                atomic64_inc(&sess->fec_nack_tx);
            else
                tbt_fec_nack_drop(sess, err, last_block, &last);
        }
        tbt_session_arm(sess, TBT_SW_FEC_RX,
                        msecs_to_jiffies(TBT_FEC_NACK_MIN_MS));
        return;
    }
    if (tbt_fec_nack_due(rx->nack, now, s, &n))
    {
        int err;

        if (debug >= 2)
            pr_info("tbtun: fec NACK tx block=%llu next=%llu hi=%llu group=%*phN share=%*phN\n",
                    s, rx->next_super, hi, TBT_FEC_BITMAP_BYTES,
                    n.group_mask, TBT_FEC_BITMAP_BYTES, n.share_mask);
        err = tbt_fec_send_nack(sess, s, &n);
        if (!err)
            atomic64_inc(&sess->fec_nack_tx);
        else
            tbt_fec_nack_drop(sess, err, s, &n);
    }
    /* The NACK itself may be lost; retry while the block is in the window.
     * No total attempt cap: only the window and the carrier TCP lifetime
     * bound the repair. */
    tbt_session_arm(sess, TBT_SW_FEC_RX,
                    msecs_to_jiffies(TBT_FEC_NACK_MIN_MS));
}

/* Claim one block under rx->lock, decode it outside, commit under the lock.
 * The snapshot is what the codec sees; a share that arrives during the decode
 * is placed into b->data as usual, and the commit only writes slots that are
 * still missing. */
static bool tbt_fec_rx_decode_one(struct tbt_session *sess, u16 group,
                                  u16 slot)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    struct tbt_fec_blk *b = &rx->blk[(size_t)group * rx->slots + slot];
    u16 real = 0, i, k;
    bool claimed = false;
    int rc;

    spin_lock_bh(&rx->lock);
    if (b->used && !b->ready && !b->decoding)
    {
        real = tbt_fec_blk_real_len(rx, group, b);
        tbt_fec_blk_counts(rx, group, b);
        if (b->empty || b->ndata >= real)
        {
            b->ready = true; /* complete without the codec */
        }
        else if (b->nparity >= (u16)(real - b->ndata))
        {
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

    rc = tbt_fec_blk_decode(sess, group, b);
    spin_lock_bh(&rx->lock);
    if (rc == 0)
    {
        for (i = 0, k = 0; i < real; i++)
        {
            if (rx->dpresent_snap[i])
                continue;
            if (!b->dpresent[i])
            {
                memcpy(b->data + (size_t)i * rx->s,
                       rx->rec + (size_t)k * rx->s, rx->s);
                b->dpresent[i] = 1;
                b->dlen[i] = 0; /* a recovered share has no own length */
                b->ndata++;
            }
            k++;
        }
        b->ready = true;
    }
    b->decoding = false;
    spin_unlock_bh(&rx->lock);
    return rc == 0;
}

/* One pass over groups*slots. Called only by w_fec_rx. */
static void tbt_fec_rx_pass(struct tbt_session *sess)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    u16 group, slot;

    for (group = 0; group < rx->groups; group++)
        for (slot = 0; slot < rx->slots; slot++)
            (void)tbt_fec_rx_decode_one(sess, group, slot);
}

static void tbt_fec_rx_work_body(struct tbt_session *sess)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    bool again;
    bool nack_done = false;
    int rounds = 0;

    if (!rx || READ_ONCE(sess->closing) || READ_ONCE(rx->stopping))
        return;
    do
    {
        again = false;
        tbt_fec_rx_pass(sess);
        /* Ask for the missing shares before the delivery walk arms its own
         * (possibly 30 s) stall timer: the earlier 10 ms arm wins and keeps
         * the NACK retry alive. */
        if (!nack_done)
        {
            tbt_fec_nack_scan(sess);
            nack_done = true;
        }
        if (tbt_fec_deliver_ready(sess) == -ENOMEM)
            return; /* a retry is already armed (NACK 10 ms or delivery 2 ms) */
        spin_lock_bh(&rx->lock);
        if (rx->work_pending)
        {
            rx->work_pending = false;
            again = true;
        }
        spin_unlock_bh(&rx->lock);
    } while (again && ++rounds < 4);
    if (again)
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
}

void tbt_fec_rx_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_rx);
    struct tbt_fec_rx *rx = sess->fec_rx;
    bool owned = test_and_clear_bit(TBT_SW_FEC_RX, &sess->sched);

    if (!rx)
        goto out;
    if (atomic_fetch_or(1u << TBT_FEC_RUN_BIT, &rx->run) &
        (1u << TBT_FEC_RUN_BIT))
    {
        atomic_fetch_or(1u << TBT_FEC_AGAIN_BIT, &rx->run);
        goto out;
    }
    tbt_fec_rx_work_body(sess);
    atomic_fetch_and(~(1u << TBT_FEC_RUN_BIT), &rx->run);
    if (atomic_fetch_and(~(1u << TBT_FEC_AGAIN_BIT), &rx->run) &
        (1u << TBT_FEC_AGAIN_BIT))
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
out:
    if (owned)
        tbt_session_put(sess);
}

int tbt_fec_rx_record(struct tbt_carrier *c, u8 *rec, size_t len)
{
    struct tbt_session *sess = c->sess;
    struct tbt_fec_rx *rx = sess->fec_rx;
    struct tbt_fec_hdr h;
    struct tbt_fec_blk *b;
    const u8 *payload;
    u16 shares, group, share, slot;
    bool wake = false;

    if (!rx || len < TBT_FEC_HDR_LEN)
        return -EPROTO;
    if (tbt_fec_hdr_unpack(rec, &h))
    {
        atomic64_inc(&sess->fec_proto_fail);
        return -EPROTO;
    }
    if (tbt_fec_hdr_check(&h, TBT_FEC_S))
    {
        atomic64_inc(&sess->fec_proto_fail);
        return -EPROTO;
    }
    {
        u8 hb[TBT_FEC_HDR_LEN];
        u32 want;

        tbt_fec_hdr_pack(&h, hb);
        want = tbt_fec_crc32c(0, hb, TBT_FEC_H_HDRCRC);
        if (want != h.header_crc32c)
        {
            atomic64_inc(&sess->fec_hdr_crc_fail);
            return -EPROTO;
        }
        /* Teardown may start between the carrier parse and this record. Stop
         * before decryption so a non-zero return leaves the bytes untouched;
         * the raw parser consumes the record on any return, so a retry can
         * never decrypt the same buffer twice. */
        if (READ_ONCE(rx->stopping))
            return -ESHUTDOWN;
        /* A duplicate record must be dropped before decryption. The record
         * buffer is decrypted in place; a second pass XORs the plaintext and
         * fails the payload CRC, which looks like wire corruption and hides
         * the real duplicate. The header CRC above makes these names
         * trustworthy. */
        if (h.type == TBT_FEC_DATA || h.type == TBT_FEC_PARITY)
        {
            u16 cg = rx->groups ? (u16)(h.seq % rx->groups) : 0;
            u16 csh = h.share_index;
            u16 cshares = (u16)(rx->n + rx->m);
            u64 next = READ_ONCE(rx->next_super);
            bool dup = false, late = false;

            if (rx->groups && cg < rx->groups && csh < cshares &&
                h.block_id >= next && h.block_id < next + rx->slots)
            {
                spin_lock_bh(&rx->lock);
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
                spin_unlock_bh(&rx->lock);
            }
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
            if (tbt_crypto_body_xor_share(&sess->crypto_ctx.rx, h.seq,
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
        tx = sess->fec_tx;
        st = tbt_fec_stream(sess);
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
                u16 rk = tbt_fec_replay_k(tx->replay);
                u32 blocks = rk > 2 ? (u32)(rk - 2) : 1;
                u32 cap = blocks * tx->groups * tx->n;

                /* The peer cannot widen the window past the replay ring:
                 * the oldest block it may still NACK must stay in the ring. */
                WRITE_ONCE(tx->credit,
                           min_t(u32, a.credit_packets,
                                 min_t(u32, cap, TBT_FEC_CREDIT_MAX)));
            }
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
        struct tbt_fec_tx *tx = sess->fec_tx;
        u64 t0 = ktime_get_ns();
        u8 *buf;
        u16 g, s, shares;
        u32 requested = 0;

        if (tbt_fec_nack_unpack(rec + TBT_FEC_HDR_LEN, &n))
            return -EPROTO;
        if (!tx || !tx->replay)
        {
            atomic64_inc(&sess->fec_replay_miss_no_ring);
            atomic64_inc(&sess->fec_replay_miss);
            return 0;
        }
        shares = (u16)(tx->n + tx->m);
        if (!tbt_fec_bitmap_clear_from(n.group_mask, tx->groups) ||
            !tbt_fec_bitmap_clear_from(n.share_mask, shares))
        {
            atomic64_inc(&sess->fec_nack_bad);
            return -EPROTO;
        }
        atomic64_inc(&sess->fec_nack_rx);
        /* Repair is active from this point: the sender is about to build and
         * queue replay records, and the TX stall check must not kill the
         * stream while that work is in flight. */
        WRITE_ONCE(sess->fec_repair_last_ns, t0);
        buf = kmalloc(TBT_FEC_REC_MAX, GFP_ATOMIC);
        if (!buf)
        {
            atomic64_inc(&sess->fec_replay_miss_alloc);
            atomic64_inc(&sess->fec_replay_miss);
            return -ENOMEM;
        }
        for (g = 0; g < tx->groups; g++)
        {
            if (!tbt_fec_bitmap_test(n.group_mask, g))
                continue;
            if (requested > TBT_FEC_NACK_REQ_MAX)
                break;
            for (s = 0; s < shares; s++)
            {
                struct tbt_carrier *pc;
                u16 cidx;
                bool replaced = false;
                int err, len;

                if (!tbt_fec_bitmap_test(n.share_mask, s))
                    continue;
                /* A NACK may name a large bitmap; bound the replay work per
                 * record. The receiver retries the remainder. */
                if (++requested > TBT_FEC_NACK_REQ_MAX)
                    break;
                spin_lock_bh(&tx->replay_lock);
                len = tbt_fec_replay_get(tx->replay, h.block_id, g, s, buf,
                                         TBT_FEC_REC_MAX);
                spin_unlock_bh(&tx->replay_lock);
                if (len == -ENODATA)
                {
                    bool replied = false;
                    u16 pi;

                    if (debug >= 2)
                        pr_info("tbtun: fec NACK absent block=%llu group=%u share=%u cur=%llu gblk=%llu\n",
                                h.block_id, g, s, READ_ONCE(tx->cur_block),
                                READ_ONCE(tx->g[g].blk_id));
                    atomic64_inc(&sess->fec_replay_miss_never_stored);
                    atomic64_inc(&sess->fec_replay_miss_get);
                    atomic64_inc(&sess->fec_replay_miss);
                    if (h.block_id >= READ_ONCE(tx->cur_block))
                        continue; /* still being built: the share may appear */
                    /* The block is closed and this share was never
                     * generated. A short block (win_len < n) has padding
                     * shares that will never exist, but it does have parity
                     * rows whose header carries the real win_len/epoch.
                     * Re-send one of those rows: the receiver learns the cut
                     * and stops waiting for padding. This also repairs a
                     * real lost share when the parity can recover it. */
                    for (pi = 0; pi < tx->m && !replied; pi++)
                    {
                        int plen;

                        spin_lock_bh(&tx->replay_lock);
                        plen = tbt_fec_replay_get(tx->replay, h.block_id, g,
                                                  (u16)(tx->n + pi), buf,
                                                  TBT_FEC_REC_MAX);
                        spin_unlock_bh(&tx->replay_lock);
                        if (plen <= 0)
                            continue;
                        if (tbt_fec_replay_enqueue(sess, buf, (u16)plen, t0))
                            continue;
                        atomic64_inc(&sess->fec_nack_retx);
                        atomic64_add((u64)plen, &sess->fec_nack_retx_bytes);
                        atomic64_inc(&sess->fec_nack_parity_reply);
                        if (debug >= 2)
                        {
                            struct tbt_fec_hdr ph;

                            if (!tbt_fec_hdr_unpack(buf, &ph))
                                pr_info("tbtun: fec NACK parity reply block=%llu group=%u parity=%u win_len=%u epoch=%u\n",
                                        h.block_id, g, pi, ph.win_len,
                                        ph.epoch);
                        }
                        replied = true;
                    }
                    /* A group with no packet in the block has no parity
                     * row either: its empty marker is the truth. */
                    if (!replied &&
                        READ_ONCE(tx->g[g].blk_id) != h.block_id)
                    {
                        if (!tbt_fec_send_empty_marker(sess, g, h.block_id))
                            atomic64_inc(&sess->fec_nack_empty_reply);
                    }
                    continue;
                }
                if (len == -ESTALE)
                {
                    atomic64_inc(&sess->fec_replay_miss_evicted);
                    atomic64_inc(&sess->fec_replay_miss_get);
                    atomic64_inc(&sess->fec_replay_miss);
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
                cidx = (u16)(g * (tx->n + tx->m) + s);
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
                err = tbt_fec_replay_enqueue(sess, buf, (u16)len, t0);
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
        return 0;
    }
    if (h.type != TBT_FEC_DATA && h.type != TBT_FEC_PARITY)
        return 0; /* FBK/CFG are accepted and ignored by this cut. */

    /* The header names the share; the carrier it arrived on only has to be
     * one of this session's. A carrier that dies with a share still queued has
     * that share re-sent on a live carrier (tbt_fec_out_try), and the block is
     * placed by (seq, share_index) either way -- so a share is never lost just
     * because its usual carrier was replaced, and the codec is not asked to
     * recover a loss that could be avoided. */
    shares = (u16)(rx->n + rx->m);
    if (!shares)
        return -EPROTO;
    group = (u16)(h.seq % rx->groups);
    share = h.share_index;
    if (group >= rx->groups || share >= shares)
        return -EPROTO;
    if (h.n != (u8)rx->n || h.m != (u8)rx->m)
        return -EPROTO;
    if (h.block_id != (h.seq / rx->groups) / rx->n)
        return -EPROTO;

    payload = rec + TBT_FEC_HDR_LEN;
    spin_lock_bh(&rx->lock);
    if (READ_ONCE(rx->stopping))
    {
        spin_unlock_bh(&rx->lock);
        return -ESHUTDOWN;
    }
    if (h.block_id < rx->next_super)
    {
        atomic64_inc(&sess->fec_late_share);
        spin_unlock_bh(&rx->lock);
        return 0; /* already delivered */
    }
    if (h.block_id >= rx->next_super + rx->slots)
    {
        atomic64_inc(&sess->fec_drop_window);
        spin_unlock_bh(&rx->lock);
        return 0; /* too far ahead to buffer */
    }
    slot = (u16)((size_t)group * rx->slots + (h.block_id % rx->slots));
    b = &rx->blk[slot];
    if (!b->used || b->block_id != h.block_id)
        tbt_fec_blk_reset(b, h.block_id);
    if (h.block_id > rx->highest_block)
        rx->highest_block = h.block_id;
    if (b->empty)
    {
        /* A marker fixed this block as empty. Any later DATA or PARITY for it
         * means the sender mixed two block shapes; the block is immutable. */
        spin_unlock_bh(&rx->lock);
        return -EPROTO;
    }
    /* A marker that arrives after any real share is a mixed-source error even
     * when the block is already complete; the ready check below would
     * otherwise file it as a harmless late share. */
    if (h.type == TBT_FEC_PARITY && h.win_len == 0 &&
        (b->ndata || b->nparity || b->win_len_set || b->count_real != 0xffff))
    {
        spin_unlock_bh(&rx->lock);
        return -EPROTO;
    }
    if (b->ready)
    {
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
    else if (b->nparity >=
             (u16)(tbt_fec_blk_real_len(rx, group, b) - b->ndata))
        rx->work_pending = true;
    wake = rx->work_pending;
    spin_unlock_bh(&rx->lock);
    if (wake)
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
    return 0;
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
    struct tbt_fec_rx *rx = sess->fec_rx;
    u64 span;
    bool wake = false;

    if (!rx || !st)
        return;
    spin_lock_bh(&rx->lock);
    if (READ_ONCE(rx->stopping))
    {
        spin_unlock_bh(&rx->lock);
        return;
    }
    span = (u64)rx->groups * rx->n;
    if (!tbt_fec_fin_valid(pkt_end, pkt_base, span))
    {
        atomic64_inc(&sess->fec_fin_bad);
        spin_unlock_bh(&rx->lock);
        return;
    }
    if (rx->stream_sid && st->sid != rx->stream_sid)
    {
        rx->next_fin_sid = st->sid;
        rx->next_fin_end = pkt_end;
        rx->next_fin_base = pkt_base;
        rx->next_fin_bytes = total_bytes;
        rx->next_fin_valid = true;
        rx->work_pending = true;
        wake = true;
        spin_unlock_bh(&rx->lock);
        if (wake)
            tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
        return;
    }
    if (!rx->stream_sid)
    {
        rx->stream_sid = st->sid;
        rx->stream_base = rx->deliver_bytes;
        rx->stream_pkt_base = pkt_base;
    }
    if (!rx->total_known)
    {
        /* LAST was lost: the FIN's absolute end is the only statement of the
         * stream's extent. total_bytes is kept for the last-packet length
         * correction, not for deriving the packet count. */
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

static void tbt_fec_rx_free(struct tbt_session *sess)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    u32 total, i;

    if (!rx)
        return;
    if (!rx->blk)
    {
        tbt_fec_grouped_free(rx->codec);
        kvfree(rx->rec);
        kvfree(rx->zero);
        kvfree(rx->nack);
        kvfree(rx);
        sess->fec_rx = NULL;
        return;
    }
    total = (u32)rx->groups * rx->slots;
    for (i = 0; i < total; i++)
    {
        kvfree(rx->blk[i].data);
        kvfree(rx->blk[i].parity);
        kvfree(rx->blk[i].dpresent);
        kvfree(rx->blk[i].ppresent);
        kvfree(rx->blk[i].dlen);
    }
    kvfree(rx->blk);
    kvfree(rx->rec);
    kvfree(rx->zero);
    kvfree(rx->nack);
    tbt_fec_grouped_free(rx->codec);
    kvfree(rx);
    sess->fec_rx = NULL;
}

void tbt_fec_data_free(struct tbt_session *sess)
{
    struct tbt_fec_tx *tx = sess->fec_tx;
    struct tbt_fec_out *o, *tmp;
    u16 i;

    /* The work items are cancelled before this point, but records already
     * queued for a carrier are not owned by them. Drain every queue here or
     * every teardown with a stalled carrier leaks its pending shares. The
     * lists and lock exist only after a successful init (sess->fec). */
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
    }
    kvfree(sess->fec_outq);
    sess->fec_outq = NULL;
    if (tx)
    {
        if (tx->g)
        {
            for (i = 0; i < tx->groups; i++)
            {
                kvfree(tx->g[i].blk);
                kvfree(tx->g[i].par);
            }
            kvfree(tx->g);
        }
        tbt_fec_replay_free(tx->replay);
        tbt_fec_grouped_free(tx->codec);
        kvfree(tx->ring);
        kvfree(tx->pkt);
        kvfree(tx->rec);
        kvfree(tx);
        sess->fec_tx = NULL;
    }
    tbt_fec_rx_free(sess);
}

int tbt_fec_data_init(struct tbt_session *sess)
{
    struct tbt_tunnel *t = sess->tunnel;
    struct tbt_fec_rx *rx;
    struct tbt_fec_tx *tx;
    u16 n = t->cfg.n, m = t->cfg.m, shares, groups, w, d;
    u32 per, slots, total, i;

    /* Every record the parsers accept must fit the buffer that assembles it.
     * Without these, raising TBT_FEC_PAYLOAD_MAX would make a full record
     * impossible to complete and the parser would stick at p->have ==
     * sizeof(p->buf) with no error path. */
    BUILD_BUG_ON(TBT_PARSER_BUF < TBT_FEC_REC_MAX);
    BUILD_BUG_ON(TBT_FEC_REC_MAX != TBT_FEC_HDR_LEN + TBT_FEC_PAYLOAD_MAX);
    BUILD_BUG_ON(TBT_PARSER_BUF < TBT_REC_MAX);
    BUILD_BUG_ON(TBT_CAR_PART_MAX < TBT_FEC_REC_MAX);
    BUILD_BUG_ON(sizeof(((struct tbt_http_rx *)0)->rec) != TBT_FEC_REC_MAX);

    if (!n || !m || (u16)n + (u16)m > TBT_FEC_MAX_SHARES)
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec rejected: n=%u m=%u exceeds %u shares\n",
                            (unsigned long long)t->cfg.id, n, m,
                            TBT_FEC_MAX_SHARES);
        return -EINVAL;
    }
    if (t->cfg.mode == 3)
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec rejected: mode=3 is SC-LDPC\n",
                            (unsigned long long)t->cfg.id);
        return -EOPNOTSUPP; /* SC-LDPC is D's codec, not a silent RLNC alias */
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
    /* One group keeps the proven block code (w = n). A grouped session uses
     * the configured window (2n by default): its wider coefficient space lets
     * four parity rows reach rank 4, which the even-weight rows of w = n
     * cannot do for n = 4. The window is a local codec parameter; win_len on
     * the wire still names the n packets of one block. */
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

    /* One super-block across every group is groups*(n+m)*S bytes. Keep the
     * whole RX arena at or below 16 MiB: 32x8 gets 8 slots, one group keeps
     * the historical 256. */
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
             /* Data plus parity for every block, not the descriptor array. */
             (u32)(((size_t)total * shares * TBT_FEC_S) >> 10));

    /* One queue per carrier: the flush walks these directly, so a full socket
     * stops only its own queue. Control records live on fec_out_ctl. */
    sess->fec_outq = kvzalloc((size_t)t->cfg.carriers *
                                  sizeof(*sess->fec_outq),
                              GFP_KERNEL);
    if (!sess->fec_outq)
        goto fail;
    for (i = 0; i < t->cfg.carriers; i++)
        INIT_LIST_HEAD(&sess->fec_outq[i].queue);

    rx = kzalloc(sizeof(*rx), GFP_KERNEL);
    tx = kzalloc(sizeof(*tx), GFP_KERNEL);
    if (!rx || !tx)
        goto fail;
    spin_lock_init(&rx->lock);
    atomic_set(&rx->run, 0);
    rx->n = n;
    rx->m = m;
    rx->s = TBT_FEC_S;
    rx->groups = groups;
    rx->slots = (u16)slots;
    /* The receiver NACK window and the ACK credit use the same effective K
     * as the sender ring: K-2 super-blocks of credit plus two of slack. */
    rx->replay_k = tbt_fec_replay_k_for(groups, n, m, TBT_FEC_S);
    /* Optional: without it the NACK scan stays silent and the stream relies
     * on parity and the carrier TCP retransmission. */
    rx->nack = kvzalloc(sizeof(*rx->nack), GFP_KERNEL);
    rx->blk = kvzalloc((size_t)total * sizeof(*rx->blk), GFP_KERNEL);
    /* Decode scratch: one recovered slot per parity row, and one zero slot
     * for padding and absent parity. Both are written outside rx->lock. */
    rx->rec = kvzalloc((size_t)m * TBT_FEC_S, GFP_KERNEL);
    rx->zero = kvzalloc(TBT_FEC_S, GFP_KERNEL);
    TBT_DIAG("fec grouped rx codec start tunnel=%llu groups=%u n=%u m=%u w=%u d=%u",
             (unsigned long long)t->cfg.id, groups, n, m, w, d);
    rx->codec = tbt_fec_grouped_new(groups, n, m, n, w, TBT_FEC_S, d,
                                    tbt_fec_base_nonce(sess));
    TBT_DIAG("fec grouped rx codec %s tunnel=%llu groups=%u blk=%d",
             rx->codec ? "ok" : "FAILED", (unsigned long long)t->cfg.id,
             groups, !!rx->blk);
    if (!rx->blk || !rx->codec || !rx->rec || !rx->zero)
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec rx arena/codec failed: blk=%d codec=%d rec=%d zero=%d\n",
                            (unsigned long long)t->cfg.id, !!rx->blk,
                            !!rx->codec, !!rx->rec, !!rx->zero);
        goto fail;
    }

    tx->nonce = tbt_fec_base_nonce(sess);
    spin_lock_init(&tx->replay_lock);
    tx->replay = tbt_fec_replay_new(groups, n, m, TBT_FEC_S);
    if (tx->replay)
        TBT_DIAG("fec replay ring tunnel=%llu k=%u bytes=%zu budget=%u",
                 (unsigned long long)t->cfg.id,
                 tbt_fec_replay_k(tx->replay),
                 tbt_fec_replay_bytes(groups, n, m, TBT_FEC_S,
                                      tbt_fec_replay_k(tx->replay)),
                 TBT_FEC_REPLAY_BUDGET);
    else
        pr_warn_ratelimited("tbtun: tunnel %llu: no FEC replay ring; NACK repair disabled\n",
                            (unsigned long long)t->cfg.id);
    tx->groups = groups;
    /* The first flight uses the same K-2 super-block window the receiver
     * grants, so the replay ring covers it before the first ACK arrives. */
    {
        u16 rk = tbt_fec_replay_k(tx->replay);
        u32 blocks = rk > 2 ? (u32)(rk - 2) : 1;

        tx->credit = (u32)min_t(u64, (u64)slots * groups * n,
                                (u64)blocks * groups * n);
        tx->credit = min(tx->credit, TBT_FEC_CREDIT_MAX);
    }
    /* Capture pauses at ring granularity, so match the ring to the credit
     * window instead of a fixed 256 KiB. The floor keeps a small shape from
     * pausing on every chunk; the ceiling bounds per-session memory. */
    {
        u64 want = (u64)tx->credit * TBT_FEC_S;

        if (want > TBT_FEC_TX_RING_MAX)
            want = TBT_FEC_TX_RING_MAX;
        if (want < TBT_FEC_TX_RING)
            want = TBT_FEC_TX_RING;
        tx->ring_size = (u32)want;
    }
    tx->n = n;
    tx->m = m;
    tx->s = TBT_FEC_S;
    spin_lock_init(&tx->lock);
    atomic_set(&tx->run, 0);
    tx->ring = kvzalloc(tx->ring_size, GFP_KERNEL);
    tx->pkt = kvzalloc(TBT_FEC_S, GFP_KERNEL);
    tx->rec = kvzalloc(TBT_FEC_REC_MAX, GFP_KERNEL);
    tx->g = kvzalloc((size_t)groups * sizeof(*tx->g), GFP_KERNEL);
    TBT_DIAG("fec grouped tx codec start tunnel=%llu groups=%u n=%u m=%u w=%u d=%u",
             (unsigned long long)t->cfg.id, groups, n, m, w, d);
    tx->codec = tbt_fec_grouped_new(groups, n, m, n, w, TBT_FEC_S, d,
                                    tx->nonce);
    TBT_DIAG("fec grouped tx codec %s tunnel=%llu groups=%u ring=%d pkt=%d rec=%d g=%d",
             tx->codec ? "ok" : "FAILED", (unsigned long long)t->cfg.id,
             groups, !!tx->ring, !!tx->pkt, !!tx->rec, !!tx->g);
    if (!tx->ring || !tx->pkt || !tx->rec || !tx->g || !tx->codec)
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec tx arena/codec failed: ring=%d pkt=%d rec=%d g=%d codec=%d\n",
                            (unsigned long long)t->cfg.id, !!tx->ring,
                            !!tx->pkt, !!tx->rec, !!tx->g, !!tx->codec);
        goto fail;
    }

    for (i = 0; i < groups; i++)
    {
        tx->g[i].blk = kvzalloc((size_t)n * TBT_FEC_S, GFP_KERNEL);
        tx->g[i].par = kvzalloc((size_t)m * TBT_FEC_S, GFP_KERNEL);
        /* No group belongs to block 0 until it writes a packet into it. The
         * sentinel is what lets close_block() tell "empty group" from "full
         * block whose nreal was already cleared". */
        tx->g[i].blk_id = ~0ULL;
        if (!tx->g[i].blk || !tx->g[i].par)
        {
            pr_warn_ratelimited("tbtun: tunnel %llu: fec tx group alloc failed: group=%u blk=%d par=%d\n",
                                (unsigned long long)t->cfg.id, i,
                                !!tx->g[i].blk, !!tx->g[i].par);
            goto fail;
        }
    }
    for (i = 0; i < total; i++)
    {
        struct tbt_fec_blk *b = &rx->blk[i];

        b->data = kvzalloc((size_t)n * TBT_FEC_S, GFP_KERNEL);
        b->parity = kvzalloc((size_t)m * TBT_FEC_S, GFP_KERNEL);
        b->dpresent = kvzalloc(TBT_FEC_MAX_SHARES, GFP_KERNEL);
        b->ppresent = kvzalloc(TBT_FEC_MAX_SHARES, GFP_KERNEL);
        b->dlen = kvzalloc(TBT_FEC_MAX_SHARES * sizeof(u16), GFP_KERNEL);
        if (!b->data || !b->parity || !b->dpresent || !b->ppresent ||
            !b->dlen)
        {
            pr_warn_ratelimited("tbtun: tunnel %llu: fec rx block alloc failed: block=%u of %u data=%d parity=%d dpresent=%d ppresent=%d dlen=%d\n",
                                (unsigned long long)t->cfg.id, i, total,
                                !!b->data, !!b->parity, !!b->dpresent,
                                !!b->ppresent, !!b->dlen);
            goto fail;
        }
    }

    sess->fec_rx = rx;
    sess->fec_tx = tx;
    INIT_LIST_HEAD(&sess->fec_out_ctl);
    INIT_LIST_HEAD(&sess->fec_out_replay);
    spin_lock_init(&sess->fec_out_lock);
    INIT_DELAYED_WORK(&sess->w_fec_tx, tbt_fec_tx_work);
    INIT_DELAYED_WORK(&sess->w_fec_rx, tbt_fec_rx_work);
    sess->fec = true;
    TBT_DIAG("fec init done tunnel=%llu groups=%u slots=%u credit=%u tx_ring=%u",
             (unsigned long long)t->cfg.id, groups, slots, tx->credit,
             tx->ring_size);
    return 0;

fail:
    pr_warn_ratelimited("tbtun: tunnel %llu: fec init failed\n",
                        (unsigned long long)t->cfg.id);
    sess->fec_rx = rx;
    sess->fec_tx = tx;
    tbt_fec_data_free(sess);
    return -ENOMEM;
}
