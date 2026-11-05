#include "fec.h"
#include "gf256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Precompute a 256-byte multiply table for each matrix coefficient when the
 * matrix is small. Larger matrices use gf_mul() directly. Both paths use the
 * exp/log tables.
 */
#define FEC_TAB_LIMIT 1024u

/* Upper bound on the work of the exhaustive MDS check, in matrix cells. */
#define FEC_MDS_WORK_LIMIT 50000000ull

struct fec_ctx
{
    unsigned n;
    unsigned m;
    unsigned total;

    /* (n + m) x n generation matrix. Top n rows are the identity. */
    uint8_t *g;

    int use_tab;
    uint8_t *enc_tab; /* m x n x 256 */
    uint8_t *dec_tab; /* m x n x 256, rebuilt on each decode */

    /* Decode workspace. The byte loops never allocate. */
    uint8_t *a;    /* n x n */
    uint8_t *aug;  /* n x 2n */
    uint8_t *inv;  /* n x n */
    uint8_t *comb; /* m x n */
    unsigned *sel;
    unsigned *miss;
};

static int valid_len(size_t len)
{
    return len != 0 && (len & (len - 1)) == 0;
}

static int mask_bit(const uint8_t mask[FEC_MASK_BYTES], unsigned bit)
{
    return (mask[bit >> 3] >> (bit & 7)) & 1;
}

static unsigned mask_count(const uint8_t mask[FEC_MASK_BYTES], unsigned total)
{
    unsigned i;
    unsigned c = 0;

    for (i = 0; i < total; i++)
    {
        c += (unsigned)mask_bit(mask, i);
    }

    return c;
}

/* Bits at or above total must be zero. */
static int mask_valid(const uint8_t mask[FEC_MASK_BYTES], unsigned total)
{
    unsigned i;

    for (i = total; i < FEC_MASK_BYTES * 8u; i++)
    {
        if (mask_bit(mask, i))
        {
            return 0;
        }
    }

    return 1;
}

static void xor_into(uint8_t *restrict dst, const uint8_t *restrict src,
                     size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        dst[i] ^= src[i];
    }
}

static void mul_fill(uint8_t *restrict dst, const uint8_t *restrict src,
                     const uint8_t *restrict tab, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        dst[i] = tab[src[i]];
    }
}

static void mul_add(uint8_t *restrict dst, const uint8_t *restrict src,
                    const uint8_t *restrict tab, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        dst[i] ^= tab[src[i]];
    }
}

static void mul_add_gf(uint8_t *restrict dst, const uint8_t *restrict src,
                       uint8_t c, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        dst[i] ^= gf_mul(c, src[i]);
    }
}

/*
 * Invert the n x n matrix a into inv.
 * aug is a caller buffer of n * 2n bytes.
 * Return 0 on success and -1 when a is singular.
 */
static int mat_inv(const uint8_t *a, uint8_t *inv, unsigned n, uint8_t *aug)
{
    unsigned w = 2 * n;
    unsigned r;
    unsigned c;
    unsigned k;

    for (r = 0; r < n; r++)
    {
        memcpy(aug + (size_t)r * w, a + (size_t)r * n, n);
        memset(aug + (size_t)r * w + n, 0, n);
        aug[(size_t)r * w + n + r] = 1;
    }

    for (c = 0; c < n; c++)
    {
        unsigned p = c;

        while (p < n && aug[(size_t)p * w + c] == 0)
        {
            p++;
        }

        if (p == n)
        {
            return -1;
        }

        if (p != c)
        {
            for (k = 0; k < w; k++)
            {
                uint8_t t = aug[(size_t)c * w + k];
                aug[(size_t)c * w + k] = aug[(size_t)p * w + k];
                aug[(size_t)p * w + k] = t;
            }
        }

        if (aug[(size_t)c * w + c] != 1)
        {
            uint8_t ip = gf_inv(aug[(size_t)c * w + c]);

            for (k = 0; k < w; k++)
            {
                aug[(size_t)c * w + k] = gf_mul(aug[(size_t)c * w + k], ip);
            }
        }

        for (r = 0; r < n; r++)
        {
            uint8_t f;

            if (r == c)
            {
                continue;
            }

            f = aug[(size_t)r * w + c];
            if (f == 0)
            {
                continue;
            }

            for (k = 0; k < w; k++)
            {
                aug[(size_t)r * w + k] ^= gf_mul(f, aug[(size_t)c * w + k]);
            }
        }
    }

    for (r = 0; r < n; r++)
    {
        memcpy(inv + (size_t)r * n, aug + (size_t)r * w + n, n);
    }

    return 0;
}

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ull);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* Build the n x n matrix from the selected rows and invert it. */
static int rows_invertible(struct fec_ctx *ctx, const unsigned *sel)
{
    unsigned k;

    for (k = 0; k < ctx->n; k++)
    {
        memcpy(ctx->a + (size_t)k * ctx->n,
               ctx->g + (size_t)sel[k] * ctx->n, ctx->n);
    }

    return mat_inv(ctx->a, ctx->inv, ctx->n, ctx->aug) == 0;
}

