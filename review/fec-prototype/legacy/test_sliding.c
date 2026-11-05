#include "sliding.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(cond, ...)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static uint64_t rng_state = 0x243f6a8885a308d3ull;

static uint32_t rnd32(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

static void fill_random(uint8_t *p, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        p[i] = (uint8_t)rnd32();
    }
}

static void *xmalloc(size_t len)
{
    void *p = malloc(len);

    if (!p)
    {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }

    return p;
}

struct tcase
{
    unsigned n;
    unsigned m;
    unsigned k;
    unsigned w;
    unsigned L;
    size_t S;
    enum sliding_scheme scheme;
    unsigned G;
    unsigned P;
    struct sliding_ctx *ctx;
    uint8_t **data;
    uint8_t **parity;
    uint8_t *dbuf;
    uint8_t *pbuf;
    uint8_t *orig;
    uint8_t *dp;
    uint8_t *pp;
};

static const char *scheme_name(enum sliding_scheme s)
{
    switch (s)
    {
    case SLIDING_L0_XOR:
        return "L0";
    case SLIDING_L1_XOR_PART:
        return "L1";
    case SLIDING_L2_RS4:
        return "L2";
    default:
        return "L3";
    }
}

static struct tcase *tc_new(unsigned n, unsigned m, unsigned k, unsigned w,
                            unsigned L, size_t S, enum sliding_scheme scheme)
{
    struct tcase *tc = calloc(1, sizeof(*tc));
    unsigned i;

    if (!tc)
    {
        exit(2);
    }

    tc->n = n;
    tc->m = m;
    tc->k = k;
    tc->w = w;
    tc->L = L;
    tc->S = S;
    tc->scheme = scheme;
    tc->G = L / k + (L % k != 0);
    tc->P = tc->G * m;
    tc->ctx = sliding_new(n, m, k, w, S, scheme);
    if (!tc->ctx)
    {
        free(tc);
        return NULL;
    }

    tc->dbuf = xmalloc((size_t)L * S);
    tc->pbuf = xmalloc((size_t)tc->P * S);
    tc->orig = xmalloc((size_t)L * S);
    tc->data = xmalloc((size_t)L * sizeof(uint8_t *));
    tc->parity = xmalloc((size_t)tc->P * sizeof(uint8_t *));
    tc->dp = xmalloc(L);
    tc->pp = xmalloc(tc->P);

    for (i = 0; i < L; i++)
    {
        tc->data[i] = tc->dbuf + (size_t)i * S;
    }
    for (i = 0; i < tc->P; i++)
    {
        tc->parity[i] = tc->pbuf + (size_t)i * S;
    }

    fill_random(tc->dbuf, (size_t)L * S);
    memcpy(tc->orig, tc->dbuf, (size_t)L * S);

    if (sliding_encode(tc->ctx, tc->data, L, tc->parity, NULL) !=
        (int)tc->P)
    {
        fprintf(stderr, "encode failed for %s (%u,%u,%u,%u) L=%u S=%zu\n",
                scheme_name(scheme), n, m, k, w, L, S);
        exit(2);
    }

    return tc;
}

static void tc_reset(struct tcase *tc)
{
    memcpy(tc->dbuf, tc->orig, (size_t)tc->L * tc->S);
    memset(tc->dp, 0, tc->L);
    memset(tc->pp, 0, tc->P);
}

static void tc_free(struct tcase *tc)
{
    if (!tc)
    {
        return;
    }

    sliding_free(tc->ctx);
    free(tc->dbuf);
    free(tc->pbuf);
    free(tc->orig);
    free(tc->data);
    free(tc->parity);
    free(tc->dp);
    free(tc->pp);
    free(tc);
}

static void window_of(const struct tcase *tc, unsigned g, unsigned *i0,
                      unsigned *i1)
{
    int ws = (int)(g * tc->k) - (int)(tc->w - tc->k);
    int we = (int)(g * tc->k + tc->k - 1);

    *i0 = ws < 0 ? 0 : (unsigned)ws;
    *i1 = we >= (int)tc->L ? tc->L - 1 : (unsigned)we;
}

