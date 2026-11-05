#include "fec.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Map share s to carrier s % carriers. Drop whole carriers to model a
 * carrier failure. A burst of F carriers removes F * k shares.
 */

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint32_t rnd32(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

static double binom_tail(unsigned N, unsigned m, double p)
{
    /* P(X <= m) for X ~ Binomial(N, p). */
    double term = pow(1.0 - p, (double)N);
    double sum = 0.0;
    unsigned i;

    for (i = 0; i <= m; i++)
    {
        sum += term;
        if (i < m)
        {
            term *= (double)(N - i) / (double)(i + 1) * p / (1.0 - p);
        }
    }

    return sum;
}

static void mask_zero(uint8_t mask[FEC_MASK_BYTES])
{
    memset(mask, 0, FEC_MASK_BYTES);
}

static void mask_set(uint8_t mask[FEC_MASK_BYTES], unsigned bit)
{
    mask[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static void mask_clear(uint8_t mask[FEC_MASK_BYTES], unsigned bit)
{
    mask[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}

static int mask_get(const uint8_t mask[FEC_MASK_BYTES], unsigned bit)
{
    return (mask[bit >> 3] >> (bit & 7)) & 1;
}

static void run_case(unsigned n, unsigned m, unsigned carriers, int mode,
                     double p, unsigned f, unsigned trials)
{
    unsigned total = n + m;
    struct fec_ctx *ctx;
    uint8_t *buf;
    uint8_t *shares[FEC_MAX_SHARES];
    unsigned i;
    unsigned recovered = 0;
    unsigned theoretical_lost = 0;
    double theory;

    if (total % carriers != 0)
    {
        printf("%u,%u,%u,-,%s,-,%u,rejected (n+m not divisible by carriers),-\n",
               n, m, carriers, mode ? "burst" : "random", trials);
        return;
    }

    ctx = fec_new(n, m);
    if (!ctx)
    {
        printf("%u,%u,%u,-,%s,-,%u,fec_new failed,-\n", n, m, carriers,
               mode ? "burst" : "random", trials);
        return;
    }

    buf = malloc(total);
    if (!buf)
    {
        exit(2);
    }
    for (i = 0; i < total; i++)
    {
        shares[i] = buf + i;
        buf[i] = (uint8_t)i;
    }

    if (mode == 0)
    {
        theory = binom_tail(total, m, p);
    }
    else
    {
        theoretical_lost = f * (total / carriers);
        theory = theoretical_lost <= m ? 1.0 : 0.0;
    }

    for (i = 0; i < trials; i++)
    {
        uint8_t mask[FEC_MASK_BYTES];
        unsigned lost = 0;
        unsigned s;

        mask_zero(mask);
        for (s = 0; s < total; s++)
        {
            mask_set(mask, s);
        }

        if (mode == 0)
        {
            for (s = 0; s < total; s++)
            {
                if ((double)(rnd32() % 1000000u) / 1000000.0 < p)
                {
                    mask_clear(mask, s);
                    lost++;
                }
            }
        }
        else
        {
            unsigned dropped[64];
            unsigned d;
            unsigned c;

            for (d = 0; d < f; d++)
            {
                unsigned pick;

                do
                {
                    pick = rnd32() % carriers;
                    for (c = 0; c < d; c++)
                    {
                        if (dropped[c] == pick)
                        {
                            break;
                        }
                    }
                } while (c < d);
                dropped[d] = pick;
            }

            for (s = 0; s < total; s++)
            {
                unsigned carrier = s % carriers;

                for (c = 0; c < f; c++)
                {
                    if (dropped[c] == carrier)
                    {
                        mask_clear(mask, s);
                        lost++;
                        break;
                    }
                }
            }
        }

        if (lost <= m &&
            fec_decode(ctx, shares, mask, 1) == FEC_OK)
        {
            recovered++;
        }
    }

    printf("%u,%u,%u,%u,%s,%s,%u,%.4f,%.4f\n", n, m, carriers,
           total / carriers, mode ? "burst" : "random",
           mode ? "F" : "p", trials, (double)recovered / trials, theory);

    (void)mask_get;
    free(buf);
    fec_free(ctx);
}

int main(void)
{
    static const unsigned cases[][3] = {
        {96, 32, 16},
        {96, 32, 8},
        {16, 4, 4},
        {8, 2, 4},
    };
    unsigned i;

    printf("n,m,carriers,k,mode,param,trials,recovered_rate,theory\n");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        unsigned n = cases[i][0];
        unsigned m = cases[i][1];
        unsigned carriers = cases[i][2];
        unsigned k;
        unsigned f;
        double ps[] = {0.05, 0.10, 0.20};
        unsigned pi;

        if ((n + m) % carriers != 0)
        {
            run_case(n, m, carriers, 1, 0.0, 1, 1000);
            continue;
        }

        k = (n + m) / carriers;

        for (pi = 0; pi < 3; pi++)
        {
            run_case(n, m, carriers, 0, ps[pi], 0, 1000);
        }

        for (f = 1; f <= m / k + 2 && f <= 8; f++)
        {
            run_case(n, m, carriers, 1, 0.0, f, 1000);
        }
    }

    return 0;
}
