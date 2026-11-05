/*
 * sc_ldpc_bench.c - harness for the SC-LDPC prototype.
 *
 * Single point (the interface the cross-machine harness uses):
 *
 *   cc -O3 -std=c99 -Wall -Wextra -Werror sc_ldpc.c sc_ldpc_bench.c -lm \
 *      -o sc_ldpc_bench
 *   ./sc_ldpc_bench --block 256 --L 8 --rate 0.50 --p 0.30 --trials 2000
 *
 * Prints a header line and one CSV row:
 *
 *   block,L,rate,p,trials,recovery,ci99,iter_mean,enc_mbps,dec_mbps,
 *   latency_ms,mem_bytes
 *
 * recovery is the full-recovery probability of the payload variables,
 * ci99 is the half-width of the Wilson score interval at 99 %,
 * iter_mean is the mean number of peeling waves (BP sweeps),
 * enc_mbps / dec_mbps are payload MB/s measured on this machine,
 * latency_ms is L * (seg_bytes*8)/1e9*1000 at 1 Gbit/s (default 8192 B),
 * mem_bytes is the resident size of the code object.
 *
 * Extra flags:
 *   --model systematic|symmetric  channel model (default systematic)
 *   --term acc|causal|native      boundary rule (default acc)
 *   --extra-dc N                  payload degree of extra checks (default 5)
 *   --codes N                     code instances averaged (default 4)
 *   --seed N                      construction / channel seed
 *   --seg-bytes N                 payload bytes per coupling block (8192)
 *   --no-header                   omit the CSV header line
 *   --sweep                       run the 54-point grid instead of one point
 *   --full                        also run the symmetric, native and fine
 *                                 p sweeps (writes --report if given)
 *   --report FILE                 extended CSV with a series column
 *   --payload-check               verify a real payload round trip (stderr)
 */

#define _POSIX_C_SOURCE 200809L

#include "sc_ldpc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* xorshift128+ for the channel; construction has its own stream. */
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

/* ------------------------------------------------------------------ */
/* one measurement point                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    long trials;
    double recovery;
    double ci99;
    double iter_mean;
    double unres_mean;
    double visits_mean;
} point_res;

static void make_mask(sc_code *c, int p_mil, int model, crng *rng, uint8_t *mask)
{
    sc_info in;
    int v;
    sc_get_info(c, &in);
    if (model == SC_MODEL_SYSTEMATIC) {
        for (v = 0; v < in.n_data; v++)
            mask[v] = (uint8_t)(crng_mil(rng) < (uint32_t)p_mil);
        for (; v < in.n; v++)
            mask[v] = 0;
    } else {
        for (v = 0; v < in.n; v++)
            mask[v] = (uint8_t)(crng_mil(rng) < (uint32_t)p_mil);
    }
}

static point_res run_point(sc_code **codes, int ncodes, long trials, int p_mil,
                           int model, uint64_t seed, uint8_t *recv, uint8_t *out,
                           uint8_t *mask)
{
    point_res r;
    long done = 0, full = 0;
    double sum_unres = 0.0, sum_rounds = 0.0, sum_visits = 0.0;
    int k;

    memset(&r, 0, sizeof(r));
    for (k = 0; k < ncodes && done < trials; k++) {
        sc_info in;
        long per, t;
        crng rng, prng;
        uint8_t *code;
        long target;
        sc_get_info(codes[k], &in);
        target = (model == SC_MODEL_SYSTEMATIC) ? in.n_data : in.n;
        code = calloc((size_t)in.n, 1);
        if (!code)
            continue;
        /* One random codeword per instance; every trial keeps it and only
         * redraws the erasure mask.  recv is the codeword itself, so a
         * "recovered" bit must also match the transmitted value. */
        crng_seed(&prng, seed + (uint64_t)(k * 1000003) + 0x9e3779b9u);
        for (t = 0; t < in.n_data; t++)
            code[t] = (uint8_t)(crng_next(&prng) & 1u);
        (void)sc_encode(codes[k], code);
        memcpy(recv, code, (size_t)in.n);
        per = (trials - done) / (ncodes - k);
        if (per <= 0)
            per = 1;
        if (per > trials - done)
            per = trials - done;
        crng_seed(&rng, seed + (uint64_t)(k * 1000003) + (uint64_t)p_mil);
        for (t = 0; t < per; t++) {
            sc_dec_stat st;
            int ok;
            long v;
            make_mask(codes[k], p_mil, model, &rng, mask);
            st = sc_decode(codes[k], code, mask, out,
                           model == SC_MODEL_SYSTEMATIC);
            done++;
            ok = (st.unresolved == 0);
            if (ok)
                for (v = 0; v < target; v++)
                    if (out[v] != code[v]) {
                        ok = 0;
                        break;
                    }
            if (ok)
                full++;
            sum_unres += (double)st.unresolved / (double)st.target;
            sum_rounds += (double)st.rounds;
            sum_visits += (double)st.edge_visits;
        }
        free(code);
    }
    r.trials = done;
    r.recovery = done ? (double)full / (double)done : 0.0;
    r.ci99 = wilson99(r.recovery, done, &(double){0.0}, &(double){1.0});
    r.iter_mean = done ? sum_rounds / (double)done : 0.0;
    r.unres_mean = done ? sum_unres / (double)done : 0.0;
    r.visits_mean = done ? sum_visits / (double)done : 0.0;
    return r;
}

