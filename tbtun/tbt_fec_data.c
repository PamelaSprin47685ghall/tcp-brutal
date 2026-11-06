/* tbt_fec_data.c - FEC v6 data plane: packetizer, block codec, record writer.
 *
 * Scope (T2 intermediate):
 *   - one FEC group per session: carriers == n + m, one share per carrier;
 *   - DATA share i rides carrier i, PARITY share n+r rides carrier n+r;
 *   - a block is n DATA packets followed by m parity packets; the codec is the
 *     host-tested GF(2) RLNC in tbt_fec.c, reused with a per-block nonce;
 *   - raw profile only. The HTTP chunk container needs a 40-byte record path
 *     and is left to the integration that adds it; fec=on + http-kal is
 *     refused at session creation rather than half-supported.
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

/* Both ends derive the same coefficients from the configured parameters. The
 * session nonce is not available on the far side until HELLO, and FEC state is
 * built before that; a fixed base keeps sender and receiver aligned. FEC_CFG
 * can carry a negotiated seed later without changing the 40-byte header. */
static u64 tbt_fec_base_nonce(const struct tbt_session *sess)
{
    (void)sess;
    return 0x5a5a5a5a5a5a5a5aULL;
}

static u64 tbt_fec_block_nonce(u64 base, u64 block_id)
{
    return base ^ (block_id * TBT_FEC_NONCE_GOLDEN);
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

static int tbt_fec_out_flush(struct tbt_session *sess)
{
    for (;;)
    {
        struct tbt_fec_out *o;
        int err;

        spin_lock(&sess->fec_out_lock);
        o = list_first_entry_or_null(&sess->fec_out, struct tbt_fec_out, node);
        spin_unlock(&sess->fec_out_lock);
        if (!o)
            return 0;
        err = tbt_fec_out_try(sess, o);
        if (err == -EAGAIN)
            return -EAGAIN;
        spin_lock(&sess->fec_out_lock);
        tbt_fec_out_free(o);
        spin_unlock(&sess->fec_out_lock);
    }
}

/* Queue one complete record. Control records may ride any up carrier; data
 * records are bound to their share's carrier. Returns 0 when queued. */
static int tbt_fec_send_record(struct tbt_session *sess, u16 carrier, u8 *buf,
                               u16 len, bool control)
{
    return tbt_fec_out_push(sess, carrier, buf, len, control);
}

static int tbt_fec_make_record(struct tbt_fec_hdr *h, const u8 *payload, u8 *out,
                               u16 *out_len)
{
    if (tbt_fec_hdr_seal(h, payload))
        return -EINVAL;
    if (tbt_fec_hdr_pack(h, out))
        return -EINVAL;
    if (h->payload_len)
        memcpy(out + TBT_FEC_HDR_LEN, payload, h->payload_len);
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

static void tbt_fec_packetize(struct tbt_session *sess, struct tbt_stream *st,
                              bool last)
{
    struct tbt_fec_tx *tx = sess->fec_tx;
    struct tbt_fec_hdr h;
    u16 rec_len;
    u64 seq, block_id;
    u16 local, share, len;
    u8 *rec = tx->rec;
    int err;

    if (!tx->fill && !last)
        return;
    len = tx->fill;
    seq = tx->seq++;
    local = (u16)(seq % sess->fec_rx->n);
    block_id = seq / sess->fec_rx->n;

    /* DATA share: local index within the block, carrier = share. */
    share = local;
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_DATA;
    h.flags = last ? TBT_FEC_FF_LAST : 0;
    h.share_index = share;
    h.n = (u8)sess->fec_rx->n;
    h.m = (u8)sess->fec_rx->m;
    h.block_id = block_id;
    h.seq = seq;
    h.win_len = 0;
    h.payload_len = len;
    h.conv = TBT_FEC_CONV;
    if (tbt_fec_make_record(&h, tx->pkt, rec, &rec_len))
        return;
    err = tbt_fec_send_record(sess, share, rec, rec_len, false);
    if (err)
        return;

    memcpy(tx->blk + (size_t)local * TBT_FEC_S, tx->pkt, len);
    if (len < TBT_FEC_S)
        memset(tx->blk + (size_t)local * TBT_FEC_S + len, 0, TBT_FEC_S - len);
    tx->fill = 0;

    if (local + 1 != sess->fec_rx->n)
        return;

    /* Block complete: encode m parity rows over the n data packets. */
    {
        u16 i;

        tbt_fec_rlnc_set_nonce(tx->ctx,
                               tbt_fec_block_nonce(tx->nonce, block_id));
        for (i = 0; i < sess->fec_rx->n; i++)
            tx->data_ptrs[i] = tx->blk + (size_t)i * TBT_FEC_S;
        for (i = 0; i < sess->fec_rx->m; i++)
            tx->parity_ptrs[i] = tx->par + (size_t)i * TBT_FEC_S;
        if (tbt_fec_rlnc_encode(tx->ctx, tx->data_ptrs, sess->fec_rx->n,
                                tx->parity_ptrs) < 0)
            return;
        for (i = 0; i < sess->fec_rx->m; i++)
        {
            memset(&h, 0, sizeof(h));
            h.type = TBT_FEC_PARITY;
            h.share_index = (u16)(sess->fec_rx->n + i);
            h.n = (u8)sess->fec_rx->n;
            h.m = (u8)sess->fec_rx->m;
            h.block_id = block_id;
            h.seq = block_id * sess->fec_rx->n + sess->fec_rx->n - 1;
            h.win_len = sess->fec_rx->n;
            h.payload_len = TBT_FEC_S;
            h.conv = TBT_FEC_CONV;
            if (tbt_fec_make_record(&h, tx->parity_ptrs[i], rec, &rec_len))
                continue;
            tbt_fec_send_record(sess, (u16)(sess->fec_rx->n + i), rec, rec_len,
                                false);
        }
    }
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
                tbt_fec_packetize(sess, st, false);
            continue;
        }
        if (tx->fill == TBT_FEC_S)
        {
            tbt_fec_packetize(sess, st, false);
            continue;
        }
        if (READ_ONCE(tx->flush_req) && tx->head == tx->tail)
        {
            if (tx->fill)
                tbt_fec_packetize(sess, st, true);
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
    b->block_id = block_id;
    memset(b->dpresent, 0, TBT_FEC_MAX_SHARES);
    memset(b->ppresent, 0, TBT_FEC_MAX_SHARES);
    memset(b->dlen, 0, TBT_FEC_MAX_SHARES * sizeof(u16));
}

static int tbt_fec_blk_decode(struct tbt_session *sess, struct tbt_fec_blk *b)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    u16 i, missing = 0, have_parity = 0;
    int rc;

    for (i = 0; i < rx->n; i++)
    {
        b->data_ptrs[i] = b->data + (size_t)i * rx->s;
        if (!b->dpresent[i])
            missing++;
    }
    for (i = 0; i < rx->m; i++)
    {
        b->parity_ptrs[i] = b->parity + (size_t)i * rx->s;
        if (b->ppresent[i])
            have_parity++;
    }
    if (!missing)
        return 0;
    if (have_parity < missing)
        return -EAGAIN;
    tbt_fec_rlnc_set_nonce(b->ctx, tbt_fec_block_nonce(tbt_fec_base_nonce(sess),
                                                       b->block_id));
    rc = tbt_fec_rlnc_decode(b->ctx, b->data_ptrs, b->parity_ptrs, b->dpresent,
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
    if (tbt_fec_make_record(&h, body, rec, &len))
        return;
    tbt_fec_send_record(sess, 0, rec, len, true);
}

static void tbt_fec_deliver_one(struct tbt_session *sess, struct tbt_stream *st,
                                struct tbt_fec_blk *b)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    u16 i;

    for (i = 0; i < rx->n; i++)
    {
        u64 seq = b->block_id * rx->n + i;
        u16 len = b->dlen[i] ? b->dlen[i] : rx->s;

        tbt_rx_fec_deliver(st, seq * rx->s, b->data + (size_t)i * rx->s, len);
    }
    b->used = false;
    b->ready = false;
}

/* Deliver completed blocks strictly in block order. A block may finish before
 * an earlier one (carriers run independently); delivering it early would put
 * a hole in the stream's byte space and trip the old hole reset. */
static void tbt_fec_deliver_ready(struct tbt_session *sess)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    struct tbt_stream *st = tbt_fec_stream(sess);

    if (!st)
        return;
    for (;;)
    {
        struct tbt_fec_blk *b = &rx->blk[rx->next_block % rx->slots];

        if (!b->used || !b->ready || b->block_id != rx->next_block)
            break;
        tbt_fec_deliver_one(sess, st, b);
        rx->next_block++;
        rx->next_deliver = rx->next_block * rx->n;
    }
    tbt_fec_ack(sess, rx->next_deliver);
    tbt_stream_put(st);
}