static void lose_one(struct tcase *tc, unsigned idx)
{
    tc->dp[idx] = 0;
    memset(tc->data[idx], 0xa5, tc->S);
}

static int check_ok(struct tcase *tc, const char *what)
{
    int rc = sliding_decode(tc->ctx, tc->data, tc->parity, tc->dp, tc->pp,
                            tc->L);
    int ok = rc == SLIDING_OK &&
             memcmp(tc->dbuf, tc->orig, (size_t)tc->L * tc->S) == 0;

    if (!ok)
    {
        fprintf(stderr,
                "FAIL %s: %s rc=%d scheme=%s n=%u m=%u k=%u w=%u L=%u S=%zu\n",
                __func__, what, rc, scheme_name(tc->scheme), tc->n, tc->m,
                tc->k, tc->w, tc->L, tc->S);
        failures++;
    }

    return ok;
}

static void prepare_losses(struct tcase *tc)
{
    tc_reset(tc);
    memset(tc->dp, 1, tc->L);
    memset(tc->pp, 1, tc->P);
}

static void test_no_loss(struct tcase *tc)
{
    int rc;

    prepare_losses(tc);
    rc = sliding_decode(tc->ctx, tc->data, tc->parity, tc->dp, tc->pp, tc->L);
    CHECK(rc == SLIDING_OK, "no-loss decode failed scheme=%s (%u,%u,%u,%u)",
          scheme_name(tc->scheme), tc->n, tc->m, tc->k, tc->w);
    CHECK(sliding_recovered_packets(tc->ctx) == 0,
          "no-loss decode recovered packets");
    CHECK(memcmp(tc->dbuf, tc->orig, (size_t)tc->L * tc->S) == 0,
          "no-loss decode changed data");
}

static void run_window_loss(struct tcase *tc, const unsigned *idx,
                            unsigned count)
{
    unsigned i;

    prepare_losses(tc);
    for (i = 0; i < count; i++)
    {
        lose_one(tc, idx[i]);
    }
    check_ok(tc, "window loss");
}

/*
 * For L0, L2 and L3: every 1..m losses inside one window must recover.
 * samples == 0: exhaust k = 1..2, then 20 random sets for larger k.
 */
static void test_window_losses(struct tcase *tc, unsigned samples)
{
    unsigned g;
    unsigned k;

    for (g = 0; g < tc->G; g++)
    {
        unsigned i0;
        unsigned i1;
        unsigned win_len;
        unsigned idx[64];

        window_of(tc, g, &i0, &i1);
        win_len = i1 - i0 + 1;
        if (win_len > 64)
        {
            win_len = 64;
        }
        for (k = 0; k < win_len; k++)
        {
            idx[k] = i0 + k;
        }

        for (k = 1; k <= tc->m && k <= win_len; k++)
        {
            if (k <= 2)
            {
                unsigned a;

                for (a = 0; a < win_len; a++)
                {
                    unsigned one[2];
                    unsigned b;

                    one[0] = idx[a];
                    run_window_loss(tc, one, 1);

                    if (k < 2)
                    {
                        continue;
                    }

                    for (b = a + 1; b < win_len; b++)
                    {
                        one[0] = idx[a];
                        one[1] = idx[b];
                        run_window_loss(tc, one, 2);
                    }
                }
            }
            else
            {
                unsigned t;
                unsigned reps = samples ? samples : 20;

                for (t = 0; t < reps; t++)
                {
                    unsigned pick[16];
                    unsigned a;

                    for (a = 0; a < k; a++)
                    {
                        unsigned j = a + rnd32() % (win_len - a);
                        unsigned tmp = idx[a];

                        idx[a] = idx[j];
                        idx[j] = tmp;
                        pick[a] = idx[a];
                    }
                    run_window_loss(tc, pick, k);
                }
            }
        }
    }
}

/*
 * L1: one loss per subset recovers. Two losses in one subset do not.
 * The negative test uses w == k, so only one group covers the losses.
 */
