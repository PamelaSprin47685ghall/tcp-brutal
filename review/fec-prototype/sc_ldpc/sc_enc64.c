/*
 * sc_enc64.c - compile an SC-IRA check plan into bit-sliced form and run the
 * 64-codeword scalar kernel.  See sc_enc64.h for the layout.
 *
 * The plan is the same fixed-stride, uint16 check adjacency as the fast
 * decoder layout (review/fec-prototype/sc_ldpc/sc_ldpc.c, commit 345682d):
 * cadj[ck*dcs + k] holds the full adjacency of check ck, with the parity
 * variable the check owns moved to the last slot of the row.  The XOR list
 * is therefore [0, cdeg-1) and the kernel needs no per-edge skip.  Variable
 * and check order are unchanged; a row is a permutation of the decoder's
 * cadj_f row, so the decoder can use the array unchanged.
 *
 * The encoder is the SC-IRA forward substitution: parity word a is solved
 * before any check of block a+1 reads it.
 */

#include "sc_enc64_internal.h"

#include <stdio.h>
#include <stdlib.h>

static int plan_fail(sc_enc64 *e, char *err, size_t errlen, const char *why)
{
    sc_enc64_free(e);
    snprintf(err, errlen, "%s", why);
    return -1;
}

sc_enc64 *sc_enc64_build(sc_code *c, char *err, size_t errlen)
{
    sc_graph g;
    sc_enc64 *e;
    int ck, k, d, dcs = 0;
    long xors = 0;

    if (!c) {
        snprintf(err, errlen, "null code");
        return NULL;
    }
    if (!sc_systematic(c)) {
        snprintf(err, errlen, "code is not systematically encodable");
        return NULL;
    }
    sc_get_graph(c, &g);
    if (g.n_chk <= 0 || g.n_data <= 0 || g.n_par != g.n_chk) {
        snprintf(err, errlen, "inconsistent code dimensions");
        return NULL;
    }
    for (ck = 0; ck < g.n_chk; ck++) {
        d = g.coff[ck + 1] - g.coff[ck];
        if (d < 1) {
            snprintf(err, errlen, "check has no adjacency");
            return NULL;
        }
        if (d > dcs)
            dcs = d;
        xors += d - 1;
    }
    if (g.n >= 65536) {
        snprintf(err, errlen, "code has more than 65535 variables");
        return NULL;
    }
    if (dcs > 255) {
        snprintf(err, errlen, "check degree above 255");
        return NULL;
    }

    e = calloc(1, sizeof(*e));
    if (!e) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    e->n = g.n;
    e->n_data = g.n_data;
    e->n_par = g.n_par;
    e->n_chk = g.n_chk;
    e->dcs = dcs;
    e->xors = xors;
    e->cadj = calloc((size_t)e->n_chk * (size_t)dcs, sizeof(uint16_t));
    e->cdeg = calloc((size_t)e->n_chk, 1);
    e->owned = calloc((size_t)e->n_chk, sizeof(int));
    e->pair = calloc((size_t)e->n_chk, 1);
    if (!e->cadj || !e->cdeg || !e->owned || !e->pair) {
        plan_fail(e, err, errlen, "out of memory");
        return NULL;
    }

    for (ck = 0; ck < e->n_chk; ck++) {
        const int *adj = g.cadj + g.coff[ck];
        int owned = g.owned[ck];
        int seen = 0, pos = 0;
        d = g.coff[ck + 1] - g.coff[ck];
        e->cdeg[ck] = (uint8_t)d;
        e->owned[ck] = owned;
        if (owned < e->n_data || owned >= e->n) {
            plan_fail(e, err, errlen, "owned parity out of range");
            return NULL;
        }
        for (k = 0; k < d; k++) {
            int v = adj[k];
            if (v < 0 || v >= e->n) {
                plan_fail(e, err, errlen, "variable index out of range");
                return NULL;
            }
            if (v == owned) {
                seen++;
                continue;
            }
            e->cadj[(size_t)ck * (size_t)dcs + pos++] = (uint16_t)v;
        }
        if (seen != 1 || pos != d - 1) {
            plan_fail(e, err, errlen,
                      "owned parity is not adjacent exactly once");
            return NULL;
        }
        /* Keep the owned parity in the row, after the XOR list. */
        e->cadj[(size_t)ck * (size_t)dcs + d - 1] = (uint16_t)owned;
    }

    /*
     * The plan is only a forward sweep if every parity variable is owned by
     * exactly one check and every read of a parity word comes from a check
     * that was solved earlier.  Both hold for SC_TERM_ACC by construction;
     * checking them here turns a malformed graph into a build error instead
     * of silently wrong parity.
     */
    {
        int *owner = malloc((size_t)e->n_par * sizeof(int));
        if (!owner) {
            plan_fail(e, err, errlen, "out of memory");
            return NULL;
        }
        for (ck = 0; ck < e->n_par; ck++)
            owner[ck] = -1;
        for (ck = 0; ck < e->n_chk; ck++) {
            int p = e->owned[ck];
            if (owner[p - e->n_data] != -1) {
                free(owner);
                plan_fail(e, err, errlen,
                          "parity variable not owned exactly once");
                return NULL;
            }
            owner[p - e->n_data] = ck;
        }
        for (ck = 0; ck < e->n_chk; ck++) {
            const uint16_t *a = e->cadj + (size_t)ck * (size_t)dcs;
            d = e->cdeg[ck] - 1;
            for (k = 0; k < d; k++) {
                int v = (int)a[k];
                if (v >= e->n_data && owner[v - e->n_data] >= ck) {
                    free(owner);
                    plan_fail(e, err, errlen,
                              "parity read before it is solved");
                    return NULL;
                }
            }
        }
        free(owner);
    }

    /*
     * Pair a check with its successor when neither reads the other's parity
     * word.  The two accumulators then stay independent in the kernel, which
     * hides the XOR latency without changing the sweep order.
     */
    for (ck = 0; ck + 1 < e->n_chk; ck++) {
        const uint16_t *a0 = e->cadj + (size_t)ck * (size_t)dcs;
        const uint16_t *a1 = e->cadj + (size_t)(ck + 1) * (size_t)dcs;
        int n0 = e->cdeg[ck] - 1;
        int n1 = e->cdeg[ck + 1] - 1;
        int p0 = e->owned[ck], p1 = e->owned[ck + 1];
        int ok = 1;
        for (k = 0; k < n1 && ok; k++)
            if ((int)a1[k] == p0)
                ok = 0;
        for (k = 0; k < n0 && ok; k++)
            if ((int)a0[k] == p1)
                ok = 0;
        if (ok) {
            e->pair[ck] = 1;
            ck++;
        }
    }
    return e;
}

