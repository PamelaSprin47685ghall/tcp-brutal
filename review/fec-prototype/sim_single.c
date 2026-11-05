/*
 * sim_single: host-side feasibility study of ONE 256-share sliding-window
 * RLNC, with no 32-way grouping.
 *
 * Model: N + M = 256 shares, K = N, W = 2N, L = W.  Then
 * G = ceil(L/K) = 2 coding groups and P = G*M = 2M parity packets.
 * With this geometry group 0 covers only the first half of the window and
 * group 1 covers the whole window (ws = g*K + K - W in rlnc.c).  So the
 * second half of the window is protected by the M group-1 rows alone.
 *
 * Per trial: fill L packets of S = 64 bytes from a deterministic stream,
 * encode, drop each data packet iid with probability p, keep every parity
 * packet, decode in place, compare every byte with the original.
 *
 * The rank condition of a GF(2) code depends only on the coefficient
 * matrix, never on S.  S = 64 therefore measures the same recovery
 * probability as S = 8192 and keeps the trial count affordable.  The
 * memory column is computed at the production payload S = 8192.
 *
 * Output: one CSV table on stdout, progress on stderr.
 *
 * Build:
 *   cc -O2 -std=c99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
 *      -pthread sim_single.c rlnc.c -o sim_single
 * Run:
 *   ./sim_single > ../single-sim.csv
 * Calibrate a subset:
 *   ./sim_single 200 57 3
 */

#define _POSIX_C_SOURCE 200809L

#include "rlnc.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Payload bytes used by the recovery trials. */
#define SIM_S 64u

/* Payload bytes used for the memory column (production packet size). */
#define PROD_S 8192u

/* Two-sided 99% normal quantile. */
#define Z99 2.5758293035489004

#define MAX_THREADS 16u

#define DEFAULT_TRIALS 20000u

/* Stream tags. */
#define TAG_NONCE 0x9e3779b97f4a7c15ull
#define TAG_FILL 0xd1b54a32d192ed03ull
#define TAG_DROP 0x94d049bb133111ebull

struct shape
{
    unsigned m;
    unsigned n;
    unsigned w; /* always 2*n */
};

static const struct shape shapes[] = {
    {64, 192, 384},
    {96, 160, 320},
    {128, 128, 256},
};

static const unsigned ds[] = {32, 64, 128, 230};
static const unsigned pcts[] = {1, 2, 5, 10, 20};

#define N_SHAPE (sizeof(shapes) / sizeof(shapes[0]))
#define N_D (sizeof(ds) / sizeof(ds[0]))
#define N_P (sizeof(pcts) / sizeof(pcts[0]))
#define N_POINT (N_SHAPE * N_D * N_P)

struct spec
{
    unsigned m;
    unsigned n;
    unsigned w;
    unsigned d;
    unsigned pct;
    double p;
};

struct result
{
    unsigned trials;
    unsigned recovered;
    unsigned stuck;
    unsigned corrupt;
    double r;
    double ci99;
    double xor_gbs;
    double app_mbit;
    size_t mem_bytes;
    double secs;
};

static struct spec specs[N_POINT];
static struct result results[N_POINT];
static unsigned order[N_POINT];

