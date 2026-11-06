/*
 * sc_ldpc_gf16.c - q-ary GF(16) SC-IRA prototype for the BEC.
 *
 * Clean-room C99.  The graph construction is reused from sc_ldpc.c through
 * sc_get_graph(); this file adds the GF(16) coefficients, the forward
 * encoder and the q-ary peeling decoder.  See sc_ldpc_gf16.h for the model.
 */

#include "sc_ldpc_gf16.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* GF(16), primitive polynomial x^4 + x + 1 (0x13)                     */
/* ------------------------------------------------------------------ */

static uint8_t gf_mul_tab[256];
static uint8_t gf_inv_tab[16];
static int gf_ready;

void gf16_init(void)
{
    uint8_t exp[16], log[16];
    int i, a, b, x = 1;

    if (gf_ready)
        return;
    for (i = 0; i < 15; i++) {
        exp[i] = (uint8_t)x;
        log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x10)
            x ^= 0x13;
    }
    exp[15] = exp[0];
    for (a = 0; a < 16; a++) {
        for (b = 0; b < 16; b++) {
            if (a == 0 || b == 0)
                gf_mul_tab[(a << 4) | b] = 0;
            else
                gf_mul_tab[(a << 4) | b] =
                    exp[(log[a] + log[b]) % 15];
        }
    }
    gf_inv_tab[0] = 0;
    for (a = 1; a < 16; a++)
        gf_inv_tab[a] = exp[(15 - log[a]) % 15];
    gf_ready = 1;
}

static inline uint8_t gf_mul(uint8_t a, uint8_t b)
{
    return gf_mul_tab[((int)a << 4) | b];
}

/* ------------------------------------------------------------------ */
/* deterministic RNG for coefficients (splitmix64 + xorshift128+)      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t s0, s1;
} gf_rng;

static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void rng_seed(gf_rng *r, uint64_t seed)
{
    uint64_t x = seed;
    r->s0 = splitmix64(&x);
    r->s1 = splitmix64(&x);
    if (r->s0 == 0 && r->s1 == 0)
        r->s0 = 1;
}

static uint64_t rng_next(gf_rng *r)
{
    uint64_t x = r->s0;
    uint64_t const y = r->s1;
    r->s0 = y;
    x ^= x << 23;
    r->s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
    return r->s1 + y;
}

/* uniform in [0, n), n > 0 */
static uint32_t rng_below(gf_rng *r, uint32_t n)
{
    return (uint32_t)(((rng_next(r) >> 32) * (uint64_t)n) >> 32);
}

/* ------------------------------------------------------------------ */
/* code object                                                         */
/* ------------------------------------------------------------------ */

struct gf16_code {
    int n, n_data, n_par, n_chk;
    int dc_max, dv_max;
    long edges;

    int *cdeg;       /* n_chk */
    int *vdeg;       /* n */
    uint16_t *cadj;  /* n_chk * dc_max */
    uint8_t *ccoef;  /* n_chk * dc_max */
    uint16_t *vadj;  /* n * dv_max */
    uint8_t *vcoef;  /* n * dv_max */
    uint16_t *owned; /* n_chk */

    /* decoder scratch */
    uint16_t *cx;   /* low 4 bits: known-value XOR; high byte: unknowns */
    uint8_t *val;   /* 0..15, 0xFF = unknown */
    uint8_t *known; /* 1 = value known */
    uint16_t *queue;

    size_t mem_bytes;
};

static void *zalloc(size_t n, size_t sz)
{
    if (n == 0)
        n = 1;
    return calloc(n, sz);
}

void gf16_free(gf16_code *c)
{
    if (!c)
        return;
    free(c->cdeg);
    free(c->vdeg);
    free(c->cadj);
    free(c->ccoef);
    free(c->vadj);
    free(c->vcoef);
    free(c->owned);
    free(c->cx);
    free(c->val);
    free(c->known);
    free(c->queue);
    free(c);
}

int gf16_n(const gf16_code *c)
{
    return c->n;
}

int gf16_n_data(const gf16_code *c)
{
    return c->n_data;
}

void gf16_get_info(const gf16_code *c, sc_info *out)
{
    out->n = c->n;
    out->n_data = c->n_data;
    out->n_par = c->n_par;
    out->n_chk = c->n_chk;
    out->n_extra = 0;
    out->edges = c->edges;
    out->dl_max = c->dv_max;
    out->dc_max = c->dc_max;
    out->rate = (double)c->n_data / (double)c->n;
    out->mem_bytes = c->mem_bytes;
}

