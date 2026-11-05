#include "rlnc.h"

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
    unsigned d;
    unsigned L;
    size_t S;
    unsigned G;
    unsigned P;
    struct rlnc_ctx *ctx;
    uint8_t **data;
    uint8_t **parity;
    uint8_t *dbuf;
    uint8_t *pbuf;
    uint8_t *orig;
    uint8_t *dp;
    uint8_t *pp;
};

static struct tcase *tc_new(unsigned n, unsigned m, unsigned k, unsigned w,
                            unsigned d, unsigned L, size_t S, uint64_t nonce)
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
    tc->d = d;
    tc->L = L;
    tc->S = S;
    tc->G = L / k + (L % k != 0);
    tc->P = tc->G * m;
    tc->ctx = rlnc_new(n, m, k, w, S, d, nonce);
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

    if (rlnc_encode(tc->ctx, tc->data, L, tc->parity) != (int)tc->P)
    {
        fprintf(stderr, "encode failed\n");
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

    rlnc_free(tc->ctx);
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
    int ws = (int)(g * tc->k) + (int)tc->k - (int)tc->w;
    int we = (int)(g * tc->k + tc->k - 1);

    *i0 = ws < 0 ? 0 : (unsigned)ws;
    *i1 = we >= (int)tc->L ? tc->L - 1 : (unsigned)we;
}

static void lose_one(struct tcase *tc, unsigned idx)
{
    tc->dp[idx] = 0;
    memset(tc->data[idx], 0xa5, tc->S);
}

static void prepare(struct tcase *tc)
{
    tc_reset(tc);
    memset(tc->dp, 1, tc->L);
    memset(tc->pp, 1, tc->P);
}

static void test_no_loss(struct tcase *tc)
{
    int rc;

    prepare(tc);
    rc = rlnc_decode(tc->ctx, tc->data, tc->parity, tc->dp, tc->pp, tc->L);
    CHECK(rc == 0, "no-loss decode returned %d, want 0", rc);
    CHECK(rlnc_recovered_packets(tc->ctx) == 0,
          "no-loss decode recovered packets");
    CHECK(memcmp(tc->dbuf, tc->orig, (size_t)tc->L * tc->S) == 0,
          "no-loss decode changed data");
}

static void test_window_losses(struct tcase *tc)
{
    unsigned u;
    unsigned trials = 100;
    unsigned success = 0;
    unsigned total = 0;

    /* Only losses with at least 2 equations of overhead: the m-loss case is
     * the capacity boundary and is covered by test_too_many_losses. */
    for (u = 1; u + 6 <= tc->m; u++)
    {
        unsigned t;

        for (t = 0; t < trials; t++)
        {
            unsigned g = rnd32() % tc->G;
            unsigned i0;
            unsigned i1;
            unsigned win_len;
            unsigned idx[256];
            unsigned a;
            int rc;

            prepare(tc);
            window_of(tc, g, &i0, &i1);
            win_len = i1 - i0 + 1;
            if (win_len > 256)
            {
                win_len = 256;
            }
            for (a = 0; a < win_len; a++)
            {
                idx[a] = i0 + a;
            }

            for (a = 0; a < u && a < win_len; a++)
            {
                unsigned j = a + rnd32() % (win_len - a);
                unsigned tmp = idx[a];

                idx[a] = idx[j];
                idx[j] = tmp;
                lose_one(tc, idx[a]);
            }

            rc = rlnc_decode(tc->ctx, tc->data, tc->parity, tc->dp, tc->pp,
                             tc->L);
            total++;
            if (rc >= 0)
            {
                if (memcmp(tc->dbuf, tc->orig, (size_t)tc->L * tc->S) == 0)
                {
                    success++;
                }
                else
                {
                    CHECK(0, "rlnc OK but data mismatch u=%u", u);
                }
            }
            else
            {
                CHECK(rc == RLNC_ERR_STUCK, "rlnc rc=%d", rc);
            }
        }
    }

    printf("window losses: %u/%u recovered (%.1f%%)\n", success, total,
           100.0 * success / total);
    CHECK(success * 1000 >= total * 999,
          "rlnc window recovery below 99.9%%: %u/%u", success, total);
}

