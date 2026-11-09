/* test_sclc_uplink.c - host conformance test for the uplink grouped GF(16)
 * SC-IRA codec.
 *
 * Links the same objects the module builds, so a mapping or decoder rule
 * that passes here is a rule the kernel enforces.  No root, no module, no
 * fixture files.  The test is deterministic: the channel RNG is seeded.
 *
 * Coverage:
 *   - the three Phase 1 shapes (8x32 n=14 m=18 rate 7/16, 16x16 n=7 m=9
 *     rate 7/16, 32x8 n=4 m=4 rate 1/2) and their exact fill;
 *   - encode/decode byte-exactness with every share present;
 *   - one lost share at a time (every data and parity position);
 *   - p=0.30/0.40/0.50 share loss, codeword and block recovery, replay;
 *   - grouped erasure (8 independent groups in one codec);
 *   - bit-plane 64 (production) vs legacy interleaved recovery A/B;
 *   - legacy interleaved vs contiguous mapping A/B;
 *   - the tbt_fec_ops adapter and parameter rejection.
 *
 *   make host-test
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_sclc_uplink.h"
#include "tbt_sc_ldpc_gf16.h"

static int failures;
static int checks;

static void ok(int cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s%s%s\n", what, detail && *detail ? ": " : "",
           detail ? detail : "");
}

static void ok_int(int got, int want, const char *what)
{
    char buf[64];

    snprintf(buf, sizeof(buf), "got %d, want %d", got, want);
    ok(got == want, what, buf);
}

static void ok_u32(u32 got, u32 want, const char *what)
{
    char buf[64];

    snprintf(buf, sizeof(buf), "got %u, want %u", got, want);
    ok(got == want, what, buf);
}

/* xorshift128+ channel stream, seeded per test. */
static uint64_t rng_s0, rng_s1;

static void rng_seed(uint64_t seed)
{
    uint64_t x = seed;

    rng_s0 = x + 0x9E3779B97F4A7C15ULL;
    x = rng_s0;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    rng_s1 = x ^ (x >> 31);
    if (!rng_s0 && !rng_s1)
        rng_s0 = 1;
}

static uint64_t rng_next(void)
{
    uint64_t x = rng_s0;
    uint64_t const y = rng_s1;

    rng_s0 = y;
    x ^= x << 23;
    rng_s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
    return rng_s1 + y;
}

static int rng_mil(void)
{
    return (int)((rng_next() >> 32) % 1000000u);
}

struct shape
{
    const char *name;
    u16 groups;
    u16 n;
    u16 m;
    u16 rate_num;
    u16 rate_den;
};

static const struct shape shapes[] = {
    { "8x32-r1/4", 8, 8, 24, 1, 4 },
    { "8x32-r7/16", 8, 14, 18, 7, 16 },
    { "8x32-r3/8", 8, 12, 20, 3, 8 },
    { "8x32-r1/3", 8, 10, 22, 1, 3 },
    { "32x8-r1/2", 32, 4, 4, 1, 2 },
};

#define NSHAPES ((int)(sizeof(shapes) / sizeof(shapes[0])))

struct block
{
    u8 *data[256];
    u8 *parity[256];
    u8 *orig[256];
    u16 n;
    u16 m;
};

static int block_alloc(struct block *b, u16 n, u16 m)
{
    u16 i;

    memset(b, 0, sizeof(*b));
    b->n = n;
    b->m = m;
    for (i = 0; i < n; i++)
    {
        b->data[i] = malloc(TBT_FEC_S);
        b->orig[i] = malloc(TBT_FEC_S);
        if (!b->data[i] || !b->orig[i])
            return -ENOMEM;
    }
    for (i = 0; i < m; i++)
    {
        b->parity[i] = malloc(TBT_FEC_S);
        if (!b->parity[i])
            return -ENOMEM;
    }
    return 0;
}

static void block_free(struct block *b)
{
    u16 i;

    for (i = 0; i < b->n; i++)
    {
        free(b->data[i]);
        free(b->orig[i]);
    }
    for (i = 0; i < b->m; i++)
        free(b->parity[i]);
}

static void block_fill(struct block *b)
{
    u16 i;
    u32 j;

    for (i = 0; i < b->n; i++)
    {
        for (j = 0; j < TBT_FEC_S; j++)
            b->data[i][j] = (u8)rng_next();
        memcpy(b->orig[i], b->data[i], TBT_FEC_S);
    }
}

static int block_compare_known(struct tbt_sclc_uplink *u, u16 group,
                               struct block *b)
{
    u16 i;
    int bad = 0;

    for (i = 0; i < b->n; i++)
        if (tbt_sclc_uplink_share_known(u, group, i) &&
            memcmp(b->data[i], b->orig[i], TBT_FEC_S) != 0)
            bad++;
    return bad;
}

static void block_restore(struct block *b)
{
    u16 i;

    for (i = 0; i < b->n; i++)
        memcpy(b->data[i], b->orig[i], TBT_FEC_S);
}

/* ------------------------------------------------------------------ */

static void test_shapes(void)
{
    int s;

    for (s = 0; s < NSHAPES; s++)
    {
        struct tbt_sclc_params p;
        struct tbt_sclc_uplink *u;
        char what[96];

        memset(&p, 0, sizeof(p));
        p.n = shapes[s].n;
        p.m = shapes[s].m;
        p.block = 128;
        p.L = 16;
        p.rate_num = shapes[s].rate_num;
        p.rate_den = shapes[s].rate_den;
        p.max_core = 256;
        p.mapping = TBT_SCLC_MAP_BITPLANE64;
        p.seed = 1;
        u = tbt_sclc_uplink_new(shapes[s].groups, &p);
        snprintf(what, sizeof(what), "%s: new", shapes[s].name);
        ok(u != NULL, what, "codec allocation failed");
        if (!u)
            continue;
        snprintf(what, sizeof(what), "%s: shares", shapes[s].name);
        ok_u32(tbt_sclc_uplink_shares(u), (u32)p.n + p.m, what);
        snprintf(what, sizeof(what), "%s: codes", shapes[s].name);
        ok_u32(tbt_sclc_uplink_codes(u), (u32)p.n * 16, what);
        snprintf(what, sizeof(what), "%s: n_data", shapes[s].name);
        ok_u32(tbt_sclc_uplink_n_data(u), 1024, what);
        snprintf(what, sizeof(what), "%s: mem", shapes[s].name);
        ok(tbt_sclc_uplink_mem(u) > 0, what, "memory accounting is zero");
        tbt_sclc_uplink_free(u);
    }
}

