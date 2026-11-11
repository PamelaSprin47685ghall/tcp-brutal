/* calib_sclc_down.c - recovery calibration for the shipped 1x256 downlink.
 *
 * The codec under test is tbtun/tbt_fec_sclc.c, compiled unchanged. One
 * trial is one 2 MiB FEC block: 256 shares of 8192 bytes, n data shares and
 * m = 256 - n parity shares, 128 batches of 64 codewords. A block succeeds
 * when every data share is known after one decode, which is the delivery
 * condition in tbt_fec_data.c.
 *
 * Scenarios (first argument):
 *   iid     share erasures i.i.d. at p
 *   first   exactly n+8 present shares, the 0 ms wake line
 *   ks      first decodable prefix in a random arrival order
 *   ge      Gilbert-Elliott bursts along the carrier axis
 *   dead    k carriers dead for the whole block
 *   minstop greedy shrink of a failing mask to a stopping set
 *   batch   check that the 128 batches fail and succeed together
 *
 * Everything is deterministic: the RNG seed is derived from the scenario,
 * rung, p, burst/k and the seed_base argument, so a row does not depend on
 * how many other rows run in parallel. CALIB_NONCE overrides the codec
 * nonce, which selects one graph instance.
 */
#include "tbt_fec_sclc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NSHARES TBT_FEC_MAX_SHARES /* 256 */
#define SHARE TBT_SCLC_SHARE       /* 8192 */
#define WAKE_EXTRA 8u              /* TBT_FEC_SCLC_WAKE_EXTRA_DOWN */

struct rung
{
    int pct;
    int n;
    int m;
};

static const struct rung rungs[] = {
    {25, 64, 192}, {30, 76, 180}, {40, 102, 154},
    {50, 128, 128}, {60, 151, 105}, {70, 178, 78},
};

enum scenario
{
    SC_IID = 1,
    SC_FIRST,
    SC_KS,
    SC_GE,
    SC_DEAD,
    SC_MINSTOP,
    SC_BATCH,
};

static void die(const char *what)
{
    fprintf(stderr, "calib_sclc_down: %s\n", what);
    exit(2);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: calib_sclc_down <scenario> <pct> <p_ppm> <trials> "
            "<seed_base> [burst|k] [nonce]\n"
            "       calib_sclc_down --header\n"
            "scenario: iid first ks ge dead minstop batch\n"
            "pct: 25 30 40 50 60 70\n"
            "CALIB_NONCE also overrides the codec nonce.\n");
    exit(2);
}

/* ------------------------------------------------------------------ *
 * Deterministic RNG: splitmix64, as the codec's own seeding.
 * ------------------------------------------------------------------ */

struct rng
{
    u64 s;
};

