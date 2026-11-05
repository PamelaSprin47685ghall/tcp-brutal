#include "rlnc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state = 0x243f6a8885a308d3ull;

static uint32_t rnd32(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

static double rnd_unit(void)
{
    return (double)(rnd32() % 1000000u) / 1000000.0;
}

struct point
{
    unsigned n;
    unsigned m;
    unsigned w;
    unsigned d;
    unsigned k;
    double p;
    unsigned trials;
    double recovery;
    double overhead;
    double rank_fail;
    double latency;
    size_t mem;
};

static struct point run_point(unsigned n, unsigned m, unsigned w, unsigned d,
                              unsigned k, double p, unsigned trials)
{
    struct point out;
    unsigned L = w;
    unsigned G = L / k + (L % k != 0);
    unsigned P = G * m;
    uint64_t nonce = 0x9e3779b97f4a7c15ull ^ ((uint64_t)n << 32) ^
                     ((uint64_t)m << 16) ^ ((uint64_t)w << 8) ^
                     ((uint64_t)d << 4) ^ k;
    struct rlnc_ctx *ctx = rlnc_new(n, m, k, w, 1, d, nonce);
    uint8_t *dbuf;
    uint8_t *pbuf;
    uint8_t *orig;
    uint8_t **data;
    uint8_t **parity;
    uint8_t *dp;
    uint8_t *pp;
    unsigned i;
    unsigned recovered = 0;
    double overhead_sum = 0.0;
    double latency_sum = 0.0;
    unsigned latency_n = 0;

    memset(&out, 0, sizeof(out));
    out.n = n;
    out.m = m;
    out.w = w;
    out.d = d;
    out.k = k;
    out.p = p;
    out.trials = trials;

    if (!ctx)
    {
        out.trials = 0;
        return out;
    }

    dbuf = malloc(L);
    pbuf = malloc(P);
    orig = malloc(L);
    data = malloc((size_t)L * sizeof(uint8_t *));
    parity = malloc((size_t)P * sizeof(uint8_t *));
    dp = malloc(L);
    pp = malloc(P);
    if (!dbuf || !pbuf || !orig || !data || !parity || !dp || !pp)
    {
        exit(2);
    }

    for (i = 0; i < L; i++)
    {
        data[i] = dbuf + i;
    }
    for (i = 0; i < P; i++)
    {
        parity[i] = pbuf + i;
    }

    for (i = 0; i < trials; i++)
    {
        unsigned t;
        unsigned missing = 0;
        unsigned g;
        unsigned worst = 0;
        int rc;

        for (t = 0; t < L; t++)
        {
            dbuf[t] = (uint8_t)rnd32();
        }
        memcpy(orig, dbuf, L);
        rlnc_encode(ctx, data, L, parity);

        for (t = 0; t < L; t++)
        {
            dp[t] = rnd_unit() >= p;
            if (!dp[t])
            {
                missing++;
            }
        }
        for (t = 0; t < P; t++)
        {
            pp[t] = 1;
        }

        rc = rlnc_decode(ctx, data, parity, dp, pp, L);
        if (rc >= 0 && memcmp(dbuf, orig, L) == 0)
        {
            recovered++;
        }

        for (t = 0; t < L; t++)
        {
            unsigned latency;

            if (dp[t])
            {
                continue;
            }

            for (g = 0; g < G; g++)
            {
                int we = (int)(g * k + k - 1);
                int ws = (int)(g * k) + (int)k - (int)w;

                if (we >= (int)t && ws <= (int)t)
                {
                    if (we >= (int)L)
                    {
                        we = (int)L - 1;
                    }
                    latency = (unsigned)(we - (int)t);
                    if (latency > worst)
                    {
                        worst = latency;
                    }
                    break;
                }
            }
        }

        if (missing > 0)
        {
            overhead_sum += (double)P / (double)missing;
            latency_sum += worst;
            latency_n++;
        }
    }

    out.recovery = (double)recovered / trials;
    out.overhead = latency_n ? overhead_sum / latency_n : 0.0;
    out.rank_fail = 1.0 - out.recovery;
    out.latency = latency_n ? latency_sum / latency_n : 0.0;
    out.mem = rlnc_memory_bytes(ctx, L);

    free(dbuf);
    free(pbuf);
    free(orig);
    free(data);
    free(parity);
    free(dp);
    free(pp);
    rlnc_free(ctx);

    return out;
}

int main(void)
{
    static const unsigned nm[][2] = {{96, 32}, {16, 4}, {8, 2}};
    static const unsigned ws[] = {64, 128, 256};
    static const unsigned ds[] = {8, 16, 32};
    static const double ps[] = {0.01, 0.05, 0.10, 0.20, 0.25, 0.30};
    static struct point res[3][3][3][2][6];
    unsigned ni;
    unsigned wi;
    unsigned di;
    unsigned ki;
    unsigned pi;

    printf("n,m,w,d,k,p,trials,recovery,overhead,rank_fail,latency_pkts,"
           "mem_bytes\n");

    for (ni = 0; ni < 3; ni++)
    {
        for (wi = 0; wi < 3; wi++)
        {
            for (di = 0; di < 3; di++)
            {
                for (ki = 0; ki < 2; ki++)
                {
                    unsigned n = nm[ni][0];
                    unsigned m = nm[ni][1];
                    unsigned w = ws[wi];
                    unsigned d = ds[di];
                    unsigned k = ki == 0 ? n : 2 * n;

                    if (k > w)
                    {
                        for (pi = 0; pi < 6; pi++)
                        {
                            memset(&res[ni][wi][di][ki][pi], 0,
                                   sizeof(struct point));
                            res[ni][wi][di][ki][pi].n = n;
                            res[ni][wi][di][ki][pi].m = m;
                            res[ni][wi][di][ki][pi].w = w;
                            res[ni][wi][di][ki][pi].d = d;
                            res[ni][wi][di][ki][pi].k = k;
                            res[ni][wi][di][ki][pi].p = ps[pi];
                        }
                        printf("%u,%u,%u,%u,%u,NA,rejected (k>w),NA,NA,NA,"
                               "NA,NA\n",
                               n, m, w, d, k);
                        continue;
                    }

                    for (pi = 0; pi < 6; pi++)
                    {
                        struct point pt;

                        res[ni][wi][di][ki][pi] =
                            run_point(n, m, w, d, k, ps[pi], 2000);
                        pt = res[ni][wi][di][ki][pi];
                        printf("%u,%u,%u,%u,%u,%.2f,%u,%.4f,%.3f,%.4f,"
                               "%.2f,%zu\n",
                               pt.n, pt.m, pt.w, pt.d, pt.k, pt.p,
                               pt.trials, pt.recovery, pt.overhead,
                               pt.rank_fail, pt.latency, pt.mem);
                    }
                }
            }
        }
    }

    printf("\nrecovery curve (rows: n,m,w,d,k; columns: p)\n");
    printf("n,m,w,d,k");
    for (pi = 0; pi < 6; pi++)
    {
        printf(",p=%.2f", ps[pi]);
    }
    printf("\n");

    for (ni = 0; ni < 3; ni++)
    {
        for (wi = 0; wi < 3; wi++)
        {
            for (di = 0; di < 3; di++)
            {
                for (ki = 0; ki < 2; ki++)
                {
                    struct point *pt = &res[ni][wi][di][ki][0];

                    printf("%u,%u,%u,%u,%u", pt->n, pt->m, pt->w, pt->d,
                           pt->k);
                    if (pt->trials == 0)
                    {
                        for (pi = 0; pi < 6; pi++)
                        {
                            printf(",NA");
                        }
                        printf("\n");
                        continue;
                    }
                    for (pi = 0; pi < 6; pi++)
                    {
                        printf(",%.3f", res[ni][wi][di][ki][pi].recovery);
                    }
                    printf("\n");
                }
            }
        }
    }

    return 0;
}
