#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPACKETS 10000u
#define NCARRIERS 5u
#define NDATA 4u
#define NPARITY 1u
#define KSTEP 8u
#define WINDOW 16u

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
    return ((double)(rnd32() % 1000000u) + 1.0) / 1000001.0;
}

static double jitter(double mean)
{
    return -log(rnd_unit()) * mean;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;

    return x < y ? -1 : (x > y ? 1 : 0);
}

static double percentile(const double *v, unsigned n, double q)
{
    unsigned idx = (unsigned)(q * (double)(n - 1));

    return v[idx];
}

static void run_block(const double rate[NCARRIERS], double *lat)
{
    double next_free[NCARRIERS] = {0};
    double size_bits = 8192.0 * 8.0;
    unsigned blocks = NPACKETS / NDATA;
    unsigned b;
    unsigned i;
    double block_start = 0;

    for (b = 0; b < blocks; b++)
    {
        double arrival[NCARRIERS];
        double start = block_start;
        double done;
        unsigned c;
        unsigned j;

        for (c = 0; c < NCARRIERS; c++)
        {
            double tx = size_bits / rate[c];

            if (next_free[c] < start)
            {
                next_free[c] = start;
            }
            arrival[c] = next_free[c] + tx + 0.218 + jitter(0.020);
            next_free[c] += tx;
        }

        /* Sort the five arrival times and take the fourth (index 3). */
        for (i = 0; i < NCARRIERS; i++)
        {
            for (j = i + 1; j < NCARRIERS; j++)
            {
                if (arrival[j] < arrival[i])
                {
                    double t = arrival[i];

                    arrival[i] = arrival[j];
                    arrival[j] = t;
                }
            }
        }

        done = arrival[NDATA - 1];
        block_start = done;
        for (i = 0; i < NDATA; i++)
        {
            lat[b * NDATA + i] = (done - start) * 1e3;
        }
    }
}

static void run_sliding(const double rate[NCARRIERS], double *lat,
                        double *stall)
{
    double next_free[NCARRIERS] = {0};
    double size_bits = 8192.0 * 8.0;
    unsigned groups = NPACKETS / KSTEP;
    double *data_start;
    double *data_arrival;
    double *parity_arrival;
    unsigned i;
    unsigned g;
    double total_stall = 0.0;

    data_start = malloc((size_t)NPACKETS * sizeof(double));
    data_arrival = malloc((size_t)NPACKETS * sizeof(double));
    parity_arrival = malloc((size_t)groups * sizeof(double));
    if (!data_start || !data_arrival || !parity_arrival)
    {
        exit(2);
    }

    for (i = 0; i < NPACKETS; i++)
    {
        unsigned c = i % NDATA;
        double tx = size_bits / rate[c];

        data_start[i] = next_free[c];
        data_arrival[i] =
            next_free[c] + tx + 0.218 + jitter(0.020);
        next_free[c] += tx;
    }

    for (g = 0; g < groups; g++)
    {
        unsigned we = g * KSTEP + KSTEP - 1;
        double start = next_free[NPARITY + 3];
        double tx = size_bits / rate[NPARITY + 3];

        if (we >= NPACKETS)
        {
            we = NPACKETS - 1;
        }
        if (data_start[we] > start)
        {
            start = data_start[we];
        }
        parity_arrival[g] =
            start + tx + 0.218 + jitter(0.020);
    }

    for (i = 0; i < NPACKETS; i++)
    {
        int lost = rnd32() % 100 == 0;
        double done = data_arrival[i];

        if (lost)
        {
            unsigned g0 = i / KSTEP;
            unsigned g1 = g0 + 1;
            double best = 1e30;

            if (g0 > 0 && g0 - 1 < groups)
            {
                /* The previous group also covers the window start. */
            }
            if (g0 < groups && parity_arrival[g0] < best)
            {
                best = parity_arrival[g0];
            }
            if (g1 < groups && parity_arrival[g1] < best)
            {
                best = parity_arrival[g1];
            }
            if (best < 1e29)
            {
                done = best;
            }
        }

        lat[i] = (done - data_start[i]) * 1e3;
        if (done > data_arrival[i])
        {
            total_stall += done - data_arrival[i];
        }
    }

    *stall = total_stall;

    free(data_start);
    free(data_arrival);
    free(parity_arrival);
}

static void report(const char *name, const double *lat, double stall)
{
    double *copy = malloc(NPACKETS * sizeof(double));

    if (!copy)
    {
        exit(2);
    }
    memcpy(copy, lat, NPACKETS * sizeof(double));
    qsort(copy, NPACKETS, sizeof(double), cmp_double);

    printf("%s,%.2f,%.2f,%.2f,%.2f,%.3f\n", name,
           percentile(copy, NPACKETS, 0.50),
           percentile(copy, NPACKETS, 0.90),
           percentile(copy, NPACKETS, 0.99),
           percentile(copy, NPACKETS, 0.999), stall);

    free(copy);
}

int main(void)
{
    static const double rates[3][NCARRIERS] = {
        {3.4e6, 3.4e6, 3.4e6, 3.4e6, 3.4e6},
        {3.4e6, 3.4e6, 3.4e6, 3.4e6, 1.7e6},
        {3.4e6, 3.4e6, 3.4e6, 1.133e6, 1.133e6},
    };
    static const char *names[3] = {
        "balanced",
        "one_slow_2x",
        "two_slow_3x",
    };
    double *lat = malloc(NPACKETS * sizeof(double));
    unsigned r;

    if (!lat)
    {
        exit(2);
    }

    printf("rates,scheme,p50_ms,p90_ms,p99_ms,p999_ms,stall_s\n");

    for (r = 0; r < 3; r++)
    {
        double stall = 0.0;

        rng_state = 0x9e3779b97f4a7c15ull + r;
        run_block(rates[r], lat);
        report(names[r], lat, 0.0);

        rng_state = 0x243f6a8885a308d3ull + r;
        run_sliding(rates[r], lat, &stall);
        {
            char buf[64];

            snprintf(buf, sizeof(buf), "%s_sliding", names[r]);
            report(buf, lat, stall);
        }
    }

    free(lat);
    return 0;
}
