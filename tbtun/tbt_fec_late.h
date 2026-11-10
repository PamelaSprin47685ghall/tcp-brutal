/* Bounded late-DATA ledger, shared by the kernel RX path and host tests.
 * A record is keyed by (current epoch, block id); a bitmap deduplicates
 * (group, share). Pending bits are settled at fold; a post-fold share may
 * correct exactly one counted miss in its committed super-block. */
#ifndef TBT_FEC_LATE_H
#define TBT_FEC_LATE_H

#include "tbt_uapi.h"

#define TBT_FEC_LATE_RECS 16

struct tbt_fec_late_rec
{
    u64 block_id;
    u64 super_id;
    u32 epoch;
    u8 used;
    u8 folded;
    u16 miss;
    u8 pending[TBT_FEC_BITMAP_BYTES];
    u8 settled[TBT_FEC_BITMAP_BYTES];
    u8 miss_bits[TBT_FEC_BITMAP_BYTES];
};

struct tbt_fec_late_ctx
{
    struct tbt_fec_late_rec rec[TBT_FEC_LATE_RECS];
    u32 epoch;
    u64 next_super;
    u16 n;
    u16 m;
    u16 groups;
};

enum tbt_fec_late_evt
{
    TBT_FEC_LATE_UNKNOWN = 0,
    TBT_FEC_LATE_DUP,
    TBT_FEC_LATE_PENDING,
    TBT_FEC_LATE_SETTLED,
    TBT_FEC_LATE_SETTLED_NOMISS,
};

static inline bool tbt_fec_late_bit(const u8 *bm, u16 bit)
{
    return (bm[bit >> 3] >> (bit & 7)) & 1;
}

static inline void tbt_fec_late_set(u8 *bm, u16 bit)
{
    bm[bit >> 3] |= (u8)(1u << (bit & 7));
}

static inline void tbt_fec_late_clear(u8 *bm, u16 bit)
{
    bm[bit >> 3] &= (u8)~(1u << (bit & 7));
}

static inline u16 tbt_fec_late_count(const u8 *bm)
{
    u16 i, n = 0;

    for (i = 0; i < TBT_FEC_BITMAP_BYTES * 8u; i++)
        n += tbt_fec_late_bit(bm, i);
    return n;
}

