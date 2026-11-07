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

#include "tbtun.h"
#include "tbt_fec.h"
#include "tbt_rec.h"

#define TBT_FEC_TX_RING (256 * 1024)

/* How long a partial packet may wait for company. The packetizer emits a
 * share only when its packet is full, so an application that writes a small
 * request and then waits for the answer -- an HTTP request, iperf3's control
 * cookie -- would wait for 8192 bytes that never come, and the tunnel would
 * look connected and silent. 2 ms is above the gap between two writes of a
 * saturating sender, so bulk traffic still fills its packets, and far below
 * any round trip, so an interactive exchange pays one tick. */
#define TBT_FEC_SHORT_MS 2
#define TBT_FEC_NONCE_GOLDEN 0x9e3779b97f4a7c15ULL
/* The RX arena keeps at most one window of 1024 packets, and the ACK grants
 * exactly that: slots * groups * n reaches 1024 for every supported shape
 * (the 16 MiB arena divided by (n + m) * S is 256 super-blocks). */
#define TBT_FEC_RX_BUDGET (16u * 1024 * 1024)
#define TBT_FEC_CREDIT_MAX 1024u

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

static void tbt_fec_out_free(struct tbt_fec_out *o)
{
    list_del(&o->node);
    kfree(o);
}

static int tbt_fec_out_push(struct tbt_session *sess, u16 carrier, const u8 *buf,
                            u16 len, bool control)
{
    struct tbt_fec_out *o;

    o = kmalloc(struct_size(o, data, len), GFP_ATOMIC);
    if (!o)
        return -ENOMEM;
    o->carrier = carrier;
    o->len = len;
    o->control = control ? 1 : 0;
    memcpy(o->data, buf, len);
    spin_lock(&sess->fec_out_lock);
    if (control)
        list_add(&o->node, &sess->fec_out);
    else
        list_add_tail(&o->node, &sess->fec_out);
    spin_unlock(&sess->fec_out_lock);
    tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    return 0;
}