/* ------------------------------------------------------------------ */
/* throughput                                                          */
/* ------------------------------------------------------------------ */

static double payload_bytes(const sc_code *c)
{
    sc_info in;
    sc_get_info(c, &in);
    return (double)in.n_data / 8.0;
}

static void measure_enc(sc_code *c, double *mbps, long *reps_out)
{
    sc_info in;
    uint8_t *code;
    double t0, t1, dt;
    long reps, r;

    sc_get_info(c, &in);
    code = calloc((size_t)in.n, 1);
    if (!code) {
        *mbps = 0.0;
        *reps_out = 0;
        return;
    }
    for (r = 0; r < in.n_data; r++)
        code[r] = (uint8_t)((r * 2654435761u) >> 13);

    t0 = now_sec();
    sc_encode(c, code);
    t1 = now_sec();
    dt = t1 - t0;
    if (dt < 1e-7)
        dt = 1e-7;
    reps = (long)(0.20 / dt);
    if (reps < 1)
        reps = 1;
    if (reps > 200000)
        reps = 200000;

    t0 = now_sec();
    for (r = 0; r < reps; r++)
        sc_encode(c, code);
    t1 = now_sec();
    dt = t1 - t0;
    if (dt <= 0.0)
        dt = 1e-9;
    *mbps = payload_bytes(c) * (double)reps / dt / 1e6;
    *reps_out = reps;
    free(code);
}

static void measure_dec(sc_code *c, int p_mil, double *mbps, long *reps_out)
{
    sc_info in;
    uint8_t *code, *mask, *out, *recv;
    crng rng;
    double t0, t1, dt;
    long reps, r;
    int i;

    sc_get_info(c, &in);
    code = calloc((size_t)in.n, 1);
    mask = calloc((size_t)in.n, 1);
    out = calloc((size_t)in.n, 1);
    recv = calloc((size_t)in.n, 1);
    if (!code || !mask || !out || !recv) {
        free(code);
        free(mask);
        free(out);
        free(recv);
        *mbps = 0.0;
        *reps_out = 0;
        return;
    }
    for (i = 0; i < in.n_data; i++)
        code[i] = (uint8_t)(((i * 40503u) >> 7) & 1u);
    if (sc_systematic(c))
        (void)sc_encode(c, code);
    memcpy(recv, code, (size_t)in.n);
    crng_seed(&rng, 999331u);
    make_mask(c, p_mil, SC_MODEL_SYMMETRIC, &rng, mask);

    t0 = now_sec();
    (void)sc_decode(c, recv, mask, out, 1);
    t1 = now_sec();
    dt = t1 - t0;
    if (dt < 1e-7)
        dt = 1e-7;
    reps = (long)(0.20 / dt);
    if (reps < 1)
        reps = 1;
    if (reps > 200000)
        reps = 200000;

    t0 = now_sec();
    for (r = 0; r < reps; r++)
        (void)sc_decode(c, recv, mask, out, 1);
    t1 = now_sec();
    dt = t1 - t0;
    if (dt <= 0.0)
        dt = 1e-9;
    *mbps = payload_bytes(c) * (double)reps / dt / 1e6;
    *reps_out = reps;
    free(code);
    free(mask);
    free(out);
    free(recv);
}

/* ------------------------------------------------------------------ */
/* payload round trip                                                  */
/* ------------------------------------------------------------------ */

