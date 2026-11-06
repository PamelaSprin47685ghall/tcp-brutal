/*
 * sc_ldpc_bs.c - bit-sliced BEC peeling decoder.
 *
 * The scalar decoder in sc_ldpc.c walks one codeword at a time and touches
 * a check's counter and value through random indirection for every resolved
 * variable.  This decoder keeps 64 codewords in one uint64_t: bit j of a
 * word is codeword j.  A check therefore has a bit-sliced unknown count
 * (five planes, degree <= 31), a bit-sliced XOR of known values, and a
 * bit-sliced "exactly one unknown" mask.  Resolving a variable is a mask
 * operation that advances all 64 codewords at once; the value of the unique
 * unknown is the check's known-value XOR.
 *
 * Only scalar uint64_t arithmetic is used.  The HND0 target is qemu64 with
 * SSE2 and no AVX, so the 64-wide word is the vector unit.  A group of 64
 * codewords is the unit of work; `groups` such groups share one pass over
 * the graph, which lets the compiler widen the inner loops with SSE2 on the
 * HND0 build and AVX2/AVX-512 on a modern host without changing semantics.
 */

#include "sc_ldpc_bs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BS_GROUP 64
#define BS_PLANES_MAX 5 /* check degree <= 31 */
#define BS_MAX_GROUPS 32
#define BS_MAX_BATCH (BS_GROUP * BS_MAX_GROUPS)

#ifdef BS_TIMING
#include <time.h>
static double bs_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#endif

struct sc_bs {
    sc_graph g;
    sc_code *c;
    int batch;
    int groups;
    int planes;
    size_t stride;

    uint64_t *unk;     /* n * groups */
    uint64_t *val;     /* n * groups */
    uint64_t *cnt;     /* (n_chk * groups) * BS_PLANES */
    uint64_t *xr;      /* n_chk * groups */
    uint64_t *act;     /* n_chk * groups */
    int *stack;        /* n_chk, the active-check list */
    int *touched;      /* n */
    uint8_t *tflag;    /* n */
    uint64_t *resolve; /* n * groups */
    uint64_t *vbits;   /* n * groups */

    uint64_t rows[BS_GROUP];
    sc_bs_stat st;
#ifdef BS_TIMING
    double t_init, t_wave, t_unres, t_out;
#endif
};

/* ------------------------------------------------------------------ */
/* bit-matrix transpose (Hacker's Delight, 64x64, in place)            */
/* ------------------------------------------------------------------ */

static void transpose64(uint64_t *a)
{
    int j, k;
    uint64_t m, t;

    for (j = 32, m = 0x00000000FFFFFFFFULL; j; j >>= 1, m ^= m << j) {
        for (k = 0; k < 64; k = (k + j + 1) & ~j) {
            t = ((a[k] >> j) ^ a[k + j]) & m;
            a[k] ^= (t << j);
            a[k + j] ^= t;
        }
    }
}

static uint64_t load64(const uint8_t *p)
{
    uint64_t w = 0;
    int k;
    for (k = 0; k < 8; k++)
        w |= (uint64_t)p[k] << (8 * k);
    return w;
}

static void store64(uint8_t *p, uint64_t w)
{
    int k;
    for (k = 0; k < 8; k++)
        p[k] = (uint8_t)(w >> (8 * k));
}

/* ------------------------------------------------------------------ */
/* bit-sliced counter helpers                                          */
/* ------------------------------------------------------------------ */

/* Runtime-width variants, used by the cold init and debug paths. */
static uint64_t count_one_n(const uint64_t *p, int planes)
{
    uint64_t r = p[0];
    int i;
    for (i = 1; i < planes; i++)
        r &= ~p[i];
    return r;
}

static void count_add_n(uint64_t *p, uint64_t m, int planes)
{
    uint64_t carry = m;
    int i;
    for (i = 0; i < planes; i++) {
        uint64_t old = p[i];
        p[i] = old ^ carry;
        carry = old & carry;
    }
}

/*
 * Fixed-width counters.  P is a literal at the call site, so the carry
 * chain unrolls and the compiler drops the loop bookkeeping.
 */