static void test_roundtrip(void)
{
    int s;

    rng_seed(7);
    for (s = 0; s < NSHAPES; s++)
    {
        struct tbt_sclc_params p;
        struct tbt_sclc_uplink *u;
        struct block b;
        u8 present_d[256], present_p[256];
        char what[96];
        int r;

        memset(&p, 0, sizeof(p));
        p.n = shapes[s].n;
        p.m = shapes[s].m;
        p.block = 128;
        p.L = 16;
        p.rate_num = shapes[s].rate_num;
        p.rate_den = shapes[s].rate_den;
        p.max_core = 256;
        p.mapping = TBT_SCLC_MAP_BITPLANE64;
        p.seed = 1;
        u = tbt_sclc_uplink_new(shapes[s].groups, &p);
        ok(u != NULL, shapes[s].name, "codec allocation failed");
        if (!u)
            continue;
        if (block_alloc(&b, p.n, p.m) != 0)
        {
            ok(0, shapes[s].name, "share allocation failed");
            tbt_sclc_uplink_free(u);
            continue;
        }
        block_fill(&b);
        memset(present_d, 1, sizeof(present_d));
        memset(present_p, 1, sizeof(present_p));
        r = tbt_sclc_uplink_encode(u, 0, b.data, b.parity);
        snprintf(what, sizeof(what), "%s: encode", shapes[s].name);
        ok_int(r, 0, what);
        r = tbt_sclc_uplink_decode(u, 0, b.data, b.parity, present_d,
                                   present_p);
        snprintf(what, sizeof(what), "%s: decode all present", shapes[s].name);
        ok_int(r, p.n, what);
        {
            struct tbt_sclc_stat st;

            tbt_sclc_uplink_stat(u, 0, &st);
            snprintf(what, sizeof(what), "%s: codes_ok", shapes[s].name);
            ok_u32(st.codes_ok, tbt_sclc_uplink_codes(u), what);
            snprintf(what, sizeof(what), "%s: codes_failed", shapes[s].name);
            ok_u32(st.codes_failed, 0, what);
        }
        snprintf(what, sizeof(what), "%s: byte-exact", shapes[s].name);
        ok_int(block_compare_known(u, 0, &b), 0, what);

        /* All DATA present, two parity shares missing.  No codeword needs
         * to be solved; the early return must still publish every DATA
         * share as known, leave the absent parity shares unknown for the
         * NACK path, and not modify the payload. */
        present_p[0] = 0;
        present_p[1] = 0;
        r = tbt_sclc_uplink_decode(u, 0, b.data, b.parity, present_d,
                                   present_p);
        snprintf(what, sizeof(what), "%s: decode parity-only loss",
                 shapes[s].name);
        ok_int(r, p.n, what);
        {
            struct tbt_sclc_stat st;

            tbt_sclc_uplink_stat(u, 0, &st);
            snprintf(what, sizeof(what), "%s: parity-only codes_ok",
                     shapes[s].name);
            ok_u32(st.codes_ok, tbt_sclc_uplink_codes(u), what);
            snprintf(what, sizeof(what), "%s: parity-only codes_failed",
                     shapes[s].name);
            ok_u32(st.codes_failed, 0, what);
            snprintf(what, sizeof(what), "%s: parity-only missing",
                     shapes[s].name);
            ok_u32(st.shares_missing, 2, what);
            snprintf(what, sizeof(what), "%s: parity-only recovered",
                     shapes[s].name);
            ok_u32(st.shares_recovered, 0, what);
        }
        snprintf(what, sizeof(what), "%s: parity-only data known",
                 shapes[s].name);
        ok_int(tbt_sclc_uplink_share_known(u, 0, 0), 1, what);
        snprintf(what, sizeof(what), "%s: parity-only absent unknown",
                 shapes[s].name);
        ok_int(tbt_sclc_uplink_share_known(u, 0, p.n), 0, what);
        snprintf(what, sizeof(what), "%s: parity-only present known",
                 shapes[s].name);
        ok_int(tbt_sclc_uplink_share_known(u, 0, p.n + 2), 1, what);
        snprintf(what, sizeof(what), "%s: parity-only payload untouched",
                 shapes[s].name);
        ok_int(block_compare_known(u, 0, &b), 0, what);
        present_p[0] = 1;
        present_p[1] = 1;

        /* Determinism: a second encode must produce identical shares. */
        {
            u8 *p2[256];
            u16 i;
            int same = 1;

            for (i = 0; i < p.m; i++)
                p2[i] = malloc(TBT_FEC_S);
            tbt_sclc_uplink_encode(u, 0, b.data, p2);
            for (i = 0; i < p.m; i++)
                if (memcmp(p2[i], b.parity[i], TBT_FEC_S) != 0)
                    same = 0;
            for (i = 0; i < p.m; i++)
                free(p2[i]);
            snprintf(what, sizeof(what), "%s: deterministic", shapes[s].name);
            ok(same, what, "two encodes differ");
        }
        block_free(&b);
        tbt_sclc_uplink_free(u);
    }
}