static void test_too_many_losses(struct tcase *tc)
{
    unsigned g = 2;
    unsigned i;
    int rc;

    if (g * tc->k + tc->m >= tc->L)
    {
        return;
    }

    prepare(tc);
    for (i = 0; i <= tc->m; i++)
    {
        lose_one(tc, g * tc->k + i);
    }

    rc = rlnc_decode(tc->ctx, tc->data, tc->parity, tc->dp, tc->pp, tc->L);
    if (rc >= 0)
    {
        CHECK(memcmp(tc->dbuf, tc->orig, (size_t)tc->L * tc->S) == 0,
              "over-loss recovery wrote wrong data");
    }
    else
    {
        CHECK(rc == RLNC_ERR_STUCK, "over-loss rc=%d", rc);
        for (i = 0; i < tc->L; i++)
        {
            if (rlnc_is_known(tc->ctx, i))
            {
                CHECK(memcmp(tc->data[i], tc->orig + (size_t)i * tc->S,
                             tc->S) == 0,
                      "known packet %u wrong after stuck", i);
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
                CHECK(all_a5, "unknown packet %u overwritten after stuck", i);
            }
        }
    }
}

static void test_order_and_late(struct tcase *tc)
{
    unsigned i;
    int f;

    tc_reset(tc);
    CHECK(rlnc_bind(tc->ctx, tc->data, tc->parity, tc->L) == RLNC_OK,
          "bind failed");

    f = rlnc_advance(tc->ctx, RLNC_KIND_PARITY, 0);
    CHECK(f == 0, "early parity advanced frontier to %d", f);

    for (i = 0; i < tc->k; i++)
    {
        f = rlnc_advance(tc->ctx, RLNC_KIND_DATA, i);
        CHECK(f >= (int)(i + 1), "frontier %d after data %u", f, i);
    }

    tc_reset(tc);
    CHECK(rlnc_bind(tc->ctx, tc->data, tc->parity, tc->L) == RLNC_OK,
          "bind failed");
    f = rlnc_advance(tc->ctx, RLNC_KIND_DATA, 1);
    CHECK(f == 0, "data 1 alone advanced frontier to %d", f);
    f = rlnc_advance(tc->ctx, RLNC_KIND_DATA, 0);
    CHECK(f == 2, "frontier %d after data 0, want 2", f);

    /* Late packet: recover packet 0 from parity, then deliver it again. */
    tc_reset(tc);
    CHECK(rlnc_bind(tc->ctx, tc->data, tc->parity, tc->L) == RLNC_OK,
          "bind failed");
    for (i = 1; i < tc->k; i++)
    {
        f = rlnc_advance(tc->ctx, RLNC_KIND_DATA, i);
        CHECK(f == 0, "gap run advanced to %d", f);
    }
    for (i = 0; i < tc->m; i++)
    {
        rlnc_advance(tc->ctx, RLNC_KIND_PARITY, i);
    }
    if (rlnc_is_known(tc->ctx, 0))
    {
        CHECK(memcmp(tc->data[0], tc->orig, tc->S) == 0,
              "recovered packet 0 wrong");
    }
    f = rlnc_advance(tc->ctx, RLNC_KIND_DATA, 0);
    CHECK(f >= 1, "late packet did not advance frontier");
    CHECK(memcmp(tc->data[0], tc->orig, tc->S) == 0,
          "late packet changed data");
}

static void test_frontier(struct tcase *tc)
{
    unsigned i;

    tc_reset(tc);
    CHECK(rlnc_bind(tc->ctx, tc->data, tc->parity, tc->L) == RLNC_OK,
          "bind failed");

    for (i = 0; i < tc->L; i++)
    {
        int f = rlnc_advance(tc->ctx, RLNC_KIND_DATA, i);
        unsigned g;
        unsigned expected = 0;

        CHECK(f == (int)(i + 1), "frontier %d after data %u", f, i);
        for (g = 0; g < tc->G; g++)
        {
            int we = (int)(g * tc->k + tc->k - 1);

            if (we < (int)(i + 1))
            {
                expected++;
            }
        }
        CHECK(rlnc_retired_groups(tc->ctx) == expected,
              "retired %u after data %u, want %u",
              rlnc_retired_groups(tc->ctx), i, expected);
    }
}

