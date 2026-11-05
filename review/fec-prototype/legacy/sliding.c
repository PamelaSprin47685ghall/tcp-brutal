#include "sliding.h"
#include "gf16.h"
#include "gf256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLIDING_TAB_LIMIT 4096u /* m * w coefficients */

typedef uint8_t (*gf_mul_fn)(uint8_t, uint8_t);
typedef uint8_t (*gf_inv_fn)(uint8_t);

struct sliding_ctx
{
    unsigned n;
    unsigned m;
    unsigned k;
    unsigned w;
    size_t s;
    enum sliding_scheme scheme;

    unsigned L;
    unsigned n_groups;
    unsigned n_parity;

    uint8_t *coef;    /* m x w */
    uint8_t *enc_tab; /* m x w x 256, L3 only */
    int use_tab;

    uint8_t *data_known;
    uint8_t *parity_known;
    uint8_t *group_retired;

    unsigned frontier;
    unsigned stuck_at;
    unsigned retired_count;
    unsigned recovered_count;

    uint8_t **data;
    uint8_t **parity;

    /* Decode workspace. */
    uint8_t *a;
    uint8_t *inv;
    uint8_t *aug;
    uint8_t *tmp; /* m x s */
    unsigned *cols;
    unsigned *rows;
};

static void xor_into(uint8_t *restrict dst, const uint8_t *restrict src,
                     size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        dst[i] ^= src[i];
    }
}

static void mul_add_gf(uint8_t *restrict dst, const uint8_t *restrict src,
                       uint8_t c, size_t len, gf_mul_fn mul)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        dst[i] ^= mul(c, src[i]);
    }
}

/*
 * GF(2^4) multiply-add. Each byte holds two symbols: high and low nibble.
 * Multiply both nibbles and put the results back together.
 */
static void mul_add16(uint8_t *restrict dst, const uint8_t *restrict src,
                      uint8_t c, size_t len)
{
    const uint8_t *t = gf16_mul_tab + (size_t)c * 16;
    size_t i;

    for (i = 0; i < len; i++)
    {
        uint8_t v = src[i];

        dst[i] ^= (uint8_t)((t[v >> 4] << 4) | t[v & 0xfu]);
    }
}

/*
 * Invert the u x u matrix a into inv.
 * aug is a caller buffer of u * 2u bytes.
 * Return 0 on success and -1 when a is singular.
 */
static int mat_inv(const uint8_t *a, uint8_t *inv, unsigned u, uint8_t *aug,
                   gf_mul_fn mul, gf_inv_fn inv_fn)
{
    unsigned w = 2 * u;
    unsigned r;
    unsigned c;
    unsigned k;

    for (r = 0; r < u; r++)
    {
        memcpy(aug + (size_t)r * w, a + (size_t)r * u, u);
        memset(aug + (size_t)r * w + u, 0, u);
        aug[(size_t)r * w + u + r] = 1;
    }

    for (c = 0; c < u; c++)
    {
        unsigned p = c;

        while (p < u && aug[(size_t)p * w + c] == 0)
        {
            p++;
        }

        if (p == u)
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
            uint8_t ip = inv_fn(aug[(size_t)c * w + c]);

            for (k = 0; k < w; k++)
            {
                aug[(size_t)c * w + k] = mul(aug[(size_t)c * w + k], ip);
            }
        }

        for (r = 0; r < u; r++)
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
                aug[(size_t)r * w + k] ^= mul(f, aug[(size_t)c * w + k]);
            }
        }
    }

    for (r = 0; r < u; r++)
    {
        memcpy(inv + (size_t)r * u, aug + (size_t)r * w + u, u);
    }

    return 0;
}

static unsigned group_count(unsigned L, unsigned k)
{
    return L / k + (L % k != 0);
}

static void group_window(const struct sliding_ctx *ctx, unsigned g,
                         int *win_start, int *win_end)
{
    *win_start = (int)(g * ctx->k) - (int)(ctx->w - ctx->k);
    *win_end = (int)(g * ctx->k + ctx->k - 1);
}

static void window_limits(const struct sliding_ctx *ctx, unsigned g,
                          unsigned *i0, unsigned *i1)
{
    int ws;
    int we;

    group_window(ctx, g, &ws, &we);

    *i0 = ws < 0 ? 0 : (unsigned)ws;
    *i1 = we >= (int)ctx->L ? ctx->L - 1 : (unsigned)we;
}