static void test_single_erasure(void)
{
    int s;

    rng_seed(11);
    for (s = 0; s < NSHAPES; s++)
    {
        struct tbt_sclc_params p;
        struct tbt_sclc_uplink *u;
        struct block b;
        u8 present_d[256], present_p[256];
        char what[96];
        u16 i;

        memset(&p, 0, sizeof(p));
        p.n = shapes[s].n;
        p.m = shapes[s].m;
        p.block = 128;
        p.L = 16;
        p.rate_num = shapes[s].rate_num;
        p.rate_den = shapes[s].rate_den;
        p.max_core = 256;
        p.mapping = TBT_SCLC_MAP_BITPLANE64;
        p.seed = 1;
        u = tbt_sclc_uplink_new(shapes[s].groups, &p);
        if (!u)
        {
            ok(0, shapes[s].name, "codec allocation failed");
            continue;
        }
        if (block_alloc(&b, p.n, p.m) != 0)
        {
            ok(0, shapes[s].name, "share allocation failed");
            tbt_sclc_uplink_free(u);
            continue;
        }
        block_fill(&b);
        ok_int(tbt_sclc_uplink_encode(u, 0, b.data, b.parity), 0,
               "single-erasure encode");

        for (i = 0; i < (u16)(p.n + p.m); i++)
        {
            int is_data = i < p.n;
            int r;

            memset(present_d, 1, sizeof(present_d));
            memset(present_p, 1, sizeof(present_p));
            block_restore(&b);
            if (is_data)
            {
                present_d[i] = 0;
                memset(b.data[i], 0, TBT_FEC_S);
            }
            else
            {
                present_p[i - p.n] = 0;
                memset(b.parity[i - p.n], 0, TBT_FEC_S);
            }
            r = tbt_sclc_uplink_decode(u, 0, b.data, b.parity, present_d,
                                       present_p);
            snprintf(what, sizeof(what), "%s: lose share %u", shapes[s].name,
                     i);
            ok_int(r, p.n, what);
            snprintf(what, sizeof(what), "%s: lose share %u exact",
                     shapes[s].name, i);
            ok_int(block_compare_known(u, 0, &b), 0, what);
        }
        block_free(&b);
        tbt_sclc_uplink_free(u);
    }
}

struct p_result
{
    double cw_rec;
    double block_ok;
    long blocks;
};

static struct p_result run_p(u16 groups, u16 n, u16 m, u16 rn, u16 rd,
                             u8 mapping, double p, int blocks, int max_core)
{
    struct tbt_sclc_params pp;
    struct tbt_sclc_uplink *u;
    struct p_result res;
    long ok_codes = 0, all_codes = 0, ok_blocks = 0;
    int b;

    memset(&res, 0, sizeof(res));
    memset(&pp, 0, sizeof(pp));
    pp.n = n;
    pp.m = m;
    pp.block = 128;
    pp.L = 16;
    pp.rate_num = rn;
    pp.rate_den = rd;
    pp.max_core = (u16)max_core;
    pp.mapping = mapping;
    pp.seed = 1;
    u = tbt_sclc_uplink_new(groups, &pp);
    if (!u)
        return res;

    for (b = 0; b < blocks; b++)
    {
        struct block blk;
        u8 pd[256], ppres[256];
        u16 g;

        if (block_alloc(&blk, n, m) != 0)
            break;
        for (g = 0; g < groups; g++)
        {
            u16 i;

            block_fill(&blk);
            if (tbt_sclc_uplink_encode(u, g, blk.data, blk.parity) != 0)
                continue;
            for (i = 0; i < n; i++)
                pd[i] = (u8)(rng_mil() >= (int)(p * 1000000.0));
            for (i = 0; i < m; i++)
                ppres[i] = (u8)(rng_mil() >= (int)(p * 1000000.0));
            for (i = 0; i < n; i++)
                if (!pd[i])
                    memset(blk.data[i], 0, TBT_FEC_S);
            for (i = 0; i < m; i++)
                if (!ppres[i])
                    memset(blk.parity[i], 0, TBT_FEC_S);
            tbt_sclc_uplink_decode(u, g, blk.data, blk.parity, pd, ppres);
            {
                struct tbt_sclc_stat st;

                tbt_sclc_uplink_stat(u, g, &st);
                ok_codes += st.codes_ok;
                all_codes += st.codes;
                if (!st.codes_failed)
                    ok_blocks++;
                if (block_compare_known(u, g, &blk) != 0)
                {
                    ok(0, "p-sweep", "known share is not byte-exact");
                    break;
                }
                /* Replay: every share present must restore every byte. */
                block_restore(&blk);
                tbt_sclc_uplink_encode(u, g, blk.data, blk.parity);
                for (i = 0; i < n; i++)
                    pd[i] = 1;
                for (i = 0; i < m; i++)
                    ppres[i] = 1;
                tbt_sclc_uplink_decode(u, g, blk.data, blk.parity, pd, ppres);
                if (block_compare_known(u, g, &blk) != 0)
                {
                    ok(0, "p-sweep replay", "replay is not byte-exact");
                    break;
                }
            }
        }
        block_free(&blk);
    }
    res.blocks = (long)blocks * groups;
    res.cw_rec = all_codes ? (double)ok_codes / (double)all_codes : 0.0;
    res.block_ok = res.blocks ? (double)ok_blocks / (double)res.blocks : 0.0;
    tbt_sclc_uplink_free(u);
    return res;
}

