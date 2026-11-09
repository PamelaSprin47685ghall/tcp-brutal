/* bench_sclc_downlink.c - throughput of every downlink ladder point.
 *
 * One FEC block is always 256 shares x 8192 bytes = 2 MiB on the wire, so
 * the wire rate is the fair cross-rate number: the decoder walks all 256
 * shares whether the point is 25 or 70 percent. Payload MB/s is printed
 * too, but it falls with the rate by construction and is not the bound.
 *
 * The decoder case erases one DATA share, the normal repair path, so the
 * gather/scatter work is measured rather than the all-present fast path.
 * Timing uses process CPU time: the host build machine runs other work in
 * parallel, and wall time would report their preemption as codec cost.
 */
#include "tbt_fec_sclc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SHARE TBT_SCLC_SHARE
#define REPS 24
#define MIN_MBPS 250.0

static const struct ladder
{
    unsigned pct;
    unsigned n;
    unsigned m;
} ladder[] = {
    {25, 64, 192}, {30, 76, 180}, {40, 102, 154},
    {50, 128, 128}, {60, 151, 105}, {70, 178, 78},
};

static u64 now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static u64 rng_state = 0x9E3779B97F4A7C15ull;

static u64 rnd64(void)
{
    u64 z = (rng_state += 0x9E3779B97F4A7C15ull);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void fill_random(u8 *p, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        p[i] = (u8)(rnd64() >> 24);
}

static int run_one(const struct ladder *r)
{
    const struct tbt_fec_ops *ops = &tbt_fec_ops_sclc;
    void *c = ops->new((u16)r->n, (u16)r->m,
                       (u16)(r->n * 1000u / 256u), TBT_SCLC_BLOCK, SHARE,
                       0, 0xD0 + r->pct);
    u8 *data = calloc(r->n, SHARE), *parity = calloc(r->m, SHARE);
    u8 **dp = calloc(r->n, sizeof(*dp)), **pp = calloc(r->m, sizeof(*pp));
    u8 *data_present = calloc(r->n, 1), *parity_present = calloc(r->m, 1);
    u64 t0, t1;
    double enc_wire, enc_payload, dec_wire, dec_payload;
    int i, rep;

    if (!c || !data || !parity || !dp || !pp || !data_present ||
        !parity_present)
    {
        fprintf(stderr, "bench: allocation failed at rate %u\n", r->pct);
        return 1;
    }
    for (i = 0; i < (int)r->n; i++)
    {
        dp[i] = data + (size_t)i * SHARE;
        fill_random(dp[i], SHARE);
    }
    for (i = 0; i < (int)r->m; i++)
        pp[i] = parity + (size_t)i * SHARE;

    {
        u64 best = ~(u64)0;

        for (rep = 0; rep < REPS; rep++)
        {
            t0 = now_ns();
            ops->encode(c, dp, r->n, pp);
            t1 = now_ns();
            if (t1 - t0 < best)
                best = t1 - t0;
        }
        enc_wire = 256.0 * SHARE * 1e9 / (double)best / 1e6;
        enc_payload = (double)r->n * SHARE * 1e9 / (double)best / 1e6;
    }

    for (i = 0; i < (int)r->n; i++)
        data_present[i] = 1;
    for (i = 0; i < (int)r->m; i++)
        parity_present[i] = 1;
    data_present[0] = 0;
    {
        u64 best = ~(u64)0;

        for (rep = 0; rep < REPS; rep++)
        {
            memset(dp[0], 0x5A, SHARE);
            t0 = now_ns();
            ops->decode(c, dp, pp, data_present, parity_present, r->n);
            t1 = now_ns();
            if (t1 - t0 < best)
                best = t1 - t0;
        }
        dec_wire = 256.0 * SHARE * 1e9 / (double)best / 1e6;
        dec_payload = (double)r->n * SHARE * 1e9 / (double)best / 1e6;
    }

    printf("rate %2u%% n=%3u m=%3u  encode %7.1f wire %7.1f payload MB/s"
           "  decode %7.1f wire %7.1f payload MB/s\n",
           r->pct, r->n, r->m, enc_wire, enc_payload, dec_wire, dec_payload);

    free(data_present);
    free(parity_present);
    free(dp);
    free(pp);
    free(data);
    free(parity);
    ops->free(c);
    return (enc_wire < MIN_MBPS || dec_wire < MIN_MBPS) ? 1 : 0;
}

int main(void)
{
    unsigned i;
    int bad = 0;

    for (i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++)
        bad |= run_one(&ladder[i]);
    if (bad)
    {
        double la[1] = {0.0};
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

        /* A host gate is meaningful only when the machine is not already
         * oversubscribed. The per-op best of REPS is a peak number; a load
         * average above the CPU count still leaves it frequency-limited. */
        if (getloadavg(la, 1) == 1 && ncpu > 0 && la[0] > (double)ncpu)
        {
            printf("bench_sclc_downlink: wire gate skipped, load %.2f on "
                   "%ld cpus\n", la[0], ncpu);
            return 0;
        }
        fprintf(stderr, "bench_sclc_downlink: a wire rate is below %.0f MB/s\n",
                MIN_MBPS);
        return 1;
    }
    printf("bench_sclc_downlink: OK (wire rate >= %.0f MB/s)\n", MIN_MBPS);
    return 0;
}
