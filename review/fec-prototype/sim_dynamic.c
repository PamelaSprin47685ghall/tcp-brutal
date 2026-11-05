#include "rlnc.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

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

/*
 * Recovery probability for one window. L = w. All parity packets arrive.
 * The data loss rate is p. A trial succeeds when every lost packet returns
 * and the bytes match.
 */
static double recovery(unsigned n, unsigned m, unsigned w, unsigned d,
                       unsigned k, double p, unsigned trials, uint64_t nonce)
{
    unsigned L = w;
    unsigned G = L / k + (L % k != 0);
    unsigned P = G * m;
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

    if (!ctx)
    {
        return 0.0;
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
        int rc;

        /* Average over several coefficient sets, not just one nonce. */
        if (i > 0 && i % 100 == 0)
        {
            rlnc_free(ctx);
            ctx = rlnc_new(n, m, k, w, 1, d, nonce + i);
            if (!ctx)
            {
                exit(2);
            }
        }

        for (t = 0; t < L; t++)
        {
            dbuf[t] = (uint8_t)rnd32();
        }
        memcpy(orig, dbuf, L);
        rlnc_encode(ctx, data, L, parity);

        for (t = 0; t < L; t++)
        {
            dp[t] = rnd_unit() >= p;
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
    }

    free(dbuf);
    free(pbuf);
    free(orig);
    free(data);
    free(parity);
    free(dp);
    free(pp);
    rlnc_free(ctx);

    return (double)recovered / trials;
}

static unsigned target_m(double p)
{
    unsigned m = (unsigned)ceil(256.0 * (p + 0.09));

    m = ((m + 7) / 8) * 8;
    if (m < 16)
    {
        m = 16;
    }
    if (m > 128)
    {
        m = 128;
    }
    return m;
}

static void controller(void)
{
    static const double steps[] = {0.01, 0.01, 0.10, 0.10, 0.20, 0.20,
                                   0.05, 0.05, 0.05, 0.05};
    double p_ewma = 0.01;
    unsigned m_current = 32;
    unsigned epoch;

    printf("\ncontroller: margin=0.09 M_min=16 M_max=128 hysteresis=8\n");
    printf("epoch,p_true,p_ewma,M_target,M_current,recovery\n");

    for (epoch = 0; epoch < sizeof(steps) / sizeof(steps[0]); epoch++)
    {
        unsigned n = 256 - m_current;
        unsigned target;
        double rec;

        p_ewma = 0.2 * p_ewma + 0.8 * steps[epoch];
        target = target_m(p_ewma);

        if (target > m_current + 8 || target + 8 < m_current)
        {
            m_current = target;
        }

        n = 256 - m_current;
        rec = recovery(n, m_current, 2 * n, 16, n, steps[epoch], 2000,
                       0x1234u + epoch);

        printf("%u,%.3f,%.4f,%u,%u,%.4f\n", epoch, steps[epoch], p_ewma,
               target, m_current, rec);
    }
}

static void rate_table(void)
{
    static const unsigned ms[] = {16, 32, 48, 56, 64, 80, 96, 104, 128};
    static const double ps[] = {0.01, 0.05, 0.10, 0.15, 0.20, 0.25, 0.30};
    unsigned mi;
    unsigned pi;

    printf("\nrate table: W=2K=2N=2(256-M) K=N d=16\n");
    printf("M,N");
    for (pi = 0; pi < sizeof(ps) / sizeof(ps[0]); pi++)
    {
        printf(",p=%.2f", ps[pi]);
    }
    printf("\n");

    for (mi = 0; mi < sizeof(ms) / sizeof(ms[0]); mi++)
    {
        unsigned m = ms[mi];
        unsigned n = 256 - m;

        printf("%u,%u", m, n);
        for (pi = 0; pi < sizeof(ps) / sizeof(ps[0]); pi++)
        {
            double rec = recovery(n, m, 2 * n, 16, n, ps[pi], 2000,
                                  0xabcdu + mi * 16 + pi);

            printf(",%.3f", rec);
        }
        printf("\n");
    }
}

static void window_bound(void)
{
    static const unsigned ms[] = {16, 32, 40, 48, 64, 96, 128};
    static const double ps[] = {0.01, 0.05, 0.10, 0.20, 0.30};
    static const unsigned ws[] = {64, 128, 192, 256};
    unsigned mi;
    unsigned pi;

    printf("\nwindow bound: largest W with recovery >= 0.99 (d=16, K=W/2)\n");
    printf("M,p,rule_W,largest_W\n");

    for (mi = 0; mi < sizeof(ms) / sizeof(ms[0]); mi++)
    {
        unsigned m = ms[mi];
        unsigned n = 256 - m;

        for (pi = 0; pi < sizeof(ps) / sizeof(ps[0]); pi++)
        {
            unsigned best = 0;
            unsigned wi;

            for (wi = 0; wi < sizeof(ws) / sizeof(ws[0]); wi++)
            {
                unsigned w = ws[wi];
                unsigned k = w / 2;
                double rec;

                if (k == 0)
                {
                    continue;
                }
                rec = recovery(n, m, w, 16, k, ps[pi], 2000,
                               0x5555u + mi * 32 + pi * 4 + wi);
                if (rec >= 0.99)
                {
                    best = w;
                }
            }

            printf("%u,%.2f,%.0f,%u\n", m, ps[pi],
                   ps[pi] > 0.0 ? (double)m / (2.0 * ps[pi]) : 0.0, best);
        }
    }
}

static void epoch_transition(void)
{
    static const unsigned pairs[][2] = {{32, 64}, {16, 48}, {64, 96}};
    unsigned i;

    printf("\nepoch transition: 50%% old + 50%% new, no cross-epoch mixing\n");
    printf("M_old,M_new,p,old_recovery,new_recovery,joint_recovery,loss\n");

    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
    {
        unsigned m0 = pairs[i][0];
        unsigned m1 = pairs[i][1];
        double p = 0.20;
        double r0 = recovery(256 - m0, m0, 256 - m0, 16, 256 - m0, p, 2000,
                             0x111u + i);
        double r1 = recovery(256 - m1, m1, 256 - m1, 16, 256 - m1, p, 2000,
                             0x222u + i);
        double joint = r0 * r1;
        double loss = r1 - joint;

        printf("%u,%u,%.2f,%.3f,%.3f,%.3f,%.3f\n", m0, m1, p, r0, r1, joint,
               loss);
    }
}

static void cost_table(void)
{
    static const unsigned ms[] = {16, 32, 48, 56, 64, 80, 96, 104, 128};
    unsigned i;

    printf("\ncost: W=2K=2N d=16 S=8192 encode XOR/byte = M*d/N, "
           "window bytes = 256*S\n");
    printf("M,N,goodput_Mbit_s,encode_xor_per_byte,window_kib,"
           "encode_core_pct_at_125MBps\n");

    for (i = 0; i < sizeof(ms) / sizeof(ms[0]); i++)
    {
        unsigned m = ms[i];
        unsigned n = 256 - m;
        double goodput = 1000.0 * (double)n / 256.0;
        double ops = (double)m * 16.0 / (double)n;
        double window = 256.0 * 8192.0 / 1024.0;
        /* Measured RLNC encode is about 10 GB/s at d=16 on this host. */
        double mbps = 10000.0;
        double core = 125.0 / mbps * 100.0;

        printf("%u,%u,%.1f,%.2f,%.0f,%.2f\n", m, n, goodput, ops, window,
               core);
    }
}

int main(void)
{
    controller();
    rate_table();
    window_bound();
    epoch_transition();
    cost_table();
    return 0;
}
