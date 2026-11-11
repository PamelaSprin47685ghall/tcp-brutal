/* tbt_sclc_uplink.c - uplink grouped GF(16) SC-IRA block codec.
 *
 * One group FEC block carries n data shares and m parity shares of S bytes.
 * The block is C codewords of the SC_TERM_ACC GF(16) code.
 *
 * Bit-plane 64 mapping (production, Wire v8): the codewords split into
 * batches of 64 (the tail batch holds the remainder).  Every codeword of a
 * batch uses the same symbol->share map, so a lost share erases one
 * identical variable set in all 64 lanes.  One symbol of a batch occupies
 * four little-endian bit planes of lanes/8 bytes each, still two symbols
 * per byte.  A share therefore carries one contiguous region per batch,
 * and the batch size plus the per-share symbol counts are solved at build
 * time so every data share is filled exactly.
 *
 * Legacy interleaved mapping (host A/B): payload symbol j of codeword c
 * goes to data share (j + c) mod n; parity symbol j goes to parity share
 * (j + c) mod m.  The offsets are running counters, so no table travels.
 *
 * Contiguous mapping is the A/B control: codeword c occupies payload
 * positions [c*n_data, (c+1)*n_data).  It loses whole codewords when a data
 * share is lost; the host test measures the difference.
 *
 * Encoding is bit-sliced in every mode: a batch of 64 codewords shares the
 * graph and the check order, so one call to gf16_encode_planes() replaces
 * 64 scalar sweeps.  The bit-plane decoder peels one batch with one
 * control flow and runs one dense core elimination for all 64 lanes.
 *
 * This file does not touch tbt_fec_data.c; the data plane calls the ops
 * table or the direct API.
 */

#include "tbt_sclc_uplink.h"
#include "tbt_sc_ldpc.h"
#include "tbt_sc_ldpc_gf16.h"

#define SCLC_SYM_PER_SHARE (TBT_FEC_S * 2) /* 8192 bytes = 16384 GF(16) */
#define SCLC_COEF_SALT 0xD1B54A32D192ED03ULL
#define SCLC_BATCH_LANES 64
#define SCLC_MAX_BATCHES 64

/* One codeword batch: lanes/8 bytes per plane, four planes per symbol. */
struct sclc_bmap
{
    u8 lanes;
    u8 lw;
};

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
    u8 *erased;    /* nsym, one mask for the whole batch */
    u16 *seq_rem;  /* max(n, m), symbol->share generator */
    u16 *seq_pos;  /* max(n, m) */
    u16 seq_idx;
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
    u8 nb; /* 64-lane batches */
    struct sclc_bmap *bm;
    u32 *dcount; /* nb * n, symbols per batch and data share */
    u32 *dbase;  /* nb * n, byte offset of the batch region */
    u32 *pcount; /* nb * m */
    u32 *pbase;  /* nb * m */
    u8 *dphase;  /* nb */
    u8 *pphase;  /* nb */
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

/* Little-endian plane bytes.  Both wire ends are little-endian; the fallback
 * keeps a big-endian host correct, just slower. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define SCLC_LE_PLANE 1
#else
#define SCLC_LE_PLANE 0
#endif

static inline u64 sclc_ld_plane(const u8 *p, u8 lw)
{
#if SCLC_LE_PLANE
    switch (lw)
    {
    case 8:
    {
        u64 w;

        memcpy(&w, p, 8);
        return w;
    }
    case 6:
    {
        u32 lo;
        u16 hi;

        memcpy(&lo, p, 4);
        memcpy(&hi, p + 4, 2);
        return (u64)lo | ((u64)hi << 32);
    }
    case 4:
    {
        u32 w;

        memcpy(&w, p, 4);
        return w;
    }
    case 2:
    {
        u16 w;

        memcpy(&w, p, 2);
        return w;
    }
    default:
    {
        u64 w = 0;
        int i;

        for (i = (int)lw - 1; i >= 0; i--)
            w = (w << 8) | p[i];
        return w;
    }
    }
#else
    u64 w = 0;
    int i;

    for (i = (int)lw - 1; i >= 0; i--)
        w = (w << 8) | p[i];
    return w;
#endif
}

static inline void sclc_st_plane(u8 *p, u64 w, u8 lw)
{
#if SCLC_LE_PLANE
    switch (lw)
    {
    case 8:
        memcpy(p, &w, 8);
        break;
    case 6:
    {
        u32 lo = (u32)w;
        u16 hi = (u16)(w >> 32);

        memcpy(p, &lo, 4);
        memcpy(p + 4, &hi, 2);
        break;
    }
    case 4:
    {
        u32 lo = (u32)w;

        memcpy(p, &lo, 4);
        break;
    }
    case 2:
    {
        u16 lo = (u16)w;

        memcpy(p, &lo, 2);
        break;
    }
    default:
    {
        int i;

        for (i = 0; i < (int)lw; i++)
            p[i] = (u8)(w >> (8 * i));
        break;
    }
    }
#else
    int i;

    for (i = 0; i < (int)lw; i++)
        p[i] = (u8)(w >> (8 * i));
#endif
}

static u32 sclc_gcd(u32 a, u32 b)
{
    while (b)
    {
        u32 t = a % b;

        a = b;
        b = t;
    }
    return a;
}

/* The codec's parity count after the SC-IRA graph quantization.  The uplink
 * block must use the graph's count, not the share-ratio estimate: above
 * rate 1/2 sc_build rounds n_base down to a whole lifting block. */
