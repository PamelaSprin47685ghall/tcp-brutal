/*
 * sim_hnd0: host-side study of the grouped 32x8 RLNC shape used by the
 * 256-carrier / 1G production profile.
 *
 * The simulation models ONE coding window L = W data packets with K = N,
 * G = ceil(L/K) coding groups and P = G*M parity packets.  Recovery is
 * measured with the production coefficient generator in rlnc.c, so the
 * numbers describe that code, not an idealised erasure code.
 *
 * Output is CSV on stdout, split into labelled '#' sections.
 *
 * Build:
 *   cc -O2 -std=c99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
 *      sim_hnd0.c rlnc.c -o sim_hnd0
 */

#define _POSIX_C_SOURCE 200809L

#include "rlnc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Payload bytes used by the recovery simulation.  The rank condition of a
 * GF(2) code depends only on the coefficient matrix, never on S, so a small
 * S measures exactly the same probability as S = 8192 and keeps the trial
 * count affordable.  S = 64 still makes the byte comparison meaningful. */
#define SIM_S 64u

/* Encode throughput is measured at the production payload size. */
#define BENCH_S 8192u

#define TRIALS 50000u

/* Two-sided 99% normal quantile. */
#define Z99 2.5758293035489004

#define P_MAX 8u

static const double ps[P_MAX] = {0.01, 0.02, 0.05, 0.10,
                                 0.15, 0.20, 0.25, 0.30};

struct cfg
{
    const char *name;
    const char *kind;
    unsigned n; /* data carriers, sizes caller buffers only */
    unsigned m; /* parity per coding group */
    unsigned k; /* data packets per coding group (K = N) */
    unsigned w; /* coding window in packets */
    unsigned d; /* XOR terms per parity packet */
    unsigned l; /* modelled window length in packets (normally L = W) */
};

/*
 * Production shape: review/fec-design.md section 3.8.7, one group of the
 * 32x8 split.  d = W for every row, as written in the design table.
 */
static const struct cfg cfgs[] = {
    {"prod-M2-N6-W12-d12", "prod", 6, 2, 6, 12, 12, 12},
    {"prod-M3-N5-W10-d10", "prod", 5, 3, 5, 10, 10, 10},
    {"prod-M4-N4-W8-d8", "prod", 4, 4, 4, 8, 8, 8},
    {"prod-M5-N3-W6-d6", "prod", 3, 5, 3, 6, 6, 6},
    {"prod-M6-N2-W4-d4", "prod", 2, 6, 2, 4, 4, 4},

    /* Rows that also exist in design table 3.8.2 with d < W.  They are the
     * only way to check this simulator against the design's own numbers. */
    {"design-M2-N6-W12-d8", "design", 6, 2, 6, 12, 8, 12},
    {"design-M3-N5-W10-d8", "design", 5, 3, 5, 10, 8, 10},

    /* Half-density diagnostic: d = W/2.  Not a design row; shows what the
     * same code can do once the parity rows are no longer identical. */
    {"diag-M2-N6-W12-d6", "diag", 6, 2, 6, 12, 6, 12},
    {"diag-M3-N5-W10-d5", "diag", 5, 3, 5, 10, 5, 10},
    {"diag-M4-N4-W8-d4", "diag", 4, 4, 4, 8, 4, 8},
    {"diag-M5-N3-W6-d3", "diag", 3, 5, 3, 6, 3, 6},
    {"diag-M6-N2-W4-d2", "diag", 2, 6, 2, 4, 2, 4},

    /* Steady-state probe: L = 3W so most packets sit inside two full group
     * windows instead of one truncated and one full window. */
    {"probe-M4-N4-W8-d8-L24", "probe", 4, 4, 4, 8, 8, 24},
    {"probe-M4-N4-W8-d4-L24", "probe", 4, 4, 4, 8, 4, 24},

    /* Non-production control from the earlier small-N/M matrix.  These are
     * single coding groups, never a recommendation. */
    {"ctrl-N4-M1-W8-d8", "ctrl", 4, 1, 4, 8, 8, 8},
    {"ctrl-N8-M2-W16-d8", "ctrl", 8, 2, 8, 16, 8, 16},
    {"ctrl-N8-M2-W16-d16", "ctrl", 8, 2, 8, 16, 16, 16},
    {"ctrl-N16-M4-W32-d8", "ctrl", 16, 4, 16, 32, 8, 32},
    {"ctrl-N16-M4-W32-d16", "ctrl", 16, 4, 16, 32, 16, 32},
};

