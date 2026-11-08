/*
 * sc_enc64_bench.c - throughput benchmark for the 64-codeword bit-sliced
 * SC-IRA encoder.  See sc_enc64.h for the encoder and sc_ldpc.h for the code.
 *
 * One timed run repeats a kernel --reps times.  --reps is calibrated once per
 * metric from a single call so that one run lasts about --seconds.  Every
 * metric is the median of --runs timed runs.  The reported number is payload
 * MB/s: payload_bytes * reps / dt / 1e6, with dt from CLOCK_MONOTONIC.
 *
 * All payloads come from a seeded xorshift64 stream, so two runs of the
 * benchmark see the same bits.  One payload bit is toggled per repetition.
 * The toggle keeps the compiler from hoisting the kernel out of the loop and
 * does not change the work of the kernel.
 *
 * Metrics:
 *   scalar_ref_mbps  sc_encode() on 64 byte-per-bit codewords, one after the
 *                    other.  Payload is n_data * 64 / 8 bytes.
 *   enc64_mbps       sc_enc64_run() on one 64-codeword bit-plane batch.
 *                    Payload is n_data * 64 / 8 bytes.
 *   e2e_mbps         sc_pack64() of the payload, sc_enc64_run(), then
 *                    sc_unpack64() of the parity.  Payload is the same
 *                    n_data * 64 / 8 bytes.  The parity is unpacked at its
 *                    own width; when n_par is not a multiple of 8 it is
 *                    padded to a whole byte, and when n_data is not byte
 *                    sized the whole codeword is packed and unpacked.  The
 *                    echo line reports which case ran as e2e_mode.
 *   sse2_mbps        128 codewords per batch through sc_enc128_run_sse2(),
 *                    two 64-bit lanes.  Payload is n_data * 128 / 8 bytes.
 *                    Printed only when the kernel reports SSE2 support.
 *   enc64_mt_mbps    aggregate over --threads workers.  Worker k owns a code
 *                    built from seed + k * 7919 and its own batch; the payload
 *                    of all workers is summed.
 *
 * The last line repeats the plan size: xors, n_data, n_par, n_chk.
 *
 * usage: sc_enc64_bench [--block N] [--L N] [--rate R] [--seed S]
 *                       [--seconds X] [--runs N] [--threads N]
 */

#define _POSIX_C_SOURCE 200809L

#include "sc_enc64.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCH_MAX_RUNS 101
#define BENCH_MAX_THREADS 64
#define BENCH_REP_CAP 200000L

