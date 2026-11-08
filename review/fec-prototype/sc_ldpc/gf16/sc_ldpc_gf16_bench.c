/*
 * sc_ldpc_gf16_bench.c - harness for the GF(16) SC-IRA prototype.
 *
 * One point:
 *   ./sc_ldpc_gf16_bench --block 256 --L 16 --rate 0.50 --p 0.30 \
 *       --trials 2000 --model symmetric
 *
 * The CSV row is:
 *   series,host,block,L,rate_target,rate_actual,p,model,trials,codes,seed,
 *   recovery,ci99,unresolved_mean,iter_mean,enc_mbps,dec_mbps,mem_bytes,
 *   n,n_data,n_chk,edges
 *
 * recovery requires every target variable to be known and correct.  With
 * --model symmetric the target is every variable; with --model systematic
 * it is the payload only.  MB/s is payload throughput: n_data symbols of
 * four bits each, so n_data/2 bytes per codeword.
 *
 * --payload-check runs a no-erasure round trip and exits.
 * --cross-check also decodes the same graph/mask with the binary decoder and
 * requires the two known-variable sets to be identical.
 */

#define _POSIX_C_SOURCE 200809L

#include "sc_ldpc.h"
#include "sc_ldpc_gf16.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double wilson99(double ph, long n, double *lo, double *hi)
{
    const double z = 2.5758293035489004;
    double den, center, half;
    if (n <= 0) {
        *lo = 0.0;
        *hi = 1.0;
        return 0.5;
    }
    den = 1.0 + z * z / (double)n;
    center = (ph + z * z / (2.0 * (double)n)) / den;
    half = z / den * sqrt(ph * (1.0 - ph) / (double)n +
                          z * z / (4.0 * (double)n * (double)n));
    *lo = center - half;
    *hi = center + half;
    if (*lo < 0.0)
        *lo = 0.0;
    if (*hi > 1.0)
        *hi = 1.0;
    return (double)(*hi - *lo) * 0.5;
}

/* xorshift128+ channel stream */
typedef struct {
    uint64_t s0, s1;
} crng;

static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void crng_seed(crng *r, uint64_t seed)
{
    uint64_t x = seed;
    r->s0 = splitmix64(&x);
    r->s1 = splitmix64(&x);
    if (r->s0 == 0 && r->s1 == 0)
        r->s0 = 1;
}

static uint64_t crng_next(crng *r)
{
    uint64_t x = r->s0;
    uint64_t const y = r->s1;
    r->s0 = y;
    x ^= x << 23;
    r->s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
    return r->s1 + y;
}

/* uniform in [0,1000000) */
static uint32_t crng_mil(crng *r)
{
    return (uint32_t)(((crng_next(r) >> 32) * 1000000ULL) >> 32);
}

static double median(double *v, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        double x = v[i];
        for (j = i - 1; j >= 0 && v[j] > x; j--)
            v[j + 1] = v[j];
        v[j + 1] = x;
    }
    return v[n / 2];
}

/* ------------------------------------------------------------------ */
/* one point                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    gf16_code *g;
    sc_code *b; /* binary reference for --cross-check, or NULL */
} code_pair;

typedef struct {
    int block, L, rate_pct, trials, codes, extra_dc, model, threads, runs;
    int core_max;
    int p_mil;
    double bench_sec;
    uint64_t seed;
    int no_header, cross_check, payload_check;
    const char *report;
    const char *series;
} opts;

typedef struct {
    gf16_code *c;
    const uint8_t *recv;
    const uint8_t *erased;
    uint8_t *out;
    int target_data_only;
    int max_core;
    double sec;
    double mbps;
    long reps;
} dec_job;

static void *dec_worker(void *arg)
{
    dec_job *j = arg;
    double bytes = (double)gf16_n_data(j->c) * 0.5;
    double start = now_sec();
    double end = start + j->sec;
    long reps = 0;
    uint32_t sink = 0;

    do {
        int i;
        for (i = 0; i < 8; i++) {
            (void)gf16_decode(j->c, j->recv, j->erased, j->out,
                              j->target_data_only, j->max_core);
            sink ^= j->out[0];
            reps++;
        }
    } while (now_sec() < end);

    {
        double elapsed = now_sec() - start;
        j->reps = reps;
        j->mbps = (double)reps * bytes / elapsed / 1e6;
    }
    if (sink == 0x12345678u)
        fprintf(stderr, "sink\n");
    return NULL;
}