#define N_CFG (sizeof(cfgs) / sizeof(cfgs[0]))

static uint64_t rng_state;

static uint64_t rnd64(void)
{
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15ull);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static double rnd_unit(void)
{
    return (double)(rnd64() >> 11) * (1.0 / 9007199254740992.0);
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

static unsigned ceil_div(unsigned a, unsigned b)
{
    return a / b + (a % b != 0);
}

static unsigned group_count(const struct cfg *c)
{
    return ceil_div(c->l, c->k);
}

static unsigned parity_count(const struct cfg *c)
{
    return group_count(c) * c->m;
}

static uint64_t cfg_nonce(const struct cfg *c)
{
    return 0x243f6a8885a308d3ull ^ ((uint64_t)c->m << 40) ^
           ((uint64_t)c->k << 32) ^ ((uint64_t)c->w << 16) ^
           (uint64_t)c->d;
}

static uint64_t seed_for(const struct cfg *c, unsigned pi)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const char *s = c->name;

    while (*s)
    {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ull;
    }

    return h ^ ((uint64_t)pi * 0x9e3779b97f4a7c15ull);
}

/*
 * Packets-to-window-close for each data index: the smallest (window end -
 * index) over the coding groups that cover the packet.  This is the wait
 * before the covering equations are complete.
 */
static void precompute_latency(const struct cfg *c, unsigned *lat)
{
    unsigned t;

    for (t = 0; t < c->l; t++)
    {
        unsigned best = 0xffffffffu;
        unsigned g;

        for (g = 0; g < group_count(c); g++)
        {
            int ws = (int)(g * c->k) + (int)c->k - (int)c->w;
            int we = (int)(g * c->k) + (int)c->k - 1;

            if ((int)t >= ws && (int)t <= we)
            {
                int end = we;

                if (end > (int)c->l - 1)
                {
                    end = (int)c->l - 1;
                }
                if (end >= (int)t && (unsigned)(end - (int)t) < best)
                {
                    best = (unsigned)(end - (int)t);
                }
            }
        }

        lat[t] = best == 0xffffffffu ? 0 : best;
    }
}

struct point
{
    double r;        /* unconditional per-group recovery */
    double r_loss;   /* recovery conditioned on at least one loss */
    double ci_half;  /* half width of the Wilson 99% interval */
    double lat_mean; /* mean over loss cases of the worst packet */
    double lat_max;  /* largest worst-case latency seen, in packets */
    unsigned recovered;
    unsigned recovered_loss;
    unsigned loss_trials;
    unsigned rank_fail;
    unsigned corrupt;
};

static struct point res[N_CFG][P_MAX];

