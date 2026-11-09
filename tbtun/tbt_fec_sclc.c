/* tbt_fec_sclc.c - downlink SC-IRA codec (rate 0.50, block 64/128/256, L 8).
 *
 * Construction and message semantics follow review/fec-prototype/sc_ldpc
 * (SC_TERM_ACC).  Only the rate 0.50 path is kept: every group has n data
 * and n parity shares, and the coupling block is w = 2n.  n is 32, 64 or
 * 128 (block 64/128/256), so a session can split its 256 carriers into
 * 1, 2 or 4 independent codec groups.  A rate change is a later, separate
 * graph.  The encoder is the bit64 forward sweep; the decoder is the
 * bit-sliced peeling decoder, one 64-codeword batch at a time.  A share
 * carries one 8-bit segment of all 8192 codewords, so a lost share erases
 * the same segment in every batch and the 64 codewords of a batch share one
 * mask.  See tbt_fec_sclc.h for the exact byte mapping.
 *
 * No allocation happens on the per-share path: the code object owns the
 * graph, the encoder plan, and one batch of decoder scratch.
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

static void seg_gather(u64 plane[8], const u8 *seg)
{
    int k, j;

    for (j = 0; j < 8; j++)
        plane[j] = 0;
    for (k = 0; k < 8; k++)
    {
        u64 y = sclc_transpose8(sclc_load8(seg + k * 8));

        for (j = 0; j < 8; j++)
            plane[j] |= (u64)((y >> (8 * j)) & 0xffu) << (8 * k);
    }
}

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
    u16 m_shares; /* parity shares per FEC block */
    u16 n_batches; /* 64-codeword batches in one share: s / SEG_BYTES */
    u16 k;        /* rate in parts per thousand, 500 */
    u16 w;        /* codeword block, 2 * n_shares */
    size_t s;     /* share bytes, 8192 */
    u64 nonce;

    int m;      /* lifting factor, w / 2 */
    int n;      /* variables per codeword, 8 * (n_shares + m_shares) */
    int n_data; /* 8 * n_shares */
    int n_par;  /* 8 * m_shares */
    int n_chk;  /* 8 * m_shares */
    int edges;
    int dcs; /* encoder plan stride, max check degree */

    int *voff; /* n + 1 */
    int *coff; /* n_chk + 1 */
    int *vadj; /* edges, check ids */
    int *cadj; /* edges, variable ids */
    int *owned;  /* n_chk: parity variable this check solves */
    u16 *padj;   /* n_chk * dcs: XOR list, owned parity last */
    u8 *pdeg;    /* n_chk: check degree */

    u64 *v;       /* n: encoder scratch, payload then parity */
    u64 *unk;     /* n: bit-sliced unknown mask */
    u64 *val;     /* n: bit-sliced known value */
    u64 *cnt;     /* n_chk * 3: bit-sliced unknown count */
    u64 *xr;      /* n_chk: XOR of known neighbours */
    u64 *act;     /* n_chk: exactly-one-unknown mask */
    u64 *resolve; /* n: resolved mask for this wave */
    u64 *vbits;   /* n: resolved value for this wave */
    int *stack;   /* n_chk */
    int *touched; /* n */
    u8 *tflag;    /* n */

    u8 *known;     /* n_shares: data share known */
    u8 *batch_ok;  /* n_shares: batch decoded */
    u32 recovered_count;
    u32 frontier;
    u32 stuck_at;
};

/* ------------------------------------------------------------------ *
 * Bit-sliced 3-plane counters (check degree <= 7)
 * ------------------------------------------------------------------ */

static void cnt_add(u64 c[3], u64 m)
{
    u64 n0 = c[0] ^ m;
    u64 k0 = c[0] & m;
    u64 n1 = c[1] ^ k0;
    u64 k1 = c[1] & k0;

    c[0] = n0;
    c[1] = n1;
    c[2] ^= k1;
}

static void cnt_sub(u64 c[3], u64 m)
{
    u64 n0 = c[0] ^ m;
    u64 b0 = ~c[0] & m;
    u64 n1 = c[1] ^ b0;
    u64 b1 = ~c[1] & b0;

    c[0] = n0;
    c[1] = n1;
    c[2] ^= b1;
}

