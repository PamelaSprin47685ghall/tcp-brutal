/* bench_sclc_uplink.c - host throughput bench for the uplink grouped
 * GF(16) SC-IRA codec.
 *
 *   build/bin/bench_sclc_uplink [groups] [n] [m] [rate_num] [rate_den] [p]
 *                            [mapping]
 *
 * Payload MB/s: one group block carries n*S bytes of application data.
 * Encode is measured over fresh blocks; decode is measured over a block
 * whose missing shares are re-zeroed before every call, so the work is the
 * same on every iteration.  The p=0 line is the raw decoder cost.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tbt_sclc_uplink.h"

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t rng_state = 1;

static uint64_t rng_next(void)
{
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

int main(int argc, char **argv)
{
    u16 groups = argc > 1 ? (u16)atoi(argv[1]) : 8;
    u16 n = argc > 2 ? (u16)atoi(argv[2]) : 14;
    u16 m = argc > 3 ? (u16)atoi(argv[3]) : 18;
    u16 rn = argc > 4 ? (u16)atoi(argv[4]) : 7;
    u16 rd = argc > 5 ? (u16)atoi(argv[5]) : 16;
    double p = argc > 6 ? atof(argv[6]) : 0.0;
    u8 mapping = argc > 7 ? (u8)atoi(argv[7]) : TBT_SCLC_MAP_BITPLANE64;
    struct tbt_sclc_params par;
    struct tbt_sclc_uplink *u;
    u8 *data[256], *parity[256];
    u8 pd[256], pp[256];
    u16 i, g;
    double t0, enc_s, dec_s;
    long enc_blocks = 0, dec_blocks = 0;
    double enc_mbps, dec_mbps;

    memset(&par, 0, sizeof(par));
    par.n = n;
    par.m = m;
    par.block = 128;
    par.L = 16;
    par.rate_num = rn;
    par.rate_den = rd;
    par.max_core = 256;
    par.mapping = mapping;
    par.seed = 1;
    u = tbt_sclc_uplink_new(groups, &par);
    if (!u)
    {
        fprintf(stderr, "codec build failed for %ux(%u+%u)\n", groups, n, m);
        return 1;
    }
    for (i = 0; i < n; i++)
        data[i] = malloc(TBT_FEC_S);
    for (i = 0; i < m; i++)
        parity[i] = malloc(TBT_FEC_S);

    printf("shape %u groups x (%u data + %u parity), block %u, L %u, "
           "rate %u/%u, codes %u, npar %u, mapping %u, mem %u KiB\n",
           groups, n, m, par.block, par.L, rn, rd,
           tbt_sclc_uplink_codes(u), tbt_sclc_uplink_n_par(u), mapping,
           tbt_sclc_uplink_mem(u) / 1024);

    /* Encode: fill fresh payload, encode every group in turn. */
    t0 = now_sec();
    do
    {
        for (g = 0; g < groups; g++)
        {
            for (i = 0; i < n; i++)
            {
                u32 j;

                for (j = 0; j < TBT_FEC_S; j++)
                    data[i][j] = (u8)rng_next();
            }
            if (tbt_sclc_uplink_encode(u, g, data, parity) != 0)
                return 1;
        }
        enc_blocks++;
    } while (now_sec() - t0 < 0.5);
    enc_s = now_sec() - t0;
    enc_mbps = (double)enc_blocks * groups * n * TBT_FEC_S / 1e6 / enc_s;

    /* One encoded block per group, then decode the same blocks repeatedly.
     * Missing shares are zeroed before each call so every iteration does
     * the full erasure work. */
    for (i = 0; i < n; i++)
    {
        u32 j;

        for (j = 0; j < TBT_FEC_S; j++)
            data[i][j] = (u8)rng_next();
    }
    if (tbt_sclc_uplink_encode(u, 0, data, parity) != 0)
        return 1;
    for (i = 0; i < n; i++)
        pd[i] = (u8)(p == 0.0 || ((rng_next() >> 32) % 1000) >=
                                   (uint64_t)(p * 1000.0));
    for (i = 0; i < m; i++)
        pp[i] = (u8)(p == 0.0 || ((rng_next() >> 32) % 1000) >=
                                   (uint64_t)(p * 1000.0));
    if (p > 0.0)
    {
        u16 present = 0;

        for (i = 0; i < n; i++)
            present += pd[i] ? 1u : 0u;
        if (present == n)
            pd[0] = 0; /* measure the peel, not the all-data-present return */
    }

    t0 = now_sec();
    do
    {
        for (i = 0; i < n; i++)
            if (!pd[i])
                memset(data[i], 0, TBT_FEC_S);
        for (i = 0; i < m; i++)
            if (!pp[i])
                memset(parity[i], 0, TBT_FEC_S);
        (void)tbt_sclc_uplink_decode(u, 0, data, parity, pd, pp);
        dec_blocks++;
    } while (now_sec() - t0 < 0.5);
    dec_s = now_sec() - t0;
    dec_mbps = (double)dec_blocks * n * TBT_FEC_S / 1e6 / dec_s;

    printf("encode %.2f MB/s   decode %.2f MB/s (p=%.2f, 1 group)\n",
           enc_mbps, dec_mbps, p);
    {
        struct tbt_sclc_stat st;

        tbt_sclc_uplink_stat(u, 0, &st);
        printf("last decode: codes_ok %u/%u failed %u, shares missing %u "
               "recovered %u\n",
               st.codes_ok, st.codes, st.codes_failed, st.shares_missing,
               st.shares_recovered);
    }
    printf("projected %u-group encode %.2f MB/s, decode %.2f MB/s\n",
           groups, enc_mbps, dec_mbps);
    return 0;
}