static struct point run_point(const struct cfg *c, unsigned pi, double p,
                              unsigned trials)
{
    struct point out;
    unsigned L = c->l;
    unsigned P = parity_count(c);
    size_t s = SIM_S;
    struct rlnc_ctx *ctx;
    uint8_t *dbuf;
    uint8_t *pbuf;
    uint8_t *orig;
    uint8_t **data;
    uint8_t **parity;
    uint8_t *dp;
    uint8_t *pp;
    unsigned *lat;
    unsigned i;
    unsigned t;
    unsigned recovered = 0;
    unsigned recovered_loss = 0;
    unsigned rank_fail = 0;
    unsigned corrupt = 0;
    unsigned loss_trials = 0;
    double lat_sum = 0.0;
    double lat_max = 0.0;

    memset(&out, 0, sizeof(out));
    rng_state = seed_for(c, pi);

    ctx = rlnc_new(c->n, c->m, c->k, c->w, s, c->d, cfg_nonce(c));
    dbuf = malloc((size_t)L * s);
    pbuf = malloc((size_t)P * s);
    orig = malloc((size_t)L * s);
    data = malloc((size_t)L * sizeof(*data));
    parity = malloc((size_t)P * sizeof(*parity));
    dp = malloc(L);
    pp = malloc(P);
    lat = malloc((size_t)L * sizeof(*lat));

    if (!ctx || !dbuf || !pbuf || !orig || !data || !parity || !dp || !pp ||
        !lat)
    {
        fprintf(stderr, "sim_hnd0: out of memory\n");
        exit(2);
    }

    for (i = 0; i < L; i++)
    {
        data[i] = dbuf + (size_t)i * s;
    }
    for (i = 0; i < P; i++)
    {
        parity[i] = pbuf + (size_t)i * s;
    }

    precompute_latency(c, lat);

    for (i = 0; i < trials; i++)
    {
        unsigned missing = 0;
        unsigned worst = 0;
        int rc;

        for (t = 0; t < L * (unsigned)s; t++)
        {
            dbuf[t] = (uint8_t)rnd64();
        }
        memcpy(orig, dbuf, (size_t)L * s);

        if (rlnc_encode(ctx, data, L, parity) != (int)P)
        {
            fprintf(stderr, "sim_hnd0: encode failed\n");
            exit(2);
        }

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
        if (rc >= 0 && memcmp(dbuf, orig, (size_t)L * s) == 0)
        {
            recovered++;
        }
        else if (rc >= 0)
        {
            corrupt++;
        }
        else
        {
            rank_fail++;
        }

        if (missing > 0)
        {
            loss_trials++;
            if (rc >= 0 && memcmp(dbuf, orig, (size_t)L * s) == 0)
            {
                recovered_loss++;
            }
            for (t = 0; t < L; t++)
            {
                if (!dp[t] && lat[t] > worst)
                {
                    worst = lat[t];
                }
            }
            lat_sum += (double)worst;
            if ((double)worst > lat_max)
            {
                lat_max = (double)worst;
            }
        }
    }

    out.recovered = recovered;
    out.recovered_loss = recovered_loss;
    out.r = (double)recovered / (double)trials;
    out.r_loss = loss_trials ? (double)recovered_loss / (double)loss_trials
                             : 0.0;
    out.lat_mean = loss_trials ? lat_sum / (double)loss_trials : 0.0;
    out.lat_max = lat_max;
    out.loss_trials = loss_trials;
    out.rank_fail = rank_fail;
    out.corrupt = corrupt;

    {
        double n = (double)trials;
        double z2 = Z99 * Z99;
        double den = 1.0 + z2 / n;
        double centre = (out.r + z2 / (2.0 * n)) / den;
        double half = Z99 * dsqrt(out.r * (1.0 - out.r) / n +
                                           z2 / (4.0 * n * n)) / den;

        out.ci_half = half;
        (void)centre;
    }

    free(dbuf);
    free(pbuf);
    free(orig);
    free(data);
    free(parity);
    free(dp);
    free(pp);
    free(lat);
    rlnc_free(ctx);

    return out;
}

/*
 * Nominal XOR bytes one rlnc_encode() call moves: each parity XORs every
 * selected position that lands inside [0, L).
 */
static size_t xor_bytes_per_call(const struct cfg *c, size_t s)
{
    size_t total = 0;
    unsigned g;
    unsigned r;
    unsigned i;
    unsigned idx[64];
    struct rlnc_ctx *ctx = rlnc_new(c->n, c->m, c->k, c->w, s, c->d,
                                    cfg_nonce(c));

    if (!ctx || c->w > 64)
    {
        rlnc_free(ctx);
        return 0;
    }

    for (g = 0; g < group_count(c); g++)
    {
        int ws = (int)(g * c->k) + (int)c->k - (int)c->w;

        for (r = 0; r < c->m; r++)
        {
            unsigned nidx = rlnc_coeffs(ctx, g, r, idx, 64);

            for (i = 0; i < nidx; i++)
            {
                int abs = ws + (int)idx[i];

                if (abs >= 0 && abs < (int)c->l)
                {
                    total += s;
                }
            }
        }
    }

    rlnc_free(ctx);
    return total;
}

