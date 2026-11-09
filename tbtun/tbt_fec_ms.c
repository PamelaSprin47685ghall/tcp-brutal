/* tbt_fec_ms.c - stream multiplexing inside one SC-LDPC FEC plane.
 *
 * The host test links this file directly; the module links the same object.
 * Keep it free of stdio, libc allocation, and kernel-only globals so both
 * builds stay honest. See tbt_fec_ms.h for the contract.
 */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/crc32c.h>
#else
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#endif

#include "tbt_fec_ms.h"

/* Room for a full subframe even when the stream ring is otherwise at its
 * flow-control window. */
#define TBT_MS_RING_SLACK (2u * TBT_MS_DATA_MAX)

/* ------------------------------------------------------------------ *
 * Allocation and byte order
 * ------------------------------------------------------------------ */

static void *tbt_ms_alloc(size_t len)
{
#ifdef __KERNEL__
    return kvzalloc(len, GFP_KERNEL);
#else
    return calloc(1, len);
#endif
}

static void tbt_ms_free(void *p)
{
#ifdef __KERNEL__
    kvfree(p);
#else
    free(p);
#endif
}

static void ms_put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void ms_put64(u8 *p, u64 v)
{
    int i;

    for (i = 0; i < 8; i++)
        p[i] = (u8)(v >> (8 * i));
}

