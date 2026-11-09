/* bench_sclc_recovery.c - paired BEC recovery comparison for the uplink
 * GF(16) SC-IRA codec.
 *
 *   ./host/bench_sclc_recovery [trials]
 *
 * Every trial uses one random block and one random share-loss mask, then
 * decodes it twice: once with the legacy symbol-interleaved mapping and
 * once with the Wire v8 64-lane bit-plane mapping.  Because the mask is
 * shared, the columns are paired and the block delta is not a comparison of
 * two independent binomial samples.
 *
 * CSV columns:
 *   n,m,p,mapping,codewords_ok,codewords,block_ok,blocks,share_recovered,
 *   shares_missing
 * mapping 0 is legacy interleaved, 3 is bit-plane 64.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tbt_sclc_uplink.h"

static uint64_t rng_s0 = 1, rng_s1 = 2;

static void rng_seed(uint64_t seed)
{
    uint64_t x = seed;

    rng_s0 = x + 0x9E3779B97F4A7C15ULL;
    x = rng_s0;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    rng_s1 = x ^ (x >> 31);
    if (!rng_s0 && !rng_s1)
        rng_s0 = 1;
}

static uint64_t rng_next(void)
{
    uint64_t x = rng_s0, y = rng_s1;

    rng_s0 = y;
    x ^= x << 23;
    rng_s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
    return rng_s1 + y;
}

static int rng_lost(double p)
{
    return (rng_next() >> 32) % 1000000u < (uint64_t)(p * 1000000.0);
}

struct run
{
    long code_ok, code_all, block_ok, blocks;
    long shares_missing, shares_recovered;
};

static int alloc_block(unsigned n, unsigned m, unsigned char *data[256],
                       unsigned char *parity[256], unsigned char *orig[256])
{
    unsigned i;

    for (i = 0; i < n; i++)
    {
        data[i] = malloc(TBT_FEC_S);
        orig[i] = malloc(TBT_FEC_S);
        if (!data[i] || !orig[i])
            return -1;
    }
    for (i = 0; i < m; i++)
    {
        parity[i] = malloc(TBT_FEC_S);
        if (!parity[i])
            return -1;
    }
    return 0;
}

static void free_block(unsigned n, unsigned m, unsigned char *data[256],
                       unsigned char *parity[256], unsigned char *orig[256])
{
    unsigned i;

    for (i = 0; i < n; i++)
    {
        free(data[i]);
        free(orig[i]);
    }
    for (i = 0; i < m; i++)
        free(parity[i]);
}

static void run_one(struct tbt_sclc_uplink *u, unsigned n, unsigned m,
                    double p, int trials, struct run *out)
{
    unsigned char *data[256], *parity[256], *orig[256];
    unsigned char pd[256], pp[256];
    unsigned i;
    int t;

    memset(out, 0, sizeof(*out));
    if (alloc_block(n, m, data, parity, orig) != 0)
    {
        free_block(n, m, data, parity, orig);
        return;
    }
    for (t = 0; t < trials; t++)
    {
        unsigned j;

        for (i = 0; i < n; i++)
        {
            for (j = 0; j < TBT_FEC_S; j++)
                data[i][j] = (unsigned char)rng_next();
            memcpy(orig[i], data[i], TBT_FEC_S);
        }
        for (i = 0; i < n; i++)
            pd[i] = (unsigned char)!rng_lost(p);
        for (i = 0; i < m; i++)
            pp[i] = (unsigned char)!rng_lost(p);
        if (tbt_sclc_uplink_encode(u, 0, data, parity) != 0)
            break;
        for (i = 0; i < n; i++)
            if (!pd[i])
                memset(data[i], 0, TBT_FEC_S);
        for (i = 0; i < m; i++)
            if (!pp[i])
                memset(parity[i], 0, TBT_FEC_S);
        (void)tbt_sclc_uplink_decode(u, 0, data, parity, pd, pp);
        {
            struct tbt_sclc_stat st;

            tbt_sclc_uplink_stat(u, 0, &st);
            out->code_ok += st.codes_ok;
            out->code_all += st.codes;
            out->block_ok += st.codes_failed == 0;
            out->blocks++;
            out->shares_missing += st.shares_missing;
            out->shares_recovered += st.shares_recovered;
        }
        /* Every share the codec calls known must hold the original bytes. */
        for (i = 0; i < n; i++)
            if (tbt_sclc_uplink_share_known(u, 0, (u16)i) &&
                memcmp(data[i], orig[i], TBT_FEC_S) != 0)
            {
                fprintf(stderr, "byte mismatch n=%u p=%.2f share=%u\n", n, p,
                        i);
                out->blocks = -1;
                goto out;
            }
    }
out:
    free_block(n, m, data, parity, orig);
}

int main(int argc, char **argv)
{
    static const unsigned n[6] = {8, 9, 12, 16, 18, 22};
    static const unsigned m[6] = {24, 23, 20, 16, 14, 10};
    static const double plist[8] = {0.10, 0.15, 0.20, 0.25,
                                    0.30, 0.35, 0.40, 0.45};
    int trials = argc > 1 ? atoi(argv[1]) : 300;
    int ri, pi, mi;

    printf("n,m,p,mapping,codewords_ok,codewords,block_ok,blocks,"
           "shares_recovered,shares_missing\n");
    for (ri = 0; ri < 6; ri++)
    {
        struct tbt_sclc_params par;
        struct tbt_sclc_uplink *u[2];
        unsigned d = n[ri] * 256u / 32u;

        memset(&par, 0, sizeof(par));
        par.n = (u16)n[ri];
        par.m = (u16)m[ri];
        par.block = 128;
        par.L = 16;
        par.rate_num = (u16)d;
        par.rate_den = 256;
        par.max_core = 256;
        par.seed = 1;
        for (mi = 0; mi < 2; mi++)
        {
            par.mapping = mi ? TBT_SCLC_MAP_BITPLANE64
                             : TBT_SCLC_MAP_INTERLEAVED;
            u[mi] = tbt_sclc_uplink_new(1, &par);
            if (!u[mi])
            {
                fprintf(stderr, "build failed n=%u m=%u map=%d\n", n[ri],
                        m[ri], par.mapping);
                return 1;
            }
        }
        for (pi = 0; pi < 8; pi++)
        {
            struct run r[2];

            rng_seed(0x9E3779B97F4A7C15ULL * (u64)(ri + 1) +
                     (u64)(pi + 1) * 0xD1B54A32D192ED03ULL);
            run_one(u[0], n[ri], m[ri], plist[pi], trials, &r[0]);
            rng_seed(0x9E3779B97F4A7C15ULL * (u64)(ri + 1) +
                     (u64)(pi + 1) * 0xD1B54A32D192ED03ULL);
            run_one(u[1], n[ri], m[ri], plist[pi], trials, &r[1]);
            for (mi = 0; mi < 2; mi++)
                printf("%u,%u,%.2f,%d,%ld,%ld,%ld,%ld,%ld,%ld\n", n[ri],
                       m[ri], plist[pi],
                       mi ? TBT_SCLC_MAP_BITPLANE64
                          : TBT_SCLC_MAP_INTERLEAVED,
                       r[mi].code_ok, r[mi].code_all, r[mi].block_ok,
                       r[mi].blocks, r[mi].shares_recovered,
                       r[mi].shares_missing);
        }
        for (mi = 0; mi < 2; mi++)
            tbt_sclc_uplink_free(u[mi]);
    }
    return 0;
}