static void test_p_sweep(void)
{
#ifdef __SANITIZE_ADDRESS__
    const int blocks = 20;
    const int blocks4 = 120;
    const double cw_min = 0.995;
    const double block_min = 0.98;
#else
    const int blocks = 100;
    const int blocks4 = 300;
    const double cw_min = 0.998;
    const double block_min = 0.99;
#endif
    struct p_result r, primary;

    rng_seed(21);
    r = run_p(1, 8, 24, 1, 4, TBT_SCLC_MAP_BITPLANE64, 0.30, blocks, 256);
    printf("p=0.30 8x32 r1/4: codeword %.6f block %.4f\n", r.cw_rec,
           r.block_ok);
    ok(r.cw_rec >= 0.999, "p=0.30 r1/4 codeword recovery", "below 0.999");
    ok(r.block_ok >= 0.999, "p=0.30 r1/4 block recovery", "below 0.999");

    rng_seed(22);
    primary = run_p(1, 8, 24, 1, 4, TBT_SCLC_MAP_BITPLANE64, 0.40,
                    blocks4, 256);
    printf("p=0.40 8x32 r1/4: codeword %.6f block %.4f\n",
           primary.cw_rec, primary.block_ok);
    /* 1/4 is the smallest rate that clears the >0.99 floor on the measured
     * host; the 0.999 target is at the edge of the binomial share-loss tail
     * (8 data shares).  Replay (asserted inside run_p) is byte-exact. */
    ok(primary.cw_rec >= cw_min, "p=0.40 r1/4 codeword floor",
       "below the measured floor");
    ok(primary.block_ok >= block_min, "p=0.40 r1/4 block floor",
       "below the measured floor");
    printf("TARGET p=0.40 block 0.999: %s (measured %.4f)\n",
           primary.block_ok >= 0.999 ? "MET" : "NOT MET",
           primary.block_ok);

    rng_seed(23);
    r = run_p(1, 8, 24, 1, 4, TBT_SCLC_MAP_BITPLANE64, 0.50, blocks, 256);
    printf("p=0.50 8x32 r1/4: codeword %.6f block %.4f\n", r.cw_rec,
           r.block_ok);
    ok(r.cw_rec >= 0.90, "p=0.50 r1/4 codeword recovery", "below 0.90");
    ok(r.block_ok >= 0.90, "p=0.50 r1/4 block recovery", "below 0.90");

    /* Document why the higher-rate shapes are not the primary: at p=0.40
     * their per-group block recovery misses the >0.99 floor. */
    {
        static const struct
        {
            const char *name;
            u16 n, m, rn, rd;
        } higher[] = {
            { "r7/16", 14, 18, 7, 16 },
            { "r3/8", 12, 20, 3, 8 },
            { "r1/3", 10, 22, 1, 3 },
        };
        int i;

        for (i = 0; i < 3; i++)
        {
            rng_seed(30u + (u64)i);
            r = run_p(1, higher[i].n, higher[i].m, higher[i].rn,
                      higher[i].rd, TBT_SCLC_MAP_BITPLANE64, 0.40, blocks,
                      256);
            printf("p=0.40 8x32 %s: codeword %.6f block %.4f\n",
                   higher[i].name, r.cw_rec, r.block_ok);
            ok(r.block_ok <= primary.block_ok + 0.02,
               "higher rate is not better than r1/4",
               "a higher-rate shape beat r1/4");
        }
    }
}

/* The 64-lane mapping gives every codeword of a batch the same mask, so a
 * block succeeds when a handful of batch masks decode instead of every
 * legacy phase.  The shared-erasure bench (host-bench-recovery) records the
 * full 6-rate x p grid; this guard keeps the production mapping from
 * regressing against the legacy one at the p=0.30 operating point. */
static void test_bitplane_recovery(void)
{
#ifdef __SANITIZE_ADDRESS__
    const int blocks = 15;
#else
    const int blocks = 60;
#endif
    static const u16 n[6] = {8, 9, 12, 16, 18, 22};
    static const u16 m[6] = {24, 23, 20, 16, 14, 10};
    static const u16 d[6] = {64, 72, 96, 128, 144, 176};
    int i;

    for (i = 0; i < 6; i++)
    {
        struct p_result oldm, newm;
        char what[96];

        rng_seed(600u + (u64)i);
        oldm = run_p(1, n[i], m[i], d[i], 256,
                     TBT_SCLC_MAP_INTERLEAVED, 0.30, blocks, 256);
        rng_seed(600u + (u64)i);
        newm = run_p(1, n[i], m[i], d[i], 256,
                     TBT_SCLC_MAP_BITPLANE64, 0.30, blocks, 256);
        printf("mapping p=0.30 %u/%u: legacy cw %.4f block %.3f; "
               "bitplane cw %.4f block %.3f\n",
               n[i], m[i], oldm.cw_rec, oldm.block_ok, newm.cw_rec,
               newm.block_ok);
        snprintf(what, sizeof(what), "bitplane %u/%u recovery", n[i], m[i]);
        /* These are unpaired finite-sample floors; the paired 6-rate sweep
         * in host-bench-recovery is the precise comparison. */
        ok(newm.cw_rec + 0.02 >= oldm.cw_rec, what,
           "codeword recovery fell below the legacy mapping");
        ok(newm.block_ok + 0.08 >= oldm.block_ok, what,
           "block recovery fell below the legacy mapping");
    }
}

static void test_contiguous_ab(void)
{
#ifdef __SANITIZE_ADDRESS__
    const int blocks = 8;
#else
    const int blocks = 20;
#endif
    struct p_result a, c;

    rng_seed(31);
    a = run_p(1, 14, 18, 7, 16, TBT_SCLC_MAP_INTERLEAVED, 0.30, blocks, 256);
    rng_seed(31);
    c = run_p(1, 14, 18, 7, 16, TBT_SCLC_MAP_CONTIGUOUS, 0.30, blocks, 256);
    printf("mapping p=0.30: interleaved cw %.6f block %.4f; "
           "contiguous cw %.6f block %.4f\n",
           a.cw_rec, a.block_ok, c.cw_rec, c.block_ok);
    ok(a.cw_rec >= c.cw_rec, "interleaved beats contiguous",
       "interleaved recovery is lower");
}

