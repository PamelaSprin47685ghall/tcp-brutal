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
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "tbtun.h"
#include "tbt_fec.h"
#include "tbt_rec.h"

#define TBT_FEC_TX_RING (256 * 1024)
#define TBT_FEC_S TBT_FEC_PAYLOAD_MAX
#define TBT_FEC_NONCE_GOLDEN 0x9e3779b97f4a7c15ULL
#define TBT_FEC_RX_BUDGET (16u * 1024 * 1024)

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
    schedule_work(&sess->w_fec_tx.work);
    return 0;
}

static int tbt_fec_out_try(struct tbt_session *sess, struct tbt_fec_out *o)
{
    struct tbt_carrier *c;
    int i;

    if (o->control)
    {
        for (i = 0; i < sess->n_car; i++)
        {
            c = READ_ONCE(sess->car[i]);
            if (!c || !atomic_read(&c->up) || atomic_read(&c->stopping))
                continue;
            return tbt_carrier_send_fec(c, o->data, o->len);
        }
        return -ENODEV;
    }
    if (o->carrier >= sess->n_car)
        return -ENODEV;
    c = READ_ONCE(sess->car[o->carrier]);
    if (!c || !atomic_read(&c->up) || atomic_read(&c->stopping))
        return -ENODEV;
    return tbt_carrier_send_fec(c, o->data, o->len);
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
        h.conv = TBT_FEC_CONV;
        if (tbt_fec_make_record(sess, &h, tx->parity_ptrs[i], tx->rec, &rec_len))
            continue;
        tbt_fec_send_record(sess,
                            (u16)(group * (tx->n + tx->m) + tx->n + i),
                            tx->rec, rec_len, false);
    }
}

/* Flush the stream: every group whose final block is short gets parity over
 * its zero-filled tail, so a lost real share is still recoverable and the
 * receiver learns the real length from win_len. Groups whose final block is
 * full already sent their parity when the block completed. */
static void tbt_fec_tx_finish(struct tbt_session *sess)
{
    struct tbt_fec_tx *tx = sess->fec_tx;
    u64 last;
    u16 group;

    if (!tx->seq)
        return;
    last = tx->seq - 1;
    for (group = 0; group < tx->groups && group <= last; group++)
    {
        u64 local = (last - group) / tx->groups;
        u16 len = (u16)(local % tx->n) + 1;
        u64 block_id = local / tx->n;

        if (len == tx->n)
            continue;
        memset(tx->g[group].blk + (size_t)len * TBT_FEC_S, 0,
               (size_t)(tx->n - len) * TBT_FEC_S);
        tbt_fec_tx_emit_parity(sess, group, block_id, len);
    }
}

static void tbt_fec_packetize(struct tbt_session *sess, bool last)
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

    if (share + 1 == tx->n)
        tbt_fec_tx_emit_parity(sess, group, block_id, tx->n);
    if (last)
        tbt_fec_tx_finish(sess);
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

void tbt_fec_tx_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_tx);
    struct tbt_fec_tx *tx = sess->fec_tx;
    struct tbt_stream *st;
    bool paused = false;

    if (!tx || READ_ONCE(sess->closing))
        return;
    if (tbt_fec_out_flush(sess) == -EAGAIN)
    {
        mod_delayed_work(system_wq, &sess->w_fec_tx, msecs_to_jiffies(2));
        return;
    }

    /* crypto=on: the far side cannot packetize until the near's HELLO has
     * delivered the session key. The ring keeps the bytes; retry shortly. */
    if (sess->crypto && !smp_load_acquire(&sess->crypto_tx_ready))
    {
        mod_delayed_work(system_wq, &sess->w_fec_tx, msecs_to_jiffies(10));
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
            if (tx->fill == TBT_FEC_S)
                tbt_fec_packetize(sess, false);
            continue;
        }
        if (tx->fill == TBT_FEC_S)
        {
            tbt_fec_packetize(sess, false);
            continue;
        }
        if (READ_ONCE(tx->flush_req) && tx->head == tx->tail)
        {
            if (tx->fill)
                tbt_fec_packetize(sess, true);
            else
                tbt_fec_tx_finish(sess);
            WRITE_ONCE(tx->flush_req, false);
            continue;
        }
        break;
    }
    if (paused)
    {
        WRITE_ONCE(st->tx_paused, false);
        schedule_work(&st->w_read);
    }
    tbt_stream_put(st);
    if (tbt_fec_out_flush(sess) == -EAGAIN)
        mod_delayed_work(system_wq, &sess->w_fec_tx, msecs_to_jiffies(2));
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
    schedule_work(&sess->w_fec_tx.work);
    return take;
}

