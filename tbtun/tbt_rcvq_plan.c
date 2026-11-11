/* tbt_rcvq_plan.c - pure decisions for receive-queue repair.
 *
 * Two questions, no kernel state, no allocation:
 *
 *   tbt_rcvq_pick_carrier()
 *     The normal schedule maps one share to one carrier over a group's
 *     [base, base + span).  When every share but one has arrived, the missing
 *     carrier is the set difference.  A named carrier outside the range means
 *     the caller's mapping is inconsistent (-EINVAL).  A replay or a second
 *     loss makes the difference ambiguous (-ENOENT); the caller then falls
 *     back to the FEC_MAP or to TCP retransmission.
 *
 *   tbt_rcvq_plan()
 *     Map a rebuilt wire record onto the hole [rcv_nxt, ooo_seq).  A record
 *     with a known start (FEC_MAP) accepts the MSS subrange; a record without
 *     one is accepted only when the hole is exactly that whole record.  When
 *     in doubt the planner refuses: a guessed delta corrupts the stream for
 *     good.
 *
 * Compiles into the module and into host/test_rcvq_plan.c.  See
 * review/calib/rcvq-design.md sections 3.3 and 3.4.
 */
#include "tbt_rcvq.h"

/* The kernel spells the signed 32-bit type s32; tbt_uapi.h gives a host build
 * int32_t.  Both are two's complement, which the 32-bit sequence arithmetic
 * in tbt_rcvq_plan() relies on. */
#ifdef __KERNEL__
typedef s32 tbt_rcvq_s32;
#else
typedef int32_t tbt_rcvq_s32;
#endif

/* One bit per carrier, indexed by carrier number.  n and span are capped at
 * this width, so the 32-byte stack array is the whole allocation. */
#define TBT_RCVQ_CARRIER_BITS 256u
#define TBT_RCVQ_CARRIER_BYTES (TBT_RCVQ_CARRIER_BITS / 8u)

int tbt_rcvq_pick_carrier(const u8 *src_carrier, u16 n, u16 base, u16 span,
                          u8 *carrier)
{
    u8 seen[TBT_RCVQ_CARRIER_BYTES] = { 0 };
    u32 end;
    u32 missing = 0;
    u32 found = TBT_RCVQ_NO_CARRIER;
    u16 i;
    u32 c;

    if (!carrier)
        return -EINVAL;
    if (n > TBT_RCVQ_CARRIER_BITS || span > TBT_RCVQ_CARRIER_BITS)
        return -EINVAL;
    if (n && !src_carrier)
        return -EINVAL;

    /* A carrier is a u8, so the range must stay inside 0..255.  A span of 256
     * with base 0 is the widest legal range. */
    end = (u32)base + (u32)span;
    if (end > TBT_RCVQ_CARRIER_BITS)
        return -EINVAL;

    for (i = 0; i < n; i++)
    {
        u8 named = src_carrier[i];

        if (named == TBT_RCVQ_NO_CARRIER)
            continue;
        if ((u32)named < (u32)base || (u32)named >= end)
            return -EINVAL;
        seen[named >> 3] |= (u8)(1u << (named & 7u));
    }

    for (c = base; c < end; c++)
    {
        if (seen[c >> 3] & (u8)(1u << (c & 7u)))
            continue;
        if (++missing > 1)
            return -ENOENT;
        found = c;
    }

    if (missing != 1)
        return -ENOENT;

    *carrier = (u8)found;
    return 0;
}

static enum tbt_rcvq_skip tbt_rcvq_plan_skip(struct tbt_rcvq_plan *out,
                                             enum tbt_rcvq_skip why)
{
    out->delta = 0;
    out->inject_len = 0;
    out->skip = why;
    return why;
}

enum tbt_rcvq_skip tbt_rcvq_plan(const struct tbt_rcvq_plan_in *in,
                                 struct tbt_rcvq_plan *out)
{
    tbt_rcvq_s32 delta;
    u32 hole;
    u32 inject;

    if (!in->gen_ok || in->carrier != in->expect_carrier)
        return tbt_rcvq_plan_skip(out, TBT_RCVQ_SKIP_CARRIER);

    if (in->ooo_seq == in->rcv_nxt)
        return tbt_rcvq_plan_skip(out, TBT_RCVQ_SKIP_NOHOLE);

    /* Unsigned subtraction is the 32-bit sequence distance.  A distance at or
     * above 2^31 is behind rcv_nxt, not a hole. */
    hole = in->ooo_seq - in->rcv_nxt;
    if ((tbt_rcvq_s32)hole <= 0)
        return tbt_rcvq_plan_skip(out, TBT_RCVQ_SKIP_NOHOLE);

    if (!in->has_record_seq)
    {
        if (hole != in->wire_len)
            return tbt_rcvq_plan_skip(out, TBT_RCVQ_SKIP_OVERLAP);
        delta = 0;
        inject = in->wire_len;
    }
    else
    {
        delta = (tbt_rcvq_s32)(in->rcv_nxt - in->record_seq);
        if (delta < 0)
            return tbt_rcvq_plan_skip(out, TBT_RCVQ_SKIP_DELTA);
        if (delta >= (tbt_rcvq_s32)in->wire_len)
            return tbt_rcvq_plan_skip(out, TBT_RCVQ_SKIP_OVERLAP);

        /* Inject the part of the record the hole can take.  The tail past
         * ooo_seq is already queued, or the next hole will ask for it. */
        inject = (u32)in->wire_len - (u32)delta;
        if (inject > hole)
            inject = hole;
    }

    if (inject == 0)
        return tbt_rcvq_plan_skip(out, TBT_RCVQ_SKIP_OVERLAP);

    out->delta = (u16)delta;
    out->inject_len = (u16)inject;
    out->skip = TBT_RCVQ_OK;
    return TBT_RCVQ_OK;
}