static u32 sclc_code_n_par(u32 n_data, const struct tbt_sclc_params *p)
{
    u64 want;

    if ((u32)p->rate_num * 2u == p->rate_den)
        return n_data;
    want = (u64)n_data * (p->rate_den - p->rate_num) / p->rate_num;
    if ((u32)p->rate_num * 2u > p->rate_den)
    {
        u32 lift = p->block / 2;
        u32 nb = (u32)((want + lift / 2u) / lift);

        return nb * lift;
    }
    return (u32)want;
}

/* Symbol->share generator.  Counts must match the table the other side
 * derives from the same build parameters; the cyclic walk spreads each
 * share's symbols through the codeword instead of grouping them. */
static void sclc_seq_init(struct sclc_group *g, const u32 *counts, u16 nsh,
                          u8 phase)
{
    u16 s;

    for (s = 0; s < nsh; s++)
    {
        g->seq_rem[s] = (u16)counts[s];
        g->seq_pos[s] = 0;
    }
    g->seq_idx = phase < nsh ? phase : 0;
}

static inline void sclc_seq_next(struct sclc_group *g, u16 nsh, u16 *share,
                                 u16 *slot)
{
    for (;;)
    {
        u16 i = g->seq_idx;

        if (g->seq_rem[i])
        {
            g->seq_rem[i]--;
            *share = i;
            *slot = g->seq_pos[i]++;
            g->seq_idx = (u16)((i + 1u) % nsh);
            return;
        }
        g->seq_idx = (u16)((i + 1u) % nsh);
    }
}

/*
 * Solve the per-batch symbol counts.  Row b has n_sym symbols and one
 * four-plane block per symbol costs lanes/8 * 4 bytes, so share s holds
 * sum_b count[b][s] * lanes[b] / 2 bytes.  Data must fill every share to
 * S bytes exactly; parity may leave a zero tail.  The full batches carry
 * weight 64 and the tail carries its own lane count; the per-share target
 * W_s is split into q full-batch counts F_s and one tail count L_s by
 * solving 64 F + tail L = W.
 */