typedef struct {
    int block;
    int L;
    int rate_pct;
    int extra_dc;
    uint64_t seed;
    double seconds;
    int runs;
    int threads;
} bench_cfg;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* xorshift64; the state must not be zero. */
static uint64_t xs_next(uint64_t *s)
{
    uint64_t x = *s;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static void xs_seed(uint64_t *s, uint64_t seed)
{
    *s = seed + 0x9E3779B97F4A7C15ULL;
    if (*s == 0)
        *s = 0x2545F4914F6CDD1DULL;
}

static void *xmalloc(size_t bytes)
{
    void *p = malloc(bytes);

    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

static void *xcalloc(size_t count, size_t size)
{
    void *p = calloc(count, size);

    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

static long reps_for(double dt, double target)
{
    long reps;

    if (dt < 1e-7)
        dt = 1e-7;
    reps = (long)(target / dt);
    if (reps < 1)
        reps = 1;
    if (reps > BENCH_REP_CAP)
        reps = BENCH_REP_CAP;
    return reps;
}

static double mbps_of(double bytes, long reps, double dt)
{
    if (dt <= 0.0)
        dt = 1e-9;
    return bytes * (double)reps / dt / 1e6;
}

static int cmp_double(const void *pa, const void *pb)
{
    double a = *(const double *)pa;
    double b = *(const double *)pb;

    return (a > b) - (a < b);
}

static double median(double *v, int n)
{
    qsort(v, (size_t)n, sizeof(*v), cmp_double);
    if (n & 1)
        return v[n / 2];
    return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* ------------------------------------------------------------------ */
/* scalar reference: 64 codewords, byte per bit                        */
/* ------------------------------------------------------------------ */

static double bench_scalar(sc_code *c, const bench_cfg *g)
{
    sc_info in;
    double samples[BENCH_MAX_RUNS];
    double t0, t1;
    uint8_t *code;
    uint64_t s;
    size_t cw, total;
    long reps, r;
    int b, k, run;

    sc_get_info(c, &in);
    cw = (size_t)in.n;
    total = cw * 64u;
    code = xcalloc(total, 1);

    xs_seed(&s, g->seed ^ 0x51ED270BULL);
    for (b = 0; b < 64; b++)
        for (k = 0; k < in.n_data; k++)
            code[(size_t)b * cw + (size_t)k] = (uint8_t)(xs_next(&s) & 1u);

    t0 = now_sec();
    for (b = 0; b < 64; b++)
        (void)sc_encode(c, code + (size_t)b * cw);
    t1 = now_sec();
    reps = reps_for(t1 - t0, g->seconds);

    for (run = 0; run < g->runs; run++) {
        t0 = now_sec();
        for (r = 0; r < reps; r++) {
            code[0] ^= 1u;
            for (b = 0; b < 64; b++)
                (void)sc_encode(c, code + (size_t)b * cw);
        }
        t1 = now_sec();
        samples[run] = mbps_of((double)in.n_data * 8.0, reps, t1 - t0);
    }

    free(code);
    return median(samples, g->runs);
}

/* ------------------------------------------------------------------ */
/* one 64-codeword bit-plane batch                                     */
/* ------------------------------------------------------------------ */

static double bench_enc64(const sc_enc64 *e, const bench_cfg *g)
{
    double samples[BENCH_MAX_RUNS];
    double t0, t1;
    uint64_t *v;
    uint64_t s;
    int n = sc_enc64_n(e);
    int nd = sc_enc64_n_data(e);
    long reps, r;
    int k, run;

    v = xcalloc((size_t)n, sizeof(*v));
    xs_seed(&s, g->seed ^ 0xA5A5A5A5ULL);
    for (k = 0; k < nd; k++)
        v[k] = xs_next(&s);

    t0 = now_sec();
    v[0] ^= 1u;
    sc_enc64_run(e, v);
    t1 = now_sec();
    reps = reps_for(t1 - t0, g->seconds);

    for (run = 0; run < g->runs; run++) {
        t0 = now_sec();
        for (r = 0; r < reps; r++) {
            v[0] ^= 1u;
            sc_enc64_run(e, v);
        }
        t1 = now_sec();
        samples[run] = mbps_of((double)nd * 8.0, reps, t1 - t0);
    }

    free(v);
    return median(samples, g->runs);
}

/* ------------------------------------------------------------------ */
/* pack + encode + unpack                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int pay_bits;
    int par_bits;
    int pay_off;
    int par_off;
    int vwords;
    int padded; /* parity width rounded up to a whole byte */
    int full;   /* whole codeword packed and unpacked */
} e2e_plan;

static void e2e_make(const sc_enc64 *e, e2e_plan *p)
{
    int n = sc_enc64_n(e);
    int nd = sc_enc64_n_data(e);
    int np = sc_enc64_n_par(e);

    if (nd % 8 == 0) {
        p->pay_bits = nd;
        p->pay_off = 0;
        p->par_bits = (np + 7) & ~7;
        p->par_off = nd;
        p->vwords = nd + p->par_bits;
        p->padded = p->par_bits != np;
        p->full = 0;
    } else {
        p->pay_bits = (n + 7) & ~7;
        p->par_bits = p->pay_bits;
        p->pay_off = 0;
        p->par_off = 0;
        p->vwords = p->pay_bits;
        p->padded = 1;
        p->full = 1;
    }
}

static const char *e2e_mode_name(const e2e_plan *p)
{
    if (p->full)
        return "full";
    if (p->padded)
        return "parity";
    return "exact";
}

static double bench_e2e(const sc_enc64 *e, const bench_cfg *g)
{
    e2e_plan p;
    double samples[BENCH_MAX_RUNS];
    double t0, t1;
    uint8_t *pay, *par;
    uint64_t *v;
    uint64_t s;
    size_t pay_bytes, par_bytes, i;
    long reps, r;
    int nd = sc_enc64_n_data(e);
    int run;

    e2e_make(e, &p);
    pay_bytes = (size_t)(p.pay_bits / 8) * 64u;
    par_bytes = (size_t)(p.par_bits / 8) * 64u;
    pay = xmalloc(pay_bytes);
    par = xmalloc(par_bytes);
    v = xcalloc((size_t)p.vwords, sizeof(*v));

    xs_seed(&s, g->seed ^ 0x9E3779B9ULL);
    for (i = 0; i < pay_bytes; i++)
        pay[i] = (uint8_t)xs_next(&s);

    t0 = now_sec();
    sc_pack64(v + p.pay_off, pay, p.pay_bits);
    sc_enc64_run(e, v);
    sc_unpack64(par, v + p.par_off, p.par_bits);
    t1 = now_sec();
    reps = reps_for(t1 - t0, g->seconds);

    for (run = 0; run < g->runs; run++) {
        t0 = now_sec();
        for (r = 0; r < reps; r++) {
            pay[0] ^= 1u;
            sc_pack64(v + p.pay_off, pay, p.pay_bits);
            sc_enc64_run(e, v);
            sc_unpack64(par, v + p.par_off, p.par_bits);
        }
        t1 = now_sec();
        samples[run] = mbps_of((double)nd * 8.0, reps, t1 - t0);
    }

    free(pay);
    free(par);
    free(v);
    return median(samples, g->runs);
}

/* ------------------------------------------------------------------ */
/* 128 codewords, two 64-bit lanes                                     */
/* ------------------------------------------------------------------ */

static int bench_sse2(const sc_enc64 *e, const bench_cfg *g, double *mbps_out)
{
    double samples[BENCH_MAX_RUNS];
    double t0, t1;
    void *raw = NULL;
    uint64_t *v;
    uint64_t s;
    int n = sc_enc64_n(e);
    int nd = sc_enc64_n_data(e);
    long reps, r;
    int k, run;

    if (posix_memalign(&raw, 16, (size_t)n * 16u) != 0 || !raw)
        return -1;
    v = raw;

    xs_seed(&s, g->seed ^ 0xC001D00DULL);
    for (k = 0; k < 2 * nd; k++)
        v[k] = xs_next(&s);
    for (k = 2 * nd; k < 2 * n; k++)
        v[k] = 0;

    if (sc_enc128_run_sse2(e, v) != 0) {
        free(raw);
        return -1;
    }

    t0 = now_sec();
    v[0] ^= 1u;
    (void)sc_enc128_run_sse2(e, v);
    t1 = now_sec();
    reps = reps_for(t1 - t0, g->seconds);

    for (run = 0; run < g->runs; run++) {
        t0 = now_sec();
        for (r = 0; r < reps; r++) {
            v[0] ^= 1u;
            (void)sc_enc128_run_sse2(e, v);
        }
        t1 = now_sec();
        samples[run] = mbps_of((double)nd * 16.0, reps, t1 - t0);
    }

    *mbps_out = median(samples, g->runs);
    free(raw);
    return 0;
}

/* ------------------------------------------------------------------ */
/* multi-threaded 64-codeword batches                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const bench_cfg *g;
    int id;
    long reps;
    double mbps;
    int ok;
    pthread_barrier_t *bar;
} mt_worker_arg;

static void *mt_worker(void *arg)
{
    mt_worker_arg *a = arg;
    const bench_cfg *g = a->g;
    sc_params p;
    sc_code *c;
    sc_enc64 *e;
    sc_info in;
    uint64_t *v;
    uint64_t s;
    char err[160];
    double t0, t1;
    long r;
    int k;

    memset(&p, 0, sizeof(p));
    p.block = g->block;
    p.L = g->L;
    p.rate_pct = g->rate_pct;
    p.term = SC_TERM_ACC;
    p.extra_dc = g->extra_dc;
    p.seed = g->seed + (uint64_t)a->id * 7919u;

    c = sc_build(&p, err, sizeof(err));
    if (!c) {
        fprintf(stderr, "enc64_mt: sc_build: %s\n", err);
        pthread_barrier_wait(a->bar);
        return NULL;
    }
    e = sc_enc64_build(c, err, sizeof(err));
    if (!e) {
        fprintf(stderr, "enc64_mt: sc_enc64_build: %s\n", err);
        sc_free(c);
        pthread_barrier_wait(a->bar);
        return NULL;
    }
    sc_get_info(c, &in);

    v = xmalloc((size_t)in.n * sizeof(*v));
    xs_seed(&s, p.seed ^ 0x5BD1E995ULL);
    for (k = 0; k < in.n; k++)
        v[k] = 0;
    for (k = 0; k < in.n_data; k++)
        v[k] = xs_next(&s);
    a->ok = 1;

    pthread_barrier_wait(a->bar);
    t0 = now_sec();
    for (r = 0; r < a->reps; r++) {
        v[0] ^= 1u;
        sc_enc64_run(e, v);
    }
    t1 = now_sec();
    a->mbps = mbps_of((double)in.n_data * 8.0, a->reps, t1 - t0);

    free(v);
    sc_enc64_free(e);
    sc_free(c);
    return NULL;
}

static double bench_mt(const sc_enc64 *ref, const bench_cfg *g, int *ok_out)
{
    pthread_barrier_t bar;
    pthread_t tid[BENCH_MAX_THREADS];
    mt_worker_arg args[BENCH_MAX_THREADS];
    double samples[BENCH_MAX_RUNS];
    uint64_t *v;
    uint64_t s;
    double t0, t1, sum;
    long reps;
    int run, t, ok = 1;
    int n = sc_enc64_n(ref);
    int nd = sc_enc64_n_data(ref);

    /* Calibrate on the reference plan; every worker has the same shape. */
    v = xcalloc((size_t)n, sizeof(*v));
    xs_seed(&s, g->seed ^ 0x77777777ULL);
    for (t = 0; t < nd; t++)
        v[t] = xs_next(&s);
    t0 = now_sec();
    v[0] ^= 1u;
    sc_enc64_run(ref, v);
    t1 = now_sec();
    reps = reps_for(t1 - t0, g->seconds);
    free(v);

    for (run = 0; run < g->runs; run++) {
        if (pthread_barrier_init(&bar, NULL, (unsigned)g->threads) != 0) {
            fprintf(stderr, "enc64_mt: pthread_barrier_init failed\n");
            *ok_out = 0;
            return 0.0;
        }
        for (t = 0; t < g->threads; t++) {
            args[t].g = g;
            args[t].id = t;
            args[t].reps = reps;
            args[t].mbps = 0.0;
            args[t].ok = 0;
            args[t].bar = &bar;
            if (pthread_create(&tid[t], NULL, mt_worker, &args[t]) != 0) {
                fprintf(stderr, "enc64_mt: pthread_create failed\n");
                exit(1);
            }
        }
        for (t = 0; t < g->threads; t++)
            (void)pthread_join(tid[t], NULL);
        (void)pthread_barrier_destroy(&bar);

        sum = 0.0;
        for (t = 0; t < g->threads; t++) {
            if (!args[t].ok)
                ok = 0;
            sum += args[t].mbps;
        }
        samples[run] = sum;
    }

    *ok_out = ok;
    return median(samples, g->runs);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
            "usage: sc_enc64_bench [--block N] [--L N] [--rate R] [--seed S]\n"
            "                      [--seconds X] [--runs N] [--threads N]\n"
            "  --block N    variables per coupling block (256)\n"
            "  --L N        coupling length (16)\n"
            "  --rate R     target rate (0.50)\n"
            "  --seed S     construction and payload seed (1)\n"
            "  --seconds X  target time of one timed run (0.20)\n"
            "  --runs N     timed runs per metric, median reported (5)\n"
            "  --threads N  enc64_mt workers (1)\n");
}

int main(int argc, char **argv)
{
    bench_cfg g;
    sc_params p;
    sc_code *c;
    sc_enc64 *e;
    sc_info in;
    e2e_plan ep;
    char err[160];
    double scalar, enc64, e2e, sse2 = 0.0, mt;
    int have_sse2, mt_ok = 1;
    int i;

    memset(&g, 0, sizeof(g));
    g.block = 256;
    g.L = 16;
    g.rate_pct = 50;
    g.extra_dc = 5;
    g.seed = 1;
    g.seconds = 0.20;
    g.runs = 5;
    g.threads = 1;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else if (!strcmp(a, "--block") && i + 1 < argc) {
            g.block = (int)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--L") && i + 1 < argc) {
            g.L = (int)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--rate") && i + 1 < argc) {
            g.rate_pct = (int)llround(strtod(argv[++i], NULL) * 100.0);
        } else if (!strcmp(a, "--seed") && i + 1 < argc) {
            g.seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--seconds") && i + 1 < argc) {
            g.seconds = strtod(argv[++i], NULL);
        } else if (!strcmp(a, "--runs") && i + 1 < argc) {
            g.runs = (int)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--threads") && i + 1 < argc) {
            g.threads = (int)strtol(argv[++i], NULL, 10);
        } else {
            usage();
            return 2;
        }
    }

    if (g.runs < 1)
        g.runs = 1;
    if (g.runs > BENCH_MAX_RUNS)
        g.runs = BENCH_MAX_RUNS;
    if (g.threads < 1)
        g.threads = 1;
    if (g.threads > BENCH_MAX_THREADS)
        g.threads = BENCH_MAX_THREADS;
    if (!(g.seconds > 0.0))
        g.seconds = 0.20;

    memset(&p, 0, sizeof(p));
    p.block = g.block;
    p.L = g.L;
    p.rate_pct = g.rate_pct;
    p.term = SC_TERM_ACC;
    p.extra_dc = g.extra_dc;
    p.seed = g.seed;

    c = sc_build(&p, err, sizeof(err));
    if (!c) {
        fprintf(stderr, "sc_build: %s\n", err);
        return 1;
    }
    e = sc_enc64_build(c, err, sizeof(err));
    if (!e) {
        fprintf(stderr, "sc_enc64_build: %s\n", err);
        sc_free(c);
        return 1;
    }
    sc_get_info(c, &in);
    e2e_make(e, &ep);

    printf("config block=%d L=%d rate=%.3f seed=%llu seconds=%.3f runs=%d "
           "threads=%d n=%d n_data=%d n_par=%d n_chk=%d xors=%ld batch=64 "
           "sse2_batch=128 e2e_mode=%s\n",
           g.block, g.L, (double)g.rate_pct / 100.0,
           (unsigned long long)g.seed, g.seconds, g.runs, g.threads, in.n,
           in.n_data, in.n_par, in.n_chk, sc_enc64_xors(e),
           e2e_mode_name(&ep));

    scalar = bench_scalar(c, &g);
    printf("scalar_ref_mbps=%.3f\n", scalar);

    enc64 = bench_enc64(e, &g);
    printf("enc64_mbps=%.3f\n", enc64);

    e2e = bench_e2e(e, &g);
    printf("e2e_mbps=%.3f\n", e2e);

    have_sse2 = bench_sse2(e, &g, &sse2);
    if (have_sse2 == 0)
        printf("sse2_mbps=%.3f\n", sse2);
    else
        fprintf(stderr,
                "sse2: sc_enc128_run_sse2 reports no SSE2 support, "
                "metric omitted\n");

    mt = bench_mt(e, &g, &mt_ok);
    if (!mt_ok) {
        fprintf(stderr, "enc64_mt: a worker failed, metric is incomplete\n");
        sc_enc64_free(e);
        sc_free(c);
        return 1;
    }
    printf("enc64_mt_mbps=%.3f\n", mt);

    printf("xors=%ld n_data=%d n_par=%d n_chk=%d\n", sc_enc64_xors(e),
           sc_enc64_n_data(e), sc_enc64_n_par(e), sc_enc64_n_chk(e));

    sc_enc64_free(e);
    sc_free(c);
    return 0;
}