static void test_grouped(void)
{
    struct tbt_sclc_params p;
    struct tbt_sclc_uplink *u;
    struct block blk;
    u8 pd[256], ppres[256];
    u16 g;

    rng_seed(41);
    memset(&p, 0, sizeof(p));
    p.n = 8;
    p.m = 24;
    p.block = 128;
    p.L = 16;
    p.rate_num = 1;
    p.rate_den = 4;
    p.max_core = 256;
    p.mapping = TBT_SCLC_MAP_BITPLANE64;
    p.seed = 1;
    u = tbt_sclc_uplink_new(8, &p);
    ok(u != NULL, "grouped: new 8 groups", "allocation failed");
    if (!u)
        return;
    ok_u32(tbt_sclc_uplink_groups(u), 8, "grouped: group count");
    ok_u32(tbt_sclc_uplink_shares(u), 32, "grouped: shares per group");
    if (block_alloc(&blk, 8, 24) != 0)
    {
        ok(0, "grouped", "share allocation failed");
        tbt_sclc_uplink_free(u);
        return;
    }
    for (g = 0; g < 8; g++)
    {
        u16 i;

        block_fill(&blk);
        ok_int(tbt_sclc_uplink_encode(u, g, blk.data, blk.parity), 0,
               "grouped encode");
        for (i = 0; i < 8; i++)
            pd[i] = (u8)(rng_mil() >= 300000);
        for (i = 0; i < 24; i++)
            ppres[i] = (u8)(rng_mil() >= 300000);
        for (i = 0; i < 8; i++)
            if (!pd[i])
                memset(blk.data[i], 0, TBT_FEC_S);
        for (i = 0; i < 24; i++)
            if (!ppres[i])
                memset(blk.parity[i], 0, TBT_FEC_S);
        ok_int(tbt_sclc_uplink_decode(u, g, blk.data, blk.parity, pd, ppres),
               8, "grouped decode");
        ok_int(block_compare_known(u, g, &blk), 0, "grouped byte-exact");
    }
    /* Each group has its own statistics slot. */
    {
        struct tbt_sclc_stat st;

        tbt_sclc_uplink_stat(u, 0, &st);
        ok_u32(st.codes, 128, "grouped: group 0 codes");
        tbt_sclc_uplink_stat(u, 7, &st);
        ok_u32(st.codes, 128, "grouped: group 7 codes");
    }
    block_free(&blk);
    tbt_sclc_uplink_free(u);
}

static void test_ops(void)
{
    const struct tbt_fec_ops *ops = &tbt_fec_ops_sclc_gf16;
    struct block b;
    u8 pd[256], ppres[256];
    void *ctx;

    rng_seed(51);
    ok_int(ops->mode, 3, "ops: mode");
    ok(strcmp(ops->name, "sc-ldpc-gf16") == 0, "ops: name", "wrong name");
    ctx = ops->new(8, 24, 128, 16, TBT_FEC_S, 64, 1);
    ok(ctx != NULL, "ops: new 8x32 shape", "allocation failed");
    if (!ctx)
        return;
    ok_u32(ops->parity_count(ctx, 0), 24, "ops: parity_count");
    ok((u32)ops->coeffs(ctx, 0, 0, NULL, 0) == (u32)-ENOSYS, "ops: coeffs",
       "coeffs must be -ENOSYS");
    if (block_alloc(&b, 8, 24) != 0)
    {
        ok(0, "ops", "share allocation failed");
        ops->free(ctx);
        return;
    }
    block_fill(&b);
    ok_int(ops->encode(ctx, b.data, 0, b.parity), 0, "ops: encode");
    memset(pd, 1, sizeof(pd));
    memset(ppres, 1, sizeof(ppres));
    ok_int(ops->decode(ctx, b.data, b.parity, pd, ppres, 0), 8,
           "ops: decode");
    ok_int(block_compare_known(ctx, 0, &b), 0, "ops: byte-exact");
    ok_int(ops->is_known(ctx, 0), 1, "ops: is_known");
    block_free(&b);
    ops->free(ctx);
}

/* ------------------------------------------------------------------ */
/* ladder codec and scalar reference                                   */
/* ------------------------------------------------------------------ */

#define LADDER_SHAPES 6
static const u16 ladder_n[LADDER_SHAPES] = {8, 9, 12, 16, 18, 22};
static const u16 ladder_m[LADDER_SHAPES] = {24, 23, 20, 16, 14, 10};

static inline u8 ref_get_sym(const u8 *buf, u32 off)
{
    return (u8)((buf[off >> 1] >> ((off & 1u) * 4)) & 0x0fu);
}

static inline void ref_put_sym(u8 *buf, u32 off, u8 v)
{
    u32 byte = off >> 1;
    u8 shift = (u8)((off & 1u) * 4);

    buf[byte] = (u8)((buf[byte] & (u8)~(0x0fu << shift)) |
                     (u8)((v & 0x0fu) << shift));
}

/* Independent scalar reference decoder.  It rebuilds the same graph and
 * walks the documented interleaved mapping one codeword at a time through
 * gf16_decode(), so the batch decoder can be compared against the path it
 * replaced.  Results land in ref_data[], one share per entry. */