bool tbt_fec_tx_idle(struct tbt_stream *st)
{
    return tbt_fec_tx_is_idle(st->sess);
}

void tbt_fec_tx_flush(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;
    struct tbt_fec_tx *tx = sess->fec_tx;

    if (!tx)
        return;
    /* The packetizer is owned by tbt_fec_tx_work; a flush from the stream
     * close path only raises a flag, so two CPUs never touch tx->pkt. */
    WRITE_ONCE(tx->flush_req, true);
    mod_delayed_work(system_wq, &sess->w_fec_tx, 0);
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
    return have < *need ? TBT_REC_NEED_MORE : TBT_REC_OK;
}

static void tbt_fec_blk_reset(struct tbt_fec_blk *b, u64 block_id)
{
    b->used = true;
    b->ready = false;
    b->nreal = 0;
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
    u16 len = b->nreal ? b->nreal : rx->n;

    if (rx->total_known)
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

/* Decode one block in place. Padding positions (index >= the real length) are
 * known zeros: the sender encoded them that way, and they are never delivered.
 * Returns 0 when every real share is present, -EAGAIN otherwise. */
static int tbt_fec_blk_decode(struct tbt_session *sess, u16 group,
                              struct tbt_fec_blk *b)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    struct tbt_fec_rlnc *ctx = tbt_fec_grouped_ctx(rx->codec, group);
    u16 len, missing = 0, have_parity = 0, i;
    int rc;

    len = tbt_fec_blk_real_len(rx, group, b);
    if (!len)
        return -EAGAIN; /* this group has no packet in this block */
    for (i = len; i < rx->n; i++)
    {
        if (!b->dpresent[i] || b->dlen[i])
        {
            memset(b->data + (size_t)i * rx->s, 0, rx->s);
            b->dpresent[i] = 1;
            b->dlen[i] = 0;
        }
    }
    for (i = 0; i < rx->n; i++)
    {
        rx->data_ptrs[i] = b->data + (size_t)i * rx->s;
        if (!b->dpresent[i])
            missing++;
    }
    for (i = 0; i < rx->m; i++)
    {
        rx->parity_ptrs[i] = b->parity + (size_t)i * rx->s;
        if (b->ppresent[i])
            have_parity++;
    }
    if (!missing)
        return 0;
    if (!ctx || have_parity < missing)
        return -EAGAIN;
    tbt_fec_rlnc_set_nonce(ctx, tbt_fec_group_nonce(tbt_fec_base_nonce(sess),
                                                    group, b->block_id));
    rc = tbt_fec_rlnc_decode(ctx, rx->data_ptrs, rx->parity_ptrs, b->dpresent,
                             b->ppresent, rx->n);
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
    a.credit_packets = 1024;
    if (tbt_fec_ack_pack(&a, body))
        return;
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_ACK;
    h.seq = next_deliver;
    h.payload_len = TBT_FEC_ACK_LEN;
    h.conv = TBT_FEC_CONV;
    if (tbt_fec_make_record(sess, &h, body, rec, &len))
        return;
    tbt_fec_send_record(sess, 0, rec, len, true);
}

static struct tbt_fec_blk *tbt_fec_rx_blk(struct tbt_fec_rx *rx, u16 group,
                                          u64 block_id)
{
    return &rx->blk[(size_t)group * rx->slots + (block_id % rx->slots)];
}