static int sclc_region_plan(u32 n_sym, u16 n_sh, u32 cap_syms, u16 codes,
                            u8 nb, const u8 *lanes, int exact,
                            u32 *counts, u32 *base_off, u8 *phase)
{
    u32 *w = NULL, *f = NULL, *l = NULL;
    u64 total = (u64)n_sym * codes;
    u32 tail = lanes[nb - 1] < SCLC_BATCH_LANES ? lanes[nb - 1] : 0;
    u32 q = tail ? (u32)nb - 1u : (u32)nb;
    u32 s, b, k;
    u32 g, units, per, rem;
    int rc = -EINVAL;

    if (exact ? total != (u64)n_sh * cap_syms
              : total > (u64)n_sh * cap_syms)
        return -EINVAL;
    w = sclc_zalloc((size_t)n_sh * sizeof(u32));
    f = sclc_zalloc((size_t)n_sh * sizeof(u32));
    l = sclc_zalloc((size_t)n_sh * sizeof(u32));
    if (!w || !f || !l)
    {
        rc = -ENOMEM;
        goto out;
    }

    if (!tail)
    {
        if (total % 64u)
            goto out;
        units = (u32)(total / 64u);
        if (units > (u32)n_sh * (cap_syms / 64u))
            goto out;
        per = units / n_sh;
        rem = units % n_sh;
        for (s = 0; s < n_sh; s++)
        {
            w[s] = (per + (s < rem ? 1u : 0u)) * 64u;
            f[s] = w[s] / 64u;
            l[s] = 0;
        }
    }
    else
    {
        u32 a, bb, inv = 0;
        u64 lsum = 0;
        u32 add;

        g = sclc_gcd(64u, tail);
        if (total % g)
            goto out;
        units = (u32)(total / g);
        per = units / n_sh;
        rem = units % n_sh;
        for (s = 0; s < n_sh; s++)
        {
            w[s] = (per + (s < rem ? 1u : 0u)) * g;
            if (w[s] > cap_syms)
                goto out;
        }
        a = 64u / g;
        bb = tail / g;
        for (k = 1; k < a; k++)
            if ((bb * k) % a == 1)
            {
                inv = k;
                break;
            }
        if (!inv)
            goto out;
        for (s = 0; s < n_sh; s++)
        {
            l[s] = ((w[s] / g) % a * inv) % a;
            lsum += l[s];
        }
        if (lsum > n_sym)
            goto out;
        add = (u32)((n_sym - lsum) / a);
        if ((u64)add * a != n_sym - lsum)
            goto out;
        while (add)
        {
            int moved = 0;

            for (s = 0; s < n_sh && add; s++)
            {
                u32 maxl = w[s] / tail;

                if (maxl > n_sym)
                    maxl = n_sym;
                if (l[s] + a <= maxl)
                {
                    l[s] += a;
                    add--;
                    moved = 1;
                }
            }
            if (!moved)
                goto out;
        }
        for (s = 0; s < n_sh; s++)
        {
            u32 num;

            if (w[s] < (u64)tail * l[s])
                goto out;
            num = w[s] - tail * l[s];
            if (num % 64u)
                goto out;
            f[s] = num / 64u;
        }
    }

    {
        u64 sf = 0, sl = 0;

        for (s = 0; s < n_sh; s++)
        {
            sf += f[s];
            sl += l[s];
        }
        if (sf != (u64)q * n_sym || sl != (u64)(tail ? n_sym : 0))
            goto out;
    }

    memset(counts, 0, (size_t)nb * n_sh * sizeof(u32));
    if (q)
    {
        u32 rowsum = 0, extra, tokens = 0, totalrem = 0;

        for (s = 0; s < n_sh; s++)
            rowsum += f[s] / q;
        if (rowsum > n_sym)
            goto out;
        extra = n_sym - rowsum;
        for (s = 0; s < n_sh; s++)
            totalrem += f[s] % q;
        if (totalrem != (u64)q * extra)
            goto out;
        for (s = 0; s < n_sh; s++)
        {
            u32 base = f[s] / q;
            u32 more = f[s] % q;

            for (b = 0; b < q; b++)
                counts[(size_t)b * n_sh + s] = base;
            for (k = 0; k < more; k++)
            {
                u32 row = tokens++ % q;

                counts[(size_t)row * n_sh + s]++;
            }
        }
    }
    if (tail)
        for (s = 0; s < n_sh; s++)
            counts[(size_t)(nb - 1u) * n_sh + s] = l[s];

    for (b = 0; b < nb; b++)
    {
        u64 rs = 0;

        for (s = 0; s < n_sh; s++)
            rs += counts[(size_t)b * n_sh + s];
        if (rs != n_sym)
            goto out;
    }
    for (s = 0; s < n_sh; s++)
    {
        u32 off = 0;

        for (b = 0; b < nb; b++)
        {
            u32 block = (u32)lanes[b] / 2u;

            base_off[(size_t)b * n_sh + s] = off;
            off += counts[(size_t)b * n_sh + s] * block;
        }
        if (off > TBT_FEC_S ||
            (exact && off != TBT_FEC_S))
            goto out;
    }
    for (b = 0; b < nb; b++)
        phase[b] = (u8)((b * 7u + (u32)n_sh / 2u + 1u) % n_sh);
    rc = 0;

out:
    sclc_free(w);
    sclc_free(f);
    sclc_free(l);
    return rc;
}

