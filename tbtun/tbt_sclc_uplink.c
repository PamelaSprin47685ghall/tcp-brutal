/* tbt_sclc_uplink.c - uplink grouped GF(16) SC-IRA block codec.
 *
 * One group FEC block carries n data shares and m parity shares of S bytes.
 * The block is C codewords of the SC_TERM_ACC GF(16) code.  Encoding and
 * decoding walk the codewords one at a time and reuse one codec context per
 * group, so the per-group state is one graph plus 4*n scratch bytes.
 *
 * Interleaved mapping (production): payload symbol j of codeword c goes to
 * data share (j + c) mod n; parity symbol j goes to parity share
 * (j + c) mod m.  Offsets inside a share are assigned in (codeword, j)
 * order, so both sides derive them from the same running counters and no
 * table travels.  A lost share erases every n-th (or m-th) symbol of each
 * codeword, a uniform mask instead of a burst.  C is a multiple of n, so
 * every data share fills exactly.
 *
 * Contiguous mapping is the A/B control: codeword c occupies payload
 * positions [c*n_data, (c+1)*n_data).  It loses whole codewords when a data
 * share is lost; the host test measures the difference.
 *
 * Encoding is bit-sliced: a batch of 64 codewords shares the graph and the
 * check order, so one call to gf16_encode_planes() carries four uint64
 * planes per variable and replaces 64 scalar sweeps.  The gather and
 * scatter remain per codeword, which keeps the share offsets and therefore
 * the wire layout identical to the scalar path.
 *
 * Decoding keeps the scalar peel (each codeword erases a different symbol
 * set) but peels GF16_BATCH codewords in lockstep: independent adjacency
 * walks overlap their cache misses.  Both paths write byte-identical
 * output to the scalar reference.
 *
 * This file does not touch tbt_fec_data.c; the data plane calls the ops
 * table or the direct API.
 */

#include "tbt_sclc_uplink.h"
#include "tbt_sc_ldpc.h"
#include "tbt_sc_ldpc_gf16.h"

#define SCLC_SYM_PER_SHARE (TBT_FEC_S * 2) /* 8192 bytes = 16384 GF(16) */
#define SCLC_COEF_SALT 0xD1B54A32D192ED03ULL

struct sclc_group
{
    gf16_code *code;
    /* One batch of codeword slots for the lockstep decoder.  recv_b and
     * out_b share one buffer: the batch init reads every slot before the
     * peel writes any output. */
    u8 *recv_b;    /* GF16_BATCH * nsym symbols */
    u8 *erased_b;  /* GF16_BATCH * nsym bytes */
    u8 *out_b;     /* alias of recv_b */
    gf16_dec_stat *st_b; /* GF16_BATCH */
    u64 *vp;       /* n * 4 bit planes, 64-lane encoder scratch */
    u8 *codebuf;   /* n symbols, encoder scratch */
    u8 *share_bad; /* n+m bytes, 1 = a failed codeword touches the share */
    u8 *share_known;
    u32 *offp;     /* n counters, payload share offsets */
    u32 *offq;     /* m counters, parity share offsets */
    u32 *woff;     /* n counters, scatter offsets */
    struct tbt_sclc_stat st;
};

struct tbt_sclc_uplink
{
    u16 groups;
    u16 n;
    u16 m;
    u16 shares;
    u16 codes;
    u32 n_data;
    u32 n_par;
    u32 nsym;
    u16 max_core;
    u8 mapping;
    u64 seed;
    u8 *plab; /* random mapping: C*n_data share labels */
    u8 *qlab; /* random mapping: C*n_par share labels */
    struct sclc_group *g;
    u32 mem;
};

static inline u8 sclc_get_sym(const u8 *buf, u32 off)
{
    return (u8)((buf[off >> 1] >> ((off & 1u) * 4)) & 0x0fu);
}

static inline void sclc_put_sym(u8 *buf, u32 off, u8 v)
{
    u32 byte = off >> 1;
    u8 shift = (u8)((off & 1u) * 4);

    buf[byte] = (u8)((buf[byte] & (u8)~(0x0fu << shift)) |
                     (u8)((v & 0x0fu) << shift));
}