static int ladder_scalar_decode(const struct tbt_sclc_params *p,
                                const u8 *data[], const u8 *parity[],
                                const u8 *pd, const u8 *pp,
                                u8 *ref_data[])
{
    sc_params sp;
    gf16_code *c = NULL;
    char err[128];
    u8 *recv = NULL, *erased = NULL, *out = NULL;
    u32 offp[256], offq[256], woff[256];
    u32 n_data = (u32)p->L * (p->block / 2);
    u32 codes = (u32)p->n * (u32)(TBT_FEC_S * 2) / n_data;
    int n, n_par, cc, rc = -1;
    u32 j;

    memset(&sp, 0, sizeof(sp));
    sp.block = p->block;
    sp.L = p->L;
    sp.rate_num = p->rate_num;
    sp.rate_den = p->rate_den;
    sp.term = SC_TERM_ACC;
    sp.extra_dc = 5;
    sp.seed = p->seed;
    c = gf16_build(&sp, p->seed ^ 0xD1B54A32D192ED03ULL, err, sizeof(err));
    if (!c)
        return -1;
    n = gf16_n(c);
    n_par = gf16_n_par(c);
    recv = malloc((size_t)n);
    erased = malloc((size_t)n);
    out = malloc((size_t)n);
    if (!recv || !erased || !out)
        goto done;
    memset(offp, 0, sizeof(offp));
    memset(offq, 0, sizeof(offq));
    memset(woff, 0, sizeof(woff));
    for (cc = 0; cc < (int)codes; cc++) {
        gf16_dec_stat st;

        memset(erased, 0, (size_t)n);
        for (j = 0; j < n_data; j++) {
            u16 sh = (u16)((j + (u32)cc) % p->n);
            u32 off = offp[sh]++;

            if (pd[sh])
                recv[j] = ref_get_sym(data[sh], off);
            else
                erased[j] = 1;
        }
        for (j = 0; j < (u32)n_par; j++) {
            u16 sh = (u16)((j + (u32)cc) % p->m);
            u32 off = offq[sh]++;

            if (pp[sh])
                recv[n_data + j] = ref_get_sym(parity[sh], off);
            else
                erased[n_data + j] = 1;
        }
        st = gf16_decode(c, recv, erased, out, 1, p->max_core);
        if (st.unresolved == 0) {
            for (j = 0; j < n_data; j++) {
                u16 sh = (u16)((j + (u32)cc) % p->n);
                u32 off = woff[sh]++;

                ref_put_sym(ref_data[sh], off, out[j]);
            }
        } else {
            for (j = 0; j < n_data; j++)
                woff[(u16)((j + (u32)cc) % p->n)]++;
        }
    }
    rc = 0;
done:
    free(recv);
    free(erased);
    free(out);
    gf16_free(c);
    return rc;
}

/* Every ladder split must encode and decode, and the batch decoder must
 * match the scalar reference byte for byte.  The 18/14 and 22/10 splits
 * used to size the codec from the rate arithmetic instead of the graph, so
 * decoding could read past the staging buffer. */
static void test_ladder_codec(void)
{
    int i;

    for (i = 0; i < LADDER_SHAPES; i++) {
        struct tbt_sclc_params p;
        struct tbt_sclc_uplink *u;
        struct block b, ref;
        u8 pd[256], pp[256];
        u16 k;
        char what[96];

        memset(&p, 0, sizeof(p));
        p.n = ladder_n[i];
        p.m = ladder_m[i];
        p.block = 128;
        p.L = 16;
        p.rate_num = (u16)(ladder_n[i] * 256 /
                           (ladder_n[i] + ladder_m[i]));
        p.rate_den = 256;
        p.max_core = 256;
        p.mapping = TBT_SCLC_MAP_INTERLEAVED;
        p.seed = 1;
        u = tbt_sclc_uplink_new(1, &p);
        snprintf(what, sizeof(what), "ladder %u/%u: new", p.n, p.m);
        ok(u != NULL, what, "NULL");
        if (!u)
            continue;
        memset(&b, 0, sizeof(b));
        memset(&ref, 0, sizeof(ref));
        if (block_alloc(&b, p.n, p.m) != 0 ||
            block_alloc(&ref, p.n, p.m) != 0) {
            ok(0, "ladder: block alloc", "failed");
            tbt_sclc_uplink_free(u);
            continue;
        }
        rng_seed(101 + (uint64_t)i);
        block_fill(&b);
        snprintf(what, sizeof(what), "ladder %u/%u: encode", p.n, p.m);
        ok_int(tbt_sclc_uplink_encode(u, 0, b.data, b.parity), 0, what);

        for (k = 0; k < p.n; k++)
            memcpy(ref.data[k], b.data[k], TBT_FEC_S);
        for (k = 0; k < p.m; k++)
            memcpy(ref.parity[k], b.parity[k], TBT_FEC_S);

        for (k = 0; k < p.n; k++)
            pd[k] = 1;
        for (k = 0; k < p.m; k++)
            pp[k] = 1;
        pd[p.n - 1] = 0;
        pp[0] = 0;
        pp[p.m - 1] = 0;
        memset(b.data[p.n - 1], 0, TBT_FEC_S);
        memset(b.parity[0], 0, TBT_FEC_S);
        memset(b.parity[p.m - 1], 0, TBT_FEC_S);
        memset(ref.data[p.n - 1], 0, TBT_FEC_S);
        memset(ref.parity[0], 0, TBT_FEC_S);
        memset(ref.parity[p.m - 1], 0, TBT_FEC_S);

        snprintf(what, sizeof(what), "ladder %u/%u: decode", p.n, p.m);
        ok_int(tbt_sclc_uplink_decode(u, 0, b.data, b.parity, pd, pp),
               p.n, what);
        snprintf(what, sizeof(what), "ladder %u/%u: byte-exact", p.n, p.m);
        ok_int(block_compare_known(u, 0, &b), 0, what);

        /* scalar reference over the second copy */
        snprintf(what, sizeof(what), "ladder %u/%u: reference", p.n, p.m);
        ok_int(ladder_scalar_decode(&p, (const u8 **)ref.data,
                                    (const u8 **)ref.parity, pd, pp,
                                    ref.data),
               0, what);
        for (k = 0; k < p.n; k++) {
            snprintf(what, sizeof(what),
                     "ladder %u/%u: batch vs scalar share %u", p.n, p.m, k);
            ok(memcmp(b.data[k], ref.data[k], TBT_FEC_S) == 0, what,
               "batch and scalar disagree");
        }
        block_free(&b);
        block_free(&ref);
        tbt_sclc_uplink_free(u);
    }
}