static int sclc_bmap_build(struct tbt_sclc_uplink *u)
{
    u8 lanes[SCLC_MAX_BATCHES];
    u32 b;
    int rc;
    size_t dsz, psz;

    u->nb = (u8)((u->codes + SCLC_BATCH_LANES - 1u) / SCLC_BATCH_LANES);
    if (!u->nb || u->nb > SCLC_MAX_BATCHES)
        return -EINVAL;
    /* A symbol block is lanes/8 * 4 bytes.  The shipped uplink shape
     * (block 128, L 16) leaves a tail of 16, 32, 48 or 64 lanes; 48 works
     * when the region plan can still fill every share exactly (n=7, 11,
     * 15, 23), and only the single-batch 48-lane shape (n=3) fails.  The
     * region planner rejects any shape whose per-share bytes do not add
     * up to S. */
    for (b = 0; b < u->nb; b++)
    {
        u32 left = u->codes - b * SCLC_BATCH_LANES;

        lanes[b] = (u8)(left > SCLC_BATCH_LANES ? SCLC_BATCH_LANES : left);
        if (!lanes[b] || lanes[b] % 8u)
            return -EINVAL;
    }
    u->bm = sclc_zalloc((size_t)u->nb * sizeof(*u->bm));
    dsz = (size_t)u->nb * u->n;
    psz = (size_t)u->nb * u->m;
    u->dcount = sclc_zalloc(dsz * sizeof(u32));
    u->dbase = sclc_zalloc(dsz * sizeof(u32));
    u->pcount = sclc_zalloc(psz * sizeof(u32));
    u->pbase = sclc_zalloc(psz * sizeof(u32));
    u->dphase = sclc_zalloc(u->nb);
    u->pphase = sclc_zalloc(u->nb);
    if (!u->bm || !u->dcount || !u->dbase || !u->pcount || !u->pbase ||
        !u->dphase || !u->pphase)
        return -ENOMEM;
    for (b = 0; b < u->nb; b++)
    {
        u->bm[b].lanes = lanes[b];
        u->bm[b].lw = (u8)(lanes[b] / 8u);
    }
    rc = sclc_region_plan(u->n_data, u->n, SCLC_SYM_PER_SHARE, u->codes,
                          u->nb, lanes, 1, u->dcount, u->dbase, u->dphase);
    if (rc)
        return rc;
    rc = sclc_region_plan(u->n_par, u->m, SCLC_SYM_PER_SHARE, u->codes,
                          u->nb, lanes, 0, u->pcount, u->pbase, u->pphase);
    if (rc)
        return rc;
    u->mem += (u32)((size_t)u->nb * sizeof(*u->bm) +
                    (dsz + psz) * 2u * sizeof(u32) + (u32)u->nb * 2u);
    return 0;
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
        par->mapping != TBT_SCLC_MAP_RANDOM &&
        par->mapping != TBT_SCLC_MAP_BITPLANE64)
        return NULL;

    n_data = (u32)par->L * (par->block / 2);
    if (!n_data)
        return NULL;
    if (((u32)par->n * SCLC_SYM_PER_SHARE) % n_data)
        return NULL; /* the payload must split into whole codewords */
    codes = ((u32)par->n * SCLC_SYM_PER_SHARE) / n_data;
    if (!codes || codes > 4096)
        return NULL;
    n_par = sclc_code_n_par(n_data, par);
    if (!n_par)
        return NULL;
    if (n_data > 0xffffu || n_par > 0xffffu ||
        n_data + n_par > 0xffffu)
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
         * agree.  The mapping tables follow the final nsym.
         */
        if ((u32)gf16_n_data(g->code) != u->n_data ||
            gf16_n_par(g->code) <= 0)
            goto fail;
        if (i == 0)
        {
            u->n_par = (u32)gf16_n_par(g->code);
            u->nsym = u->n_data + u->n_par;
            if ((u64)codes * u->n_par >
                (u64)par->m * SCLC_SYM_PER_SHARE)
                goto fail;
            if (u->mapping == TBT_SCLC_MAP_BITPLANE64)
            {
                if (sclc_bmap_build(u) != 0)
                    goto fail;
            }
            if (u->mapping == TBT_SCLC_MAP_INTERLEAVED)
            {
                u16 cnt[TBT_FEC_MAX_SHARES];

                if (codes % par->n)
                    goto fail; /* exact fill needs C a multiple of n */
                if (sclc_parity_counts(u, cnt) > SCLC_SYM_PER_SHARE)
                    goto fail;
            }
            if (u->mapping == TBT_SCLC_MAP_RANDOM &&
                sclc_build_labels(u) != 0)
                goto fail;
        }
        else if ((u32)gf16_n(g->code) != u->nsym ||
                 (u32)gf16_n_par(g->code) != u->n_par)
            goto fail;
        g->recv_b = sclc_zalloc((size_t)GF16_BATCH * u->nsym);
        g->erased_b = sclc_zalloc((size_t)GF16_BATCH * u->nsym);
        g->out_b = g->recv_b;
        g->st_b = sclc_zalloc((size_t)GF16_BATCH * sizeof(gf16_dec_stat));
        g->vp = sclc_zalloc((size_t)u->nsym * 4u * sizeof(u64));
        g->erased = sclc_zalloc((size_t)u->nsym);
        g->seq_rem = sclc_zalloc((size_t)(u->n > u->m ? u->n : u->m) *
                                 sizeof(u16));
        g->seq_pos = sclc_zalloc((size_t)(u->n > u->m ? u->n : u->m) *
                                 sizeof(u16));
        g->codebuf = sclc_zalloc((size_t)u->nsym);
        g->share_bad = sclc_zalloc(share_bytes);
        g->share_known = sclc_zalloc(share_bytes);
        g->offp = sclc_zalloc((size_t)TBT_FEC_MAX_SHARES * sizeof(u32));
        g->offq = sclc_zalloc((size_t)TBT_FEC_MAX_SHARES * sizeof(u32));
        g->woff = sclc_zalloc((size_t)TBT_FEC_MAX_SHARES * sizeof(u32));
        if (!g->recv_b || !g->erased_b || !g->st_b || !g->vp ||
            !g->erased || !g->seq_rem || !g->seq_pos || !g->codebuf ||
            !g->share_bad || !g->share_known || !g->offp || !g->offq ||
            !g->woff)
            goto fail;
        u->mem += gf16_mem(g->code) +
                  (u32)GF16_BATCH * u->nsym * 2u +
                  u->nsym +
                  (u32)GF16_BATCH * (u32)sizeof(gf16_dec_stat) +
                  (u32)u->nsym * 4u * (u32)sizeof(u64) +
                  (u32)u->nsym +
                  (u32)(u->n > u->m ? u->n : u->m) * 2u * sizeof(u16) +
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
            sclc_free(g->erased);
            sclc_free(g->seq_rem);
            sclc_free(g->seq_pos);
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
    sclc_free(u->bm);
    sclc_free(u->dcount);
    sclc_free(u->dbase);
    sclc_free(u->pcount);
    sclc_free(u->pbase);
    sclc_free(u->dphase);
    sclc_free(u->pphase);
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