static unsigned n_points;
static unsigned next_order;
static unsigned n_threads;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t mix64(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

static uint64_t seed_of(unsigned m, unsigned d, unsigned pct, unsigned trial,
                        uint64_t tag)
{
    uint64_t h = 0x243f6a8885a308d3ull;

    h = mix64(h ^ ((uint64_t)m << 48) ^ ((uint64_t)d << 32) ^
              ((uint64_t)pct << 16) ^ (uint64_t)trial);
    return mix64(h ^ tag);
}

static uint64_t rnd64(uint64_t *st)
{
    uint64_t z = (*st += 0x9e3779b97f4a7c15ull);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static double rnd_unit(uint64_t *st)
{
    return (double)(rnd64(st) >> 11) * (1.0 / 9007199254740992.0);
}

/* Newton square root: keeps the build free of -lm. */
static double dsqrt(double x)
{
    double r;
    unsigned i;

    if (x <= 0.0)
    {
        return 0.0;
    }

    r = x > 1.0 ? x : 1.0;
    for (i = 0; i < 60; i++)
    {
        r = 0.5 * (r + x / r);
    }

    return r;
}

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Wilson score interval half width for a two-sided 99% interval. */
static double wilson_half(double r, unsigned n)
{
    double nn = (double)n;
    double z2 = Z99 * Z99;
    double den = 1.0 + z2 / nn;

    return Z99 * dsqrt(r * (1.0 - r) / nn + z2 / (4.0 * nn * nn)) / den;
}

static void die(const char *what)
{
    fprintf(stderr, "sim_single: %s\n", what);
    exit(2);
}

static struct result run_point(const struct spec *sp, unsigned trials)
{
    unsigned L = sp->w;
    unsigned P = 2u * sp->m;
    struct result out;
    struct rlnc_ctx *mem_ctx;
    uint8_t *dbuf;
    uint8_t *pbuf;
    uint8_t *orig;
    uint8_t **data;
    uint8_t **parity;
    uint8_t *dp;
    uint8_t *pp;
    unsigned i;
    unsigned t;
    double t0;

    memset(&out, 0, sizeof(out));
    out.trials = trials;

    t0 = now_sec();

    dbuf = malloc((size_t)L * SIM_S);
    pbuf = malloc((size_t)P * SIM_S);
    orig = malloc((size_t)L * SIM_S);
    data = malloc((size_t)L * sizeof(*data));
    parity = malloc((size_t)P * sizeof(*parity));
    dp = malloc(L);
    pp = malloc(P);

    if (!dbuf || !pbuf || !orig || !data || !parity || !dp || !pp)
    {
        die("out of memory");
    }

    for (i = 0; i < L; i++)
    {
        data[i] = dbuf + (size_t)i * SIM_S;
    }
    for (i = 0; i < P; i++)
    {
        parity[i] = pbuf + (size_t)i * SIM_S;
    }

    for (i = 0; i < trials; i++)
    {
        uint64_t fill_st = seed_of(sp->m, sp->d, sp->pct, i, TAG_FILL);
        uint64_t drop_st = seed_of(sp->m, sp->d, sp->pct, i, TAG_DROP);
        uint64_t nonce = seed_of(sp->m, sp->d, sp->pct, i, TAG_NONCE);
        struct rlnc_ctx *ctx;
        int rc;

        for (t = 0; t < L * SIM_S; t++)
        {
            dbuf[t] = (uint8_t)rnd64(&fill_st);
        }
        memcpy(orig, dbuf, (size_t)L * SIM_S);

        ctx = rlnc_new(sp->n, sp->m, sp->n, sp->w, SIM_S, sp->d, nonce);
        if (!ctx)
        {
            die("rlnc_new failed");
        }
        if (rlnc_parity_count(ctx, L) != P)
        {
            die("unexpected parity count");
        }
        if (rlnc_encode(ctx, data, L, parity) != (int)P)
        {
            die("rlnc_encode failed");
        }

        for (t = 0; t < L; t++)
        {
            dp[t] = rnd_unit(&drop_st) >= sp->p;
        }
        memset(pp, 1, P);

        rc = rlnc_decode(ctx, data, parity, dp, pp, L);
        if (rc >= 0 && memcmp(dbuf, orig, (size_t)L * SIM_S) == 0)
        {
            out.recovered++;
        }
        else if (rc >= 0)
        {
            out.corrupt++;
        }
        else
        {
            out.stuck++;
        }

        rlnc_free(ctx);
    }

    out.r = (double)out.recovered / (double)trials;
    out.ci99 = wilson_half(out.r, trials);

    /* Derived cost at 1 Gbit/s of application payload (125 MB/s), q = 2. */
    out.xor_gbs = (double)sp->m / (double)sp->n * (double)sp->d * 2.0 *
                  0.125;
    /* Application goodput when the wire carries 1 Gbit/s in total. */
    out.app_mbit = 1000.0 * (double)sp->n / 256.0;

    mem_ctx = rlnc_new(sp->n, sp->m, sp->n, sp->w, PROD_S, sp->d, 1);
    if (!mem_ctx)
    {
        die("rlnc_new for memory failed");
    }
    out.mem_bytes = rlnc_memory_bytes(mem_ctx, L);
    rlnc_free(mem_ctx);

    out.secs = now_sec() - t0;

    free(dbuf);
    free(pbuf);
    free(orig);
    free(data);
    free(parity);
    free(dp);
    free(pp);

    return out;
}

static unsigned cost_key(const struct spec *sp)
{
    /* Dominant term is the O(d^2) duplicate check inside rlnc_coeffs(),
     * paid once per parity row and once per decode row. */
    return 2u * sp->m * sp->d * sp->d;
}

static int cmp_cost_desc(const void *a, const void *b)
{
    unsigned ia = *(const unsigned *)a;
    unsigned ib = *(const unsigned *)b;
    unsigned ka = cost_key(&specs[ia]);
    unsigned kb = cost_key(&specs[ib]);

    if (ka != kb)
    {
        return ka > kb ? -1 : 1;
    }

    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

static void *worker(void *arg)
{
    (void)arg;

    for (;;)
    {
        unsigned slot;
        unsigned idx;
        struct result res;

        pthread_mutex_lock(&queue_lock);
        slot = next_order++;
        pthread_mutex_unlock(&queue_lock);

        if (slot >= n_points)
        {
            break;
        }

        idx = order[slot];
        res = run_point(&specs[idx], results[idx].trials);
        results[idx] = res;

        pthread_mutex_lock(&log_lock);
        fprintf(stderr,
                "point %2u/%u M=%3u N=%3u W=%3u d=%3u p=%.2f "
                "r=%.6f stuck=%u corrupt=%u %.1fs\n",
                idx + 1, n_points, specs[idx].m, specs[idx].n,
                specs[idx].w, specs[idx].d, specs[idx].p, res.r,
                res.stuck, res.corrupt, res.secs);
        pthread_mutex_unlock(&log_lock);
    }

    return NULL;
}

int main(int argc, char **argv)
{
    unsigned trials = DEFAULT_TRIALS;
    unsigned start = 0;
    unsigned count = 0;
    unsigned si;
    unsigned di;
    unsigned pi;
    unsigned i;
    long ncpu;
    unsigned corrupt_total = 0;
    double t0;

    if (argc > 1)
    {
        trials = (unsigned)strtoul(argv[1], NULL, 10);
        if (trials == 0)
        {
            trials = 1;
        }
    }
    if (argc > 2)
    {
        start = (unsigned)strtoul(argv[2], NULL, 10);
    }
    if (argc > 3)
    {
        count = (unsigned)strtoul(argv[3], NULL, 10);
    }

    n_points = 0;
    for (si = 0; si < N_SHAPE; si++)
    {
        for (di = 0; di < N_D; di++)
        {
            for (pi = 0; pi < N_P; pi++)
            {
                struct spec *sp = &specs[n_points];

                sp->m = shapes[si].m;
                sp->n = shapes[si].n;
                sp->w = shapes[si].w;
                sp->d = ds[di];
                sp->pct = pcts[pi];
                sp->p = (double)pcts[pi] / 100.0;
                n_points++;
            }
        }
    }

    if (start >= n_points)
    {
        start = 0;
    }
    if (count == 0 || start + count > n_points)
    {
        count = n_points - start;
    }

    /* Keep the requested slice, preserving matrix order in the output. */
    for (i = 0; i < count; i++)
    {
        order[i] = start + i;
    }
    n_points = count;
    qsort(order, n_points, sizeof(order[0]), cmp_cost_desc);

    for (i = 0; i < n_points; i++)
    {
        results[order[i]].trials = trials;
    }

    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1)
    {
        ncpu = 1;
    }
    n_threads = (unsigned)ncpu;
    if (n_threads > MAX_THREADS)
    {
        n_threads = MAX_THREADS;
    }
    if (n_threads > n_points)
    {
        n_threads = n_points;
    }

    fprintf(stderr,
            "sim_single: %u points, %u trials, %u threads, S=%u, "
            "mem payload=%u\n",
            n_points, trials, n_threads, SIM_S, PROD_S);

    t0 = now_sec();

    {
        pthread_t th[MAX_THREADS];
        unsigned created = 0;

        for (i = 0; i < n_threads; i++)
        {
            if (pthread_create(&th[i], NULL, worker, NULL) != 0)
            {
                die("pthread_create failed");
            }
            created++;
        }
        for (i = 0; i < created; i++)
        {
            pthread_join(th[i], NULL);
        }
    }

    printf("M,N,W,d,p,trials,recovery,ci99,xor_gbs,app_mbit,mem_bytes\n");
    for (i = 0; i < n_points; i++)
    {
        unsigned idx = start + i;
        const struct spec *sp = &specs[idx];
        const struct result *res = &results[idx];

        if (res->recovered + res->stuck + res->corrupt != res->trials)
        {
            die("trial accounting mismatch");
        }
        corrupt_total += res->corrupt;

        printf("%u,%u,%u,%u,%.2f,%u,%.6f,%.6f,%.3f,%.0f,%zu\n",
               sp->m, sp->n, sp->w, sp->d, sp->p, res->trials,
               res->r, res->ci99, res->xor_gbs, res->app_mbit,
               res->mem_bytes);
    }

    fprintf(stderr, "sim_single: done in %.1fs, corrupt=%u\n",
            now_sec() - t0, corrupt_total);

    return corrupt_total == 0 ? 0 : 1;
}
