/*
 * sc_enc128_test.c - bit-exact acceptance test for the SSE2 batch kernel.
 *
 * For each configuration the test builds the SC_TERM_ACC code and its plan,
 * fills two independent 64-word payload lanes with pseudo-random bits, packs
 * them into n 16-byte-aligned 128-bit words (lane 0 low, lane 1 high), runs
 * the scalar kernel on a copy of each lane, runs sc_enc128_run_sse2(), and
 * compares every parity word of both lanes against the scalar result.
 *
 * A lane count of 64/64 means every codeword of that lane matched in all
 * parity words.  Any mismatch exits nonzero.
 */

#define _ISOC11_SOURCE 1

#include "sc_enc64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int block;
    int L;
    int rate_pct;
    uint64_t seed;
} cfg;

static const cfg configs[] = {
    {256, 16, 50, 0x5c1d0001ULL},
    {256, 16, 40, 0x5c1d0002ULL},
    {256, 16, 30, 0x5c1d0003ULL},
    {512, 16, 50, 0x5c1d0004ULL},
};
#define NCONFIG ((int)(sizeof(configs) / sizeof(configs[0])))

/* splitmix64: one word per call, no shared state. */
static uint64_t rng_next(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static int popcount64(uint64_t x)
{
    int n = 0;

    while (x) {
        x &= x - 1;
        n++;
    }
    return n;
}

static int run_config(const cfg *g)
{
    sc_params sp;
    sc_code *c = NULL;
    sc_enc64 *e = NULL;
    sc_info in;
    char err[160];
    uint64_t *lane0 = NULL, *lane1 = NULL;
    uint64_t *ref0 = NULL, *ref1 = NULL;
    uint64_t *w = NULL;
    uint64_t rng0, rng1, diff0 = 0, diff1 = 0;
    int k, n, n_data, n_par, bad0 = 0, bad1 = 0, fail = 0, rc;

    memset(&sp, 0, sizeof(sp));
    sp.block = g->block;
    sp.L = g->L;
    sp.rate_pct = g->rate_pct;
    sp.term = SC_TERM_ACC;
    sp.extra_dc = 5;
    sp.seed = g->seed;

    c = sc_build(&sp, err, sizeof(err));
    if (!c) {
        printf("block=%d L=%d rate=%.2f FAIL build: %s\n", g->block, g->L,
               g->rate_pct / 100.0, err);
        return 1;
    }
    e = sc_enc64_build(c, err, sizeof(err));
    if (!e) {
        printf("block=%d L=%d rate=%.2f FAIL plan: %s\n", g->block, g->L,
               g->rate_pct / 100.0, err);
        sc_free(c);
        return 1;
    }
    sc_get_info(c, &in);
    n = in.n;
    n_data = in.n_data;
    n_par = in.n_par;

    lane0 = malloc((size_t)n * sizeof(uint64_t));
    lane1 = malloc((size_t)n * sizeof(uint64_t));
    ref0 = malloc((size_t)n * sizeof(uint64_t));
    ref1 = malloc((size_t)n * sizeof(uint64_t));
    w = aligned_alloc(16, (size_t)n * 16);
    if (!lane0 || !lane1 || !ref0 || !ref1 || !w) {
        printf("block=%d L=%d rate=%.2f FAIL out of memory\n", g->block, g->L,
               g->rate_pct / 100.0);
        fail = 1;
        goto out;
    }

    /* Two independent pseudo-random payload lanes. */
    rng0 = g->seed ^ 0xd1b54a32d192ed03ULL;
    rng1 = g->seed ^ 0x94d049bb133111ebULL;
    for (k = 0; k < n_data; k++) {
        lane0[k] = rng_next(&rng0);
        lane1[k] = rng_next(&rng1);
    }
    for (k = n_data; k < n; k++) {
        lane0[k] = 0;
        lane1[k] = 0;
    }

    /* Scalar reference: one 64-word batch per lane. */
    memcpy(ref0, lane0, (size_t)n * sizeof(uint64_t));
    memcpy(ref1, lane1, (size_t)n * sizeof(uint64_t));
    sc_enc64_run(e, ref0);
    sc_enc64_run(e, ref1);

    /* Interleave the lanes into the 128-bit layout. */
    for (k = 0; k < n; k++) {
        w[2 * k] = lane0[k];
        w[2 * k + 1] = lane1[k];
    }

    rc = sc_enc128_run_sse2(e, w);
    if (rc != 0) {
        printf("block=%d L=%d rate=%.2f FAIL sse2 unavailable\n", g->block,
               g->L, g->rate_pct / 100.0);
        fail = 1;
        goto out;
    }

    for (k = n_data; k < n; k++) {
        uint64_t d0 = w[2 * k] ^ ref0[k];
        uint64_t d1 = w[2 * k + 1] ^ ref1[k];

        if (d0) {
            bad0++;
            diff0 |= d0;
        }
        if (d1) {
            bad1++;
            diff1 |= d1;
        }
    }

    printf("block=%d L=%d rate=%.2f lane0=%d/%d lane1=%d/%d parity_words=%d\n",
           g->block, g->L, g->rate_pct / 100.0,
           SC_ENC64_BATCH - popcount64(diff0), SC_ENC64_BATCH,
           SC_ENC64_BATCH - popcount64(diff1), SC_ENC64_BATCH, n_par);
    if (bad0 || bad1)
        fail = 1;

out:
    free(lane0);
    free(lane1);
    free(ref0);
    free(ref1);
    free(w);
    sc_enc64_free(e);
    sc_free(c);
    return fail;
}

int main(void)
{
    int i, fails = 0;

    printf("sc_enc128_test: configs=%d batch=%d\n", NCONFIG, SC_ENC64_BATCH);
    for (i = 0; i < NCONFIG; i++)
        fails += run_config(&configs[i]);

    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