static long payload_check(sc_code *c, int p_mil, long seg_bytes, uint64_t seed,
                          long *codewords_out)
{
    sc_info in;
    long chunks, ch, bad = 0;
    uint8_t *code, *mask, *out, *recv;
    uint8_t *payload;
    crng rng;

    sc_get_info(c, &in);
    if (!sc_systematic(c))
        return -1;
    chunks = (seg_bytes * 8 + in.n_data - 1) / in.n_data;
    code = calloc((size_t)in.n, 1);
    mask = calloc((size_t)in.n, 1);
    out = calloc((size_t)in.n, 1);
    recv = calloc((size_t)in.n, 1);
    payload = calloc((size_t)chunks * (size_t)(in.n_data / 8) + 8, 1);
    if (!code || !mask || !out || !recv || !payload) {
        free(code);
        free(mask);
        free(out);
        free(recv);
        free(payload);
        return -1;
    }
    crng_seed(&rng, seed);
    for (ch = 0; ch < chunks * (in.n_data / 8); ch++)
        payload[ch] = (uint8_t)(crng_next(&rng) & 0xffu);

    for (ch = 0; ch < chunks; ch++) {
        int i;
        sc_dec_stat st;
        memset(code, 0, (size_t)in.n);
        for (i = 0; i < in.n_data; i++) {
            int byte = (int)(ch * (in.n_data / 8) + i / 8);
            code[i] = (uint8_t)((payload[byte] >> (i % 8)) & 1u);
        }
        if (sc_encode(c, code) != 0)
            break;
        if (sc_verify(c, code) != 0) {
            bad += in.n_data;
            break;
        }
        memcpy(recv, code, (size_t)in.n);
        make_mask(c, p_mil, SC_MODEL_SYSTEMATIC, &rng, mask);
        st = sc_decode(c, recv, mask, out, 1);
        (void)st;
        for (i = 0; i < in.n_data; i++) {
            int byte = (int)(ch * (in.n_data / 8) + i / 8);
            uint8_t want = (uint8_t)((payload[byte] >> (i % 8)) & 1u);
            if (out[i] != want)
                bad++;
        }
    }
    *codewords_out = chunks;
    free(code);
    free(mask);
    free(out);
    free(recv);
    free(payload);
    return bad;
}

/*
 * Diagnostic: where do the unresolved payload bits sit along the coupling
 * chain?  Prints one count per coupling block to stderr.
 */
static void block_diag(sc_code *c, int L, int p_mil, int model, uint64_t seed,
                       int trials)
{
    sc_info in;
    int m, t, v;
    long *hist;
    long total = 0, clean = 0;
    uint8_t *mask, *out, *recv;
    crng rng;

    sc_get_info(c, &in);
    if (L < 1)
        return;
    m = in.n_data / L;
    if (m < 1)
        return;
    hist = calloc((size_t)L, sizeof(long));
    mask = calloc((size_t)in.n, 1);
    out = calloc((size_t)in.n, 1);
    recv = calloc((size_t)in.n, 1);
    if (!hist || !mask || !out || !recv) {
        free(hist);
        free(mask);
        free(out);
        free(recv);
        return;
    }
    crng_seed(&rng, seed ^ 0x5bd1e995u);
    for (t = 0; t < trials; t++) {
        const uint8_t *kn;
        int nz = 0;
        make_mask(c, p_mil, model, &rng, mask);
        (void)sc_decode(c, recv, mask, out,
                        model == SC_MODEL_SYSTEMATIC);
        kn = sc_known_bits(c);
        for (v = 0; v < in.n_data; v++) {
            if (!kn[v]) {
                hist[v / m]++;
                nz++;
            }
        }
        total += nz;
        if (nz == 0)
            clean++;
    }
    fprintf(stderr, "diag: trials=%d clean=%ld unresolved_total=%ld per_block=",
            trials, clean, total);
    for (v = 0; v < L; v++)
        fprintf(stderr, "%s%ld", v ? ":" : "", hist[v]);
    fprintf(stderr, "\n");
    free(hist);
    free(mask);
    free(out);
    free(recv);
}

