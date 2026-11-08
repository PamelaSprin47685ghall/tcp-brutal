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
    u8 *recv;      /* n symbols */
    u8 *erased;    /* n bytes */
    u8 *out;       /* n symbols */
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
    u32 cnt[TBT_FEC_MAX_SHARES];
    size_t i;

    memset(cnt, 0, (size_t)shares * sizeof(cnt[0]));
    for (i = 0; i < count; i++)
        if (++cnt[lab[i]] > SCLC_SYM_PER_SHARE)
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
static u32 sclc_parity_counts(const struct tbt_sclc_uplink *u, u32 *cnt)
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
    if ((u64)codes * n_par > (u64)par->m * SCLC_SYM_PER_SHARE)
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

    if (u->mapping == TBT_SCLC_MAP_INTERLEAVED)
    {
        u32 cnt[TBT_FEC_MAX_SHARES];

        if (codes % par->n)
            goto fail; /* exact fill needs C a multiple of n */
        if (sclc_parity_counts(u, cnt) > SCLC_SYM_PER_SHARE)
            goto fail;
    }
    if (u->mapping == TBT_SCLC_MAP_RANDOM && sclc_build_labels(u) != 0)
        goto fail;

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
        g->recv = sclc_zalloc((size_t)u->nsym);
        g->erased = sclc_zalloc((size_t)u->nsym);
        g->out = sclc_zalloc((size_t)u->nsym);
        g->codebuf = sclc_zalloc((size_t)u->nsym);
        g->share_bad = sclc_zalloc(share_bytes);
        g->share_known = sclc_zalloc(share_bytes);
        g->offp = sclc_zalloc((size_t)TBT_FEC_MAX_SHARES * sizeof(u32));
        g->offq = sclc_zalloc((size_t)TBT_FEC_MAX_SHARES * sizeof(u32));
        g->woff = sclc_zalloc((size_t)TBT_FEC_MAX_SHARES * sizeof(u32));
        if (!g->recv || !g->erased || !g->out || !g->codebuf ||
            !g->share_bad || !g->share_known || !g->offp || !g->offq ||
            !g->woff)
            goto fail;
        u->mem += gf16_mem(g->code) + u->nsym * 4u +
                  (u32)share_bytes * 2u +
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
            sclc_free(g->recv);
            sclc_free(g->erased);
            sclc_free(g->out);
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

int tbt_sclc_uplink_decode(struct tbt_sclc_uplink *u, u16 group,
                           u8 *data[], u8 *parity[],
                           const u8 *data_present,
                           const u8 *parity_present)
{
    struct sclc_group *g;
    u32 c, j, sh, off;
    u32 missing = 0, recovered = 0;
    u32 ok = 0, failed = 0;

    if (!u || group >= u->groups || !data || !parity ||
        !data_present || !parity_present)
        return -EINVAL;
    g = &u->g[group];
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

    for (c = 0; c < u->codes; c++)
    {
        memset(g->recv, 0, u->nsym);
        memset(g->erased, 0, u->nsym);
        for (j = 0; j < u->n_data; j++)
        {
            sh = sclc_payload_share(u, c, j);
            off = (u->mapping == TBT_SCLC_MAP_CONTIGUOUS)
                      ? sclc_payload_off(u, c, j)
                      : g->offp[sh]++;
            if (data_present[sh])
                g->recv[j] = sclc_get_sym(data[sh], off);
            else
                g->erased[j] = 1;
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
                g->recv[u->n_data + j] = sclc_get_sym(parity[sh], off);
            else
                g->erased[u->n_data + j] = 1;
        }

        {
            gf16_dec_stat st = gf16_decode(g->code, g->recv, g->erased,
                                           g->out, 1, u->max_core);

            g->st.core_calls += (u32)st.core_calls;
            g->st.core_solved += (u32)st.core_solved;
            g->st.core_skipped += (u32)st.core_skipped;
            if (st.unresolved == 0)
            {
                ok++;
                for (j = 0; j < u->n_data; j++)
                {
                    sh = sclc_payload_share(u, c, j);
                    off = (u->mapping == TBT_SCLC_MAP_CONTIGUOUS)
                              ? sclc_payload_off(u, c, j)
                              : g->woff[sh]++;
                    sclc_put_sym(data[sh], off, g->out[j]);
                }
            }
            else
            {
                failed++;
                sclc_mark_bad(u, g, c);
                if (u->mapping == TBT_SCLC_MAP_INTERLEAVED ||
                    u->mapping == TBT_SCLC_MAP_RANDOM)
                {
                    /* woff is only used for successful codewords; advance
                     * it past this codeword's payload slots. */
                    for (j = 0; j < u->n_data; j++)
                        g->woff[sclc_payload_share(u, c, j)]++;
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