static u64 cnt_one(const u64 c[3])
{
    return c[0] & ~c[1] & ~c[2];
}

/* ------------------------------------------------------------------ *
 * Graph construction: SC_TERM_ACC, rate 0.50
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
 * Payload block i owns check supernode i and connects to supernodes
 * (i .. i+4) mod L, degree 5.  Parity block a owns check a and feeds
 * check a+1, so the parity submatrix is a non-wrapping double diagonal.
 * Every edge inside a supernode is lifted by an independent permutation,
 * which keeps the graph free of parallel edges and makes owned[] a
 * bijection.  The check order is the construction order: supernode a
 * holds checks a*m .. a*m+m-1.
 */
static int sclc_build_graph(struct tbt_sclc *c)
{
    struct sclc_rng rng;
    int *vdeg = NULL, *cdeg = NULL, *cur = NULL, *perm = NULL;
    int i, r, b, e, ck;
    int rc = -ENOMEM;

    /* The share mapping and the graph must describe the same codeword: a
     * data share is eight payload variables, so n_data = 8 * n_shares, and
     * the parity side is the same size at rate 0.50. */
    c->m = c->w / 2;
    c->n_data = TBT_SCLC_L * c->m;
    c->n_par = TBT_SCLC_L * c->m;
    c->n_chk = TBT_SCLC_L * c->m;
    c->n = c->n_data + c->n_par;
    if (c->n_data != 8 * c->n_shares || c->n_par != 8 * c->m_shares)
        return -EINVAL;
    c->edges = TBT_SCLC_L * c->m * 5 /* payload */ +
               c->n_par + (TBT_SCLC_L - 1) * c->m; /* parity */

    vdeg = sclc_alloc((size_t)c->n * sizeof(int));
    cdeg = sclc_alloc((size_t)c->n_chk * sizeof(int));
    perm = sclc_alloc((size_t)c->m * sizeof(int));
    c->voff = sclc_alloc((size_t)(c->n + 1) * sizeof(int));
    c->coff = sclc_alloc((size_t)(c->n_chk + 1) * sizeof(int));
    c->vadj = sclc_alloc((size_t)c->edges * sizeof(int));
    c->cadj = sclc_alloc((size_t)c->edges * sizeof(int));
    c->owned = sclc_alloc((size_t)c->n_chk * sizeof(int));
    if (!vdeg || !cdeg || !perm || !c->voff || !c->coff || !c->vadj ||
        !c->cadj || !c->owned)
        goto out;

    for (i = 0; i < c->n; i++)
        vdeg[i] = 0;
    for (i = 0; i < c->n_chk; i++)
        cdeg[i] = 0;

    /* Degree pass.  Each check in supernode j receives one payload edge
     * from every payload block j-4 .. j (five blocks) and one diagonal
     * parity edge; checks j >= 1 also receive the accumulator edge of
     * parity block j-1. */
    for (i = 0; i < TBT_SCLC_L; i++)
    {
        for (b = 0; b < c->m; b++)
            vdeg[i * c->m + b] = 5;
        for (r = 0; r < 5; r++)
            for (b = 0; b < c->m; b++)
                cdeg[((i + r) % TBT_SCLC_L) * c->m + b]++;
        for (b = 0; b < c->m; b++)
            vdeg[c->n_data + i * c->m + b] =
                (i < TBT_SCLC_L - 1) ? 2 : 1;
        for (b = 0; b < c->m; b++)
            cdeg[i * c->m + b]++;
        if (i < TBT_SCLC_L - 1)
            for (b = 0; b < c->m; b++)
                cdeg[(i + 1) * c->m + b]++;
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
    if (!cur)
        goto out;
    memcpy(cur, c->voff, (size_t)c->n * sizeof(int));

    sclc_rng_seed(&rng, c->nonce);

    for (i = 0; i < TBT_SCLC_L; i++)
    {
        int vbase = i * c->m;

        for (r = 0; r < 5; r++)
        {
            int ckbase = ((i + r) % TBT_SCLC_L) * c->m;

            for (b = 0; b < c->m; b++)
                perm[b] = b;
            sclc_shuffle(&rng, perm, c->m);
            for (b = 0; b < c->m; b++)
                c->vadj[cur[vbase + b]++] = ckbase + perm[b];
        }
    }
    for (i = 0; i < TBT_SCLC_L; i++)
    {
        int pbase = c->n_data + i * c->m;

        for (b = 0; b < c->m; b++)
            perm[b] = b;
        sclc_shuffle(&rng, perm, c->m);
        for (b = 0; b < c->m; b++)
        {
            ck = i * c->m + perm[b];
            c->vadj[cur[pbase + b]++] = ck;
            c->owned[ck] = pbase + b;
        }
        if (i < TBT_SCLC_L - 1)
        {
            for (b = 0; b < c->m; b++)
                perm[b] = b;
            sclc_shuffle(&rng, perm, c->m);
            for (b = 0; b < c->m; b++)
                c->vadj[cur[pbase + b]++] = (i + 1) * c->m + perm[b];
        }
    }
    for (i = 0; i < c->n; i++)
        if (cur[i] != c->voff[i + 1])
        {
            rc = -EINVAL;
            goto out;
        }

    /* Check side: invert the variable adjacency. */
    for (i = 0; i < c->n_chk; i++)
        cur[i] = c->coff[i];
    for (i = 0; i < c->n; i++)
    {
        int k;

        for (k = c->voff[i]; k < c->voff[i + 1]; k++)
        {
            ck = c->vadj[k];
            c->cadj[cur[ck]++] = i;
        }
    }

    /* Encoder plan: fixed stride, owned parity in the last slot. */
    c->dcs = 0;
    for (i = 0; i < c->n_chk; i++)
    {
        int d = c->coff[i + 1] - c->coff[i];

        if (d > c->dcs)
            c->dcs = d;
    }
    c->padj = sclc_alloc((size_t)c->n_chk * (size_t)c->dcs * sizeof(u16));
    c->pdeg = sclc_alloc((size_t)c->n_chk);
    if (!c->padj || !c->pdeg)
        goto out;
    for (ck = 0; ck < c->n_chk; ck++)
    {
        int d = c->coff[ck + 1] - c->coff[ck];
        int j = 0;

        c->pdeg[ck] = (u8)d;
        for (e = c->coff[ck]; e < c->coff[ck + 1]; e++)
        {
            int v = c->cadj[e];

            if (v != c->owned[ck])
                c->padj[(size_t)ck * c->dcs + j++] = (u16)v;
        }
        if (j != d - 1)
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
    sclc_free(perm);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Encoder: one 64-codeword batch
 * ------------------------------------------------------------------ */

/*
 * Batch b owns codewords b*64 .. b*64+63.  Gather segment b of every data
 * share into the payload planes, run the SC-IRA forward sweep, then
 * scatter the parity planes into segment b of every parity share.
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

    for (s = 0; s < c->n_shares; s++)
        seg_scatter(parity[s] + (size_t)b * 64, &c->v[c->n_data + s * 8]);
}

/* ------------------------------------------------------------------ *
 * Decoder: bit-sliced peeling for one 64-codeword batch
 * ------------------------------------------------------------------ */

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

    memset(c->cnt, 0, (size_t)c->n_chk * 3 * sizeof(u64));
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
                cnt_add(&c->cnt[(size_t)ck * 3], c->unk[v]);
            c->xr[ck] ^= c->val[v];
        }
    }
    for (v = 0; v < c->n_chk; v++)
        c->act[v] = cnt_one(&c->cnt[(size_t)v * 3]);
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

                cnt_sub(&c->cnt[(size_t)ck * 3], rv);
                c->xr[ck] ^= vb;
                c->act[ck] = cnt_one(&c->cnt[(size_t)ck * 3]);
            }
            c->resolve[v] = 0;
            c->vbits[v] = 0;
            c->tflag[v] = 0;
        }
    }
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

    /* A group is rate 0.50: n == m, block = 2n, and n is one of the three
     * shipped group sizes (256, 128 or 64 carriers for data+parity). */
    if (!n || !m || n != m || n > TBT_SCLC_MAX_SEGS ||
        (u32)n + m > TBT_FEC_MAX_SHARES)
        return NULL;
    if (n != 32 && n != 64 && n != 128)
        return NULL;
    if (!w || w != (u16)(2 * n) || w > 2 * TBT_SCLC_MAX_SEGS)
        return NULL;
    if (s != TBT_SCLC_SHARE || d != 0)
        return NULL;
    if (s % TBT_SCLC_SEG_BYTES || !s / TBT_SCLC_SEG_BYTES ||
        s / TBT_SCLC_SEG_BYTES > 0xffffu)
        return NULL;
    if (k != 0 && k != TBT_SCLC_RATE)
        return NULL;

    c = sclc_alloc(sizeof(*c));
    if (!c)
        return NULL;
    c->n_shares = n;
    c->m_shares = m;
    c->n_batches = (u16)(s / TBT_SCLC_SEG_BYTES);
    c->k = k ? k : TBT_SCLC_RATE;
    c->w = w;
    c->s = s;
    c->nonce = nonce;

    if (sclc_build_graph(c) != 0)
    {
        sclc_graph_free(c);
        sclc_free(c);
        return NULL;
    }

    c->v = sclc_alloc((size_t)c->n * sizeof(u64));
    c->unk = sclc_alloc((size_t)c->n * sizeof(u64));
    c->val = sclc_alloc((size_t)c->n * sizeof(u64));
    c->cnt = sclc_alloc((size_t)c->n_chk * 3 * sizeof(u64));
    c->xr = sclc_alloc((size_t)c->n_chk * sizeof(u64));
    c->act = sclc_alloc((size_t)c->n_chk * sizeof(u64));
    c->resolve = sclc_alloc((size_t)c->n * sizeof(u64));
    c->vbits = sclc_alloc((size_t)c->n * sizeof(u64));
    c->stack = sclc_alloc((size_t)c->n_chk * sizeof(int));
    c->touched = sclc_alloc((size_t)c->n * sizeof(int));
    c->tflag = sclc_alloc((size_t)c->n);
    c->known = sclc_alloc(n);
    c->batch_ok = sclc_alloc(c->n_batches);
    if (!c->v || !c->unk || !c->val || !c->cnt || !c->xr || !c->act ||
        !c->resolve || !c->vbits || !c->stack || !c->touched || !c->tflag ||
        !c->known || !c->batch_ok)
    {
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
        return NULL;
    }
    return c;
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
        if (!data[i] || !parity[i])
            return -EINVAL;

    for (b = 0; b < c->n_batches; b++)
        sclc_encode_batch(c, data, parity, b);
    return 0;
}

