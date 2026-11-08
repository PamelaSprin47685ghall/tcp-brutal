/* tbt_fec.c - FEC wire v1 and the GF(2) sliding-window RLNC codec.
 *
 * The host test links this file directly; the module links the same object.
 * Keep it free of stdio, libc allocation, and kernel-only globals so both
 * builds stay honest. See tbt_fec.h for the contract.
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

#include "tbt_fec.h"

/* ------------------------------------------------------------------ *
 * Allocation
 * ------------------------------------------------------------------ */

static void *tbt_fec_alloc(size_t len)
{
#ifdef __KERNEL__
    return kvzalloc(len, GFP_KERNEL);
#else
    return calloc(1, len);
#endif
}

static void tbt_fec_free(void *p)
{
#ifdef __KERNEL__
    kvfree(p);
#else
    free(p);
#endif
}

/* ------------------------------------------------------------------ *
 * CRC32C (Castagnoli). Standard reflected form: init ~0, final xor.
 * ------------------------------------------------------------------ */

#ifdef __KERNEL__

u32 tbt_fec_crc32c(u32 seed, const void *data, size_t len)
{
    if (!len)
        return seed;
    return crc32c(~seed, data, len) ^ ~0u;
}

#else

static u32 tbt_crc32c_table[256];
static int tbt_crc32c_ready;