static int mds_rec(struct fec_ctx *ctx, unsigned start, unsigned depth)
{
    unsigned r;

    if (depth == ctx->n)
    {
        return rows_invertible(ctx, ctx->sel);
    }

    for (r = start; r + (ctx->n - depth) <= ctx->total; r++)
    {
        ctx->sel[depth] = r;
        if (!mds_rec(ctx, r + 1, depth + 1))
        {
            return 0;
        }
    }

    return 1;
}

static unsigned long long binom(unsigned n, unsigned k)
{
    unsigned long long r = 1;
    unsigned i;

    if (k > n)
    {
        return 0;
    }

    if (k > n - k)
    {
        k = n - k;
    }

    for (i = 1; i <= k; i++)
    {
        r = r * (n - k + i) / i;
    }

    return r;
}

/*
 * Check that every n rows of the generation matrix are invertible.
 *
 * Exhaustive search is cheap for small n + m. For larger codes the search
 * space is too large. The Cauchy construction guarantees the MDS property;
 * the sampled check only guards the implementation.
 */
static int mds_check(struct fec_ctx *ctx)
{
    unsigned long long combos;
    unsigned long long work;
    unsigned long long n3;
    unsigned trials;
    unsigned i;
    unsigned j;
    uint64_t rng;

    for (i = 0; i < ctx->m; i++)
    {
        for (j = 0; j < ctx->n; j++)
        {
            if ((uint8_t)(i ^ (ctx->m + j)) == 0)
            {
                return 0;
            }
        }
    }

    n3 = (unsigned long long)ctx->n * ctx->n * ctx->n;
    combos = ctx->total <= 32 ? binom(ctx->total, ctx->n) : ~0ull;
    work = combos * n3;

    if (work <= FEC_MDS_WORK_LIMIT)
    {
        return mds_rec(ctx, 0, 0);
    }

    trials = (unsigned)(FEC_MDS_WORK_LIMIT / n3);
    if (trials > 4096)
    {
        trials = 4096;
    }
    if (trials == 0)
    {
        trials = 1;
    }

    rng = 0x9e3779b97f4a7c15ull ^ ((uint64_t)ctx->n << 32) ^ ctx->m;
    for (i = 0; i < trials; i++)
    {
        for (j = 0; j < ctx->total; j++)
        {
            ctx->sel[j] = j;
        }

        for (j = 0; j < ctx->n; j++)
        {
            unsigned p = j + (unsigned)(splitmix64(&rng) %
                                       (uint64_t)(ctx->total - j));
            unsigned t = ctx->sel[j];

            ctx->sel[j] = ctx->sel[p];
            ctx->sel[p] = t;
        }

        if (!rows_invertible(ctx, ctx->sel))
        {
            return 0;
        }
    }

    return 1;
}

struct fec_ctx *fec_new(unsigned n, unsigned m)
{
    struct fec_ctx *ctx;
    unsigned i;
    unsigned j;
    unsigned v;

    if (n == 0 || m == 0 || n + m > FEC_MAX_SHARES)
    {
        return NULL;
    }

    gf256_init();

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
    {
        return NULL;
    }

    ctx->n = n;
    ctx->m = m;
    ctx->total = n + m;
    ctx->use_tab = n * m <= FEC_TAB_LIMIT;

    ctx->g = malloc((size_t)ctx->total * n);
    ctx->a = malloc((size_t)n * n);
    ctx->aug = malloc((size_t)n * 2 * n);
    ctx->inv = malloc((size_t)n * n);
    ctx->comb = malloc((size_t)m * n);
    ctx->sel = malloc((size_t)ctx->total * sizeof(unsigned));
    ctx->miss = malloc((size_t)m * sizeof(unsigned));

    if (ctx->use_tab)
    {
        ctx->enc_tab = malloc((size_t)m * n * 256);
        ctx->dec_tab = malloc((size_t)m * n * 256);
    }