int tbt_fec_rx_record(struct tbt_carrier *c, const u8 *rec, size_t len)
{
    struct tbt_session *sess = c->sess;
    struct tbt_fec_rx *rx = sess->fec_rx;
    struct tbt_fec_hdr h;
    struct tbt_fec_blk *b;
    const u8 *payload;
    u16 slot;

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
            WRITE_ONCE(st->tx_ack, acked);
            tbt_stream_put(st);
        }
        return 0;
    }
    if (h.type != TBT_FEC_DATA && h.type != TBT_FEC_PARITY)
        return 0; /* FBK/CFG are accepted and ignored by this cut. */
    payload = rec + TBT_FEC_HDR_LEN;
    spin_lock_bh(&rx->lock);
    slot = (u16)(h.block_id % rx->slots);
    b = &rx->blk[slot];
    if (!b->used || b->block_id != h.block_id)
        tbt_fec_blk_reset(b, h.block_id);

    if (h.type == TBT_FEC_DATA)
    {
        if (h.share_index >= rx->n)
        {
            spin_unlock_bh(&rx->lock);
            return -EPROTO;
        }
        memcpy(b->data + (size_t)h.share_index * rx->s, payload, h.payload_len);
        b->dlen[h.share_index] = h.payload_len;
        b->dpresent[h.share_index] = 1;
    }
    else
    {
        u16 idx = (u16)(h.share_index - rx->n);

        if (h.share_index < rx->n || idx >= rx->m)
        {
            spin_unlock_bh(&rx->lock);
            return -EPROTO;
        }
        memcpy(b->parity + (size_t)idx * rx->s, payload, h.payload_len);
        b->ppresent[idx] = 1;
    }
    if (tbt_fec_blk_decode(sess, b) == 0)
    {
        b->ready = true;
        tbt_fec_deliver_ready(sess);
    }
    spin_unlock_bh(&rx->lock);
    return 0;
}

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