static void tbt_crc32c_init(void)
{
    u32 i;

    for (i = 0; i < 256; i++)
    {
        u32 c = i;
        int k;

        for (k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0x82f63b78u : c >> 1;
        tbt_crc32c_table[i] = c;
    }
    tbt_crc32c_ready = 1;
}

u32 tbt_fec_crc32c(u32 seed, const void *data, size_t len)
{
    const u8 *p = data;
    u32 crc = ~seed;

    if (!len)
        return seed;
    if (!tbt_crc32c_ready)
        tbt_crc32c_init();
    while (len--)
        crc = tbt_crc32c_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

#endif

/* ------------------------------------------------------------------ *
 * Little-endian field helpers. The wire is little-endian regardless of the
 * host byte order; every field is read and written one byte at a time.
 * ------------------------------------------------------------------ */

static void tbt_put_u16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void tbt_put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void tbt_put_u64(u8 *p, u64 v)
{
    tbt_put_u32(p, (u32)v);
    tbt_put_u32(p + 4, (u32)(v >> 32));
}

static u16 tbt_get_u16(const u8 *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static u32 tbt_get_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 tbt_get_u64(const u8 *p)
{
    return (u64)tbt_get_u32(p) | ((u64)tbt_get_u32(p + 4) << 32);
}

/* ------------------------------------------------------------------ *
 * 48-byte header
 * ------------------------------------------------------------------ */

int tbt_fec_hdr_pack(const struct tbt_fec_hdr *h, u8 out[TBT_FEC_HDR_LEN])
{
    if (!h || !out)
        return -EINVAL;
    out[TBT_FEC_H_TYPE] = h->type;
    out[TBT_FEC_H_FLAGS] = h->flags;
    tbt_put_u16(out + TBT_FEC_H_SHARE, h->share_index);
    out[TBT_FEC_H_N] = h->n;
    out[TBT_FEC_H_M] = h->m;
    tbt_put_u16(out + TBT_FEC_H_EPOCH, h->epoch);
    tbt_put_u64(out + TBT_FEC_H_BLOCK, h->block_id);
    tbt_put_u64(out + TBT_FEC_H_SEQ, h->seq);
    tbt_put_u16(out + TBT_FEC_H_WINLEN, h->win_len);
    tbt_put_u16(out + TBT_FEC_H_PAYLEN, h->payload_len);
    tbt_put_u16(out + TBT_FEC_H_CONV, h->conv);
    tbt_put_u16(out + TBT_FEC_H_RESERVED, h->reserved);
    tbt_put_u64(out + TBT_FEC_H_SID, h->sid);
    tbt_put_u32(out + TBT_FEC_H_PAYCRC, h->payload_crc32c);
    tbt_put_u32(out + TBT_FEC_H_HDRCRC, h->header_crc32c);
    return 0;
}

int tbt_fec_hdr_unpack(const u8 in[TBT_FEC_HDR_LEN], struct tbt_fec_hdr *h)
{
    if (!in || !h)
        return -EINVAL;
    h->type = in[TBT_FEC_H_TYPE];
    h->flags = in[TBT_FEC_H_FLAGS];
    h->share_index = tbt_get_u16(in + TBT_FEC_H_SHARE);
    h->n = in[TBT_FEC_H_N];
    h->m = in[TBT_FEC_H_M];
    h->epoch = tbt_get_u16(in + TBT_FEC_H_EPOCH);
    h->block_id = tbt_get_u64(in + TBT_FEC_H_BLOCK);
    h->seq = tbt_get_u64(in + TBT_FEC_H_SEQ);
    h->win_len = tbt_get_u16(in + TBT_FEC_H_WINLEN);
    h->payload_len = tbt_get_u16(in + TBT_FEC_H_PAYLEN);
    h->conv = tbt_get_u16(in + TBT_FEC_H_CONV);
    h->reserved = tbt_get_u16(in + TBT_FEC_H_RESERVED);
    h->sid = tbt_get_u64(in + TBT_FEC_H_SID);
    h->payload_crc32c = tbt_get_u32(in + TBT_FEC_H_PAYCRC);
    h->header_crc32c = tbt_get_u32(in + TBT_FEC_H_HDRCRC);
    return 0;
}

/* Control frames carry no share. DATA and PARITY must name one. */
static bool tbt_fec_is_share_type(u8 type)
{
    return type == TBT_FEC_DATA || type == TBT_FEC_PARITY;
}

static bool tbt_fec_is_control_type(u8 type)
{
    return type == TBT_FEC_ACK || type == TBT_FEC_NACK || type == TBT_FEC_CFG ||
           type == TBT_FEC_FBK;
}

int tbt_fec_hdr_check(const struct tbt_fec_hdr *h, u16 max_payload)
{
    u16 shares;

    if (!h)
        return -EPROTO;
    if (h->conv != TBT_FEC_CONV)
        return -EPROTO;
    if (h->flags & ~TBT_FEC_FF_MASK)
        return -EPROTO;
    if (h->reserved != 0)
        return -EPROTO;
    if (max_payload > TBT_FEC_PAYLOAD_MAX)
        return -EPROTO;
    if (h->payload_len > TBT_FEC_PAYLOAD_MAX)
        return -EPROTO;

    if (tbt_fec_is_share_type(h->type))
    {
        if (!h->n || !h->m)
            return -EPROTO;
        shares = (u16)h->n + (u16)h->m; /* cast first: 256 must not wrap */
        if (shares > TBT_FEC_MAX_SHARES)
            return -EPROTO;
        if (h->share_index >= shares)
            return -EPROTO;
        /* DATA names a data share, PARITY a parity share. */
        if (h->type == TBT_FEC_DATA)
        {
            if (h->share_index >= h->n)
                return -EPROTO;
        }
        else if (h->share_index < h->n)
        {
            return -EPROTO;
        }
        if (!max_payload || h->payload_len > max_payload)
            return -EPROTO;
        if (h->seq >= (1ULL << 63))
            return -EPROTO;
        if (h->type == TBT_FEC_DATA)
        {
            /* A short DATA packet is legal anywhere. The packetizer emits one
             * when the application goes quiet and the packet cannot fill, so
             * an interactive request is not held behind 8192 bytes that never
             * come. The receiver places a share by the sum of the payload
             * lengths before it, not by the packet index times S, so a short
             * share mid-stream is unambiguous; LAST alone marks the end of
             * the stream. */
            if (!h->payload_len || h->payload_len > max_payload)
                return -EPROTO;
            if (h->win_len != 0)
                return -EPROTO;
            /* epoch is block-level and belongs to PARITY only. A DATA record
             * that carried it let a mid-block short share overwrite the
             * block's last-share length, and a share recovered from parity
             * was then delivered with the wrong cut. */
            if (h->epoch != 0)
                return -EPROTO;
        }
        else
        {
            if (h->win_len == 0)
            {
                /* Empty-block marker: no payload, no epoch, parity 0, and no
                 * flags. It is the only way a group says "I have no packet in
                 * this super-block", so the receiver can stop waiting for a
                 * block that will never arrive. */
                if (h->share_index != h->n || h->flags != 0 ||
                    h->payload_len != 0 || h->epoch != 0)
                    return -EPROTO;
            }
            else
            {
                if (h->win_len > h->n)
                    return -EPROTO;
                if (h->payload_len != max_payload)
                    return -EPROTO;
                /* epoch is the length of share win_len-1, never more than S.
                 * The receiver stores it in short_len and uses it as a
                 * delivery length; an unbounded value would read past the
                 * block slot. 0 means that share is full. */
                if (h->epoch > max_payload)
                    return -EPROTO;
            }
        }
        return 0;
    }

    if (!tbt_fec_is_control_type(h->type))
        return -EPROTO;
    if (h->n || h->m || h->share_index || h->win_len)
        return -EPROTO;
    /* The NACK names its super-block in the header. Every other control
     * record leaves block_id zero. */
    if (h->block_id && h->type != TBT_FEC_NACK)
        return -EPROTO;
    /* ACK carries its frontier in header.seq; the others must leave it zero. */
    if ((h->type == TBT_FEC_CFG || h->type == TBT_FEC_FBK ||
         h->type == TBT_FEC_NACK) && h->seq)
        return -EPROTO;

    switch (h->type)
    {
    case TBT_FEC_ACK:
        return h->payload_len == TBT_FEC_ACK_LEN ? 0 : -EPROTO;
    case TBT_FEC_NACK:
        return h->payload_len == TBT_FEC_NACK_LEN ? 0 : -EPROTO;
    case TBT_FEC_CFG:
        return h->payload_len == TBT_FEC_CFG_LEN ? 0 : -EPROTO;
    case TBT_FEC_FBK:
        return h->payload_len == TBT_FEC_FBK_LEN ? 0 : -EPROTO;
    default:
        return -EPROTO;
    }
}

/* Fallback only. A 0x1003 ACK carries deliver_bytes, which is the byte count
 * the receiver actually delivered; this multiplication is kept for a caller
 * that holds no such field and for the host contract tests. It is exact only
 * while every block is full and no block was closed early, because
 * next_deliver is a packet index and a short share or an empty slot leaves a
 * gap between that index and the byte count. */
u64 tbt_fec_ack_bytes(u64 next_deliver, u64 pkt_base, u64 tx_seq)
{
    u64 rel;

    /* Packets before the stream's first one are not this stream's bytes. */
    rel = next_deliver > pkt_base ? next_deliver - pkt_base : 0;
    /* Saturate before multiplying: rel * S would wrap for a hostile frontier
     * even when rel is only one past tx_seq / S. */
    if (rel > tx_seq / TBT_FEC_S)
        return tx_seq;
    rel *= TBT_FEC_S;
    return rel > tx_seq ? tx_seq : rel;
}

int tbt_fec_hdr_seal(struct tbt_fec_hdr *h, const u8 *payload)
{
    u8 buf[TBT_FEC_HDR_LEN];

    if (!h || (h->payload_len && !payload))
        return -EINVAL;
    h->payload_crc32c = tbt_fec_crc32c(0, payload, h->payload_len);
    h->header_crc32c = 0;
    if (tbt_fec_hdr_pack(h, buf))
        return -EINVAL;
    h->header_crc32c = tbt_fec_crc32c(0, buf, TBT_FEC_H_HDRCRC);
    return 0;
}

int tbt_fec_hdr_verify(const struct tbt_fec_hdr *h, const u8 *payload)
{
    u8 buf[TBT_FEC_HDR_LEN];
    u32 want;

    if (!h)
        return -EPROTO;
    if (tbt_fec_hdr_pack(h, buf))
        return -EINVAL;
    want = tbt_fec_crc32c(0, buf, TBT_FEC_H_HDRCRC);
    if (want != h->header_crc32c)
        return -EBADMSG;
    if (h->payload_len && !payload)
        return -EINVAL;
    if (tbt_fec_crc32c(0, payload, h->payload_len) != h->payload_crc32c)
        return -EBADMSG;
    return 0;
}

/* ------------------------------------------------------------------ *
 * 256-bit carrier bitmap
 * ------------------------------------------------------------------ */

void tbt_fec_bitmap_set(u8 bm[TBT_FEC_BITMAP_BYTES], u16 index)
{
    if (!bm || index >= TBT_FEC_MAX_CARRIERS)
        return;
    bm[index >> 3] |= (u8)(1u << (index & 7));
}

void tbt_fec_bitmap_clear(u8 bm[TBT_FEC_BITMAP_BYTES], u16 index)
{
    if (!bm || index >= TBT_FEC_MAX_CARRIERS)
        return;
    bm[index >> 3] &= (u8)~(1u << (index & 7));
}

bool tbt_fec_bitmap_test(const u8 bm[TBT_FEC_BITMAP_BYTES], u16 index)
{
    if (!bm || index >= TBT_FEC_MAX_CARRIERS)
        return false;
    return (bm[index >> 3] & (u8)(1u << (index & 7))) != 0;
}

u16 tbt_fec_bitmap_count(const u8 bm[TBT_FEC_BITMAP_BYTES])
{
    u16 i;
    u16 n = 0;

    if (!bm)
        return 0;
    for (i = 0; i < TBT_FEC_BITMAP_BYTES; i++)
    {
        u8 v = bm[i];

        while (v)
        {
            n += (u16)(v & 1);
            v >>= 1;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ *
 * Control bodies
 * ------------------------------------------------------------------ */

int tbt_fec_ack_pack(const struct tbt_fec_ack *a, u8 out[TBT_FEC_ACK_LEN])
{
    if (!a || !out)
        return -EINVAL;
    tbt_put_u64(out + 0, a->next_deliver);
    tbt_put_u32(out + 8, a->credit_packets);
    tbt_put_u32(out + 12, a->rtt_us);
    tbt_put_u64(out + 16, a->deliver_bytes);
    return 0;
}

int tbt_fec_ack_unpack(const u8 in[TBT_FEC_ACK_LEN], struct tbt_fec_ack *a)
{
    if (!in || !a)
        return -EINVAL;
    a->next_deliver = tbt_get_u64(in + 0);
    a->credit_packets = tbt_get_u32(in + 8);
    a->rtt_us = tbt_get_u32(in + 12);
    a->deliver_bytes = tbt_get_u64(in + 16);
    return 0;
}

int tbt_fec_nack_pack(const struct tbt_fec_nack *n, u8 out[TBT_FEC_NACK_LEN])
{
    if (!n || !out)
        return -EINVAL;
    memcpy(out, n->group_mask, TBT_FEC_BITMAP_BYTES);
    memcpy(out + TBT_FEC_BITMAP_BYTES, n->share_mask, TBT_FEC_BITMAP_BYTES);
    return TBT_FEC_NACK_LEN;
}

int tbt_fec_nack_unpack(const u8 in[TBT_FEC_NACK_LEN], struct tbt_fec_nack *n)
{
    if (!in || !n)
        return -EPROTO;
    memcpy(n->group_mask, in, TBT_FEC_BITMAP_BYTES);
    memcpy(n->share_mask, in + TBT_FEC_BITMAP_BYTES, TBT_FEC_BITMAP_BYTES);
    return 0;
}

/* ------------------------------------------------------------------ *
 * Bounded replay ring
 * ------------------------------------------------------------------ */

struct tbt_fec_replay
{
    u16 groups, n, m, s, k;
    u16 stride; /* TBT_FEC_HDR_LEN + s */
    u32 slots;  /* k * groups * (n + m) */
    u32 bslots; /* k * groups: one resident-block id per (slot, group) */
    u64 *block_id;
    u64 *block_gen; /* block id of the resident block, or ~0 */
    /* When a share was last queued as a replay. The NACK handler stamps it
     * and skips a share whose reply is still in flight; storage and this
     * stamp are both protected by the caller's TX replay lock. */
    u64 *sent_ns;
    u16 *len;
    u8 *rec;
};

u16 tbt_fec_replay_k(const struct tbt_fec_replay *r)
{
    return r ? r->k : 0;
}

size_t tbt_fec_replay_bytes(u16 groups, u16 n, u16 m, u16 s, u16 k)
{
    size_t shares = (size_t)n + m;
    size_t stride = TBT_FEC_HDR_LEN + s;
    size_t per = stride + 2 * sizeof(u64) + sizeof(u16);

    if (!groups || !shares || !k || !s)
        return 0;
    return sizeof(struct tbt_fec_replay) +
           (size_t)k * groups * shares * per +
           (size_t)k * groups * sizeof(u64);
}

u16 tbt_fec_replay_k_for(u16 groups, u16 n, u16 m, u16 s)
{
    u16 k = TBT_FEC_REPLAY_K_MAX;

    if (!groups || groups > TBT_FEC_MAX_GROUPS || !n || !m ||
        (u32)n + m > TBT_FEC_MAX_SHARES || !s || s > TBT_FEC_PAYLOAD_MAX)
        return 0;
    /* Start at the cap and shrink until the ring fits the budget. A small
     * shape therefore gets the largest ring the budget allows (32 carriers
     * K=59, 64 carriers K=29) instead of the old fixed K=7; the 256-carrier
     * shape still lands on K=7. A shape that cannot hold one super-block
     * gets no ring; the data plane then relies on parity and the carrier TCP
     * retransmission. */
    while (k > 0 && tbt_fec_replay_bytes(groups, n, m, s, k) >
                        TBT_FEC_REPLAY_BUDGET)
        k--;
    return k;
}

struct tbt_fec_replay *tbt_fec_replay_new(u16 groups, u16 n, u16 m, u16 s)
{
    struct tbt_fec_replay *r;
    size_t shares = (size_t)n + m;
    size_t stride, slots;
    u16 k;
    u32 i;

    if (!groups || !shares || !s || s > TBT_FEC_PAYLOAD_MAX)
        return NULL;
    stride = TBT_FEC_HDR_LEN + s;
    k = tbt_fec_replay_k_for(groups, n, m, s);
    if (!k)
        return NULL;
    slots = (size_t)k * groups * shares;
    if (slots > 0xffffffffu)
        return NULL;
    r = tbt_fec_alloc(sizeof(*r));
    if (!r)
        return NULL;
    r->groups = groups;
    r->n = n;
    r->m = m;
    r->s = s;
    r->k = k;
    r->stride = (u16)stride;
    r->slots = (u32)slots;
    r->bslots = (u32)((size_t)k * groups);
    r->block_id = tbt_fec_alloc(slots * sizeof(*r->block_id));
    r->block_gen = tbt_fec_alloc((size_t)r->bslots * sizeof(*r->block_gen));
    r->sent_ns = tbt_fec_alloc(slots * sizeof(*r->sent_ns));
    r->len = tbt_fec_alloc(slots * sizeof(*r->len));
    r->rec = tbt_fec_alloc(slots * stride);
    if (!r->block_id || !r->block_gen || !r->sent_ns || !r->len || !r->rec)
    {
        tbt_fec_replay_free(r);
        return NULL;
    }
    for (i = 0; i < r->slots; i++)
        r->block_id[i] = ~0ULL;
    for (i = 0; i < r->bslots; i++)
        r->block_gen[i] = ~0ULL;
    return r;
}

void tbt_fec_replay_free(struct tbt_fec_replay *r)
{
    if (!r)
        return;
    tbt_fec_free(r->block_id);
    tbt_fec_free(r->block_gen);
    tbt_fec_free(r->sent_ns);
    tbt_fec_free(r->len);
    tbt_fec_free(r->rec);
    tbt_fec_free(r);
}

static u32 tbt_fec_replay_slot(const struct tbt_fec_replay *r, u64 block_id,
                               u16 group, u16 share)
{
    u64 b = block_id % r->k;
    u16 shares = (u16)(r->n + r->m);

    return (u32)((b * r->groups + group) * shares + share);
}

static u32 tbt_fec_replay_bslot(const struct tbt_fec_replay *r, u64 block_id,
                                u16 group)
{
    return (u32)((block_id % r->k) * r->groups + group);
}

int tbt_fec_replay_store(struct tbt_fec_replay *r, u64 block_id, u16 group,
                         u16 share, const u8 *rec, u16 len)
{
    u32 slot;

    if (!r || !rec || group >= r->groups || share >= (u16)(r->n + r->m) ||
        len > r->stride)
        return -EINVAL;
    slot = tbt_fec_replay_slot(r, block_id, group, share);
    memcpy(r->rec + (size_t)slot * r->stride, rec, len);
    r->block_id[slot] = block_id;
    r->block_gen[tbt_fec_replay_bslot(r, block_id, group)] = block_id;
    r->sent_ns[slot] = 0; /* a fresh share has no replay in flight */
    r->len[slot] = len;
    return 0;
}

int tbt_fec_replay_get(const struct tbt_fec_replay *r, u64 block_id,
                       u16 group, u16 share, u8 *out, u16 cap)
{
    u32 slot;
    u16 len;

    if (!r || !out || group >= r->groups || share >= (u16)(r->n + r->m))
        return -EINVAL;
    slot = tbt_fec_replay_slot(r, block_id, group, share);
    if (r->block_gen[tbt_fec_replay_bslot(r, block_id, group)] != block_id)
        return -ESTALE; /* the block was evicted: the slot holds another */
    if (r->block_id[slot] != block_id)
        return -ENODATA; /* the block is resident, this share never stored */
    len = r->len[slot];
    if (!len)
        return -ENODATA; /* the block is resident, this share never stored */
    if (cap < len)
        return -ENOSPC;
    memcpy(out, r->rec + (size_t)slot * r->stride, len);
    return len;
}

bool tbt_fec_replay_has(const struct tbt_fec_replay *r, u64 block_id,
                        u16 group, u16 share)
{
    u32 slot;

    if (!r || group >= r->groups || share >= (u16)(r->n + r->m))
        return false;
    slot = tbt_fec_replay_slot(r, block_id, group, share);
    return r->block_gen[tbt_fec_replay_bslot(r, block_id, group)] == block_id &&
           r->block_id[slot] == block_id && r->len[slot] != 0;
}

u64 tbt_fec_replay_sent_ns(const struct tbt_fec_replay *r, u64 block_id,
                           u16 group, u16 share)
{
    u32 slot;

    if (!r || group >= r->groups || share >= (u16)(r->n + r->m))
        return 0;
    slot = tbt_fec_replay_slot(r, block_id, group, share);
    if (r->block_gen[tbt_fec_replay_bslot(r, block_id, group)] != block_id ||
        r->block_id[slot] != block_id)
        return 0;
    return r->sent_ns[slot];
}

void tbt_fec_replay_mark_sent(struct tbt_fec_replay *r, u64 block_id,
                              u16 group, u16 share, u64 now_ns)
{
    u32 slot;

    if (!r || group >= r->groups || share >= (u16)(r->n + r->m))
        return;
    slot = tbt_fec_replay_slot(r, block_id, group, share);
    if (r->block_gen[tbt_fec_replay_bslot(r, block_id, group)] != block_id ||
        r->block_id[slot] != block_id || !r->len[slot])
        return;
    r->sent_ns[slot] = now_ns;
}

void tbt_fec_replay_reset(struct tbt_fec_replay *r)
{
    u32 i;

    if (!r)
        return;
    for (i = 0; i < r->slots; i++)
    {
        r->block_id[i] = ~0ULL;
        r->sent_ns[i] = 0;
        r->len[i] = 0;
    }
    for (i = 0; i < r->bslots; i++)
        r->block_gen[i] = ~0ULL;
}

/* ------------------------------------------------------------------ *
 * NACK pacing
 * ------------------------------------------------------------------ */

static bool tbt_fec_nack_same(const struct tbt_fec_nack_state *st,
                              u64 block_id, const struct tbt_fec_nack *n)
{
    return st->valid && st->block_id == block_id &&
           !memcmp(st->group_mask, n->group_mask, TBT_FEC_BITMAP_BYTES) &&
           !memcmp(st->share_mask, n->share_mask, TBT_FEC_BITMAP_BYTES);
}

bool tbt_fec_nack_due_at(struct tbt_fec_nack_state *st, u64 now_ns,
                         u64 block_id, const struct tbt_fec_nack *n,
                         u32 retry_ms)
{
    u64 interval;

    if (!st || !n)
        return false;
    if (!tbt_fec_nack_same(st, block_id, n))
    {
        interval = (u64)TBT_FEC_NACK_MIN_MS * 1000000ULL;
        if (st->valid && now_ns - st->last_ns < interval)
            return false;
        st->block_id = block_id;
        memcpy(st->group_mask, n->group_mask, TBT_FEC_BITMAP_BYTES);
        memcpy(st->share_mask, n->share_mask, TBT_FEC_BITMAP_BYTES);
        st->first_ns = now_ns;
        st->last_ns = now_ns;
        st->tries = 1;
        st->valid = true;
        return true;
    }
    /* An identical NACK is a retry: the previous one may have been lost.
     * There is no total cap; the replay window and the carrier TCP lifetime
     * bound the repair, and the caller keeps the rate. */
    if (retry_ms < TBT_FEC_NACK_RETRY_MS)
        retry_ms = TBT_FEC_NACK_RETRY_MS;
    interval = (u64)retry_ms * 1000000ULL;
    if (now_ns - st->last_ns < interval)
        return false;
    st->last_ns = now_ns;
    st->tries++;
    return true;
}

void tbt_fec_nack_reset(struct tbt_fec_nack_state *st)
{
    if (st)
        memset(st, 0, sizeof(*st));
}

int tbt_fec_cfg_pack(const struct tbt_fec_cfg *c, u8 out[TBT_FEC_CFG_LEN])
{
    if (!c || !out)
        return -EINVAL;
    memset(out, 0, TBT_FEC_CFG_LEN);
    tbt_put_u32(out + TBT_FEC_CFG_EPOCH, c->epoch);
    tbt_put_u16(out + TBT_FEC_CFG_N, c->n);
    tbt_put_u16(out + TBT_FEC_CFG_M, c->m);
    tbt_put_u16(out + TBT_FEC_CFG_W, c->w);
    tbt_put_u16(out + TBT_FEC_CFG_K, c->k);
    out[TBT_FEC_CFG_D] = c->d;
    out[TBT_FEC_CFG_FLAGS] = c->flags;
    tbt_put_u16(out + TBT_FEC_CFG_GROUPS, c->groups);
    tbt_put_u64(out + TBT_FEC_CFG_SWITCH_PKT, c->switch_pkt);
    tbt_put_u64(out + TBT_FEC_CFG_BLOCK_BASE, c->block_base);
    tbt_put_u64(out + TBT_FEC_CFG_SEED, c->seed);
    tbt_put_u32(out + TBT_FEC_CFG_CRC,
                tbt_fec_crc32c(0, out, TBT_FEC_CFG_CRC_BYTES));
    tbt_put_u32(out + TBT_FEC_CFG_RESERVED, 0);
    return 0;
}

int tbt_fec_cfg_unpack(const u8 in[TBT_FEC_CFG_LEN], struct tbt_fec_cfg *c)
{
    u32 crc;
    u32 shares;
    u16 n, m, groups;

    if (!in || !c)
        return -EINVAL;
    /* The CRC covers every field before the reserved word, so verify it
     * before trusting n/m/groups or the switch coordinates. */
    crc = tbt_fec_crc32c(0, in, TBT_FEC_CFG_CRC_BYTES);
    if (crc != tbt_get_u32(in + TBT_FEC_CFG_CRC))
        return -EPROTO;
    if (tbt_get_u32(in + TBT_FEC_CFG_RESERVED) != 0)
        return -EPROTO;
    if (in[TBT_FEC_CFG_FLAGS] & ~TBT_FEC_CFG_F_MASK)
        return -EPROTO;
    c->epoch = tbt_get_u32(in + TBT_FEC_CFG_EPOCH);
    n = tbt_get_u16(in + TBT_FEC_CFG_N);
    m = tbt_get_u16(in + TBT_FEC_CFG_M);
    c->w = tbt_get_u16(in + TBT_FEC_CFG_W);
    c->k = tbt_get_u16(in + TBT_FEC_CFG_K);
    c->d = in[TBT_FEC_CFG_D];
    c->flags = in[TBT_FEC_CFG_FLAGS];
    groups = tbt_get_u16(in + TBT_FEC_CFG_GROUPS);
    c->switch_pkt = tbt_get_u64(in + TBT_FEC_CFG_SWITCH_PKT);
    c->block_base = tbt_get_u64(in + TBT_FEC_CFG_BLOCK_BASE);
    c->seed = tbt_get_u64(in + TBT_FEC_CFG_SEED);
    c->n = n;
    c->m = m;
    c->groups = groups;
    c->crc32c = crc;
    c->reserved = 0;
    shares = (u32)n + m; /* u32 first: 65535 + 65535 must not wrap */
    if (!n || !m || shares > TBT_FEC_MAX_SHARES)
        return -EPROTO;
    if (!c->k || c->w > TBT_FEC_MAX_W)
        return -EPROTO;
    /* Three encodings share this body and (d, k vs w) tells them apart.
     * RLNC has d >= 1, d <= w and k <= w (k is the window's data count).
     * SC-LDPC binary has d == 0, k is the code rate in parts per thousand,
     * and w is the codeword block, so w >= k does not describe it: the
     * downlink is w=256, k=500. SC-LDPC GF(16) has k = codeword block,
     * w = coupling length, d = rate in 1/256, so k > w and d may exceed w.
     * Refusing these pairs here dropped every SC-LDPC CFG before the codec
     * was built; the codec factory stays the authority on the exact set. */
    if (c->d == 0)
    {
        if (c->k < 100 || c->k > 1000)
            return -EPROTO;
    }
    else if (c->w < c->k)
    {
        /* GF(16): k is the codeword block, w the coupling length, d the rate
         * in 1/256 units (its u8 range is already the valid set). The codec's
         * own bounds are the discriminator: a codeword block is at least 16
         * and even, and RLNC never has k > w. */
        if (c->k < 16 || (c->k & 1) || c->w < 2)
            return -EPROTO;
    }
    else
    {
        if (c->d > c->w)
            return -EPROTO;
        if (c->w > 1 && c->d == c->w)
            return -EPROTO;
    }
    /* groups is the carrier geometry: one share per carrier, so n+m must
     * divide the 256 carriers and groups must be exactly the quotient. */
    if (!groups || (TBT_FEC_MAX_CARRIERS % shares) ||
        groups != TBT_FEC_MAX_CARRIERS / shares)
        return -EPROTO;
    return 0;
}

int tbt_fec_fbk_pack(const struct tbt_fec_fbk *f, u8 out[TBT_FEC_FBK_LEN])
{
    if (!f || !out)
        return -EINVAL;
    memset(out, 0, TBT_FEC_FBK_LEN);
    tbt_put_u16(out + TBT_FEC_FBK_EPOCH_LO, f->epoch_lo);
    out[TBT_FEC_FBK_M_CURRENT] = f->m_current;
    out[TBT_FEC_FBK_M_TARGET] = f->m_target;
    tbt_put_u32(out + TBT_FEC_FBK_P_EWMA, f->p_ewma_ppm);
    tbt_put_u16(out + TBT_FEC_FBK_EXPECTED, f->expected_shares);
    tbt_put_u16(out + TBT_FEC_FBK_RECEIVED, f->received_shares);
    tbt_put_u16(out + TBT_FEC_FBK_EPOCH_HI, f->epoch_hi);
    out[TBT_FEC_FBK_FLAGS] = f->flags;
    out[TBT_FEC_FBK_N_CURRENT] = f->n_current;
    memcpy(out + TBT_FEC_FBK_BITMAP, f->carrier_bitmap,
           TBT_FEC_BITMAP_BYTES);
    return 0;
}

int tbt_fec_fbk_unpack(const u8 in[TBT_FEC_FBK_LEN], struct tbt_fec_fbk *f)
{
    if (!in || !f)
        return -EINVAL;
    if (in[TBT_FEC_FBK_FLAGS] & ~TBT_FEC_FBK_F_MASK)
        return -EPROTO;
    f->epoch_lo = tbt_get_u16(in + TBT_FEC_FBK_EPOCH_LO);
    f->m_current = in[TBT_FEC_FBK_M_CURRENT];
    f->m_target = in[TBT_FEC_FBK_M_TARGET];
    f->p_ewma_ppm = tbt_get_u32(in + TBT_FEC_FBK_P_EWMA);
    f->expected_shares = tbt_get_u16(in + TBT_FEC_FBK_EXPECTED);
    f->received_shares = tbt_get_u16(in + TBT_FEC_FBK_RECEIVED);
    f->epoch_hi = tbt_get_u16(in + TBT_FEC_FBK_EPOCH_HI);
    f->flags = in[TBT_FEC_FBK_FLAGS];
    f->n_current = in[TBT_FEC_FBK_N_CURRENT];
    memcpy(f->carrier_bitmap, in + TBT_FEC_FBK_BITMAP,
           TBT_FEC_BITMAP_BYTES);
    return 0;
}

/* ------------------------------------------------------------------ *
 * RLNC codec
 * ------------------------------------------------------------------ */

struct tbt_fec_rlnc
{
    u16 n;
    u16 m;
    u16 k;
    u16 w;
    size_t s;
    u16 d;
    u16 band;
    u64 nonce;

    u32 L;
    u32 n_groups;
    u32 n_parity;

    u8 *data_known;
    u8 *parity_known;

    u32 frontier;
    u32 stuck_at;
    u32 recovered_count;

    u8 **data;
    u8 **parity;

    /* Decode workspace, sized in bind(). */
    u64 *mat;
    u8 *rhs_buf;
    u8 **rhs_rows;
    u32 *unknown;
    int *pos;
    u32 *pivot;
    u32 *idx;
};

static u32 tbt_group_count(u32 L, u16 k)
{
    return L / k + (L % k != 0);
}

static void tbt_xor_into(u8 *dst, const u8 *src, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        dst[i] ^= src[i];
}

/* Row r covers the own-data columns c with (c - r) mod m < band. The band
 * is chosen once in tbt_fec_rlnc_new(): it is the widest one whose rows fit
 * d and whose m x k matrix has rank min(m, k). Every column therefore sits
 * in at least one row (band >= 1), so a single lost share is always covered
 * by a parity row. This replaces the old splitmix selection, which could
 * leave a column in no row at all: with n=4, m=4, w=8, d=4, four random
 * rows missed a given share with probability 1/16, and a block with 3 DATA
 * and all 4 parity stayed undecodable. */
static bool tbt_fec_band_has(u16 m, u16 r, u16 c, u16 band)
{
    return (u16)((c + m - (r % m)) % m) < band;
}

static u32 tbt_fec_band_weight(u16 k, u16 m, u16 r, u16 band)
{
    u32 c;
    u32 n = 0;

    for (c = 0; c < k; c++)
        if (tbt_fec_band_has(m, r, (u16)c, band))
            n++;
    return n;
}

/* Rank of the m x k band matrix over GF(2), capped at min(m, k). */
static u32 tbt_fec_band_rank(u16 k, u16 m, u16 band)
{
    u32 words = ((u32)k + 63) / 64;
    u64 *mat;
    u32 rank = 0;
    u32 col;

    mat = tbt_fec_alloc((size_t)m * words * sizeof(*mat));
    if (!mat)
        return 0;
    memset(mat, 0, (size_t)m * words * sizeof(*mat));
    for (col = 0; col < m; col++)
    {
        u32 c;

        for (c = 0; c < k; c++)
            if (tbt_fec_band_has(m, (u16)col, (u16)c, band))
                mat[(size_t)col * words + (c >> 6)] |= 1ull << (c & 63);
    }
    for (col = 0; col < k && rank < m; col++)
    {
        u32 word = col >> 6;
        u64 bit = 1ull << (col & 63);
        u32 p;
        u32 r;

        for (p = rank; p < m; p++)
            if (mat[(size_t)p * words + word] & bit)
                break;
        if (p == m)
            continue;
        if (p != rank)
        {
            u32 w;

            for (w = 0; w < words; w++)
            {
                u64 t = mat[(size_t)rank * words + w];

                mat[(size_t)rank * words + w] = mat[(size_t)p * words + w];
                mat[(size_t)p * words + w] = t;
            }
        }
        for (r = 0; r < m; r++)
        {
            u32 w;

            if (r == rank || !(mat[(size_t)r * words + word] & bit))
                continue;
            for (w = 0; w < words; w++)
                mat[(size_t)r * words + w] ^= mat[(size_t)rank * words + w];
        }
        rank++;
    }
    tbt_fec_free(mat);
    return rank;
}

/* Widest band that fits d terms per row and keeps rank min(m, k). b == 1 is
 * the partition matrix (column c belongs to row c mod m), which always fits:
 * its row weight is ceil(k/m), at most d in every configured shape. */
static u16 tbt_fec_band_pick(u16 k, u16 m, u16 d)
{
    u32 target = m < k ? m : k;
    u16 band;

    for (band = (u16)(m > 1 ? m - 1 : 1); band > 1; band--)
    {
        u32 r;
        u32 wmax = 0;

        for (r = 0; r < m; r++)
        {
            u32 wt = tbt_fec_band_weight(k, m, (u16)r, band);

            if (wt > wmax)
                wmax = wt;
        }
        if (wmax > d)
            continue;
        if (tbt_fec_band_rank(k, m, band) == target)
            return band;
    }
    return 1;
}

u32 tbt_fec_rlnc_coeffs(const struct tbt_fec_rlnc *ctx, u32 g, u32 r, u32 *idx, u32 max)
{
    u32 half;
    u32 c;
    u32 n = 0;

    if (!ctx || !idx || max < ctx->d)
        return 0;
    (void)g;
    half = ctx->w - ctx->k;
    for (c = 0; c < ctx->k && n < ctx->d; c++)
        if (tbt_fec_band_has(ctx->m, (u16)r, (u16)c, ctx->band))
            idx[n++] = half + c;
    /* Fill the rest of the row with the window's leading positions. The data
     * plane binds L == k, so those positions fall outside [0, L) and both
     * sides skip them; keeping them means a row still names d positions and
     * the wire format is unchanged. */
    for (c = 0; c < half && n < ctx->d; c++)
        idx[n++] = c;
    return n;
}

struct tbt_fec_rlnc *tbt_fec_rlnc_new(u16 n, u16 m, u16 k, u16 w, size_t s, u16 d,
                                      u64 nonce)
{
    struct tbt_fec_rlnc *ctx;

    if (!n || !m || !k || w < k || w > TBT_FEC_MAX_W || !s || !d || d > w ||
        d > TBT_FEC_MAX_D)
        return NULL;
    if ((u16)n + (u16)m > TBT_FEC_MAX_SHARES)
        return NULL;
    /* d == w makes every parity row the all-ones row (rank 1). Refuse it
     * unless the window itself is one packet, where it is the only choice. */
    if (w > 1 && d == w)
        return NULL;
    ctx = tbt_fec_alloc(sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->n = n;
    ctx->m = m;
    ctx->k = k;
    ctx->w = w;
    ctx->s = s;
    ctx->d = d;
    ctx->nonce = nonce;
    ctx->band = tbt_fec_band_pick(k, m, d);
    ctx->idx = tbt_fec_alloc((size_t)w * sizeof(*ctx->idx));
    if (!ctx->idx)
    {
        tbt_fec_rlnc_free(ctx);
        return NULL;
    }
    return ctx;
}

void tbt_fec_rlnc_free(struct tbt_fec_rlnc *ctx)
{
    if (!ctx)
        return;
    tbt_fec_free(ctx->data_known);
    tbt_fec_free(ctx->parity_known);
    tbt_fec_free(ctx->mat);
    tbt_fec_free(ctx->rhs_buf);
    tbt_fec_free(ctx->rhs_rows);
    tbt_fec_free(ctx->unknown);
    tbt_fec_free(ctx->pos);
    tbt_fec_free(ctx->pivot);
    tbt_fec_free(ctx->idx);
    tbt_fec_free(ctx);
}

void tbt_fec_rlnc_set_nonce(struct tbt_fec_rlnc *ctx, u64 nonce)
{
    if (ctx)
        ctx->nonce = nonce;
}

u16 tbt_fec_rlnc_band(const struct tbt_fec_rlnc *ctx)
{
    return ctx ? ctx->band : 0;
}

u64 tbt_fec_rlnc_nonce(const struct tbt_fec_rlnc *ctx)
{
    return ctx ? ctx->nonce : 0;
}

u32 tbt_fec_rlnc_parity_count(const struct tbt_fec_rlnc *ctx, u32 L)
{
    if (!ctx || !L)
        return 0;
    return tbt_group_count(L, ctx->k) * ctx->m;
}

static int tbt_fec_rlnc_bind(struct tbt_fec_rlnc *ctx, u8 *data[], u8 *parity[], u32 L)
{
    u32 groups;
    u32 parity_count;
    u32 words;
    u32 e;

    if (!ctx || !data || !parity || !L || L > TBT_FEC_MAX_W)
        return TBT_FEC_RLNC_ERR_PARAM;
    groups = tbt_group_count(L, ctx->k);
    parity_count = groups * ctx->m;
    words = (L + 63) / 64;

    if (ctx->L != L)
    {
        tbt_fec_free(ctx->data_known);
        tbt_fec_free(ctx->parity_known);
        tbt_fec_free(ctx->mat);
        tbt_fec_free(ctx->rhs_buf);
        tbt_fec_free(ctx->rhs_rows);
        tbt_fec_free(ctx->unknown);
        tbt_fec_free(ctx->pos);
        tbt_fec_free(ctx->pivot);
        ctx->data_known = NULL;
        ctx->parity_known = NULL;
        ctx->mat = NULL;
        ctx->rhs_buf = NULL;
        ctx->rhs_rows = NULL;
        ctx->unknown = NULL;
        ctx->pos = NULL;
        ctx->pivot = NULL;

        ctx->data_known = tbt_fec_alloc(L);
        ctx->parity_known = tbt_fec_alloc(parity_count);
        ctx->mat = tbt_fec_alloc((size_t)parity_count * words * sizeof(u64));
        ctx->rhs_buf = tbt_fec_alloc((size_t)parity_count * ctx->s);
        ctx->rhs_rows = tbt_fec_alloc((size_t)parity_count * sizeof(u8 *));
        ctx->unknown = tbt_fec_alloc((size_t)L * sizeof(u32));
        ctx->pos = tbt_fec_alloc((size_t)L * sizeof(int));
        ctx->pivot = tbt_fec_alloc((size_t)(parity_count < L ? parity_count : L) *
                                   sizeof(u32));
        if (!ctx->data_known || !ctx->parity_known || !ctx->mat || !ctx->rhs_buf ||
            !ctx->rhs_rows || !ctx->unknown || !ctx->pos || !ctx->pivot)
        {
            ctx->L = 0;
            return TBT_FEC_RLNC_ERR_NOMEM;
        }
        for (e = 0; e < parity_count; e++)
            ctx->rhs_rows[e] = ctx->rhs_buf + (size_t)e * ctx->s;
        ctx->L = L;
    }
    else
    {
        memset(ctx->data_known, 0, L);
        memset(ctx->parity_known, 0, parity_count);
    }
    ctx->n_groups = groups;
    ctx->n_parity = parity_count;
    ctx->data = data;
    ctx->parity = parity;
    ctx->frontier = 0;
    ctx->stuck_at = 0;
    ctx->recovered_count = 0;
    return TBT_FEC_RLNC_OK;
}

int tbt_fec_rlnc_encode(struct tbt_fec_rlnc *ctx, u8 *data[], u32 L, u8 *parity[])
{
    u32 groups;
    u32 g;
    u32 r;
    u32 i;

    if (!ctx || !data || !parity || !L)
        return TBT_FEC_RLNC_ERR_PARAM;
    for (i = 0; i < L; i++)
        if (!data[i])
            return TBT_FEC_RLNC_ERR_PARAM;
    groups = tbt_group_count(L, ctx->k);
    for (i = 0; i < groups * ctx->m; i++)
        if (!parity[i])
            return TBT_FEC_RLNC_ERR_PARAM;

    for (g = 0; g < groups; g++)
    {
        int ws = (int)(g * ctx->k) + (int)ctx->k - (int)ctx->w;

        for (r = 0; r < ctx->m; r++)
        {
            u8 *dst = parity[(size_t)g * ctx->m + r];
            u32 nidx;

            memset(dst, 0, ctx->s);
            nidx = tbt_fec_rlnc_coeffs(ctx, g, r, ctx->idx, ctx->w);
            for (i = 0; i < nidx; i++)
            {
                int abs = ws + (int)ctx->idx[i];

                if (abs >= 0 && abs < (int)L)
                    tbt_xor_into(dst, data[abs], ctx->s);
            }
        }
    }
    return (int)(groups * ctx->m);
}

static int tbt_fec_rlnc_recover(struct tbt_fec_rlnc *ctx)
{
    u32 L = ctx->L;
    u32 words = (L + 63) / 64;
    u32 u = 0;
    u32 e = 0;
    u32 g;
    u32 r;
    u32 i;
    u32 rank;
    u32 col;

    for (i = 0; i < L; i++)
    {
        ctx->pos[i] = -1;
        if (!ctx->data_known[i])
        {
            ctx->pos[i] = (int)u;
            ctx->unknown[u++] = i;
        }
    }
    if (!u)
        return 0;

    for (g = 0; g < ctx->n_groups; g++)
    {
        int ws = (int)(g * ctx->k) + (int)ctx->k - (int)ctx->w;

        for (r = 0; r < ctx->m; r++)
        {
            u32 nidx;
            u64 *row;
            u8 *rhs;

            if (!ctx->parity_known[(size_t)g * ctx->m + r])
                continue;
            row = ctx->mat + (size_t)e * words;
            rhs = ctx->rhs_rows[e];
            memset(row, 0, (size_t)words * sizeof(u64));
            memcpy(rhs, ctx->parity[(size_t)g * ctx->m + r], ctx->s);

            nidx = tbt_fec_rlnc_coeffs(ctx, g, r, ctx->idx, ctx->w);
            for (i = 0; i < nidx; i++)
            {
                int abs = ws + (int)ctx->idx[i];
                int p;

                if (abs < 0 || abs >= (int)L)
                    continue;
                p = ctx->pos[abs];
                if (p < 0)
                    tbt_xor_into(rhs, ctx->data[abs], ctx->s);
                else
                    row[(u32)p >> 6] |= 1ull << (p & 63);
            }
            e++;
        }
    }
    if (!e)
        return TBT_FEC_RLNC_ERR_STUCK;

    rank = 0;
    for (col = 0; col < u && rank < e; col++)
    {
        u32 p;
        u64 bit = 1ull << (col & 63);
        u32 word = col >> 6;
        u32 rr;

        for (p = rank; p < e; p++)
            if (ctx->mat[(size_t)p * words + word] & bit)
                break;
        if (p == e)
            continue;
        if (p != rank)
        {
            u32 k;
            u8 *tmp;

            for (k = 0; k < words; k++)
            {
                u64 t = ctx->mat[(size_t)rank * words + k];

                ctx->mat[(size_t)rank * words + k] = ctx->mat[(size_t)p * words + k];
                ctx->mat[(size_t)p * words + k] = t;
            }
            tmp = ctx->rhs_rows[rank];
            ctx->rhs_rows[rank] = ctx->rhs_rows[p];
            ctx->rhs_rows[p] = tmp;
        }
        for (rr = 0; rr < e; rr++)
        {
            u32 k;

            if (rr == rank)
                continue;
            if (!(ctx->mat[(size_t)rr * words + word] & bit))
                continue;
            for (k = 0; k < words; k++)
                ctx->mat[(size_t)rr * words + k] ^= ctx->mat[(size_t)rank * words + k];
            tbt_xor_into(ctx->rhs_rows[rr], ctx->rhs_rows[rank], ctx->s);
        }
        ctx->pivot[rank] = col;
        rank++;
    }
    if (rank < u)
        return TBT_FEC_RLNC_ERR_STUCK;

    for (i = 0; i < u; i++)
    {
        u32 idx = ctx->unknown[ctx->pivot[i]];

        memcpy(ctx->data[idx], ctx->rhs_rows[i], ctx->s);
        ctx->data_known[idx] = 1;
        ctx->recovered_count++;
    }
    return (int)u;
}

static void tbt_fec_rlnc_update_frontier(struct tbt_fec_rlnc *ctx)
{
    while (ctx->frontier < ctx->L && ctx->data_known[ctx->frontier])
        ctx->frontier++;
    ctx->stuck_at = ctx->frontier;
}

int tbt_fec_rlnc_decode(struct tbt_fec_rlnc *ctx, u8 *data[], u8 *parity[],
                        const u8 *data_present, const u8 *parity_present, u32 L)
{
    u32 i;
    int rc;

    if (!ctx || !data || !parity || !data_present || !parity_present || !L ||
        L > TBT_FEC_MAX_W)
        return TBT_FEC_RLNC_ERR_PARAM;
    rc = tbt_fec_rlnc_bind(ctx, data, parity, L);
    if (rc != TBT_FEC_RLNC_OK)
        return rc;
    for (i = 0; i < L; i++)
        ctx->data_known[i] = data_present[i] ? 1 : 0;
    for (i = 0; i < ctx->n_parity; i++)
        ctx->parity_known[i] = parity_present[i] ? 1 : 0;
    rc = tbt_fec_rlnc_recover(ctx);
    tbt_fec_rlnc_update_frontier(ctx);
    return rc;
}

int tbt_fec_rlnc_advance(struct tbt_fec_rlnc *ctx, int kind, u32 index)
{
    int rc;

    if (!ctx || !ctx->L || !ctx->data || !ctx->parity || ctx->L > TBT_FEC_MAX_W)
        return TBT_FEC_RLNC_ERR_PARAM;
    if (kind == TBT_FEC_RLNC_DATA_KNOWN)
    {
        if (index >= ctx->L)
            return TBT_FEC_RLNC_ERR_PARAM;
        if (ctx->data_known[index])
            return (int)ctx->frontier;
        ctx->data_known[index] = 1;
    }
    else if (kind == TBT_FEC_RLNC_DATA_MISSING)
    {
        /* The caller reports a hole. Keep the packet unknown and let the
         * parity equations already present try to fill it. */
        if (index >= ctx->L)
            return TBT_FEC_RLNC_ERR_PARAM;
        if (ctx->data_known[index])
            return (int)ctx->frontier;
    }
    else if (kind == TBT_FEC_RLNC_PARITY_KNOWN)
    {
        if (index >= ctx->n_parity)
            return TBT_FEC_RLNC_ERR_PARAM;
        if (ctx->parity_known[index])
            return (int)ctx->frontier;
        ctx->parity_known[index] = 1;
    }
    else
    {
        return TBT_FEC_RLNC_ERR_PARAM;
    }
    rc = tbt_fec_rlnc_recover(ctx);
    tbt_fec_rlnc_update_frontier(ctx);
    return rc < 0 ? (int)ctx->frontier : (int)ctx->frontier;
}

u32 tbt_fec_rlnc_frontier(const struct tbt_fec_rlnc *ctx)
{
    return ctx ? ctx->frontier : 0;
}

u32 tbt_fec_rlnc_stuck_at(const struct tbt_fec_rlnc *ctx)
{
    return ctx ? ctx->stuck_at : 0;
}

u32 tbt_fec_rlnc_recovered(const struct tbt_fec_rlnc *ctx)
{
    return ctx ? ctx->recovered_count : 0;
}

int tbt_fec_rlnc_is_known(const struct tbt_fec_rlnc *ctx, u32 index)
{
    if (!ctx || index >= ctx->L)
        return 0;
    return ctx->data_known[index] != 0;
}

/* ------------------------------------------------------------------ *
 * Grouped mode
 * ------------------------------------------------------------------ */

struct tbt_fec_grouped
{
    const struct tbt_fec_ops *ops;
    u16 groups;
    u16 n;
    u16 m;
    u16 k;
    u16 w;
    size_t s;
    u16 d;
    u64 nonce;
    void *ctx[TBT_FEC_MAX_GROUPS];
};

struct tbt_fec_grouped *tbt_fec_grouped_new_ops(const struct tbt_fec_ops *ops,
                                                u16 groups, u16 n, u16 m,
                                                u16 k, u16 w, size_t s,
                                                u16 d, u64 nonce)
{
    struct tbt_fec_grouped *g;
    u16 i;

    if (!ops || !ops->new || !ops->free || !ops->encode || !ops->decode)
        return NULL;
    if (!groups || groups > TBT_FEC_MAX_GROUPS)
        return NULL;
    g = tbt_fec_alloc(sizeof(*g));
    if (!g)
        return NULL;
    g->ops = ops;
    g->groups = groups;
    g->n = n;
    g->m = m;
    g->k = k;
    g->w = w;
    g->s = s;
    g->d = d;
    g->nonce = nonce;
    for (i = 0; i < groups; i++)
    {
        g->ctx[i] = ops->new(n, m, k, w, s, d, nonce ^ ((u64)i << 32));
        if (!g->ctx[i])
        {
            tbt_fec_grouped_free(g);
            return NULL;
        }
    }
    return g;
}

struct tbt_fec_grouped *tbt_fec_grouped_new(u16 groups, u16 n, u16 m, u16 k, u16 w,
                                            size_t s, u16 d, u64 nonce)
{
    return tbt_fec_grouped_new_ops(&tbt_fec_ops_rlnc, groups, n, m, k, w, s,
                                   d, nonce);
}

void tbt_fec_grouped_free(struct tbt_fec_grouped *g)
{
    u16 i;

    if (!g)
        return;
    for (i = 0; i < g->groups; i++)
        if (g->ctx[i])
            g->ops->free(g->ctx[i]);
    tbt_fec_free(g);
}

u16 tbt_fec_grouped_count(const struct tbt_fec_grouped *g)
{
    return g ? g->groups : 0;
}

struct tbt_fec_rlnc *tbt_fec_grouped_ctx(struct tbt_fec_grouped *g, u16 group)
{
    if (!g || group >= g->groups || g->ops->mode != 1)
        return NULL;
    return g->ctx[group];
}

u8 tbt_fec_grouped_mode(const struct tbt_fec_grouped *g)
{
    return g ? g->ops->mode : 0;
}

int tbt_fec_grouped_encode(struct tbt_fec_grouped *g, u16 group, u8 *data[],
                           u32 L, u8 *parity[])
{
    if (!g || group >= g->groups)
        return -EINVAL;
    return g->ops->encode(g->ctx[group], data, L, parity);
}

int tbt_fec_grouped_decode(struct tbt_fec_grouped *g, u16 group, u8 *data[],
                           u8 *parity[], const u8 *data_present,
                           const u8 *parity_present, u32 L)
{
    if (!g || group >= g->groups)
        return -EINVAL;
    return g->ops->decode(g->ctx[group], data, parity, data_present,
                          parity_present, L);
}

void tbt_fec_grouped_set_nonce(struct tbt_fec_grouped *g, u16 group,
                               u64 nonce)
{
    if (!g || group >= g->groups || g->ops->mode != 1)
        return;
    tbt_fec_rlnc_set_nonce(g->ctx[group], nonce);
}

u16 tbt_fec_grouped_band(const struct tbt_fec_grouped *g, u16 group)
{
    if (!g || group >= g->groups || g->ops->mode != 1)
        return 0;
    return tbt_fec_rlnc_band(g->ctx[group]);
}

int tbt_fec_grouped_is_known(const struct tbt_fec_grouped *g, u16 group,
                             u32 index)
{
    if (!g || group >= g->groups || !g->ops->is_known)
        return 0;
    return g->ops->is_known(g->ctx[group], index);
}

/* ------------------------------------------------------------------ *
 * Dynamic parameters
 * ------------------------------------------------------------------ */

u16 tbt_fec_m_for_p(u32 p_ppm, u16 group_size)
{
    u16 m;

    if (group_size < 2)
        return 0;
    if (p_ppm <= 10000)
        m = 3;
    else if (p_ppm <= 200000)
        m = 4;
    else if (p_ppm <= 300000)
        m = 5;
    else
        m = 6;
    if (m >= group_size)
        m = (u16)(group_size - 1);
    if (!m)
        m = 1;
    return m;
}

int tbt_fec_params_single(u32 p_ppm, u16 carriers, struct tbt_fec_params *out)
{
    if (!out || carriers != TBT_FEC_MAX_CARRIERS || p_ppm > 200000)
        return -ERANGE;
    out->codec = TBT_FEC_CODEC_RLNC_SINGLE;
    out->groups = 1;
    out->group_size = TBT_FEC_MAX_CARRIERS;
    out->m = 64;
    out->n = (u16)(TBT_FEC_MAX_CARRIERS - out->m);
    out->k = out->n;
    out->w = (u16)(2 * out->n);
    out->d = 64;
    out->nonce = 0;
    return 0;
}

int tbt_fec_params_grouped(u32 p_ppm, u16 groups, u16 group_size,
                           struct tbt_fec_params *out)
{
    u16 m;

    if (!out || !groups || groups > TBT_FEC_MAX_GROUPS || group_size < 2 ||
        (u32)groups * group_size > TBT_FEC_MAX_SHARES)
        return -EINVAL;
    m = tbt_fec_m_for_p(p_ppm, group_size);
    if (!m || m >= group_size)
        return -EINVAL;
    out->codec = TBT_FEC_CODEC_RLNC_GROUPED;
    out->groups = groups;
    out->group_size = group_size;
    out->m = m;
    out->n = (u16)(group_size - m);
    out->k = out->n;
    out->w = (u16)(2 * out->n);
    if (out->w > TBT_FEC_MAX_W)
        return -EINVAL;
    out->d = (u16)(out->w / 2);
    if (!out->d)
        out->d = 1;
    if (out->w > 1 && out->d >= out->w)
        out->d = (u16)(out->w - 1);
    out->nonce = 0;
    return 0;
}

int tbt_fec_params_dynamic(u32 p_ppm, u16 carriers, struct tbt_fec_params *out)
{
    u16 group_size;
    u16 groups;

    if (!out || carriers < 2 || carriers > TBT_FEC_MAX_CARRIERS)
        return -EINVAL;
    if (carriers == TBT_FEC_MAX_CARRIERS && p_ppm <= 200000)
        return tbt_fec_params_single(p_ppm, carriers, out);
    group_size = carriers < 8 ? carriers : 8;
    groups = (u16)(carriers / group_size);
    if (!groups || (u32)groups * group_size != carriers)
    {
        group_size = carriers;
        groups = 1;
    }
    return tbt_fec_params_grouped(p_ppm, groups, group_size, out);
}

int tbt_fec_params_check(const struct tbt_fec_params *p)
{
    u16 shares;

    if (!p)
        return -EINVAL;
    /* SC-LDPC shares the n/m/groups geometry and reuses k/w/d for the code
     * rate and the codeword block, so it needs its own rules rather than the
     * RLNC window ones. */
    if (p->codec == TBT_FEC_CODEC_SC_LDPC)
    {
        if (!p->groups || p->groups > TBT_FEC_MAX_GROUPS || !p->group_size)
            return -EINVAL;
        if (!p->n || !p->m ||
            (u32)p->n + p->m != p->group_size ||
            (u32)p->groups * p->group_size > TBT_FEC_MAX_SHARES)
            return -EINVAL;
        if (!p->k || p->k < 100 || p->k > 1000)
            return -EINVAL;
        if (p->d || !p->w || p->w > TBT_FEC_MAX_W)
            return -EINVAL;
        return 0;
    }
    if (p->codec != TBT_FEC_CODEC_RLNC_SINGLE &&
        p->codec != TBT_FEC_CODEC_RLNC_GROUPED)
        return -EINVAL;
    if (!p->groups || p->groups > TBT_FEC_MAX_GROUPS || p->group_size < 2)
        return -EINVAL;
    if ((u32)p->groups * p->group_size > TBT_FEC_MAX_SHARES)
        return -EINVAL;
    if (!p->n || !p->m)
        return -EINVAL;
    shares = (u16)p->n + (u16)p->m;
    if (shares != p->group_size || shares > TBT_FEC_MAX_SHARES)
        return -EINVAL;
    if (!p->k || p->w < p->k || p->w > TBT_FEC_MAX_W)
        return -EINVAL;
    if (!p->d || p->d > p->w || p->d > TBT_FEC_MAX_D ||
        (p->w > 1 && p->d == p->w))
        return -EINVAL;
    if (p->codec == TBT_FEC_CODEC_RLNC_SINGLE && p->groups != 1)
        return -EINVAL;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Codec ops table (T0 §1.7). SC-LDPC supplies the same shape from D.
 * ------------------------------------------------------------------ */

static void *tbt_fec_ops_rlnc_new(u16 n, u16 m, u16 k, u16 w, size_t s, u16 d,
                                  u64 nonce)
{
    return tbt_fec_rlnc_new(n, m, k, w, s, d, nonce);
}

static void tbt_fec_ops_rlnc_free(void *ctx)
{
    tbt_fec_rlnc_free(ctx);
}

static u32 tbt_fec_ops_rlnc_parity_count(const void *ctx, u32 L)
{
    return tbt_fec_rlnc_parity_count(ctx, L);
}

static u32 tbt_fec_ops_rlnc_coeffs(const void *ctx, u32 g, u32 r, u32 *idx, u32 max)
{
    return tbt_fec_rlnc_coeffs(ctx, g, r, idx, max);
}

static int tbt_fec_ops_rlnc_encode(void *ctx, u8 *data[], u32 L, u8 *parity[])
{
    return tbt_fec_rlnc_encode(ctx, data, L, parity);
}

static int tbt_fec_ops_rlnc_decode(void *ctx, u8 *data[], u8 *parity[],
                                   const u8 *data_present, const u8 *parity_present, u32 L)
{
    return tbt_fec_rlnc_decode(ctx, data, parity, data_present, parity_present, L);
}

static int tbt_fec_ops_rlnc_advance(void *ctx, int kind, u32 index)
{
    return tbt_fec_rlnc_advance(ctx, kind, index);
}

static u32 tbt_fec_ops_rlnc_frontier(const void *ctx)
{
    return tbt_fec_rlnc_frontier(ctx);
}

static u32 tbt_fec_ops_rlnc_stuck_at(const void *ctx)
{
    return tbt_fec_rlnc_stuck_at(ctx);
}

static u32 tbt_fec_ops_rlnc_recovered(const void *ctx)
{
    return tbt_fec_rlnc_recovered(ctx);
}

static int tbt_fec_ops_rlnc_is_known(const void *ctx, u32 index)
{
    return tbt_fec_rlnc_is_known(ctx, index);
}

const struct tbt_fec_ops tbt_fec_ops_rlnc = {
    .name = "rlnc",
    .mode = 1,
    .new = tbt_fec_ops_rlnc_new,
    .free = tbt_fec_ops_rlnc_free,
    .parity_count = tbt_fec_ops_rlnc_parity_count,
    .coeffs = tbt_fec_ops_rlnc_coeffs,
    .encode = tbt_fec_ops_rlnc_encode,
    .decode = tbt_fec_ops_rlnc_decode,
    .advance = tbt_fec_ops_rlnc_advance,
    .frontier = tbt_fec_ops_rlnc_frontier,
    .stuck_at = tbt_fec_ops_rlnc_stuck_at,
    .recovered = tbt_fec_ops_rlnc_recovered,
    .is_known = tbt_fec_ops_rlnc_is_known,
};

/* ------------------------------------------------------------------ *
 * SC-LDPC shape selection (mode 3)
 *
 * The shape alone names the variant: one group is the binary downlink block
 * of 256 shares; more than one group is the GF(16) uplink, one block per
 * group. The rate is derived from n/(n+m) and never configured, so a shape
 * can never claim more parity capacity than its graph produces.
 * ------------------------------------------------------------------ */

int tbt_fec_sclc_shape_of_cfg(const struct tbt_fec_cfg *cfg, u16 carriers,
                              struct tbt_fec_sclc_shape *out)
{
    if (!cfg || !out)
        return -EINVAL;
    if (cfg->d == 0 && cfg->k >= 100 && cfg->k <= 1000)
    {
        /* Binary: w is the codeword block, k the rate. The binary codec's
         * graph has no coupling-length parameter to carry. */
        return tbt_fec_sclc_shape_of(cfg->n, cfg->m, carriers, cfg->w, 0,
                                     out);
    }
    if (cfg->k > cfg->w)
    {
        /* GF(16): k is the codeword block, w the coupling length, d the
         * rate in 1/256. */
        return tbt_fec_sclc_shape_of(cfg->n, cfg->m, carriers, cfg->k,
                                     cfg->w, out);
    }
    return -EINVAL;
}

int tbt_fec_sclc_shape_of(u16 n, u16 m, u16 carriers, u16 block, u16 L,
                          struct tbt_fec_sclc_shape *out)
{
    u32 shares = (u32)n + m;
    u16 groups;

    if (!out || !n || !m || !carriers || !shares ||
        shares > TBT_FEC_MAX_SHARES || carriers % shares)
        return -EINVAL;
    groups = (u16)(carriers / shares);
    if (!groups || groups > TBT_FEC_MAX_GROUPS)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->n = n;
    out->m = m;
    out->groups = groups;
    out->rate_ppm = (u16)((u32)n * 1000u / shares);
    out->rate_256 = (u16)((u32)n * 256u / shares);
    if (groups == 1 && shares == TBT_FEC_MAX_SHARES)
    {
        /* The binary codec's graph is fixed at 128 payload + 128 parity
         * variables per codeword block; any other split of the 256 shares
         * cannot be encoded, so name it as a shape error instead of letting
         * the codec factory fail as an allocation error. */
        if (n != TBT_FEC_MAX_SHARES / 2 || m != TBT_FEC_MAX_SHARES / 2)
            return -EINVAL;
        out->variant = TBT_FEC_SCLC_DOWN;
        out->block = block ? block : TBT_FEC_SCLC_BLOCK_DOWN;
        out->L = L ? L : TBT_FEC_SCLC_L_DOWN;
    }
    else if (groups > 1)
    {
        out->variant = TBT_FEC_SCLC_UP;
        out->block = block ? block : TBT_FEC_SCLC_BLOCK_UP;
        out->L = L ? L : TBT_FEC_SCLC_L_UP;
    }
    else
    {
        /* One group of fewer than 256 shares is neither shipped shape. The
         * binary codec's graph is bound to 128+128, so accepting it would
         * build a codec that cannot encode the block. */
        return -EINVAL;
    }
    if (!out->block || (out->block & 1) || !out->L)
        return -EINVAL;
    if (!out->rate_ppm || !out->rate_256)
        return -EINVAL;
    return 0;
}

#ifndef __KERNEL__
/* Host-only: bench_micro times the encoder's XOR inner loop without the
 * codec around it. The module build does not compile this. */
void tbt_fec_host_xor_into(u8 *dst, const u8 *src, size_t len)
{
    tbt_xor_into(dst, src, len);
}
#endif