    if (!ctx->g || !ctx->a || !ctx->aug || !ctx->inv || !ctx->comb ||
        !ctx->sel || !ctx->miss ||
        (ctx->use_tab && (!ctx->enc_tab || !ctx->dec_tab)))
    {
        fec_free(ctx);
        return NULL;
    }

    memset(ctx->g, 0, (size_t)ctx->total * n);
    for (i = 0; i < n; i++)
    {
        ctx->g[(size_t)i * n + i] = 1;
    }

    if (m == 1)
    {
        /* The single parity share is the XOR of all data shares. */
        for (j = 0; j < n; j++)
        {
            ctx->g[(size_t)n * n + j] = 1;
        }
    }
    else
    {
        /*
         * Cauchy matrix: C[i][j] = 1 / (x_i + y_j).
         * x_i = i and y_j = m + j are distinct and disjoint.
         */
        for (i = 0; i < m; i++)
        {
            for (j = 0; j < n; j++)
            {
                ctx->g[(size_t)(n + i) * n + j] =
                    gf_inv((uint8_t)(i ^ (m + j)));
            }
        }
    }

    if (ctx->use_tab)
    {
        for (i = 0; i < m; i++)
        {
            for (j = 0; j < n; j++)
            {
                uint8_t c = ctx->g[(size_t)(n + i) * n + j];
                uint8_t *tab = ctx->enc_tab + ((size_t)i * n + j) * 256;

                for (v = 0; v < 256; v++)
                {
                    tab[v] = gf_mul(c, (uint8_t)v);
                }
            }
        }
    }

    if (!mds_check(ctx))
    {
        fprintf(stderr, "fec: MDS check failed for n=%u m=%u\n", n, m);
        fec_free(ctx);
        return NULL;
    }

    return ctx;
}

void fec_free(struct fec_ctx *ctx)
{
    if (!ctx)
    {
        return;
    }

    free(ctx->g);
    free(ctx->enc_tab);
    free(ctx->dec_tab);
    free(ctx->a);
    free(ctx->aug);
    free(ctx->inv);
    free(ctx->comb);
    free(ctx->sel);
    free(ctx->miss);
    free(ctx);
}

unsigned fec_n(const struct fec_ctx *ctx)
{
    return ctx ? ctx->n : 0;
}

unsigned fec_m(const struct fec_ctx *ctx)
{
    return ctx ? ctx->m : 0;
}

int fec_encode(struct fec_ctx *ctx, uint8_t *data_shares[],
               uint8_t *parity_shares[], size_t share_len)
{
    unsigned i;
    unsigned j;

    if (!ctx || !data_shares || !parity_shares || !valid_len(share_len))
    {
        return FEC_ERR_PARAM;
    }

    for (i = 0; i < ctx->n; i++)
    {
        if (!data_shares[i])
        {
            return FEC_ERR_PARAM;
        }
    }

    for (i = 0; i < ctx->m; i++)
    {
        if (!parity_shares[i])
        {
            return FEC_ERR_PARAM;
        }
    }

    if (ctx->m == 1)
    {
        memcpy(parity_shares[0], data_shares[0], share_len);
        for (j = 1; j < ctx->n; j++)
        {
            xor_into(parity_shares[0], data_shares[j], share_len);
        }
        return FEC_OK;
    }

    if (ctx->use_tab)
    {
        for (i = 0; i < ctx->m; i++)
        {
            const uint8_t *row = ctx->enc_tab + (size_t)i * ctx->n * 256;
            uint8_t *dst = parity_shares[i];

            mul_fill(dst, data_shares[0], row, share_len);
            for (j = 1; j < ctx->n; j++)
            {
                mul_add(dst, data_shares[j], row + (size_t)j * 256, share_len);
            }
        }
        return FEC_OK;
    }

    for (i = 0; i < ctx->m; i++)
    {
        const uint8_t *row = ctx->g + (size_t)(ctx->n + i) * ctx->n;
        uint8_t *dst = parity_shares[i];

        memset(dst, 0, share_len);
        for (j = 0; j < ctx->n; j++)
        {
            uint8_t c = row[j];

            if (c == 1)
            {
                xor_into(dst, data_shares[j], share_len);
            }
            else if (c != 0)
            {
                mul_add_gf(dst, data_shares[j], c, share_len);
            }
        }
    }

    return FEC_OK;
}