static u32 ms_get32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static u64 ms_get64(const u8 *p)
{
    u64 v = 0;
    int i;

    for (i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

/* ------------------------------------------------------------------ *
 * Subframe header
 * ------------------------------------------------------------------ */

int tbt_ms_hdr_pack(const struct tbt_ms_hdr *h, u8 out[TBT_MS_HDR_LEN])
{
    u32 crc;

    if (!h || !out)
        return -EINVAL;
    ms_put32(out + 0, h->magic);
    out[4] = h->ver;
    out[5] = h->type;
    out[6] = (u8)h->flags;
    out[7] = (u8)(h->flags >> 8);
    ms_put32(out + 8, h->len);
    ms_put64(out + 12, h->sid);
    ms_put64(out + 20, h->off);
    crc = tbt_fec_crc32c(0, out, TBT_MS_HDR_LEN - sizeof(u32));
    ms_put32(out + 28, crc);
    return 0;
}

int tbt_ms_hdr_unpack(const u8 in[TBT_MS_HDR_LEN], struct tbt_ms_hdr *h)
{
    if (!in || !h)
        return -EINVAL;
    h->magic = ms_get32(in + 0);
    h->ver = in[4];
    h->type = in[5];
    h->flags = (u16)in[6] | ((u16)in[7] << 8);
    h->len = ms_get32(in + 8);
    h->sid = ms_get64(in + 12);
    h->off = ms_get64(in + 20);
    h->crc32c = ms_get32(in + 28);
    return 0;
}

int tbt_ms_hdr_check(const struct tbt_ms_hdr *h, u32 max_payload)
{
    u8 raw[TBT_MS_HDR_LEN];
    u32 crc;

    if (!h)
        return -EINVAL;
    if (h->magic != TBT_MS_MAGIC || h->ver != TBT_MS_VER || h->flags ||
        !h->sid || h->type < TBT_MS_DATA || h->type > TBT_MS_RST ||
        max_payload > TBT_MS_DATA_MAX)
        return -EPROTO;
    if (h->type == TBT_MS_DATA)
    {
        if (!h->len || h->len > max_payload)
            return -EPROTO;
        if (h->off + h->len < h->off)
            return -EPROTO;
    }
    else if (h->len)
    {
        return -EPROTO;
    }
    if (tbt_ms_hdr_pack(h, raw))
        return -EPROTO;
    crc = ms_get32(raw + 28);
    if (crc != h->crc32c)
        return -EPROTO;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Sender
 * ------------------------------------------------------------------ */

struct tbt_ms_tx_stream
{
    bool used;
    bool fin_queued;
    bool fin_sent;
    bool rst_queued;
    bool rst_sent;
    u64 sid;
    u8 *ring;
    u32 cap;
    u32 credit;
    u64 peer_off;
    u64 sent_off;
    u64 tx_off;
};

struct tbt_ms_tx
{
    u16 max_streams;
    u16 rr;
    u32 block_bytes;
    u32 window;
    u32 quantum;
    u8 *block;
    struct tbt_ms_tx_stream *streams;
};

static struct tbt_ms_tx_stream *tbt_ms_tx_find(struct tbt_ms_tx *tx, u64 sid)
{
    u16 i;

    if (!tx)
        return NULL;
    for (i = 0; i < tx->max_streams; i++)
        if (tx->streams[i].used && tx->streams[i].sid == sid)
            return &tx->streams[i];
    return NULL;
}

static u32 tbt_ms_tx_space1(const struct tbt_ms_tx_stream *s)
{
    u64 queued;
    u32 free_bytes;
    u32 credit_left;

    if (!s->used || s->fin_queued || s->fin_sent || s->rst_queued ||
        s->rst_sent)
        return 0;
    queued = s->sent_off - s->tx_off;
    free_bytes = queued >= s->cap ? 0 : s->cap - (u32)queued;
    /* peer_off never exceeds sent_off: tx_window clamps it. */
    if (s->sent_off - s->peer_off >= s->credit)
        return 0;
    credit_left = s->credit - (u32)(s->sent_off - s->peer_off);
    return credit_left < free_bytes ? credit_left : free_bytes;
}

struct tbt_ms_tx *tbt_ms_tx_new(u16 max_streams, u32 block_bytes, u32 window,
                                u32 quantum)
{
    struct tbt_ms_tx *tx;

    if (!max_streams || max_streams > TBT_MS_MAX_STREAMS)
        return NULL;
    if (block_bytes < TBT_MS_HDR_LEN + TBT_MS_DATA_MAX)
        return NULL;
    if (window < 2u * TBT_MS_DATA_MAX)
        return NULL;
    if (window > 0xffffffffu - TBT_MS_RING_SLACK)
        return NULL;
    tx = tbt_ms_alloc(sizeof(*tx));
    if (!tx)
        return NULL;
    tx->max_streams = max_streams;
    tx->block_bytes = block_bytes;
    tx->window = window;
    tx->quantum = quantum;
    tx->streams = tbt_ms_alloc((size_t)max_streams * sizeof(*tx->streams));
    tx->block = tbt_ms_alloc(block_bytes);
    if (!tx->streams || !tx->block)
    {
        tbt_ms_free(tx->streams);
        tbt_ms_free(tx->block);
        tbt_ms_free(tx);
        return NULL;
    }
    return tx;
}

void tbt_ms_tx_free(struct tbt_ms_tx *tx)
{
    u16 i;

    if (!tx)
        return;
    if (tx->streams)
        for (i = 0; i < tx->max_streams; i++)
            tbt_ms_free(tx->streams[i].ring);
    tbt_ms_free(tx->streams);
    tbt_ms_free(tx->block);
    tbt_ms_free(tx);
}

int tbt_ms_tx_open(struct tbt_ms_tx *tx, u64 sid)
{
    struct tbt_ms_tx_stream *s;
    u16 i, slot = tx ? tx->max_streams : 0;
    u32 cap;

    if (!tx || !sid)
        return -EINVAL;
    if (tbt_ms_tx_find(tx, sid))
        return -EEXIST;
    for (i = 0; i < tx->max_streams; i++)
        if (!tx->streams[i].used)
        {
            slot = i;
            break;
        }
    if (slot == tx->max_streams)
        return -ENOSPC;
    cap = tx->window + TBT_MS_RING_SLACK;
    s = &tx->streams[slot];
    s->ring = tbt_ms_alloc(cap);
    if (!s->ring)
        return -ENOMEM;
    s->used = true;
    s->sid = sid;
    s->cap = cap;
    s->credit = tx->window;
    s->peer_off = 0;
    s->sent_off = 0;
    s->tx_off = 0;
    return 0;
}

int tbt_ms_tx_close(struct tbt_ms_tx *tx, u64 sid)
{
    struct tbt_ms_tx_stream *s = tbt_ms_tx_find(tx, sid);

    if (!s)
        return -ENOENT;
    tbt_ms_free(s->ring);
    memset(s, 0, sizeof(*s));
    return 0;
}

int tbt_ms_tx_fin(struct tbt_ms_tx *tx, u64 sid)
{
    struct tbt_ms_tx_stream *s = tbt_ms_tx_find(tx, sid);

    if (!s)
        return -ENOENT;
    if (s->rst_queued || s->rst_sent || s->fin_sent)
        return 0;
    s->fin_queued = true;
    return 0;
}

int tbt_ms_tx_rst(struct tbt_ms_tx *tx, u64 sid)
{
    struct tbt_ms_tx_stream *s = tbt_ms_tx_find(tx, sid);

    if (!s)
        return -ENOENT;
    if (s->rst_sent)
        return 0;
    /* Bytes the packetizer already packed are on their way; bytes still in
     * the ring are dropped, and the RST names the new tail offset. */
    s->sent_off = s->tx_off;
    s->fin_queued = false;
    s->rst_queued = true;
    return 0;
}

int tbt_ms_tx_window(struct tbt_ms_tx *tx, u64 sid, u64 delivered, u32 credit)
{
    struct tbt_ms_tx_stream *s = tbt_ms_tx_find(tx, sid);
    u64 lim;

    if (!s)
        return -ENOENT;
    lim = s->sent_off;
    if (delivered > lim)
        delivered = lim;
    if (delivered >= s->peer_off)
        s->peer_off = delivered;
    if (credit)
    {
        if (credit > tx->window)
            credit = tx->window;
        s->credit = credit;
    }
    return 0;
}

u32 tbt_ms_tx_space(struct tbt_ms_tx *tx, u64 sid)
{
    struct tbt_ms_tx_stream *s = tbt_ms_tx_find(tx, sid);

    return s ? tbt_ms_tx_space1(s) : 0;
}

u32 tbt_ms_tx_reserve(struct tbt_ms_tx *tx, u64 sid, u32 want, u8 **p)
{
    struct tbt_ms_tx_stream *s = tbt_ms_tx_find(tx, sid);
    u32 avail, idx, contig;

    if (p)
        *p = NULL;
    if (!s || !p || !want)
        return 0;
    avail = tbt_ms_tx_space1(s);
    if (!avail)
        return 0;
    if (avail > want)
        avail = want;
    idx = (u32)(s->sent_off % s->cap);
    contig = s->cap - idx;
    if (avail > contig)
        avail = contig;
    if (!avail)
        return 0;
    *p = s->ring + idx;
    return avail;
}

void tbt_ms_tx_commit(struct tbt_ms_tx *tx, u64 sid, u32 len)
{
    struct tbt_ms_tx_stream *s = tbt_ms_tx_find(tx, sid);
    u32 avail, contig, idx;

    if (!s || !len)
        return;
    avail = tbt_ms_tx_space1(s);
    if (len > avail)
        len = avail;
    idx = (u32)(s->sent_off % s->cap);
    contig = s->cap - idx;
    if (len > contig)
        len = contig;
    s->sent_off += len;
}

u32 tbt_ms_tx_write(struct tbt_ms_tx *tx, u64 sid, const u8 *src, u32 len)
{
    u32 done = 0;

    if (!src)
        return 0;
    while (done < len)
    {
        u8 *p;
        u32 got = tbt_ms_tx_reserve(tx, sid, len - done, &p);

        if (!got)
            break;
        memcpy(p, src + done, got);
        tbt_ms_tx_commit(tx, sid, got);
        done += got;
    }
    return done;
}

static void tbt_ms_ring_copy(const struct tbt_ms_tx_stream *s, u64 off,
                             u8 *dst, u32 len)
{
    u32 idx = (u32)(off % s->cap);
    u32 first = s->cap - idx;

    if (first > len)
        first = len;
    memcpy(dst, s->ring + idx, first);
    if (len > first)
        memcpy(dst + first, s->ring, len - first);
}

static u32 tbt_ms_tx_credit(const struct tbt_ms_tx_stream *s)
{
    u64 limit;
    u64 room;

    if (s->peer_off > ~(u64)0 - (u64)s->credit)
        return 0xffffffffu;
    limit = s->peer_off + (u64)s->credit;
    if (s->tx_off >= limit)
        return 0;
    room = limit - s->tx_off;
    return room > 0xffffffffu ? 0xffffffffu : (u32)room;
}

int tbt_ms_tx_block(struct tbt_ms_tx *tx, const u8 **buf, u32 *len)
{
    u32 used = 0;
    u16 start, i;
    bool progress;

    if (!tx || !buf || !len)
        return -EINVAL;
    *buf = tx->block;
    *len = 0;
    start = tx->rr;
    do
    {
        progress = false;
        for (i = 0; i < tx->max_streams; i++)
        {
            struct tbt_ms_tx_stream *s =
                &tx->streams[(start + i) % tx->max_streams];
            struct tbt_ms_hdr h;
            u32 room, n, credit;

            if (!s->used)
                continue;
            room = tx->block_bytes - used;
            if (room < TBT_MS_HDR_LEN)
                continue;
            if (s->rst_queued)
            {
                h.magic = TBT_MS_MAGIC;
                h.ver = TBT_MS_VER;
                h.type = TBT_MS_RST;
                h.flags = 0;
                h.len = 0;
                h.sid = s->sid;
                h.off = s->tx_off;
                tbt_ms_hdr_pack(&h, tx->block + used);
                used += TBT_MS_HDR_LEN;
                s->rst_queued = false;
                s->rst_sent = true;
                progress = true;
                continue;
            }
            if (s->sent_off != s->tx_off)
            {
                u64 queued = s->sent_off - s->tx_off;

                credit = tbt_ms_tx_credit(s);
                n = tx->quantum;
                if (n > TBT_MS_DATA_MAX)
                    n = TBT_MS_DATA_MAX;
                if ((u64)n > queued)
                    n = (u32)queued;
                if (n > credit)
                    n = credit;
                if (room < TBT_MS_HDR_LEN + 1)
                    n = 0;
                else if (n > room - TBT_MS_HDR_LEN)
                    n = room - TBT_MS_HDR_LEN;
                if (n)
                {
                    h.magic = TBT_MS_MAGIC;
                    h.ver = TBT_MS_VER;
                    h.type = TBT_MS_DATA;
                    h.flags = 0;
                    h.len = n;
                    h.sid = s->sid;
                    h.off = s->tx_off;
                    tbt_ms_hdr_pack(&h, tx->block + used);
                    tbt_ms_ring_copy(s, s->tx_off,
                                     tx->block + used + TBT_MS_HDR_LEN, n);
                    used += TBT_MS_HDR_LEN + n;
                    s->tx_off += n;
                    progress = true;
                    continue;
                }
            }
            if (s->fin_queued && !s->fin_sent && s->sent_off == s->tx_off)
            {
                h.magic = TBT_MS_MAGIC;
                h.ver = TBT_MS_VER;
                h.type = TBT_MS_FIN;
                h.flags = 0;
                h.len = 0;
                h.sid = s->sid;
                h.off = s->sent_off;
                tbt_ms_hdr_pack(&h, tx->block + used);
                used += TBT_MS_HDR_LEN;
                s->fin_queued = false;
                s->fin_sent = true;
                progress = true;
            }
        }
    } while (progress && used < tx->block_bytes);
    if (used)
        tx->rr = (u16)((start + 1) % tx->max_streams);
    *len = used;
    return 0;
}

bool tbt_ms_tx_pending(struct tbt_ms_tx *tx, u64 sid)
{
    struct tbt_ms_tx_stream *s = tbt_ms_tx_find(tx, sid);

    if (!s)
        return false;
    return s->sent_off != s->tx_off || s->fin_queued || s->rst_queued;
}

u32 tbt_ms_tx_queued(struct tbt_ms_tx *tx)
{
    u64 total = 0;
    u16 i;

    if (!tx)
        return 0;
    for (i = 0; i < tx->max_streams; i++)
        if (tx->streams[i].used)
            total += tx->streams[i].sent_off - tx->streams[i].tx_off;
    return total > 0xffffffffu ? 0xffffffffu : (u32)total;
}

bool tbt_ms_tx_has_control(struct tbt_ms_tx *tx)
{
    u16 i;

    if (!tx)
        return false;
    for (i = 0; i < tx->max_streams; i++)
        if (tx->streams[i].used &&
            (tx->streams[i].fin_queued || tx->streams[i].rst_queued))
            return true;
    return false;
}

/* ------------------------------------------------------------------ *
 * Receiver
 * ------------------------------------------------------------------ */

struct tbt_ms_rx_leg
{
    bool used;
    bool fin_seen;
    bool fin_obs;
    bool reset;
    bool broken;
    u64 sid;
    u8 *ring;
    u64 app_off;
    u64 expected_off;
    u64 fin_off;
    u64 last_win_off;
};

struct tbt_ms_rx
{
    u16 max_streams;
    u16 nlegs;
    u16 rr;
    u32 window;
    struct tbt_ms_rx_leg *legs;
    u8 hdr[TBT_MS_HDR_LEN];
    u32 hdr_len;
    u32 pay_left;
    u64 pay_off;
    struct tbt_ms_rx_leg *pay_leg;
    u32 skip_left;
    /* Recently closed stream ids: a late subframe for one must not recreate
     * a leg after its stream detached. The ring is bounded and only ever
     * holds sids, so churn cannot grow memory. */
    u64 closed[TBT_MS_MAX_STREAMS * 2];
    u16 closed_next;
    u16 nclosed;
};

static struct tbt_ms_rx_leg *tbt_ms_rx_find(const struct tbt_ms_rx *rx,
                                            u64 sid)
{
    u16 i;

    if (!rx)
        return NULL;
    for (i = 0; i < rx->max_streams; i++)
        if (rx->legs[i].used && rx->legs[i].sid == sid)
            return &rx->legs[i];
    return NULL;
}

static bool tbt_ms_rx_closed(const struct tbt_ms_rx *rx, u64 sid)
{
    u16 i;

    for (i = 0; i < rx->nclosed; i++)
        if (rx->closed[i] == sid)
            return true;
    return false;
}

static void tbt_ms_rx_closed_add(struct tbt_ms_rx *rx, u64 sid)
{
    if (!sid || tbt_ms_rx_closed(rx, sid))
        return;
    rx->closed[rx->closed_next] = sid;
    rx->closed_next =
        (u16)((rx->closed_next + 1) % (TBT_MS_MAX_STREAMS * 2));
    if (rx->nclosed < TBT_MS_MAX_STREAMS * 2)
        rx->nclosed++;
}

static void tbt_ms_rx_closed_del(struct tbt_ms_rx *rx, u64 sid)
{
    u16 i;

    for (i = 0; i < rx->nclosed; i++)
        if (rx->closed[i] == sid)
            rx->closed[i] = rx->closed[--rx->nclosed];
}

struct tbt_ms_rx *tbt_ms_rx_new(u16 max_streams, u32 window)
{
    struct tbt_ms_rx *rx;

    if (!max_streams || max_streams > TBT_MS_MAX_STREAMS || !window)
        return NULL;
    rx = tbt_ms_alloc(sizeof(*rx));
    if (!rx)
        return NULL;
    rx->legs = tbt_ms_alloc((size_t)max_streams * sizeof(*rx->legs));
    if (!rx->legs)
    {
        tbt_ms_free(rx);
        return NULL;
    }
    rx->max_streams = max_streams;
    rx->window = window;
    return rx;
}

void tbt_ms_rx_free(struct tbt_ms_rx *rx)
{
    u16 i;

    if (!rx)
        return;
    if (rx->legs)
        for (i = 0; i < rx->max_streams; i++)
            tbt_ms_free(rx->legs[i].ring);
    tbt_ms_free(rx->legs);
    tbt_ms_free(rx);
}

static struct tbt_ms_rx_leg *tbt_ms_rx_leg_new(struct tbt_ms_rx *rx, u64 sid)
{
    struct tbt_ms_rx_leg *l;
    u16 i, slot = rx->max_streams;

    for (i = 0; i < rx->max_streams; i++)
        if (!rx->legs[i].used)
        {
            slot = i;
            break;
        }
    if (slot == rx->max_streams)
        return NULL;
    l = &rx->legs[slot];
    l->ring = tbt_ms_alloc(rx->window);
    if (!l->ring)
        return NULL;
    l->used = true;
    l->sid = sid;
    l->app_off = 0;
    l->expected_off = 0;
    l->fin_off = 0;
    l->last_win_off = 0;
    rx->nlegs++;
    return l;
}

int tbt_ms_rx_open(struct tbt_ms_rx *rx, u64 sid)
{
    if (!rx || !sid)
        return -EINVAL;
    if (tbt_ms_rx_find(rx, sid))
        return 0;
    /* An explicit open for this sid is a new stream, not a late frame. */
    tbt_ms_rx_closed_del(rx, sid);
    if (!tbt_ms_rx_leg_new(rx, sid))
        return -ENOSPC;
    return 0;
}

int tbt_ms_rx_close(struct tbt_ms_rx *rx, u64 sid)
{
    struct tbt_ms_rx_leg *l = tbt_ms_rx_find(rx, sid);

    if (!l)
        return -ENOENT;
    tbt_ms_rx_closed_add(rx, sid);
    if (rx->pay_leg == l)
        rx->pay_leg = NULL;
    tbt_ms_free(l->ring);
    memset(l, 0, sizeof(*l));
    rx->nlegs--;
    return 0;
}

static void tbt_ms_leg_store(const struct tbt_ms_rx_leg *l, u32 window,
                             u64 off, const u8 *src, u32 len)
{
    u32 idx = (u32)(off % window);
    u32 first = window - idx;

    if (first > len)
        first = len;
    memcpy(l->ring + idx, src, first);
    if (len > first)
        memcpy(l->ring, src + first, len - first);
}

u32 tbt_ms_rx_feed(struct tbt_ms_rx *rx, const u8 *data, u32 len)
{
    u32 used = 0;

    if (!rx || (!data && len))
        return 0;
    while (used < len)
    {
        struct tbt_ms_hdr h;
        struct tbt_ms_rx_leg *l;
        u32 take, n;

        if (rx->skip_left)
        {
            take = len - used;
            if (take > rx->skip_left)
                take = rx->skip_left;
            used += take;
            rx->skip_left -= take;
            continue;
        }
        if (rx->pay_left)
        {
            take = len - used;
            if (take > rx->pay_left)
                take = rx->pay_left;
            if (rx->pay_leg)
                tbt_ms_leg_store(rx->pay_leg, rx->window, rx->pay_off,
                                 data + used, take);
            rx->pay_off += take;
            rx->pay_left -= take;
            used += take;
            if (!rx->pay_left)
            {
                if (rx->pay_leg)
                    rx->pay_leg->expected_off = rx->pay_off;
                rx->pay_leg = NULL;
            }
            continue;
        }
        n = TBT_MS_HDR_LEN - rx->hdr_len;
        take = len - used;
        if (take > n)
            take = n;
        memcpy(rx->hdr + rx->hdr_len, data + used, take);
        used += take;
        rx->hdr_len += take;
        if (rx->hdr_len < TBT_MS_HDR_LEN)
            break;
        rx->hdr_len = 0;
        if (tbt_ms_hdr_unpack(rx->hdr, &h))
            continue;
        if (tbt_ms_hdr_check(&h, TBT_MS_DATA_MAX))
        {
            /* The header failed its CRC or structure, so neither its sid nor
             * its length can be trusted. Touching a leg named by those bytes
             * would let one corrupt frame break an unrelated stream. Slide one
             * byte instead of skipping a whole candidate: the next real frame
             * may start at any payload offset, and only the true boundary
             * passes magic plus CRC. */
            memmove(rx->hdr, rx->hdr + 1, TBT_MS_HDR_LEN - 1);
            rx->hdr_len = TBT_MS_HDR_LEN - 1;
            continue;
        }
        l = tbt_ms_rx_find(rx, h.sid);
        if (!l && !tbt_ms_rx_closed(rx, h.sid) &&
            rx->nlegs < rx->max_streams)
            l = tbt_ms_rx_leg_new(rx, h.sid);
        if (h.type == TBT_MS_DATA)
        {
            u64 buffered;

            if (!l)
            {
                rx->skip_left = h.len;
                continue;
            }
            if (l->reset || l->broken || l->fin_seen)
            {
                if (l->fin_seen && !l->broken)
                    l->broken = true;
                rx->skip_left = h.len;
                continue;
            }
            if (h.off != l->expected_off)
            {
                l->broken = true;
                rx->skip_left = h.len;
                continue;
            }
            buffered = l->expected_off - l->app_off;
            if (buffered + h.len > rx->window)
            {
                l->broken = true;
                rx->skip_left = h.len;
                continue;
            }
            rx->pay_leg = l;
            rx->pay_off = h.off;
            rx->pay_left = h.len;
            continue;
        }
        if (!l)
            continue;
        if (h.type == TBT_MS_FIN)
        {
            if (l->reset || l->broken)
                continue;
            if (h.off != l->expected_off)
            {
                l->broken = true;
                continue;
            }
            if (!l->fin_seen)
                l->fin_obs = false;
            l->fin_seen = true;
            l->fin_off = h.off;
            continue;
        }
        /* TBT_MS_RST: the reset is authoritative and clears the leg. */
        l->app_off = 0;
        l->expected_off = 0;
        l->fin_seen = false;
        l->fin_obs = false;
        l->fin_off = 0;
        l->last_win_off = 0;
        l->broken = false;
        l->reset = true;
    }
    return used;
}

u32 tbt_ms_rx_avail(struct tbt_ms_rx *rx, u64 sid)
{
    struct tbt_ms_rx_leg *l = tbt_ms_rx_find(rx, sid);
    u64 avail;

    if (!l)
        return 0;
    avail = l->expected_off - l->app_off;
    return avail > 0xffffffffu ? 0xffffffffu : (u32)avail;
}

u64 tbt_ms_rx_delivered(const struct tbt_ms_rx *rx, u64 sid)
{
    const struct tbt_ms_rx_leg *l = tbt_ms_rx_find(rx, sid);

    return l ? l->app_off : 0;
}

int tbt_ms_rx_peek(struct tbt_ms_rx *rx, u64 sid, const u8 **p, u32 max,
                   u32 *len)
{
    struct tbt_ms_rx_leg *l = tbt_ms_rx_find(rx, sid);
    u64 avail;
    u32 idx, n;

    if (!p || !len)
        return -EINVAL;
    *p = NULL;
    *len = 0;
    if (!l)
        return -ENOENT;
    avail = l->expected_off - l->app_off;
    if (avail > max)
        avail = max;
    idx = (u32)(l->app_off % rx->window);
    n = rx->window - idx;
    if (avail > n)
        avail = n;
    *p = l->ring + idx;
    *len = (u32)avail;
    return 0;
}

int tbt_ms_rx_consume(struct tbt_ms_rx *rx, u64 sid, u32 len)
{
    struct tbt_ms_rx_leg *l = tbt_ms_rx_find(rx, sid);

    if (!l)
        return -ENOENT;
    if (len > l->expected_off - l->app_off)
        return -EINVAL;
    l->app_off += len;
    return 0;
}

bool tbt_ms_rx_fin(struct tbt_ms_rx *rx, u64 sid, u64 *end_off)
{
    struct tbt_ms_rx_leg *l = tbt_ms_rx_find(rx, sid);

    if (!l)
        return false;
    if (end_off)
        *end_off = l->fin_off;
    return l->fin_seen && l->app_off == l->fin_off;
}

bool tbt_ms_rx_reset(struct tbt_ms_rx *rx, u64 sid)
{
    struct tbt_ms_rx_leg *l = tbt_ms_rx_find(rx, sid);

    return l ? l->reset : false;
}

bool tbt_ms_rx_broken(struct tbt_ms_rx *rx, u64 sid)
{
    struct tbt_ms_rx_leg *l = tbt_ms_rx_find(rx, sid);

    return l ? l->broken : false;
}

int tbt_ms_rx_win(struct tbt_ms_rx *rx, u64 sid, u64 *delivered, u32 *credit,
                  bool force)
{
    struct tbt_ms_rx_leg *l = tbt_ms_rx_find(rx, sid);
    u64 threshold;

    if (!l || !delivered || !credit)
        return 0;
    *delivered = l->app_off;
    *credit = rx->window;
    if (force)
        return 1;
    threshold = rx->window / 8;
    if (!threshold)
        threshold = 1;
    if (l->app_off < l->last_win_off)
        return 0;
    return l->app_off - l->last_win_off >= threshold;
}

void tbt_ms_rx_win_ack(struct tbt_ms_rx *rx, u64 sid, u64 delivered)
{
    struct tbt_ms_rx_leg *l = tbt_ms_rx_find(rx, sid);

    if (l)
        l->last_win_off = delivered;
}

int tbt_ms_rx_next(struct tbt_ms_rx *rx, u64 *sid)
{
    u16 i;

    if (!rx || !sid)
        return 0;
    for (i = 0; i < rx->max_streams; i++)
    {
        u16 idx = (u16)((rx->rr + i) % rx->max_streams);
        struct tbt_ms_rx_leg *l = &rx->legs[idx];

        if (!l->used)
            continue;
        if (l->expected_off > l->app_off)
        {
            rx->rr = (u16)((idx + 1) % rx->max_streams);
            *sid = l->sid;
            return 1;
        }
        if (l->fin_seen && !l->fin_obs)
        {
            l->fin_obs = true;
            rx->rr = (u16)((idx + 1) % rx->max_streams);
            *sid = l->sid;
            return 1;
        }
    }
    return 0;
}

u16 tbt_ms_rx_streams(const struct tbt_ms_rx *rx)
{
    return rx ? rx->nlegs : 0;
}
