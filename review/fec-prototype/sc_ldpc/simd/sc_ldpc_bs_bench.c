/*
 * sc_ldpc_bs_bench.c - throughput and recovery of the bit-sliced decoder.
 *
 * Measures the same work two ways on the same machine and the same data:
 *   scalar  - sc_decode() once per codeword, byte arrays
 *   sliced  - sc_decode_bs() on a batch of `batch` codewords, packed arrays
 *
 * Both paths include their own input preparation inside the timed region?
 * No: codewords and masks are prepared once, outside the loop, exactly like
 * the production decoder would receive them.  The decoder call itself does
 * the packing/transposition for the sliced path, so that cost is included.
 *
 * Recovery runs decode each batch with both paths and compare the output
 * bytes and the unresolved counts.
 */

#define _POSIX_C_SOURCE 200809L

#include "sc_ldpc.h"
#include "sc_ldpc_bs.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* rng and helpers                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t s;
} brng;

static uint64_t brng_next(brng *r)
{
    uint64_t x = r->s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->s = x;
    return x;
}

static void brng_seed(brng *r, uint64_t seed)
{
    r->s = seed ? seed : 1;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void pack_cw(uint8_t *dst, size_t stride, const uint8_t *bits, int n)
{
    int v;
    memset(dst, 0, stride);
    for (v = 0; v < n; v++)
        if (bits[v] & 1u)
            dst[(size_t)(v >> 3)] |= (uint8_t)(1u << (v & 7));
}

static int bit_of(const uint8_t *p, int v)
{
    return (p[(size_t)(v >> 3)] >> (v & 7)) & 1u;
}

/* ------------------------------------------------------------------ */
/* batch preparation                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    sc_code *c;
    sc_info in;
    int batch;
    size_t stride;
    uint8_t *truth;   /* batch * n bytes */
    uint8_t *erased;  /* batch * n bytes */
    uint8_t *pbits;   /* batch * stride */
    uint8_t *pmask;
    uint8_t *pout;
    uint8_t *out;     /* scratch for one scalar codeword */
    long *unres;
} batch_ctx;

static int batch_ctx_init(batch_ctx *b, sc_code *c, int batch)
{
    size_t n, nb;

    memset(b, 0, sizeof(*b));
    b->c = c;
    sc_get_info(c, &b->in);
    b->batch = batch;
    n = (size_t)b->in.n;
    nb = (size_t)batch * n;
    b->stride = (n + 7) / 8;
    b->truth = calloc(nb, 1);
    b->erased = calloc(nb, 1);
    b->pbits = calloc((size_t)batch * b->stride, 1);
    b->pmask = calloc((size_t)batch * b->stride, 1);
    b->pout = calloc((size_t)batch * b->stride, 1);
    b->out = calloc(n, 1);
    b->unres = calloc((size_t)batch, sizeof(long));
    if (!b->truth || !b->erased || !b->pbits || !b->pmask || !b->pout ||
        !b->out || !b->unres)
        return -1;
    return 0;
}

static void batch_ctx_free(batch_ctx *b)
{
    free(b->truth);
    free(b->erased);
    free(b->pbits);
    free(b->pmask);
    free(b->pout);
    free(b->out);
    free(b->unres);
    memset(b, 0, sizeof(*b));
}

/*
 * Fill the batch: random payload codewords and independent erasure masks.
 * `model` selects the channel.  Packed arrays are derived from the bytes so
 * the scalar and sliced paths see exactly the same input.
 */