int fec_decode(struct fec_ctx *ctx, uint8_t *shares[],
               const uint8_t present_mask[FEC_MASK_BYTES], size_t share_len)
{
    unsigned i;
    unsigned j;
    unsigned k;
    unsigned count;
    unsigned nmiss;

    if (!ctx || !shares || !present_mask || !valid_len(share_len))
    {
        return FEC_ERR_PARAM;
    }

    for (i = 0; i < ctx->total; i++)
    {
        if (!shares[i])
        {
            return FEC_ERR_PARAM;
        }
    }

    for (i = 0; i < ctx->total; i++)
    {
        for (j = i + 1; j < ctx->total; j++)
        {
            if (shares[i] == shares[j])
            {
                return FEC_ERR_DUP;
            }
        }
    }

    if (!mask_valid(present_mask, ctx->total))
    {
        return FEC_ERR_MASK;
    }

    count = mask_count(present_mask, ctx->total);
    if (count < ctx->n)
    {
        return FEC_ERR_TOO_FEW;
    }

    k = 0;
    for (i = 0; i < ctx->total; i++)
    {
        if (mask_bit(present_mask, i))
        {
            ctx->sel[k++] = i;
        }
    }

    if (count == ctx->total)
    {
        return FEC_OK;
    }

    if (ctx->m == 1)
    {
        unsigned missing = 0;

        for (i = 0; i < ctx->total; i++)
        {
            if (!mask_bit(present_mask, i))
            {
                missing = i;
                break;
            }
        }

        /* Every share XORs to zero. The missing share is the XOR of the rest. */
        memcpy(shares[missing], shares[ctx->sel[0]], share_len);
        for (j = 1; j < count; j++)
        {
            xor_into(shares[missing], shares[ctx->sel[j]], share_len);
        }

        return FEC_OK;
    }

    for (k = 0; k < ctx->n; k++)
    {
        memcpy(ctx->a + (size_t)k * ctx->n,
               ctx->g + (size_t)ctx->sel[k] * ctx->n, ctx->n);
    }

    if (mat_inv(ctx->a, ctx->inv, ctx->n, ctx->aug) != 0)
    {
        return FEC_ERR_SINGULAR;
    }

    nmiss = ctx->total - count;
    k = 0;
    for (i = 0; i < ctx->total; i++)
    {
        if (!mask_bit(present_mask, i))
        {
            ctx->miss[k++] = i;
        }
    }

    for (k = 0; k < nmiss; k++)
    {
        unsigned r = ctx->miss[k];
        const uint8_t *grow = ctx->g + (size_t)r * ctx->n;
        uint8_t *comb = ctx->comb + (size_t)k * ctx->n;
        uint8_t *dst = shares[r];

        /*
         * share[r] = row(r) * inverse(A) * present.
         * Compute the n coefficients once, outside the byte loop.
         */
        for (j = 0; j < ctx->n; j++)
        {
            uint8_t acc = 0;
            unsigned c;

            for (c = 0; c < ctx->n; c++)
            {
                acc ^= gf_mul(grow[c], ctx->inv[(size_t)c * ctx->n + j]);
            }
            comb[j] = acc;
        }

        if (ctx->use_tab)
        {
            uint8_t *tab = ctx->dec_tab + (size_t)k * ctx->n * 256;
            const uint8_t *tabs[FEC_MAX_SHARES];
            const uint8_t *srcs[FEC_MAX_SHARES];
            size_t b;

            for (j = 0; j < ctx->n; j++)
            {
                uint8_t c = comb[j];
                uint8_t *t = tab + (size_t)j * 256;
                unsigned v;

                if (c == 0)
                {
                    memset(t, 0, 256);
                }
                else
                {
                    for (v = 0; v < 256; v++)
                    {
                        t[v] = gf_mul(c, (uint8_t)v);
                    }
                }

                tabs[j] = t;
                srcs[j] = shares[ctx->sel[j]];
            }

            /* One pass over dst. The n source shares stream in parallel. */
            for (b = 0; b < share_len; b++)
            {
                uint8_t acc = 0;

                for (j = 0; j < ctx->n; j++)
                {
                    acc ^= tabs[j][srcs[j][b]];
                }
                dst[b] = acc;
            }

            continue;
        }

        memset(dst, 0, share_len);
        for (j = 0; j < ctx->n; j++)
        {
            uint8_t c = comb[j];
            const uint8_t *src = shares[ctx->sel[j]];

            if (c == 0)
            {
                continue;
            }

            if (c == 1)
            {
                xor_into(dst, src, share_len);
            }
            else
            {
                mul_add_gf(dst, src, c, share_len);
            }
        }
    }

    return FEC_OK;
}