static void test_l1(struct tcase *tc)
{
    unsigned g = tc->G > 2 ? 2 : 0;
    unsigned i0;
    unsigned i1;
    unsigned row;
    unsigned i;
    unsigned count = 0;

    prepare_losses(tc);
    window_of(tc, g, &i0, &i1);

    for (row = 0; row < tc->m; row++)
    {
        for (i = i0; i <= i1; i++)
        {
            unsigned j = (unsigned)((int)i -
                                    ((int)(g * tc->k) - (int)(tc->w - tc->k)));

            if (j % tc->m == row)
            {
                lose_one(tc, i);
                count++;
                break;
            }
        }
    }

    CHECK(count > 0, "L1 has no subset to lose");
    check_ok(tc, "L1 one loss per subset");

    if (tc->w == tc->k)
    {
        /* No overlap: only group g covers these packets. */
        for (row = 0; row < tc->m; row++)
        {
            unsigned first = 0;
            unsigned second = 0;
            unsigned found = 0;

            for (i = i0; i <= i1; i++)
            {
                unsigned j = (unsigned)((int)i - (int)(g * tc->k));

                if (j % tc->m != row)
                {
                    continue;
                }
                if (found == 0)
                {
                    first = i;
                }
                else if (found == 1)
                {
                    second = i;
                }
                found++;
            }

            if (found < 2)
            {
                continue;
            }

            prepare_losses(tc);
            lose_one(tc, first);
            lose_one(tc, second);
            {
                int rc = sliding_decode(tc->ctx, tc->data, tc->parity, tc->dp,
                                        tc->pp, tc->L);

                CHECK(rc == SLIDING_ERR_STUCK,
                      "L1 two losses in one subset returned %d, want STUCK",
                      rc);
            }
            break;
        }
    }
}

/* m + 1 losses, one at the end of each of m + 1 neighbouring groups. */
static void test_scattered_loss(struct tcase *tc)
{
    unsigned j;

    if ((tc->m + 1) * tc->k > tc->L)
    {
        return;
    }

    prepare_losses(tc);
    for (j = 0; j <= tc->m; j++)
    {
        lose_one(tc, j * tc->k + tc->k - 1);
    }
    check_ok(tc, "scattered loss");
}

/*
 * m + 1 consecutive losses at the start of group 2.
 * When w == k only one group covers them, so they must be stuck.
 * When w > k the overlap can split the run and recover it. Both outcomes are
 * valid. The test checks that a stuck result never writes wrong bytes.
 */
static void test_clustered_loss(struct tcase *tc)
{
    unsigned g = 2;
    unsigned i;
    int rc;

    if (tc->k < 2 || g * tc->k + tc->m >= tc->L)
    {
        return;
    }

    prepare_losses(tc);
    for (i = 0; i <= tc->m; i++)
    {
        lose_one(tc, g * tc->k + i);
    }

    rc = sliding_decode(tc->ctx, tc->data, tc->parity, tc->dp, tc->pp, tc->L);

    if (tc->w == tc->k && tc->m + 1 <= tc->k)
    {
        CHECK(rc == SLIDING_ERR_STUCK,
              "clustered loss returned %d, want STUCK scheme=%s "
              "(%u,%u,%u,%u)",
              rc, scheme_name(tc->scheme), tc->n, tc->m, tc->k, tc->w);
        CHECK(sliding_stuck_at(tc->ctx) == g * tc->k,
              "clustered stuck_at=%u, want %u", sliding_stuck_at(tc->ctx),
              g * tc->k);
    }
    else if (rc == SLIDING_OK)
    {
        CHECK(memcmp(tc->dbuf, tc->orig, (size_t)tc->L * tc->S) == 0,
              "clustered overlap recovery wrote wrong data");
        return;
    }

    for (i = 0; i < tc->L; i++)
    {
        if (sliding_is_known(tc->ctx, i))
        {
            CHECK(memcmp(tc->data[i], tc->orig + (size_t)i * tc->S,
                         tc->S) == 0,
                  "clustered known packet %u wrong", i);
        }
        else
        {
            size_t b;
            int all_a5 = 1;

            for (b = 0; b < tc->S; b++)
            {
                if (tc->data[i][b] != 0xa5)
                {
                    all_a5 = 0;
                    break;
                }
            }
            CHECK(all_a5, "clustered unknown packet %u overwritten", i);
        }
    }
}