/* ------------------------------------------------------------------ */
/* one CSV row                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int block, L, rate_pct, trials, codes, extra_dc, term, model;
    double p;
    uint64_t seed;
    long seg_bytes;
} cfg;

static void print_header(FILE *f)
{
    fprintf(f, "block,L,rate,p,trials,recovery,ci99,iter_mean,enc_mbps,"
               "dec_mbps,latency_ms,mem_bytes\n");
}

static void print_row(FILE *f, const cfg *g, const point_res *r, double enc,
                      double dec, double lat_ms, size_t mem)
{
    fprintf(f, "%d,%d,%.2f,%.2f,%ld,%.6f,%.6f,%.2f,%.3f,%.3f,%.6f,%zu\n",
            g->block, g->L, (double)g->rate_pct / 100.0, g->p, r->trials,
            r->recovery, r->ci99, r->iter_mean, enc, dec, lat_ms, mem);
}

static void print_report_header(FILE *f)
{
    fprintf(f, "series,block,L,rate,p,trials,recovery,ci99,iter_mean,"
               "unresolved_mean,edge_visits_mean,enc_mbps,dec_mbps,"
               "latency_ms,mem_bytes,rate_actual,n,n_data,n_chk,edges\n");
}

static void print_report_row(FILE *f, const char *series, const cfg *g,
                             const point_res *r, const sc_info *in, double enc,
                             double dec, double lat_ms)
{
    fprintf(f, "%s,%d,%d,%.2f,%.2f,%ld,%.6f,%.6f,%.2f,%.6f,%.1f,%.3f,%.3f,"
               "%.6f,%zu,%.6f,%d,%d,%d,%ld\n",
            series, g->block, g->L, (double)g->rate_pct / 100.0, g->p,
            r->trials, r->recovery, r->ci99, r->iter_mean, r->unres_mean,
            r->visits_mean, enc, dec, lat_ms, in->mem_bytes, in->rate, in->n,
            in->n_data, in->n_chk, in->edges);
}

/* ------------------------------------------------------------------ */
/* one full point                                                      */
/* ------------------------------------------------------------------ */

static int run_cfg(const cfg *g, int want_report, FILE *rep, const char *series,
                   int do_payload, int do_diag, int do_dump)
{
    sc_code *codes[16];
    char err[160];
    int ncodes = g->codes;
    int k, p_mil;
    sc_info in;
    point_res r;
    uint8_t *recv, *out, *mask;
    double enc = 0.0, dec = 0.0, lat_ms;
    long reps_e = 0, reps_d = 0;

    if (ncodes < 1)
        ncodes = 1;
    if (ncodes > 16)
        ncodes = 16;
    p_mil = (int)llround(g->p * 1000000.0);

    for (k = 0; k < ncodes; k++) {
        sc_params p;
        p.block = g->block;
        p.L = g->L;
        p.rate_pct = g->rate_pct;
        p.term = g->term;
        p.extra_dc = g->extra_dc;
        p.seed = g->seed + (uint64_t)(k * 7919);
        codes[k] = sc_build(&p, err, sizeof(err));
        if (!codes[k]) {
            fprintf(stderr, "build failed: %s\n", err);
            while (k-- > 0)
                sc_free(codes[k]);
            return -1;
        }
    }
    sc_get_info(codes[0], &in);

    recv = calloc((size_t)in.n, 1);
    out = calloc((size_t)in.n, 1);
    mask = calloc((size_t)in.n, 1);
    if (!recv || !out || !mask) {
        for (k = 0; k < ncodes; k++)
            sc_free(codes[k]);
        free(recv);
        free(out);
        free(mask);
        return -1;
    }

    r = run_point(codes, ncodes, g->trials, p_mil, g->model, g->seed, recv, out,
                  mask);

    if (g->term != SC_TERM_NATIVE)
        measure_enc(codes[0], &enc, &reps_e);
    measure_dec(codes[0], p_mil, &dec, &reps_d);

    lat_ms = (double)g->L * (double)g->seg_bytes * 8.0 / 1e9 * 1000.0;

    print_row(stdout, g, &r, enc, dec, lat_ms, in.mem_bytes);
    if (want_report && rep)
        print_report_row(rep, series, g, &r, &in, enc, dec, lat_ms);

    if (do_payload) {
        long cw = 0;
        long bad = payload_check(codes[0], p_mil, g->seg_bytes, g->seed + 17, &cw);
        fprintf(stderr,
                "payload_check: bytes=%ld codewords=%ld bit_errors=%ld ok=%d\n",
                g->seg_bytes, cw, bad, bad == 0 ? 1 : 0);
    }
    if (do_diag)
        block_diag(codes[0], g->L, p_mil, g->model, g->seed + 29,
                   g->trials < 400 ? g->trials : 400);
    if (do_dump) {
        int vh[SC_DEG_MAX + 1], ch[SC_DEG_MAX + 1], d, first;
        sc_degrees(codes[0], vh, ch);
        fprintf(stderr, "matrix: term=%s n=%d data=%d par=%d chk=%d extra=%d "
                        "edges=%ld rate=%.5f\n",
                sc_term_name(codes[0]), in.n, in.n_data, in.n_par, in.n_chk,
                in.n_extra, in.edges, in.rate);
        fprintf(stderr, "vdeg:");
        first = 1;
        for (d = 0; d <= SC_DEG_MAX; d++)
            if (vh[d]) {
                fprintf(stderr, "%s%d:%d", first ? "" : " ", d, vh[d]);
                first = 0;
            }
        fprintf(stderr, "\ncdeg:");
        first = 1;
        for (d = 0; d <= SC_DEG_MAX; d++)
            if (ch[d]) {
                fprintf(stderr, "%s%d:%d", first ? "" : " ", d, ch[d]);
                first = 0;
            }
        fprintf(stderr, "\n");
    }

    fprintf(stderr,
            "point: block=%d L=%d rate=%.2f p=%.2f model=%s term=%s "
            "n=%d data=%d chk=%d edges=%ld rate_actual=%.5f mem=%zu "
            "enc=%.3fMB/s(%ld) dec=%.3fMB/s(%ld)\n",
            g->block, g->L, (double)g->rate_pct / 100.0, g->p,
            g->model == SC_MODEL_SYSTEMATIC ? "systematic" : "symmetric",
            sc_term_name(codes[0]), in.n, in.n_data,
            in.n_chk, in.edges, in.rate, in.mem_bytes, enc, reps_e, dec,
            reps_d);

    for (k = 0; k < ncodes; k++)
        sc_free(codes[k]);
    free(recv);
    free(out);
    free(mask);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
            "usage: sc_ldpc_bench --block N --L N --rate 0.30..0.70 --p 0.30 "
            "[--trials N] [options]\n"
            "  --model systematic|symmetric  channel model (default systematic)\n"
            "  --term acc|causal|native      boundary rule (default acc)\n"
            "  --extra-dc N                  extra check payload degree (5)\n"
            "  --codes N                     code instances (4)\n"
            "  --seed N                      seed (1)\n"
            "  --seg-bytes N                 payload bytes per coupling block "
            "for the latency model (8192)\n"
            "  --no-header                   omit the CSV header\n"
            "  --sweep                       run the 54-point grid\n"
            "  --full                        also symmetric, native and fine "
            "sweeps\n"
            "  --report FILE                 extended CSV output\n"
            "  --payload-check               verify a payload round trip "
            "(stderr)\n"
            "  --diag                        unresolved-bit histogram per "
            "block (stderr)\n"
            "  --dump                        matrix and degree histograms "
            "(stderr)\n");
}