static void update_frontier(struct sliding_ctx *ctx)
{
    while (ctx->frontier < ctx->L && ctx->data_known[ctx->frontier])
    {
        ctx->frontier++;
    }
    ctx->stuck_at = ctx->frontier;
}

static void retire_groups(struct sliding_ctx *ctx)
{
    unsigned g;

    for (g = 0; g < ctx->n_groups; g++)
    {
        int ws;
        int we;

        if (ctx->group_retired[g])
        {
            continue;
        }

        group_window(ctx, g, &ws, &we);
        if (we < (int)ctx->frontier)
        {
            ctx->group_retired[g] = 1;
            ctx->retired_count++;
        }
    }
}

/* L0: one XOR parity per window. */
static unsigned recover_group_l0(struct sliding_ctx *ctx, unsigned g)
{
    unsigned i0;
    unsigned i1;
    unsigned i;
    unsigned unknown = 0;
    unsigned target = 0;

    if (!ctx->parity_known[(size_t)g * ctx->m])
    {
        return 0;
    }

    window_limits(ctx, g, &i0, &i1);
    for (i = i0; i <= i1; i++)
    {
        if (!ctx->data_known[i])
        {
            unknown++;
            target = i;
        }
    }

    if (unknown != 1)
    {
        return 0;
    }

    {
        uint8_t *dst = ctx->data[target];

        memcpy(dst, ctx->parity[(size_t)g * ctx->m], ctx->s);
        for (i = i0; i <= i1; i++)
        {
            if (ctx->data_known[i])
            {
                xor_into(dst, ctx->data[i], ctx->s);
            }
        }
    }

    ctx->data_known[target] = 1;
    ctx->recovered_count++;
    return 1;
}

/*
 * L1: row r covers the subset j % m == r. Each subset has one XOR equation.
 * Recover at most one loss per subset.
 */
static unsigned recover_group_l1(struct sliding_ctx *ctx, unsigned g)
{
    unsigned i0;
    unsigned i1;
    unsigned row;
    unsigned recovered = 0;
    int ws;
    int we;

    group_window(ctx, g, &ws, &we);
    window_limits(ctx, g, &i0, &i1);

    for (row = 0; row < ctx->m; row++)
    {
        unsigned i;
        unsigned unknown = 0;
        unsigned target = 0;
        uint8_t *dst;

        if (!ctx->parity_known[(size_t)g * ctx->m + row])
        {
            continue;
        }

        for (i = i0; i <= i1; i++)
        {
            unsigned j = (unsigned)((int)i - ws);

            if (j % ctx->m != row)
            {
                continue;
            }

            if (!ctx->data_known[i])
            {
                unknown++;
                target = i;
            }
        }

        if (unknown != 1)
        {
            continue;
        }

        dst = ctx->data[target];
        memcpy(dst, ctx->parity[(size_t)g * ctx->m + row], ctx->s);
        for (i = i0; i <= i1; i++)
        {
            unsigned j = (unsigned)((int)i - ws);

            if (j % ctx->m != row)
            {
                continue;
            }

            if (ctx->data_known[i])
            {
                xor_into(dst, ctx->data[i], ctx->s);
            }
        }

        ctx->data_known[target] = 1;
        ctx->recovered_count++;
        recovered++;
    }

    return recovered;
}

static void coef_mul_add(struct sliding_ctx *ctx, uint8_t *restrict dst,
                         const uint8_t *restrict src, uint8_t c,
                         unsigned row, unsigned j)
{
    if (c == 1)
    {
        xor_into(dst, src, ctx->s);
        return;
    }

    if (ctx->scheme == SLIDING_L2_RS4)
    {
        mul_add16(dst, src, c, ctx->s);
        return;
    }

    {
        const uint8_t *t =
            ctx->enc_tab + ((size_t)row * ctx->w + j) * 256;
        size_t b;

        for (b = 0; b < ctx->s; b++)
        {
            dst[b] ^= t[src[b]];
        }
    }
}