static void test_order_and_late(struct tcase *tc)
{
    unsigned i;
    int f;

    if (tc->k < 2)
    {
        return;
    }

    tc_reset(tc);
    CHECK(sliding_bind(tc->ctx, tc->data, tc->parity, tc->L) == SLIDING_OK,
          "bind failed");

    f = sliding_advance(tc->ctx, SLIDING_KIND_PARITY, 0);
    CHECK(f == 0, "early parity advanced frontier to %d", f);

    for (i = 0; i < tc->k; i++)
    {
        f = sliding_advance(tc->ctx, SLIDING_KIND_DATA, i);
        CHECK(f >= (int)(i + 1), "frontier %d after data %u", f, i);
    }
    CHECK(f == (int)tc->k, "frontier %d after k data packets, want %u", f,
          tc->k);

    tc_reset(tc);
    CHECK(sliding_bind(tc->ctx, tc->data, tc->parity, tc->L) == SLIDING_OK,
          "bind failed");

    f = sliding_advance(tc->ctx, SLIDING_KIND_DATA, 1);
    CHECK(f == 0, "data 1 alone advanced frontier to %d", f);
    f = sliding_advance(tc->ctx, SLIDING_KIND_DATA, 0);
    CHECK(f == 2, "frontier %d after data 0, want 2", f);

    tc_reset(tc);
    CHECK(sliding_bind(tc->ctx, tc->data, tc->parity, tc->L) == SLIDING_OK,
          "bind failed");

    for (i = 1; i < tc->k; i++)
    {
        f = sliding_advance(tc->ctx, SLIDING_KIND_DATA, i);
        CHECK(f == 0, "gap run advanced to %d", f);
    }

    f = sliding_advance(tc->ctx, SLIDING_KIND_PARITY, 0);
    CHECK(f == (int)tc->k, "parity recovery frontier %d, want %u", f, tc->k);
    CHECK(memcmp(tc->data[0], tc->orig, tc->S) == 0,
          "parity recovery wrote wrong data");

    f = sliding_advance(tc->ctx, SLIDING_KIND_DATA, 0);
    CHECK(f == (int)tc->k, "late packet moved frontier to %d", f);
    CHECK(memcmp(tc->data[0], tc->orig, tc->S) == 0,
          "late packet changed data");
}

static void test_frontier_reset(struct tcase *tc)
{
    unsigned i;
    int f;

    tc_reset(tc);
    CHECK(sliding_bind(tc->ctx, tc->data, tc->parity, tc->L) == SLIDING_OK,
          "bind failed");

    for (i = 0; i < tc->L; i++)
    {
        unsigned g;
        unsigned expected = 0;

        f = sliding_advance(tc->ctx, SLIDING_KIND_DATA, i);
        CHECK(f == (int)(i + 1), "frontier %d after data %u, want %u", f, i,
              i + 1);

        for (g = 0; g < tc->G; g++)
        {
            int we = (int)(g * tc->k + tc->k - 1);

            if (we < (int)(i + 1))
            {
                expected++;
            }
        }

        CHECK(sliding_retired_groups(tc->ctx) == expected,
              "retired %u after data %u, want %u",
              sliding_retired_groups(tc->ctx), i, expected);
    }
}