gf16_code *gf16_build(const sc_params *p, uint64_t coef_seed, char *err,
                      size_t errlen)
{
    sc_code *sc = NULL;
    sc_graph g;
    gf16_code *c = NULL;
    int *cur = NULL;
    int i, ck, k;
    size_t mem = 0;

    gf16_init();

    if (!p) {
        snprintf(err, errlen, "null params");
        return NULL;
    }
    if (p->term != SC_TERM_ACC) {
        snprintf(err, errlen, "gf16 requires SC_TERM_ACC");
        return NULL;
    }

    sc = sc_build(p, err, errlen);
    if (!sc)
        return NULL;
    sc_get_graph(sc, &g);

    if (g.n <= 0 || g.n_chk <= 0 || g.n_data <= 0) {
        snprintf(err, errlen, "empty graph");
        goto fail;
    }
    if (g.n >= 65536 || g.n_chk >= 65536) {
        snprintf(err, errlen, "graph too large for uint16 indices");
        goto fail;
    }

    c = zalloc(1, sizeof(*c));
    if (!c) {
        snprintf(err, errlen, "out of memory");
        goto fail;
    }
    c->n = g.n;
    c->n_data = g.n_data;
    c->n_par = g.n_par;
    c->n_chk = g.n_chk;
    c->edges = g.edges;

    for (i = 0; i < g.n_chk; i++) {
        int d = g.coff[i + 1] - g.coff[i];
        if (d > c->dc_max)
            c->dc_max = d;
    }
    for (i = 0; i < g.n; i++) {
        int d = g.voff[i + 1] - g.voff[i];
        if (d > c->dv_max)
            c->dv_max = d;
    }
    if (c->dc_max <= 0 || c->dv_max <= 0) {
        snprintf(err, errlen, "bad degrees");
        goto fail;
    }

    c->cdeg = zalloc((size_t)c->n_chk, sizeof(int));
    c->vdeg = zalloc((size_t)c->n, sizeof(int));
    c->cadj = zalloc((size_t)c->n_chk * (size_t)c->dc_max,
                     sizeof(uint16_t));
    c->ccoef = zalloc((size_t)c->n_chk * (size_t)c->dc_max, 1);
    c->vadj = zalloc((size_t)c->n * (size_t)c->dv_max, sizeof(uint16_t));
    c->vcoef = zalloc((size_t)c->n * (size_t)c->dv_max, 1);
    c->owned = zalloc((size_t)c->n_chk, sizeof(uint16_t));
    cur = zalloc((size_t)c->n, sizeof(int));
    if (!c->cdeg || !c->vdeg || !c->cadj || !c->ccoef || !c->vadj ||
        !c->vcoef || !c->owned || !cur) {
        snprintf(err, errlen, "out of memory");
        goto fail;
    }

    {
        gf_rng rng;
        rng_seed(&rng, coef_seed);
        for (ck = 0; ck < c->n_chk; ck++) {
            int d = g.coff[ck + 1] - g.coff[ck];
            int own = g.owned[ck];
            int own_seen = 0;

            if (own < 0 || own >= c->n) {
                snprintf(err, errlen, "check %d has no owned parity", ck);
                goto fail;
            }
            c->cdeg[ck] = d;
            c->owned[ck] = (uint16_t)own;
            for (k = 0; k < d; k++) {
                int v = g.cadj[g.coff[ck] + k];
                uint8_t coef;
                int pos;

                if (v == own) {
                    coef = 1; /* owned coefficient must be 1 */
                    own_seen++;
                } else {
                    coef = (uint8_t)(1 + rng_below(&rng, 15));
                }
                c->cadj[(size_t)ck * c->dc_max + k] = (uint16_t)v;
                c->ccoef[(size_t)ck * c->dc_max + k] = coef;

                pos = cur[v]++;
                if (pos >= c->dv_max) {
                    snprintf(err, errlen, "variable degree overflow");
                    goto fail;
                }
                c->vadj[(size_t)v * c->dv_max + pos] = (uint16_t)ck;
                c->vcoef[(size_t)v * c->dv_max + pos] = coef;
            }
            if (own_seen != 1) {
                snprintf(err, errlen,
                         "check %d has %d owned-parity edges (want 1)", ck,
                         own_seen);
                goto fail;
            }
        }
    }

    for (i = 0; i < c->n; i++) {
        int want = g.voff[i + 1] - g.voff[i];
        if (cur[i] != want) {
            snprintf(err, errlen, "variable %d degree mismatch", i);
            goto fail;
        }
        c->vdeg[i] = cur[i];
    }
    free(cur);
    cur = NULL;

    c->cx = zalloc((size_t)c->n_chk, sizeof(uint16_t));
    c->val = zalloc((size_t)c->n, 1);
    c->known = zalloc((size_t)c->n, 1);
    c->queue = zalloc((size_t)c->n_chk, sizeof(uint16_t));
    if (!c->cx || !c->val || !c->known || !c->queue) {
        snprintf(err, errlen, "out of memory");
        goto fail;
    }

    mem = sizeof(*c) +
          (size_t)c->n_chk * sizeof(int) + (size_t)c->n * sizeof(int) +
          (size_t)c->n_chk * (size_t)c->dc_max * sizeof(uint16_t) +
          (size_t)c->n_chk * (size_t)c->dc_max +
          (size_t)c->n * (size_t)c->dv_max * sizeof(uint16_t) +
          (size_t)c->n * (size_t)c->dv_max +
          (size_t)c->n_chk * sizeof(uint16_t) +
          (size_t)c->n_chk * sizeof(uint16_t) + (size_t)c->n +
          (size_t)c->n + (size_t)c->n_chk * sizeof(uint16_t);
    c->mem_bytes = mem;

    sc_free(sc);
    return c;

fail:
    free(cur);
    if (sc)
        sc_free(sc);
    gf16_free(c);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* encoder / verifier                                                  */
/* ------------------------------------------------------------------ */

int gf16_encode(gf16_code *c, uint8_t *code)
{
    int ck, k;

    for (ck = 0; ck < c->n_chk; ck++) {
        int d = c->cdeg[ck];
        int own = c->owned[ck];
        uint8_t s = 0;
        int found = 0;

        for (k = 0; k < d; k++) {
            int v = c->cadj[(size_t)ck * c->dc_max + k];
            uint8_t coef = c->ccoef[(size_t)ck * c->dc_max + k];
            if (v == own) {
                found = 1;
                continue;
            }
            s ^= gf_mul(coef, code[v]);
        }
        if (!found)
            return -1;
        code[own] = s;
    }
    return 0;
}

int gf16_verify(const gf16_code *c, const uint8_t *code)
{
    int ck, k;

    for (ck = 0; ck < c->n_chk; ck++) {
        int d = c->cdeg[ck];
        uint8_t s = 0;
        for (k = 0; k < d; k++) {
            int v = c->cadj[(size_t)ck * c->dc_max + k];
            uint8_t coef = c->ccoef[(size_t)ck * c->dc_max + k];
            s ^= gf_mul(coef, code[v]);
        }
        if (s != 0)
            return ck + 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* q-ary peeling decoder                                               */
/* ------------------------------------------------------------------ */

/*
 * Algebraic core solve.  After peeling stalls, every unresolved variable
 * belongs to a small stopping set.  The binary decoder cannot recover such
 * a set when its GF(2) incidence matrix is rank-deficient.  Over GF(16) the
 * same edge pattern carries random nonzero coefficients, so the matrix is
 * often full rank.  Solve the core by Gaussian elimination and verify the
 * whole codeword before accepting the result.
 *
 * The routine is deliberately bounded: cores larger than max_core are
 * skipped, and the dense matrix is capped so a pathological core cannot
 * turn a decode into an unbounded allocation.
 */
static void gf16_core_solve(gf16_code *c, uint8_t *out, int target_data_only,
                            int max_core, gf16_dec_stat *st)
{
    int n = c->n, nchk = c->n_chk;
    int *col = NULL, *uv = NULL, *rows = NULL, *pivot = NULL;
    uint8_t *mat = NULL, *bin = NULL;
    int nu = 0, nr = 0;
    int ck, k, v, i;
    int rank = 0, g2 = 0;
    size_t stride;
    int rc = 0;

    if (max_core <= 0)
        return;
    col = malloc((size_t)n * sizeof(int));
    uv = malloc((size_t)n * sizeof(int));
    rows = malloc((size_t)nchk * sizeof(int));
    if (!col || !uv || !rows)
        goto done;
    for (v = 0; v < n; v++)
        col[v] = -1;

    for (ck = 0; ck < nchk; ck++) {
        if ((c->cx[ck] >> 8) == 0)
            continue;
        rows[nr++] = ck;
        for (k = 0; k < c->cdeg[ck]; k++) {
            v = c->cadj[(size_t)ck * c->dc_max + k];
            if (c->val[v] == 0xFF && col[v] < 0) {
                col[v] = nu;
                uv[nu++] = v;
            }
        }
    }
    if (nu == 0)
        goto done;

    st->core_calls++;
    if (nu > st->core_max_vars)
        st->core_max_vars = nu;
    if (nu > max_core || nr <= 0) {
        st->core_skipped++;
        goto done;
    }
    if (nr < nu) {
        /* Fewer equations than unknowns: rank cannot be full. */
        st->core_failed++;
        st->core_under++;
        goto done;
    }
    /*
     * Dense elimination is O(nr*nu^2).  A stopping set normally has
     * nr ~ nu*dc/2; the cap only guards a pathological core.
     */
    if ((long long)nr * (long long)(nu + 1) > 64LL * 1024 * 1024) {
        st->core_skipped++;
        goto done;
    }

    stride = (size_t)nu + 1;
    mat = calloc((size_t)nr * stride, 1);
    bin = calloc((size_t)nr * (size_t)nu, 1);
    pivot = malloc((size_t)nu * sizeof(int));
    if (!mat || !bin || !pivot)
        goto done;

    for (i = 0; i < nr; i++) {
        uint8_t *row = mat + (size_t)i * stride;
        uint8_t *brow = bin + (size_t)i * (size_t)nu;
        ck = rows[i];
        for (k = 0; k < c->cdeg[ck]; k++) {
            v = c->cadj[(size_t)ck * c->dc_max + k];
            if (c->val[v] == 0xFF) {
                int cc = col[v];
                row[cc] ^= c->ccoef[(size_t)ck * c->dc_max + k];
                brow[cc] ^= 1; /* GF(2) incidence, parallel edges cancel */
            }
        }
        row[nu] = (uint8_t)(c->cx[ck] & 15u);
    }

    /* GF(16) reduced row echelon form. */
    for (i = 0; i < nu && rank < nr; i++) {
        int piv = -1;
        for (k = rank; k < nr; k++)
            if (mat[(size_t)k * stride + i]) {
                piv = k;
                break;
            }
        if (piv < 0)
            continue;
        if (piv != rank) {
            uint8_t *a = mat + (size_t)rank * stride;
            uint8_t *b = mat + (size_t)piv * stride;
            size_t j;
            for (j = 0; j < stride; j++) {
                uint8_t t = a[j];
                a[j] = b[j];
                b[j] = t;
            }
        }
        {
            uint8_t *prow = mat + (size_t)rank * stride;
            uint8_t inv = gf_inv_tab[prow[i]];
            size_t j;
            for (j = (size_t)i; j < stride; j++)
                prow[j] = gf_mul(prow[j], inv);
            for (k = 0; k < nr; k++) {
                uint8_t *rrow;
                uint8_t f;
                if (k == rank)
                    continue;
                rrow = mat + (size_t)k * stride;
                f = rrow[i];
                if (!f)
                    continue;
                for (j = (size_t)i; j < stride; j++)
                    rrow[j] ^= gf_mul(f, prow[j]);
            }
        }
        pivot[rank] = i;
        rank++;
    }

    /* GF(2) rank of the same incidence pattern, for the comparison. */
    for (i = 0; i < nu && g2 < nr; i++) {
        int piv = -1;
        for (k = g2; k < nr; k++)
            if (bin[(size_t)k * (size_t)nu + i]) {
                piv = k;
                break;
            }
        if (piv < 0)
            continue;
        if (piv != g2) {
            uint8_t *a = bin + (size_t)g2 * (size_t)nu;
            uint8_t *b = bin + (size_t)piv * (size_t)nu;
            size_t j;
            for (j = 0; j < (size_t)nu; j++) {
                uint8_t t = a[j];
                a[j] = b[j];
                b[j] = t;
            }
        }
        for (k = 0; k < nr; k++) {
            uint8_t *rrow;
            size_t j;
            if (k == g2)
                continue;
            rrow = bin + (size_t)k * (size_t)nu;
            if (!rrow[i])
                continue;
            for (j = (size_t)i; j < (size_t)nu; j++)
                rrow[j] ^= bin[(size_t)g2 * (size_t)nu + j];
        }
        g2++;
    }
    st->core_vars_sum += nu;
    st->core_gf16_rank_sum += rank;
    if (g2 < nu)
        st->core_gf2_deficient++;

    if (rank < nu) {
        st->core_failed++;
        st->core_rankdef++;
        goto done;
    }
    /* With rank == nu there are no free columns; rows below are all zero. */
    for (i = rank; i < nr; i++) {
        uint8_t *row = mat + (size_t)i * stride;
        int any = 0;
        for (k = 0; k < nu; k++)
            if (row[k]) {
                any = 1;
                break;
            }
        if (!any && row[nu]) {
            st->core_failed++;
            st->core_bad++;
            goto done;
        }
    }

    for (i = 0; i < nu; i++)
        c->val[uv[pivot[i]]] = mat[(size_t)i * stride + nu];

    for (v = 0; v < n; v++)
        out[v] = (c->val[v] == 0xFF) ? 0 : c->val[v];
    if (gf16_verify(c, out) != 0) {
        for (i = 0; i < nu; i++)
            c->val[uv[i]] = 0xFF;
        st->core_failed++;
        st->core_bad++;
        goto done;
    }
    {
        int tgt = target_data_only ? c->n_data : c->n;
        int unres = 0;
        for (v = 0; v < tgt; v++)
            if (c->val[v] == 0xFF)
                unres++;
        if (unres) {
            for (i = 0; i < nu; i++)
                c->val[uv[i]] = 0xFF;
            st->core_failed++;
            st->core_bad++;
        } else {
            st->core_solved++;
        }
    }

done:
    (void)rc;
    free(col);
    free(uv);
    free(rows);
    free(mat);
    free(bin);
    free(pivot);
}

gf16_dec_stat gf16_decode(gf16_code *c, const uint8_t *recv,
                          const uint8_t *erased, uint8_t *out,
                          int target_data_only, int max_core)
{
    gf16_dec_stat st;
    int n = c->n, nchk = c->n_chk;
    uint16_t *cx = c->cx;
    uint8_t *val = c->val;
    uint16_t *queue = c->queue;
    int qh = 0, qt = 0;
    int v, ck, k;
    long visits = 0;

    memset(&st, 0, sizeof(st));
    memset(cx, 0, (size_t)nchk * sizeof(uint16_t));
    for (v = 0; v < n; v++) {
        int d = c->vdeg[v];
        if (erased[v]) {
            val[v] = 0xFF;
            for (k = 0; k < d; k++) {
                int c2 = c->vadj[(size_t)v * c->dv_max + k];
                cx[c2] = (uint16_t)(cx[c2] + 0x100u);
            }
        } else {
            uint8_t x = (uint8_t)(recv[v] & 15u);
            val[v] = x;
            for (k = 0; k < d; k++) {
                int c2 = c->vadj[(size_t)v * c->dv_max + k];
                uint8_t coef = c->vcoef[(size_t)v * c->dv_max + k];
                cx[c2] ^= gf_mul(coef, x);
            }
        }
    }
    for (ck = 0; ck < nchk; ck++) {
        visits += c->cdeg[ck];
        if ((cx[ck] >> 8) == 1)
            queue[qt++] = (uint16_t)ck;
    }

    st.rounds = 0;
    while (qh < qt) {
        int end = qt;
        st.rounds++;
        while (qh < end) {
            int uv = -1;
            uint8_t h = 0, x;
            int d;

            ck = queue[qh++];
            if ((cx[ck] >> 8) != 1)
                continue;
            d = c->cdeg[ck];
            for (k = 0; k < d; k++) {
                int vv = c->cadj[(size_t)ck * c->dc_max + k];
                visits++;
                if (val[vv] == 0xFF) {
                    uv = vv;
                    h = c->ccoef[(size_t)ck * c->dc_max + k];
                    break;
                }
            }
            if (uv < 0) {
                cx[ck] = 0;
                continue;
            }
            x = gf_mul((uint8_t)(cx[ck] & 15u), gf_inv_tab[h]);
            val[uv] = x;
            cx[ck] = 0;
            d = c->vdeg[uv];
            for (k = 0; k < d; k++) {
                int c2 = c->vadj[(size_t)uv * c->dv_max + k];
                uint8_t coef = c->vcoef[(size_t)uv * c->dv_max + k];
                visits++;
                if (c2 == ck)
                    continue;
                cx[c2] = (uint16_t)((cx[c2] - 0x100u) ^ gf_mul(coef, x));
                if ((cx[c2] >> 8) == 1)
                    queue[qt++] = (uint16_t)c2;
            }
        }
    }

    if (max_core > 0) {
        int any = 0;
        for (v = 0; v < n; v++)
            if (val[v] == 0xFF) {
                any = 1;
                break;
            }
        if (any)
            gf16_core_solve(c, out, target_data_only, max_core, &st);
    }

    {
        long tgt = target_data_only ? c->n_data : c->n;
        long unres = 0;
        for (v = 0; v < tgt; v++)
            if (val[v] == 0xFF)
                unres++;
        st.unresolved = unres;
        st.target = tgt;
        st.resolved = tgt - unres;
    }
    st.edge_visits = visits;

    for (v = 0; v < n; v++) {
        c->known[v] = (val[v] != 0xFF);
        out[v] = (val[v] == 0xFF) ? 0 : val[v];
    }
    return st;
}

const uint8_t *gf16_known(const gf16_code *c)
{
    return c->known;
}