static inline void tbt_fec_late_reset(struct tbt_fec_late_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

static inline struct tbt_fec_late_rec *
tbt_fec_late_find(struct tbt_fec_late_ctx *ctx, u64 block_id)
{
    u16 i;

    for (i = 0; i < TBT_FEC_LATE_RECS; i++)
        if (ctx->rec[i].used && ctx->rec[i].block_id == block_id &&
            ctx->rec[i].epoch == ctx->epoch)
            return &ctx->rec[i];
    return NULL;
}

static inline struct tbt_fec_late_rec *
tbt_fec_late_get(struct tbt_fec_late_ctx *ctx, u64 block_id)
{
    struct tbt_fec_late_rec *rec = tbt_fec_late_find(ctx, block_id);
    u16 i, victim = 0;

    if (rec)
        return rec;
    for (i = 0; i < TBT_FEC_LATE_RECS; i++)
    {
        if (!ctx->rec[i].used)
        {
            victim = i;
            break;
        }
        if (ctx->rec[i].block_id < ctx->rec[victim].block_id)
            victim = i;
    }
    rec = &ctx->rec[victim];
    memset(rec, 0, sizeof(*rec));
    rec->used = true;
    rec->epoch = ctx->epoch;
    rec->block_id = block_id;
    return rec;
}

static inline enum tbt_fec_late_evt
tbt_fec_late_credit(struct tbt_fec_late_ctx *ctx, u64 block_id, u16 group,
                    u16 share, bool *has_miss)
{
    struct tbt_fec_late_rec *rec;
    u16 bit;

    *has_miss = false;
    if (!ctx->n || !ctx->m || group >= ctx->groups ||
        share >= (u16)(ctx->n + ctx->m) ||
        (u32)ctx->groups * (ctx->n + ctx->m) >
            TBT_FEC_BITMAP_BYTES * 8u ||
        block_id > ctx->next_super ||
        ctx->next_super - block_id >= TBT_FEC_LATE_RECS)
        return TBT_FEC_LATE_UNKNOWN;
    bit = (u16)(group * (ctx->n + ctx->m) + share);
    if (bit >= TBT_FEC_BITMAP_BYTES * 8u)
        return TBT_FEC_LATE_UNKNOWN;
    rec = tbt_fec_late_find(ctx, block_id);
    if (!rec && block_id < ctx->next_super)
        return TBT_FEC_LATE_UNKNOWN;
    if (!rec)
        rec = tbt_fec_late_get(ctx, block_id);
    if (tbt_fec_late_bit(rec->pending, bit) ||
        tbt_fec_late_bit(rec->settled, bit))
        return TBT_FEC_LATE_DUP;
    if (!rec->folded)
    {
        tbt_fec_late_set(rec->pending, bit);
        return TBT_FEC_LATE_PENDING;
    }
    tbt_fec_late_set(rec->settled, bit);
    if (!tbt_fec_late_bit(rec->miss_bits, bit))
        return TBT_FEC_LATE_SETTLED_NOMISS;
    *has_miss = true;
    return TBT_FEC_LATE_SETTLED;
}

static inline void tbt_fec_late_take(struct tbt_fec_late_ctx *ctx,
                                     u64 block_id, u16 group, u8 *out)
{
    struct tbt_fec_late_rec *rec = tbt_fec_late_find(ctx, block_id);
    u16 i, shares;

    if (!rec)
        return;
    shares = (u16)(ctx->n + ctx->m);
    if ((u32)ctx->groups * shares > TBT_FEC_BITMAP_BYTES * 8u)
        return;
    for (i = 0; i < shares; i++)
    {
        u16 bit = (u16)(group * shares + i);

        if (bit >= TBT_FEC_BITMAP_BYTES * 8u ||
            !tbt_fec_late_bit(rec->pending, bit))
            continue;
        tbt_fec_late_clear(rec->pending, bit);
        tbt_fec_late_set(rec->settled, bit);
        out[i] = 1;
    }
}

/* Record only the misses that est_block counts: real, alive, under limit,
 * not codec_fail. */
static inline bool tbt_fec_late_note_miss(struct tbt_fec_late_ctx *ctx,
                                          u64 block_id, u16 group,
                                          const u8 *sc_miss,
                                          const u8 *codec_fail,
                                          const u8 *alive, u32 limit,
                                          u16 real)
{
    struct tbt_fec_late_rec *rec;
    u16 i, shares = (u16)(ctx->n + ctx->m);
    bool any = false;

    if ((u32)ctx->groups * shares > TBT_FEC_BITMAP_BYTES * 8u ||
        real > TBT_FEC_MAX_SHARES)
        return false;
    for (i = 0; i < real; i++)
        if ((u32)group * shares + i < TBT_FEC_BITMAP_BYTES * 8u &&
            tbt_fec_late_bit(sc_miss, i) &&
            !tbt_fec_late_bit(codec_fail, i) &&
            (u32)group * shares + i < limit &&
            tbt_fec_late_bit(alive, (u16)(group * shares + i)))
        {
            any = true;
            break;
        }
    if (!any)
        return false;
    rec = tbt_fec_late_get(ctx, block_id);
    for (i = 0; i < real; i++)
    {
        u16 bit = (u16)(group * shares + i);

        if (bit >= TBT_FEC_BITMAP_BYTES * 8u ||
            !tbt_fec_late_bit(sc_miss, i) ||
            tbt_fec_late_bit(codec_fail, i) ||
            (u32)bit >= limit || !tbt_fec_late_bit(alive, bit) ||
            tbt_fec_late_bit(rec->miss_bits, bit))
            continue;
        tbt_fec_late_set(rec->miss_bits, bit);
    }
    return true;
}

/* Set the committed super id and settle pending bits that raced the group
 * loop. Returns true for each pending miss the caller must correct. */
static inline struct tbt_fec_late_rec *
tbt_fec_late_finalize(struct tbt_fec_late_ctx *ctx, u64 block_id,
                      u64 super_id, u8 *correct_bits)
{
    struct tbt_fec_late_rec *rec = tbt_fec_late_find(ctx, block_id);
    u16 i;

    if (!rec)
        return NULL;
    rec->folded = true;
    rec->super_id = super_id;
    rec->miss = tbt_fec_late_count(rec->miss_bits);
    for (i = 0; i < TBT_FEC_BITMAP_BYTES * 8u; i++)
    {
        if (!tbt_fec_late_bit(rec->pending, i))
            continue;
        tbt_fec_late_clear(rec->pending, i);
        tbt_fec_late_set(rec->settled, i);
        if (tbt_fec_late_bit(rec->miss_bits, i))
            tbt_fec_late_set(correct_bits, i);
    }
    return rec;
}

#endif