/* Every group must have decoded block s before any byte of it is delivered.
 * In the final super-block a group with no packet is complete by definition. */
static bool tbt_fec_super_ready(const struct tbt_fec_rx *rx, u64 s)
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

static void tbt_fec_deliver_ready(struct tbt_session *sess)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    struct tbt_stream *st;
    bool delivered = false;

    if (rx->total_known && !rx->total_pkts)
        return;

    st = tbt_fec_stream(sess);
    if (!st)
        return;
    for (;;)
    {
        u64 s = rx->next_super;
        u64 base = s * (u64)rx->groups * rx->n;
        u16 p, group;

        if (rx->total_known)
        {
            u64 final = tbt_fec_final_block_of_total(rx->total_pkts,
                                                     rx->groups, rx->n);

            if (s > final)
                break;
        }
        if (!tbt_fec_super_ready(rx, s))
            break;
        for (p = 0; p < rx->n; p++)
        {
            for (group = 0; group < rx->groups; group++)
            {
                struct tbt_fec_blk *b;
                u64 seq = base + (u64)p * rx->groups + group;
                u16 len;

                if (rx->total_known && seq >= rx->total_pkts)
                    continue;
                b = tbt_fec_rx_blk(rx, group, s);
                len = b->dlen[p] ? b->dlen[p] : rx->s;
                if (rx->total_known && rx->total_bytes &&
                    seq + 1 == rx->total_pkts)
                {
                    u64 tail = rx->total_bytes -
                               (rx->total_pkts - 1) * (u64)rx->s;

                    if (tail > 0 && tail <= rx->s)
                        len = (u16)tail;
                }
                tbt_rx_fec_deliver(st, seq * rx->s,
                                   b->data + (size_t)p * rx->s, len);
                delivered = true;
            }
        }
        rx->next_super++;
        rx->next_deliver = (s + 1) * (u64)rx->groups * rx->n;
        if (rx->total_known && rx->next_deliver > rx->total_pkts)
            rx->next_deliver = rx->total_pkts;
    }
    if (delivered && rx->next_deliver > rx->ack_deliver)
    {
        rx->ack_deliver = rx->next_deliver;
        tbt_fec_ack(sess, rx->next_deliver);
    }
    tbt_stream_put(st);
}

/* Re-run decode over every pending block. Called when the stream's total
 * length becomes known (LAST or FIN) or when the tail length of an existing
 * block changes; the walk is bounded by groups * slots and runs once per
 * stream end, not per record. */
static void tbt_fec_rx_retry_all(struct tbt_session *sess)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    u16 group, i;

    for (group = 0; group < rx->groups; group++)
    {
        for (i = 0; i < rx->slots; i++)
        {
            struct tbt_fec_blk *b = &rx->blk[(size_t)group * rx->slots + i];

            if (!b->used || b->ready)
                continue;
            if (tbt_fec_blk_decode(sess, group, b) == 0)
                b->ready = true;
        }
    }
    tbt_fec_deliver_ready(sess);
}

