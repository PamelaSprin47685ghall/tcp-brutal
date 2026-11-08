/*
 * sc_ldpc.c - spatially coupled LDPC / IRA for the binary erasure channel.
 *
 * Clean-room C99 implementation.  The construction rules and the BEC
 * message semantics follow npminh12's "Spatially-coupled-LDPC" simulation:
 *
 *   https://github.com/npminh12/Spatially-coupled-LDPC
 *   commit 3d403f4, MIT License, Copyright (c) 2019 npminh12
 *
 * No C++ code was copied; the original was read only to recover the graph
 * rules (coupling_regular, protograph, complete_graph) and the peeling
 * semantics of BPDecoder_BEC.  See review/sc-ldpc-sim.md for the rules
 * restated in prose and for the documented deviations.
 *
 * Reference graph (SC_TERM_NATIVE), as in npminh12:
 *
 *   - Protograph: regular (l,k) = (5,10), q = k/l = 2.
 *   - Coupling position i in [0,L) carries q = 2 variable columns of m
 *     copies each: column (r,i), r in {0,1}, connected to check supernodes
 *     i .. i+4.  Lifting uses one random permutation per (column, supernode)
 *     edge, so copy b of column (r,i) lands on check a*m + perm[b].
 *   - Native termination keeps L+4 check supernodes; M = (L+4)*m and the
 *     design rate is 1-(L+4)/(2L).
 *
 * The production construction is SC_TERM_ACC (see sc_ldpc.h):
 *
 *   - Payload block i (m bits) is connected to a ring window of five check
 *     supernodes, base = floor(i*n_base/L), checks (base..base+4) mod
 *     n_base, one random permutation per edge.
 *   - Parity block a owns check a and also feeds check a+1; the last parity
 *     stops at check n_base-1.  This is an accumulator, so the encoder is a
 *     single forward sweep and H_parity is triangular (never singular).
 *   - n_base*m checks are used at rate 0.50; rate 0.60/0.70 shrink n_base,
 *     rate below 0.50 would append extra checks (build_extra()).
 *
 * Why not the literal causal truncation: dropping the trailing four check
 * supernodes leaves the last variable block with payload and parity both
 * incident to a single check.  Two erased bits then form a permanent
 * stopping set and the full-recovery probability is zero for every L.  The
 * ring payload window removes that stopping set while keeping rate 0.50 and
 * the 5-check payload degree.
 */

#include "tbt_sc_ldpc.h"

/* ------------------------------------------------------------------ */
/* deterministic RNG (splitmix64 seeding, xorshift128+ stream)          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t s0, s1;
} rng_t;

static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void rng_seed(rng_t *r, uint64_t seed)
{
    uint64_t x = seed;
    r->s0 = splitmix64(&x);
    r->s1 = splitmix64(&x);
    if (r->s0 == 0 && r->s1 == 0)
        r->s0 = 1;
}

static uint64_t rng_next(rng_t *r)
{
    uint64_t x = r->s0;
    uint64_t const y = r->s1;
    r->s0 = y;
    x ^= x << 23;
    r->s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
    return r->s1 + y;
}

/* uniform in [0, n), n > 0; multiply-shift, bias < n/2^32 */
static uint32_t rng_below(rng_t *r, uint32_t n)
{
    return (uint32_t)(((rng_next(r) >> 32) * (uint64_t)n) >> 32);
}