/* L2/L3: solve u unknowns with u Cauchy rows. */
static unsigned recover_group_matrix(struct sliding_ctx *ctx, unsigned g)
{
    unsigned i0;
    unsigned i1;
    unsigned i;
    unsigned mi;
    unsigned u = 0;
    unsigned r = 0;
    unsigned x;
    unsigned y;
    int ws;
    int we;
    gf_mul_fn mul = ctx->scheme == SLIDING_L2_RS4 ? gf16_mul : gf_mul;
    gf_inv_fn inv_fn = ctx->scheme == SLIDING_L2_RS4 ? gf16_inv : gf_inv;

    group_window(ctx, g, &ws, &we);
    window_limits(ctx, g, &i0, &i1);

    for (i = i0; i <= i1; i++)
    {
        if (!ctx->data_known[i])
        {
            ctx->cols[u++] = i;
        }
    }

    if (u == 0)
    {
        return 0;
    }

    for (mi = 0; mi < ctx->m && r < u; mi++)
    {
        if (ctx->parity_known[(size_t)g * ctx->m + mi])
        {
            ctx->rows[r++] = mi;
        }
    }

    if (r < u)
    {
        return 0;
    }

    for (x = 0; x < u; x++)
    {
        for (y = 0; y < u; y++)
        {
            unsigned col = ctx->cols[y];
            unsigned row = ctx->rows[x];
            unsigned j = (unsigned)((int)col - ws);

            ctx->a[(size_t)x * u + y] = ctx->coef[(size_t)row * ctx->w + j];
        }
    }

    if (mat_inv(ctx->a, ctx->inv, u, ctx->aug, mul, inv_fn) != 0)
    {
        return 0;
    }

    for (x = 0; x < u; x++)
    {
        uint8_t *b = ctx->tmp + (size_t)x * ctx->s;
        unsigned row = ctx->rows[x];

        memcpy(b, ctx->parity[(size_t)g * ctx->m + row], ctx->s);
        for (i = i0; i <= i1; i++)
        {
            if (ctx->data_known[i])
            {
                unsigned j = (unsigned)((int)i - ws);
                uint8_t c = ctx->coef[(size_t)row * ctx->w + j];

                coef_mul_add(ctx, b, ctx->data[i], c, row, j);
            }
        }
    }

    for (y = 0; y < u; y++)
    {
        uint8_t *dst = ctx->data[ctx->cols[y]];

        memset(dst, 0, ctx->s);
        for (x = 0; x < u; x++)
        {
            uint8_t c = ctx->inv[(size_t)y * u + x];
            uint8_t *b = ctx->tmp + (size_t)x * ctx->s;

            if (c == 1)
            {
                xor_into(dst, b, ctx->s);
            }
            else if (c != 0)
            {
                if (ctx->scheme == SLIDING_L2_RS4)
                {
                    mul_add16(dst, b, c, ctx->s);
                }
                else
                {
                    mul_add_gf(dst, b, c, ctx->s, mul);
                }
            }
        }
        ctx->data_known[ctx->cols[y]] = 1;
        ctx->recovered_count++;
    }

    return u;
}

static unsigned recover_group(struct sliding_ctx *ctx, unsigned g)
{
    switch (ctx->scheme)
    {
    case SLIDING_L0_XOR:
        return recover_group_l0(ctx, g);
    case SLIDING_L1_XOR_PART:
        return recover_group_l1(ctx, g);
    case SLIDING_L2_RS4:
    case SLIDING_L3_RS8:
    default:
        return recover_group_matrix(ctx, g);
    }
}

static void recover_all(struct sliding_ctx *ctx)
{
    unsigned progress;

    do
    {
        unsigned g;

        progress = 0;
        for (g = 0; g < ctx->n_groups; g++)
        {
            if (ctx->group_retired[g])
            {
                continue;
            }

            progress += recover_group(ctx, g);
        }

        update_frontier(ctx);
        retire_groups(ctx);
    } while (progress != 0);
}

struct sliding_ctx *sliding_new(unsigned n, unsigned m, unsigned k,
                                unsigned w, size_t s,
                                enum sliding_scheme scheme)
{
    struct sliding_ctx *ctx;
    unsigned mi;
    unsigned j;
    unsigned v;

    if (n == 0 || m == 0 || k == 0 || w < k || s == 0 ||
        n + m > 255)
    {
        return NULL;
    }

    if (scheme < SLIDING_L0_XOR || scheme > SLIDING_L3_RS8)
    {
        return NULL;
    }

    if (scheme == SLIDING_L0_XOR && m != 1)
    {
        return NULL;
    }

    if (scheme == SLIDING_L2_RS4 && m + w > 16)
    {
        return NULL;
    }

    if (scheme == SLIDING_L3_RS8 && m + w > 256)
    {
        return NULL;
    }

    gf16_init();
    gf256_init();

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
    {
        return NULL;
    }