/* Gather one batch region into four planes per variable.  The plane block
 * of a symbol sits at base[share] + slot * (4 * lanes/8) bytes; every lane
 * of the batch reads the same share and slot, which is the same-mask
 * property the decoder relies on. */
static void sclc_gather_region(struct tbt_sclc_uplink *u,
                               struct sclc_group *g, u32 b, u8 *shares[],
                               const u8 *present, const u32 *counts,
                               const u32 *base, u8 phase, u32 n_sym, u16 nsh,
                               u32 dst_off)
{
    u8 lw = u->bm[b].lw;
    u32 j;
    u16 sh, slot;

    sclc_seq_init(g, counts, nsh, phase);
    for (j = 0; j < n_sym; j++)
    {
        u64 *p = g->vp + (size_t)(dst_off + j) * 4;

        sclc_seq_next(g, nsh, &sh, &slot);
        if (present && !present[sh])
        {
            p[0] = p[1] = p[2] = p[3] = 0;
            g->erased[dst_off + j] = 1;
            continue;
        }
        {
            const u8 *q = shares[sh] + base[sh] +
                          (u32)slot * 4u * (u32)lw;

            p[0] = sclc_ld_plane(q, lw);
            p[1] = sclc_ld_plane(q + lw, lw);
            p[2] = sclc_ld_plane(q + 2u * lw, lw);
            p[3] = sclc_ld_plane(q + 3u * lw, lw);
        }
    }
}