#define DEFINE_COUNTERS(P)                                                   \
static inline uint64_t count_one_##P(const uint64_t *p)                      \
{                                                                            \
    uint64_t r = p[0];                                                       \
    int i;                                                                   \
    for (i = 1; i < P; i++)                                                  \
        r &= ~p[i];                                                          \
    return r;                                                                \
}                                                                            \
static inline void count_sub_##P(uint64_t *p, uint64_t m)                    \
{                                                                            \
    uint64_t borrow = m;                                                     \
    int i;                                                                   \
    for (i = 0; i < P; i++) {                                                \
        uint64_t old = p[i];                                                 \
        p[i] = old ^ borrow;                                                 \
        borrow &= ~old;                                                      \
    }                                                                        \
}
DEFINE_COUNTERS(3)
DEFINE_COUNTERS(4)
DEFINE_COUNTERS(5)

/* ------------------------------------------------------------------ */
/* life cycle                                                          */
/* ------------------------------------------------------------------ */

sc_bs *sc_bs_new(sc_code *c, int batch, char *err, size_t errlen)
{
    sc_bs *bs;
    sc_graph g;
    size_t n, nchk, groups;

    sc_get_graph(c, &g);
    if (batch <= 0 || (batch % BS_GROUP) != 0 || batch > BS_MAX_BATCH) {
        snprintf(err, errlen, "batch must be a positive multiple of 64 (max %d)",
                 BS_MAX_BATCH);
        return NULL;
    }
    if (g.n <= 0 || g.n_chk <= 0 || g.dc_max > 31) {
        snprintf(err, errlen, "code check degree %d exceeds bit-sliced limit 31",
                 g.dc_max);
        return NULL;
    }

    bs = calloc(1, sizeof(*bs));
    if (!bs) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    bs->c = c;
    bs->g = g;
    bs->batch = batch;
    bs->groups = batch / BS_GROUP;
    bs->planes = g.dc_max <= 7 ? 3 : (g.dc_max <= 15 ? 4 : 5);
    bs->stride = ((size_t)g.n + 7) / 8;

    n = (size_t)g.n;
    nchk = (size_t)g.n_chk;
    groups = (size_t)bs->groups;

    bs->unk = calloc(n * groups, sizeof(uint64_t));
    bs->val = calloc(n * groups, sizeof(uint64_t));
    bs->cnt = calloc(nchk * groups * BS_PLANES_MAX, sizeof(uint64_t));
    bs->xr = calloc(nchk * groups, sizeof(uint64_t));
    bs->act = calloc(nchk * groups, sizeof(uint64_t));
    bs->stack = calloc(nchk, sizeof(int));
    bs->touched = calloc(n, sizeof(int));
    bs->tflag = calloc(n, 1);
    bs->resolve = calloc(n * groups, sizeof(uint64_t));
    bs->vbits = calloc(n * groups, sizeof(uint64_t));
    if (!bs->unk || !bs->val || !bs->cnt || !bs->xr || !bs->act ||
        !bs->stack || !bs->touched || !bs->tflag || !bs->resolve ||
        !bs->vbits) {
        snprintf(err, errlen, "out of memory");
        sc_bs_free(bs);
        return NULL;
    }
    return bs;
}

void sc_bs_free(sc_bs *bs)
{
    if (!bs)
        return;
    free(bs->unk);
    free(bs->val);
    free(bs->cnt);
    free(bs->xr);
    free(bs->act);
    free(bs->stack);
    free(bs->touched);
    free(bs->tflag);
    free(bs->resolve);
    free(bs->vbits);
    free(bs);
}

int sc_bs_batch(const sc_bs *bs)
{
    return bs->batch;
}

size_t sc_bs_stride(const sc_bs *bs)
{
    return bs->stride;
}

void sc_bs_get_stat(const sc_bs *bs, sc_bs_stat *out)
{
    *out = bs->st;
}

const uint64_t *sc_bs_unknown_mask(const sc_bs *bs)
{
    return bs->unk;
}

int sc_bs_groups(const sc_bs *bs)
{
    return bs->groups;
}

int sc_bs_check_count(const sc_bs *bs, int check, int cw)
{
    int g = cw / BS_GROUP;
    int j = cw % BS_GROUP;
    const uint64_t *p =
        &bs->cnt[((size_t)check * bs->groups + g) * bs->planes];
    int i, v = 0;
    for (i = 0; i < bs->planes; i++)
        v |= (int)((p[i] >> j) & 1u) << i;
    return v;
}

/* ------------------------------------------------------------------ */
/* input / output transposes                                           */
/* ------------------------------------------------------------------ */

/*
 * Expand one 64-variable block of `groups` groups into the sliced state.
 * src is packed codeword-major; dst[v * groups + g] receives bit jj from
 * codeword g*64+jj for variable v0+i.
 */
