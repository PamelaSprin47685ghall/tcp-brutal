#include "rlnc.h"

#include <inttypes.h>
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
    struct rlnc_ctx *ctx;

    CHECK(rlnc_new(4, 0, 8, 16, 64, 8, 1) == NULL, "m=0 accepted");
    CHECK(rlnc_new(4, 1, 8, 16, 64, 0, 1) == NULL, "d=0 accepted");
    CHECK(rlnc_new(4, 1, 8, 16, 64, 17, 1) == NULL, "d>w accepted");
    CHECK(rlnc_new(4, 1, 16, 8, 64, 8, 1) == NULL, "k>w accepted");
    CHECK(rlnc_new(4, 1, 8, 16, 0, 8, 1) == NULL, "s=0 accepted");
    ctx = rlnc_new(4, 1, 8, 16, 64, 16, 1);
    CHECK(ctx != NULL, "d=w rejected");
    rlnc_free(ctx);
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

/* ------------------------------------------------------------------ */
/* SHA-256 (FIPS 180-4). Self-contained so the frozen vectors need no
 * external dependency and stay identical on every host. */

struct sha256
{
    uint32_t h[8];
    uint64_t bits;
    uint8_t buf[64];
    size_t fill;
};

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_block(struct sha256 *s, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    unsigned i;

    for (i = 0; i < 16; i++)
    {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    }
    for (i = 16; i < 64; i++)
    {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = s->h[0];
    b = s->h[1];
    c = s->h[2];
    d = s->h[3];
    e = s->h[4];
    f = s->h[5];
    g = s->h[6];
    h = s->h[7];

    for (i = 0; i < 64; i++)
    {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

static void sha256_init(struct sha256 *s)
{
    static const uint32_t iv[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    memcpy(s->h, iv, sizeof(iv));
    s->bits = 0;
    s->fill = 0;
}

static void sha256_update(struct sha256 *s, const void *data, size_t len)
{
    const uint8_t *p = data;

    s->bits += (uint64_t)len * 8u;
    while (len > 0)
    {
        size_t take = 64 - s->fill;

        if (take > len)
        {
            take = len;
        }
        memcpy(s->buf + s->fill, p, take);
        s->fill += take;
        p += take;
        len -= take;
        if (s->fill == 64)
        {
            sha256_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void sha256_final(struct sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->bits;
    unsigned i;

    s->buf[s->fill++] = 0x80;
    if (s->fill > 56)
    {
        memset(s->buf + s->fill, 0, 64 - s->fill);
        sha256_block(s, s->buf);
        s->fill = 0;
    }
    memset(s->buf + s->fill, 0, 56 - s->fill);
    for (i = 0; i < 8; i++)
    {
        s->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha256_block(s, s->buf);

    for (i = 0; i < 8; i++)
    {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++)
    {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

static int hex_decode(const char *hex, uint8_t *out, size_t n)
{
    size_t i;

    if (strlen(hex) != 2 * n)
    {
        return -1;
    }
    for (i = 0; i < n; i++)
    {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);

        if (hi < 0 || lo < 0)
        {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static void test_sha256(void)
{
    static const char empty_hex[] =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    static const char abc_hex[] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    struct sha256 s;
    uint8_t d[32];
    char hex[65];

    sha256_init(&s);
    sha256_final(&s, d);
    hex_encode(d, 32, hex);
    CHECK(strcmp(hex, empty_hex) == 0, "sha256(empty) = %s", hex);

    sha256_init(&s);
    sha256_update(&s, "abc", 3);
    sha256_final(&s, d);
    hex_encode(d, 32, hex);
    CHECK(strcmp(hex, abc_hex) == 0, "sha256(abc) = %s", hex);
}

/* ------------------------------------------------------------------ */
/* Frozen vectors.
 *
 * One row per (W, d, seed). Parameters: K = W/2, N = K, M = 1, L = W,
 * S = 64, nonce = seed. Packet j, byte i is
 *     (uint8_t)(j*131 + i*17 + seed)
 * (the cast applies to the whole sum, so only seed & 0xff matters).
 *
 * The fixed loss set is one data packet that parity row (g=0, r=0)
 * covers. Row 0's window starts at k-w = -k, so only coefficients
 * c >= k map to real packets (packet c-k); we drop the first such
 * packet. If row 0 covers no packet at all (possible for small d), we
 * drop the first packet of the last group's row instead. Exactly one
 * unknown remains in a present parity row with coefficient 1, so
 * rlnc_decode() must return 1 and restore the bytes. The dropped
 * position varies with seed.
 *
 * The digest is SHA256(parity_bytes || data_bytes_after_decode), where
 * parity_bytes is P*S bytes (P = ceil(L/K) * M) and data_bytes_after_decode
 * is L*S bytes. One digest therefore covers both the encode output and the
 * decoder's effect on the data buffer. vector_digest() asserts rc == 1
 * and byte equality; a failure aborts loudly instead of emitting a digest.
 *
 * Grouped windows W=4,6,8,10,12 appear twice: d=W is degenerate (every
 * parity row is the all-ones row, rank 1), d=W/2 is production. W=16..480
 * are single-group/lab windows. Regenerate the table with:
 * ./test_rlnc --emit
 */
struct vector
{
    unsigned w;
    unsigned d;
    uint64_t seed;
    const char *digest;
};

static const struct vector vectors[] = {
    /* Degenerate grouped d=W (rank 1). */
    {4, 4, 0, "7ac3c999491e7e8a413cfd2d143401d73e0b65c97ac6b6513b9100b7accc32c2"},
    {4, 4, 1, "543e27a5ec03fbaeebef3d45ec379af89f54b53086287ee2814e858dd2bd8e74"},
    {4, 4, 2, "5594c3383b2451f9dfbae9608c3b44bc6052f7f83a4122a61566bce62b53d09c"},
    {4, 4, 3, "50721d1803acae69ef33a9ecd8784d91c3934c53f131a98ded5b00c3a80c6ed4"},
    {4, 4, 4, "8039bfc7e52ce2a34b84cf2c94bb290e169c2ab8b6ec562cc923cf6d83f639cc"},
    {4, 4, 5, "a80f24fc4300d36f52893fcebb14b86fe6177e9c2d710b1a785aef5ce18b89c5"},
    {4, 4, 6, "8b92a6037f87d14c26038158326e1d8ba746a5ccc45dbbb4eaa09fbf4c56d85f"},
    {4, 4, 7, "71ee936e1fe8a423e6425183ca45b16a27110e6986ba82cb4b16970721ecf5d7"},
    {4, 4, 8, "07c89009a74b8981c15f1b21d32f282a1997420d6ca47c53f4734956fc8ee65d"},
    {4, 4, 9, "3be24b3f693878c260d57237b476fac873bcda101208baf57750ef3eb22c33b0"},
    {4, 4, 10, "0d76123d398f31c91536cb2e25ed779e8d7f4c843a128f7176abd340a63d867b"},
    {4, 4, 11, "1e718e30c7c782011e0a2650e80ba397d8a851391f1443345f3a6a1bb0b7855f"},
    {4, 4, 12, "e2a388ba3d0492321d060750154592d42b6be30cb62d17172f6fbca05a86d1e0"},
    {4, 4, 13, "1beee5a93a74ba1201cc3c97007fdd326014281c526f72cb7f2dcd5e4dd366bd"},
    {4, 4, 14, "e9821d4260f2f7ab4ef6bc977170d101aeceedff09065e8bc820c81934dc241a"},
    {4, 4, 15, "d167cf7a3569342236bcc8a80af843a64b8e8eecdf9e09c2b49ac2529cc5bc45"},
    {6, 6, 0, "bdd661cd6ec32f8701fc6f1bb4465c73c9d665ac7d8047ca940808d023ee1cd4"},
    {6, 6, 1, "dbf6f723587b58468143e3198a660ce7e9863cc904dd0928d2c35eafa7d47362"},
    {6, 6, 2, "f73b634ed3d2ee3fc4dd9e8baf4efd59f253698a91abcf886e35e00bcdee8829"},
    {6, 6, 3, "07b9362e54b36e3b907cd34963efc4f818b82e8529f8ece5d4cb03216805447b"},
    {6, 6, 4, "8d4d3aa2e86c26c259a5fcdd23d4f4a09edb303b1a344f51d4f848d4e03fa1d9"},
    {6, 6, 5, "ce51aa2d9de0e2288f2ab4b5a5f09a09c1b006b04d45b1676da1ea1e3112c997"},
    {6, 6, 6, "e0e1c45515b4dd937df8acf6ba57ec3f4d5ad5d000135889f6ab26ea45bbee52"},
    {6, 6, 7, "dc4968cf737adc82c1181c82c93a888472f3470b7c9d01112103dc06e587c249"},
    {6, 6, 8, "a5c6fb525ea5fef60bfaa692764953812d17970ec6a7019abc50a45e9847b81f"},
    {6, 6, 9, "aaacf1fc4b67e2558fdf1331abcfdf243d0f24c6d6a8b057bbad7457b24720b3"},
    {6, 6, 10, "4bd1a15a8831530afcf0b91321a0e6d46f0a060586e034bae0553013e78910b2"},
    {6, 6, 11, "0036466b3684b1a91f107cb4569f0a938e25be26dee6130af6ae60c1d5711079"},
    {6, 6, 12, "374ac9e6f1f1c6157500b86fa9852f58b3f7d0d3f930b4c75f21dd3bcb6e0981"},
    {6, 6, 13, "e26bbc99d9f9faabd8cba73ee86e71f7f19d8d768452f6aff4d27e9bc14f4b23"},
    {6, 6, 14, "483484ac63918d21f714aaaefc9b51d7b41625250a1892365766037b8de921f4"},
    {6, 6, 15, "3af18959bd212a9d7bc65d087b72e8a8a4d79433c42af7808b3a17bdc7e3bee0"},
    {8, 8, 0, "73cf760a782867ccfc583fa04cff7e0d01a27ac8534009896e56a3effe89714c"},
    {8, 8, 1, "b25627d43d2e24f458e91a564210f51d7bc7ff82f2ba69c0392d7a1fb2181522"},
    {8, 8, 2, "af05472e32c3b1c3ef0606e1d51a90dba639d8175f634edf1ebc50d684853897"},
    {8, 8, 3, "96ffffc99e647fc7942c29d350b1aca585f750d2caa8e6cc6ac01fc816cac504"},
    {8, 8, 4, "759b9db9af2d110de908afa0d47e58ecb06f328c19db26005c3afad62d0b272b"},
    {8, 8, 5, "dc400f9c55fd05e0b986368a4434df5b9e2dcb476093dad1eaa05fcfcaf8745c"},
    {8, 8, 6, "6d956628223aa4836f0c7a6d08f8339a557d20d60b94cce286e8c9e3b89e1f6c"},
    {8, 8, 7, "fc92fe6a964b8ade6a66d36490c03f9d373d0a1881c82141b0ddbde7898c37c1"},
    {8, 8, 8, "cd4bfa5b0713482e1ae3f56b0680fc02ea0c75b32ac3c87c123129df26e62ac7"},
    {8, 8, 9, "3acbda6986aa34c5f67addffffee06363c6d0a695712f3dc794a3bb8ad0e1c11"},
    {8, 8, 10, "1913bb068bcd0aaf02d4e0cc61b2d89a3c084ba2a420ffafb1db7b57d386b965"},
    {8, 8, 11, "6a368e2e8f678fd15aad7a83459591a41b41868b727361de0f5b45df069a638c"},
    {8, 8, 12, "e702bfe63151d9adf68249ee5c9e1b083c8f277031c0cb42fe35f03175564d7e"},
    {8, 8, 13, "03c663a803eab5333ea69adaf512f955da29b5bbca6dff21302ef9415f2ff609"},
    {8, 8, 14, "9d38cd2bd4be3c9b71b3750c0371893b7f5d29d096ddabb6ace48595c2389f00"},
    {8, 8, 15, "6e7e58c81c46957a6ced7d298dace90d23f58edbe2d06fe810525bdc213fa4f8"},
    {10, 10, 0, "88e1cd7ff22634a9643ddcaae93afe09f3518a9c7a3621bb41ba68fcc04f9a35"},
    {10, 10, 1, "a757b0d414bf0ab5fe4872bd1757a748893ae8be270ec693a3d3a35ec4b06ba0"},
    {10, 10, 2, "606882149a04e1b51a17b64251ec20113d2230243bdef8b9a3b83af1084500f1"},
    {10, 10, 3, "aa87ff752f3e98c229ee7ff9df244ad94aaf32ae16539e2a6e3e9c6e2eae1aeb"},
    {10, 10, 4, "dbfa9d2a1400b3271db9fbfbad0e717ef1fffa589b2afe31c311f2199785853a"},
    {10, 10, 5, "1c6e47ff26606a4766f2d48e862e08d767bbd3830a83912e8fe734bfad9d6bca"},
    {10, 10, 6, "17cdcf5454085348780a68f2aee874cca73f52fe7fd937ecd80d351f92002a6f"},
    {10, 10, 7, "903e9469635304661895d394e9fce8ca222b9587a77d83ff455ae0783f3cc1bc"},
    {10, 10, 8, "d57349e9355344dfaa5f46f10799743fb6e131fdb7b8511dc58bcea41ee28459"},
    {10, 10, 9, "7cb8bdff0dbb609ec919abd12f8833a5714268610a110c75fe6a70026da74ffa"},
    {10, 10, 10, "041809c7a59208b7b70c2e0cd128aa3ecd6f86a7b5f55a06123bd5844e386a06"},
    {10, 10, 11, "0c6dec37bac63ce8446c31abe80ab3c0a6f677f8b5772c92ec990e150ceb7a1c"},
    {10, 10, 12, "83ae7caef1819807def7ae55900e1ebba8a6b1229cbcda4d6c2483e4102cd617"},
    {10, 10, 13, "99aa5b8159532898715ebf361eb701c5ff1a433009fcdd48190a310865816f9b"},
    {10, 10, 14, "46b44d757f815104f8db06d8f88770e6b2fb209feb33046129f8e2a8fdfec1bc"},
    {10, 10, 15, "58ccd9b28a2d4fea81ee95512f5505ae624b061d6ad430f256db6972e6e58f3a"},
    {12, 12, 0, "f5cb51f30886985b3ec43dac1d06489418525e28334b1bf1afb4bc6f9fbe9265"},
    {12, 12, 1, "5b2c8efea163cd203bba0dae0fcfe0f15c0a22eff1e9fdc1f54844c2ffb32079"},
    {12, 12, 2, "f1cc63a5820473fb2e7acf772a22a8f236d04dc344b43239e914da4de716b4f4"},
    {12, 12, 3, "80754cb6a87b97e97b8e91c5bd1e1b5fe07db8c96ee937635c338fe61a5e0d69"},
    {12, 12, 4, "e6a595c40f490d6b156bfc868e4d4859e9f8a696c52cdde64ebdd663733e652c"},
    {12, 12, 5, "df6a795863466bb96bf3b94d2fc3683a4407fa1bc226de1f13b29a8d9311429c"},
    {12, 12, 6, "e5c056288cb210a1929c170d6cb4ff7fcebc70e9e42e4e613175874caf6c75b5"},
    {12, 12, 7, "9a4dd2db4caf2f7edb9e02d4f38f4590936106b7a409871be496f2670b03edd7"},
    {12, 12, 8, "a8f08b32957c9bdbcb0a2d09afc70dd7be1d9284bc8276e99725d12befb82f9f"},
    {12, 12, 9, "098a4eecac452d662289eb1a9d9cdacb1b25b6eea074e0c6ca256118e60ddd20"},
    {12, 12, 10, "0ae9844fa8d0fdf9f23de8ea52a5c38857dae3a506c248eb8440f9a38db079d1"},
    {12, 12, 11, "34494b5c79f84d6c5688c1fa5e45cf32298407b0e19c7ed97111c8093084942a"},
    {12, 12, 12, "2fb9b4c736d46f3c39300c66c0bcd6dfa79e58dcac9b845de6978798a00df449"},
    {12, 12, 13, "1a55b892b32b2c5b621fb73f4c4b10815ac266c0094e50c2cf8a15be41a21ca5"},
    {12, 12, 14, "89890cdd728eb7da52d49cb0655d96a9bb110303533a2fd2a68b0863e0c3cb3d"},
    {12, 12, 15, "51e5fb7203f3f26d8f3ec43e6eab79ab1efa0bcecc4fceb412c9caa6bdedaf9d"},
    /* Production grouped d=W/2. */
    {4, 2, 0, "5702a6e9a7ddf35ca0cfac794f8f384a6bf1289944ce15705c8d32ddb51d4e38"},
    {4, 2, 1, "c78e2f6916d0e45afbf68b01c337e1c750286e3b9832b4d9baefc108a5975cf5"},
    {4, 2, 2, "f3d779c4c5bfba3f9f0f43cee5c1e668c4d39ec82b75a27a32d2d8cb94eeabc0"},
    {4, 2, 3, "fa0697bc5cae64759c81002ae198a35844873c655c815ecbba454c72a49a0830"},
    {4, 2, 4, "e64803e65c04204323120af31e06413e04729127a118af39d60933a4d5cb41be"},
    {4, 2, 5, "7e0e7c347832643e0bd5d33dbfac53a8864556d44d122f345046a60fefb63f67"},
    {4, 2, 6, "f0e8832c91c2de9068f163d221480f1f6fe0a201867cce3a095b44dd3e21b183"},
    {4, 2, 7, "f6f0a337d6aeca1a85d5544ae89e32b74c5777250bf166b0e102684cb7dd4b7f"},
    {4, 2, 8, "4b7826959fc91f7d68ec78c27e9d6a0b2ff56b8c861adfca9019ff21127dbb3d"},
    {4, 2, 9, "5e7db16ea513e4700bd10c5e397063080ebf681f03daecf15a1864cd278241eb"},
    {4, 2, 10, "80b1e09bb913a430aca5066ba74fd43164f75e7918b62313dee8d65db8ba9d31"},
    {4, 2, 11, "147ba45974201b4cde38c6beff7ee0d0f599588d95e9828dc1929aac056b5cd1"},
    {4, 2, 12, "c54eed7a661c079cf9808e7acbb6bbe79449a39550a159269fbe031497ec1f52"},
    {4, 2, 13, "b9d84381cb9b66513addefde3919ab9e8ccdc231c3549fe960abe0509f0d82f4"},
    {4, 2, 14, "d72589ed73069b87a6ce9dafde7a5b8aac10383f2ce2a04c0ee85573c8caf98d"},
    {4, 2, 15, "fe5eaaa34ad5d750bd63c0210dfd4d3cb82a9baeb4e0f66ea35a396b0d78d048"},
    {6, 3, 0, "5e5a252102774a6ddca3b30fb23e91e8eddeebaf3ad99915812ef12c01bfc68b"},
    {6, 3, 1, "564f78b58d81c2747a0d347a4bb14a6408e24bc276c117faaf0c463c75dbc5d5"},
    {6, 3, 2, "65d20a80272b60fb826ee9e9b64c4c5729c93712d668411726537c2e3ab30a92"},
    {6, 3, 3, "2e5745ea582ba3ff88814109b9bb27f7791105e5758c8461387433e10038f8fd"},
    {6, 3, 4, "c9126571931183d47f370a71a3d9b8f2015cfc777017b566d026f257df02d86e"},
    {6, 3, 5, "f4beaaa75034b0bef5b052470d8adc89b927381f0ac170141a72637701d4d61d"},
    {6, 3, 6, "0312ccad9575f831c46ea29be3bf3b7cefa8db1891734e599871b5e3eab5ca55"},
    {6, 3, 7, "d16e10b27e5e3897e25d4f48335d66a189a32e2f72ddc49186141cfd4f85d46c"},
    {6, 3, 8, "6eadd0b780e70a5088f099efdc1b134ed9c1507183be6da01d238d09e7fd81f4"},
    {6, 3, 9, "223eb15338f0f255eb753de9b51fbc571e4623343511efbe622b5deb44577cc0"},
    {6, 3, 10, "1292aeaf3ed74a0803ee3166aad8949b8e3dc1d143b096efa048aaff5312cbf1"},
    {6, 3, 11, "f42fbe79cd099fa29269d37d36b35b0be7af21df8242b8f682e858f34847aad4"},
    {6, 3, 12, "a3f1c05a4864071aa071f910b0ae8edfe7e3a8ae03a7a00c23f0b506ccb18016"},
    {6, 3, 13, "2b3707db3fd45e589282ecee79d7cada12e7db21cfd6f71706fb371f699239d3"},
    {6, 3, 14, "d572bea61610d8e20d28a54338ee5c4eec781f489d4909cd319a45afee8c2240"},
    {6, 3, 15, "558e006a1f7b35a000a423f2638660e269adb2a5702546df69976f1ac4de4601"},
    {8, 4, 0, "3a4164bed7bf78fc0529a7202ad6b65895d12b41fbe721d5c6b13ca809a5dba7"},
    {8, 4, 1, "7a259c62e9a94df2e3a17d6ae6d5817b6ae2a11cbe88f428d71123eacea329bf"},
    {8, 4, 2, "9c1f48fd6fe3b3eaac23a6f18453a6090044214596f81dc30a9ee56eb3c1db42"},
    {8, 4, 3, "d135009173a9d651d01f1f1c1c04f9ced20ed46bb40cf56e55fc44406f117b89"},
    {8, 4, 4, "c0eb39c03b078502c8ad833f82224a64e88b053f7972a7b17466ede722c3e2cc"},
    {8, 4, 5, "805aa3480017a159c0b04713e02c2c5eed7a7ebfdc82c82a48ac0bbaf874cfa0"},
    {8, 4, 6, "a9bb7984ca20a731621e42f020f37d8c6aa1e3ea4ca753833c3424a3bf700c9f"},
    {8, 4, 7, "b0878eab4aaf5b2451f09295e175aedb5fbdeed52d1ca7132cd2c538af428359"},
    {8, 4, 8, "5d87d453c10d9e33d78c65722fe0b1795dd1c80fef7cf13ce9c24b0a9637e3ca"},
    {8, 4, 9, "7a8f89558efab5dcce3033d67c7324fb3e62504537bdb7f7ae18f443eb5c1603"},
    {8, 4, 10, "3c13a909519a333cf147f374e575fbdb92a12e3d56a1c657b844112190df7410"},
    {8, 4, 11, "b25396b959c14e7231aac08e1f63c1ab21a269edfc369c2eda028d8c5c72b679"},
    {8, 4, 12, "0766dcdfb7043734697f174350ce4bdf0efde952a3038dfda4fd2dc06be4e816"},
    {8, 4, 13, "332d895777a340444ac1f6da50c62e165d196278b073894712658e4b6d34b1b5"},
    {8, 4, 14, "baf42fa50831e023229275b484274d3a86d6e69d237fdb14feaf2536f070dd87"},
    {8, 4, 15, "65990d84af998484bdc58200afe945ae8015d6a27024f0286a20ea417d6eacd6"},
    {10, 5, 0, "a857fdf1297c474d7a36b16ddf5c682ae5a5d11b8a2b8743c6b0d50b3d90169a"},
    {10, 5, 1, "96b46ccaee2a70b16621fa0804cf86c7375d568848e6c812b406208848b6b7c3"},
    {10, 5, 2, "10d2bbebbf624faf90013e16979caf3ce9284fb622e72e37e5607f8d5e6eef3f"},
    {10, 5, 3, "851ef6a9cd45575a58f92b183a9b7eba51045d32b450c2bd9611104ffbcf7e0b"},
    {10, 5, 4, "d49cd3e63e37d4cfc24832f52958923c3e09e89be923567ba837dc19e386a73a"},
    {10, 5, 5, "316b5f9ae2c05db2a2000c9cc83e00adee42371c18d74b52b58e86d363c2f269"},
    {10, 5, 6, "fe68fd68412d86c44db8c931fe31b30018dae8d70ab81c58dd99b26a2cb79fd1"},
    {10, 5, 7, "e433b824add2add5ff77a2f10a96515667d4279adfffe3795b6e574e3112a5e5"},
    {10, 5, 8, "c1cc06a6ace2c4dd41bdc6c216a336cb7598129b3c8de1b3a6d7e2a5349f40c3"},
    {10, 5, 9, "b52c66bc59677520f02e1f955f9c991eba5525318e74ba71ac4861f83b107a4b"},
    {10, 5, 10, "824f7ba71cbb87e0fb48d7237d7eab28beda451befa798b820c05f9eb3cde987"},
    {10, 5, 11, "0739db25f6f81c3094eca6e5e6dc3305f5b9759b5adacb66fbea48f77544abf1"},
    {10, 5, 12, "c5effac49f210c9338b500fa9ea06e8d26c95e3066bb25261496da0d18283e7a"},
    {10, 5, 13, "ee67c382f28e6f3baf2161095757d850ec0a3ee99afa0ea14d0b2b44722ae11d"},
    {10, 5, 14, "acf7cce9bab5067ac995d0c34e4886f62f71e43113934736875df3bd0077d5c1"},
    {10, 5, 15, "1fa4716f52e8e709745670071d4aff8df665cffe6f443adf7c5327cbc74d504a"},
    {12, 6, 0, "6ad9f4c9fc66d08ca1631cd32462ce22eeeda6b88a0118f0bce44e0c9258853b"},
    {12, 6, 1, "5ea16bf9fe1d425c685c732e50030d1e7b88c71fe8014d5e44ab5f3e0c19e954"},
    {12, 6, 2, "a0958933ca9b35cf2975cbda62a39196ff96140e2d00df25924f7a3f99d549aa"},
    {12, 6, 3, "065af4438d2452831cdac7da1f0f301ffa3b3e966d74a2167dd038efe79bf418"},
    {12, 6, 4, "b10653ffce2219ed626976e6f0b3547cbd87c5277ad3e4089cec9f9d8d3fa83f"},
    {12, 6, 5, "9f62fa979a54eb14c487f49716d5e40cfc04589839fc72dbc49dac9bd66ffd50"},
    {12, 6, 6, "55c914e9482552121ce7ae2433cf5c46a1b52f70a95c1447324f6075de03365e"},
    {12, 6, 7, "7d906b24742eb68ffd53de620b222238b12ddbb3936c661dce797bffcae8b519"},
    {12, 6, 8, "002b036f7f83a4ee8086ba1670f52adca950c512943a18d788c5a82c8c813065"},
    {12, 6, 9, "931f4f57331b876d378df67339c85a6009d5bc0ac7aadc6047d2fa561647f883"},
    {12, 6, 10, "1c73c40c8ca1e70d47b1123f8de539e65143f05cd722de8a5dafcc62d28588f8"},
    {12, 6, 11, "5f4285d64aa6e475756b8f78781f377c746e6d70489f13116968c1034d2adc3e"},
    {12, 6, 12, "6f5a878662391df646daa07dc9f75eb111821490c43b1fbcf835fd7eb066212e"},
    {12, 6, 13, "8978828d86d8abb58b05e887e2ae47f98266921602519eb13c3433c76ecc9cf1"},
    {12, 6, 14, "f040ad56883af777a75e9aeefd70e7a1d9ba299c5d28b88af3334bbf12dfad51"},
    {12, 6, 15, "b43fdf4eb6d33badb7823780f6b461a73f77492ffec81e7585a96099c58c4a3c"},
    /* Single-group/lab. */
    {16, 8, 0, "f2498848c8eae9bde721e815b55ae4da38c457ccfd117f3760e541dc4b0e31f7"},
    {16, 8, 1, "b32f9de882b6593fd1b63e4af1411cdb481a30906f7e5b1a1c69a299ec098db3"},
    {16, 8, 2, "82b457dd8d092804a1ccad4019cda9b6973cb980b7674d3b143960a3295dff22"},
    {16, 8, 3, "7ecc04f3c4e90f7e8dec814b4bc1a1b942a461f5f053eef83cf11ab938d93387"},
    {16, 8, 4, "60517f41f4e4354832a756421646992a932ef6c58244cd22cc866fd86f74bbce"},
    {16, 8, 5, "479556ddff2c9f60899270fae77a80131a5854b9c32387ed811a0d6f01ba3da2"},
    {16, 8, 6, "0cee648a4ecab9e352f3ccd6cbeb69f39720518b2abd615c77e015305465c375"},
    {16, 8, 7, "a6de743f5030d9ed7f83ed92422e832bbeaa0e88f50374d6b1dbb8d53527d264"},
    {16, 8, 8, "75927aba425143d0a8af4e0237e0d5ffeed0fb6a09e558ba63283e762a5ba5b0"},
    {16, 8, 9, "a9a6dc944fc339aecb8d8cd32adbbbdd2eabcb27a1a86ae61ae9e823b372390f"},
    {16, 8, 10, "51edcb1e13ebeb5b2048e0a6ac4fa4c998329774abe7918703155de64dd5ac14"},
    {16, 8, 11, "e47ec1f2a7716ba5c81c592e7498971e6ded755d570d9057fa0e2945352d7683"},
    {16, 8, 12, "88218e341db027e8d5abfee7b7b1c5b56ffe862dd6f827abd9251283c74452bb"},
    {16, 8, 13, "c259beb2b78eb77c2f4ad68f1987d65b955cbb6301a3fd9e9bf7990f5427bc25"},
    {16, 8, 14, "ff0eb45e3ca22cc348e25c363c849e5ebd250222b66062d6437d7e9dd126665e"},
    {16, 8, 15, "36357b4eb14b7604f485ced5e5c521d9e4897325f17c716730dc31503867fce1"},
    {16, 16, 0, "b6e62783371e74166a84d57b575b71802dab4fab9076882c0ee5e51aee29b7fd"},
    {16, 16, 1, "002a76e26b35dc5245dddbc1b499f079241eac186112efe2743185ca03004d23"},
    {16, 16, 2, "9ead8d445ac8e9f7e78bc1e14e446e6a65285a508f0cb414b23209bb9b113847"},
    {16, 16, 3, "6c0b2d428b15941ce00d05d79ce4d1d5b0b322ac379ffb45cc92a2fb7587633b"},
    {16, 16, 4, "ffa6930623c2c296c9bc0b37ebdeee9b747e8804ae426dcc381b29feac60d705"},
    {16, 16, 5, "b394e71c8b79382fa1bdfd99c555034f976201d22156a6d46a94dc0feaa19b69"},
    {16, 16, 6, "54d44fd5ac07a282d220d4ff542a169c2489a9a8a34e38a0ceb72ed67e84ebc0"},
    {16, 16, 7, "777780ac062cc32248a7a1f0506268d5e00413bf94911b9c1e75304071fb1b26"},
    {16, 16, 8, "bb14a9bf03be0c4e49ce838cc8d6053bcec5d89287d6ac43fbcf69d18f414e1f"},
    {16, 16, 9, "c73b65d47c5bd79bd3c7abd6a2f0de7802bedcb192766dabf13b6896048cd454"},
    {16, 16, 10, "6aa1c0749206685c82b133e9f48d6d385a365f1deace73ffa73de4e23bd6be1d"},
    {16, 16, 11, "34ac343a66cc89c5d17dbc9183965854ea359410a393196a06574f9f20ac54d8"},
    {16, 16, 12, "470472027e0025ec39954f031fa6f53af86f6cc3e7b356810126be0901c1fb2a"},
    {16, 16, 13, "e1f71c99a13517a68fe4cd1206d3dbf1b0f68ac3d898ca6e4b05a1cb05b44f42"},
    {16, 16, 14, "4c00e63fe607a30926ca3fa717340abe5d7edab70ef8c1907dbefd8459fb4f9f"},
    {16, 16, 15, "7746658b3f3513383c7ff8f48c2a00f1186ba6cd5701640bc0f71d43d4563697"},
    {32, 8, 0, "3a724191afd69d042e9fc32417797dd7f0368065892617d1d7b91a4b7abaf488"},
    {32, 8, 1, "3f93d2bf497a8cd0d521b429ace2500cd61cb08409b8bf5ca1ab3c82ebfa2f9a"},
    {32, 8, 2, "aff81e77fee6f37f5a9b55917d917accbf983b1f9573efde25df42298ed440d8"},
    {32, 8, 3, "71ec992c713758707a739dc822ca9dfa1d4eda8cad49264a62a4e5d1839e53e6"},
    {32, 8, 4, "0f1c9203ddc7ab49d4e1003722ade7b92796220350cf642e4b339509cca6f4c3"},
    {32, 8, 5, "387c8ed8c2c443bb7dc968d61b8731ce617a582b14dc0429081f784ffce51f77"},
    {32, 8, 6, "94984c3cd9d3c10ddc6e23cb7246119a2ca6602d023f07260af13c17b87cfcfa"},
    {32, 8, 7, "07a54fb7adb19ac67a118b4c1840b03c854d87a60313c9995379c720ae8571e2"},
    {32, 8, 8, "862e155f2986583b8b1b30b5c30ff34d5bd8d84ef5df776ed8a6d0bbfaf911d0"},
    {32, 8, 9, "1d82a56a474e1f2b0b6388623ebbe1755f0b7280752a40834596aa4b98456a28"},
    {32, 8, 10, "a10bb9f1b90c80c8711a05dbe60dae1587143a9a355b0d3232262169ab3ceca2"},
    {32, 8, 11, "d8b7bcd9361a4ff32083758cb9f328324535f5e4fac54dec0946a975b15b12c9"},
    {32, 8, 12, "38e4487c1db1a5d3e8c1af645dad5f78ec538ab780580bff29ee067e3573b77e"},
    {32, 8, 13, "51fc02b7165546cd985776e4d237dec9864450406a66a8e4d4ad2e69ba5b1fd4"},
    {32, 8, 14, "5a7a7a9cb55a35021ee900b888324d42886ce209271bc4e364d401765700afb9"},
    {32, 8, 15, "23794152db6ef614f7604ed6c5e09478b067732ffb5993a1d9da023ad18bff03"},
    {32, 16, 0, "8d49988e0fd8aadd5bed900688c7554a914311b0d2f652ef87b53eeabcacedd7"},
    {32, 16, 1, "f9936b932269452ce21ab1e541bb1e2034607a408d4c25c4fb836a8745539fe0"},
    {32, 16, 2, "26ce36f7b3c374715887e2c7ee894fabcae08c7334823b182691184716269e49"},
    {32, 16, 3, "1c52c6f75f4b8e3d24eb607f4728f01a4c1923b5fe3fa06eeace9848506dc095"},
    {32, 16, 4, "5b41d0861026dbd6c8fa81b4e7648ab9d2decee9c1a4d428a93317646ec3a915"},
    {32, 16, 5, "6cbc64652fe8e3ac38c85e037d9e859193bfec45ec3aa7bdeaeff20f11cf337b"},
    {32, 16, 6, "969aa8a6d73efcd3ad2bbdd3ef08aa8a7fcb6bcc850983180b4aba4df8ba909f"},
    {32, 16, 7, "403c3bd5aef45c822e70f81f4c99ed8a4cbcc749c4631166640219533db72ed8"},
    {32, 16, 8, "54966916ef503ea9814a8e56e9e5c6ed243806159718f6d4f8003d0ed7a5b471"},
    {32, 16, 9, "cb8367fee6e09faa34053f80c127c642ba792a17258c8af35211af146f4106b2"},
    {32, 16, 10, "a8b027e53b475832a7bf5d43cae104651ed82ee22611af2aa03ba5a7e1c28e58"},
    {32, 16, 11, "493b781fde1b6f4a9a55e8f3b1952dda479680d6b30a32a3444a79e19d6047c6"},
    {32, 16, 12, "90f62037d714d8bd980f0217cc4e8740b6919475f1e894532447d60f8ef26ddf"},
    {32, 16, 13, "b4ffccf428ab93d9d85af0729c5829a28288ec4fd1bc5579d64272b4119bf2cb"},
    {32, 16, 14, "a7ae251b95aeaff59d6871d49574144ce2290c05d1ff8386295203a2d753781f"},
    {32, 16, 15, "5f5276c41a31b9b136048dc817d6e8ef5f87c18dcaee1052c48dceb4c092d65a"},
    {64, 8, 0, "cdc2614580a69761e7760c1d6b75b32587d0b827f311116728f080dc80cc721e"},
    {64, 8, 1, "f34b9645dc5ccae5a0b036cda9dec0d9c58c57b952980d276a1080d6589b3cfc"},
    {64, 8, 2, "cdc9904e7eda6717791e6643538fde03cd06ef82175b97118fe4571cf94efedc"},
    {64, 8, 3, "89f606bcdb17f5b3d036e4100b26fc116ed98b7e2f4afc8e7a4991d9f58c22b2"},
    {64, 8, 4, "24ad9465efb95642939707eea10b8e1f7ea4465d3b3f7de5d4d3f27add4dd25c"},
    {64, 8, 5, "d5ca5972686ba6e1de5a4e0f221850c84121f38b5ed673be7a1a210ecc9bde7a"},
    {64, 8, 6, "76bbabdc54e71bb2dfd6544b1c58727eac26b47e684e0221b47a4da7a501df60"},
    {64, 8, 7, "c5a940b2bb74012f07a231d8a21e8a4cac3aef5db40e35543b683f20fef2d8fe"},
    {64, 8, 8, "848bbcc851f9b56e9265af3d4dbb0512f5cbdce40ba10ecc17d1effd29a222c3"},
    {64, 8, 9, "aa8abdd0748953e29f0e757117238a354e4fc75a24b6aecae82dd48c2dafc0c9"},
    {64, 8, 10, "6fd303c311bfb2a4248a7ccf1d3177d6a89e1105c6596d2066ff2fd040a65782"},
    {64, 8, 11, "f13e7af0befed47fe43aeaa9eb1dc26ce760a34775db32a80d2ee9952fb65c53"},
    {64, 8, 12, "5643f39fdd21b288272ff302c4efc19726bf7ad2393e917d3d018bcb0e03bc2c"},
    {64, 8, 13, "6c8767ece051f2b663a52903f9b55556078732fb25a2d884f6b7121859066d65"},
    {64, 8, 14, "2a46fa5efb3e4ecc76da3db2c8dd73b87267bf2bb55849d82260c2d6baac1352"},
    {64, 8, 15, "16f2a402d7e1b3fa14a3b3a63c4ae56522572734b80d50aa0048042b16ba584f"},
    {64, 16, 0, "d30d331c5ce35fcdd6fe812829cc5df29d57ce5c0acfd25ea9d3f049a4bbec8b"},
    {64, 16, 1, "a78446d1045f813350bdf16898b6e4dc57bf216bc89a7a86157e9dc8d8643846"},
    {64, 16, 2, "a8a23c6da2e93a729e48f7e83e181f0790f153271e8ef19285344a0b25ed1a77"},
    {64, 16, 3, "88a79eb5b7d94b1daad63e8bfddc4c36b7972dfb3a12bd6a548f4c5219489ac7"},
    {64, 16, 4, "2a5e2d0da4ca63deddac41afd38ac6c0f28771cac3b8e370e1e0fa01d1301058"},
    {64, 16, 5, "d381b1933e50ce047661a508c5e291a951150fb3cc728ecc02edc35f09ad3660"},
    {64, 16, 6, "1e1166fdeb71e40f20cd2c9402eedecb173b3181632eb3cda00ae7e8b4123788"},
    {64, 16, 7, "6c9b79d312c37a5b9a1e9b9436a9eb9ef09b87a8a32986dd8f49aa047bd17aea"},
    {64, 16, 8, "50c58a4ed6e696f03c32156d9e78bc577c0076bdd6d441c9d9c345d775658a2e"},
    {64, 16, 9, "9f45a752d4b436fa89a7734aac5104ad7d599c7a733e34dde546f43d396f6790"},
    {64, 16, 10, "c7ed528ec482cb778057542d1a964c1db50df3ed004569ef41a878fa218f9dd5"},
    {64, 16, 11, "d24008b7462a2c5b3ff5a44482cf685974ca018ec79c5e9b52501edf6162a969"},
    {64, 16, 12, "6a19a92ce388a3a8edcb6236973ac128e52560d07738026e4c9389582da1f309"},
    {64, 16, 13, "a7eb0c7d3654df156af5416f04711d939e394e5678d1add39226d15a496ca0ea"},
    {64, 16, 14, "4e9a758ad0cd4a283811916d093319231157c95efbb8f1347697f0e844416ca3"},
    {64, 16, 15, "40b7124c44df81352529b192992847274ee2bc96eb2a0e6650bfc0e2a74e636c"},
    {256, 8, 0, "df8f0ae71fe892b1a1d5e9634cecb24d524f14cae0e622dbe9b68a8200880e0a"},
    {256, 8, 1, "603c945e160212a50616616c3bfdcceae70a9141062b616b94f7e3965f74e15a"},
    {256, 8, 2, "2fc7c51ee75eaf75e833fb1c0fc3fa7302e7f5e10a0f6c355a0c0dd468ca823e"},
    {256, 8, 3, "722c0d9c08bd2eab7c94234ae9d796fbd08ed20b1934059191a294690d0a24e9"},
    {256, 8, 4, "85282acab5046ffe7c6eb3cdb2a1eb8cef3991a92573f5f001adff3c1e10ade5"},
    {256, 8, 5, "2b6351333f9f441f1c08a762af32b93caed2ea5b40179530eb9c62bf774c3260"},
    {256, 8, 6, "ba3c013285a60532d3c812f0b1c7b76d09dfa3411d9c2bff294c2723f2028cc1"},
    {256, 8, 7, "27c99375e1d0f230862deb7fa185c07ab465595f47a545f52503592b5a427557"},
    {256, 8, 8, "0b43bc5f58e11a1b94ccdf3126b761eae18775dfc7ff1f4bc62b363effa6387b"},
    {256, 8, 9, "51972fcfcd918f29881a6089cadc678096cff6b2569e84fd5de69e8e19329293"},
    {256, 8, 10, "577550c5cdd1776f66e4a3967f2a28dc236e626b29a71321fd8ce6655d170c98"},
    {256, 8, 11, "f6649153d3a58bc6655d3b7aa5cadaddff11882bccae0d6acc1f042e4bc7b84d"},
    {256, 8, 12, "721273c080f8cf30a97895cba5d52374385e09d9d2a8d89729e63f77b6912a29"},
    {256, 8, 13, "8801fb97856226cc079fa25aff794e65087355ec57ec5ad14fd7dbfb8591199e"},
    {256, 8, 14, "7e01328f40ce09e135728b05d2e670d6eec7edfd59ce826aecb6a4ccb7e7f6e0"},
    {256, 8, 15, "d208cc7e2c2a9365576fe4349a6c882f337de7edccf0e72b2061c3291920867a"},
    {256, 16, 0, "e8f8770b62e95528a02892d4d5980626dfbd5cb4da6753e1970453a718094357"},
    {256, 16, 1, "933d1434903631a37c42a109ed7ccc3707d1765472e98d709fdbcb4461eee6c3"},
    {256, 16, 2, "4bb4f2f55530b9b24c56f5c4a4dfda6cf93c04c6df24f53f9071d0bb6cee81db"},
    {256, 16, 3, "d92226cbec67657fccb6bf0906dc8006ba1cd70dea09e173d2e3a6e50a58c59f"},
    {256, 16, 4, "d3c04726431007b9a3b5f44306c4635684a57c883af6ebac6bd2291055ff1a88"},
    {256, 16, 5, "9bb4770461bf08fb89d3764b4016ebd57360d3b6f608646c8c4ee8c069ea1b27"},
    {256, 16, 6, "04ee176e58eb4b05c620fe4d33cdd4828d517c85529d1c11c07506fee601bdf9"},
    {256, 16, 7, "bf10b784cf295c4f6022cfc65e5d74fe47345c97778074adbf7fdedd986dec85"},
    {256, 16, 8, "30b027066434da6ea947c3bee1c0d11f76e594864b8dae9fb9d1d3b17579b34f"},
    {256, 16, 9, "cdf748ed9cc5b1c1c4fb5460edfa4b2967b2d766147ea6d5f3b983030e2aead9"},
    {256, 16, 10, "69190beaa43310341ecd17af4037219957f3abb3e3dc2eec91b7735c2e2af7db"},
    {256, 16, 11, "99f1667cf2861569e9f0b2b7a0591d2d8e3abad07ae5fa51a1f382f04ace3584"},
    {256, 16, 12, "70cc96281a1af7adf966196074c4bac4017d620d053c0bba0147fa053a45403b"},
    {256, 16, 13, "d4dc0268a80bd434663faf9da084621a5f9ff97a4f33f93e4a029a5e685bbc0c"},
    {256, 16, 14, "56c97fdd6bc9aef8828ce6009fd25de0e90771d30532b7bca6c5e51f773e9d25"},
    {256, 16, 15, "6d05363e37d7a8f7f60b32b0f006824ee8417d1f907f58c5cde9d84a0caf1b4d"},
    {448, 8, 0, "d39af77b85e3819126428ab7c31b799f71a179247f6733ab36df73ab7a403cd4"},
    {448, 8, 1, "5ce87abbb1dbf10ed2cbee1bfb5934beac1494777ef037914d3946b761bfdb8f"},
    {448, 8, 2, "a8af5298bce5d63a459d9022cc66e331a912bb3ad6079a5a8382d00b80e0c163"},
    {448, 8, 3, "5465955710f7d05da0272571fffefc0faab3d0eb3b311e1da423836409a5bc75"},
    {448, 8, 4, "d974664ec16cc0da2a2215e184474ae590c1974af2a8510975aca57981b243da"},
    {448, 8, 5, "9e80bb36f728226d6f534d6ee7d6a7a345438b2c7084913913067b14ba25620e"},
    {448, 8, 6, "c9f6ef18596905940277786d35207a091ad559d290c7853be94dbd2ff8be2c1e"},
    {448, 8, 7, "03c9d00050759bbdb5864cc196634981fc6247657ca3fa5a3638392b44691ef2"},
    {448, 8, 8, "496bb0d864709c347179cb91dfdeceffd38c21c2c7ad4946f8cc53136a98e805"},
    {448, 8, 9, "e8de8fc844d1ce23fd68b4361f49e231e2ee68aed80187fb736c945ed8aaef45"},
    {448, 8, 10, "f9f59311bd1d71dcc77a5c12c8ca2c67c61490a7c0c259cb8e863e16e1607cd5"},
    {448, 8, 11, "d52d163fbfdbc541258d14a24d1da6bf97b2d0e0e7f91b13529b8ca995335962"},
    {448, 8, 12, "f88bab1057becb065197cbdcd15087e2c2e91e49556e77b5ddfa77b537636e31"},
    {448, 8, 13, "cc5e1bc53fdb8f363168db7ea928dc9e3e3aa22dc17779c2d0f3c7d55ac1d8f3"},
    {448, 8, 14, "ee0ba525c894603ebbf53452977023d8bc741d0adf9806c0367a5c14a11be74b"},
    {448, 8, 15, "7d8e7c0a3d8ebdad58e6a8ffe340431db85ed6236c96bd8625fc93a2d19e6641"},
    {448, 16, 0, "a06d32af67114f06aad2d95d3975e5c301a6fc7aed8dda9a8fe92ba8a51694d7"},
    {448, 16, 1, "ac1da8e101e4297e27c747c485bae47ea311fe2949aa6e7e9e8015bcabd8f421"},
    {448, 16, 2, "a0988ac90ea18e596c4b442ac858686fde0595afdbcb63d521a102b8f9e3544b"},
    {448, 16, 3, "58a45139bdf313b90e57d37572538b9df23e824a69e3dc01ea46a7a61b70beb8"},
    {448, 16, 4, "c9189023bfa9a03da5bf72555384ac8a6942c843659228ab64c57696d955b61f"},
    {448, 16, 5, "0790173f12340afd8a04227adc840960c04f74c2c62f34db1a0440f07fcf86bb"},
    {448, 16, 6, "2503439f4883ad12a3dff5e6cdaa06b22965fc916cede46fda04558da94db3ca"},
    {448, 16, 7, "5d524e13114559653fc0088df6dd8a94d71492cccab92a7e94d87fbf7aa8a484"},
    {448, 16, 8, "2176a6a424ba2762483d38c9831014705410de273926c334814fd38119f8e44e"},
    {448, 16, 9, "f5cc95ca984ed1be22ce9687cc8be0aef373b8bf1b459b204cc7a10002bc3ac5"},
    {448, 16, 10, "42132c171ca850c7d14b34ef2e8a470a25eb77069a599a8075e85380ec633226"},
    {448, 16, 11, "fdc9f2a8e567e0107bc725c5a4903dc9220d4989e9d5364a279fd705aaefadcf"},
    {448, 16, 12, "07d85ac462b208c40427e457106040c53568dd5b6bd4eeb3942a3033fcdd958d"},
    {448, 16, 13, "e08a1d30ae90ee18251b88333839254c45333826f89bf2fe953bdd61de6c7eb2"},
    {448, 16, 14, "69e1e6f29f6f81427375ab7314800071d47fca2f1abf98a8be0b0e1317de1481"},
    {448, 16, 15, "be7734d74e65fe05417c37cc0e5a4d04d0ace1583511d26a2e5d73822e5b9d1d"},
    {480, 8, 0, "8d4c4e48a96acf00567247833664cbe8922edddb916bf643b090fd6ecd4d2ba9"},
    {480, 8, 1, "3154324102ee8d5f654e99d0d6f5fbf14e6d69e9237acf122813fc456ae6371e"},
    {480, 8, 2, "f10413d585dc29426281c6872c08a04d88897b4da0ee78fa9e9cb1efc9067bcc"},
    {480, 8, 3, "bf65b80ac443c691eed0d4211ecf3ed3e635d5cfb54c306643b4b434f1bcc79c"},
    {480, 8, 4, "262ceafc10c9de8b319041c46c25194b0961d5ab64192f8d72d63c778938ba39"},
    {480, 8, 5, "fb1e83682f31556231fa71b6f204ba5a7f58e02e3c95d0de46e478badc6117cb"},
    {480, 8, 6, "4a6486f0c516d9ff3c0a25f0df955a1b0df47c8d8904402c9e1b266b5f4291f5"},
    {480, 8, 7, "3124dde693370326af91c8d190746513b2fc12b4de1ff2267bf5c0d07838d161"},
    {480, 8, 8, "479738b849f2f1efd41d60bf817402421a9530ee5c00d92e826c9eb4f87b708a"},
    {480, 8, 9, "71a7616b5cc845851efaa076dc16b8a80599ff78a52c0ca25f6c4edce551fb48"},
    {480, 8, 10, "a2e55cb769f8745437d67f56f0eabc0366adc78644721a8036d545bcbfc87650"},
    {480, 8, 11, "3d7a324c9ad821e478443998885cd12af374d7265536e00fc86916842f037732"},
    {480, 8, 12, "43b954c275232212b54ecbecfadf4bc511970b452fd7a7a803728e25b559b34d"},
    {480, 8, 13, "0ff071acd06d60d4b324c4c3bb46f9b53d93b7cea4cdc77b50eaa9483ca69e74"},
    {480, 8, 14, "c2b946dd251adbfa0681b6b072c71e989b8babdef7315b8dd48c83aed120b21b"},
    {480, 8, 15, "baed07bfbbb69bfa602d710e6857263b5d3bb83451b660dfe0bb362294930e4a"},
    {480, 16, 0, "849fcbf6ad82c2965f477e2c5c636898da62530bcdd83c9b063005c8e717a426"},
    {480, 16, 1, "233a351827ccae22a8a2d61a6c46aaa7f28030c514eab2ffbc33e7b030d9458a"},
    {480, 16, 2, "c75ae859c4bddee87116f6b92f949b2bf1be2c6443362a537f20c04f0216d5a3"},
    {480, 16, 3, "bf7b0b005f7f7818d8f72ddfb9c71b3db876dc4a2a823e0d934e0eabc0aa3cc3"},
    {480, 16, 4, "3a1827f184fc00be54d14533a72eba4fe85adda397a719cb3a521bc002da85e0"},
    {480, 16, 5, "750950ddc1e8b2cd0348c4a8da46390fccfbe4132faa6e6c91604c085109f998"},
    {480, 16, 6, "92f573b96b6b26e59a483dc9ec34011b25a84e538a6951e1d8de901e364cc4b7"},
    {480, 16, 7, "01f8eb7817d4e9912e79252d3a06a8c2d4f5b6939c59f959f314798301a7ede4"},
    {480, 16, 8, "323091e5940743577b3b66c7c9f976486542946c9c81ac03549fb8d9ed0d1e41"},
    {480, 16, 9, "a3b192b0eab2c2ced898b3d18525f3c02fc3c2a2824916a2f41ef4a3752587df"},
    {480, 16, 10, "af164b9c4ffed2bfd9a7a0967463c65d5db025614645c78f1815ca4df505d747"},
    {480, 16, 11, "4d284e3e5a70944c9f51c21dfd732c16b1ae0c6684e7d526800103d715e689aa"},
    {480, 16, 12, "371a46157ee5cbe3b5e6c0a144487de388edcb6147d2d3b0f0ac9a65b816c005"},
    {480, 16, 13, "9cf5c621d9ff52d06d801295f5dab48213a141d289f8f3209cc1f7b0a537a849"},
    {480, 16, 14, "edb9998f96109396ca8151b954912aa55eba6180adf0273a306bd59e33b50690"},
    {480, 16, 15, "2fb56c2c546f6167cd64069989780965b79100e025c26a951b8f62f6f0dd512a"},
};

static void vector_digest(unsigned w, unsigned d, uint64_t seed, uint8_t out[32])
{
    unsigned k = w / 2;
    unsigned L = w;
    const size_t S = 64;
    unsigned G = L / k + (L % k != 0);
    unsigned P = G; /* m = 1 */
    unsigned drop;
    unsigned *coeff;
    struct rlnc_ctx *ctx;
    uint8_t *dbuf;
    uint8_t *pbuf;
    uint8_t *orig;
    uint8_t *dp;
    uint8_t *pp;
    uint8_t **data;
    uint8_t **parity;
    struct sha256 s;
    unsigned i;
    size_t b;
    int rc;

    ctx = rlnc_new(k, 1, k, w, S, d, seed);
    if (!ctx)
    {
        fprintf(stderr, "vector: rlnc_new(%u) failed\n", w);
        exit(2);
    }

    dbuf = xmalloc((size_t)L * S);
    pbuf = xmalloc((size_t)P * S);
    orig = xmalloc((size_t)L * S);
    coeff = xmalloc((size_t)w * sizeof(*coeff));
    data = xmalloc((size_t)L * sizeof(*data));
    parity = xmalloc((size_t)P * sizeof(*parity));
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

    for (i = 0; i < L; i++)
    {
        for (b = 0; b < S; b++)
        {
            dbuf[(size_t)i * S + b] =
                (uint8_t)(i * 131u + (unsigned)b * 17u +
                          (unsigned)(seed & 0xffu));
        }
    }
    memcpy(orig, dbuf, (size_t)L * S);

    rc = rlnc_encode(ctx, data, L, parity);
    if (rc != (int)P)
    {
        fprintf(stderr, "vector: encode(%u) = %d\n", w, rc);
        exit(2);
    }

    {
        int ws = (int)k - (int)w;
        unsigned n = rlnc_coeffs(ctx, 0, 0, coeff, w);
        int found = 0;

        drop = 0;
        for (i = 0; i < n; i++)
        {
            int abs = ws + (int)coeff[i];

            if (abs >= 0 && abs < (int)L)
            {
                drop = (unsigned)abs;
                found = 1;
                break;
            }
        }
        if (!found)
        {
            int lws = (int)((G - 1) * k) + (int)k - (int)w;
            int abs;

            n = rlnc_coeffs(ctx, G - 1, 0, coeff, w);
            abs = lws + (int)coeff[0];
            if (abs < 0 || abs >= (int)L)
            {
                fprintf(stderr,
                        "vector: no drop candidate W=%u d=%u seed=%" PRIu64
                        "\n",
                        w, d, seed);
                exit(2);
            }
            drop = (unsigned)abs;
        }
    }

    memset(dp, 1, L);
    memset(pp, 1, P);
    dp[drop] = 0;
    memset(data[drop], 0xa5, S);

    rc = rlnc_decode(ctx, data, parity, dp, pp, L);
    if (rc != 1 || memcmp(data[drop], orig + (size_t)drop * S, S) != 0)
    {
        fprintf(stderr,
                "vector: W=%u d=%u seed=%" PRIu64 " drop=%u rc=%d mismatch\n",
                w, d, seed, drop, rc);
        exit(1);
    }

    sha256_init(&s);
    sha256_update(&s, pbuf, (size_t)P * S);
    sha256_update(&s, dbuf, (size_t)L * S);
    sha256_final(&s, out);

    free(dbuf);
    free(pbuf);
    free(orig);
    free(coeff);
    free(dp);
    free(pp);
    free(data);
    free(parity);
    rlnc_free(ctx);
}

static void test_vectors(void)
{
    size_t v;
    size_t rows = sizeof(vectors) / sizeof(vectors[0]);

    for (v = 0; v < rows; v++)
    {
        uint8_t got[32];
        uint8_t want[32];
        char got_hex[65];

        vector_digest(vectors[v].w, vectors[v].d, vectors[v].seed, got);
        hex_encode(got, 32, got_hex);
        if (hex_decode(vectors[v].digest, want, 32) != 0 ||
            memcmp(got, want, 32) != 0)
        {
            CHECK(0, "vector W=%u d=%u seed=%" PRIu64 " got %s want %s",
                  vectors[v].w, vectors[v].d, vectors[v].seed, got_hex,
                  vectors[v].digest);
        }
    }

    printf("frozen vectors: %u rows\n", (unsigned)rows);
}

static void emit_vectors(void)
{
    static const struct
    {
        unsigned w;
        unsigned d;
    } combos[] = {
        {4, 4},   {6, 6},   {8, 8},   {10, 10}, {12, 12},
        {4, 2},   {6, 3},   {8, 4},   {10, 5},  {12, 6},
        {16, 8},  {16, 16}, {32, 8},  {32, 16}, {64, 8},
        {64, 16}, {256, 8}, {256, 16}, {448, 8}, {448, 16},
        {480, 8}, {480, 16},
    };
    size_t c;
    unsigned seed;

    for (c = 0; c < sizeof(combos) / sizeof(combos[0]); c++)
    {
        for (seed = 0; seed < 16; seed++)
        {
            uint8_t d[32];
            char hex[65];

            vector_digest(combos[c].w, combos[c].d, seed, d);
            hex_encode(d, 32, hex);
            printf("    {%u, %u, %" PRIu64 ", \"%s\"},\n", combos[c].w,
                   combos[c].d, (uint64_t)seed, hex);
        }
    }
}

int main(int argc, char **argv)
{
    struct tcase *tc;

    if (argc == 2 && strcmp(argv[1], "--emit") == 0)
    {
        emit_vectors();
        return 0;
    }

    test_sha256();
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
    test_vectors();

    if (failures != 0)
    {
        fprintf(stderr, "FAIL: %u check(s) failed\n", failures);
        return 1;
    }

    printf("PASS: rlnc\n");
    return 0;
}