static void rng_shuffle(rng_t *r, int *a, int n)
{
    int i;
    for (i = 0; i < n - 1; i++) {
        int j = i + (int)rng_below(r, (uint32_t)(n - i));
        int t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

/* ------------------------------------------------------------------ */
/* code object                                                         */
/* ------------------------------------------------------------------ */

#define SC_L 5 /* variable degree, npminh12 l_LDPC_reg_ensemble */
#define SC_Q 2 /* k/l, npminh12 k_LDPC_reg_ensemble / l */

struct sc_code {
    sc_params par;
    int m;       /* lifting factor = block/q */
    int L;       /* coupling length */
    int n_super; /* check supernodes used: L (causal) or L+4 (native) */
    int n_base;  /* base check supernodes owned by one parity each */
    int n, n_data, n_par, n_chk, n_extra;
    int dl_max, dc_max;
    long edges;

    int *voff; /* n + 1 */
    int *coff; /* n_chk + 1 */
    int *vadj; /* edges */
    int *cadj; /* edges */
    int *owned; /* n_chk: parity variable solved by this check, -1 if none */

    /* decoder scratch */
    uint8_t *known;
    int8_t *val;
    uint16_t *cx; /* per check: low byte XOR of known values, high byte unknowns */
    int *queue;

    size_t mem_bytes;
};

static void *zalloc(size_t n, size_t sz)
{
    return sclc_zalloc(n * sz);
}

static void *zmalloc(size_t n, size_t sz)
{
    return sclc_zalloc(n * sz);
}

void sc_free(sc_code *c)
{
    if (!c)
        return;
    sclc_free(c->voff);
    sclc_free(c->coff);
    sclc_free(c->vadj);
    sclc_free(c->cadj);
    sclc_free(c->owned);
    sclc_free(c->known);
    sclc_free(c->val);
    sclc_free(c->cx);
    sclc_free(c->queue);
    sclc_free(c);
}

int sc_data_var(const sc_code *c, int k)
{
    if (k < 0 || k >= c->n_data)
        return -1;
    return k; /* payload variables occupy 0 .. n_data-1 */
}

void sc_get_graph(const sc_code *c, sc_graph *out)
{
    out->n = c->n;
    out->n_data = c->n_data;
    out->n_par = c->n_par;
    out->n_chk = c->n_chk;
    out->edges = c->edges;
    out->dc_max = c->dc_max;
    out->voff = c->voff;
    out->coff = c->coff;
    out->vadj = c->vadj;
    out->cadj = c->cadj;
    out->owned = c->owned;
}

int sc_systematic(const sc_code *c)
{
    return c->par.term != SC_TERM_NATIVE;
}

const uint8_t *sc_known_bits(const sc_code *c)
{
    return c->known;
}

/*
 * Extra check nodes for target rates below 0.50.
 *
 * Rule: n_extra = round(n_data*(1-R)/R) - n_data is the number of parity
 * bits the target rate needs beyond the base code.  Each extra check node
 * owns one extra parity variable.  Extra check j sits at coupling position
 * a = floor(j*L/n_extra); its payload edges use the same window rule as a
 * base check: one payload variable from each of up to extra_dc variable
 * blocks spread evenly over max(0,a-4) .. min(L-1,a), the copy inside a
 * block being drawn uniformly.  Its degree is (blocks + 1).
 *
 * The extra payload edges raise the payload variable degree above 5, so the
 * payload degree distribution is irregular; the extra checks are regular of
 * degree extra_dc+1.
 */
static int build_extra(sc_code *c, rng_t *rng, int **out_v, int **out_c)
{
    int n_extra = c->n_extra;
    int per = c->par.extra_dc + 1;
    int *ev, *ec;
    int j, t;

    ev = zmalloc((size_t)n_extra * (size_t)per, sizeof(int));
    ec = zmalloc((size_t)n_extra * (size_t)per, sizeof(int));
    if (!ev || !ec) {
        sclc_free(ev);
        sclc_free(ec);
        return -1;
    }
    for (j = 0; j < n_extra * per; j++)
        ev[j] = -1;

    for (j = 0; j < n_extra; j++) {
        int a = (int)((int64_t)j * c->L / n_extra);
        int lo = a - (SC_L - 1);
        int hi = a;
        int w, take, base = j * per;
        if (lo < 0)
            lo = 0;
        if (hi > c->L - 1)
            hi = c->L - 1;
        w = hi - lo + 1;
        take = c->par.extra_dc < w ? c->par.extra_dc : w;
        for (t = 0; t < take; t++) {
            int blk;
            int v;
            if (take == 1)
                blk = lo;
            else
                blk = lo + (int)(((int64_t)t * (w - 1) + (take - 1) / 2) /
                                 (take - 1));
            if (blk > hi)
                blk = hi;
            v = blk * c->m + (int)rng_below(rng, (uint32_t)c->m);
            ev[base + t] = v;
            ec[base + t] = c->n_base * c->m + j;
        }
        /* the extra parity variable owned by this check */
        ev[base + take] = 2 * c->L * c->m + j;
        ec[base + take] = c->n_base * c->m + j;
    }
    *out_v = ev;
    *out_c = ec;
    return 0;
}

sc_code *sc_build(const sc_params *p, char *err, size_t errlen)
{
    sc_code *c;
    rng_t rng;
    int *vdeg = NULL, *cdeg = NULL, *perm = NULL;
    int *ev = NULL, *ec = NULL;
    int i, r, e, b, j, k;
    long edges = 0;
    size_t mem = 0;

    if (!p) {
        snprintf(err, errlen, "null params");
        return NULL;
    }
    if (p->block < 16 || (p->block & 1) || p->block > 8192) {
        snprintf(err, errlen, "block must be even and in [16,8192]");
        return NULL;
    }
    if (p->L < 2 || p->L > 4096) {
        snprintf(err, errlen, "L must be in [2,4096]");
        return NULL;
    }
    if (!p->rate_num || !p->rate_den || p->rate_num > p->rate_den) {
        snprintf(err, errlen, "rate must be a fraction in (0,1]");
        return NULL;
    }
    if (p->term != SC_TERM_CAUSAL && p->term != SC_TERM_NATIVE &&
        p->term != SC_TERM_ACC) {
        snprintf(err, errlen, "bad termination mode");
        return NULL;
    }
    if (p->term == SC_TERM_NATIVE && p->rate_num * 2 != p->rate_den) {
        snprintf(err, errlen,
                 "native termination has a fixed rate; use acc or causal");
        return NULL;
    }
    if (p->term == SC_TERM_CAUSAL && p->rate_num * 2 != p->rate_den) {
        snprintf(err, errlen, "causal termination supports rate 0.50 only");
        return NULL;
    }
    if (p->extra_dc < 1 || p->extra_dc > 16) {
        snprintf(err, errlen, "extra_dc must be in [1,16]");
        return NULL;
    }

    c = zalloc(1, sizeof(*c));
    if (!c) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    c->par = *p;
    c->m = p->block / SC_Q;
    c->L = p->L;
    c->n_super = (p->term == SC_TERM_NATIVE) ? p->L + SC_L - 1 : p->L;

    c->n_data = c->L * c->m;
    c->n_base = c->L;
    c->n_extra = 0;
    if (p->term == SC_TERM_ACC && p->rate_num * 2 != p->rate_den) {
        /* Target parity count: n_data*(1-R)/R.  Above 0.50 the base check
         * count shrinks; below 0.50 extra checks are appended. */
        long n_par = (long)c->n_data *
                     (long)(p->rate_den - p->rate_num) / (long)p->rate_num;
        if (p->rate_num * 2 > p->rate_den) {
            long nb = (n_par + c->m / 2) / c->m;
            if (nb < 1)
                nb = 1;
            c->n_base = (int)nb;
        } else {
            long extra = n_par - (long)c->n_data;
            c->n_extra = extra > 0 ? (int)extra : 0;
        }
    }

    if (p->term != SC_TERM_NATIVE) {
        c->n_par = c->n_base * c->m + c->n_extra;
        c->n = c->n_data + c->n_par;
        c->n_chk = c->n_base * c->m + c->n_extra;
    } else {
        c->n = 2 * c->n_data;
        c->n_par = 0;
        c->n_chk = c->n_super * c->m;
    }

    rng_seed(&rng, p->seed);

    /* ---- pass 1: degrees ---------------------------------------- */
    vdeg = zalloc((size_t)c->n, sizeof(int));
    cdeg = zalloc((size_t)c->n_chk, sizeof(int));
    perm = zmalloc((size_t)c->m, sizeof(int));
    if (!vdeg || !cdeg || !perm) {
        snprintf(err, errlen, "out of memory");
        goto fail;
    }

    if (p->term == SC_TERM_ACC) {
        /* Payload block i maps to base check floor(i*n_base/L) and owns a
         * ring window of SC_L checks.  Parity block a owns check a and
         * feeds check a+1 (accumulator); the last parity stops. */
        for (i = 0; i < c->L; i++) {
            int base = (int)((long long)i * c->n_base / c->L);
            for (b = 0; b < c->m; b++)
                vdeg[i * c->m + b] += SC_L;
            for (r = 0; r < SC_L; r++)
                for (b = 0; b < c->m; b++)
                    cdeg[((base + r) % c->n_base) * c->m + b]++;
        }
        for (i = 0; i < c->n_base; i++) {
            for (b = 0; b < c->m; b++)
                vdeg[c->L * c->m + i * c->m + b] +=
                    (i < c->n_base - 1) ? 2 : 1;
            for (b = 0; b < c->m; b++)
                cdeg[i * c->m + b]++;
            if (i < c->n_base - 1)
                for (b = 0; b < c->m; b++)
                    cdeg[(i + 1) * c->m + b]++;
        }
    } else {
        for (i = 0; i < c->L; i++) {
            int last = i + SC_L;
            if (last > c->n_super)
                last = c->n_super;
            for (r = 0; r < SC_Q; r++)
                for (b = 0; b < c->m; b++)
                    vdeg[r * c->L * c->m + i * c->m + b] += last - i;
        }
        for (i = 0; i < c->n_super; i++) {
            int lo = i - (SC_L - 1);
            int hi = i;
            if (lo < 0)
                lo = 0;
            if (hi > c->L - 1)
                hi = c->L - 1;
            for (b = 0; b < c->m; b++)
                cdeg[i * c->m + b] = SC_Q * (hi - lo + 1);
        }
    }

    if (c->n_extra > 0) {
        int per;
        if (build_extra(c, &rng, &ev, &ec) != 0) {
            snprintf(err, errlen, "out of memory");
            goto fail;
        }
        per = p->extra_dc + 1;
        for (j = 0; j < c->n_extra; j++) {
            for (k = 0; k < per; k++) {
                int v = ev[j * per + k];
                int ck = ec[j * per + k];
                if (v < 0)
                    continue;
                vdeg[v]++;
                cdeg[ck]++;
            }
        }
    }

    /* ---- CSR offsets -------------------------------------------- */
    c->voff = zalloc((size_t)c->n + 1, sizeof(int));
    c->coff = zalloc((size_t)c->n_chk + 1, sizeof(int));
    if (!c->voff || !c->coff) {
        snprintf(err, errlen, "out of memory");
        goto fail;
    }
    for (i = 0; i < c->n; i++)
        c->voff[i + 1] = c->voff[i] + vdeg[i];
    for (i = 0; i < c->n_chk; i++)
        c->coff[i + 1] = c->coff[i] + cdeg[i];
    edges = c->coff[c->n_chk];
    c->edges = edges;
    c->vadj = zmalloc((size_t)edges, sizeof(int));
    c->cadj = zmalloc((size_t)edges, sizeof(int));
    c->owned = zmalloc((size_t)c->n_chk, sizeof(int));
    if (!c->vadj || !c->cadj || !c->owned) {
        snprintf(err, errlen, "out of memory");
        goto fail;
    }
    for (i = 0; i < c->n_chk; i++)
        c->owned[i] = -1;

    /* ---- fill the variable side -------------------------------- */
    {
        int *cur = sclc_zalloc((size_t)c->n * sizeof(int));
        if (!cur) {
            snprintf(err, errlen, "out of memory");
            goto fail;
        }
        memcpy(cur, c->voff, (size_t)c->n * sizeof(int));
        if (p->term == SC_TERM_ACC) {
            for (i = 0; i < c->L; i++) {
                int vbase = i * c->m;
                int bbase = (int)((long long)i * c->n_base / c->L);
                for (e = 0; e < SC_L; e++) {
                    int ckbase = ((bbase + e) % c->n_base) * c->m;
                    int tries = 0;
                    /*
                     * When n_base < SC_L the ring window revisits a check
                     * supernode.  Draw a permutation that does not repeat a
                     * check node already adjacent to the same variable, so
                     * the lifted graph has no parallel edges.
                     */
                    if (c->n_base < SC_L) {
                        for (;;) {
                            int ok = 1;
                            for (b = 0; b < c->m; b++)
                                perm[b] = b;
                            rng_shuffle(&rng, perm, c->m);
                            for (b = 0; b < c->m && ok; b++) {
                                int ck = ckbase + perm[b];
                                int kk;
                                for (kk = c->voff[vbase + b];
                                     kk < cur[vbase + b]; kk++)
                                    if (c->vadj[kk] == ck) {
                                        ok = 0;
                                        break;
                                    }
                            }
                            if (ok || ++tries >= 100)
                                break;
                        }
                    } else {
                        for (b = 0; b < c->m; b++)
                            perm[b] = b;
                        rng_shuffle(&rng, perm, c->m);
                    }
                    for (b = 0; b < c->m; b++)
                        c->vadj[cur[vbase + b]++] = ckbase + perm[b];
                }
            }
            for (i = 0; i < c->n_base; i++) {
                int pbase = c->L * c->m + i * c->m;
                for (b = 0; b < c->m; b++)
                    perm[b] = b;
                rng_shuffle(&rng, perm, c->m);
                for (b = 0; b < c->m; b++)
                    c->vadj[cur[pbase + b]++] = i * c->m + perm[b];
                if (i < c->n_base - 1) {
                    for (b = 0; b < c->m; b++)
                        perm[b] = b;
                    rng_shuffle(&rng, perm, c->m);
                    for (b = 0; b < c->m; b++)
                        c->vadj[cur[pbase + b]++] =
                            (i + 1) * c->m + perm[b];
                }
            }
        } else {
            for (i = 0; i < c->L; i++) {
                for (r = 0; r < SC_Q; r++) {
                    int last = i + SC_L;
                    int base = r * c->L * c->m + i * c->m;
                    if (last > c->n_super)
                        last = c->n_super;
                    for (e = i; e < last; e++) {
                        for (b = 0; b < c->m; b++)
                            perm[b] = b;
                        rng_shuffle(&rng, perm, c->m);
                        for (b = 0; b < c->m; b++) {
                            int v = base + b;
                            int ck = e * c->m + perm[b];
                            c->vadj[cur[v]++] = ck;
                        }
                    }
                }
            }
        }
        if (c->n_extra > 0) {
            int per = p->extra_dc + 1;
            for (j = 0; j < c->n_extra; j++) {
                for (k = 0; k < per; k++) {
                    int v = ev[j * per + k];
                    if (v < 0)
                        continue;
                    c->vadj[cur[v]++] = ec[j * per + k];
                }
            }
        }
        sclc_free(cur);
    }

    /* ---- fill the check side, positions ------------------------- */
    {
        int *cur = zalloc((size_t)c->n_chk, sizeof(int));
        if (!cur) {
            snprintf(err, errlen, "out of memory");
            goto fail;
        }
        for (i = 0; i < c->n; i++) {
            int j0 = c->voff[i], j1 = c->voff[i + 1];
            for (k = j0; k < j1; k++) {
                int ck = c->vadj[k];
                int pos = cur[ck]++;
                c->cadj[c->coff[ck] + pos] = i;
            }
        }
        for (i = 0; i < c->n_chk; i++) {
            if (cur[i] != c->coff[i + 1] - c->coff[i]) {
                snprintf(err, errlen, "internal: check degree mismatch");
                sclc_free(cur);
                goto fail;
            }
        }
        sclc_free(cur);
    }

    /*
     * owned[]: the parity copy that check (a,b) must solve.  Walk every
     * parity variable; the edge whose check lives in supernode a == i is the
     * diagonal one, and it is unique per check because the lifting
     * permutation is a bijection.
     */
    if (p->term != SC_TERM_NATIVE) {
        for (i = 0; i < c->n_base; i++) {
            int base = c->L * c->m + i * c->m;
            int v;
            for (v = 0; v < c->m; v++) {
                int var = base + v;
                int kk;
                for (kk = c->voff[var]; kk < c->voff[var + 1]; kk++) {
                    int ck = c->vadj[kk];
                    if (ck / c->m == i) {
                        c->owned[ck] = var;
                        break;
                    }
                }
            }
        }
        for (j = 0; j < c->n_extra; j++) {
            int ck = c->n_base * c->m + j;
            int pv = 2 * c->L * c->m + j;
            c->owned[ck] = pv;
        }
    }

    /* ---- decoder scratch and statistics ------------------------- */
    c->known = zalloc((size_t)c->n, 1);
    c->val = zalloc((size_t)c->n, 1);
    c->cx = zalloc((size_t)c->n_chk, sizeof(uint16_t));
    c->queue = zalloc((size_t)c->n_chk, sizeof(int));
    if (!c->known || !c->val || !c->cx || !c->queue) {
        snprintf(err, errlen, "out of memory");
        goto fail;
    }

    c->dl_max = 0;
    for (i = 0; i < c->n; i++) {
        int d = c->voff[i + 1] - c->voff[i];
        if (d > c->dl_max)
            c->dl_max = d;
    }
    c->dc_max = 0;
    for (i = 0; i < c->n_chk; i++) {
        int d = c->coff[i + 1] - c->coff[i];
        if (d > c->dc_max)
            c->dc_max = d;
    }

    mem = (size_t)(c->n + 1) * sizeof(int) +
          (size_t)(c->n_chk + 1) * sizeof(int) +
          (size_t)edges * 2 * sizeof(int) +
          (size_t)c->n_chk * sizeof(int) + (size_t)c->n * 2 +
          (size_t)c->n_chk * sizeof(uint16_t) +
          (size_t)c->n_chk * sizeof(int) + sizeof(*c);
    c->mem_bytes = mem;

    sclc_free(vdeg);
    sclc_free(cdeg);
    sclc_free(perm);
    sclc_free(ev);
    sclc_free(ec);
    return c;

fail:
    sclc_free(vdeg);
    sclc_free(cdeg);
    sclc_free(perm);
    sclc_free(ev);
    sclc_free(ec);
    sc_free(c);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* encoder                                                             */
/* ------------------------------------------------------------------ */

int sc_encode(sc_code *c, uint8_t *code)
{
    int a, b, j, k;

    if (c->par.term == SC_TERM_NATIVE)
        return -1;

    for (a = 0; a < c->n_base; a++) {
        for (b = 0; b < c->m; b++) {
            int ck = a * c->m + b;
            int p = c->owned[ck];
            uint8_t x = 0;
            if (p < 0)
                return -1;
            for (k = c->coff[ck]; k < c->coff[ck + 1]; k++) {
                int v = c->cadj[k];
                if (v != p)
                    x ^= (uint8_t)(code[v] & 1u);
            }
            code[p] = x;
        }
    }
    for (j = 0; j < c->n_extra; j++) {
        int ck = c->n_base * c->m + j;
        int p = c->owned[ck];
        uint8_t x = 0;
        if (p < 0)
            return -1;
        for (k = c->coff[ck]; k < c->coff[ck + 1]; k++) {
            int v = c->cadj[k];
            if (v != p)
                x ^= (uint8_t)(code[v] & 1u);
        }
        code[p] = x;
    }
    return 0;
}

int sc_verify(const sc_code *c, const uint8_t *code)
{
    int ck, k;
    for (ck = 0; ck < c->n_chk; ck++) {
        uint8_t x = 0;
        for (k = c->coff[ck]; k < c->coff[ck + 1]; k++)
            x ^= (uint8_t)(code[c->cadj[k]] & 1u);
        if (x != 0)
            return ck + 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* peeling decoder                                                     */
/* ------------------------------------------------------------------ */

sc_dec_stat sc_decode(sc_code *c, const uint8_t *recv, const uint8_t *erased,
                      uint8_t *out, int target_data_only)
{
    sc_dec_stat st;
    int n = c->n, nchk = c->n_chk;
    uint8_t *known = c->known;
    int8_t *val = c->val;
    uint16_t *cx = c->cx;
    int *queue = c->queue;
    int qh = 0, qt = 0;
    int v, ck;
    long visits = 0;

    memset(cx, 0, (size_t)nchk * sizeof(uint16_t));
    for (v = 0; v < n; v++) {
        int k;
        if (erased[v]) {
            val[v] = -1;
            for (k = c->voff[v]; k < c->voff[v + 1]; k++)
                cx[c->vadj[k]] += 0x100u;
        } else {
            uint8_t x = (uint8_t)(recv[v] & 1u);
            val[v] = (int8_t)x;
            for (k = c->voff[v]; k < c->voff[v + 1]; k++)
                cx[c->vadj[k]] ^= x;
        }
    }
    for (ck = 0; ck < nchk; ck++) {
        visits += c->coff[ck + 1] - c->coff[ck];
        if ((cx[ck] >> 8) == 1)
            queue[qt++] = ck;
    }

    st.rounds = 0;
    while (qh < qt) {
        int end = qt;
        st.rounds++;
        while (qh < end) {
            int k0, k1, k, uv = -1;
            uint8_t x;
            ck = queue[qh++];
            if ((cx[ck] >> 8) != 1)
                continue;
            k0 = c->coff[ck];
            k1 = c->coff[ck + 1];
            for (k = k0; k < k1; k++) {
                int vv = c->cadj[k];
                visits++;
                if (val[vv] < 0) {
                    uv = vv;
                    break;
                }
            }
            if (uv < 0) {
                cx[ck] = 0;
                continue;
            }
            x = (uint8_t)cx[ck];
            val[uv] = (int8_t)x;
            cx[ck] = 0;
            for (k = c->voff[uv]; k < c->voff[uv + 1]; k++) {
                int c2 = c->vadj[k];
                visits++;
                if (c2 == ck)
                    continue;
                cx[c2] = (uint16_t)((cx[c2] - 0x100u) ^ x);
                if ((cx[c2] >> 8) == 1)
                    queue[qt++] = c2;
            }
        }
    }

    {
        long tgt = target_data_only ? c->n_data : c->n;
        long unres = 0;
        for (v = 0; v < tgt; v++)
            if (val[v] < 0)
                unres++;
        st.unresolved = unres;
        st.target = tgt;
        st.resolved = tgt - unres;
    }
    st.edge_visits = visits;
    for (v = 0; v < n; v++) {
        known[v] = (val[v] >= 0);
        out[v] = (uint8_t)(val[v] < 0 ? 0 : val[v]);
    }
    return st;
}