static void load_block(const uint8_t *src, size_t stride, uint64_t *dst,
                       int groups, int n, int v0, uint64_t *rows)
{
    int g, i, jj;

    for (g = 0; g < groups; g++) {
        const uint8_t *base = src + (size_t)g * BS_GROUP * stride;
        for (jj = 0; jj < BS_GROUP; jj++)
            rows[jj] = load64(base + (size_t)jj * stride + (size_t)(v0 >> 3));
        transpose64(rows);
        for (i = 0; i < BS_GROUP; i++)
            dst[(size_t)(v0 + i) * groups + g] = rows[i];
    }
    (void)n;
}

static void load_tail(const uint8_t *src, size_t stride, uint64_t *dst,
                      int groups, int n, int v0)
{
    int g, v, jj;

    for (g = 0; g < groups; g++) {
        const uint8_t *base = src + (size_t)g * BS_GROUP * stride;
        for (v = v0; v < n; v++) {
            uint64_t w = 0;
            for (jj = 0; jj < BS_GROUP; jj++)
                w |= (uint64_t)((base[(size_t)jj * stride + (size_t)(v >> 3)] >>
                                 (v & 7)) & 1u)
                     << jj;
            dst[(size_t)v * groups + g] = w;
        }
    }
}

static void store_block(uint8_t *dst, size_t stride, const uint64_t *src,
                        int groups, int v0, uint64_t *rows)
{
    int g, i, jj;

    for (g = 0; g < groups; g++) {
        uint8_t *base = dst + (size_t)g * BS_GROUP * stride;
        for (i = 0; i < BS_GROUP; i++)
            rows[i] = src[(size_t)(v0 + i) * groups + g];
        transpose64(rows);
        for (jj = 0; jj < BS_GROUP; jj++)
            store64(base + (size_t)jj * stride + (size_t)(v0 >> 3), rows[jj]);
    }
}