static void tbt_fec_rx_free(struct tbt_session *sess)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    int i;

    if (!rx)
        return;
    if (!rx->blk)
    {
        kvfree(rx);
        sess->fec_rx = NULL;
        return;
    }
    for (i = 0; i < rx->slots; i++)
    {
        tbt_fec_rlnc_free(rx->blk[i].ctx);
        kvfree(rx->blk[i].data);
        kvfree(rx->blk[i].parity);
        kvfree(rx->blk[i].dpresent);
        kvfree(rx->blk[i].ppresent);
        kvfree(rx->blk[i].dlen);
    }
    kvfree(rx->blk);
    kvfree(rx);
    sess->fec_rx = NULL;
}

void tbt_fec_data_free(struct tbt_session *sess)
{
    struct tbt_fec_tx *tx = sess->fec_tx;

    if (tx)
    {
        tbt_fec_rlnc_free(tx->ctx);
        kvfree(tx->ring);
        kvfree(tx->pkt);
        kvfree(tx->par);
        kvfree(tx->rec);
        kvfree(tx->blk);
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
    u16 n = t->cfg.n, m = t->cfg.m, d;
    u32 per, slots;
    int i;

    if (!n || !m || (u16)n + (u16)m > TBT_FEC_MAX_SHARES)
        return -EINVAL;
    if (t->cfg.carriers != (u16)(n + m))
        return -EOPNOTSUPP; /* grouped data plane is not in this cut */
    d = (u16)(n / 2);
    if (!d)
        d = 1;
    if (d >= n && n > 1)
        d = (u16)(n - 1);

    per = (u32)(n + m) * TBT_FEC_S;
    slots = (16u * 1024 * 1024) / per;
    if (slots < 8)
        slots = 8;
    if (slots > 256)
        slots = 256;
    rx = kzalloc(sizeof(*rx), GFP_KERNEL);
    tx = kzalloc(sizeof(*tx), GFP_KERNEL);
    if (!rx || !tx)
        goto fail;
    spin_lock_init(&rx->lock);
    rx->n = n;
    rx->m = m;
    rx->s = TBT_FEC_S;
    rx->slots = (u16)slots;
    rx->blk = kvzalloc((size_t)slots * sizeof(*rx->blk), GFP_KERNEL);
    if (!rx->blk)
        goto fail;
    tx->ring_size = TBT_FEC_TX_RING;
    tx->nonce = tbt_fec_base_nonce(sess);
    spin_lock_init(&tx->lock);
    tx->ring = kvzalloc(tx->ring_size, GFP_KERNEL);
    tx->pkt = kvzalloc(TBT_FEC_S, GFP_KERNEL);
    tx->par = kvzalloc((size_t)m * TBT_FEC_S, GFP_KERNEL);
    tx->rec = kvzalloc(TBT_FEC_REC_MAX, GFP_KERNEL);
    tx->blk = kvzalloc((size_t)n * TBT_FEC_S, GFP_KERNEL);
    tx->ctx = tbt_fec_rlnc_new(n, m, n, n, TBT_FEC_S, d, tx->nonce);
    if (!tx->ring || !tx->pkt || !tx->par || !tx->rec || !tx->blk || !tx->ctx)
        goto fail;

    for (i = 0; i < rx->slots; i++)
    {
        struct tbt_fec_blk *b = &rx->blk[i];

        b->data = kvzalloc((size_t)n * TBT_FEC_S, GFP_KERNEL);
        b->parity = kvzalloc((size_t)m * TBT_FEC_S, GFP_KERNEL);
        b->dpresent = kvzalloc(TBT_FEC_MAX_SHARES, GFP_KERNEL);
        b->ppresent = kvzalloc(TBT_FEC_MAX_SHARES, GFP_KERNEL);
        b->dlen = kvzalloc(TBT_FEC_MAX_SHARES * sizeof(u16), GFP_KERNEL);
        b->ctx = tbt_fec_rlnc_new(n, m, n, n, TBT_FEC_S, d, tx->nonce);
        if (!b->data || !b->parity || !b->dpresent || !b->ppresent ||
            !b->dlen || !b->ctx)
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