int tbt_fec_rx_record(struct tbt_carrier *c, u8 *rec, size_t len)
{
    struct tbt_session *sess = c->sess;
    struct tbt_fec_rx *rx = sess->fec_rx;
    struct tbt_fec_hdr h;
    struct tbt_fec_blk *b;
    const u8 *payload;
    u16 shares, group, share, slot;
    bool new_total = false;

    if (!rx || len < TBT_FEC_HDR_LEN)
        return -EPROTO;
    if (tbt_fec_hdr_unpack(rec, &h))
        return -EPROTO;
    if (tbt_fec_hdr_check(&h, TBT_FEC_S))
        return -EPROTO;
    {
        u8 hb[TBT_FEC_HDR_LEN];
        u32 want;

        tbt_fec_hdr_pack(&h, hb);
        want = tbt_fec_crc32c(0, hb, TBT_FEC_H_HDRCRC);
        if (want != h.header_crc32c)
            return -EPROTO;
        /* Header framing is trusted here; decrypt DATA/PARITY before the
         * payload CRC, which covers the clear bytes (T0 §1.3, §2.2). The rx
         * context is shared by carriers and body_xor is read-only. */
        if (sess->crypto &&
            (h.type == TBT_FEC_DATA || h.type == TBT_FEC_PARITY))
        {
            if (!smp_load_acquire(&sess->crypto_ready))
                return 0;
            if (tbt_crypto_body_xor_share(&sess->crypto_ctx.rx, h.seq,
                                          h.type == TBT_FEC_PARITY,
                                          h.share_index,
                                          rec + TBT_FEC_HDR_LEN,
                                          h.payload_len))
                return 0; /* counter overflow: drop this share */
        }
        if (tbt_fec_crc32c(0, rec + TBT_FEC_HDR_LEN, h.payload_len) !=
            h.payload_crc32c)
            return 0; /* payload CRC failure drops this record only */
    }
    if (h.type == TBT_FEC_ACK)
    {
        struct tbt_fec_ack a;
        struct tbt_stream *st;

        if (tbt_fec_ack_unpack(rec + TBT_FEC_HDR_LEN, &a))
            return 0;
        st = tbt_fec_stream(sess);
        if (st)
        {
            u64 acked = a.next_deliver * TBT_FEC_S;

            if (acked > READ_ONCE(st->tx_seq))
                acked = READ_ONCE(st->tx_seq);
            spin_lock(&sess->lock);
            if (acked > READ_ONCE(st->tx_ack))
            {
                WRITE_ONCE(st->tx_ack, acked);
                st->last_ack_ns = ktime_get_ns();
                sess->tx_ack = acked;
                sess->last_ack_ns = st->last_ack_ns;
            }
            spin_unlock(&sess->lock);
            tbt_stream_put(st);
            /* The FIN pump holds the close until tx_ack covers tx_seq. */
            tbt_tx_wake(sess);
        }
        return 0;
    }
    if (h.type != TBT_FEC_DATA && h.type != TBT_FEC_PARITY)
        return 0; /* FBK/CFG are accepted and ignored by this cut. */

    /* One share per carrier: the carrier names the group and the share, and
     * the header must agree. A replaced carrier keeps its index, so this is
     * also the guard against a record arriving on the wrong path. */
    shares = (u16)(rx->n + rx->m);
    group = (u16)(c->index / shares);
    share = (u16)(c->index % shares);
    if (!shares || group >= rx->groups || h.share_index != share)
        return -EPROTO;
    if (h.n != (u8)rx->n || h.m != (u8)rx->m)
        return -EPROTO;
    if ((u16)(h.seq % rx->groups) != group)
        return -EPROTO;
    if (h.block_id != (h.seq / rx->groups) / rx->n)
        return -EPROTO;
    if (h.type == TBT_FEC_PARITY &&
        (!h.win_len || h.win_len > rx->n))
        return -EPROTO;

    payload = rec + TBT_FEC_HDR_LEN;
    spin_lock_bh(&rx->lock);
    if (h.block_id < rx->next_super ||
        h.block_id >= rx->next_super + rx->slots)
    {
        spin_unlock_bh(&rx->lock);
        return 0; /* already delivered, or too far ahead to buffer */
    }
    slot = (u16)((size_t)group * rx->slots + (h.block_id % rx->slots));
    b = &rx->blk[slot];
    if (!b->used || b->block_id != h.block_id)
        tbt_fec_blk_reset(b, h.block_id);

    if (h.type == TBT_FEC_DATA)
    {
        memcpy(b->data + (size_t)share * rx->s, payload, h.payload_len);
        b->dlen[share] = h.payload_len;
        b->dpresent[share] = 1;
        if (h.flags & TBT_FEC_FF_LAST)
        {
            if (!rx->total_known)
            {
                rx->total_pkts = h.seq + 1;
                rx->total_known = true;
                new_total = true;
            }
        }
    }
    else
    {
        u16 idx = (u16)(h.share_index - rx->n);

        memcpy(b->parity + (size_t)idx * rx->s, payload, h.payload_len);
        b->ppresent[idx] = 1;
        if (h.win_len < rx->n &&
            (!b->nreal || h.win_len < b->nreal))
            b->nreal = h.win_len;
    }
    if (new_total)
        tbt_fec_rx_retry_all(sess);
    else if (tbt_fec_blk_decode(sess, group, b) == 0)
    {
        b->ready = true;
        tbt_fec_deliver_ready(sess);
    }
    spin_unlock_bh(&rx->lock);
    return 0;
}