static int batch_fill(batch_ctx *b, int p_mil, int model, int corr, brng *rng)
{
    int j, v;
    int n = b->in.n;
    int nd = b->in.n_data;

    for (j = 0; j < b->batch; j++) {
        uint8_t *truth = b->truth + (size_t)j * n;
        uint8_t *erased = b->erased + (size_t)j * n;
        for (v = 0; v < nd; v++)
            truth[v] = (uint8_t)(brng_next(rng) & 1u);
        if (sc_encode(b->c, truth) != 0)
            return -1;
        if (sc_verify(b->c, truth) != 0)
            return -1;
        if (corr && j > 0) {
            memcpy(erased, b->erased, (size_t)n);
        } else {
            for (v = 0; v < n; v++) {
                if (model == SC_MODEL_SYSTEMATIC && v >= nd)
                    erased[v] = 0;
                else
                    erased[v] = (uint8_t)((brng_next(rng) % 1000u) <
                                          (uint64_t)p_mil);
            }
        }
        pack_cw(b->pbits + (size_t)j * b->stride, b->stride, truth, n);
        pack_cw(b->pmask + (size_t)j * b->stride, b->stride, erased, n);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* timing                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    double mbps;
    double sec;
    long reps;
} timing;

static double payload_bytes(const sc_info *in, int batch)
{
    return (double)batch * (double)in->n_data / 8.0;
}

static timing time_scalar(batch_ctx *b, int reps)
{
    timing t;
    double t0, t1;
    long r;
    int j;

    t0 = now_sec();
    for (r = 0; r < reps; r++)
        for (j = 0; j < b->batch; j++) {
            const uint8_t *truth = b->truth + (size_t)j * b->in.n;
            const uint8_t *erased = b->erased + (size_t)j * b->in.n;
            (void)sc_decode(b->c, truth, erased, b->out, 1);
        }
    t1 = now_sec();
    t.sec = t1 - t0;
    if (t.sec <= 0.0)
        t.sec = 1e-9;
    t.reps = reps;
    t.mbps = payload_bytes(&b->in, b->batch) * (double)reps / t.sec / 1e6;
    return t;
}

static timing time_bs(batch_ctx *b, sc_bs *bs, int reps)
{
    timing t;
    double t0, t1;
    long r;

    t0 = now_sec();
    for (r = 0; r < reps; r++)
        (void)sc_decode_bs(bs, b->pbits, b->pmask, b->pout, 1, b->unres);
    t1 = now_sec();
    t.sec = t1 - t0;
    if (t.sec <= 0.0)
        t.sec = 1e-9;
    t.reps = reps;
    t.mbps = payload_bytes(&b->in, b->batch) * (double)reps / t.sec / 1e6;
    return t;
}

static int calibrate(batch_ctx *b, sc_bs *bs, int use_bs, double target_sec)
{
    double t0, t1, dt;
    int reps;

    t0 = now_sec();
    if (use_bs)
        (void)sc_decode_bs(bs, b->pbits, b->pmask, b->pout, 1, b->unres);
    else {
        int j;
        for (j = 0; j < b->batch; j++)
            (void)sc_decode(b->c, b->truth + (size_t)j * b->in.n,
                            b->erased + (size_t)j * b->in.n, b->out, 1);
    }
    t1 = now_sec();
    dt = t1 - t0;
    if (dt < 1e-7)
        dt = 1e-7;
    reps = (int)(target_sec / dt);
    if (reps < 1)
        reps = 1;
    if (reps > 200000)
        reps = 200000;
    return reps;
}

/* ------------------------------------------------------------------ */
/* recovery comparison                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    long cw;
    long full_bs;
    long full_scalar;
    long mismatch;
    long unres_mismatch;
    double iter_mean;
} recov;

static recov check_recovery(batch_ctx *b, sc_bs *bs, int batches)
{
    recov r;
    int bi, j, v;
    int n = b->in.n;
    int lim = b->in.n_data; /* sc_decode_bs is called with target_data_only */

    memset(&r, 0, sizeof(r));
    for (bi = 0; bi < batches; bi++) {
        long recovered = sc_decode_bs(bs, b->pbits, b->pmask, b->pout, 1,
                                      b->unres);
        (void)recovered;
        for (j = 0; j < b->batch; j++) {
            sc_dec_stat st;
            const uint8_t *pj = b->pout + (size_t)j * b->stride;
            st = sc_decode(b->c, b->truth + (size_t)j * n,
                           b->erased + (size_t)j * n, b->out, 1);
            r.cw++;
            r.iter_mean += (double)st.rounds;
            if (st.unresolved != b->unres[j])
                r.unres_mismatch++;
            if (st.unresolved == 0)
                r.full_scalar++;
            if (b->unres[j] == 0)
                r.full_bs++;
            for (v = 0; v < lim; v++)
                if (bit_of(pj, v) != b->out[v]) {
                    r.mismatch++;
                    break;
                }
        }
    }
    if (r.cw)
        r.iter_mean /= (double)r.cw;
    return r;
}

