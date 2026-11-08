/*
 * sc_enc64_test.c - bit-exact acceptance test for the bit-sliced encoder.
 *
 * For every configuration and payload pattern the test:
 *   1. encodes 64 codewords with the scalar reference sc_encode(),
 *   2. packs the same payloads into 64-bit planes, runs sc_enc64_run(),
 *   3. unpacks the parity planes and compares every parity bit of every
 *      codeword with the reference (the payload must be untouched),
 *   4. checks every codeword with sc_verify(),
 *   5. checks sc_pack64()/sc_unpack64() against a naive bit transpose,
 *   6. when the SSE2 kernel is available, compares two independent 64-word
 *      lanes against the scalar kernel.
 *
 * Any mismatch is a failure; the exit status is nonzero.
 */

#include "sc_enc64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_state = 0x243f6a8885a308d3ULL;

static unsigned long long rng_next(void)
{
    unsigned long long x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

typedef struct {
    int block;
    int L;
    int rate_pct;
} cfg;

static const cfg configs[] = {
    {256, 8, 50},  {256, 16, 50}, {512, 16, 50},
    {256, 16, 40}, {512, 16, 40}, {256, 16, 30},
    {512, 16, 30}, {256, 8, 30},  {256, 16, 60},
    {256, 16, 70}, {256, 16, 25}, {1024, 16, 50},
};
#define NCONFIG ((int)(sizeof(configs) / sizeof(configs[0])))

enum { PAT_RANDOM, PAT_ZERO, PAT_ONE, PAT_ALT, PAT_BIT };
#define NPAT 5

static const char *pat_name(int p)
{
    static const char *names[NPAT] = {"random", "zero", "one", "alt", "bit"};
    return names[p];
}

/* Fill 64 payload codewords, byte-per-bit, with pattern p. */
static void make_payload(uint8_t *pay, int n_data, int p)
{
    int b, k;

    for (b = 0; b < SC_ENC64_BATCH; b++) {
        uint8_t *row = pay + (size_t)b * (size_t)n_data;
        for (k = 0; k < n_data; k++) {
            int v;
            switch (p) {
            case PAT_RANDOM:
                v = (int)(rng_next() & 1ULL);
                break;
            case PAT_ZERO:
                v = 0;
                break;
            case PAT_ONE:
                v = 1;
                break;
            case PAT_ALT:
                v = (b + k) & 1;
                break;
            default: /* PAT_BIT: variable k repeats the codeword bit */
                v = (k >> (b % 3)) & 1;
                break;
            }
            row[k] = (uint8_t)v;
        }
    }
}

/* Naive pack: 64 byte-per-bit rows -> 64-bit planes. */
static void pack_ref(uint64_t *out, const uint8_t *pay, int n_data)
{
    int b, k;

    for (k = 0; k < n_data; k++) {
        uint64_t w = 0;
        for (b = 0; b < SC_ENC64_BATCH; b++)
            w |= (uint64_t)(pay[(size_t)b * (size_t)n_data + k] & 1) << b;
        out[k] = w;
    }
}

/* 64 byte-per-bit rows -> bit-packed bytes, codeword-major, LSB first. */
static void rows_to_bytes(uint8_t *out, const uint8_t *pay, int n_data)
{
    int stride = n_data / 8;
    int b, k;

    memset(out, 0, (size_t)SC_ENC64_BATCH * (size_t)stride);
    for (b = 0; b < SC_ENC64_BATCH; b++)
        for (k = 0; k < n_data; k++)
            out[(size_t)b * (size_t)stride + (size_t)(k / 8)] |=
                (uint8_t)((pay[(size_t)b * (size_t)n_data + k] & 1) << (k % 8));
}

static int pack_check(int n_data, int p)
{
    uint8_t *pay, *bytes, *back;
    uint64_t *ref, *fast;
    int b, k, stride = n_data / 8;
    int bad = 0;

    pay = malloc((size_t)SC_ENC64_BATCH * (size_t)n_data);
    bytes = malloc((size_t)SC_ENC64_BATCH * (size_t)stride);
    back = malloc((size_t)SC_ENC64_BATCH * (size_t)stride);
    ref = malloc((size_t)n_data * sizeof(uint64_t));
    fast = malloc((size_t)n_data * sizeof(uint64_t));
    if (!pay || !bytes || !back || !ref || !fast) {
        free(pay);
        free(bytes);
        free(back);
        free(ref);
        free(fast);
        return -1;
    }
    make_payload(pay, n_data, p);
    pack_ref(ref, pay, n_data);
    rows_to_bytes(bytes, pay, n_data);
    memset(fast, 0, (size_t)n_data * sizeof(uint64_t));
    sc_pack64(fast, bytes, n_data);
    for (k = 0; k < n_data; k++)
        if (fast[k] != ref[k])
            bad++;
    memset(back, 0, (size_t)SC_ENC64_BATCH * (size_t)stride);
    sc_unpack64(back, fast, n_data);
    for (b = 0; b < SC_ENC64_BATCH; b++)
        for (k = 0; k < stride; k++)
            if (back[(size_t)b * (size_t)stride + k] !=
                bytes[(size_t)b * (size_t)stride + k])
                bad++;
    free(pay);
    free(bytes);
    free(back);
    free(ref);
    free(fast);
    return bad;
}

static int run_config(const cfg *g, int p, long *exact_total)
{
    sc_params sp;
    sc_code *c;
    sc_info in;
    sc_enc64 *e;
    char err[160];
    uint8_t *pay = NULL, *code = NULL, *par_ref = NULL;
    uint64_t *v = NULL;
    int b, pv, exact = 0, verified = 0, bad = 0;
    int n, n_data, n_par;

    memset(&sp, 0, sizeof(sp));
    sp.block = g->block;
    sp.L = g->L;
    sp.rate_pct = g->rate_pct;
    sp.term = SC_TERM_ACC;
    sp.extra_dc = 5;
    sp.seed = 0x51ed270bU + (uint64_t)(g->block * 131 + g->L * 17 +
                                        g->rate_pct);

    c = sc_build(&sp, err, sizeof(err));
    if (!c) {
        printf("FAIL build block=%d L=%d rate=%.2f: %s\n", g->block, g->L,
               g->rate_pct / 100.0, err);
        return 1;
    }
    e = sc_enc64_build(c, err, sizeof(err));
    if (!e) {
        printf("FAIL plan block=%d L=%d rate=%.2f: %s\n", g->block, g->L,
               g->rate_pct / 100.0, err);
        sc_free(c);
        return 1;
    }
    sc_get_info(c, &in);
    n = in.n;
    n_data = in.n_data;
    n_par = in.n_par;
    if (sc_enc64_n(e) != n || sc_enc64_n_data(e) != n_data ||
        sc_enc64_n_par(e) != n_par) {
        printf("FAIL plan dimensions\n");
        bad = 1;
        goto out;
    }

    pay = malloc((size_t)SC_ENC64_BATCH * (size_t)n_data);
    code = malloc((size_t)n);
    par_ref = malloc((size_t)SC_ENC64_BATCH * (size_t)n_par);
    v = calloc((size_t)n, sizeof(uint64_t));
    if (!pay || !code || !par_ref || !v) {
        printf("FAIL out of memory\n");
        bad = 1;
        goto out;
    }

    make_payload(pay, n_data, p);

    /* Reference: scalar sc_encode per codeword, keep the parity bytes. */
    for (b = 0; b < SC_ENC64_BATCH; b++) {
        memset(code, 0, (size_t)n);
        memcpy(code, pay + (size_t)b * (size_t)n_data, (size_t)n_data);
        if (sc_encode(c, code) != 0) {
            printf("FAIL sc_encode\n");
            bad = 1;
            goto out;
        }
        memcpy(par_ref + (size_t)b * (size_t)n_par, code + n_data,
               (size_t)n_par);
    }

    /* Bit-sliced: pack, encode, unpack and compare bit for bit. */
    memset(v, 0, (size_t)n * sizeof(uint64_t));
    pack_ref(v, pay, n_data);
    sc_enc64_run(e, v);
    for (b = 0; b < SC_ENC64_BATCH; b++) {
        int same = 1;
        memset(code, 0, (size_t)n);
        memcpy(code, pay + (size_t)b * (size_t)n_data, (size_t)n_data);
        for (pv = 0; pv < n_par; pv++)
            code[n_data + pv] = (uint8_t)((v[n_data + pv] >> b) & 1u);
        for (pv = 0; pv < n_par; pv++)
            if (code[n_data + pv] !=
                par_ref[(size_t)b * (size_t)n_par + pv])
                same = 0;
        if (same)
            exact++;
        if (sc_verify(c, code) == 0)
            verified++;
    }

    bad += pack_check(n_data, p);
    if (exact != SC_ENC64_BATCH || verified != SC_ENC64_BATCH)
        bad = 1;
    *exact_total += exact;

    printf("%s block=%3d L=%2d rate=%.2f pat=%-6s exact=%d/%d "
           "verify=%d/%d xors=%ld pack=%s\n",
           bad ? "FAIL" : "ok  ", g->block, g->L, g->rate_pct / 100.0,
           pat_name(p), exact, SC_ENC64_BATCH, verified, SC_ENC64_BATCH,
           sc_enc64_xors(e), bad ? "bad" : "ok");

out:
    free(pay);
    free(code);
    free(par_ref);
    free(v);
    sc_enc64_free(e);
    sc_free(c);
    return bad;
}

/*
 * SSE2: two independent 64-word lanes in one 128-bit word must reproduce the
 * scalar kernel on both lanes.
 */
static int sse2_check(const cfg *g)
{
    sc_params sp;
    sc_code *c;
    sc_info in;
    sc_enc64 *e;
    char err[160];
    uint8_t *pay = NULL;
    uint64_t *a = NULL, *b = NULL, *sa = NULL, *sb = NULL;
    void *raw = NULL, *w = NULL;
    int k, n, n_data, n_par, bad = 0, rc, unavailable = 0;

    memset(&sp, 0, sizeof(sp));
    sp.block = g->block;
    sp.L = g->L;
    sp.rate_pct = g->rate_pct;
    sp.term = SC_TERM_ACC;
    sp.extra_dc = 5;
    sp.seed = 0x9e3779b9U + (uint64_t)g->block;

    c = sc_build(&sp, err, sizeof(err));
    if (!c)
        return 1;
    e = sc_enc64_build(c, err, sizeof(err));
    if (!e) {
        sc_free(c);
        return 1;
    }
    sc_get_info(c, &in);
    n = in.n;
    n_data = in.n_data;
    n_par = in.n_par;

    pay = malloc((size_t)SC_ENC64_BATCH * (size_t)n_data);
    a = calloc((size_t)n, sizeof(uint64_t));
    b = calloc((size_t)n, sizeof(uint64_t));
    sa = calloc((size_t)n, sizeof(uint64_t));
    sb = calloc((size_t)n, sizeof(uint64_t));
    raw = malloc((size_t)n * 16 + 15);
    if (raw)
        w = (void *)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
    if (!pay || !a || !b || !sa || !sb || !w) {
        bad = 1;
        goto out;
    }
    memset(w, 0, (size_t)n * 16);

    make_payload(pay, n_data, PAT_RANDOM);
    pack_ref(a, pay, n_data);
    make_payload(pay, n_data, PAT_BIT);
    pack_ref(b, pay, n_data);
    memcpy(sa, a, (size_t)n_data * sizeof(uint64_t));
    memcpy(sb, b, (size_t)n_data * sizeof(uint64_t));
    sc_enc64_run(e, sa);
    sc_enc64_run(e, sb);
    for (k = 0; k < n_data; k++) {
        ((uint64_t *)w)[2 * k] = a[k];
        ((uint64_t *)w)[2 * k + 1] = b[k];
    }
    rc = sc_enc128_run_sse2(e, w);
    if (rc != 0) {
        unavailable = 1; /* kernel not built with SSE2 */
        goto out;
    }
    for (k = n_data; k < n; k++) {
        if (((uint64_t *)w)[2 * k] != sa[k])
            bad++;
        if (((uint64_t *)w)[2 * k + 1] != sb[k])
            bad++;
    }
    printf("%s sse2 block=%3d L=%2d rate=%.2f lanes=%d/%d parity_words=%d\n",
           bad ? "FAIL" : "ok  ", g->block, g->L, g->rate_pct / 100.0,
           bad ? 0 : SC_ENC64_BATCH, bad ? 0 : SC_ENC64_BATCH, n_par);
out:
    free(pay);
    free(a);
    free(b);
    free(sa);
    free(sb);
    free(raw);
    sc_enc64_free(e);
    sc_free(c);
    if (unavailable)
        return 2;
    return bad ? 1 : 0;
}

int main(void)
{
    long exact_total = 0;
    int i, p, fails = 0, sse2_fails = 0, sse2_pass = 0, sse2_runs = 0;
    int sse2_avail = 1;

    printf("sc_enc64_test: batch=%d configs=%d patterns=%d\n", SC_ENC64_BATCH,
           NCONFIG, NPAT);
    for (i = 0; i < NCONFIG; i++)
        for (p = 0; p < NPAT; p++)
            fails += run_config(&configs[i], p, &exact_total);

    for (i = 0; i < NCONFIG; i++) {
        int r = sse2_check(&configs[i]);
        if (r == 2) {
            sse2_avail = 0;
            break;
        }
        sse2_runs++;
        if (r)
            sse2_fails++;
        else
            sse2_pass++;
    }
    if (!sse2_avail)
        printf("sse2 kernel not available in this build\n");

    printf("summary: codewords_exact=%ld/%ld configs=%d patterns=%d "
           "failures=%d sse2_configs=%d/%d\n",
           exact_total, (long)NCONFIG * NPAT * SC_ENC64_BATCH, NCONFIG, NPAT,
           fails, sse2_pass, sse2_runs);
    if (fails || sse2_fails) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
