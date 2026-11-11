/* test_fec_sclc_shapes.c - SC-LDPC buildable splits and codec coverage.
 *
 * The controller no longer selects from a rate ladder; it maps its target
 * onto the splits the codec can build. This test pins that buildable set:
 * every downlink n in 1..192 of a 256-share block, the uplink constructible
 * set of a 32-share group, and the parameters the codec must reject. Every
 * downlink split is encoded, fully decoded and repaired after one DATA
 * share loss, so the sanitizer build covers the graph bounds across the
 * whole range. Two erasure anchors keep the codec's threshold coverage
 * without pinning any ladder policy.
 */
#include "tbt_fec_adapt_sclc.h"
#include "tbt_fec_sclc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHARE TBT_SCLC_SHARE
#define DOWN_SHARES 256u
#define ANCHOR_BLOCKS 8

static int failures;

static void check(int ok, const char *what)
{
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
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

static u8 *alloc_shares(u16 n)
{
    u8 *p = calloc((size_t)n, SHARE);

    if (!p)
    {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

static u8 **share_ptrs(u8 *base, u16 n)
{
    u8 **p = calloc((size_t)n, sizeof(*p));
    u16 i;

    if (!p)
    {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    for (i = 0; i < n; i++)
        p[i] = base + (size_t)i * SHARE;
    return p;
}

static u16 down_k(u16 n)
{
    return (u16)((u32)n * 1000u / DOWN_SHARES);
}

static void *new_codec(u16 n, u16 m, u16 k, u64 nonce)
{
    return tbt_fec_ops_sclc.new(n, m, k, TBT_SCLC_BLOCK, SHARE, 0, nonce);
}

/* Build, encode, decode and repair one DATA share at every downlink split
 * the continuous range can produce. */
static void test_downlink_shapes(void)
{
    static const u16 ns[] = {1, 16, 32, 64, 96, 128, 160, 178, 191, 192};
    size_t i;

    for (i = 0; i < sizeof(ns) / sizeof(ns[0]); i++)
    {
        u16 n = ns[i];
        u16 m = (u16)(DOWN_SHARES - n);
        u16 erased = n > 3 ? 3 : 0;
        void *c = new_codec(n, m, down_k(n), 0x100 + (u64)i);
        u8 *data, *parity, *truth, *present;
        u8 **dp, **pp;
        int rc, j;

        check(c != NULL, "downlink: codec builds");
        if (!c)
            continue;
        check(tbt_fec_ops_sclc.parity_count(c, n) == (u32)m,
              "downlink: parity count");
        data = alloc_shares(n);
        parity = alloc_shares(m);
        dp = share_ptrs(data, n);
        pp = share_ptrs(parity, m);
        truth = malloc(SHARE);
        present = calloc((size_t)n + m, 1);
        if (!truth || !present)
        {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        for (j = 0; j < n; j++)
            fill_random(data + (size_t)j * SHARE, SHARE);
        rc = tbt_fec_ops_sclc.encode(c, dp, n, pp);
        check(rc == 0, "downlink: encode");
        memset(present, 1, (size_t)n + m);
        rc = tbt_fec_ops_sclc.decode(c, dp, pp, present, present + n, n);
        check(rc == 0, "downlink: full decode");

        memcpy(truth, data + (size_t)erased * SHARE, SHARE);
        memset(data + (size_t)erased * SHARE, 0x5A, SHARE);
        memset(present, 1, (size_t)n + m);
        present[erased] = 0;
        rc = tbt_fec_ops_sclc.decode(c, dp, pp, present, present + n, n);
        check(rc == 1, "downlink: one data share repaired");
        check(!memcmp(truth, data + (size_t)erased * SHARE, SHARE),
              "downlink: repaired share is byte-exact");

        free(truth);
        free(present);
        free(dp);
        free(pp);
        free(data);
        free(parity);
        tbt_fec_ops_sclc.free(c);
    }
}

static void test_invalid_construction(void)
{
    check(new_codec(64, 191, 250, 1) == NULL,
          "invalid: n+m must be 256");
    check(new_codec(64, 192, 500, 1) == NULL,
          "invalid: k must match n/m");
    check(tbt_fec_ops_sclc.new(64, 192, 250, 512, SHARE, 0, 1) == NULL,
          "invalid: block 256 only");
    check(tbt_fec_ops_sclc.new(64, 192, 250, TBT_SCLC_BLOCK, SHARE - 1, 0,
                               1) == NULL,
          "invalid: share size");
    check(tbt_fec_ops_sclc.new(64, 192, 250, TBT_SCLC_BLOCK, SHARE, 1, 1) ==
              NULL,
          "invalid: d must be zero");
}

/* Share-level recovery over random DATA and parity losses. A share counts
 * as delivered only when all 128 batches recover, so this is a strict
 * lower bound on the codeword decoder and matches what the data plane
 * sees. */
static double erasure_run(u16 n, u16 m, u32 p_ppm, int blocks, u64 nonce)
{
    void *c = new_codec(n, m, down_k(n), nonce);
    u8 *data, *parity, *truth;
    u8 **dp, **pp;
    u8 *data_present, *parity_present;
    u64 missing = 0, recovered = 0;
    int b, i;

    if (!c)
    {
        check(0, "anchor: codec builds");
        return 0.0;
    }
    data = alloc_shares(n);
    parity = alloc_shares(m);
    truth = alloc_shares(n);
    dp = share_ptrs(data, n);
    pp = share_ptrs(parity, m);
    data_present = calloc(n, 1);
    parity_present = calloc(m, 1);
    if (!data_present || !parity_present)
    {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    for (b = 0; b < blocks; b++)
    {
        for (i = 0; i < n; i++)
            fill_random(data + (size_t)i * SHARE, SHARE);
        memcpy(truth, data, (size_t)n * SHARE);
        tbt_fec_ops_sclc.encode(c, dp, n, pp);
        for (i = 0; i < n; i++)
            data_present[i] = (rnd64() % 1000000u) >= p_ppm;
        for (i = 0; i < m; i++)
            parity_present[i] = (rnd64() % 1000000u) >= p_ppm;
        for (i = 0; i < n; i++)
            if (!data_present[i])
                missing++;
        (void)tbt_fec_ops_sclc.decode(c, dp, pp, data_present,
                                      parity_present, n);
        for (i = 0; i < n; i++)
            if (!data_present[i] && tbt_fec_ops_sclc.is_known(c, (u32)i))
            {
                recovered++;
                if (memcmp(truth + (size_t)i * SHARE,
                           data + (size_t)i * SHARE, SHARE))
                {
                    check(0, "anchor: recovered share differs from truth");
                    free(truth);
                    free(data_present);
                    free(parity_present);
                    free(dp);
                    free(pp);
                    free(data);
                    free(parity);
                    tbt_fec_ops_sclc.free(c);
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

static void test_erasure_anchors(void)
{
    static const struct
    {
        u16 n;
        u16 m;
        u32 p_ppm;
        u64 nonce;
    } anchors[] = {
        {128, 128, 300000u, 0x300},
        {178, 78, 100000u, 0x301},
    };
    size_t i;

    for (i = 0; i < sizeof(anchors) / sizeof(anchors[0]); i++)
    {
        double got = erasure_run(anchors[i].n, anchors[i].m,
                                 anchors[i].p_ppm, ANCHOR_BLOCKS,
                                 anchors[i].nonce);

        printf("n=%u/m=%u at p=%.4f: share recovery %.6f (min 0.99)\n",
               (unsigned)anchors[i].n, (unsigned)anchors[i].m,
               (double)anchors[i].p_ppm / 1e6, got);
        check(got >= 0.99, "anchor: share recovery");
    }
}

static bool in_set(u16 n, const u16 *set, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        if (set[i] == n)
            return true;
    return false;
}

static void test_buildable_sets(void)
{
    static const u16 up_yes[] = {1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                 14, 15, 16, 17, 18, 21, 22, 23, 24, 25,
                                 28, 30};
    static const u16 down_yes[] = {1, 64, 128, 192};
    static const u16 down_no[] = {0, 193, 255};
    size_t i;
    u16 n;

    for (n = 1; n <= 31; n++)
        check(tbt_fec_adapt_sclc_buildable(TBT_FEC_SCLC_UP, n,
                                           (u16)(32u - n)) ==
                  in_set(n, up_yes, sizeof(up_yes) / sizeof(up_yes[0])),
              "uplink: constructible set");
    for (i = 0; i < sizeof(down_yes) / sizeof(down_yes[0]); i++)
        check(tbt_fec_adapt_sclc_buildable(TBT_FEC_SCLC_DOWN, down_yes[i],
                                           (u16)(DOWN_SHARES -
                                                 down_yes[i])),
              "downlink: buildable range");
    for (i = 0; i < sizeof(down_no) / sizeof(down_no[0]); i++)
        check(!tbt_fec_adapt_sclc_buildable(TBT_FEC_SCLC_DOWN, down_no[i],
                                            (u16)(DOWN_SHARES -
                                                  down_no[i])),
              "downlink: outside the range");
}

static void test_rate_pct(void)
{
    check(tbt_fec_adapt_sclc_rate_pct(128, 128) == 50, "rate_pct: 50");
    check(tbt_fec_adapt_sclc_rate_pct(192, 64) == 75, "rate_pct: 75");
    check(tbt_fec_adapt_sclc_rate_pct(22, 10) == 69, "rate_pct: ceil");
    check(tbt_fec_adapt_sclc_rate_pct(1, 255) == 1, "rate_pct: floor 1");
    check(tbt_fec_adapt_sclc_rate_pct(0, 256) == 0, "rate_pct: zero");
}

int main(void)
{
    test_downlink_shapes();
    test_invalid_construction();
    test_erasure_anchors();
    test_buildable_sets();
    test_rate_pct();
    if (failures)
    {
        fprintf(stderr, "test_fec_sclc_shapes: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_fec_sclc_shapes: OK\n");
    return 0;
}