void tbt_fec_rx_fin(struct tbt_session *sess, u64 total_bytes)
{
    struct tbt_fec_rx *rx = sess->fec_rx;

    if (!rx)
        return;
    spin_lock_bh(&rx->lock);
    if (!rx->total_known)
    {
        rx->total_bytes = total_bytes;
        rx->total_pkts = total_bytes ?
            (total_bytes + rx->s - 1) / rx->s : 0;
        rx->total_known = true;
        tbt_fec_rx_retry_all(sess);
    }
    else if (!rx->total_bytes)
    {
        rx->total_bytes = total_bytes;
    }
    spin_unlock_bh(&rx->lock);
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
    tbt_fec_grouped_free(rx->codec);
    kvfree(rx);
    sess->fec_rx = NULL;
}

void tbt_fec_data_free(struct tbt_session *sess)
{
    struct tbt_fec_tx *tx = sess->fec_tx;
    u16 i;

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

    if (!n || !m || (u16)n + (u16)m > TBT_FEC_MAX_SHARES)
        return -EINVAL;
    if (t->cfg.mode == 3)
        return -EOPNOTSUPP; /* SC-LDPC is D's codec, not a silent RLNC alias */
    shares = (u16)(n + m);
    if (!t->cfg.carriers || t->cfg.carriers % shares)
        return -EINVAL;
    groups = (u16)(t->cfg.carriers / shares);
    if (!groups || groups > TBT_FEC_MAX_GROUPS)
        return -EINVAL;
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
    rx->codec = tbt_fec_grouped_new(groups, n, m, n, w, TBT_FEC_S, d,
                                    tbt_fec_base_nonce(sess));
    if (!rx->blk || !rx->codec)
        goto fail;

    tx->ring_size = TBT_FEC_TX_RING;
    tx->nonce = tbt_fec_base_nonce(sess);
    tx->groups = groups;
    tx->n = n;
    tx->m = m;
    tx->s = TBT_FEC_S;
    spin_lock_init(&tx->lock);
    tx->ring = kvzalloc(tx->ring_size, GFP_KERNEL);
    tx->pkt = kvzalloc(TBT_FEC_S, GFP_KERNEL);
    tx->rec = kvzalloc(TBT_FEC_REC_MAX, GFP_KERNEL);
    tx->g = kvzalloc((size_t)groups * sizeof(*tx->g), GFP_KERNEL);
    tx->codec = tbt_fec_grouped_new(groups, n, m, n, w, TBT_FEC_S, d,
                                    tx->nonce);
    if (!tx->ring || !tx->pkt || !tx->rec || !tx->g || !tx->codec)
        goto fail;

    for (i = 0; i < groups; i++)
    {
        tx->g[i].blk = kvzalloc((size_t)n * TBT_FEC_S, GFP_KERNEL);
        tx->g[i].par = kvzalloc((size_t)m * TBT_FEC_S, GFP_KERNEL);
        if (!tx->g[i].blk || !tx->g[i].par)
            goto fail;
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
            goto fail;
    }

    sess->fec_rx = rx;
    sess->fec_tx = tx;
    INIT_LIST_HEAD(&sess->fec_out);
    spin_lock_init(&sess->fec_out_lock);
    INIT_DELAYED_WORK(&sess->w_fec_tx, tbt_fec_tx_work);
    sess->fec = true;
    return 0;

fail:
    sess->fec_rx = rx;
    sess->fec_tx = tx;
    tbt_fec_data_free(sess);
    return -ENOMEM;
}
