#include "fec.h"
#include "gf256.h"

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

static unsigned popcount32(uint32_t x)
{
    unsigned c = 0;

    while (x != 0)
    {
        c += x & 1u;
        x >>= 1;
    }

    return c;
}

static void mask_zero(uint8_t mask[FEC_MASK_BYTES])
{
    memset(mask, 0, FEC_MASK_BYTES);
}

static void mask_set(uint8_t mask[FEC_MASK_BYTES], unsigned bit)
{
    mask[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static void mask_clear(uint8_t mask[FEC_MASK_BYTES], unsigned bit)
{
    mask[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}

static int mask_get(const uint8_t mask[FEC_MASK_BYTES], unsigned bit)
{
    return (mask[bit >> 3] >> (bit & 7)) & 1;
}

static void mask_from_bits(uint8_t mask[FEC_MASK_BYTES], uint32_t bits,
                           unsigned total)
{
    unsigned i;

    mask_zero(mask);
    for (i = 0; i < total; i++)
    {
        if ((bits >> i) & 1u)
        {
            mask_set(mask, i);
        }
    }
}

static void test_gf(void)
{
    unsigned a;
    unsigned b;

    gf256_init();

    CHECK(gf_mul(0, 0x53) == 0, "0 * x != 0");
    CHECK(gf_mul(1, 0x53) == 0x53, "1 * x != x");
    CHECK(gf_mul(2, 0x80) == 0x1d, "2 * 0x80 != 0x1d (polynomial 0x11d)");
    CHECK(gf256_exp[8] == 0x1d, "exp[8] != 0x1d");

    for (a = 1; a < 256; a++)
    {
        CHECK(gf_inv((uint8_t)a) != 0, "inverse of %u is 0", a);
        CHECK(gf_mul((uint8_t)a, gf_inv((uint8_t)a)) == 1,
              "a * inv(a) != 1 for a=%u", a);
    }

    for (a = 0; a < 256; a++)
    {
        for (b = 0; b < 256; b++)
        {
            CHECK(gf_mul((uint8_t)a, (uint8_t)b) ==
                      gf_mul((uint8_t)b, (uint8_t)a),
                  "multiply is not commutative for %u, %u", a, b);
        }
    }
}

/*
 * Encode random data. Then decode every subset of exactly n shares.
 * Compare all n + m shares byte for byte.
 */
static unsigned test_all_combinations(unsigned n, unsigned m)
{
    static const size_t lens[] = {1, 16, 1024, 65536};
    unsigned total = n + m;
    struct fec_ctx *ctx = fec_new(n, m);
    unsigned pass = 0;
    unsigned li;

    CHECK(ctx != NULL, "fec_new(%u, %u) failed", n, m);
    if (!ctx)
    {
        return 0;
    }

    for (li = 0; li < sizeof(lens) / sizeof(lens[0]); li++)
    {
        size_t len = lens[li];
        uint8_t *buf = xmalloc((size_t)total * len);
        uint8_t *orig = xmalloc((size_t)total * len);
        uint8_t *shares[FEC_MAX_SHARES];
        uint8_t *data[FEC_MAX_SHARES];
        uint8_t *parity[FEC_MAX_SHARES];
        unsigned combos = 0;
        unsigned ok = 0;
        uint32_t bits;
        unsigned i;

        for (i = 0; i < total; i++)
        {
            shares[i] = buf + (size_t)i * len;
        }
        for (i = 0; i < n; i++)
        {
            data[i] = shares[i];
        }
        for (i = 0; i < m; i++)
        {
            parity[i] = shares[n + i];
        }

        fill_random(buf, (size_t)n * len);
        CHECK(fec_encode(ctx, data, parity, len) == FEC_OK,
              "encode failed for (%u,%u) len=%zu", n, m, len);
        memcpy(orig, buf, (size_t)total * len);

        for (bits = 0; bits < (1u << total); bits++)
        {
            uint8_t mask[FEC_MASK_BYTES];
            int rc;

            if (popcount32(bits) != n)
            {
                continue;
            }

            combos++;
            mask_from_bits(mask, bits, total);

            /* Poison the missing shares. Decode must rebuild them. */
            for (i = 0; i < total; i++)
            {
                if (!mask_get(mask, i))
                {
                    memset(shares[i], 0xa5, len);
                }
            }

            rc = fec_decode(ctx, shares, mask, len);
            if (rc != FEC_OK)
            {
                CHECK(0, "decode failed for (%u,%u) len=%zu mask=0x%x rc=%d",
                      n, m, len, bits, rc);
                continue;
            }

            if (memcmp(buf, orig, (size_t)total * len) != 0)
            {
                CHECK(0, "data mismatch for (%u,%u) len=%zu mask=0x%x", n, m,
                      len, bits);
                continue;
            }

            ok++;
        }

        CHECK(ok == combos, "(%u,%u) len=%zu: %u/%u combinations", n, m, len,
              ok, combos);
        pass += ok;

        free(buf);
        free(orig);
    }

    printf("(%u,%u): %u/%u combinations ok\n", n, m, pass, pass);
    fec_free(ctx);
    return pass;
}

/*
 * Decode every pattern that loses 1..m shares. Also decode with all shares
 * present, and decode twice in a row.
 */
static void test_loss_patterns(unsigned n, unsigned m, size_t len)
{
    unsigned total = n + m;
    struct fec_ctx *ctx = fec_new(n, m);
    uint8_t *buf;
    uint8_t *orig;
    uint8_t *shares[FEC_MAX_SHARES];
    uint8_t *data[FEC_MAX_SHARES];
    uint8_t *parity[FEC_MAX_SHARES];
    unsigned i;
    unsigned k;
    unsigned cases = 0;

    CHECK(ctx != NULL, "fec_new(%u, %u) failed", n, m);
    if (!ctx)
    {
        return;
    }

    buf = xmalloc((size_t)total * len);
    orig = xmalloc((size_t)total * len);

    for (i = 0; i < total; i++)
    {
        shares[i] = buf + (size_t)i * len;
    }
    for (i = 0; i < n; i++)
    {
        data[i] = shares[i];
    }
    for (i = 0; i < m; i++)
    {
        parity[i] = shares[n + i];
    }

    fill_random(buf, (size_t)n * len);
    CHECK(fec_encode(ctx, data, parity, len) == FEC_OK,
          "encode failed for (%u,%u)", n, m);
    memcpy(orig, buf, (size_t)total * len);

    /* All shares present: decode is a no-op. */
    {
        uint8_t mask[FEC_MASK_BYTES];

        mask_zero(mask);
        for (i = 0; i < total; i++)
        {
            mask_set(mask, i);
        }
        CHECK(fec_decode(ctx, shares, mask, len) == FEC_OK,
              "decode with all shares failed for (%u,%u)", n, m);
        CHECK(memcmp(buf, orig, (size_t)total * len) == 0,
              "decode with all shares changed data for (%u,%u)", n, m);
    }

    for (k = 1; k <= m; k++)
    {
        uint32_t bits;

        for (bits = 0; bits < (1u << total); bits++)
        {
            uint8_t mask[FEC_MASK_BYTES];
            int rc;

            if (popcount32(bits) != total - k)
            {
                continue;
            }

            mask_from_bits(mask, bits, total);
            for (i = 0; i < total; i++)
            {
                if (!mask_get(mask, i))
                {
                    memset(shares[i], 0xa5, len);
                }
            }

            rc = fec_decode(ctx, shares, mask, len);
            CHECK(rc == FEC_OK,
                  "loss decode failed for (%u,%u) k=%u mask=0x%x rc=%d", n, m,
                  k, bits, rc);
            CHECK(memcmp(buf, orig, (size_t)total * len) == 0,
                  "loss decode mismatch for (%u,%u) k=%u mask=0x%x", n, m, k,
                  bits);
            cases++;
        }
    }

    /* Decode is idempotent. */
    {
        uint8_t mask[FEC_MASK_BYTES];

        mask_zero(mask);
        for (i = 1; i < total; i++)
        {
            mask_set(mask, i);
        }
        CHECK(fec_decode(ctx, shares, mask, len) == FEC_OK,
              "first single-loss decode failed for (%u,%u)", n, m);
        CHECK(fec_decode(ctx, shares, mask, len) == FEC_OK,
              "second single-loss decode failed for (%u,%u)", n, m);
        CHECK(memcmp(buf, orig, (size_t)total * len) == 0,
              "repeated decode changed data for (%u,%u)", n, m);
        cases++;
    }

    printf("(%u,%u): %u loss patterns ok\n", n, m, cases);

    free(buf);
    free(orig);
    fec_free(ctx);
}

static void test_rejections(unsigned n, unsigned m, size_t len)
{
    unsigned total = n + m;
    struct fec_ctx *ctx = fec_new(n, m);
    uint8_t *buf;
    uint8_t *shares[FEC_MAX_SHARES];
    uint8_t *data[FEC_MAX_SHARES];
    uint8_t *parity[FEC_MAX_SHARES];
    uint8_t mask[FEC_MASK_BYTES];
    uint8_t *saved;
    unsigned i;
    int rc;

    CHECK(ctx != NULL, "fec_new(%u, %u) failed", n, m);
    if (!ctx)
    {
        return;
    }

    buf = xmalloc((size_t)total * len);
    for (i = 0; i < total; i++)
    {
        shares[i] = buf + (size_t)i * len;
    }
    for (i = 0; i < n; i++)
    {
        data[i] = shares[i];
    }
    for (i = 0; i < m; i++)
    {
        parity[i] = shares[n + i];
    }

    fill_random(buf, (size_t)n * len);
    CHECK(fec_encode(ctx, data, parity, len) == FEC_OK,
          "encode failed for (%u,%u)", n, m);

    mask_zero(mask);
    for (i = 0; i < total; i++)
    {
        mask_set(mask, i);
    }

    /* Duplicate share buffer. */
    saved = shares[1];
    shares[1] = shares[0];
    rc = fec_decode(ctx, shares, mask, len);
    CHECK(rc == FEC_ERR_DUP, "duplicate share returned %d, want %d", rc,
          FEC_ERR_DUP);
    shares[1] = saved;

    /* NULL share buffer. */
    saved = shares[0];
    shares[0] = NULL;
    rc = fec_decode(ctx, shares, mask, len);
    CHECK(rc == FEC_ERR_PARAM, "NULL share returned %d, want %d", rc,
          FEC_ERR_PARAM);
    shares[0] = saved;

    /* Mask bit outside n + m. */
    mask_set(mask, total);
    rc = fec_decode(ctx, shares, mask, len);
    CHECK(rc == FEC_ERR_MASK, "bad mask returned %d, want %d", rc,
          FEC_ERR_MASK);
    mask_clear(mask, total);

    /* Fewer than n shares. */
    mask_zero(mask);
    for (i = 0; i < n - 1; i++)
    {
        mask_set(mask, i);
    }
    rc = fec_decode(ctx, shares, mask, len);
    CHECK(rc == FEC_ERR_TOO_FEW, "too few shares returned %d, want %d", rc,
          FEC_ERR_TOO_FEW);

    /* share_len must be a nonzero power of two. */
    CHECK(fec_decode(ctx, shares, mask, 0) == FEC_ERR_PARAM,
          "share_len=0 accepted");
    CHECK(fec_decode(ctx, shares, mask, 3) == FEC_ERR_PARAM,
          "share_len=3 accepted");
    CHECK(fec_decode(ctx, shares, mask, 1000) == FEC_ERR_PARAM,
          "share_len=1000 accepted");
    CHECK(fec_encode(ctx, data, parity, 0) == FEC_ERR_PARAM,
          "encode share_len=0 accepted");
    CHECK(fec_encode(ctx, data, parity, 3) == FEC_ERR_PARAM,
          "encode share_len=3 accepted");

    /* NULL arrays. */
    CHECK(fec_encode(ctx, NULL, parity, len) == FEC_ERR_PARAM,
          "NULL data array accepted");
    CHECK(fec_encode(ctx, data, NULL, len) == FEC_ERR_PARAM,
          "NULL parity array accepted");
    CHECK(fec_decode(ctx, NULL, mask, len) == FEC_ERR_PARAM,
          "NULL share array accepted");
    CHECK(fec_decode(ctx, shares, NULL, len) == FEC_ERR_PARAM,
          "NULL mask accepted");

    free(buf);
    fec_free(ctx);
}

static void test_api_guards(void)
{
    struct fec_ctx *ctx;

    CHECK(fec_new(0, 1) == NULL, "fec_new(0,1) accepted");
    CHECK(fec_new(1, 0) == NULL, "fec_new(1,0) accepted");
    CHECK(fec_new(128, 128) == NULL, "fec_new(128,128) accepted");
    CHECK(fec_new(200, 56) == NULL, "fec_new(200,56) accepted");
    CHECK(fec_new(1, 255) == NULL, "fec_new(1,255) accepted");
    CHECK(fec_n(NULL) == 0, "fec_n(NULL) != 0");
    CHECK(fec_m(NULL) == 0, "fec_m(NULL) != 0");

    /* This code is too large for the exhaustive MDS check. It uses sampling. */
    ctx = fec_new(16, 8);
    CHECK(ctx != NULL, "fec_new(16,8) failed");
    CHECK(fec_n(ctx) == 16 && fec_m(ctx) == 8, "fec_new(16,8) sizes");
    fec_free(ctx);

    /* The largest legal code. M = 1 uses the XOR path. */
    ctx = fec_new(254, 1);
    CHECK(ctx != NULL, "fec_new(254,1) failed");
    CHECK(fec_n(ctx) == 254 && fec_m(ctx) == 1, "fec_new(254,1) sizes");
    fec_free(ctx);
}

static void test_fuzz(unsigned rounds)
{
    unsigned r;

    for (r = 0; r < rounds; r++)
    {
        unsigned n = 1 + rnd32() % 8;
        unsigned m = 1 + rnd32() % 4;
        unsigned total = n + m;
        size_t len = (size_t)1u << (rnd32() % 13);
        struct fec_ctx *ctx = fec_new(n, m);
        uint8_t *buf;
        uint8_t *orig;
        uint8_t *shares[FEC_MAX_SHARES];
        uint8_t *data[FEC_MAX_SHARES];
        uint8_t *parity[FEC_MAX_SHARES];
        uint8_t mask[FEC_MASK_BYTES];
        unsigned idx[FEC_MAX_SHARES];
        unsigned loss;
        unsigned i;
        unsigned j;
        int rc;

        CHECK(ctx != NULL, "fuzz: fec_new(%u,%u) failed", n, m);
        if (!ctx)
        {
            continue;
        }

        buf = xmalloc((size_t)total * len);
        orig = xmalloc((size_t)total * len);

        for (i = 0; i < total; i++)
        {
            shares[i] = buf + (size_t)i * len;
            idx[i] = i;
        }
        for (i = 0; i < n; i++)
        {
            data[i] = shares[i];
        }
        for (i = 0; i < m; i++)
        {
            parity[i] = shares[n + i];
        }

        fill_random(buf, (size_t)n * len);
        CHECK(fec_encode(ctx, data, parity, len) == FEC_OK,
              "fuzz: encode failed for (%u,%u) len=%zu", n, m, len);
        memcpy(orig, buf, (size_t)total * len);

        loss = rnd32() % (m + 1);
        for (i = 0; i < loss; i++)
        {
            j = i + rnd32() % (total - i);
            {
                unsigned t = idx[i];

                idx[i] = idx[j];
                idx[j] = t;
            }
        }

        mask_zero(mask);
        for (i = 0; i < total; i++)
        {
            mask_set(mask, i);
        }
        for (i = 0; i < loss; i++)
        {
            mask_clear(mask, idx[i]);
        }

        for (i = 0; i < total; i++)
        {
            if (!mask_get(mask, i))
            {
                memset(shares[i], 0xa5, len);
            }
        }

        rc = fec_decode(ctx, shares, mask, len);
        CHECK(rc == FEC_OK, "fuzz: decode failed for (%u,%u) len=%zu loss=%u rc=%d",
              n, m, len, loss, rc);
        CHECK(memcmp(buf, orig, (size_t)total * len) == 0,
              "fuzz: mismatch for (%u,%u) len=%zu loss=%u", n, m, len, loss);

        free(buf);
        free(orig);
        fec_free(ctx);
    }
}

int main(void)
{
    static const unsigned pairs[][2] = {
        {4, 1}, {4, 2}, {2, 1}, {8, 2}, {8, 4}, {1, 1},
    };
    unsigned combos = 0;
    unsigned i;

    test_gf();
    test_api_guards();

    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
    {
        unsigned n = pairs[i][0];
        unsigned m = pairs[i][1];

        combos += test_all_combinations(n, m);
        test_loss_patterns(n, m, 1024);
        test_rejections(n, m, 256);
    }

    test_fuzz(1000);

    if (failures != 0)
    {
        fprintf(stderr, "FAIL: %u check(s) failed\n", failures);
        return 1;
    }

    printf("PASS: %u decode combinations, loss patterns, guards, 1000 fuzz rounds\n",
           combos);
    return 0;
}