static void store_tail(uint8_t *dst, size_t stride, const uint64_t *src,
                       int groups, int n, int v0)
{
    int g, v, jj;

    for (g = 0; g < groups; g++) {
        uint8_t *base = dst + (size_t)g * BS_GROUP * stride;
        for (v = v0; v < n; v++) {
            uint64_t w = src[(size_t)v * groups + g];
            for (jj = 0; jj < BS_GROUP; jj++) {
                uint8_t *p = base + (size_t)jj * stride + (size_t)(v >> 3);
                uint8_t bit = (uint8_t)(1u << (v & 7));
                if ((w >> jj) & 1u)
                    *p |= bit;
                else
                    *p &= (uint8_t)~bit;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* propagation                                                         */
/* ------------------------------------------------------------------ */

/*
 * One touched variable: publish its resolved bits, then decrement every
 * incident check and re-derive the "exactly one unknown" mask.  P fixes the
 * counter width so the carry chain is fully unrolled.
 */
#define DEFINE_PROPAGATE(P)                                                  \
static void propagate_##P(sc_bs *bs, int v, const uint64_t *rv,              \
                          const uint64_t *vb)                                \
{                                                                            \
    const sc_graph *g = &bs->g;                                              \
    const int groups = bs->groups;                                           \
    uint64_t *val = bs->val;                                                 \
    uint64_t *unk = bs->unk;                                                 \
    uint64_t *xr = bs->xr;                                                   \
    uint64_t *act = bs->act;                                                 \
    uint64_t *cnt = bs->cnt;                                                 \
    int k, gg;                                                               \
    for (gg = 0; gg < groups; gg++) {                                        \
        val[(size_t)v * groups + gg] |= vb[gg];                              \
        unk[(size_t)v * groups + gg] &= ~rv[gg];                             \
    }                                                                        \
    bs->st.var_resolves++;                                                   \
    for (k = g->voff[v]; k < g->voff[v + 1]; k++) {                          \
        int c2 = g->vadj[k];                                                 \
        uint64_t *xp2 = &xr[(size_t)c2 * groups];                            \
        uint64_t *ap2 = &act[(size_t)c2 * groups];                           \
        for (gg = 0; gg < groups; gg++) {                                    \
            uint64_t m = rv[gg];                                             \
            uint64_t *cp = &cnt[((size_t)c2 * groups + gg) * P];             \
            count_sub_##P(cp, m);                                            \
            xp2[gg] ^= vb[gg] & m;                                           \
            ap2[gg] = count_one_##P(cp);                                     \
        }                                                                    \
        bs->st.edge_updates++;                                               \
    }                                                                        \
}
DEFINE_PROPAGATE(3)
DEFINE_PROPAGATE(4)
DEFINE_PROPAGATE(5)

/* ------------------------------------------------------------------ */
/* decode                                                              */
/* ------------------------------------------------------------------ */

long sc_decode_bs(sc_bs *bs, const uint8_t *bits, const uint8_t *erased,
                  uint8_t *out, int target_data_only, long *unresolved)
{
    const sc_graph *g = &bs->g;
    const int groups = bs->groups;
    const int n = g->n;
    const int nchk = g->n_chk;
    const int lim = target_data_only ? g->n_data : n;
    const size_t stride = bs->stride;
    uint64_t *unk = bs->unk;
    uint64_t *val = bs->val;
    uint64_t *cnt = bs->cnt;
    uint64_t *xr = bs->xr;
    uint64_t *act = bs->act;
    int *stack = bs->stack;
    uint64_t *rows = bs->rows;
    uint64_t res[BS_MAX_GROUPS];
    int sp = 0;
    int v, c, e, gg, v0;
    long recovered = 0;

    memset(&bs->st, 0, sizeof(bs->st));

#ifdef BS_TIMING
    {
    double t0 = bs_now();
#endif
    /* ---- input transpose ---------------------------------------- */
    memset(unk, 0, (size_t)n * (size_t)groups * sizeof(uint64_t));
    memset(val, 0, (size_t)n * (size_t)groups * sizeof(uint64_t));
    for (v0 = 0; v0 + BS_GROUP <= n; v0 += BS_GROUP) {
        load_block(bits, stride, val, groups, n, v0, rows);
        load_block(erased, stride, unk, groups, n, v0, rows);
    }
    if (v0 < n) {
        load_tail(bits, stride, val, groups, n, v0);
        load_tail(erased, stride, unk, groups, n, v0);
    }
    for (v = 0; v < n * groups; v++)
        val[v] &= ~unk[v];

    /* ---- check state -------------------------------------------- */
    memset(cnt, 0, (size_t)nchk * (size_t)groups * (size_t)bs->planes * sizeof(uint64_t));
    memset(xr, 0, (size_t)nchk * (size_t)groups * sizeof(uint64_t));
    memset(act, 0, (size_t)nchk * (size_t)groups * sizeof(uint64_t));

    for (v = 0; v < n; v++) {
        const uint64_t *uu = &unk[(size_t)v * groups];
        const uint64_t *vv = &val[(size_t)v * groups];
        for (e = g->voff[v]; e < g->voff[v + 1]; e++) {
            uint64_t *xp = &xr[(size_t)g->vadj[e] * groups];
            for (gg = 0; gg < groups; gg++) {
                if (uu[gg])
                    count_add_n(&cnt[((size_t)g->vadj[e] * groups + gg) *
                                     bs->planes],
                                uu[gg], bs->planes);
                xp[gg] ^= vv[gg];
            }
        }
    }

    for (c = 0; c < nchk; c++) {
        int any = 0;
        for (gg = 0; gg < groups; gg++) {
            const uint64_t *p =
                &cnt[((size_t)c * groups + gg) * bs->planes];
            uint64_t a = count_one_n(p, bs->planes);
            act[(size_t)c * groups + gg] = a;
            any |= (a != 0);
        }
        if (any)
            stack[sp++] = c;
    }

#ifdef BS_TIMING
    bs->t_init += bs_now() - t0;
    t0 = bs_now();
#endif

    /* ---- wave peeling ------------------------------------------- */
    /*
     * All checks that are active at the start of a wave are scanned before
     * any variable is updated.  A variable resolved by several checks in the
     * same wave accumulates one mask and one value mask, and propagates to
     * its checks once.  This is the scalar decoder's wave schedule: the
     * fixed point and the values are identical, but the 64-wide propagation
     * is done once per (variable, wave) instead of once per (variable,
     * check) event, which removes most of the sparsity cost of independent
     * codeword schedules.
     */
    {
        int *list = stack;
        int nlist = sp;

        while (nlist > 0) {
            int ntouch = 0;
            int nnext = 0;
            int li;

            bs->st.waves++;

            for (li = 0; li < nlist; li++) {
                c = list[li];
                for (e = g->coff[c]; e < g->coff[c + 1]; e++) {
                    int any2 = 0;
                    v = g->cadj[e];
                    for (gg = 0; gg < groups; gg++) {
                        res[gg] = act[(size_t)c * groups + gg] &
                                  unk[(size_t)v * groups + gg];
                        any2 |= (res[gg] != 0);
                    }
                    if (!any2)
                        continue;
                    if (!bs->tflag[v]) {
                        bs->tflag[v] = 1;
                        bs->touched[ntouch++] = v;
                    }
                    for (gg = 0; gg < groups; gg++) {
                        uint64_t m = res[gg];
                        if (m) {
                            bs->resolve[(size_t)v * groups + gg] |= m;
                            bs->vbits[(size_t)v * groups + gg] |=
                                xr[(size_t)c * groups + gg] & m;
                        }
                    }
                    bs->st.check_scans++;
                }
            }

            for (li = 0; li < ntouch; li++) {
                v = bs->touched[li];
                switch (bs->planes) {
                case 3:
                    propagate_3(bs, v,
                                &bs->resolve[(size_t)v * groups],
                                &bs->vbits[(size_t)v * groups]);
                    break;
                case 4:
                    propagate_4(bs, v,
                                &bs->resolve[(size_t)v * groups],
                                &bs->vbits[(size_t)v * groups]);
                    break;
                default:
                    propagate_5(bs, v,
                                &bs->resolve[(size_t)v * groups],
                                &bs->vbits[(size_t)v * groups]);
                    break;
                }
                for (gg = 0; gg < groups; gg++) {
                    bs->resolve[(size_t)v * groups + gg] = 0;
                    bs->vbits[(size_t)v * groups + gg] = 0;
                }
                bs->tflag[v] = 0;
            }

            nnext = 0;
            for (c = 0; c < nchk; c++) {
                int any = 0;
                for (gg = 0; gg < groups; gg++)
                    any |= (act[(size_t)c * groups + gg] != 0);
                if (any)
                    list[nnext++] = c;
            }
            nlist = nnext;
        }
    }

#ifdef BS_TIMING
    bs->t_wave += bs_now() - t0;
    t0 = bs_now();
#endif

    /* ---- unresolved statistics ---------------------------------- */
    if (unresolved) {
        uint64_t any[BS_MAX_GROUPS];
        long counts[BS_GROUP];

        for (gg = 0; gg < groups; gg++)
            any[gg] = 0;
        for (v = 0; v < lim; v++)
            for (gg = 0; gg < groups; gg++)
                any[gg] |= unk[(size_t)v * groups + gg];

        for (gg = 0; gg < groups; gg++) {
            int jj;
            if (any[gg] == 0) {
                for (jj = 0; jj < BS_GROUP; jj++)
                    unresolved[gg * BS_GROUP + jj] = 0;
                recovered += BS_GROUP;
                continue;
            }
            memset(counts, 0, sizeof(counts));
            for (v = 0; v < lim; v++) {
                uint64_t w = unk[(size_t)v * groups + gg];
                while (w) {
                    int b = __builtin_ctzll(w);
                    counts[b]++;
                    w &= w - 1;
                }
            }
            for (jj = 0; jj < BS_GROUP; jj++) {
                unresolved[gg * BS_GROUP + jj] = counts[jj];
                if (counts[jj] == 0)
                    recovered++;
            }
        }
    } else {
        uint64_t any[BS_MAX_GROUPS];
        for (gg = 0; gg < groups; gg++)
            any[gg] = 0;
        for (v = 0; v < lim; v++)
            for (gg = 0; gg < groups; gg++)
                any[gg] |= unk[(size_t)v * groups + gg];
        for (gg = 0; gg < groups; gg++)
            recovered += BS_GROUP - __builtin_popcountll(any[gg]);
    }

#ifdef BS_TIMING
    bs->t_unres += bs_now() - t0;
    t0 = bs_now();
#endif

    /* ---- output transpose --------------------------------------- */
    memset(out, 0, (size_t)bs->batch * stride);
    for (v0 = 0; v0 + BS_GROUP <= n; v0 += BS_GROUP)
        store_block(out, stride, val, groups, v0, rows);
    if (v0 < n)
        store_tail(out, stride, val, groups, n, v0);

#ifdef BS_TIMING
    bs->t_out += bs_now() - t0;
    }
#endif
    return recovered;
}

#ifdef BS_TIMING
void sc_bs_timing(const sc_bs *bs, double *init, double *wave, double *unres,
                  double *out)
{
    *init = bs->t_init;
    *wave = bs->t_wave;
    *unres = bs->t_unres;
    *out = bs->t_out;
}
#endif
