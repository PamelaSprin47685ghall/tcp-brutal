/* tbt_fec_sclc.c - downlink SC-IRA codec, 256-share rate ladder.
 *
 * Construction and message semantics follow the 0.3 SC-LDPC host prototype
 * (SC_TERM_ACC; the prototype tree was dropped on 2026-11-12). The FEC block
 * always has 256 shares and 1024 codewords;
 * n data shares and m = 256 - n parity shares select the transmitted code
 * rate. Each coupling block holds n payload variables, m parity variables
 * and m checks, so every parity variable is transmitted: nothing is
 * punctured. The payload degree is 5 below or at rate 50% and 4 above it,
 * which keeps the check degree at most 14 while the check count tracks the
 * parity overhead. The encoder is the bit64 forward sweep; the decoder is
 * the bit-sliced peeling decoder, one 64-codeword batch at a time. See
 * tbt_fec_sclc.h for the exact byte mapping and the graph shape.
 *
 * The batch loop is a property of the share format (1024 bytes / 64 bytes =
 * 16 batches), never of n. Keeping it fixed is what lets every ladder point
 * share the same wire block and the same decoder scratch.
 *
 * No allocation happens on the per-share path: the code object owns the
 * graph, the encoder plan and the payload/parity scratch.  The decoder's
 * bit-sliced scratch is allocated on the first decode and reused for every
 * batch, so a transmit codec never carries it.
 */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#else
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#endif

#include "tbt_fec_sclc.h"

/* ------------------------------------------------------------------ *
 * Allocation (same policy as tbt_fec.c: process context only)
 * ------------------------------------------------------------------ */

static void *sclc_alloc(size_t len)
{
#ifdef __KERNEL__
    /* Same policy as tbt_fec.h: a sparse codeword plane above 32 KiB is
     * vmalloc-backed so a 256-carrier session does not pin high-order slab
     * page blocks on a small host. */
    if (len >= 32u * 1024u)
        return vzalloc(len ? len : 1);
    return kvzalloc(len, GFP_KERNEL);
#else
    return calloc(1, len);
#endif
}

static void sclc_free(void *p)
{
#ifdef __KERNEL__
    kvfree(p);
#else
    free(p);
#endif
}

/* ------------------------------------------------------------------ *
 * Little-endian 64-bit plane helpers.  A plane is stored little-endian
 * regardless of the host byte order; bit c is codeword c.
 * ------------------------------------------------------------------ */

/*
 * One 64-codeword x 8-bit segment <-> eight bit planes.  Segment byte c
 * holds codeword c's eight bits, LSB first; plane j bit c is bit j.  The
 * segment is the interleaved unit: every share carries one segment per
 * batch, so a lost share erases the same eight payload (or parity) bits
 * in all 64 codewords of the batch, which is the --corr mask shape.
 */
/*
 * Eight bytes -> eight bit planes.  The old scalar form touched each of
 * the 512 bits separately and dominated the decoder profile (57% gather,
 * 18% scatter on the host).  The three masked swaps below are the standard
 * 8x8 bit transpose: byte j of the result holds bit j of each input byte,
 * which is exactly one plane's eight codeword bits.  It is an involution,
 * so scatter applies the same transform to the eight plane bytes.
 *
 * The packing helpers name each byte with a shift instead of a memcpy so
 * the mapping stays little-endian on any host; the compiler still folds
 * them into one load or store.
 */
static u64 sclc_load8(const u8 *p)
{
    return (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) |
           ((u64)p[3] << 24) | ((u64)p[4] << 32) | ((u64)p[5] << 40) |
           ((u64)p[6] << 48) | ((u64)p[7] << 56);
}

static void sclc_store8(u8 *p, u64 x)
{
    p[0] = (u8)x;
    p[1] = (u8)(x >> 8);
    p[2] = (u8)(x >> 16);
    p[3] = (u8)(x >> 24);
    p[4] = (u8)(x >> 32);
    p[5] = (u8)(x >> 40);
    p[6] = (u8)(x >> 48);
    p[7] = (u8)(x >> 56);
}

static u64 sclc_transpose8(u64 x)
{
    u64 t;

    t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAULL;
    x ^= t ^ (t << 7);
    t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL;
    x ^= t ^ (t << 14);
    t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL;
    x ^= t ^ (t << 28);
    return x;
}

/* The most significant bit of each of the eight bytes, packed into bits
 * 0..7. The multiplier moves bit 7 of byte k to bit 56 + k; every other
 * product bit lands below 56, so the shift is the whole result. This is pure
 * 64-bit integer arithmetic: no FPU, no vector registers, and the same code
 * runs on x86_64 and arm64. */
static inline u64 seg_movemask8(u64 x)
{
    return ((x & 0x8080808080808080ULL) * 0x0002040810204081ULL) >> 56;
}

/* Bit-slice one 64-byte segment. For bit j, the eight rows contribute one
 * byte each; the shift moves bit j into each row byte's MSB and the multiply
 * folds those eight bits into one byte. Measured on the x86_64 test host:
 * 175 ns against 213 ns for the transpose8 form (18% less). */