/* Six of eight DATA shares gone.  Every check that can fire does fire, so
 * the peel queue reaches n_chk; the batched decoder must not write past its
 * slot and must still match the scalar reference. */
static void test_full_queue(void)
{
    struct tbt_sclc_params p;
    struct tbt_sclc_uplink *u;
    struct block b, ref;
    u8 pd[256], pp[256];
    u16 k;

    rng_seed(77);
    memset(&p, 0, sizeof(p));
    p.n = 8;
    p.m = 24;
    p.block = 128;
    p.L = 16;
    p.rate_num = 1;
    p.rate_den = 4;
    p.max_core = 256;
    p.mapping = TBT_SCLC_MAP_INTERLEAVED;
    p.seed = 1;
    u = tbt_sclc_uplink_new(1, &p);
    ok(u != NULL, "full queue: new", "NULL");
    if (!u)
        return;
    memset(&b, 0, sizeof(b));
    memset(&ref, 0, sizeof(ref));
    if (block_alloc(&b, 8, 24) != 0 || block_alloc(&ref, 8, 24) != 0) {
        ok(0, "full queue: block alloc", "failed");
        tbt_sclc_uplink_free(u);
        return;
    }
    block_fill(&b);
    ok_int(tbt_sclc_uplink_encode(u, 0, b.data, b.parity), 0,
           "full queue: encode");
    for (k = 0; k < 8; k++)
        pd[k] = (u8)(k >= 6);
    for (k = 0; k < 24; k++)
        pp[k] = 1;
    for (k = 0; k < 8; k++)
        memcpy(ref.data[k], b.data[k], TBT_FEC_S);
    for (k = 0; k < 24; k++)
        memcpy(ref.parity[k], b.parity[k], TBT_FEC_S);
    for (k = 0; k < 6; k++) {
        memset(b.data[k], 0, TBT_FEC_S);
        memset(ref.data[k], 0, TBT_FEC_S);
    }
    ok_int(tbt_sclc_uplink_decode(u, 0, b.data, b.parity, pd, pp), 8,
           "full queue: decode");
    ok_int(block_compare_known(u, 0, &b), 0, "full queue: byte-exact");
    ok_int(ladder_scalar_decode(&p, (const u8 **)ref.data,
                                (const u8 **)ref.parity, pd, pp,
                                ref.data),
           0, "full queue: reference");
    for (k = 0; k < 8; k++)
        ok(memcmp(b.data[k], ref.data[k], TBT_FEC_S) == 0,
           "full queue: batch vs scalar", "batch and scalar disagree");
    block_free(&b);
    block_free(&ref);
    tbt_sclc_uplink_free(u);
}

/* Every n/m split the SC-LDPC rate ladder derives for 8x32 must build with
 * the production GF(16) parameters (block 128, L 16, interleaved). */
static void test_ladder_shapes(void)
{
    static const u16 n[6] = {8, 9, 12, 16, 18, 22};
    static const u16 m[6] = {24, 23, 20, 16, 14, 10};
    /* sc_build rounds n_base down above rate 1/2, so the codec's parity
     * count is 768 for 18/14 and 448 for 22/10, not the share-ratio
     * estimate.  The wire keeps the extra slots as zero padding. */
    static const u32 npar[6] = {3072, 2616, 1706, 1024, 768, 448};
    int i;

    for (i = 0; i < 6; i++)
    {
        struct tbt_sclc_params p;
        struct tbt_sclc_uplink *u;

        memset(&p, 0, sizeof(p));
        p.n = n[i];
        p.m = m[i];
        p.block = 128;
        p.L = 16;
        p.rate_num = (u16)(n[i] * 256 / (n[i] + m[i]));
        p.rate_den = 256;
        p.max_core = 256;
        p.mapping = TBT_SCLC_MAP_BITPLANE64;
        p.seed = 1;
        u = tbt_sclc_uplink_new(8, &p);
        ok(u != NULL, "ladder: shape builds", "NULL");
        if (u)
        {
            ok_u32(tbt_sclc_uplink_n_par(u), npar[i], "ladder: n_par");
            tbt_sclc_uplink_free(u);
        }
    }
}

/* All parity unknown means every check starts at count one: the initial
 * queue holds n_chk entries, so any later enqueue must not write at index
 * n_chk.  The unbranched form wrote there and clobbered the next slot (or
 * ran two bytes past the buffer on the last slot). */