static double measure_dec_mt(gf16_code **codes, int threads,
                             const uint8_t *recv, const uint8_t *erased,
                             int target_data_only, int max_core, double sec)
{
    pthread_t th[8];
    dec_job jobs[8];
    double total = 0.0;
    int i;

    if (threads < 1)
        threads = 1;
    if (threads > 8)
        threads = 8;
    for (i = 0; i < threads; i++) {
        jobs[i].c = codes[i];
        jobs[i].recv = recv;
        jobs[i].erased = erased;
        jobs[i].out = malloc((size_t)gf16_n(codes[i]));
        jobs[i].target_data_only = target_data_only;
        jobs[i].max_core = max_core;
        jobs[i].sec = sec;
        jobs[i].mbps = 0.0;
        jobs[i].reps = 0;
        if (!jobs[i].out) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        if (pthread_create(&th[i], NULL, dec_worker, &jobs[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }
    for (i = 0; i < threads; i++) {
        pthread_join(th[i], NULL);
        total += jobs[i].mbps;
        free(jobs[i].out);
    }
    return total;
}

static double measure_dec_median(gf16_code **codes, int threads,
                                 const uint8_t *recv, const uint8_t *erased,
                                 int target_data_only, int max_core, double sec,
                                 int runs)
{
    double v[16];
    int r;

    if (runs < 1)
        runs = 1;
    if (runs > 16)
        runs = 16;
    for (r = 0; r < runs; r++)
        v[r] = measure_dec_mt(codes, threads, recv, erased,
                              target_data_only, max_core, sec);
    return median(v, runs);
}

static double measure_enc_once(gf16_code *c, uint8_t *payload, double sec)
{
    double bytes = (double)gf16_n_data(c) * 0.5;
    double start = now_sec();
    double end = start + sec;
    long reps = 0;
    uint32_t sink = 0;

    do {
        int i;
        for (i = 0; i < 16; i++) {
            if (gf16_encode(c, payload) != 0) {
                fprintf(stderr, "gf16_encode failed\n");
                exit(1);
            }
            sink ^= payload[gf16_n_data(c)];
            reps++;
        }
    } while (now_sec() < end);

    {
        double elapsed = now_sec() - start;
        if (sink == 0x12345678u)
            fprintf(stderr, "sink\n");
        return (double)reps * bytes / elapsed / 1e6;
    }
}

static double measure_enc_median(gf16_code *c, uint8_t *payload, double sec,
                                 int runs)
{
    double v[16];
    int r;

    if (runs < 1)
        runs = 1;
    if (runs > 16)
        runs = 16;
    for (r = 0; r < runs; r++)
        v[r] = measure_enc_once(c, payload, sec);
    return median(v, runs);
}

static int build_pairs(const opts *o, code_pair *pairs, int ncodes,
                       sc_info *info_out, char *err, size_t errlen)
{
    int i;

    for (i = 0; i < ncodes; i++) {
        sc_params p;
        uint64_t coef_seed;
        sc_info gi;

        memset(&p, 0, sizeof(p));
        p.block = o->block;
        p.L = o->L;
        p.rate_pct = o->rate_pct;
        p.term = SC_TERM_ACC;
        p.extra_dc = o->extra_dc;
        p.seed = o->seed + (uint64_t)i * 7919ULL;

        coef_seed = o->seed ^ (0xC0FFEEULL + (uint64_t)i * 0x9E3779B9ULL);
        pairs[i].g = gf16_build(&p, coef_seed, err, errlen);
        if (!pairs[i].g)
            return -1;
        if (o->cross_check) {
            pairs[i].b = sc_build(&p, err, errlen);
            if (!pairs[i].b)
                return -1;
        }
        gf16_get_info(pairs[i].g, &gi);
        if (i == 0)
            *info_out = gi;
        if (gi.n != info_out->n || gi.n_data != info_out->n_data ||
            gi.n_chk != info_out->n_chk || gi.edges != info_out->edges) {
            snprintf(err, errlen, "code instances differ");
            return -1;
        }
    }
    return 0;
}

static void free_pairs(code_pair *pairs, int ncodes)
{
    int i;
    for (i = 0; i < ncodes; i++) {
        if (pairs[i].g)
            gf16_free(pairs[i].g);
        if (pairs[i].b)
            sc_free(pairs[i].b);
    }
}

static int payload_check(const opts *o, code_pair *pairs, int ncodes)
{
    int i;
    int bad = 0;

    (void)o;
    for (i = 0; i < ncodes; i++) {
        gf16_code *c = pairs[i].g;
        int n = gf16_n(c);
        int nd = gf16_n_data(c);
        uint8_t *x = calloc((size_t)n, 1);
        uint8_t *erased = calloc((size_t)n, 1);
        uint8_t *out = calloc((size_t)n, 1);
        long errors = 0;
        int v, verify;

        if (!x || !erased || !out) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        /* Deterministic payload: a simple xorshift on v is enough. */
        for (v = 0; v < nd; v++) {
            uint32_t z = (uint32_t)v * 2654435761u + 1u;
            z ^= z << 13;
            z ^= z >> 17;
            z ^= z << 5;
            x[v] = (uint8_t)(z & 15u);
        }
        if (gf16_encode(c, x) != 0) {
            fprintf(stderr, "payload_check code %d: encode failed\n", i);
            bad = 1;
        }
        verify = gf16_verify(c, x);
        if (verify != 0) {
            fprintf(stderr, "payload_check code %d: verify failed at %d\n",
                    i, verify);
            bad = 1;
        }
        (void)gf16_decode(c, x, erased, out, 0, 0);
        for (v = 0; v < n; v++)
            if (out[v] != x[v])
                errors++;
        printf("payload_check code %d: verify=%d errors=%ld n=%d n_data=%d\n",
               i, verify, errors, n, nd);
        if (errors != 0)
            bad = 1;
        free(x);
        free(erased);
        free(out);
    }
    return bad ? 1 : 0;
}

static int run_point(const opts *o, FILE *rep)
{
    int ncodes = o->codes > o->threads ? o->codes : o->threads;
    code_pair *pairs = calloc((size_t)ncodes, sizeof(*pairs));
    sc_info info;
    char err[256];
    crng ch;
    uint8_t *x = NULL, *recv = NULL, *erased = NULL, *out = NULL;
    uint8_t *recv_bin = NULL, *out_bin = NULL;
    uint8_t *enc_payload = NULL, *dec_payload = NULL, *dec_recv = NULL;
    uint8_t *dec_erased = NULL;
    gf16_code *dec_codes[8];
    long successes = 0, unresolved_sum = 0, rounds_sum = 0;
    long value_errors = 0;
    long core_calls = 0, core_solved = 0, core_failed = 0;
    long core_skipped = 0, core_max_vars = 0, core_vars_sum = 0;
    long core_gf2_def = 0, core_gf16_rank_sum = 0;
    long core_under = 0, core_rankdef = 0, core_bad = 0;
    long binary_only = 0, gf16_extra = 0;
    int trial, v, ti;
    double recovery, ci99, unresolved_mean, iter_mean;
    double enc_mbps, dec_mbps;
    char host[256];

    memset(&info, 0, sizeof(info));
    if (!pairs) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    if (build_pairs(o, pairs, ncodes, &info, err, sizeof(err)) != 0) {
        fprintf(stderr, "build failed: %s\n", err);
        free_pairs(pairs, ncodes);
        free(pairs);
        return 1;
    }
    if (o->payload_check) {
        int rc = payload_check(o, pairs, ncodes);
        free_pairs(pairs, ncodes);
        free(pairs);
        return rc;
    }

    x = calloc((size_t)info.n, 1);
    recv = calloc((size_t)info.n, 1);
    erased = calloc((size_t)info.n, 1);
    out = calloc((size_t)info.n, 1);
    recv_bin = calloc((size_t)info.n, 1);
    out_bin = calloc((size_t)info.n, 1);
    enc_payload = calloc((size_t)info.n, 1);
    dec_payload = calloc((size_t)info.n, 1);
    dec_recv = calloc((size_t)info.n, 1);
    dec_erased = calloc((size_t)info.n, 1);
    if (!x || !recv || !erased || !out || !recv_bin || !out_bin ||
        !enc_payload || !dec_payload || !dec_recv || !dec_erased) {
        fprintf(stderr, "out of memory\n");
        free_pairs(pairs, ncodes);
        free(pairs);
        return 1;
    }

    crng_seed(&ch, o->seed ^ 0x5DEECE66DULL);

    for (trial = 0; trial < o->trials; trial++) {
        gf16_code *c = pairs[trial % ncodes].g;
        gf16_dec_stat st;
        int nd = gf16_n_data(c);
        int n = gf16_n(c);
        int target = o->model == 1 ? nd : n;
        int ok = 1;

        for (v = 0; v < nd; v++)
            x[v] = (uint8_t)(crng_next(&ch) & 15u);
        for (v = nd; v < n; v++)
            x[v] = 0;
        if (gf16_encode(c, x) != 0) {
            fprintf(stderr, "encode failed\n");
            return 1;
        }
        if (trial < ncodes) {
            int verify = gf16_verify(c, x);
            if (verify != 0) {
                fprintf(stderr, "verify failed at check %d\n", verify);
                return 1;
            }
        }
        for (v = 0; v < n; v++) {
            int e;
            if (o->model == 1 && v >= nd)
                e = 0;
            else
                e = crng_mil(&ch) < (uint32_t)o->p_mil;
            erased[v] = (uint8_t)e;
            recv[v] = e ? 0 : x[v];
        }
        st = gf16_decode(c, recv, erased, out, target == nd, o->core_max);
        unresolved_sum += st.unresolved;
        rounds_sum += st.rounds;
        core_calls += st.core_calls;
        core_solved += st.core_solved;
        core_failed += st.core_failed;
        core_skipped += st.core_skipped;
        if (st.core_max_vars > core_max_vars)
            core_max_vars = st.core_max_vars;
        core_vars_sum += st.core_vars_sum;
        core_gf2_def += st.core_gf2_deficient;
        core_gf16_rank_sum += st.core_gf16_rank_sum;
        core_under += st.core_under;
        core_rankdef += st.core_rankdef;
        core_bad += st.core_bad;
        for (v = 0; v < n; v++)
            if (gf16_known(c)[v] && out[v] != x[v])
                value_errors++;
        for (v = 0; v < target; v++) {
            if (!gf16_known(c)[v] || out[v] != x[v]) {
                ok = 0;
                break;
            }
        }
        successes += ok;

        if (o->cross_check) {
            sc_code *b = pairs[trial % ncodes].b;
            sc_dec_stat bst;
            for (v = 0; v < n; v++)
                recv_bin[v] = (uint8_t)(recv[v] & 1u);
            bst = sc_decode(b, recv_bin, erased, out_bin, target == nd);
            (void)bst;
            for (v = 0; v < n; v++) {
                int bk = sc_known_bits(b)[v] != 0;
                int gk = gf16_known(c)[v] != 0;
                if (bk && !gk) {
                    if (binary_only == 0)
                        fprintf(stderr,
                                "cross lost trial=%d v=%d: binary known, gf16 "
                                "unknown\n",
                                trial, v);
                    binary_only++;
                } else if (!bk && gk) {
                    gf16_extra++;
                }
            }
        }
    }

    recovery = (double)successes / (double)o->trials;
    ci99 = wilson99(recovery, o->trials, &(double){0.0}, &(double){1.0});
    unresolved_mean = (double)unresolved_sum / (double)o->trials;
    iter_mean = (double)rounds_sum / (double)o->trials;

    /* fixed payload / mask for throughput */
    for (v = 0; v < info.n_data; v++)
        enc_payload[v] = (uint8_t)(crng_next(&ch) & 15u);
    if (gf16_encode(pairs[0].g, enc_payload) != 0) {
        fprintf(stderr, "encode failed (throughput)\n");
        return 1;
    }
    memcpy(dec_payload, enc_payload, (size_t)info.n);
    for (v = 0; v < info.n; v++) {
        int e;
        if (o->model == 1 && v >= info.n_data)
            e = 0;
        else
            e = crng_mil(&ch) < (uint32_t)o->p_mil;
        dec_erased[v] = (uint8_t)e;
        dec_recv[v] = e ? 0 : dec_payload[v];
    }

    for (ti = 0; ti < o->threads; ti++)
        dec_codes[ti] = pairs[ti].g;
    enc_mbps = measure_enc_median(pairs[0].g, enc_payload, o->bench_sec,
                                  o->runs);
    dec_mbps = measure_dec_median(dec_codes, o->threads, dec_recv,
                                  dec_erased, o->model == 1 ? 1 : 0,
                                  o->core_max, o->bench_sec, o->runs);

    if (gethostname(host, sizeof(host)) != 0)
        snprintf(host, sizeof(host), "local");
    host[sizeof(host) - 1] = '\0';

    {
        double rate_target = (double)o->rate_pct / 100.0;
        double rate_actual = (double)info.n_data / (double)info.n;
        double p = (double)o->p_mil / 1e6;
        const char *model = o->model == 1 ? "systematic" : "symmetric";

        if (!o->no_header)
            printf("series,host,block,L,rate_target,rate_actual,p,model,"
                   "trials,codes,seed,recovery,ci99,unresolved_mean,"
                   "iter_mean,enc_mbps,dec_mbps,mem_bytes,n,n_data,n_chk,"
                   "edges\n");
        printf("%s,%s,%d,%d,%.2f,%.6f,%.2f,%s,%d,%d,%llu,%.6f,%.6f,"
               "%.3f,%.3f,%.3f,%.3f,%zu,%d,%d,%d,%ld\n",
               o->series, host, o->block, o->L, rate_target, rate_actual,
               p, model, o->trials, o->codes,
               (unsigned long long)o->seed, recovery, ci99,
               unresolved_mean, iter_mean, enc_mbps, dec_mbps,
               info.mem_bytes, info.n, info.n_data, info.n_chk,
               info.edges);
        if (rep)
            fprintf(rep,
                    "%s,%s,%d,%d,%.2f,%.6f,%.2f,%s,%d,%d,%llu,%.6f,"
                    "%.6f,%.3f,%.3f,%.3f,%.3f,%zu,%d,%d,%d,%ld\n",
                    o->series, host, o->block, o->L, rate_target,
                    rate_actual, p, model, o->trials, o->codes,
                    (unsigned long long)o->seed, recovery, ci99,
                    unresolved_mean, iter_mean, enc_mbps, dec_mbps,
                    info.mem_bytes, info.n, info.n_data, info.n_chk,
                    info.edges);
    }

    if (o->core_max > 0) {
        long attempted = core_calls - core_skipped;
        printf("# core max=%d calls=%ld solved=%ld failed=%ld skipped=%ld "
               "vars_max=%ld vars_avg=%.2f gf2_deficient=%ld "
               "gf16_rank_avg=%.2f under=%ld rankdef=%ld bad=%ld\n",
               o->core_max, core_calls, core_solved, core_failed,
               core_skipped, core_max_vars,
               attempted > 0 ? (double)core_vars_sum / (double)attempted : 0.0,
               core_gf2_def,
               attempted > 0 ? (double)core_gf16_rank_sum / (double)attempted
                             : 0.0,
               core_under, core_rankdef, core_bad);
    }
    if (o->cross_check)
        printf("# cross_check trials=%d binary_only=%ld gf16_extra=%ld\n",
               o->trials, binary_only, gf16_extra);
    if (value_errors != 0)
        fprintf(stderr, "value_errors=%ld\n", value_errors);

    free(x);
    free(recv);
    free(erased);
    free(out);
    free(recv_bin);
    free(out_bin);
    free(enc_payload);
    free(dec_payload);
    free(dec_recv);
    free(dec_erased);
    free_pairs(pairs, ncodes);
    free(pairs);

    if (value_errors != 0 || binary_only != 0)
        return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --block N        variable symbols per coupling block (256)\n"
            "  --L N            coupling length (16)\n"
            "  --rate R         target rate 0.25..0.70 (0.50)\n"
            "  --p P            symbol erasure probability (0.30)\n"
            "  --trials N       Monte-Carlo trials (2000)\n"
            "  --codes N        code instances (4)\n"
            "  --extra-dc N     payload degree of extra checks (5)\n"
            "  --model M        symmetric|systematic (symmetric)\n"
            "  --threads N      decoder worker threads (1)\n"
            "  --bench-sec S    throughput window seconds (0.20)\n"
            "  --runs N         throughput repetitions (3)\n"
            "  --seed N         construction/channel seed (1)\n"
            "  --report FILE    append CSV rows to FILE\n"
            "  --series NAME    series column (gf16-sim)\n"
            "  --no-header      omit the stdout header\n"
            "  --payload-check  no-erasure round trip and exit\n"
            "  --cross-check    compare known sets with binary sc_decode\n"
            "  --core-max N     GF(16) algebraic core limit (0 = peeling only)\n",
            argv0);
}

int main(int argc, char **argv)
{
    opts o;
    FILE *rep = NULL;
    int i;
    int rc;

    memset(&o, 0, sizeof(o));
    o.block = 256;
    o.L = 16;
    o.rate_pct = 50;
    o.p_mil = 300000;
    o.trials = 2000;
    o.codes = 4;
    o.extra_dc = 5;
    o.core_max = 0;
    o.model = 0;
    o.threads = 1;
    o.bench_sec = 0.20;
    o.runs = 3;
    o.seed = 1;
    o.series = "gf16-sim";

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--block") && i + 1 < argc)
            o.block = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--L") && i + 1 < argc)
            o.L = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--rate") && i + 1 < argc)
            o.rate_pct = (int)llround(strtod(argv[++i], NULL) * 100.0);
        else if (!strcmp(a, "--p") && i + 1 < argc)
            o.p_mil = (int)llround(strtod(argv[++i], NULL) * 1e6);
        else if (!strcmp(a, "--trials") && i + 1 < argc)
            o.trials = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--codes") && i + 1 < argc)
            o.codes = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--extra-dc") && i + 1 < argc)
            o.extra_dc = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--core-max") && i + 1 < argc)
            o.core_max = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--model") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "symmetric"))
                o.model = 0;
            else if (!strcmp(m, "systematic"))
                o.model = 1;
            else {
                fprintf(stderr, "bad --model %s\n", m);
                return 2;
            }
        } else if (!strcmp(a, "--threads") && i + 1 < argc)
            o.threads = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--bench-sec") && i + 1 < argc)
            o.bench_sec = strtod(argv[++i], NULL);
        else if (!strcmp(a, "--runs") && i + 1 < argc)
            o.runs = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--seed") && i + 1 < argc)
            o.seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--report") && i + 1 < argc)
            o.report = argv[++i];
        else if (!strcmp(a, "--series") && i + 1 < argc)
            o.series = argv[++i];
        else if (!strcmp(a, "--no-header"))
            o.no_header = 1;
        else if (!strcmp(a, "--payload-check"))
            o.payload_check = 1;
        else if (!strcmp(a, "--cross-check"))
            o.cross_check = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (o.block < 16 || (o.block & 1) || o.block > 8192) {
        fprintf(stderr, "block must be even and in [16,8192]\n");
        return 2;
    }
    if (o.L < 2 || o.L > 4096) {
        fprintf(stderr, "L must be in [2,4096]\n");
        return 2;
    }
    if (o.rate_pct != 25 && o.rate_pct != 30 && o.rate_pct != 35 &&
        o.rate_pct != 40 && o.rate_pct != 45 && o.rate_pct != 50 &&
        o.rate_pct != 60 && o.rate_pct != 70) {
        fprintf(stderr, "rate must be 0.25/0.30/0.35/0.40/0.45/0.50/"
                        "0.60/0.70\n");
        return 2;
    }
    if (o.p_mil < 0 || o.p_mil > 1000000) {
        fprintf(stderr, "p must be in [0,1]\n");
        return 2;
    }
    if (o.trials <= 0 || o.codes <= 0 || o.threads <= 0 || o.runs <= 0) {
        fprintf(stderr, "trials/codes/threads/runs must be positive\n");
        return 2;
    }
    if (o.threads > 8)
        o.threads = 8;
    if (o.codes < o.threads)
        o.codes = o.threads;
    if (o.extra_dc < 1 || o.extra_dc > 16) {
        fprintf(stderr, "extra-dc must be in [1,16]\n");
        return 2;
    }
    if (o.core_max < 0 || o.core_max > 4096) {
        fprintf(stderr, "core-max must be in [0,4096]\n");
        return 2;
    }
    if (o.bench_sec <= 0.0) {
        fprintf(stderr, "bench-sec must be positive\n");
        return 2;
    }

    if (o.report) {
        rep = fopen(o.report, "a");
        if (!rep) {
            perror(o.report);
            return 1;
        }
        fseek(rep, 0, SEEK_END);
        if (ftell(rep) == 0)
            fprintf(rep, "series,host,block,L,rate_target,rate_actual,p,"
                         "model,trials,codes,seed,recovery,ci99,"
                         "unresolved_mean,iter_mean,enc_mbps,dec_mbps,"
                         "mem_bytes,n,n_data,n_chk,edges\n");
    }

    rc = run_point(&o, rep);
    if (rep)
        fclose(rep);
    return rc;
}