static void seg_gather(u64 plane[8], const u8 *seg)
{
    u64 row[8];
    int k, j;

    for (k = 0; k < 8; k++)
        row[k] = sclc_load8(seg + k * 8);
    for (j = 0; j < 8; j++)
    {
        u64 p = 0;

        for (k = 0; k < 8; k++)
            p |= seg_movemask8(row[k] << (7 - j)) << (8 * k);
        plane[j] = p;
    }
}

/* The inverse is already cheaper than a movemask formulation: eight byte
 * extracts plus one transpose8 per row. A SWAR scatter measured equal to
 * this form, so it stays. */
static void seg_scatter(u8 *seg, const u64 plane[8])
{
    int k, j;

    for (k = 0; k < 8; k++)
    {
        u64 x = 0;

        for (j = 0; j < 8; j++)
            x |= (u64)((plane[j] >> (8 * k)) & 0xffu) << (8 * j);
        sclc_store8(seg + k * 8, sclc_transpose8(x));
    }
}

/* ------------------------------------------------------------------ *
 * PRNG: splitmix64 seeding and xorshift128+ draw, as the prototype.
 * ------------------------------------------------------------------ */

struct sclc_rng
{
    u64 s0, s1;
};

static u64 sclc_splitmix64(u64 *x)
{
    u64 z = (*x += 0x9E3779B97F4A7C15ULL);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void sclc_rng_seed(struct sclc_rng *r, u64 seed)
{
    u64 x = seed;

    r->s0 = sclc_splitmix64(&x);
    r->s1 = sclc_splitmix64(&x);
    if (!r->s0 && !r->s1)
        r->s0 = 1;
}

static u64 sclc_rng_next(struct sclc_rng *r)
{
    u64 x = r->s0;
    u64 y = r->s1;

    r->s0 = y;
    x ^= x << 23;
    r->s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
    return r->s1 + y;
}

static u32 sclc_rng_below(struct sclc_rng *r, u32 n)
{
    return (u32)(((sclc_rng_next(r) >> 32) * (u64)n) >> 32);
}

static void sclc_shuffle(struct sclc_rng *r, int *a, int n)
{
    int i;

    for (i = 0; i < n - 1; i++)
    {
        int j = i + (int)sclc_rng_below(r, (u32)(n - i));
        int t = a[i];

        a[i] = a[j];
        a[j] = t;
    }
}

/* ------------------------------------------------------------------ *
 * Code object
 * ------------------------------------------------------------------ */

struct tbt_sclc
{
    u16 n_shares; /* data shares per FEC block */
    u16 m_shares; /* parity shares and checks per FEC block */
    u16 k;        /* rate in parts per thousand */
    u16 w;        /* codeword block, 256 */
    size_t s;     /* share bytes, 1024 */
    u64 nonce;

    int n;       /* variables per codeword, 8*(n_shares+m_shares) */
    int n_data;  /* 8 * n_shares */
    int n_par;   /* 8 * m_shares */
    int n_chk;   /* one check per parity variable */
    int batches; /* 1024 codewords / 64, always 16 */
    int edges;
    int dcs; /* encoder plan stride, max check degree */

    int *voff; /* n + 1 */
    int *coff; /* n_chk + 1 */
    int *vadj; /* edges, check ids */
    int *cadj; /* edges, variable ids */
    int *owned;  /* n_chk: internal parity variable this check solves */
    u16 *padj;   /* n_chk * dcs: XOR list, owned parity last */
    u8 *pdeg;    /* n_chk: check degree */

    u64 *v;       /* n: encoder scratch, payload then parity */
    u8 dec_ready; /* decoder scratch below is allocated on first decode */
    u64 *unk;     /* n: bit-sliced unknown mask */
    u64 *val;     /* n: bit-sliced known value */
    u64 *cnt;     /* n_chk * 4: bit-sliced unknown count */
    u64 *xr;      /* n_chk: XOR of known neighbours */
    u64 *act;     /* n_chk: exactly-one-unknown mask */
    u64 *resolve; /* n: resolved mask for this wave */
    u64 *vbits;   /* n: resolved value for this wave */
    int *stack;   /* n_chk */
    int *touched; /* n */
    u8 *tflag;    /* n */

    u8 *known;     /* n_shares: data share known */
    u8 *batch_ok;  /* batches: batch decoded */
    u32 recovered_count;
    u32 frontier;
    u32 stuck_at;
};

/* ------------------------------------------------------------------ *
 * Bit-sliced 4-plane counters (check degree <= 15)
 * ------------------------------------------------------------------ */

static void cnt_add(u64 c[4], u64 m)
{
    u64 n0 = c[0] ^ m;
    u64 k0 = c[0] & m;
    u64 n1 = c[1] ^ k0;
    u64 k1 = c[1] & k0;
    u64 n2 = c[2] ^ k1;
    u64 k2 = c[2] & k1;

    c[0] = n0;
    c[1] = n1;
    c[2] = n2;
    c[3] ^= k2;
}

static void cnt_sub(u64 c[4], u64 m)
{
    u64 n0 = c[0] ^ m;
    u64 b0 = ~c[0] & m;
    u64 n1 = c[1] ^ b0;
    u64 b1 = ~c[1] & b0;
    u64 n2 = c[2] ^ b1;
    u64 b2 = ~c[2] & b1;

    c[0] = n0;
    c[1] = n1;
    c[2] = n2;
    c[3] ^= b2;
}

static u64 cnt_one(const u64 c[4])
{
    return c[0] & ~c[1] & ~c[2] & ~c[3];
}

/* ------------------------------------------------------------------ *
 * Graph construction: SC_TERM_ACC, 256-share rate ladder
 * ------------------------------------------------------------------ */

static void sclc_graph_free(struct tbt_sclc *c)
{
    sclc_free(c->voff);
    sclc_free(c->coff);
    sclc_free(c->vadj);
    sclc_free(c->cadj);
    sclc_free(c->owned);
    sclc_free(c->padj);
    sclc_free(c->pdeg);
    c->voff = NULL;
    c->coff = NULL;
    c->vadj = NULL;
    c->cadj = NULL;
    c->owned = NULL;
    c->padj = NULL;
    c->pdeg = NULL;
}

/*
 * A payload slot maps the n variables of one source block onto the pc
 * checks of one supernode. Below or at rate 50% (n <= pc) one random
 * permutation suffices: every variable gets its own check and pc - n checks
 * stay empty for that group. Above rate 50% (n > pc) the slot array holds
 * each check q = n / pc times plus one extra for the first n % pc checks,
 * then shuffles: every check gets q or q + 1 edges from that group. The
 * shuffle keeps the split random while bounding the degree, which is what
 * the four-plane counter relies on.
 */
static void sclc_payload_slots(struct sclc_rng *rng, int n, int pc, int *slot)
{
    int b;

    if (n <= pc)
    {
        for (b = 0; b < pc; b++)
            slot[b] = b;
        sclc_shuffle(rng, slot, pc);
        return;
    }
    for (b = 0; b < n; b++)
        slot[b] = b % pc;
    sclc_shuffle(rng, slot, n);
}

/*
 * Payload block i owns n variables and connects to dv check supernodes
 * (i .. i+dv-1) mod L, so every payload variable has degree dv. dv is 5
 * below or at rate 50% and 4 above it: with pc = m checks per supernode the
 * higher rate has fewer checks, and degree 4 keeps every check within the
 * four-plane counter while keeping the controller's upper boundary fully
 * recoverable. Each incoming payload group has its own slot assignment, so a
 * check sees at most ceil(n / pc) edges from each source block. Independent
 * shuffled slots keep the groups from aligning on the same bit position.
 *
 * Every check also owns one parity variable of its supernode (a bijection
 * over the pc checks) and, except supernode 0, consumes one parity variable
 * from supernode j-1. That keeps the parity submatrix a non-wrapping double
 * diagonal and the encoder a forward sweep in supernode order. The check
 * order is the construction order: supernode a holds checks a*pc .. a*pc+pc-1.
 *
 * There is no puncturing: pc = m, so the encoder computes exactly the m
 * parity variables it transmits, and the decoder never starts a parity
 * variable as an erasure beyond the channel's own loss.
 */
static int sclc_build_graph(struct tbt_sclc *c)
{
    const int L = TBT_SCLC_L;
    const int n = c->n_shares;
    const int m = c->m_shares;
    const int pc = m;
    const int dv = n > m ? TBT_SCLC_DV_HIGH : TBT_SCLC_DV;
    struct sclc_rng rng;
    int *vdeg = NULL, *cdeg = NULL, *cur = NULL, *ccur = NULL;
    int *perm = NULL, *slot = NULL, *slot0 = NULL, *inv_slot = NULL;
    int i, j, r, b, e, ck;
    int rc = -ENOMEM;

    if (!n || !m || (u32)n + m != TBT_SCLC_SHARES)
        return -EINVAL;
    if (pc > TBT_SCLC_SHARES || n > TBT_SCLC_SHARES)
        return -EINVAL;

    c->n_data = 8 * n;
    c->n_par = 8 * pc;
    c->n_chk = c->n_par;
    c->n = c->n_data + c->n_par;
    c->batches = TBT_SCLC_BATCHES;
    c->edges = L * dv * n + L * pc + (L - 1) * pc;
    if (c->batches != TBT_SCLC_BATCHES)
        return -EINVAL;

    vdeg = sclc_alloc((size_t)c->n * sizeof(int));
    cdeg = sclc_alloc((size_t)c->n_chk * sizeof(int));
    perm = sclc_alloc((size_t)pc * sizeof(int));
    slot = sclc_alloc((size_t)(n > pc ? n : pc) * sizeof(int));
    slot0 = sclc_alloc((size_t)L * pc * sizeof(int));
    inv_slot = sclc_alloc((size_t)pc * sizeof(int));
    c->voff = sclc_alloc((size_t)(c->n + 1) * sizeof(int));
    c->coff = sclc_alloc((size_t)(c->n_chk + 1) * sizeof(int));
    c->vadj = sclc_alloc((size_t)c->edges * sizeof(int));
    c->cadj = sclc_alloc((size_t)c->edges * sizeof(int));
    c->owned = sclc_alloc((size_t)c->n_chk * sizeof(int));
    if (!vdeg || !cdeg || !perm || !slot || !slot0 || !inv_slot ||
        !c->voff || !c->coff || !c->vadj || !c->cadj || !c->owned)
        goto out;

    sclc_rng_seed(&rng, c->nonce);

    for (i = 0; i < c->n; i++)
        vdeg[i] = 0;
    for (i = 0; i < c->n_chk; i++)
        cdeg[i] = 0;

    /* Degree pass. Payload block i has n variables of degree dv. Parity
     * block i has pc variables: degree 2 except the last block, which does
     * not feed an accumulator check. Each check owns one parity variable
     * and, for i > 0, consumes one from the previous block. */
    for (i = 0; i < L; i++)
        for (b = 0; b < n; b++)
            vdeg[i * n + b] = dv;
    for (i = 0; i < L; i++)
        for (b = 0; b < pc; b++)
            vdeg[c->n_data + i * pc + b] = (i < L - 1) ? 2 : 1;
    for (i = 0; i < L; i++)
        for (b = 0; b < pc; b++)
            cdeg[i * pc + b] = 1 + (i ? 1 : 0);

    /* Payload counts: one fresh slot assignment per (supernode, source
     * slot). */
    for (j = 0; j < L; j++)
    {
        for (r = 0; r < dv; r++)
        {
            sclc_payload_slots(&rng, n, pc, slot);
            for (b = 0; b < n; b++)
                cdeg[j * pc + slot[b]]++;
        }
    }

    c->voff[0] = 0;
    for (i = 0; i < c->n; i++)
        c->voff[i + 1] = c->voff[i] + vdeg[i];
    c->coff[0] = 0;
    for (i = 0; i < c->n_chk; i++)
        c->coff[i + 1] = c->coff[i] + cdeg[i];
    if (c->voff[c->n] != c->edges || c->coff[c->n_chk] != c->edges)
    {
        rc = -EINVAL;
        goto out;
    }
    for (i = 0; i < c->n_chk; i++)
        c->owned[i] = -1;

    cur = sclc_alloc((size_t)c->n * sizeof(int));
    ccur = sclc_alloc((size_t)c->n_chk * sizeof(int));
    if (!cur || !ccur)
        goto out;
    memcpy(cur, c->voff, (size_t)c->n * sizeof(int));
    memcpy(ccur, c->coff, (size_t)c->n_chk * sizeof(int));

    /* Payload edges: reseed and replay the same slot stream. */
    sclc_rng_seed(&rng, c->nonce);
    for (j = 0; j < L; j++)
    {
        for (r = 0; r < dv; r++)
        {
            i = (j + L - r) % L;
            sclc_payload_slots(&rng, n, pc, slot);
            /* r == 0 is source block j feeding check supernode j: the
             * mapping a parity wire position must align with. */
            if (r == 0)
                memcpy(slot0 + (size_t)j * pc, slot,
                       (size_t)pc * sizeof(int));
            for (b = 0; b < n; b++)
            {
                int v = i * n + b;
                int ck = j * pc + slot[b];

                c->vadj[cur[v]++] = ck;
                c->cadj[ccur[ck]++] = v;
            }
        }
    }

    /* Parity edges: own check, then the accumulator check of the next
     * supernode. Both maps are bijections over the pc checks.
     *
     * Wire-order relabel: a parity variable travels in the bit-plane
     * position that indexes it, and a short block sends only the first
     * win_len parity rows. Give the variable whose own check is slot[b]
     * the id of wire position b (wv = slot^-1[perm[b]]), so row b owns
     * the check that also carries payload bit b of the same block. The
     * graph is the same edge set under this renaming, so full-block
     * thresholds are unchanged; only the share a parity bit travels in
     * moves. n > pc has no bijection between payload slots and checks, so
     * that shape keeps the identity order. */
    for (i = 0; i < L; i++)
    {
        int pbase = c->n_data + i * pc;

        if (n <= pc)
            for (b = 0; b < pc; b++)
                inv_slot[slot0[(size_t)i * pc + b]] = b;
        for (b = 0; b < pc; b++)
            perm[b] = b;
        sclc_shuffle(&rng, perm, pc);
        for (b = 0; b < pc; b++)
        {
            int wv = (n <= pc) ? inv_slot[perm[b]] : b;

            /* slot[] is free now; keep the variable's wire id for the
             * accumulator edge drawn from the second shuffle. */
            slot[b] = wv;
            ck = i * pc + perm[b];
            c->vadj[cur[pbase + wv]++] = ck;
            c->cadj[ccur[ck]++] = pbase + wv;
            c->owned[ck] = pbase + wv;
        }
        if (i < L - 1)
        {
            for (b = 0; b < pc; b++)
                perm[b] = b;
            sclc_shuffle(&rng, perm, pc);
            for (b = 0; b < pc; b++)
            {
                ck = (i + 1) * pc + perm[b];
                c->vadj[cur[pbase + slot[b]]++] = ck;
                c->cadj[ccur[ck]++] = pbase + slot[b];
            }
        }
    }

    for (i = 0; i < c->n; i++)
        if (cur[i] != c->voff[i + 1])
        {
            rc = -EINVAL;
            goto out;
        }
    for (i = 0; i < c->n_chk; i++)
        if (ccur[i] != c->coff[i + 1] || c->owned[i] < 0)
        {
            rc = -EINVAL;
            goto out;
        }

    /* Encoder plan: fixed stride, owned parity in the last slot. */
    c->dcs = 0;
    for (i = 0; i < c->n_chk; i++)
    {
        int d = c->coff[i + 1] - c->coff[i];

        if (d > c->dcs)
            c->dcs = d;
    }
    if (c->dcs > TBT_SCLC_CNT_MAX)
    {
        rc = -EINVAL;
        goto out;
    }
    c->padj = sclc_alloc((size_t)c->n_chk * (size_t)c->dcs * sizeof(u16));
    c->pdeg = sclc_alloc((size_t)c->n_chk);
    if (!c->padj || !c->pdeg)
        goto out;
    for (ck = 0; ck < c->n_chk; ck++)
    {
        int d = c->coff[ck + 1] - c->coff[ck];
        int out = 0;

        c->pdeg[ck] = (u8)d;
        for (e = c->coff[ck]; e < c->coff[ck + 1]; e++)
        {
            int v = c->cadj[e];

            if (v != c->owned[ck])
                c->padj[(size_t)ck * c->dcs + out++] = (u16)v;
        }
        if (out != d - 1)
        {
            rc = -EINVAL;
            goto out;
        }
        c->padj[(size_t)ck * c->dcs + d - 1] = (u16)c->owned[ck];
    }

    rc = 0;
out:
    sclc_free(vdeg);
    sclc_free(cdeg);
    sclc_free(cur);
    sclc_free(ccur);
    sclc_free(perm);
    sclc_free(slot);
    sclc_free(slot0);
    sclc_free(inv_slot);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Encoder: one 64-codeword batch
 * ------------------------------------------------------------------ */

/*
 * Batch b owns codewords b*64 .. b*64+63.  Gather segment b of every data
 * share into the payload planes, run the SC-IRA forward sweep, then
 * scatter segment b of every parity share. pc == m_shares, so block a's
 * parity variable t is share a*m_shares + t at the same position.
 */
static void sclc_encode_batch(struct tbt_sclc *c, u8 *const data[],
                              u8 *const parity[], u32 b)
{
    int s, ck;

    for (s = 0; s < c->n_shares; s++)
        seg_gather(&c->v[s * 8], data[s] + (size_t)b * 64);

    for (ck = 0; ck < c->n_chk; ck++)
    {
        const u16 *row = &c->padj[(size_t)ck * c->dcs];
        int d = c->pdeg[ck];
        u64 x = 0;
        int j;

        for (j = 0; j < d - 1; j++)
            x ^= c->v[row[j]];
        c->v[c->owned[ck]] = x;
    }

    for (s = 0; s < c->m_shares; s++)
        seg_scatter(parity[s] + (size_t)b * 64,
                    &c->v[c->n_data + s * 8]);
}

/* ------------------------------------------------------------------ *
 * Decoder: bit-sliced peeling for one 64-codeword batch
 * ------------------------------------------------------------------ */

/* Initialize the payload and parity planes from the present shares. */
static void sclc_decode_init(struct tbt_sclc *c, u8 *const data[],
                             u8 *const parity[], const u8 *data_present,
                             const u8 *parity_present, u32 b)
{
    int s, j, v;

    for (s = 0; s < c->n_shares; s++)
    {
        if (data_present[s])
        {
            seg_gather(&c->val[s * 8], data[s] + (size_t)b * 64);
            for (j = 0; j < 8; j++)
                c->unk[s * 8 + j] = 0;
        }
        else
        {
            for (j = 0; j < 8; j++)
            {
                c->val[s * 8 + j] = 0;
                c->unk[s * 8 + j] = ~(u64)0;
            }
        }
    }

    for (s = 0; s < c->m_shares; s++)
    {
        if (parity_present[s])
        {
            seg_gather(&c->val[c->n_data + s * 8],
                       parity[s] + (size_t)b * 64);
            for (j = 0; j < 8; j++)
                c->unk[c->n_data + s * 8 + j] = 0;
        }
        else
        {
            for (j = 0; j < 8; j++)
            {
                c->val[c->n_data + s * 8 + j] = 0;
                c->unk[c->n_data + s * 8 + j] = ~(u64)0;
            }
        }
    }

    memset(c->cnt, 0, (size_t)c->n_chk * 4 * sizeof(u64));
    memset(c->xr, 0, (size_t)c->n_chk * sizeof(u64));
    memset(c->act, 0, (size_t)c->n_chk * sizeof(u64));
    memset(c->resolve, 0, (size_t)c->n * sizeof(u64));
    memset(c->vbits, 0, (size_t)c->n * sizeof(u64));
    memset(c->tflag, 0, (size_t)c->n);

    for (v = 0; v < c->n; v++)
    {
        int e;

        for (e = c->voff[v]; e < c->voff[v + 1]; e++)
        {
            int ck = c->vadj[e];

            if (c->unk[v])
                cnt_add(&c->cnt[(size_t)ck * 4], c->unk[v]);
            c->xr[ck] ^= c->val[v];
        }
    }
    for (v = 0; v < c->n_chk; v++)
        c->act[v] = cnt_one(&c->cnt[(size_t)v * 4]);
}

/*
 * Run the peeling fixed point.  Each wave scans every active check, then
 * publishes the resolved variables once.  This is the prototype's wave
 * schedule: values are identical to the scalar decoder, and one 64-bit
 * operation advances all 64 codewords.
 */
static void sclc_decode_run(struct tbt_sclc *c)
{
    int wave;

    for (wave = 0; wave < c->n; wave++)
    {
        int nlist = 0;
        int ntouch = 0;
        int i;

        for (i = 0; i < c->n_chk; i++)
            if (c->act[i])
                c->stack[nlist++] = i;
        if (!nlist)
            return;

        for (i = 0; i < nlist; i++)
        {
            int ck = c->stack[i];
            int e;

            for (e = c->coff[ck]; e < c->coff[ck + 1]; e++)
            {
                int v = c->cadj[e];
                u64 res = c->act[ck] & c->unk[v];

                if (!res)
                    continue;
                if (!c->tflag[v])
                {
                    c->tflag[v] = 1;
                    c->touched[ntouch++] = v;
                }
                c->resolve[v] |= res;
                c->vbits[v] |= c->xr[ck] & res;
            }
        }

        for (i = 0; i < ntouch; i++)
        {
            int v = c->touched[i];
            u64 rv = c->resolve[v];
            u64 vb = c->vbits[v] & rv;
            int e;

            c->unk[v] &= ~rv;
            c->val[v] |= vb;
            for (e = c->voff[v]; e < c->voff[v + 1]; e++)
            {
                int ck = c->vadj[e];

                cnt_sub(&c->cnt[(size_t)ck * 4], rv);
                c->xr[ck] ^= vb;
                c->act[ck] = cnt_one(&c->cnt[(size_t)ck * 4]);
            }
            c->resolve[v] = 0;
            c->vbits[v] = 0;
            c->tflag[v] = 0;
        }
    }
}

/*
 * Residual/parity certification of a peeled batch.
 *
 * Every target payload variable being known is necessary but not
 * sufficient: check rows may still be unsatisfied when parity symbols were
 * erased, and a foreign graph (a peer with another seed) or corrupted
 * parity can leave the payload bits self-consistent while the row
 * equations fail.  Walk the checks in construction order.  The owned
 * parity symbol is the only possible unknown at its check (all payload and
 * every earlier owned symbol are known), so a known owned symbol is
 * compared with the XOR of the others and an erased one is filled from
 * them.  The comparison is the residual test: a received parity symbol
 * that disagrees with the recovered payload fails the batch instead of
 * being delivered as a complete share.
 *
 * Caller guarantees every payload variable is known.  Returns 1 when the
 * batch is certified (and every parity variable is now known), 0 when the
 * rows are inconsistent or an earlier owned symbol was not completed.
 */
static int sclc_certify(struct tbt_sclc *c)
{
    int ck, k, any = 0;

    /*
     * Common case: the peel already knows every variable.  The wave
     * updates leave each check's XOR of known neighbours in xr, so a check
     * whose bit-sliced unknown count is zero in every lane exposes its
     * residual directly.  Only when some parity is still erased is the
     * row walk needed.
     */
    for (ck = 0; ck < c->n_chk; ck++) {
        const u64 *cnt = &c->cnt[(size_t)ck * 4];
        u64 zero = ~(cnt[0] | cnt[1] | cnt[2] | cnt[3]);

        if (zero & c->xr[ck])
            return 0;
        if (cnt[0] | cnt[1] | cnt[2] | cnt[3])
            any = 1;
    }
    if (!any)
        return 1;

    for (ck = 0; ck < c->n_chk; ck++) {
        int own = c->owned[ck];
        int d = c->pdeg[ck];
        u64 unknown = c->unk[own];
        u64 s = 0;

        for (k = 0; k < d - 1; k++) {
            int v = c->padj[(size_t)ck * c->dcs + k];

            if (c->unk[v])
                return 0; /* an earlier owned symbol was not completed */
            s ^= c->val[v];
        }
        if ((c->val[own] ^ s) & ~unknown)
            return 0;
        c->val[own] = (c->val[own] & ~unknown) | (s & unknown);
        c->unk[own] = 0;
    }
    return 1;
}

/* Returns 1 when every payload variable is known, 0 otherwise. */
static int sclc_decode_batch(struct tbt_sclc *c, u8 *const data[],
                             u8 *const parity[], const u8 *data_present,
                             const u8 *parity_present, u32 b)
{
    int s, v;

    for (s = 0; s < c->n_shares; s++)
        if (!data_present[s])
            break;
    if (s == c->n_shares)
        return 1;

    sclc_decode_init(c, data, parity, data_present, parity_present, b);
    sclc_decode_run(c);

    for (v = 0; v < c->n_data; v++)
        if (c->unk[v])
            return 0;
    if (!sclc_certify(c))
        return 0;
    for (s = 0; s < c->n_shares; s++)
        if (!data_present[s])
            seg_scatter(data[s] + (size_t)b * 64, &c->val[s * 8]);
    return 1;
}

/* ------------------------------------------------------------------ *
 * Codec object lifecycle
 * ------------------------------------------------------------------ */

static void *sclc_new(u16 n, u16 m, u16 k, u16 w, size_t s, u16 d, u64 nonce)
{
    struct tbt_sclc *c;
    u16 rate;

    if (!n || !m || (u32)n + m != TBT_SCLC_SHARES)
        return NULL;
    if (s != TBT_SCLC_SHARE || w != TBT_SCLC_BLOCK || d != 0)
        return NULL;
    rate = (u16)((u32)n * 1000u / (u32)(n + m));
    if (k != 0 && k != rate)
        return NULL;

    c = sclc_alloc(sizeof(*c));
    if (!c)
        return NULL;
    c->n_shares = n;
    c->m_shares = m;
    c->k = k ? k : rate;
    c->w = w;
    c->s = s;
    c->nonce = nonce;

    if (sclc_build_graph(c) != 0)
    {
        sclc_graph_free(c);
        sclc_free(c);
        return NULL;
    }

    /* Encoder state and the share-level bookkeeping are always needed (a
     * transmit codec exposes is_known()/advance() before any decode).  The
     * bit-sliced decoder scratch is allocated by sclc_ensure_decode() on
     * the first decode, so a TX codec never pays for it. */
    c->v = sclc_alloc((size_t)c->n * sizeof(u64));
    c->known = sclc_alloc(n);
    c->batch_ok = sclc_alloc((size_t)c->batches);
    if (!c->v || !c->known || !c->batch_ok)
    {
        sclc_graph_free(c);
        sclc_free(c->v);
        sclc_free(c->known);
        sclc_free(c->batch_ok);
        sclc_free(c);
        return NULL;
    }
    return c;
}

/* Allocate the bit-sliced decoder scratch on first use.  Idempotent;
 * returns -ENOMEM without changing the object on failure. */
static int sclc_ensure_decode(struct tbt_sclc *c)
{
    if (c->dec_ready)
        return 0;
    c->unk = sclc_alloc((size_t)c->n * sizeof(u64));
    c->val = sclc_alloc((size_t)c->n * sizeof(u64));
    c->cnt = sclc_alloc((size_t)c->n_chk * 4 * sizeof(u64));
    c->xr = sclc_alloc((size_t)c->n_chk * sizeof(u64));
    c->act = sclc_alloc((size_t)c->n_chk * sizeof(u64));
    c->resolve = sclc_alloc((size_t)c->n * sizeof(u64));
    c->vbits = sclc_alloc((size_t)c->n * sizeof(u64));
    c->stack = sclc_alloc((size_t)c->n_chk * sizeof(int));
    c->touched = sclc_alloc((size_t)c->n * sizeof(int));
    c->tflag = sclc_alloc((size_t)c->n);
    if (!c->unk || !c->val || !c->cnt || !c->xr || !c->act ||
        !c->resolve || !c->vbits || !c->stack || !c->touched || !c->tflag)
    {
        sclc_free(c->unk);
        sclc_free(c->val);
        sclc_free(c->cnt);
        sclc_free(c->xr);
        sclc_free(c->act);
        sclc_free(c->resolve);
        sclc_free(c->vbits);
        sclc_free(c->stack);
        sclc_free(c->touched);
        sclc_free(c->tflag);
        c->unk = NULL;
        c->val = NULL;
        c->cnt = NULL;
        c->xr = NULL;
        c->act = NULL;
        c->resolve = NULL;
        c->vbits = NULL;
        c->stack = NULL;
        c->touched = NULL;
        c->tflag = NULL;
        return -ENOMEM;
    }
    c->dec_ready = 1;
    return 0;
}

static void sclc_free_ctx(void *ctx)
{
    struct tbt_sclc *c = ctx;

    if (!c)
        return;
    sclc_graph_free(c);
    sclc_free(c->v);
    sclc_free(c->unk);
    sclc_free(c->val);
    sclc_free(c->cnt);
    sclc_free(c->xr);
    sclc_free(c->act);
    sclc_free(c->resolve);
    sclc_free(c->vbits);
    sclc_free(c->stack);
    sclc_free(c->touched);
    sclc_free(c->tflag);
    sclc_free(c->known);
    sclc_free(c->batch_ok);
    sclc_free(c);
}

static void sclc_update_frontier(struct tbt_sclc *c)
{
    u32 i;

    for (i = 0; i < c->n_shares; i++)
        if (!c->known[i])
            break;
    c->frontier = i;
    c->stuck_at = i;
}

static u32 sclc_parity_count(const void *ctx, u32 L)
{
    const struct tbt_sclc *c = ctx;

    if (!c || L != c->n_shares)
        return 0;
    return c->m_shares;
}

static u32 sclc_coeffs(const void *ctx, u32 g, u32 r, u32 *idx, u32 max)
{
    (void)ctx;
    (void)g;
    (void)r;
    (void)idx;
    (void)max;
    return (u32)-ENOSYS;
}

static int sclc_encode(void *ctx, u8 *data[], u32 L, u8 *parity[])
{
    struct tbt_sclc *c = ctx;
    u32 b, i;

    if (!c || !data || !parity || L != c->n_shares)
        return -EINVAL;
    for (i = 0; i < L; i++)
        if (!data[i])
            return -EINVAL;
    for (i = 0; i < c->m_shares; i++)
        if (!parity[i])
            return -EINVAL;

    for (b = 0; b < (u32)c->batches; b++)
        sclc_encode_batch(c, data, parity, b);
    return 0;
}

static int sclc_decode(void *ctx, u8 *data[], u8 *parity[],
                       const u8 *data_present, const u8 *parity_present,
                       u32 L)
{
    struct tbt_sclc *c = ctx;
    u32 b, s;
    u32 recovered = 0;
    u32 missing = 0;

    if (!c || !data || !parity || !data_present || !parity_present ||
        L != c->n_shares)
        return -EINVAL;
    if (sclc_ensure_decode(c) != 0)
        return -ENOMEM;

    for (s = 0; s < c->n_shares; s++)
    {
        c->known[s] = data_present[s] ? 1 : 0;
        if (data_present[s] && !data[s])
            return -EINVAL;
        if (!data_present[s])
        {
            missing++;
            if (!data[s])
                return -EINVAL;
        }
    }
    for (s = 0; s < c->m_shares; s++)
        if (parity_present[s] && !parity[s])
            return -EINVAL;

    memset(c->batch_ok, 0, (size_t)c->batches);
    for (b = 0; b < (u32)c->batches; b++)
    {
        if (!sclc_decode_batch(c, data, parity, data_present,
                               parity_present, b))
            break;
        c->batch_ok[b] = 1;
    }
    /*
     * Every batch runs the same graph over the same share->variable map,
     * and a missing share becomes known only after every batch decodes.
     * A failed batch therefore makes that outcome impossible no matter
     * what the remaining batches hold; decoding them would only fill
     * buffers the caller must not trust.  (A batch can also fail the
     * residual check on corrupted/foreign parity, which is likewise a
     * block-level failure for missing shares.)
     */

    for (s = 0; s < c->n_shares; s++)
    {
        if (c->known[s])
            continue;
        for (b = 0; b < (u32)c->batches; b++)
            if (!c->batch_ok[b])
                break;
        if (b == (u32)c->batches)
        {
            c->known[s] = 1;
            recovered++;
        }
    }
    c->recovered_count += recovered;
    sclc_update_frontier(c);
    if (!recovered && missing)
        return -EAGAIN;
    return (int)recovered;
}

static int sclc_advance(void *ctx, int kind, u32 index)
{
    struct tbt_sclc *c = ctx;

    if (!c || index >= c->n_shares)
        return -EINVAL;
    if (kind == 0)
        c->known[index] = 1;
    else if (kind == 1 || kind == 2)
        ; /* data missing stays unknown; parity presence is per decode */
    else
        return -EINVAL;
    sclc_update_frontier(c);
    return 0;
}

static u32 sclc_frontier(const void *ctx)
{
    const struct tbt_sclc *c = ctx;

    return c ? c->frontier : 0;
}

static u32 sclc_stuck_at(const void *ctx)
{
    const struct tbt_sclc *c = ctx;

    return c ? c->stuck_at : 0;
}

static u32 sclc_recovered(const void *ctx)
{
    const struct tbt_sclc *c = ctx;

    return c ? c->recovered_count : 0;
}

static int sclc_is_known(const void *ctx, u32 index)
{
    const struct tbt_sclc *c = ctx;

    if (!c || index >= c->n_shares)
        return 0;
    return c->known[index] != 0;
}

const struct tbt_fec_ops tbt_fec_ops_sclc = {
    .name = "sc-ldpc",
    .mode = 3,
    .new = sclc_new,
    .free = sclc_free_ctx,
    .parity_count = sclc_parity_count,
    .coeffs = sclc_coeffs,
    .encode = sclc_encode,
    .decode = sclc_decode,
    .advance = sclc_advance,
    .frontier = sclc_frontier,
    .stuck_at = sclc_stuck_at,
    .recovered = sclc_recovered,
    .is_known = sclc_is_known,
};