/* Payload share/offset for codeword `code`, symbol j.  Interleaved uses the
 * running counters, contiguous uses the flat position. */
static inline u16 sclc_payload_share(const struct tbt_sclc_uplink *u, u32 code,
                                     u32 j)
{
    if (u->mapping == TBT_SCLC_MAP_RANDOM)
        return u->plab[(size_t)code * u->n_data + j];
    if (u->mapping == TBT_SCLC_MAP_CONTIGUOUS)
        return (u16)((code * u->n_data + j) / SCLC_SYM_PER_SHARE);
    return (u16)((j + code) % u->n);
}

static inline u32 sclc_payload_off(const struct tbt_sclc_uplink *u, u32 code,
                                   u32 j)
{
    if (u->mapping == TBT_SCLC_MAP_CONTIGUOUS)
        return (code * u->n_data + j) % SCLC_SYM_PER_SHARE;
    return 0; /* caller uses the running counter */
}

static inline u16 sclc_parity_share(const struct tbt_sclc_uplink *u, u32 code,
                                    u32 j)
{
    if (u->mapping == TBT_SCLC_MAP_RANDOM)
        return u->qlab[(size_t)code * u->n_par + j];
    return (u16)((j + code) % u->m);
}

static uint64_t sclc_splitmix(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void sclc_shuffle(u8 *lab, u32 n, u64 seed)
{
    uint64_t x = seed;
    u32 i;

    for (i = n - 1; i > 0; i--)
    {
        u32 j = (u32)(sclc_splitmix(&x) % (i + 1));
        u8 t = lab[i];

        lab[i] = lab[j];
        lab[j] = t;
    }
}

static int sclc_labels_fit(const u8 *lab, size_t count, u32 shares)
{
    u16 cnt[TBT_FEC_MAX_SHARES];
    size_t i;
    u32 s;

    memset(cnt, 0, (size_t)shares * sizeof(cnt[0]));
    for (i = 0; i < count; i++)
    {
        if (cnt[lab[i]] < 0xffffu)
            cnt[lab[i]]++;
    }
    for (s = 0; s < shares; s++)
        if (cnt[s] > SCLC_SYM_PER_SHARE)
            return 0;
    return 1;
}

/* Build the seeded share-label tables for TBT_SCLC_MAP_RANDOM.  Each
 * codeword gets the same multiset (every share floor(n_data/n) times plus a
 * rotating remainder) in a different random order, so every data share
 * fills exactly and a lost share erases a random-looking slice. */
static int sclc_build_labels(struct tbt_sclc_uplink *u)
{
    size_t psz = (size_t)u->codes * u->n_data;
    size_t qsz = (size_t)u->codes * u->n_par;
    u32 c;

    u->plab = sclc_zalloc(psz);
    u->qlab = sclc_zalloc(qsz);
    if (!u->plab || !u->qlab)
        return -ENOMEM;
    u->mem += (u32)(psz + qsz);

    for (c = 0; c < u->codes; c++)
    {
        u8 *pl = u->plab + (size_t)c * u->n_data;
        u8 *ql = u->qlab + (size_t)c * u->n_par;
        u32 base = u->n_data / u->n, rem = u->n_data % u->n;
        u32 baseq = u->n_par / u->m, remq = u->n_par % u->m;
        u32 s, r, k = 0;

        for (s = 0; s < u->n; s++)
            for (r = 0; r < base; r++)
                pl[k++] = (u8)s;
        for (r = 0; r < rem; r++)
            pl[k++] = (u8)((c * rem + r) % u->n);
        sclc_shuffle(pl, u->n_data,
                     u->seed ^ ((u64)c * 0x9E3779B97F4A7C15ULL));

        k = 0;
        for (s = 0; s < u->m; s++)
            for (r = 0; r < baseq; r++)
                ql[k++] = (u8)s;
        for (r = 0; r < remq; r++)
            ql[k++] = (u8)((c * remq + r) % u->m);
        sclc_shuffle(ql, u->n_par,
                     u->seed ^ ((u64)c * 0xD1B54A32D192ED03ULL) ^
                         0xA5A5A5A5A5A5A5A5ULL);
    }
    if (!sclc_labels_fit(u->plab, psz, u->n) ||
        !sclc_labels_fit(u->qlab, qsz, u->m))
        return -EINVAL;
    return 0;
}

/* Count the parity symbols each share receives under the interleaved map.
 * Returns the maximum, or 0 when a share would overflow. */
static u32 sclc_parity_counts(const struct tbt_sclc_uplink *u, u16 *cnt)
{
    u32 c, j, max = 0;

    memset(cnt, 0, (size_t)u->m * sizeof(*cnt));
    for (c = 0; c < u->codes; c++)
        for (j = 0; j < u->n_par; j++)
            cnt[(j + c) % u->m]++;
    for (j = 0; j < u->m; j++)
        if (cnt[j] > max)
            max = cnt[j];
    return max;
}

static void sclc_mark_bad(struct tbt_sclc_uplink *u, struct sclc_group *g,
                          u32 code)
{
    u32 first, last, sh;

    if (u->mapping == TBT_SCLC_MAP_INTERLEAVED ||
        u->mapping == TBT_SCLC_MAP_RANDOM)
    {
        /* The (j + c) mod n cycle touches every data share, and n_par >= m
         * makes the parity cycle touch every parity share. */
        memset(g->share_bad, 1, u->shares);
        return;
    }

    first = code * u->n_data;
    last = first + u->n_data - 1;
    for (sh = first / SCLC_SYM_PER_SHARE;
         sh <= last / SCLC_SYM_PER_SHARE; sh++)
        g->share_bad[sh] = 1;

    if (!u->n_par)
        return;
    first = code * u->n_par;
    last = first + u->n_par - 1;
    for (sh = first / SCLC_SYM_PER_SHARE;
         sh <= last / SCLC_SYM_PER_SHARE; sh++)
        g->share_bad[u->n + sh] = 1;
}

struct tbt_sclc_uplink *tbt_sclc_uplink_new(u16 groups,
                                            const struct tbt_sclc_params *par)
{
    struct tbt_sclc_uplink *u;
    u32 n_data, n_par, codes, i;
    u16 share_bytes;

    if (!par || !groups || groups > TBT_SCLC_MAX_GROUPS)
        return NULL;
    if (!par->n || !par->m || (u32)par->n + par->m > TBT_FEC_MAX_SHARES)
        return NULL;
    if (groups * ((u32)par->n + par->m) > TBT_FEC_MAX_SHARES)
        return NULL;
    if (par->block < 16 || (par->block & 1) || par->block > 8192)
        return NULL;
    if (par->L < 2 || par->L > 4096)
        return NULL;
    if (!par->rate_num || !par->rate_den ||
        par->rate_num > par->rate_den)
        return NULL;
    if (par->mapping != TBT_SCLC_MAP_INTERLEAVED &&
        par->mapping != TBT_SCLC_MAP_CONTIGUOUS &&
        par->mapping != TBT_SCLC_MAP_RANDOM)
        return NULL;

    n_data = (u32)par->L * (par->block / 2);
    if (!n_data)
        return NULL;
    if (((u32)par->n * SCLC_SYM_PER_SHARE) % n_data)
        return NULL; /* the payload must split into whole codewords */
    codes = ((u32)par->n * SCLC_SYM_PER_SHARE) / n_data;
    if (!codes || codes > 4096)
        return NULL;
    n_par = n_data * (par->rate_den - par->rate_num) / par->rate_num;
    if (!n_par)
        return NULL;

    share_bytes = (u16)((u32)par->n + par->m);

    u = sclc_zalloc(sizeof(*u));
    if (!u)
        return NULL;
    u->groups = groups;
    u->n = par->n;
    u->m = par->m;
    u->shares = share_bytes;
    u->codes = (u16)codes;
    u->n_data = n_data;
    u->n_par = n_par;
    u->nsym = n_data + n_par;
    u->max_core = par->max_core;
    u->mapping = par->mapping;
    u->seed = par->seed;
    u->mem = sizeof(*u);

    if (u->mapping == TBT_SCLC_MAP_INTERLEAVED && codes % par->n)
        goto fail; /* exact fill needs C a multiple of n */

    u->g = sclc_zalloc((size_t)groups * sizeof(*u->g));
    if (!u->g)
        goto fail;
    u->mem += (u32)groups * (u32)sizeof(*u->g);

    for (i = 0; i < groups; i++)
    {
        struct sclc_group *g = &u->g[i];
        sc_params p;
        char err[96];
        u64 seed = u->seed ^ ((u64)i << 32);

        memset(&p, 0, sizeof(p));
        p.block = par->block;
        p.L = par->L;
        p.rate_num = par->rate_num;
        p.rate_den = par->rate_den;
        p.term = SC_TERM_ACC;
        p.extra_dc = 5;
        p.seed = seed;
        err[0] = 0;
        g->code = gf16_build(&p, seed ^ SCLC_COEF_SALT, err, sizeof(err));
        if (!g->code)
            goto fail;
        /*
         * The graph decides the real parity count.  The rate arithmetic
         * above overestimates it on the 18/14 and 22/10 ladder splits, and
         * an underestimate would read past the staging buffer, so size the
         * codec from group 0's graph and require every other group to
         * agree.
         */
        if (gf16_n_data(g->code) != (int)n_data || gf16_n_par(g->code) <= 0)
            goto fail; /* the share mapping assumes this payload size */
        if (i == 0)
        {
            u->n_par = (u32)gf16_n_par(g->code);
            u->nsym = n_data + u->n_par;
            if ((u64)codes * u->n_par >
                (u64)par->m * SCLC_SYM_PER_SHARE)
                goto fail;
            if (u->mapping == TBT_SCLC_MAP_INTERLEAVED)
            {
                u32 cnt[TBT_FEC_MAX_SHARES];

                if (sclc_parity_counts(u, cnt) > SCLC_SYM_PER_SHARE)
                    goto fail;
            }
            if (u->mapping == TBT_SCLC_MAP_RANDOM &&
                sclc_build_labels(u) != 0)
                goto fail;
        }
        else if (gf16_n(g->code) != (int)u->nsym ||
                 gf16_n_par(g->code) != (int)u->n_par)
            goto fail; /* every group must agree on the block size */

        g->recv_b = sclc_zalloc((size_t)GF16_BATCH * u->nsym);
        g->erased_b = sclc_zalloc((size_t)GF16_BATCH * u->nsym);
        g->out_b = g->recv_b;
        g->st_b = sclc_zalloc((size_t)GF16_BATCH * sizeof(gf16_dec_stat));
        g->vp = sclc_zalloc((size_t)u->nsym * 4u * sizeof(u64));
        g->codebuf = sclc_zalloc((size_t)u->nsym);
        g->share_bad = sclc_zalloc(share_bytes);
        g->share_known = sclc_zalloc(share_bytes);
        g->offp = sclc_zalloc((size_t)TBT_FEC_MAX_SHARES * sizeof(u32));
        g->offq = sclc_zalloc((size_t)TBT_FEC_MAX_SHARES * sizeof(u32));
        g->woff = sclc_zalloc((size_t)TBT_FEC_MAX_SHARES * sizeof(u32));
        if (!g->recv_b || !g->erased_b || !g->st_b || !g->vp ||
            !g->codebuf ||
            !g->share_bad || !g->share_known || !g->offp || !g->offq ||
            !g->woff)
            goto fail;
        u->mem += gf16_mem(g->code) +
                  (u32)GF16_BATCH * u->nsym * 2u +
                  u->nsym +
                  (u32)GF16_BATCH * (u32)sizeof(gf16_dec_stat) +
                  (u32)u->nsym * 4u * (u32)sizeof(u64) +
                  (u32)TBT_FEC_MAX_SHARES * 3u * sizeof(u32);
    }
    return u;

fail:
    tbt_sclc_uplink_free(u);
    return NULL;
}

void tbt_sclc_uplink_free(struct tbt_sclc_uplink *u)
{
    u16 i;

    if (!u)
        return;
    if (u->g)
    {
        for (i = 0; i < u->groups; i++)
        {
            struct sclc_group *g = &u->g[i];

            gf16_free(g->code);
            sclc_free(g->recv_b);
            sclc_free(g->erased_b);
            sclc_free(g->st_b);
            sclc_free(g->vp);
            sclc_free(g->codebuf);
            sclc_free(g->share_bad);
            sclc_free(g->share_known);
            sclc_free(g->offp);
            sclc_free(g->offq);
            sclc_free(g->woff);
        }
        sclc_free(u->g);
    }
    sclc_free(u->plab);
    sclc_free(u->qlab);
    sclc_free(u);
}

u16 tbt_sclc_uplink_groups(const struct tbt_sclc_uplink *u)
{
    return u->groups;
}

u16 tbt_sclc_uplink_shares(const struct tbt_sclc_uplink *u)
{
    return u->shares;
}

u16 tbt_sclc_uplink_codes(const struct tbt_sclc_uplink *u)
{
    return u->codes;
}

u32 tbt_sclc_uplink_n_data(const struct tbt_sclc_uplink *u)
{
    return u->n_data;
}

u32 tbt_sclc_uplink_n_par(const struct tbt_sclc_uplink *u)
{
    return u->n_par;
}

u32 tbt_sclc_uplink_mem(const struct tbt_sclc_uplink *u)
{
    return u->mem;
}

int tbt_sclc_uplink_encode(struct tbt_sclc_uplink *u, u16 group,
                           u8 *data[], u8 *parity[])
{
    struct sclc_group *g;
    u32 c, j, sh, off;

    if (!u || group >= u->groups || !data || !parity)
        return -EINVAL;
    g = &u->g[group];
    for (j = 0; j < u->m; j++)
        memset(parity[j], 0, TBT_FEC_S);
    memset(g->offp, 0, (size_t)TBT_FEC_MAX_SHARES * sizeof(u32));
    memset(g->offq, 0, (size_t)TBT_FEC_MAX_SHARES * sizeof(u32));

    if (u->mapping == TBT_SCLC_MAP_INTERLEAVED)
    {
        /*
         * 64 codewords share the graph and the scan order, so the check
         * sweep runs bit-sliced: one uint64 holds lane cw's symbol bit.
         * The gather and scatter stay per codeword because the share
         * offsets advance codeword by codeword, exactly as the scalar
         * path does, which keeps the wire layout identical.
         */
        u32 c0;

        for (c0 = 0; c0 < u->codes; c0 += GF16_LANES)
        {
            int B = (int)((u->codes - c0 < GF16_LANES)
                              ? u->codes - c0
                              : GF16_LANES);
            int cw;

            memset(g->vp, 0, (size_t)u->nsym * 4u * sizeof(u64));
            for (cw = 0; cw < B; cw++)
            {
                u32 shp = (c0 + (u32)cw) % u->n;

                for (j = 0; j < u->n_data; j++)
                {
                    u64 *p = g->vp + (size_t)j * 4;
                    u8 x;

                    off = g->offp[shp]++;
                    x = sclc_get_sym(data[shp], off);
                    p[0] |= (u64)(x & 1u) << cw;
                    p[1] |= (u64)((x >> 1) & 1u) << cw;
                    p[2] |= (u64)((x >> 2) & 1u) << cw;
                    p[3] |= (u64)((x >> 3) & 1u) << cw;
                    if (++shp == u->n)
                        shp = 0;
                }
            }
            gf16_encode_planes(g->code, B, g->vp);
            for (cw = 0; cw < B; cw++)
            {
                u32 shq = (c0 + (u32)cw) % u->m;

                for (j = 0; j < u->n_par; j++)
                {
                    const u64 *p = g->vp + (size_t)(u->n_data + j) * 4;
                    u8 x = (u8)(((p[0] >> cw) & 1u) |
                                (((p[1] >> cw) & 1u) << 1) |
                                (((p[2] >> cw) & 1u) << 2) |
                                (((p[3] >> cw) & 1u) << 3));

                    off = g->offq[shq]++;
                    sclc_put_sym(parity[shq], off, x);
                    if (++shq == u->m)
                        shq = 0;
                }
            }
        }
        return 0;
    }

    for (c = 0; c < u->codes; c++)
    {
        memset(g->codebuf, 0, u->nsym);
        for (j = 0; j < u->n_data; j++)
        {
            sh = sclc_payload_share(u, c, j);
            off = (u->mapping == TBT_SCLC_MAP_CONTIGUOUS)
                      ? sclc_payload_off(u, c, j)
                      : g->offp[sh]++;
            g->codebuf[j] = sclc_get_sym(data[sh], off);
        }
        if (gf16_encode(g->code, g->codebuf) != 0)
            return -EIO;
        for (j = 0; j < u->n_par; j++)
        {
            if (u->mapping == TBT_SCLC_MAP_CONTIGUOUS)
            {
                u32 pos = c * u->n_par + j;

                sh = (u16)(pos / SCLC_SYM_PER_SHARE);
                off = pos % SCLC_SYM_PER_SHARE;
            }
            else
            {
                sh = sclc_parity_share(u, c, j);
                off = g->offq[sh]++;
            }
            sclc_put_sym(parity[sh], off, g->codebuf[u->n_data + j]);
        }
    }
    return 0;
}

/* Gather one codeword into a batch slot.  The interleaved mapping is the
 * production path: share (j + c) mod n, so a codeword's symbols rotate
 * through the shares in step.  Walking that rotation with a counter avoids
 * one integer division per symbol; only the first share needs a division
 * per codeword. */
static void sclc_gather_slot(struct tbt_sclc_uplink *u, struct sclc_group *g,
                             u32 c, u8 *data[], const u8 *data_present,
                             u8 *parity[], const u8 *parity_present,
                             u8 *recv, u8 *erased)
{
    u32 j, sh, off;

    memset(erased, 0, u->nsym);
    if (u->mapping == TBT_SCLC_MAP_INTERLEAVED)
    {
        u32 shp = c % u->n;
        u32 shq = c % u->m;

        for (j = 0; j < u->n_data; j++)
        {
            off = g->offp[shp]++;
            if (data_present[shp])
                recv[j] = sclc_get_sym(data[shp], off);
            else
                erased[j] = 1;
            if (++shp == u->n)
                shp = 0;
        }
        for (j = 0; j < u->n_par; j++)
        {
            off = g->offq[shq]++;
            if (parity_present[shq])
                recv[u->n_data + j] = sclc_get_sym(parity[shq], off);
            else
                erased[u->n_data + j] = 1;
            if (++shq == u->m)
                shq = 0;
        }
        return;
    }

    for (j = 0; j < u->n_data; j++)
    {
        sh = sclc_payload_share(u, c, j);
        off = (u->mapping == TBT_SCLC_MAP_CONTIGUOUS)
                  ? sclc_payload_off(u, c, j)
                  : g->offp[sh]++;
        if (data_present[sh])
            recv[j] = sclc_get_sym(data[sh], off);
        else
            erased[j] = 1;
    }
    for (j = 0; j < u->n_par; j++)
    {
        if (u->mapping == TBT_SCLC_MAP_CONTIGUOUS)
        {
            u32 pos = c * u->n_par + j;

            sh = (u16)(pos / SCLC_SYM_PER_SHARE);
            off = pos % SCLC_SYM_PER_SHARE;
        }
        else
        {
            sh = sclc_parity_share(u, c, j);
            off = g->offq[sh]++;
        }
        if (parity_present[sh])
            recv[u->n_data + j] = sclc_get_sym(parity[sh], off);
        else
            erased[u->n_data + j] = 1;
    }
}

/* Scatter a decoded codeword back into the data shares. */
static void sclc_scatter_slot(struct tbt_sclc_uplink *u, struct sclc_group *g,
                              u32 c, u8 *data[], const u8 *out)
{
    u32 j, sh, off;

    if (u->mapping == TBT_SCLC_MAP_INTERLEAVED)
    {
        u32 shp = c % u->n;

        for (j = 0; j < u->n_data; j++)
        {
            off = g->woff[shp]++;
            sclc_put_sym(data[shp], off, out[j]);
            if (++shp == u->n)
                shp = 0;
        }
        return;
    }
    for (j = 0; j < u->n_data; j++)
    {
        sh = sclc_payload_share(u, c, j);
        off = (u->mapping == TBT_SCLC_MAP_CONTIGUOUS)
                  ? sclc_payload_off(u, c, j)
                  : g->woff[sh]++;
        sclc_put_sym(data[sh], off, out[j]);
    }
}

int tbt_sclc_uplink_decode(struct tbt_sclc_uplink *u, u16 group,
                           u8 *data[], u8 *parity[],
                           const u8 *data_present,
                           const u8 *parity_present)
{
    struct sclc_group *g;
    u32 j, c0;
    u32 missing = 0, recovered = 0;
    u32 ok = 0, failed = 0;

    if (!u || group >= u->groups || !data || !parity ||
        !data_present || !parity_present)
        return -EINVAL;
    g = &u->g[group];

    /*
     * No DATA share is missing: every payload byte the caller can deliver
     * is already in data[].  Solving the codewords would only recompute
     * bytes that are already present, and a missing parity share is a
     * repair question that share_known answers below.  Return before the
     * gather/peel/scatter pass.
     */
    for (j = 0; j < u->n; j++)
        if (!data_present[j])
            break;
    if (j == u->n)
    {
        u32 absent = 0;

        memset(g->share_bad, 0, u->shares);
        for (j = 0; j < u->n; j++)
            g->share_known[j] = data_present[j] ? 1 : 0;
        for (j = 0; j < u->m; j++)
        {
            g->share_known[u->n + j] = parity_present[j] ? 1 : 0;
            if (!parity_present[j])
                absent++;
        }
        memset(&g->st, 0, sizeof(g->st));
        g->st.codes = u->codes;
        g->st.codes_ok = u->codes;
        g->st.shares_missing = absent;
        return (int)u->n;
    }

    memset(g->share_bad, 0, u->shares);
    memset(&g->st, 0, sizeof(g->st));
    memset(g->offp, 0, (size_t)TBT_FEC_MAX_SHARES * sizeof(u32));
    memset(g->offq, 0, (size_t)TBT_FEC_MAX_SHARES * sizeof(u32));
    memset(g->woff, 0, (size_t)TBT_FEC_MAX_SHARES * sizeof(u32));

    for (j = 0; j < u->n; j++)
        if (!data_present[j])
            missing++;
    for (j = 0; j < u->m; j++)
        if (!parity_present[j])
            missing++;

    for (c0 = 0; c0 < u->codes; c0 += GF16_BATCH)
    {
        int B = (int)((u->codes - c0 < GF16_BATCH) ? u->codes - c0
                                                   : GF16_BATCH);
        int cw;

        for (cw = 0; cw < B; cw++)
            sclc_gather_slot(u, g, c0 + (u32)cw, data, data_present,
                             parity, parity_present,
                             g->recv_b + (size_t)cw * u->nsym,
                             g->erased_b + (size_t)cw * u->nsym);
        gf16_decode_batch(g->code, B, g->recv_b, g->erased_b, g->out_b,
                          1, u->max_core, g->st_b);
        for (cw = 0; cw < B; cw++)
        {
            gf16_dec_stat *st = &g->st_b[cw];
            u32 cc = c0 + (u32)cw;

            g->st.core_calls += (u32)st->core_calls;
            g->st.core_solved += (u32)st->core_solved;
            g->st.core_skipped += (u32)st->core_skipped;
            if (st->unresolved == 0)
            {
                ok++;
                sclc_scatter_slot(u, g, cc, data,
                                  g->out_b + (size_t)cw * u->nsym);
            }
            else
            {
                failed++;
                sclc_mark_bad(u, g, cc);
                if (u->mapping == TBT_SCLC_MAP_INTERLEAVED)
                {
                    u32 shp = cc % u->n;

                    for (j = 0; j < u->n_data; j++)
                    {
                        g->woff[shp]++;
                        if (++shp == u->n)
                            shp = 0;
                    }
                }
                else if (u->mapping == TBT_SCLC_MAP_RANDOM)
                {
                    for (j = 0; j < u->n_data; j++)
                        g->woff[sclc_payload_share(u, cc, j)]++;
                }
            }
        }
    }

    for (j = 0; j < u->shares; j++)
    {
        int present = (j < u->n) ? data_present[j]
                                 : parity_present[j - u->n];

        g->share_known[j] = (u8)(present || !g->share_bad[j]);
        if (g->share_known[j] && !present)
            recovered++;
    }

    g->st.codes = u->codes;
    g->st.codes_ok = ok;
    g->st.codes_failed = failed;
    g->st.shares_missing = missing;
    g->st.shares_recovered = recovered;
    {
        u32 complete = 0;

        for (j = 0; j < u->n; j++)
            if (g->share_known[j])
                complete++;
        return (int)complete;
    }
}

void tbt_sclc_uplink_stat(const struct tbt_sclc_uplink *u, u16 group,
                          struct tbt_sclc_stat *out)
{
    if (!u || group >= u->groups || !out)
        return;
    *out = u->g[group].st;
}

int tbt_sclc_uplink_share_known(const struct tbt_sclc_uplink *u, u16 group,
                                u16 share)
{
    if (!u || group >= u->groups || share >= u->shares)
        return 0;
    return u->g[group].share_known[share];
}

/* ------------------------------------------------------------------ */
/* tbt_fec_ops adapter                                                 */
/* ------------------------------------------------------------------ */

static void *sclc_ops_new(u16 n, u16 m, u16 k, u16 w, size_t s, u16 d,
                          u64 nonce)
{
    struct tbt_sclc_params p;

    if (s != TBT_FEC_S)
        return NULL;
    memset(&p, 0, sizeof(p));
    p.n = n;
    p.m = m;
    p.block = k;
    p.L = w;
    p.rate_num = d;
    p.rate_den = 256;
    p.max_core = 256;
    p.mapping = TBT_SCLC_MAP_INTERLEAVED;
    p.seed = nonce;
    return tbt_sclc_uplink_new(1, &p);
}

static void sclc_ops_free(void *ctx)
{
    tbt_sclc_uplink_free(ctx);
}

static u32 sclc_ops_parity_count(const void *ctx, u32 L)
{
    const struct tbt_sclc_uplink *u = ctx;

    (void)L;
    return u->m;
}

static u32 sclc_ops_coeffs(const void *ctx, u32 g, u32 r, u32 *idx, u32 max)
{
    (void)ctx;
    (void)g;
    (void)r;
    (void)idx;
    (void)max;
    return (u32)-ENOSYS;
}

static int sclc_ops_encode(void *ctx, u8 *data[], u32 L, u8 *parity[])
{
    (void)L;
    return tbt_sclc_uplink_encode(ctx, 0, data, parity);
}

static int sclc_ops_decode(void *ctx, u8 *data[], u8 *parity[],
                           const u8 *data_present,
                           const u8 *parity_present, u32 L)
{
    (void)L;
    return tbt_sclc_uplink_decode(ctx, 0, data, parity, data_present,
                                  parity_present);
}

static int sclc_ops_advance(void *ctx, int kind, u32 index)
{
    (void)ctx;
    (void)kind;
    (void)index;
    return 0; /* a block codec has no sliding frontier */
}

static u32 sclc_ops_frontier(const void *ctx)
{
    (void)ctx;
    return 0;
}

static u32 sclc_ops_stuck_at(const void *ctx)
{
    (void)ctx;
    return 0;
}

static u32 sclc_ops_recovered(const void *ctx)
{
    const struct tbt_sclc_uplink *u = ctx;

    return u->g[0].st.shares_recovered;
}

static int sclc_ops_is_known(const void *ctx, u32 index)
{
    const struct tbt_sclc_uplink *u = ctx;

    return tbt_sclc_uplink_share_known(u, 0, (u16)index);
}

const struct tbt_fec_ops tbt_fec_ops_sclc_gf16 = {
    .name = "sc-ldpc-gf16",
    .mode = 3,
    .new = sclc_ops_new,
    .free = sclc_ops_free,
    .parity_count = sclc_ops_parity_count,
    .coeffs = sclc_ops_coeffs,
    .encode = sclc_ops_encode,
    .decode = sclc_ops_decode,
    .advance = sclc_ops_advance,
    .frontier = sclc_ops_frontier,
    .stuck_at = sclc_ops_stuck_at,
    .recovered = sclc_ops_recovered,
    .is_known = sclc_ops_is_known,
};