/* ------------------------------------------------------------------ */
/* two-thread measurement                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    batch_ctx *b;
    sc_bs *bs;
    int reps;
    int use_bs;
    double mbps;
    pthread_barrier_t *bar;
} worker_arg;

static void *worker(void *arg)
{
    worker_arg *w = arg;
    timing t;

    pthread_barrier_wait(w->bar);
    if (w->use_bs)
        t = time_bs(w->b, w->bs, w->reps);
    else
        t = time_scalar(w->b, w->reps);
    pthread_barrier_wait(w->bar);
    w->mbps = t.mbps;
    return NULL;
}

static double time_two_threads(batch_ctx *b0, sc_bs *bs0, batch_ctx *b1,
                               sc_bs *bs1, int use_bs, int reps)
{
    pthread_barrier_t bar;
    pthread_t tid[2];
    worker_arg a[2];
    double t0, t1, dt, bytes;
    int i;

    pthread_barrier_init(&bar, NULL, 3);
    for (i = 0; i < 2; i++) {
        a[i].b = i ? b1 : b0;
        a[i].bs = i ? bs1 : bs0;
        a[i].reps = reps;
        a[i].use_bs = use_bs;
        a[i].mbps = 0.0;
        a[i].bar = &bar;
        if (pthread_create(&tid[i], NULL, worker, &a[i]) != 0) {
            pthread_barrier_destroy(&bar);
            return 0.0;
        }
    }
    t0 = now_sec();
    pthread_barrier_wait(&bar);
    pthread_barrier_wait(&bar);
    t1 = now_sec();
    for (i = 0; i < 2; i++)
        pthread_join(tid[i], NULL);
    pthread_barrier_destroy(&bar);
    dt = t1 - t0;
    if (dt <= 0.0)
        dt = 1e-9;
    bytes = payload_bytes(&b0->in, b0->batch) +
            payload_bytes(&b1->in, b1->batch);
    return bytes * (double)reps / dt / 1e6;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    int block;
    int L;
    int rate_pct;
    int p_mil;
    int trials;
    int batch;
    int threads;
    int model;
    double bench_sec;
    int corr; /* all codewords share one erasure mask */
    uint64_t seed;
} opts;

static sc_code *build_code(const opts *o, uint64_t seed)
{
    sc_params p;
    char err[256];
    sc_code *c;

    memset(&p, 0, sizeof(p));
    p.block = o->block;
    p.L = o->L;
    p.rate_pct = o->rate_pct;
    p.term = SC_TERM_ACC;
    p.extra_dc = 5;
    p.seed = seed;
    c = sc_build(&p, err, sizeof(err));
    if (!c)
        fprintf(stderr, "build failed: %s\n", err);
    return c;
}