static double bench_encode(const struct cfg *c, size_t s)
{
    unsigned L = c->l;
    unsigned P = parity_count(c);
    struct rlnc_ctx *ctx;
    uint8_t *dbuf;
    uint8_t *pbuf;
    uint8_t **data;
    uint8_t **parity;
    unsigned i;
    unsigned iters = 1;
    double rates[3];
    double best;
    double app_bytes = (double)L * (double)s;

    ctx = rlnc_new(c->n, c->m, c->k, c->w, s, c->d, cfg_nonce(c));
    dbuf = malloc((size_t)L * s);
    pbuf = malloc((size_t)P * s);
    data = malloc((size_t)L * sizeof(*data));
    parity = malloc((size_t)P * sizeof(*parity));

    if (!ctx || !dbuf || !pbuf || !data || !parity)
    {
        fprintf(stderr, "sim_hnd0: out of memory\n");
        exit(2);
    }

    for (i = 0; i < L; i++)
    {
        data[i] = dbuf + (size_t)i * s;
    }
    for (i = 0; i < P; i++)
    {
        parity[i] = pbuf + (size_t)i * s;
    }
    for (i = 0; i < L * (unsigned)s; i++)
    {
        dbuf[i] = (uint8_t)rnd64();
    }

    (void)rlnc_encode(ctx, data, L, parity);

    for (;;)
    {
        double t0 = now_sec();
        double t1;

        for (i = 0; i < iters; i++)
        {
            (void)rlnc_encode(ctx, data, L, parity);
        }
        t1 = now_sec();
        if (t1 - t0 >= 0.25 || iters >= (1u << 24))
        {
            break;
        }
        iters *= 2;
    }

    for (i = 0; i < 3; i++)
    {
        unsigned j;
        double t0 = now_sec();
        double t1;

        for (j = 0; j < iters; j++)
        {
            (void)rlnc_encode(ctx, data, L, parity);
        }
        t1 = now_sec();
        rates[i] = (double)iters * app_bytes / (t1 - t0) / 1e6;
    }

    best = rates[0];
    if (rates[1] > best)
    {
        best = rates[1];
    }
    if (rates[2] > best)
    {
        best = rates[2];
    }

    free(dbuf);
    free(pbuf);
    free(data);
    free(parity);
    rlnc_free(ctx);

    return best;
}

/*
 * Direct check of the coefficient generator: how many byte-distinct parity
 * packets does one coding group produce?  With d = W every parity row is the
 * XOR of the whole window, so all M rows are identical and the group has
 * rank one.
 */
static void parity_distinct(const struct cfg *c)
{
    unsigned L = c->l;
    unsigned P = parity_count(c);
    size_t s = 16;
    struct rlnc_ctx *ctx = rlnc_new(c->n, c->m, c->k, c->w, s, c->d,
                                    cfg_nonce(c));
    uint8_t *dbuf;
    uint8_t *pbuf;
    uint8_t **data;
    uint8_t **parity;
    unsigned i;
    unsigned g;

    if (!ctx)
    {
        fprintf(stderr, "sim_hnd0: ctx alloc failed\n");
        exit(2);
    }

    dbuf = malloc((size_t)L * s);
    pbuf = malloc((size_t)P * s);
    data = malloc((size_t)L * sizeof(*data));
    parity = malloc((size_t)P * sizeof(*parity));
    if (!dbuf || !pbuf || !data || !parity)
    {
        fprintf(stderr, "sim_hnd0: out of memory\n");
        exit(2);
    }

    for (i = 0; i < L; i++)
    {
        data[i] = dbuf + (size_t)i * s;
    }
    for (i = 0; i < P; i++)
    {
        parity[i] = pbuf + (size_t)i * s;
    }
    for (i = 0; i < L * (unsigned)s; i++)
    {
        dbuf[i] = (uint8_t)rnd64();
    }
    (void)rlnc_encode(ctx, data, L, parity);

    for (g = 0; g < group_count(c); g++)
    {
        unsigned distinct = 0;
        unsigned r;

        for (r = 0; r < c->m; r++)
        {
            unsigned q;
            int seen = 0;

            for (q = 0; q < r; q++)
            {
                if (memcmp(parity[(size_t)g * c->m + r],
                           parity[(size_t)g * c->m + q], s) == 0)
                {
                    seen = 1;
                    break;
                }
            }
            if (!seen)
            {
                distinct++;
            }
        }

        printf("%s,%u,%u,%u\n", c->name, g, c->m, distinct);
    }

    free(dbuf);
    free(pbuf);
    free(data);
    free(parity);
    rlnc_free(ctx);
}

