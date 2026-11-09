/* test_fec_sclc_ladder.c - downlink 256-share rate ladder and controller.
 *
 * Pins the six shipped splits, the encoder/decoder roundtrip and one-share
 * repair at every point, the erasure threshold at each point's controller
 * boundary, and the control law's suggestion shapes for a 0..45 percent
 * loss sweep. Every suggested shape is built and exercised, so the host
 * sanitizer build also covers the per-rate allocation and graph bounds.
 */
#include "tbt_fec_adapt_sclc.h"
#include "tbt_fec_sclc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SHARE TBT_SCLC_SHARE
#define LEVELS TBT_FEC_ADAPT_SCLC_LEVELS

static int failures;

static void check(int ok, const char *what)
{
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static const struct ladder
{
    u8 pct;
    u16 n;
    u16 m;
    u32 boundary_ppm; /* highest DATA-share loss that still picks this rate */
    double min_recovery;
} ladder[LEVELS] = {
    {25, 64, 192, 550000u, 0.99},
    {30, 76, 180, 500000u, 0.99},
    {40, 102, 154, 400000u, 0.99},
    {50, 128, 128, 300000u, 0.99},
    {60, 151, 105, 207407u, 0.99},
    {70, 178, 78, 104348u, 0.80},
};

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

static u8 *alloc_shares(int n)
{
    u8 *p = calloc((size_t)n, SHARE);

    if (!p)
    {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

static u8 **share_ptrs(u8 *base, int n)
{
    u8 **p = calloc((size_t)n, sizeof(*p));
    int i;

    if (!p)
    {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    for (i = 0; i < n; i++)
        p[i] = base + (size_t)i * SHARE;
    return p;
}

static u16 rate_of(const struct ladder *r)
{
    return (u16)((u32)r->n * 1000u / (u32)(r->n + r->m));
}

static void *new_codec(const struct ladder *r, u64 nonce)
{
    return tbt_fec_ops_sclc.new(r->n, r->m, rate_of(r), TBT_SCLC_BLOCK,
                                SHARE, 0, nonce);
}

static void test_params(void)
{
    int i;

    for (i = 0; i < LEVELS; i++)
    {
        void *c = new_codec(&ladder[i], 0x100 + (u64)i);

        check(c != NULL, "ladder: codec builds");
        if (!c)
            continue;
        check(tbt_fec_ops_sclc.parity_count(c, ladder[i].n) == ladder[i].m,
              "ladder: parity count");
        tbt_fec_ops_sclc.free(c);
    }
    check(tbt_fec_ops_sclc.new(64, 191, 250, TBT_SCLC_BLOCK, SHARE, 0, 1) ==
              NULL,
          "ladder: n+m must be 256");
    check(tbt_fec_ops_sclc.new(64, 192, 500, TBT_SCLC_BLOCK, SHARE, 0, 1) ==
              NULL,
          "ladder: k must match n/m");
    check(tbt_fec_ops_sclc.new(64, 192, 250, 512, SHARE, 0, 1) == NULL,
          "ladder: block 256 only");
    check(tbt_fec_ops_sclc.new(64, 192, 250, TBT_SCLC_BLOCK, SHARE - 1, 0,
                               1) == NULL,
          "ladder: share size");
    check(tbt_fec_ops_sclc.new(64, 192, 250, TBT_SCLC_BLOCK, SHARE, 1, 1) ==
              NULL,
          "ladder: d must be zero");
}

static void test_roundtrip(void)
{
    int i;

    for (i = 0; i < LEVELS; i++)
    {
        const struct ladder *r = &ladder[i];
        void *c = new_codec(r, 0x200 + (u64)i);
        u8 *data = alloc_shares(r->n), *parity = alloc_shares(r->m);
        u8 **dp = share_ptrs(data, r->n), **pp = share_ptrs(parity, r->m);
        u8 *present = calloc((size_t)r->n + r->m, 1);
        u8 *orig = malloc(SHARE);
        int j, rc;

        if (!c || !present || !orig)
        {
            check(0, "ladder: roundtrip allocation");
            exit(1);
        }
        for (j = 0; j < r->n; j++)
            fill_random(data + (size_t)j * SHARE, SHARE);
        rc = tbt_fec_ops_sclc.encode(c, dp, r->n, pp);
        check(rc == 0, "ladder: encode");
        memset(present, 1, (size_t)r->n + r->m);
        rc = tbt_fec_ops_sclc.decode(c, dp, pp, present, present + r->n,
                                     r->n);
        check(rc == 0, "ladder: full decode");

        memcpy(orig, data + (size_t)3 * SHARE, SHARE);
        memset(data + (size_t)3 * SHARE, 0x5A, SHARE);
        memset(present, 1, (size_t)r->n + r->m);
        present[3] = 0;
        rc = tbt_fec_ops_sclc.decode(c, dp, pp, present, present + r->n,
                                     r->n);
        check(rc == 1, "ladder: one data share repaired");
        check(!memcmp(orig, data + (size_t)3 * SHARE, SHARE),
              "ladder: repaired share is byte-exact");

        free(orig);
        free(present);
        free(dp);
        free(pp);
        free(data);
        free(parity);
        tbt_fec_ops_sclc.free(c);
    }
}

/* Share-level recovery over random DATA and parity losses. A share is
 * delivered only when all 128 batches recover, so this is a strict lower
 * bound on the codeword decoder and matches what the data plane sees. */
static double erasure_run(const struct ladder *r, u32 p_ppm, int blocks)
{
    void *c = new_codec(r, 0x300 + (u64)r->pct);
    u8 *data = alloc_shares(r->n), *parity = alloc_shares(r->m);
    u8 **dp = share_ptrs(data, r->n), **pp = share_ptrs(parity, r->m);
    u8 *data_present = calloc(r->n, 1), *parity_present = calloc(r->m, 1);
    u8 *truth = alloc_shares(r->n);
    u64 missing = 0, recovered = 0;
    int b, i;

    if (!c)
    {
        check(0, "ladder: erasure codec");
        return 0.0;
    }
    for (b = 0; b < blocks; b++)
    {
        for (i = 0; i < r->n; i++)
            fill_random(data + (size_t)i * SHARE, SHARE);
        memcpy(truth, data, (size_t)r->n * SHARE);
        tbt_fec_ops_sclc.encode(c, dp, r->n, pp);
        for (i = 0; i < r->n; i++)
            data_present[i] = (rnd64() % 1000000u) >= p_ppm;
        for (i = 0; i < r->m; i++)
            parity_present[i] = (rnd64() % 1000000u) >= p_ppm;
        for (i = 0; i < r->n; i++)
            if (!data_present[i])
                missing++;
        (void)tbt_fec_ops_sclc.decode(c, dp, pp, data_present,
                                      parity_present, r->n);
        for (i = 0; i < r->n; i++)
            if (!data_present[i] && tbt_fec_ops_sclc.is_known(c, (u32)i))
            {
                recovered++;
                if (memcmp(truth + (size_t)i * SHARE,
                           data + (size_t)i * SHARE, SHARE))
                {
                    check(0, "ladder: recovered share differs from truth");
                    free(truth);
                    return 0.0;
                }
            }
    }
    free(truth);
    free(data_present);
    free(parity_present);
    free(dp);
    free(pp);
    free(data);
    free(parity);
    tbt_fec_ops_sclc.free(c);
    return missing ? (double)recovered / (double)missing : 1.0;
}

static void test_erasure_thresholds(void)
{
    int i;

    for (i = 0; i < LEVELS; i++)
    {
        const struct ladder *r = &ladder[i];
        int blocks = r->pct == 70 ? 16 : 8;
        double got = erasure_run(r, r->boundary_ppm, blocks);

        printf("rate %u at p=%.4f: share recovery %.6f (min %.2f)\n",
               (unsigned)r->pct, (double)r->boundary_ppm / 1e6, got,
               r->min_recovery);
        check(got >= r->min_recovery, "ladder: erasure threshold");
    }
}

/* ------------------------------------------------------------------ *
 * Control law: a fresh controller starts at rate 50 and sees one 250 ms
 * sample. Fast start may move several steps at once, so 0..45 percent
 * covers every raise and both fast-down shapes.
 * ------------------------------------------------------------------ */

static void snap(struct tbt_fec_adapt_sclc_est_out *o, u32 p_ppm)
{
    memset(o, 0, sizeof(*o));
    o->p_ppm = p_ppm;
    o->p_ewma_ppm = p_ppm;
    o->p_win_ppm = p_ppm;
    o->p_fast_ppm = p_ppm;
    o->r_fail_ppm = 0;
    o->r_valid = true;
    o->p_valid = true;
    o->sent_win = 4096;
    o->supers_win = 16;
    o->alive = 256;
    o->carriers = 256;
}

static void test_controller_switch(void)
{
    static const struct
    {
        u32 p_ppm;
        u8 to;
    } cases[] = {
        {0u, 5}, {100000u, 5}, {200000u, 4}, {300000u, 3}, {350000u, 2},
        {450000u, 1},
    };
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    struct tbt_fec_adapt_sclc_est_out o;
    u8 idx, mask;
    u32 i;

    check(tbt_fec_adapt_sclc_init_plan(128, 128, 1, TBT_FEC_SCLC_DOWN, &idx,
                                       &mask) == 0,
          "ctl: initial plan");
    check(idx == 3, "ctl: initial index");
    check(mask == (u8)((1u << LEVELS) - 1u), "ctl: downlink full ladder");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        void *codec;

        memset(&c, 0, sizeof(c));
        check(tbt_fec_adapt_sclc_ctl_init(&c, true, 50, 256, 1) == 0,
              "ctl: init");
        tbt_fec_adapt_sclc_ctl_set_rate_mask(&c, mask);
        tbt_fec_adapt_sclc_ctl_set_epoch(&c, 1);
        snap(&o, cases[i].p_ppm);
        tbt_fec_adapt_sclc_ctl_eval(&c, &o, 1000000000ull, &out);
        check(out.to == cases[i].to, "ctl: target rate");
        if (cases[i].to == 3)
            check(!out.switch_now, "ctl: the start rate is already the target");
        else
            check(out.switch_now, "ctl: fast start switches");
        check(out.shape.n == ladder[out.to].n &&
                  out.shape.m == ladder[out.to].m,
              "ctl: suggested n/m");
        codec = tbt_fec_ops_sclc.new(out.shape.n, out.shape.m,
                                     (u16)((u32)out.shape.n * 1000u / 256u),
                                     TBT_SCLC_BLOCK, SHARE, 0,
                                     0x400 + (u64)i);
        check(codec != NULL, "ctl: suggested shape builds");
        if (codec)
        {
            u8 *data = alloc_shares(out.shape.n);
            u8 *parity = alloc_shares(out.shape.m);
            u8 **dp = share_ptrs(data, out.shape.n);
            u8 **pp = share_ptrs(parity, out.shape.m);
            u8 *present = calloc((size_t)out.shape.n + out.shape.m, 1);
            int j;

            for (j = 0; j < out.shape.n; j++)
                fill_random(data + (size_t)j * SHARE, SHARE);
            memset(present, 1, (size_t)out.shape.n + out.shape.m);
            check(tbt_fec_ops_sclc.encode(codec, dp, out.shape.n, pp) == 0,
                  "ctl: suggested shape encodes");
            present[0] = 0;
            check(tbt_fec_ops_sclc.decode(codec, dp, pp, present,
                                          present + out.shape.n,
                                          out.shape.n) == 1,
                  "ctl: suggested shape repairs");
            free(present);
            free(dp);
            free(pp);
            free(data);
            free(parity);
            tbt_fec_ops_sclc.free(codec);
        }
    }
}

int main(void)
{
    test_params();
    test_roundtrip();
    test_erasure_thresholds();
    test_controller_switch();
    if (failures)
    {
        fprintf(stderr, "test_fec_sclc_ladder: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_fec_sclc_ladder: OK\n");
    return 0;
}