static void test_edges(void)
{
    CHECK(sliding_new(4, 0, 8, 16, 64, SLIDING_L3_RS8) == NULL, "m=0");
    CHECK(sliding_new(4, 1, 0, 16, 64, SLIDING_L3_RS8) == NULL, "k=0");
    CHECK(sliding_new(4, 1, 16, 8, 64, SLIDING_L3_RS8) == NULL, "k>w");
    CHECK(sliding_new(4, 1, 8, 16, 0, SLIDING_L3_RS8) == NULL, "s=0");
    CHECK(sliding_new(4, 2, 8, 16, 64, SLIDING_L0_XOR) == NULL, "L0 m=2");
    CHECK(sliding_new(16, 8, 8, 16, 64, SLIDING_L2_RS4) == NULL,
          "L2 n+m>15");
    CHECK(sliding_new(4, 2, 8, 16, 64, SLIDING_L2_RS4) == NULL,
          "L2 m+w>16");
    CHECK(sliding_new(4, 1, 8, 255, 64, SLIDING_L3_RS8) != NULL,
          "L3 m+w=256 rejected");
    sliding_free(sliding_new(4, 1, 8, 255, 64, SLIDING_L3_RS8));
    CHECK(sliding_new(4, 1, 8, 256, 64, SLIDING_L3_RS8) == NULL,
          "L3 m+w=257 accepted");

    printf("edges ok\n");
}

static void test_fuzz(unsigned rounds)
{
    static const size_t sizes[] = {1, 64, 256};
    unsigned r;

    for (r = 0; r < rounds; r++)
    {
        unsigned scheme = rnd32() % 4;
        unsigned m;
        unsigned k = 1 + rnd32() % 16;
        unsigned w;
        unsigned n;
        unsigned L = 8 + rnd32() % 121;
        size_t S = sizes[rnd32() % 3];
        unsigned G;
        unsigned P;
        struct sliding_ctx *ctx;
        uint8_t **data;
        uint8_t **parity;
        uint8_t *dbuf;
        uint8_t *pbuf;
        uint8_t *orig;
        uint8_t *dp;
        uint8_t *pp;
        unsigned i;
        int rc;

        if (scheme == SLIDING_L0_XOR)
        {
            m = 1;
            n = 1 + rnd32() % 8;
        }
        else
        {
            m = 1 + rnd32() % 4;
            n = 1 + rnd32() % 8;
        }

        w = k + rnd32() % (33 - k);

        if (scheme == SLIDING_L2_RS4)
        {
            if (n + m > 15)
            {
                n = 15 - m;
            }
            if (m + w > 16)
            {
                w = 16 - m;
                if (w < k)
                {
                    continue;
                }
            }
        }

        ctx = sliding_new(n, m, k, w, S, (enum sliding_scheme)scheme);
        CHECK(ctx != NULL, "fuzz: new failed scheme=%u n=%u m=%u k=%u w=%u",
              scheme, n, m, k, w);
        if (!ctx)
        {
            continue;
        }

        G = L / k + (L % k != 0);
        P = G * m;

        data = xmalloc((size_t)L * sizeof(uint8_t *));
        parity = xmalloc((size_t)P * sizeof(uint8_t *));
        dbuf = xmalloc((size_t)L * S);
        pbuf = xmalloc((size_t)P * S);
        orig = xmalloc((size_t)L * S);
        dp = xmalloc(L);
        pp = xmalloc(P);

        for (i = 0; i < L; i++)
        {
            data[i] = dbuf + (size_t)i * S;
        }
        for (i = 0; i < P; i++)
        {
            parity[i] = pbuf + (size_t)i * S;
        }

        fill_random(dbuf, (size_t)L * S);
        memcpy(orig, dbuf, (size_t)L * S);
        CHECK(sliding_encode(ctx, data, L, parity, NULL) == (int)P,
              "fuzz: encode failed");

        for (i = 0; i < L; i++)
        {
            dp[i] = rnd32() % 10 != 0;
            if (!dp[i])
            {
                memset(data[i], 0xa5, S);
            }
        }
        for (i = 0; i < P; i++)
        {
            pp[i] = rnd32() % 10 != 0;
        }

        rc = sliding_decode(ctx, data, parity, dp, pp, L);
        if (rc == SLIDING_OK)
        {
            CHECK(memcmp(dbuf, orig, (size_t)L * S) == 0,
                  "fuzz: OK but data mismatch r=%u scheme=%u n=%u m=%u k=%u "
                  "w=%u L=%u S=%zu",
                  r, scheme, n, m, k, w, L, S);
        }
        else
        {
            CHECK(rc == SLIDING_ERR_STUCK, "fuzz: rc=%d", rc);
            for (i = 0; i < L; i++)
            {
                if (sliding_is_known(ctx, i))
                {
                    CHECK(memcmp(data[i], orig + (size_t)i * S, S) == 0,
                          "fuzz: known packet %u wrong r=%u scheme=%u n=%u "
                          "m=%u k=%u w=%u L=%u S=%zu",
                          i, r, scheme, n, m, k, w, L, S);
                }
                else
                {
                    size_t b;
                    int all_a5 = 1;

                    for (b = 0; b < S; b++)
                    {
                        if (data[i][b] != 0xa5)
                        {
                            all_a5 = 0;
                            break;
                        }
                    }
                    CHECK(all_a5, "fuzz: unknown packet %u overwritten r=%u",
                          i, r);
                }
            }
        }

        free(data);
        free(parity);
        free(dbuf);
        free(pbuf);
        free(orig);
        free(dp);
        free(pp);
        sliding_free(ctx);
    }

    printf("fuzz: %u rounds\n", rounds);
}