static void test_prng(struct tcase *tc)
{
    unsigned a[256];
    unsigned b[256];
    unsigned g;
    unsigned r;

    for (g = 0; g < tc->G && g < 8; g++)
    {
        for (r = 0; r < tc->m; r++)
        {
            unsigned na = rlnc_coeffs(tc->ctx, g, r, a, 256);
            unsigned nb = rlnc_coeffs(tc->ctx, g, r, b, 256);
            unsigned i;

            CHECK(na == tc->d && nb == tc->d, "coeff count mismatch");
            CHECK(memcmp(a, b, na * sizeof(unsigned)) == 0,
                  "coeffs are not reproducible");
            for (i = 0; i < na; i++)
            {
                unsigned j;

                CHECK(a[i] < tc->w, "coeff %u out of range", a[i]);
                for (j = i + 1; j < na; j++)
                {
                    CHECK(a[i] != a[j], "coeffs repeat");
                }
            }
        }
    }
}

static void test_edges(void)
{
    CHECK(rlnc_new(4, 0, 8, 16, 64, 8, 1) == NULL, "m=0 accepted");
    CHECK(rlnc_new(4, 1, 8, 16, 64, 0, 1) == NULL, "d=0 accepted");
    CHECK(rlnc_new(4, 1, 8, 16, 64, 17, 1) == NULL, "d>w accepted");
    CHECK(rlnc_new(4, 1, 16, 8, 64, 8, 1) == NULL, "k>w accepted");
    CHECK(rlnc_new(4, 1, 8, 16, 0, 8, 1) == NULL, "s=0 accepted");
    CHECK(rlnc_new(4, 1, 8, 16, 64, 16, 1) != NULL, "d=w rejected");
    rlnc_free(rlnc_new(4, 1, 8, 16, 64, 16, 1));
}

static void test_fuzz(unsigned rounds)
{
    unsigned r;

    for (r = 0; r < rounds; r++)
    {
        unsigned n = 1 + rnd32() % 16;
        unsigned m = 1 + rnd32() % 8;
        unsigned k = 1 + rnd32() % 16;
        unsigned w = k + rnd32() % (33 - k);
        unsigned d = 1 + rnd32() % w;
        unsigned L = 8 + rnd32() % 121;
        size_t S = (size_t)1 << (rnd32() % 8);
        unsigned G = L / k + (L % k != 0);
        unsigned P = G * m;
        struct rlnc_ctx *ctx = rlnc_new(n, m, k, w, S, d, rnd32());
        uint8_t **data;
        uint8_t **parity;
        uint8_t *dbuf;
        uint8_t *pbuf;
        uint8_t *orig;
        uint8_t *dp;
        uint8_t *pp;
        unsigned i;
        int rc;

        CHECK(ctx != NULL, "fuzz: new failed");
        if (!ctx)
        {
            continue;
        }

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
        CHECK(rlnc_encode(ctx, data, L, parity) == (int)P, "fuzz: encode");

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

        rc = rlnc_decode(ctx, data, parity, dp, pp, L);
        if (rc >= 0)
        {
            CHECK(memcmp(dbuf, orig, (size_t)L * S) == 0,
                  "fuzz: OK but mismatch r=%u", r);
        }
        else
        {
            CHECK(rc == RLNC_ERR_STUCK, "fuzz: rc=%d", rc);
            for (i = 0; i < L; i++)
            {
                if (rlnc_is_known(ctx, i))
                {
                    CHECK(memcmp(data[i], orig + (size_t)i * S, S) == 0,
                          "fuzz: known %u wrong r=%u", i, r);
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
        rlnc_free(ctx);
    }

    printf("rlnc fuzz: %u rounds\n", rounds);
}

int main(void)
{
    struct tcase *tc;

    test_edges();

    tc = tc_new(16, 16, 16, 64, 32, 256, 64, 0x123456789abcdefull);
    CHECK(tc != NULL, "tc_new failed");
    if (tc)
    {
        test_no_loss(tc);
        test_window_losses(tc);
        test_too_many_losses(tc);
        test_order_and_late(tc);
        test_frontier(tc);
        test_prng(tc);
        tc_free(tc);
    }

    tc = tc_new(96, 32, 96, 256, 16, 512, 64, 0xfeedfacecafebeefull);
    CHECK(tc != NULL, "large tc_new failed");
    if (tc)
    {
        test_no_loss(tc);
        test_prng(tc);
        tc_free(tc);
    }

    test_fuzz(1000);

    if (failures != 0)
    {
        fprintf(stderr, "FAIL: %u check(s) failed\n", failures);
        return 1;
    }

    printf("PASS: rlnc\n");
    return 0;
}