static void sclc_scatter_region(struct tbt_sclc_uplink *u,
                                struct sclc_group *g, u32 b, u8 *shares[],
                                const u32 *counts, const u32 *base, u8 phase,
                                u32 n_sym, u16 nsh, u32 src_off)
{
    u8 lw = u->bm[b].lw;
    u32 j;
    u16 sh, slot;

    sclc_seq_init(g, counts, nsh, phase);
    for (j = 0; j < n_sym; j++)
    {
        const u64 *p = g->vp + (size_t)(src_off + j) * 4;
        u8 *q;

        sclc_seq_next(g, nsh, &sh, &slot);
        q = shares[sh] + base[sh] + (u32)slot * 4u * (u32)lw;
        sclc_st_plane(q, p[0], lw);
        sclc_st_plane(q + lw, p[1], lw);
        sclc_st_plane(q + 2u * lw, p[2], lw);
        sclc_st_plane(q + 3u * lw, p[3], lw);
    }
}

/* A failed batch leaves every absent share it touches incomplete.  A share
 * with no symbol in this batch is untouched by the failure. */
static void sclc_mark_bad_batch(struct tbt_sclc_uplink *u,
                                struct sclc_group *g, u32 b,
                                const u8 *data_present,
                                const u8 *parity_present)
{
    const u32 *dc = u->dcount + (size_t)b * u->n;
    const u32 *pc = u->pcount + (size_t)b * u->m;
    u32 s;

    for (s = 0; s < u->n; s++)
        if (dc[s] && !data_present[s])
            g->share_bad[s] = 1;
    for (s = 0; s < u->m; s++)
        if (pc[s] && !parity_present[s])
            g->share_bad[u->n + s] = 1;
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

    if (u->mapping == TBT_SCLC_MAP_BITPLANE64)
    {
        u32 b;

        for (b = 0; b < u->nb; b++)
        {
            sclc_gather_region(u, g, b, data, NULL, u->dcount + (size_t)b * u->n,
                               u->dbase + (size_t)b * u->n, u->dphase[b],
                               u->n_data, u->n, 0);
            gf16_encode_planes(g->code, u->bm[b].lanes, g->vp);
            sclc_scatter_region(u, g, b, parity,
                                u->pcount + (size_t)b * u->m,
                                u->pbase + (size_t)b * u->m, u->pphase[b],
                                u->n_par, u->m, u->n_data);
        }
        return 0;
    }

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

    if (u->mapping == TBT_SCLC_MAP_BITPLANE64)
    {
        u32 b;

        for (b = 0; b < u->nb; b++)
        {
            u8 lanes = u->bm[b].lanes;
            gf16_dec_stat st;

            memset(g->erased, 0, u->nsym);
            sclc_gather_region(u, g, b, data, data_present,
                               u->dcount + (size_t)b * u->n,
                               u->dbase + (size_t)b * u->n, u->dphase[b],
                               u->n_data, u->n, 0);
            sclc_gather_region(u, g, b, parity, parity_present,
                               u->pcount + (size_t)b * u->m,
                               u->pbase + (size_t)b * u->m, u->pphase[b],
                               u->n_par, u->m, u->n_data);
            st = gf16_decode64(g->code, g->vp, g->erased, 1, u->max_core);
            g->st.core_calls += (u32)st.core_calls;
            g->st.core_solved += (u32)st.core_solved;
            g->st.core_skipped += (u32)st.core_skipped;
            if (st.unresolved == 0)
            {
                ok += lanes;
                sclc_scatter_region(u, g, b, data,
                                    u->dcount + (size_t)b * u->n,
                                    u->dbase + (size_t)b * u->n,
                                    u->dphase[b], u->n_data, u->n, 0);
            }
            else
            {
                failed += lanes;
                sclc_mark_bad_batch(u, g, b, data_present, parity_present);
            }
        }
        goto stats;
    }

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

stats:
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
    p.max_core = 512;
    p.mapping = TBT_SCLC_MAP_BITPLANE64;
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