void sc_enc64_free(sc_enc64 *e)
{
    if (!e)
        return;
    free(e->cadj);
    free(e->cdeg);
    free(e->owned);
    free(e->pair);
    free(e);
}

int sc_enc64_n(const sc_enc64 *e)
{
    return e->n;
}

int sc_enc64_n_data(const sc_enc64 *e)
{
    return e->n_data;
}

int sc_enc64_n_par(const sc_enc64 *e)
{
    return e->n_par;
}

int sc_enc64_n_chk(const sc_enc64 *e)
{
    return e->n_chk;
}

long sc_enc64_xors(const sc_enc64 *e)
{
    return e->xors;
}

/*
 * XOR the check's XOR list.  cadj holds the full adjacency with the owned
 * parity moved to the last slot of the row, so the list is [0, cdeg-1).
 */
static inline uint64_t check_xor(const sc_enc64 *e, int ck,
                                 const uint64_t *restrict v)
{
    const uint16_t *restrict a =
        e->cadj + (size_t)ck * (size_t)e->dcs;
    int d = e->cdeg[ck] - 1;
    uint64_t x = 0;
    int k;

    for (k = 0; k < d; k++)
        x ^= v[a[k]];
    return x;
}

void sc_enc64_run(const sc_enc64 *e, uint64_t *restrict v)
{
    int ck = 0;
    int nchk = e->n_chk;

    while (ck < nchk) {
        if (e->pair[ck]) {
            int p0 = e->owned[ck];
            int p1 = e->owned[ck + 1];
            uint64_t x0, x1;
            x0 = check_xor(e, ck, v);
            x1 = check_xor(e, ck + 1, v);
            v[p0] = x0;
            v[p1] = x1;
            ck += 2;
        } else {
            v[e->owned[ck]] = check_xor(e, ck, v);
            ck++;
        }
    }
}