    ctx->n = n;
    ctx->m = m;
    ctx->k = k;
    ctx->w = w;
    ctx->s = s;
    ctx->scheme = scheme;
    ctx->use_tab = scheme == SLIDING_L3_RS8 && m * w <= SLIDING_TAB_LIMIT;

    ctx->coef = malloc((size_t)m * w);
    ctx->a = malloc((size_t)m * m);
    ctx->inv = malloc((size_t)m * m);
    ctx->aug = malloc((size_t)m * 2 * m);
    ctx->tmp = malloc((size_t)m * s);
    ctx->cols = malloc((size_t)w * sizeof(unsigned));
    ctx->rows = malloc((size_t)m * sizeof(unsigned));

    if (ctx->use_tab)
    {
        ctx->enc_tab = malloc((size_t)m * w * 256);
    }

    if (!ctx->coef || !ctx->a || !ctx->inv || !ctx->aug || !ctx->tmp ||
        !ctx->cols || !ctx->rows ||
        (ctx->use_tab && !ctx->enc_tab))
    {
        sliding_free(ctx);
        return NULL;
    }

    for (mi = 0; mi < m; mi++)
    {
        for (j = 0; j < w; j++)
        {
            uint8_t c = sliding_coef(scheme, mi, j, m);

            ctx->coef[(size_t)mi * w + j] = c;

            if (ctx->use_tab)
            {
                uint8_t *tab = ctx->enc_tab + ((size_t)mi * w + j) * 256;

                for (v = 0; v < 256; v++)
                {
                    tab[v] = gf_mul(c, (uint8_t)v);
                }
            }
        }
    }

    return ctx;
}

void sliding_free(struct sliding_ctx *ctx)
{
    if (!ctx)
    {
        return;
    }

    free(ctx->coef);
    free(ctx->enc_tab);
    free(ctx->data_known);
    free(ctx->parity_known);
    free(ctx->group_retired);
    free(ctx->a);
    free(ctx->inv);
    free(ctx->aug);
    free(ctx->tmp);
    free(ctx->cols);
    free(ctx->rows);
    free(ctx);
}

unsigned sliding_parity_count(const struct sliding_ctx *ctx, unsigned L)
{
    if (!ctx || L == 0)
    {
        return 0;
    }

    return group_count(L, ctx->k) * ctx->m;
}

int sliding_bind(struct sliding_ctx *ctx, uint8_t *data[], uint8_t *parity[],
                 unsigned L)
{
    unsigned groups;

    if (!ctx || !data || !parity || L == 0)
    {
        return SLIDING_ERR_PARAM;
    }

    groups = group_count(L, ctx->k);

    if (ctx->L != L)
    {
        free(ctx->data_known);
        free(ctx->parity_known);
        free(ctx->group_retired);

        ctx->data_known = calloc(L, 1);
        ctx->parity_known = calloc((size_t)groups * ctx->m, 1);
        ctx->group_retired = calloc(groups, 1);

        if (!ctx->data_known || !ctx->parity_known || !ctx->group_retired)
        {
            ctx->L = 0;
            return SLIDING_ERR_NOMEM;
        }

        ctx->L = L;
    }
    else
    {
        memset(ctx->data_known, 0, L);
        memset(ctx->parity_known, 0, (size_t)groups * ctx->m);
        memset(ctx->group_retired, 0, groups);
    }

    ctx->n_groups = groups;
    ctx->n_parity = groups * ctx->m;
    ctx->data = data;
    ctx->parity = parity;
    ctx->frontier = 0;
    ctx->stuck_at = 0;
    ctx->retired_count = 0;
    ctx->recovered_count = 0;

    return SLIDING_OK;
}