int main(int argc, char **argv)
{
    cfg g;
    int i, header = 1, sweep = 0, full = 0, do_payload = 0, do_diag = 0;
    int do_dump = 0;
    const char *report_path = NULL;
    FILE *rep = NULL;

    memset(&g, 0, sizeof(g));
    g.block = 256;
    g.L = 8;
    g.rate_pct = 50;
    g.p = 0.30;
    g.trials = 2000;
    g.codes = 4;
    g.extra_dc = 5;
    g.term = SC_TERM_ACC;
    g.model = SC_MODEL_SYSTEMATIC;
    g.seed = 1;
    g.seg_bytes = 8192;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else if (!strcmp(a, "--no-header")) {
            header = 0;
        } else if (!strcmp(a, "--sweep")) {
            sweep = 1;
        } else if (!strcmp(a, "--full")) {
            sweep = 1;
            full = 1;
        } else if (!strcmp(a, "--payload-check")) {
            do_payload = 1;
        } else if (!strcmp(a, "--diag")) {
            do_diag = 1;
        } else if (!strcmp(a, "--dump")) {
            do_dump = 1;
        } else if (!strcmp(a, "--block") && i + 1 < argc) {
            g.block = (int)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--L") && i + 1 < argc) {
            g.L = (int)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--rate") && i + 1 < argc) {
            g.rate_pct = (int)llround(strtod(argv[++i], NULL) * 100.0);
        } else if (!strcmp(a, "--p") && i + 1 < argc) {
            g.p = strtod(argv[++i], NULL);
        } else if (!strcmp(a, "--trials") && i + 1 < argc) {
            g.trials = (int)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--codes") && i + 1 < argc) {
            g.codes = (int)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--extra-dc") && i + 1 < argc) {
            g.extra_dc = (int)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--seed") && i + 1 < argc) {
            g.seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--seg-bytes") && i + 1 < argc) {
            g.seg_bytes = (long)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--model") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "systematic"))
                g.model = SC_MODEL_SYSTEMATIC;
            else if (!strcmp(m, "symmetric"))
                g.model = SC_MODEL_SYMMETRIC;
            else {
                usage();
                return 2;
            }
        } else if (!strcmp(a, "--term") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "causal"))
                g.term = SC_TERM_ACC;
            else if (!strcmp(m, "native"))
                g.term = SC_TERM_NATIVE;
            else {
                usage();
                return 2;
            }
        } else if (!strcmp(a, "--report") && i + 1 < argc) {
            report_path = argv[++i];
        } else {
            fprintf(stderr, "unknown or incomplete argument: %s\n", a);
            usage();
            return 2;
        }
    }

    if (g.trials < 1)
        g.trials = 1;
    if (g.p < 0.0)
        g.p = 0.0;
    if (g.p > 1.0)
        g.p = 1.0;

    if (report_path) {
        long sz;
        rep = fopen(report_path, "a");
        if (!rep) {
            fprintf(stderr, "cannot open %s\n", report_path);
            return 1;
        }
        fseek(rep, 0, SEEK_END);
        sz = ftell(rep);
        if (sz == 0)
            print_report_header(rep);
    }
    if (header)
        print_header(stdout);

    if (!sweep) {
        int rc = run_cfg(&g, rep != NULL, rep, "point", do_payload, do_diag,
                         do_dump);
        if (rep)
            fclose(rep);
        return rc == 0 ? 0 : 1;
    }

    {
        static const int blocks[2] = {256, 512};
        static const int Ls[3] = {8, 16, 32};
        static const int rates[3] = {70, 60, 50};
        static const double ps[3] = {0.30, 0.40, 0.50};
        int bi, li, ri, pi;
        cfg s = g;

        for (bi = 0; bi < 2; bi++) {
            for (li = 0; li < 3; li++) {
                for (ri = 0; ri < 3; ri++) {
                    for (pi = 0; pi < 3; pi++) {
                        s.block = blocks[bi];
                        s.L = Ls[li];
                        s.rate_pct = rates[ri];
                        s.p = ps[pi];
                        s.model = SC_MODEL_SYSTEMATIC;
                        s.term = SC_TERM_ACC;
                        if (run_cfg(&s, rep != NULL, rep, "pcurve_b", 0, 0, 0) != 0)
                            return 1;
                    }
                }
            }
        }

        if (full) {
            /* symmetric channel on the same causal codes */
            for (bi = 0; bi < 2; bi++) {
                for (li = 0; li < 3; li++) {
                    for (ri = 0; ri < 3; ri++) {
                        for (pi = 0; pi < 3; pi++) {
                            s.block = blocks[bi];
                            s.L = Ls[li];
                            s.rate_pct = rates[ri];
                            s.p = ps[pi];
                            s.model = SC_MODEL_SYMMETRIC;
                            s.term = SC_TERM_ACC;
                            if (run_cfg(&s, rep != NULL, rep, "pcurve_a", 0, 0, 0) != 0)
                                return 1;
                        }
                    }
                }
            }
            /* literal npminh12 termination, symmetric channel */
            for (bi = 0; bi < 2; bi++) {
                for (li = 0; li < 3; li++) {
                    for (pi = 0; pi < 3; pi++) {
                        s.block = blocks[bi];
                        s.L = Ls[li];
                        s.rate_pct = 50;
                        s.p = ps[pi];
                        s.model = SC_MODEL_SYMMETRIC;
                        s.term = SC_TERM_NATIVE;
                        if (run_cfg(&s, rep != NULL, rep, "native_a", 0, 0, 0) != 0)
                            return 1;
                    }
                }
            }
            /* fine p sweep on the symmetric channel, block 512 */
            for (ri = 0; ri < 3; ri++) {
                for (li = 0; li < 3; li++) {
                    int qi;
                    for (qi = 0; qi <= 8; qi++) {
                        s.block = 512;
                        s.L = Ls[li];
                        s.rate_pct = rates[ri];
                        s.p = 0.10 + 0.05 * (double)qi;
                        s.model = SC_MODEL_SYMMETRIC;
                        s.term = SC_TERM_ACC;
                        if (run_cfg(&s, rep != NULL, rep, "fine_a", 0, 0, 0) != 0)
                            return 1;
                    }
                }
            }
        }
    }

    if (rep)
        fclose(rep);
    return 0;
}