int main(void)
{
    static const unsigned nm[][2] = {
        {1, 1}, {2, 1}, {4, 1}, {4, 2}, {8, 2}, {8, 4}, {16, 8},
    };
    static const unsigned kw[][2] = {
        {4, 8}, {8, 16}, {8, 32}, {16, 32}, {8, 8},
    };
    static const enum sliding_scheme schemes[] = {
        SLIDING_L0_XOR, SLIDING_L1_XOR_PART, SLIDING_L2_RS4, SLIDING_L3_RS8,
    };
    unsigned si;
    unsigned ni;
    unsigned ki;

    test_edges();

    for (si = 0; si < 4; si++)
    {
        for (ni = 0; ni < sizeof(nm) / sizeof(nm[0]); ni++)
        {
            for (ki = 0; ki < sizeof(kw) / sizeof(kw[0]); ki++)
            {
                unsigned n = nm[ni][0];
                unsigned m = nm[ni][1];
                unsigned k = kw[ki][0];
                unsigned w = kw[ki][1];
                struct tcase *tc =
                    tc_new(n, m, k, w, 64, 64, schemes[si]);

                if (!tc)
                {
                    continue;
                }

                test_no_loss(tc);
                if (schemes[si] == SLIDING_L1_XOR_PART)
                {
                    test_l1(tc);
                }
                else
                {
                    test_window_losses(tc, 0);
                    test_scattered_loss(tc);
                    test_clustered_loss(tc);
                }

                tc_free(tc);
            }
        }
    }

    /* Larger packets and streams: random samples only. */
    for (si = 0; si < 4; si++)
    {
        for (ni = 0; ni < sizeof(nm) / sizeof(nm[0]); ni++)
        {
            unsigned n = nm[ni][0];
            unsigned m = nm[ni][1];
            unsigned k = kw[0][0];
            unsigned w = kw[0][1];
            struct tcase *tc;

            tc = tc_new(n, m, k, w, 256, 1024, schemes[si]);
            if (tc)
            {
                test_no_loss(tc);
                if (schemes[si] != SLIDING_L1_XOR_PART)
                {
                    test_window_losses(tc, 10);
                }
                tc_free(tc);
            }

            tc = tc_new(n, m, k, w, 1024, 8192, schemes[si]);
            if (tc)
            {
                test_no_loss(tc);
                if (schemes[si] != SLIDING_L1_XOR_PART)
                {
                    test_window_losses(tc, 2);
                }
                tc_free(tc);
            }
        }
    }

    /* Order, late delivery, frontier and retire on one representative. */
    {
        struct tcase *tc = tc_new(4, 2, 8, 16, 64, 64, SLIDING_L3_RS8);

        if (tc)
        {
            test_order_and_late(tc);
            test_frontier_reset(tc);
            tc_free(tc);
        }
    }

    test_fuzz(1000);

    if (failures != 0)
    {
        fprintf(stderr, "FAIL: %u check(s) failed\n", failures);
        return 1;
    }

    printf("PASS: sliding-window L0/L1/L2/L3\n");
    return 0;
}