int sliding_encode(struct sliding_ctx *ctx, uint8_t *data[], unsigned L,
                   uint8_t *parity[], struct sliding_parity_info info[])
{
    unsigned groups;
    unsigned g;
    unsigned mi;
    unsigned i;

    if (!ctx || !data || !parity || L == 0)
    {
        return SLIDING_ERR_PARAM;
    }

    for (i = 0; i < L; i++)
    {
        if (!data[i])
        {
            return SLIDING_ERR_PARAM;
        }
    }

    groups = group_count(L, ctx->k);
    for (i = 0; i < groups * ctx->m; i++)
    {
        if (!parity[i])
        {
            return SLIDING_ERR_PARAM;
        }
    }

    for (g = 0; g < groups; g++)
    {
        int ws;
        int we;
        unsigned i0;
        unsigned i1;

        group_window(ctx, g, &ws, &we);
        i0 = ws < 0 ? 0 : (unsigned)ws;
        i1 = we >= (int)L ? L - 1 : (unsigned)we;

        for (mi = 0; mi < ctx->m; mi++)
        {
            uint8_t *dst = parity[(size_t)g * ctx->m + mi];

            memset(dst, 0, ctx->s);
            for (i = i0; i <= i1; i++)
            {
                unsigned j = (unsigned)((int)i - ws);
                uint8_t c = ctx->coef[(size_t)mi * ctx->w + j];

                if (c == 0)
                {
                    continue;
                }

                if (c == 1)
                {
                    xor_into(dst, data[i], ctx->s);
                }
                else if (ctx->scheme == SLIDING_L2_RS4)
                {
                    mul_add16(dst, data[i], c, ctx->s);
                }
                else if (ctx->use_tab)
                {
                    const uint8_t *t =
                        ctx->enc_tab + ((size_t)mi * ctx->w + j) * 256;
                    size_t b;

                    for (b = 0; b < ctx->s; b++)
                    {
                        dst[b] ^= t[data[i][b]];
                    }
                }
                else
                {
                    mul_add_gf(dst, data[i], c, ctx->s, gf_mul);
                }
            }

            if (info)
            {
                info[(size_t)g * ctx->m + mi].group = g;
                info[(size_t)g * ctx->m + mi].m = mi;
                info[(size_t)g * ctx->m + mi].win_start = ws;
                info[(size_t)g * ctx->m + mi].win_end = we;
            }
        }
    }

    return (int)(groups * ctx->m);
}

int sliding_decode(struct sliding_ctx *ctx, uint8_t *data[], uint8_t *parity[],
                   const uint8_t *data_present,
                   const uint8_t *parity_present, unsigned L)
{
    unsigned i;
    int rc;

    if (!ctx || !data || !parity || !data_present || !parity_present ||
        L == 0)
    {
        return SLIDING_ERR_PARAM;
    }

    rc = sliding_bind(ctx, data, parity, L);
    if (rc != SLIDING_OK)
    {
        return rc;
    }

    for (i = 0; i < L; i++)
    {
        ctx->data_known[i] = data_present[i] ? 1 : 0;
    }

    for (i = 0; i < ctx->n_parity; i++)
    {
        ctx->parity_known[i] = parity_present[i] ? 1 : 0;
    }

    recover_all(ctx);

    return ctx->frontier == L ? SLIDING_OK : SLIDING_ERR_STUCK;
}

int sliding_advance(struct sliding_ctx *ctx, int kind, unsigned index)
{
    if (!ctx || ctx->L == 0 || !ctx->data || !ctx->parity)
    {
        return SLIDING_ERR_PARAM;
    }

    if (kind == SLIDING_KIND_DATA)
    {
        if (index >= ctx->L)
        {
            return SLIDING_ERR_PARAM;
        }

        if (ctx->data_known[index])
        {
            return (int)ctx->frontier;
        }

        ctx->data_known[index] = 1;
    }
    else if (kind == SLIDING_KIND_PARITY)
    {
        if (index >= ctx->n_parity)
        {
            return SLIDING_ERR_PARAM;
        }

        if (ctx->parity_known[index])
        {
            return (int)ctx->frontier;
        }

        ctx->parity_known[index] = 1;
    }
    else
    {
        return SLIDING_ERR_PARAM;
    }

    recover_all(ctx);

    return (int)ctx->frontier;
}

unsigned sliding_frontier(const struct sliding_ctx *ctx)
{
    return ctx ? ctx->frontier : 0;
}

unsigned sliding_stuck_at(const struct sliding_ctx *ctx)
{
    return ctx ? ctx->stuck_at : 0;
}

unsigned sliding_retired_groups(const struct sliding_ctx *ctx)
{
    return ctx ? ctx->retired_count : 0;
}

unsigned sliding_recovered_packets(const struct sliding_ctx *ctx)
{
    return ctx ? ctx->recovered_count : 0;
}

int sliding_is_known(const struct sliding_ctx *ctx, unsigned index)
{
    if (!ctx || index >= ctx->L)
    {
        return 0;
    }

    return ctx->data_known[index] != 0;
}

enum sliding_scheme sliding_scheme_of(const struct sliding_ctx *ctx)
{
    return ctx ? ctx->scheme : SLIDING_L0_XOR;
}