/* Whether the parity share that covers data share i is present. */
static int sclc_parity_source(const struct tbt_sclc *c, const u8 *present,
                              u32 i)
{
    if (i < c->m_shares && present[i])
        return (int)i;
    return -1;
}

static int sclc_decode(void *ctx, u8 *data[], u8 *parity[],
                       const u8 *data_present, const u8 *parity_present,
                       u32 L)
{
    struct tbt_sclc *c = ctx;
    u8 *pp[TBT_SCLC_MAX_SEGS];
    u8 pp_present[TBT_SCLC_MAX_SEGS];
    u32 b, s;
    u32 recovered = 0;
    u32 missing = 0;

    if (!c || !data || !parity || !data_present || !parity_present ||
        L != c->n_shares)
        return -EINVAL;

    for (s = 0; s < c->n_shares; s++)
    {
        int src = sclc_parity_source(c, parity_present, s);

        pp_present[s] = src >= 0;
        pp[s] = src >= 0 ? parity[src] : NULL;
        c->known[s] = data_present[s] ? 1 : 0;
        if (!data_present[s])
        {
            missing++;
            if (!data[s])
                return -EINVAL;
        }
    }

    memset(c->batch_ok, 0, c->n_batches);
    for (b = 0; b < c->n_batches; b++)
        if (sclc_decode_batch(c, data, pp, data_present, pp_present, b))
            c->batch_ok[b] = 1;

    for (s = 0; s < c->n_shares; s++)
    {
        if (c->known[s])
            continue;
        for (b = 0; b < c->n_batches; b++)
            if (!c->batch_ok[b])
                break;
        if (b == c->n_batches)
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