static int tbt_fec_out_try(struct tbt_session *sess, struct tbt_fec_out *o)
{
    struct tbt_carrier *c;
    int i;

    if (o->control)
    {
        /* ACKs are the only control records on this queue, and a cumulative
         * ACK is safe on any carrier. Start after the carrier that last
         * accepted one and try every up carrier: pinning the queue to the
         * lowest up carrier lets one congested socket hold every ACK behind
         * its data, and a late ACK is a stalled credit window. */
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
            err = tbt_carrier_send_fec(c, o->data, o->len);
            tbt_carrier_put(c);
            if (!err)
            {
                if (tx)
                    tx->ctl_rot = (u16)((idx + 1) % sess->n_car);
                return 0;
            }
            /* A dead socket must not end the scan; a full one must not stop
             * it either, because the next carrier may have room. A full
             * carrier anywhere keeps the record queued even if every later
             * one is dead: -EAGAIN wins over -ENODEV. */
            if (err == -EAGAIN)
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
    if (c && atomic_read(&c->up) && !atomic_read(&c->stopping))
    {
        int err = tbt_carrier_send_fec(c, o->data, o->len);

        tbt_carrier_put(c);
        return err;
    }
    if (c)
        tbt_carrier_put(c);
    /* The share's own carrier is gone or not usable yet. Send it on a live one
     * instead of dropping it: the receiver places a share by its header, and a
     * dropped share is a block the codec has to recover -- which it cannot
     * always do (measured: 3 of 4 shares plus all 4 parity rows stayed
     * undecodable, and the whole stream stopped behind that block). A carrier
     * that has sent HELLO can carry bytes even before this stream's OPEN is
     * written on it, because the OPEN precedes any DATA on that carrier only
     * for frames the pump pins to it. */
    for (i = 0; i < sess->n_car; i++)
    {
        int err;

        if (i == o->carrier)
            continue;
        c = tbt_carrier_get(sess, (u8)i);
        if (!c)
            continue;
        if (!READ_ONCE(c->hello_sent) || atomic_read(&c->stopping) ||
            !atomic_read(&c->connected))
        {
            tbt_carrier_put(c);
            continue;
        }
        err = tbt_carrier_send_fec(c, o->data, o->len);
        tbt_carrier_put(c);
        return err;
    }
    return -ENODEV;
}

/* Write queued records. The session pump lock serialises these writes with
 * the old control path (HELLO/OPEN/FIN/PAD), so a record is never spliced
 * into another record's bytes on one carrier. */
static int tbt_fec_out_flush(struct tbt_session *sess)
{
    int ret = 0;

    mutex_lock(&sess->pump_lock);
    for (;;)
    {
        struct tbt_fec_out *o;
        int err;

        spin_lock(&sess->fec_out_lock);
        o = list_first_entry_or_null(&sess->fec_out, struct tbt_fec_out, node);
        spin_unlock(&sess->fec_out_lock);
        if (!o)
            break;
        err = tbt_fec_out_try(sess, o);
        if (err == -EAGAIN)
        {
            ret = -EAGAIN;
            break;
        }
        spin_lock(&sess->fec_out_lock);
        tbt_fec_out_free(o);
        spin_unlock(&sess->fec_out_lock);
    }
    mutex_unlock(&sess->pump_lock);
    return ret;
}

/* Queue one complete record. Control records may ride any up carrier; data
 * records are bound to their share's carrier. Returns 0 when queued. */
static int tbt_fec_send_record(struct tbt_session *sess, u16 carrier, u8 *buf,
                               u16 len, bool control)
{
    return tbt_fec_out_push(sess, carrier, buf, len, control);
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
        tbt_fec_send_record(sess,
                            (u16)(group * (tx->n + tx->m) + tx->n + i),
                            tx->rec, rec_len, false);
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
    tbt_fec_send_record(sess,
                        (u16)(group * (tx->n + tx->m) + tx->n),
                        tx->rec, rec_len, false);
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

    if (READ_ONCE(st->fec_finished))
        return;
    WRITE_ONCE(st->fec_pkt_end, tx->seq);
    if (st->fec_pkt_base_set)
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
        (void)tbt_fec_send_record(sess, carrier, rec, rec_len, false);
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
    idle = list_empty(&sess->fec_out);
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
    if (tbt_fec_out_flush(sess) == -EAGAIN)
    {
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
        return;
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
    if (tbt_fec_out_flush(sess) == -EAGAIN)
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
}

void tbt_fec_tx_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_tx);
    bool owned = test_and_clear_bit(TBT_SW_FEC_TX, &sess->sched);

    tbt_fec_tx_work_body(sess);
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
        WRITE_ONCE(st->tx_paused, true);
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
    u16 len;

    /* A marker says the group has no packet in this block; nothing in it is
     * ever delivered, whatever the stream-length fallback would say. */
    if (b->empty)
        return 0;
    len = b->nreal ? b->nreal : rx->n;

    /* A block that was closed early has fewer real shares than n, and its
     * parity says so. The stream-length formula below describes the standard
     * layout, which a skipped seq range no longer matches, so it is only the
     * fallback for a block whose parity was lost. */
    if (!b->nreal && rx->total_known)
    {
        u64 final = tbt_fec_final_block_of_total(rx->total_pkts, rx->groups,
                                                 rx->n);

        if (b->block_id == final)
        {
            u16 tail = tbt_fec_group_tail_len(rx->total_pkts, rx->groups,
                                              rx->n, group);

            if (tail < len)
                len = tail;
        }
    }
    return len;
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
    /* A recovered share has no payload_len of its own; its delivery length is
     * the block's last-real-share length from the parity's epoch field, or S
     * when that is zero. A block that holds a short share and states no length
     * (a peer from before the epoch field carried it) would have to be guessed,
     * and the guess would hand the application the block's zero padding.
     * Refuse it instead: the sender's stall timer is the bound, and the counter
     * says why the stream stopped. */
    if (!b->short_len)
    {
        for (i = 0; i < real; i++)
        {
            if (rx->dpresent_snap[i] && b->dlen[i] && b->dlen[i] < rx->s)
            {
                if (!b->short_unknown)
                {
                    b->short_unknown = true;
                    atomic64_inc(&sess->fec_short_unknown);
                }
                return -EAGAIN;
            }
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

static void tbt_fec_ack(struct tbt_session *sess, u64 next_deliver)
{
    struct tbt_fec_hdr h;
    struct tbt_fec_ack a;
    u8 body[TBT_FEC_ACK_LEN];
    u8 rec[TBT_FEC_HDR_LEN + TBT_FEC_ACK_LEN];
    u16 len;

    memset(&a, 0, sizeof(a));
    a.next_deliver = next_deliver;
    /* The receiver buffers `slots` super-blocks; the credit must not exceed
     * that window, or the sender would overrun it and its shares would be
     * dropped as "too far ahead". */
    a.credit_packets = (u32)min_t(u64,
                                  (u64)sess->fec_rx->slots * sess->fec_rx->groups *
                                      sess->fec_rx->n,
                                  TBT_FEC_CREDIT_MAX);
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
    if (tbt_fec_send_record(sess, 0, rec, len, true))
        return;
    atomic64_inc(&sess->fec_ack_tx);
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

        if (rx->total_known)
        {
            u64 final = tbt_fec_final_block_of_total(rx->total_pkts,
                                                     rx->groups, rx->n);

            if (s == final &&
                !tbt_fec_group_tail_len(rx->total_pkts, rx->groups, rx->n,
                                        group))
                continue;
        }
        b = tbt_fec_rx_blk((struct tbt_fec_rx *)rx, group, s);
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
            break;

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

                if (known && seq >= total_pkts)
                    continue;
                b = tbt_fec_rx_blk(rx, group, s);
                /* A block that was closed early has fewer real shares than n;
                 * the rest are the encoder's zero padding and carry no stream
                 * bytes. A marker block has no real share at all. */
                real = tbt_fec_blk_real_len(rx, group, b);
                if (p >= real)
                    continue;
                if (b->dlen[p])
                    len = b->dlen[p];
                else if (b->short_len && p + 1 == real)
                    len = b->short_len;
                else
                    len = rx->s;
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
                if (err == -EPROTO)
                    atomic64_inc(&sess->fec_deliver_bad);
                off += len;
                spin_lock_bh(&rx->lock);
                rx->deliver_bytes = off; /* per-packet commit */
                spin_unlock_bh(&rx->lock);
                delivered = true;
            }
            start_g = 0;
        }
        rx->deliver_partial = false;
        rx->deliver_p = 0;
        rx->deliver_g = 0;
        rx->next_super++;
        rx->next_deliver = (s + 1) * (u64)rx->groups * rx->n;
        if (known && rx->next_deliver > total_pkts)
            rx->next_deliver = total_pkts;
    }
    if (delivered && rx->next_deliver > rx->ack_deliver)
    {
        rx->ack_deliver = rx->next_deliver;
        tbt_fec_ack(sess, rx->next_deliver);
    }
    tbt_stream_put(st);
    return 0;
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
    int rounds = 0;

    if (!rx || READ_ONCE(sess->closing) || READ_ONCE(rx->stopping))
        return;
    do
    {
        again = false;
        tbt_fec_rx_pass(sess);
        if (tbt_fec_deliver_ready(sess) == -ENOMEM)
            return; /* the 2 ms retry is already armed */
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
    bool owned = test_and_clear_bit(TBT_SW_FEC_RX, &sess->sched);

    tbt_fec_rx_work_body(sess);
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
             * frontier first, then convert it stream-relative with the same
             * saturation tbt_fec_ack_bytes() applies. */
            u64 sent = READ_ONCE(tx->seq);
            u64 base, acked;

            if (a.next_deliver > sent)
            {
                a.next_deliver = sent;
                atomic64_inc(&sess->fec_ack_bad);
            }
            base = smp_load_acquire(&st->fec_pkt_base_set) ?
                   READ_ONCE(st->fec_pkt_base) : 0;
            acked = tbt_fec_ack_bytes(a.next_deliver, base,
                                      READ_ONCE(st->tx_seq));
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
                WRITE_ONCE(tx->credit,
                           min_t(u32, a.credit_packets, TBT_FEC_CREDIT_MAX));
            tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
            /* The FIN pump holds the close until tx_ack covers tx_seq. */
            tbt_tx_wake(sess);
        }
        if (st)
            tbt_stream_put(st);
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
        spin_unlock_bh(&rx->lock);
        return 0;
    }

    if (h.type == TBT_FEC_DATA)
    {
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
            /* ndata is a cached count; a new present share inside the real
             * range advances it without rescanning the block. A share outside
             * the cached real length is padding for a shape the block no
             * longer has, and the next counts pass will drop it. */
            if (share < b->count_real)
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
     * queued for a carrier are not owned by them. Drain the queue here or
     * every teardown with a stalled carrier leaks its pending shares. The
     * list and lock exist only after a successful init (sess->fec). */
    if (sess->fec)
    {
        spin_lock(&sess->fec_out_lock);
        list_for_each_entry_safe(o, tmp, &sess->fec_out, node)
            tbt_fec_out_free(o);
        spin_unlock(&sess->fec_out_lock);
    }
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

    rx = kzalloc(sizeof(*rx), GFP_KERNEL);
    tx = kzalloc(sizeof(*tx), GFP_KERNEL);
    if (!rx || !tx)
        goto fail;
    spin_lock_init(&rx->lock);
    rx->n = n;
    rx->m = m;
    rx->s = TBT_FEC_S;
    rx->groups = groups;
    rx->slots = (u16)slots;
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

    tx->ring_size = TBT_FEC_TX_RING;
    tx->nonce = tbt_fec_base_nonce(sess);
    tx->groups = groups;
    /* Until the first ACK arrives the sender may run one window ahead; the
     * receiver's first ACK replaces this with its own window size, which is
     * the same slots * groups * n. Deriving it here keeps the two ends from
     * drifting apart if the arena geometry changes. */
    tx->credit = (u32)min_t(u64, (u64)slots * groups * n, TBT_FEC_CREDIT_MAX);
    tx->n = n;
    tx->m = m;
    tx->s = TBT_FEC_S;
    spin_lock_init(&tx->lock);
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
    INIT_LIST_HEAD(&sess->fec_out);
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