static u64 mix64(u64 x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static u64 rng_next(struct rng *r)
{
    return mix64(r->s++);
}

static u32 rng_below(struct rng *r, u32 n)
{
    return (u32)(((rng_next(r) >> 32) * (u64)n) >> 32);
}

/* ------------------------------------------------------------------ *
 * Codec instance: real encode and decode, 2 MiB of share buffers.
 * ------------------------------------------------------------------ */

struct cal
{
    const struct tbt_fec_ops *ops;
    void *c;
    u8 *data, *parity;
    u8 **dp, **pp;
    u8 *dpres, *ppres;
    int n, m;
    u64 ns;
    u64 calls;
};

static void cal_init(struct cal *k, const struct rung *r, u64 nonce)
{
    struct rng rng = {mix64(nonce ^ 0x5C1C0DEC0FFEEull)};
    int i;

    memset(k, 0, sizeof(*k));
    k->ops = &tbt_fec_ops_sclc;
    k->n = r->n;
    k->m = r->m;
    k->c = k->ops->new((u16)r->n, (u16)r->m, 0, TBT_SCLC_BLOCK, SHARE, 0,
                       nonce);
    if (!k->c)
        die("codec new failed");
    k->data = calloc((size_t)r->n, SHARE);
    k->parity = calloc((size_t)r->m, SHARE);
    k->dp = calloc((size_t)r->n, sizeof(*k->dp));
    k->pp = calloc((size_t)r->m, sizeof(*k->pp));
    k->dpres = calloc((size_t)r->n, 1);
    k->ppres = calloc((size_t)r->m, 1);
    if (!k->data || !k->parity || !k->dp || !k->pp || !k->dpres ||
        !k->ppres)
        die("allocation failed");
    for (i = 0; i < r->n; i++)
    {
        size_t j;

        k->dp[i] = k->data + (size_t)i * SHARE;
        for (j = 0; j < SHARE; j++)
            k->dp[i][j] = (u8)(rng_next(&rng) >> 24);
    }
    for (i = 0; i < r->m; i++)
        k->pp[i] = k->parity + (size_t)i * SHARE;
    if (k->ops->encode(k->c, k->dp, (u32)r->n, k->pp) != 0)
        die("encode failed");
}

static void cal_free(struct cal *k)
{
    k->ops->free(k->c);
    free(k->data);
    free(k->parity);
    free(k->dp);
    free(k->pp);
    free(k->dpres);
    free(k->ppres);
}

static u64 now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

/* mask[i] = 1 when share i arrived; 0..n-1 are data, n..n+m-1 parity.
 * Returns 1 when every data share is known afterwards. */
static int cal_decode(struct cal *k, const u8 *mask)
{
    u64 t0, t1;
    int i, missing = 0, rc;

    for (i = 0; i < k->n; i++)
    {
        k->dpres[i] = mask[i];
        if (!mask[i])
            missing++;
    }
    for (i = 0; i < k->m; i++)
        k->ppres[i] = mask[k->n + i];

    t0 = now_ns();
    rc = k->ops->decode(k->c, k->dp, k->pp, k->dpres, k->ppres,
                        (u32)k->n);
    t1 = now_ns();
    k->ns += t1 - t0;
    k->calls++;
    if (rc < 0)
        return 0;
    return rc == missing;
}

/* ------------------------------------------------------------------ *
 * Mask helpers.
 * ------------------------------------------------------------------ */

static void mask_iid(struct rng *r, u8 *mask, u32 p_ppm)
{
    int i;

    for (i = 0; i < NSHARES; i++)
        mask[i] = rng_below(r, 1000000u) >= p_ppm;
}

static void identity(int *a)
{
    int i;

    for (i = 0; i < NSHARES; i++)
        a[i] = i;
}

static void shuffle_prefix(struct rng *r, int *a, int k)
{
    int i;

    for (i = 0; i < k; i++)
    {
        int j = i + (int)rng_below(r, (u32)(NSHARES - i));
        int t = a[i];

        a[i] = a[j];
        a[j] = t;
    }
}

/* ------------------------------------------------------------------ *
 * Statistics.
 * ------------------------------------------------------------------ */

static double sqrt_newton(double x)
{
    double r = x > 1.0 ? x : 1.0;
    int i;

    for (i = 0; i < 40; i++)
        r = 0.5 * (r + x / r);
    return r;
}

static void wilson(u64 k, u64 n, double *lo, double *hi)
{
    const double z = 1.96;
    double p, d, c, s;

    if (!n)
    {
        *lo = 0.0;
        *hi = 0.0;
        return;
    }
    p = (double)k / (double)n;
    d = 1.0 + z * z / (double)n;
    c = p + z * z / (2.0 * (double)n);
    s = z * sqrt_newton(p * (1.0 - p) / (double)n +
                        z * z / (4.0 * (double)n * (double)n));
    *lo = (c - s) / d;
    *hi = (c + s) / d;
    if (*lo < 0.0)
        *lo = 0.0;
    if (*hi > 1.0)
        *hi = 1.0;
}

/* P(X >= k) for X ~ Bin(n, p). n is 256, so the direct recurrence is exact
 * enough and keeps the program free of libm. */
static double binom_tail(int n, double p, int k)
{
    long double pmf = 1.0L, sum = 0.0L;
    int i;

    for (i = 0; i < n; i++)
        pmf *= (long double)(1.0 - p);
    for (i = 0; i < n; i++)
    {
        if (i >= k)
            sum += pmf;
        pmf *= (long double)(n - i) / (long double)(i + 1) *
               (long double)p / (long double)(1.0 - p);
    }
    if (n >= k)
        sum += pmf;
    if (sum < 0.0L)
        sum = 0.0L;
    if (sum > 1.0L)
        sum = 1.0L;
    return (double)sum;
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;

    return x < y ? -1 : x > y;
}

/* ------------------------------------------------------------------ *
 * Scenarios.
 * ------------------------------------------------------------------ */

struct result
{
    u64 events;
    u64 trials;
    u64 decodes;
    char detail[320];
};

static void run_iid(struct cal *k, const struct rung *r, u32 p_ppm,
                    u64 trials, struct rng *rng, struct result *out)
{
    u8 mask[NSHARES];
    u64 t;

    (void)r;
    memset(out, 0, sizeof(*out));
    out->trials = trials;
    for (t = 0; t < trials; t++)
    {
        mask_iid(rng, mask, p_ppm);
        if (!cal_decode(k, mask))
            out->events++;
    }
    out->decodes = k->calls;
    snprintf(out->detail, sizeof(out->detail), "model=iid");
}

static void run_first(struct cal *k, const struct rung *r, u32 p_ppm,
                      u64 trials, struct rng *rng, struct result *out)
{
    u8 mask[NSHARES];
    int idx[NSHARES];
    int k0 = r->n + (int)WAKE_EXTRA;
    double attempt;
    u64 t;

    memset(out, 0, sizeof(*out));
    out->trials = trials;
    for (t = 0; t < trials; t++)
    {
        int i;

        identity(idx);
        shuffle_prefix(rng, idx, k0);
        memset(mask, 0, sizeof(mask));
        for (i = 0; i < k0; i++)
            mask[idx[i]] = 1;
        if (!cal_decode(k, mask))
            out->events++;
    }
    out->decodes = k->calls;
    attempt = binom_tail(NSHARES, 1.0 - (double)p_ppm / 1000000.0, k0);
    snprintf(out->detail, sizeof(out->detail),
             "model=first;present=%d;attempt_rate=%.6f;uncond_fail=%.6f",
             k0, attempt, attempt * (double)out->events / (double)trials);
}

/* Returns the number of present shares needed for the first success, or
 * -1 when even the whole present set fails. j is a prefix of present[]. */
static int first_success(struct cal *k, const int *present, int npresent,
                         u8 *mask)
{
    int lo, hi;

    if (!npresent)
        return -1;
    memset(mask, 0, NSHARES);
    {
        int i;

        for (i = 0; i < npresent; i++)
            mask[present[i]] = 1;
    }
    if (!cal_decode(k, mask))
        return -1;
    lo = 1;
    hi = npresent;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        int i;

        memset(mask, 0, NSHARES);
        for (i = 0; i < mid; i++)
            mask[present[i]] = 1;
        if (cal_decode(k, mask))
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

static void run_ks(struct cal *k, const struct rung *r, u32 p_ppm,
                   u64 trials, struct rng *rng, struct result *out)
{
    u8 mask[NSHARES];
    int idx[NSHARES], present[NSHARES];
    int *ks;
    u64 t, never = 0, no_attempt = 0, first_fail = 0, attempts = 0;
    int k0 = r->n + (int)WAKE_EXTRA;
    u64 finite = 0;

    memset(out, 0, sizeof(*out));
    out->trials = trials;
    ks = malloc((size_t)trials * sizeof(*ks));
    if (!ks)
        die("allocation failed");
    for (t = 0; t < trials; t++)
    {
        int np = 0, i, star;

        mask_iid(rng, mask, p_ppm);
        identity(idx);
        {
            int j;

            for (j = NSHARES - 1; j > 0; j--)
            {
                int q = (int)rng_below(rng, (u32)(j + 1));
                int tmp = idx[j];

                idx[j] = idx[q];
                idx[q] = tmp;
            }
        }
        for (i = 0; i < NSHARES; i++)
            if (mask[idx[i]])
                present[np++] = idx[i];
        if (np < k0)
            no_attempt++;
        star = first_success(k, present, np, mask);
        if (star < 0)
        {
            never++;
            ks[t] = -1;
            continue;
        }
        finite++;
        ks[t] = star;
        if (np >= k0)
        {
            if (star > k0)
            {
                first_fail++;
                attempts += (u64)(star - k0 + 1);
            }
            else
                attempts++;
        }
    }
    out->events = finite;
    out->decodes = k->calls;
    qsort(ks, (size_t)trials, sizeof(*ks), cmp_int);
    {
        u64 seen = 0;
        double mean = 0.0;
        int p10 = 0, p50 = 0, p90 = 0, p99 = 0;

        for (t = 0; t < trials; t++)
            if (ks[t] >= 0)
            {
                mean += ks[t];
                seen++;
            }
        mean = seen ? mean / (double)seen : 0.0;
        if (seen)
        {
            u64 base = trials - seen; /* -1 sorts first */

            p10 = ks[base + (u64)(0.10 * (double)(seen - 1))];
            p50 = ks[base + (u64)(0.50 * (double)(seen - 1))];
            p90 = ks[base + (u64)(0.90 * (double)(seen - 1))];
            p99 = ks[base + (u64)(0.99 * (double)(seen - 1))];
        }
        snprintf(out->detail, sizeof(out->detail),
                 "model=ks;kstar_p10=%d;kstar_p50=%d;kstar_p90=%d;"
                 "kstar_p99=%d;kstar_mean=%.2f;never=%.6f;"
                 "no_attempt=%.6f;first_fail=%.6f;attempts_mean=%.2f",
                 p10, p50, p90, p99, mean,
                 (double)never / (double)trials,
                 (double)no_attempt / (double)trials,
                 (double)first_fail / (double)trials,
                 (double)attempts / (double)trials);
    }
    free(ks);
}

static void run_ge(struct cal *k, const struct rung *r, u32 p_ppm,
                   u64 trials, u32 burst, struct rng *rng,
                   struct result *out)
{
    u8 mask[NSHARES];
    u64 t;
    u64 r_gb; /* good -> bad, ppm */

    (void)r;
    memset(out, 0, sizeof(*out));
    out->trials = trials;
    if (p_ppm >= 1000000u || burst < 1)
        die("ge needs p < 1 and burst >= 1");
    r_gb = (u64)p_ppm * 1000000ull /
           ((u64)burst * (1000000ull - p_ppm));
    if (r_gb > 1000000ull)
        die("ge transition above 1");
    for (t = 0; t < trials; t++)
    {
        int i, bad = rng_below(rng, 1000000u) < p_ppm;

        for (i = 0; i < NSHARES; i++)
        {
            mask[i] = (u8)bad;
            if (bad)
                bad = rng_below(rng, burst) != 0;
            else
                bad = rng_below(rng, 1000000u) < (u32)r_gb;
        }
        if (!cal_decode(k, mask))
            out->events++;
    }
    out->decodes = k->calls;
    snprintf(out->detail, sizeof(out->detail),
             "model=ge;burst=%u;r_gb_ppm=%llu", burst,
             (unsigned long long)r_gb);
}

static void run_dead(struct cal *k, const struct rung *r, u32 kdead,
                     u64 trials, struct rng *rng, struct result *out)
{
    u8 mask[NSHARES];
    int idx[NSHARES];
    u64 t, data_dead = 0;

    (void)r;
    memset(out, 0, sizeof(*out));
    out->trials = trials;
    if (kdead > NSHARES)
        die("dead count above 256");
    for (t = 0; t < trials; t++)
    {
        u32 i;

        identity(idx);
        shuffle_prefix(rng, idx, (int)kdead);
        memset(mask, 1, sizeof(mask));
        for (i = 0; i < kdead; i++)
        {
            mask[idx[i]] = 0;
            if (idx[i] < k->n)
                data_dead++;
        }
        if (!cal_decode(k, mask))
            out->events++;
    }
    out->decodes = k->calls;
    snprintf(out->detail, sizeof(out->detail),
             "model=dead;k=%u;data_dead_mean=%.2f", kdead,
             trials ? (double)data_dead / (double)trials : 0.0);
}

static void shuffle_ints(struct rng *r, int *a, int n)
{
    int i;

    for (i = n - 1; i > 0; i--)
    {
        int j = (int)rng_below(r, (u32)(i + 1));
        int t = a[i];

        a[i] = a[j];
        a[j] = t;
    }
}

/* Greedy shrink: drop missing shares while the block still fails. The
 * result is inclusion-minimal for the graph, though not necessarily the
 * smallest stopping set in the code. */
static int shrink(struct cal *k, struct rng *rng, u8 *mask, int *set,
                  int nset, u8 *probe)
{
    int changed = 1, size = nset;

    while (changed)
    {
        int i;

        changed = 0;
        shuffle_ints(rng, set, size);
        for (i = 0; i < size; i++)
        {
            memcpy(probe, mask, NSHARES);
            probe[set[i]] = 1;
            if (!cal_decode(k, probe))
            {
                mask[set[i]] = 1;
                set[i] = set[--size];
                i--;
                changed = 1;
            }
        }
    }
    return size;
}

static void run_minstop(struct cal *k, const struct rung *r, u32 p_ppm,
                        u64 attempts, struct rng *rng, struct result *out)
{
    u8 mask[NSHARES], probe[NSHARES];
    int set[NSHARES];
    u64 done = 0;
    int best = NSHARES + 1, best_data = 0, best_par = 0;
    int best_set[NSHARES];
    double sum = 0.0;
    char list[128];
    size_t off = 0;

    (void)r;
    memset(out, 0, sizeof(*out));
    list[0] = '\0';
    out->trials = attempts;
    while (done < attempts)
    {
        int nset = 0, i, size;

        mask_iid(rng, mask, p_ppm);
        if (cal_decode(k, mask))
            continue;
        for (i = 0; i < NSHARES; i++)
            if (!mask[i])
                set[nset++] = i;
        size = shrink(k, rng, mask, set, nset, probe);
        sum += size;
        if (size < best)
        {
            best = size;
            best_data = 0;
            best_par = 0;
            for (i = 0; i < size; i++)
            {
                best_set[i] = set[i];
                if (set[i] < k->n)
                    best_data++;
                else
                    best_par++;
            }
        }
        done++;
    }
    out->events = (u64)best;
    out->decodes = k->calls;
    if (best <= NSHARES)
    {
        int i;

        for (i = 0; i < best && off + 5 < sizeof(list); i++)
            off += (size_t)snprintf(list + off, sizeof(list) - off, "%s%d",
                                    i ? "," : "", best_set[i]);
    }
    snprintf(out->detail, sizeof(out->detail),
             "model=minstop;min=%d;mean=%.2f;min_data=%d;min_parity=%d;"
             "set=%s",
             best, attempts ? sum / (double)attempts : 0.0, best_data,
             best_par, list);
}

/* A share is recovered per batch and published only when all 128 batches
 * decode, so a failing block whose absent shares carry any written byte
 * proves that some batches decoded while the block failed. With the same
 * mask and the same graph in every batch that must never happen. */
static void run_batch(struct cal *k, const struct rung *r, u32 p_ppm,
                      u64 trials, struct rng *rng, struct result *out)
{
    u8 mask[NSHARES];
    u64 fails = 0, attempts = 0, inconsistent = 0, changed_total = 0;
    u64 cap = trials * 100u + 100u;

    (void)r;
    memset(out, 0, sizeof(*out));
    while (fails < trials && attempts < cap)
    {
        int i, missing = 0, changed = 0;

        attempts++;
        mask_iid(rng, mask, p_ppm);
        for (i = 0; i < k->n; i++)
            if (!mask[i])
                missing++;
        if (!missing)
            continue;
        for (i = 0; i < k->n; i++)
            if (!mask[i])
                memset(k->dp[i], 0x5A, SHARE);
        if (cal_decode(k, mask))
            continue;
        fails++;
        for (i = 0; i < k->n; i++)
        {
            size_t j;

            if (mask[i])
                continue;
            for (j = 0; j < SHARE; j++)
                if (k->dp[i][j] != 0x5A)
                {
                    changed++;
                    break;
                }
        }
        if (changed)
        {
            inconsistent++;
            changed_total += (u64)changed;
        }
    }
    out->trials = fails;
    out->events = inconsistent;
    out->decodes = k->calls;
    snprintf(out->detail, sizeof(out->detail),
             "model=batch;failing=%llu;attempts=%llu;changed_shares=%llu",
             (unsigned long long)fails, (unsigned long long)attempts,
             (unsigned long long)changed_total);
}

/* ------------------------------------------------------------------ *
 * Main.
 * ------------------------------------------------------------------ */

static const struct rung *find_rung(unsigned pct)
{
    size_t i;

    for (i = 0; i < sizeof(rungs) / sizeof(rungs[0]); i++)
        if (rungs[i].pct == (int)pct)
            return &rungs[i];
    return NULL;
}

static u64 parse_u64(const char *s, const char *what)
{
    char *end = NULL;
    unsigned long long v;

    if (!s || !*s)
        die(what);
    v = strtoull(s, &end, 0);
    if (!end || *end)
        die(what);
    return (u64)v;
}

static void print_header(void)
{
    printf("scenario,rung,pct,n,m,p_ppm,p_frac,burst,trials,events,rate,"
           "ci_lo,ci_hi,ns_per_decode,detail\n");
}

int main(int argc, char **argv)
{
    const struct rung *r;
    struct cal k;
    struct result out;
    struct rng rng;
    enum scenario sc;
    u32 p_ppm, extra = 0;
    u64 trials, seed_base, nonce;
    double lo, hi;
    const char *env;

    if (argc == 2 && !strcmp(argv[1], "--header"))
    {
        print_header();
        return 0;
    }
    if (argc < 6)
        usage();
    if (!strcmp(argv[1], "iid"))
        sc = SC_IID;
    else if (!strcmp(argv[1], "first"))
        sc = SC_FIRST;
    else if (!strcmp(argv[1], "ks"))
        sc = SC_KS;
    else if (!strcmp(argv[1], "ge"))
        sc = SC_GE;
    else if (!strcmp(argv[1], "dead"))
        sc = SC_DEAD;
    else if (!strcmp(argv[1], "minstop"))
        sc = SC_MINSTOP;
    else if (!strcmp(argv[1], "batch"))
        sc = SC_BATCH;
    else
        usage();

    r = find_rung((unsigned)parse_u64(argv[2], "bad rung"));
    if (!r)
        usage();
    p_ppm = (u32)parse_u64(argv[3], "bad p_ppm");
    trials = parse_u64(argv[4], "bad trials");
    seed_base = parse_u64(argv[5], "bad seed");
    if (argc > 6)
        extra = (u32)parse_u64(argv[6], "bad burst/k");
    if (p_ppm > 1000000u)
        die("p_ppm above 1e6");
    if (!trials)
        die("trials is zero");

    nonce = 0xD0u + (u64)r->pct;
    env = getenv("CALIB_NONCE");
    if (env && *env)
        nonce = parse_u64(env, "bad CALIB_NONCE");
    else if (argc > 7)
        nonce = parse_u64(argv[7], "bad nonce");

    rng.s = mix64(seed_base ^ ((u64)sc << 56) ^ ((u64)r->pct << 40) ^
                  ((u64)p_ppm << 16) ^ ((u64)extra * 0x9E3779B1ull) ^
                  nonce);

    cal_init(&k, r, nonce);
    switch (sc)
    {
    case SC_IID:
        run_iid(&k, r, p_ppm, trials, &rng, &out);
        break;
    case SC_FIRST:
        run_first(&k, r, p_ppm, trials, &rng, &out);
        break;
    case SC_KS:
        run_ks(&k, r, p_ppm, trials, &rng, &out);
        break;
    case SC_GE:
        if (!extra)
            usage();
        run_ge(&k, r, p_ppm, trials, extra, &rng, &out);
        break;
    case SC_DEAD:
        run_dead(&k, r, extra, trials, &rng, &out);
        break;
    case SC_MINSTOP:
        run_minstop(&k, r, p_ppm, trials, &rng, &out);
        break;
    case SC_BATCH:
        run_batch(&k, r, p_ppm, trials, &rng, &out);
        break;
    default:
        usage();
    }
    wilson(out.events, out.trials, &lo, &hi);
    printf("%s,%d,%d,%d,%u,%.6f,%u,%llu,%llu,%.8f,%.8f,%.8f,%.1f,\"%s\"\n",
           sc == SC_IID ? "iid" :
           sc == SC_FIRST ? "first" :
           sc == SC_KS ? "ks" :
           sc == SC_GE ? "ge" :
           sc == SC_DEAD ? "dead" :
           sc == SC_BATCH ? "batch" : "minstop",
           r->pct, r->n, r->m, p_ppm, (double)p_ppm / 1000000.0, extra,
           (unsigned long long)out.trials,
           (unsigned long long)out.events, out.trials ?
               (double)out.events / (double)out.trials : 0.0,
           lo, hi, out.decodes ? (double)k.ns / (double)out.decodes : 0.0,
           out.detail);
    cal_free(&k);
    return 0;
}