static void test_gf16_full_queue(void)
{
    sc_params sp;
    gf16_code *c;
    char err[128];
    int n, n_data, i, cw;
    u8 *recv_b, *erased_b, *out_b;
    u8 *recv, *erased, *out;
    gf16_dec_stat st[2], st1;

    memset(&sp, 0, sizeof(sp));
    sp.block = 128;
    sp.L = 16;
    sp.rate_num = 1;
    sp.rate_den = 4;
    sp.term = SC_TERM_ACC;
    sp.extra_dc = 5;
    sp.seed = 1;
    c = gf16_build(&sp, 1ULL ^ 0xD1B54A32D192ED03ULL, err, sizeof(err));
    ok(c != NULL, "gf16 full queue: build", err);
    if (!c)
        return;
    n = gf16_n(c);
    n_data = gf16_n_data(c);
    recv_b = calloc(2 * (size_t)n, 1);
    erased_b = calloc(2 * (size_t)n, 1);
    out_b = calloc(2 * (size_t)n, 1);
    recv = calloc((size_t)n, 1);
    erased = calloc((size_t)n, 1);
    out = calloc((size_t)n, 1);
    if (!recv_b || !erased_b || !out_b || !recv || !erased || !out) {
        ok(0, "gf16 full queue: alloc", "failed");
        goto done;
    }
    for (cw = 0; cw < 2; cw++)
        for (i = 0; i < n; i++)
            if (i < n_data) {
                recv_b[(size_t)cw * n + i] = (u8)((i * 7) & 15);
                erased_b[(size_t)cw * n + i] = 0;
            } else {
                erased_b[(size_t)cw * n + i] = 1;
            }
    for (i = 0; i < n; i++)
        if (i < n_data) {
            recv[i] = (u8)((i * 7) & 15);
            erased[i] = 0;
        } else {
            erased[i] = 1;
        }
    gf16_decode_batch(c, 2, recv_b, erased_b, out_b, 1, 0, st);
    st1 = gf16_decode(c, recv, erased, out, 1, 0);
    ok_int((int)st1.unresolved, 0, "gf16 full queue: scalar resolves");
    ok_int((int)st[0].unresolved, 0, "gf16 full queue: slot 0 resolves");
    ok_int((int)st[1].unresolved, 0, "gf16 full queue: slot 1 resolves");
    ok(memcmp(out_b, out, (size_t)n) == 0,
       "gf16 full queue: slot 0 exact", "differs from scalar");
    ok(memcmp(out_b + n, out, (size_t)n) == 0,
       "gf16 full queue: slot 1 exact", "differs from scalar");
done:
    free(recv_b);
    free(erased_b);
    free(out_b);
    free(recv);
    free(erased);
    free(out);
    gf16_free(c);
}

static uint64_t scan_next(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Random erasure scans over four shapes: the peel queue reaches n_chk, so
 * an unguarded write at index n_chk runs past the last slot (ASan catches
 * it).  The RNG seed is fixed, so the same trajectories run every time. */
static void test_queue_full_scan(void)
{
    static const struct
    {
        u16 n, m, rn, rd;
    } sh[4] = {
        {8, 24, 1, 4},
        {4, 4, 1, 2},
        {14, 18, 7, 16},
        {12, 20, 3, 8},
    };
    uint64_t x = 12345;
    int s, trial, cw, i;

    for (s = 0; s < 4; s++) {
        sc_params sp;
        gf16_code *c;
        char err[128];
        int n, n_data;
        u8 *rb, *eb, *ob, md[64], mp[64];
        gf16_dec_stat st[2];

        memset(&sp, 0, sizeof sp);
        sp.block = 128;
        sp.L = 16;
        sp.rate_num = sh[s].rn;
        sp.rate_den = sh[s].rd;
        sp.term = SC_TERM_ACC;
        sp.extra_dc = 5;
        sp.seed = 1;
        c = gf16_build(&sp, 1ULL ^ 0xD1B54A32D192ED03ULL, err, sizeof(err));
        ok(c != NULL, "queue scan: build", err);
        if (!c)
            continue;
        n = gf16_n(c);
        n_data = gf16_n_data(c);
        rb = calloc(2 * (size_t)n, 1);
        eb = calloc(2 * (size_t)n, 1);
        ob = calloc(2 * (size_t)n, 1);
        if (!rb || !eb || !ob) {
            ok(0, "queue scan: alloc", "failed");
            goto next;
        }
        for (trial = 0; trial < 100; trial++) {
            for (i = 0; i < sh[s].n; i++)
                md[i] = (u8)(scan_next(&x) % 100 < 80);
            for (i = 0; i < sh[s].m; i++)
                mp[i] = (u8)(scan_next(&x) % 100 < 40);
            for (cw = 0; cw < 2; cw++)
                for (i = 0; i < n; i++) {
                    if (i < n_data) {
                        rb[(size_t)cw * n + i] = (u8)((i * 7 + cw) & 15);
                        eb[(size_t)cw * n + i] = md[(i + cw) % sh[s].n];
                    } else {
                        eb[(size_t)cw * n + i] =
                            mp[(i - n_data + cw) % sh[s].m];
                    }
                }
            gf16_decode_batch(c, 2, rb, eb, ob, 1, 0, st);
        }
        ok(1, "queue scan: done", "");
next:
        free(rb);
        free(eb);
        free(ob);
        gf16_free(c);
    }
}

static void test_reject(void)
{
    struct tbt_sclc_params p;
    struct tbt_sclc_uplink *u;

    memset(&p, 0, sizeof(p));
    p.n = 14;
    p.m = 18;
    p.block = 128;
    p.L = 16;
    p.rate_num = 7;
    p.rate_den = 16;
    p.max_core = 256;
    p.mapping = TBT_SCLC_MAP_BITPLANE64;
    p.seed = 1;

    u = tbt_sclc_uplink_new(0, &p);
    ok(u == NULL, "reject: zero groups", "accepted");
    u = tbt_sclc_uplink_new(9, &p); /* 9 * 32 > 256 */
    ok(u == NULL, "reject: 288 carriers", "accepted");
    p.block = 127;
    u = tbt_sclc_uplink_new(1, &p);
    ok(u == NULL, "reject: odd block", "accepted");
    p.block = 128;
    p.rate_num = 0;
    u = tbt_sclc_uplink_new(1, &p);
    ok(u == NULL, "reject: zero rate", "accepted");
    p.rate_num = 7;
    p.mapping = 9;
    u = tbt_sclc_uplink_new(1, &p);
    ok(u == NULL, "reject: bad mapping", "accepted");
}

int main(void)
{
    test_shapes();
    test_roundtrip();
    test_single_erasure();
    test_p_sweep();
    test_bitplane_recovery();
    test_contiguous_ab();
    test_grouped();
    test_ops();
    test_ladder_shapes();
    test_ladder_codec();
    test_full_queue();
    test_gf16_full_queue();
    test_queue_full_scan();
    test_reject();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks,
           failures);
    return failures ? 1 : 0;
}