static void print_host(void)
{
    char model[160] = "unknown";
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

    if (f)
    {
        while (fgets(line, sizeof(line), f))
        {
            if (strncmp(line, "model name", 10) == 0)
            {
                char *p = strchr(line, ':');

                if (p)
                {
                    size_t n;

                    p++;
                    while (*p == ' ' || *p == '\t')
                    {
                        p++;
                    }
                    snprintf(model, sizeof(model), "%s", p);
                    n = strlen(model);
                    while (n > 0 && (model[n - 1] == '\n' ||
                                     model[n - 1] == '\r'))
                    {
                        model[--n] = '\0';
                    }
                    break;
                }
            }
        }
        fclose(f);
    }

    {
        char *p = model;

        while ((p = strchr(p, ',')) != NULL)
        {
            *p = ';';
        }
    }

    printf("# host,nproc,%ld,cpu_model,%s\n", ncpu, model);
}

int main(void)
{
    unsigned ci;
    unsigned pi;
    unsigned k;

    print_host();

    printf("# section,parity_distinct\n");
    printf("cfg,group,m,distinct_rows\n");
    for (ci = 0; ci < N_CFG; ci++)
    {
        parity_distinct(&cfgs[ci]);
    }

    printf("# section,recovery\n");
    printf("cfg,kind,n,m,k,w,d,L,p,trials,recovered,r,ci99_half,r_loss,"
           "lat_mean_pkts,lat_max_pkts,rank_fail,corrupt\n");

    for (ci = 0; ci < N_CFG; ci++)
    {
        for (pi = 0; pi < P_MAX; pi++)
        {
            struct point pt = run_point(&cfgs[ci], pi, ps[pi], TRIALS);

            res[ci][pi] = pt;
            printf("%s,%s,%u,%u,%u,%u,%u,%u,%.2f,%u,%u,%.6f,%.6f,%.6f,"
                   "%.3f,%.0f,%u,%u\n",
                   cfgs[ci].name, cfgs[ci].kind, cfgs[ci].n, cfgs[ci].m,
                   cfgs[ci].k, cfgs[ci].w, cfgs[ci].d, cfgs[ci].l, ps[pi],
                   TRIALS, pt.recovered, pt.r, pt.ci_half, pt.r_loss,
                   pt.lat_mean, pt.lat_max, pt.rank_fail, pt.corrupt);
        }
    }

    /*
     * Derived joint success: 8 and 32 independent groups in the same window.
     * This is r^g computed from the measured per-group r, not a measurement.
     */
    printf("# section,joint_derived\n");
    printf("cfg,kind,p,r,r8_derived,r32_derived\n");
    for (ci = 0; ci < N_CFG; ci++)
    {
        for (pi = 0; pi < P_MAX; pi++)
        {
            double r8 = 1.0;
            double r32 = 1.0;
            unsigned j;

            for (j = 0; j < 8; j++)
            {
                r8 *= res[ci][pi].r;
            }
            for (j = 0; j < 32; j++)
            {
                r32 *= res[ci][pi].r;
            }

            printf("%s,%s,%.2f,%.6f,%.6f,%.6f\n", cfgs[ci].name,
                   cfgs[ci].kind, ps[pi], res[ci][pi].r, r8, r32);
        }
    }

    /*
     * Encode throughput at S = 8192, app bytes per second.  CPU% is derived
     * for 1 Gbit/s (125 MB/s), 750 Mbit/s (93.75 MB/s) and 500 Mbit/s
     * (62.5 MB/s) of application payload.
     */
    printf("# section,encode\n");
    printf("cfg,kind,n,m,k,w,d,L,S,app_MBps,xor_bytes_per_call,"
           "xor_GBps,cpu_pct_1G,cpu_pct_750M,cpu_pct_500M\n");
    for (ci = 0; ci < N_CFG; ci++)
    {
        double mbps = bench_encode(&cfgs[ci], BENCH_S);
        size_t xb = xor_bytes_per_call(&cfgs[ci], BENCH_S);
        double xgb = mbps * (double)xb / ((double)cfgs[ci].l * BENCH_S) / 1000.0;

        printf("%s,%s,%u,%u,%u,%u,%u,%u,%u,%.1f,%zu,%.3f,%.2f,%.2f,%.2f\n",
               cfgs[ci].name, cfgs[ci].kind, cfgs[ci].n, cfgs[ci].m,
               cfgs[ci].k, cfgs[ci].w, cfgs[ci].d, cfgs[ci].l, BENCH_S,
               mbps, xb, xgb, 125.0 / mbps * 100.0, 93.75 / mbps * 100.0,
               62.5 / mbps * 100.0);
    }

    /*
     * Per-window buffer cost with the production payload size.  data is
     * L*S, parity is G*M*S, ws is rlnc_memory_bytes(ctx, L).
     */
    printf("# section,memory\n");
    printf("cfg,kind,n,m,k,w,d,L,scope_groups,data_bytes,parity_bytes,"
           "ws_bytes,per_group_bytes,group_buf_total\n");
    for (ci = 0; ci < N_CFG; ci++)
    {
        struct rlnc_ctx *ctx =
            rlnc_new(cfgs[ci].n, cfgs[ci].m, cfgs[ci].k, cfgs[ci].w,
                     BENCH_S, cfgs[ci].d, cfg_nonce(&cfgs[ci]));
        size_t ws;
        size_t data_bytes;
        size_t parity_bytes;
        size_t per_group;
        unsigned scope = strcmp(cfgs[ci].kind, "ctrl") == 0 ? 1u : 32u;

        if (!ctx)
        {
            fprintf(stderr, "sim_hnd0: ctx alloc failed\n");
            return 2;
        }

        ws = rlnc_memory_bytes(ctx, cfgs[ci].l);
        data_bytes = (size_t)cfgs[ci].l * BENCH_S;
        parity_bytes = (size_t)parity_count(&cfgs[ci]) * BENCH_S;
        per_group = data_bytes + parity_bytes + ws;

        printf("%s,%s,%u,%u,%u,%u,%u,%u,%u,%zu,%zu,%zu,%zu,%zu\n",
               cfgs[ci].name, cfgs[ci].kind, cfgs[ci].n, cfgs[ci].m,
               cfgs[ci].k, cfgs[ci].w, cfgs[ci].d, cfgs[ci].l, scope,
               data_bytes, parity_bytes, ws, per_group,
               per_group * (size_t)scope);
        rlnc_free(ctx);
    }

    /*
     * Send byte window.  W_send is a packet count on the session, not the
     * coding window W.  BDP is rate * 218 ms one way, matching the WAN
     * measurements in review/wan-experiment.
     */
    printf("# section,window\n");
    printf("scenario,rate_Mbps,cfg,n,m,k,bfdp_bytes,kfac,wsend_pkts,"
           "wsend_bytes,wsend_bytes_both_sides\n");
    {
        static const char *scen[3] = {"1G_aggregate", "500M_aggregate",
                                      "34M_single_flow"};
        static const double rate_mbps[3] = {1000.0, 500.0, 34.0};

        for (ci = 0; ci < N_CFG; ci++)
        {
            unsigned si;

            if (strcmp(cfgs[ci].kind, "prod") != 0)
            {
                continue;
            }

            for (si = 0; si < 3; si++)
            {
                double bdp = rate_mbps[si] * 1e6 / 8.0 * 0.218;
                double inflate = (double)(cfgs[ci].k + cfgs[ci].m) /
                                 (double)cfgs[ci].k;

                for (k = 2; k <= 5; k++)
                {
                    double raw = (double)k * bdp / ((double)BENCH_S * inflate);
                    unsigned wsend = (unsigned)raw +
                                     (raw > (double)(unsigned)raw ? 1u : 0u);
                    size_t bytes = (size_t)wsend * BENCH_S;

                    printf("%s,%.0f,%s,%u,%u,%u,%.0f,%u,%u,%zu,%zu\n",
                           scen[si], rate_mbps[si], cfgs[ci].name, cfgs[ci].n,
                           cfgs[ci].m, cfgs[ci].k, bdp, k, wsend, bytes,
                           bytes * 2);
                }
            }
        }
    }

    return 0;
}