int main(int argc, char **argv)
{
    opts o;
    sc_code *c0 = NULL, *c1 = NULL;
    sc_bs *bs0 = NULL, *bs1 = NULL;
    batch_ctx b0, b1;
    char err[256];
    brng rng;
    int i, ok = 1;
    int batches;
    timing ts, tb, ts2, tb2;
    recov rec;
    sc_info in;

    memset(&o, 0, sizeof(o));
    o.block = 256;
    o.L = 16;
    o.rate_pct = 50;
    o.p_mil = 300;
    o.trials = 2000;
    o.batch = 64;
    o.threads = 1;
    o.model = SC_MODEL_SYMMETRIC;
    o.bench_sec = 0.2;
    o.seed = 1;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--block") && i + 1 < argc)
            o.block = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--L") && i + 1 < argc)
            o.L = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--rate") && i + 1 < argc)
            o.rate_pct = (int)(strtod(argv[++i], NULL) * 100.0 + 0.5);
        else if (!strcmp(a, "--p") && i + 1 < argc)
            o.p_mil = (int)(strtod(argv[++i], NULL) * 1000.0 + 0.5);
        else if (!strcmp(a, "--trials") && i + 1 < argc)
            o.trials = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--batch") && i + 1 < argc)
            o.batch = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--threads") && i + 1 < argc)
            o.threads = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--corr"))
            o.corr = 1;
        else if (!strcmp(a, "--bench-sec") && i + 1 < argc)
            o.bench_sec = strtod(argv[++i], NULL);
        else if (!strcmp(a, "--seed") && i + 1 < argc)
            o.seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--model") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "sys"))
                o.model = SC_MODEL_SYSTEMATIC;
            else if (!strcmp(m, "sym"))
                o.model = SC_MODEL_SYMMETRIC;
            else {
                fprintf(stderr, "bad model\n");
                return 2;
            }
        } else {
            fprintf(stderr,
                    "usage: %s [--block N] [--L N] [--rate R] [--p P] "
                    "[--trials N] [--batch 64|128|256|512] [--threads 1|2] "
                    "[--model sym|sys] [--corr] [--bench-sec S] [--seed N]\n",
                    argv[0]);
            return 2;
        }
    }
    if (o.batch % 64 || o.batch < 64 || o.batch > 2048) {
        fprintf(stderr, "batch must be 64..2048 in steps of 64\n");
        return 2;
    }

    c0 = build_code(&o, o.seed);
    if (!c0)
        return 1;
    sc_get_info(c0, &in);
    bs0 = sc_bs_new(c0, o.batch, err, sizeof(err));
    if (!bs0) {
        fprintf(stderr, "sc_bs_new: %s\n", err);
        return 1;
    }
    if (batch_ctx_init(&b0, c0, o.batch) != 0) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    brng_seed(&rng, o.seed + 0x9e3779b9u);
    if (batch_fill(&b0, o.p_mil, o.model, o.corr, &rng) != 0) {
        fprintf(stderr, "batch fill failed\n");
        return 1;
    }

    if (o.threads == 2) {
        c1 = build_code(&o, o.seed + 1);
        if (!c1)
            return 1;
        bs1 = sc_bs_new(c1, o.batch, err, sizeof(err));
        if (!bs1 || batch_ctx_init(&b1, c1, o.batch) != 0) {
            fprintf(stderr, "second context failed\n");
            return 1;
        }
        brng_seed(&rng, o.seed + 0x9e3779b9u + 1);
        if (batch_fill(&b1, o.p_mil, o.model, o.corr, &rng) != 0)
            return 1;
    }

    /* ---- recovery, same inputs for both paths -------------------- */
    batches = o.trials / o.batch;
    if (batches < 1)
        batches = 1;
    rec = check_recovery(&b0, bs0, batches);
    if (rec.mismatch || rec.unres_mismatch)
        ok = 0;

    /* ---- throughput --------------------------------------------- */
    {
        int reps_s = calibrate(&b0, bs0, 0, o.bench_sec);
        int reps_b = calibrate(&b0, bs0, 1, o.bench_sec);
        ts = time_scalar(&b0, reps_s);
        tb = time_bs(&b0, bs0, reps_b);
        {
            sc_bs_stat bst;
            sc_bs_get_stat(bs0, &bst);
            fprintf(stderr,
                    "bs stat per batch: waves=%ld check_scans=%ld "
                    "var_resolves=%ld edge_updates=%ld\n",
                    bst.waves, bst.check_scans, bst.var_resolves,
                    bst.edge_updates);
#ifdef BS_TIMING
            {
                double ti, tw, tu, to;
                sc_bs_timing(bs0, &ti, &tw, &tu, &to);
                fprintf(stderr, "bs phases: init=%.3f wave=%.3f "
                        "unres=%.3f out=%.3f\n", ti, tw, tu, to);
            }
#endif
        }
        ts2.mbps = 0.0;
        tb2.mbps = 0.0;
        if (o.threads == 2) {
            int reps_s2 = calibrate(&b1, bs1, 0, o.bench_sec);
            int reps_b2 = calibrate(&b1, bs1, 1, o.bench_sec);
            ts2.mbps = time_two_threads(&b0, bs0, &b1, bs1, 0, reps_s2);
            tb2.mbps = time_two_threads(&b0, bs0, &b1, bs1, 1, reps_b2);
        }
    }

    printf("point,block,L,rate,p,model,corr,batch,threads,cw,recovery_bs,"
           "recovery_scalar,unres_mismatch,bit_mismatch,iter_mean,"
           "dec_mbps_scalar,dec_mbps_bs,speedup,dec_mbps_bs_2t,speedup_2t\n");
    printf("point,%d,%d,%.2f,%.2f,%s,%d,%d,%d,%ld,%.6f,%.6f,%ld,%ld,%.2f,"
           "%.3f,%.3f,%.2f,%.3f,%.2f\n",
           o.block, o.L, (double)o.rate_pct / 100.0, (double)o.p_mil / 1000.0,
           o.model == SC_MODEL_SYSTEMATIC ? "sys" : "sym", o.corr, o.batch,
           o.threads,
           rec.cw,
           rec.cw ? (double)rec.full_bs / (double)rec.cw : 0.0,
           rec.cw ? (double)rec.full_scalar / (double)rec.cw : 0.0,
           rec.unres_mismatch, rec.mismatch, rec.iter_mean, ts.mbps, tb.mbps,
           ts.mbps > 0.0 ? tb.mbps / ts.mbps : 0.0, tb2.mbps,
           ts2.mbps > 0.0 ? tb2.mbps / ts2.mbps : 0.0);

    fprintf(stderr,
            "block=%d L=%d rate=%.2f p=%.2f model=%s batch=%d n=%d edges=%ld "
            "dc_max=%d\n", o.block, o.L, (double)o.rate_pct / 100.0,
            (double)o.p_mil / 1000.0,
            o.model == SC_MODEL_SYSTEMATIC ? "sys" : "sym", o.batch, in.n,
            in.edges, in.dc_max);
    if (tb.sec > 0.0)
        fprintf(stderr, "reps: scalar=%ld sliced=%ld\n", ts.reps, tb.reps);

    if (o.threads == 2) {
        batch_ctx_free(&b1);
        sc_bs_free(bs1);
        sc_free(c1);
    }
    batch_ctx_free(&b0);
    sc_bs_free(bs0);
    sc_free(c0);
    return ok ? 0 : 1;
}
