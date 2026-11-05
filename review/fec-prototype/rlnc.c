#include "rlnc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rlnc_ctx
{
    unsigned n;
    unsigned m;
    unsigned k;
    unsigned w;
    size_t s;
    unsigned d;
    uint64_t nonce;

    unsigned L;
    unsigned n_groups;
    unsigned n_parity;

    uint8_t *data_known;
    uint8_t *parity_known;

    unsigned frontier;
    unsigned stuck_at;
    unsigned recovered_count;

    uint8_t **data;
    uint8_t **parity;

    /* Decode workspace, sized in rlnc_bind(). */
    uint64_t *mat;
    uint8_t *rhs_buf;
    uint8_t **rhs_rows;
    unsigned *unknown;
    int *pos;
    unsigned *pivot;
    unsigned *idx;
};

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ull);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static unsigned group_count(unsigned L, unsigned k)
{
    return L / k + (L % k != 0);
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

unsigned rlnc_coeffs(const struct rlnc_ctx *ctx, unsigned g, unsigned r,
                     unsigned *idx, unsigned max)
{
    uint64_t seed;
    unsigned i;

    if (!ctx || !idx || max < ctx->d)
    {
        return 0;
    }

    seed = ctx->nonce ^
           ((uint64_t)((uint64_t)g * ctx->m + r) * 0x9e3779b97f4a7c15ull);

    for (i = 0; i < ctx->d; i++)
    {
        unsigned v;
        unsigned j;
        int again;

        do
        {
            v = (unsigned)(splitmix64(&seed) % ctx->w);
            again = 0;
            for (j = 0; j < i; j++)
            {
                if (idx[j] == v)
                {
                    again = 1;
                    break;
                }
            }
        } while (again);

        idx[i] = v;
    }

    return ctx->d;
}

struct rlnc_ctx *rlnc_new(unsigned n, unsigned m, unsigned k, unsigned w,
                          size_t s, unsigned d, uint64_t nonce)
{
    struct rlnc_ctx *ctx;

    if (n == 0 || m == 0 || k == 0 || w < k || s == 0 || d == 0 ||
        d > w || n + m > 256)
    {
        return NULL;
    }

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
    ctx->d = d;
    ctx->nonce = nonce;

    ctx->idx = malloc((size_t)w * sizeof(unsigned));
    if (!ctx->idx)
    {
        rlnc_free(ctx);
        return NULL;
    }

    return ctx;
}

void rlnc_free(struct rlnc_ctx *ctx)
{
    if (!ctx)
    {
        return;
    }

    free(ctx->data_known);
    free(ctx->parity_known);
    free(ctx->mat);
    free(ctx->rhs_buf);
    free(ctx->rhs_rows);
    free(ctx->unknown);
    free(ctx->pos);
    free(ctx->pivot);
    free(ctx->idx);
    free(ctx);
}

unsigned rlnc_parity_count(const struct rlnc_ctx *ctx, unsigned L)
{
    if (!ctx || L == 0)
    {
        return 0;
    }

    return group_count(L, ctx->k) * ctx->m;
}

int rlnc_bind(struct rlnc_ctx *ctx, uint8_t *data[], uint8_t *parity[],
              unsigned L)
{
    unsigned groups;
    unsigned parity_count;
    unsigned words;
    unsigned e;

    if (!ctx || !data || !parity || L == 0)
    {
        return RLNC_ERR_PARAM;
    }

    groups = group_count(L, ctx->k);
    parity_count = groups * ctx->m;
    words = (L + 63) / 64;

    if (ctx->L != L)
    {
        free(ctx->data_known);
        free(ctx->parity_known);
        free(ctx->mat);
        free(ctx->rhs_buf);
        free(ctx->rhs_rows);
        free(ctx->unknown);
        free(ctx->pos);
        free(ctx->pivot);

        ctx->data_known = calloc(L, 1);
        ctx->parity_known = calloc(parity_count, 1);
        ctx->mat = calloc((size_t)parity_count * words, sizeof(uint64_t));
        ctx->rhs_buf = malloc((size_t)parity_count * ctx->s);
        ctx->rhs_rows = malloc((size_t)parity_count * sizeof(uint8_t *));
        ctx->unknown = malloc((size_t)L * sizeof(unsigned));
        ctx->pos = malloc((size_t)L * sizeof(int));
        ctx->pivot = malloc((size_t)(parity_count < L ? parity_count : L) *
                            sizeof(unsigned));

        if (!ctx->data_known || !ctx->parity_known || !ctx->mat ||
            !ctx->rhs_buf || !ctx->rhs_rows || !ctx->unknown || !ctx->pos ||
            !ctx->pivot)
        {
            ctx->L = 0;
            return RLNC_ERR_NOMEM;
        }

        for (e = 0; e < parity_count; e++)
        {
            ctx->rhs_rows[e] = ctx->rhs_buf + (size_t)e * ctx->s;
        }

        ctx->L = L;
    }
    else
    {
        memset(ctx->data_known, 0, L);
        memset(ctx->parity_known, 0, parity_count);
    }

    ctx->n_groups = groups;
    ctx->n_parity = parity_count;
    ctx->data = data;
    ctx->parity = parity;
    ctx->frontier = 0;
    ctx->stuck_at = 0;
    ctx->recovered_count = 0;

    return RLNC_OK;
}

int rlnc_encode(struct rlnc_ctx *ctx, uint8_t *data[], unsigned L,
                uint8_t *parity[])
{
    unsigned groups;
    unsigned g;
    unsigned r;
    unsigned i;

    if (!ctx || !data || !parity || L == 0)
    {
        return RLNC_ERR_PARAM;
    }

    for (i = 0; i < L; i++)
    {
        if (!data[i])
        {
            return RLNC_ERR_PARAM;
        }
    }

    groups = group_count(L, ctx->k);
    for (i = 0; i < groups * ctx->m; i++)
    {
        if (!parity[i])
        {
            return RLNC_ERR_PARAM;
        }
    }

    for (g = 0; g < groups; g++)
    {
        int ws = (int)(g * ctx->k) + (int)ctx->k - (int)ctx->w;

        for (r = 0; r < ctx->m; r++)
        {
            uint8_t *dst = parity[(size_t)g * ctx->m + r];
            unsigned nidx;

            memset(dst, 0, ctx->s);
            nidx = rlnc_coeffs(ctx, g, r, ctx->idx, ctx->w);
            for (i = 0; i < nidx; i++)
            {
                int abs = ws + (int)ctx->idx[i];

                if (abs >= 0 && abs < (int)L)
                {
                    xor_into(dst, data[abs], ctx->s);
                }
            }
        }
    }

    return (int)(groups * ctx->m);
}

/* Return the number of recovered packets, or a negative status. */
static int rlnc_recover(struct rlnc_ctx *ctx)
{
    unsigned L = ctx->L;
    unsigned words = (L + 63) / 64;
    unsigned u = 0;
    unsigned e = 0;
    unsigned g;
    unsigned r;
    unsigned i;
    unsigned rank;
    unsigned col;

    for (i = 0; i < L; i++)
    {
        ctx->pos[i] = -1;
        if (!ctx->data_known[i])
        {
            ctx->pos[i] = (int)u;
            ctx->unknown[u++] = i;
        }
    }

    if (u == 0)
    {
        return 0;
    }

    for (g = 0; g < ctx->n_groups; g++)
    {
        int ws = (int)(g * ctx->k) + (int)ctx->k - (int)ctx->w;

        for (r = 0; r < ctx->m; r++)
        {
            unsigned nidx;
            uint64_t *row;
            uint8_t *rhs;

            if (!ctx->parity_known[(size_t)g * ctx->m + r])
            {
                continue;
            }

            row = ctx->mat + (size_t)e * words;
            rhs = ctx->rhs_rows[e];
            memset(row, 0, (size_t)words * sizeof(uint64_t));
            memcpy(rhs, ctx->parity[(size_t)g * ctx->m + r], ctx->s);

            nidx = rlnc_coeffs(ctx, g, r, ctx->idx, ctx->w);
            for (i = 0; i < nidx; i++)
            {
                int abs = ws + (int)ctx->idx[i];
                int p;

                if (abs < 0 || abs >= (int)L)
                {
                    continue;
                }

                p = ctx->pos[abs];
                if (p < 0)
                {
                    xor_into(rhs, ctx->data[abs], ctx->s);
                }
                else
                {
                    row[(unsigned)p >> 6] |= 1ull << (p & 63);
                }
            }

            e++;
        }
    }

    if (e == 0)
    {
        return RLNC_ERR_STUCK;
    }

    rank = 0;
    for (col = 0; col < u && rank < e; col++)
    {
        unsigned p;
        uint64_t bit = 1ull << (col & 63);
        unsigned word = col >> 6;
        unsigned rr;

        for (p = rank; p < e; p++)
        {
            if (ctx->mat[(size_t)p * words + word] & bit)
            {
                break;
            }
        }

        if (p == e)
        {
            continue;
        }

        if (p != rank)
        {
            unsigned k;
            uint8_t *tmp;

            for (k = 0; k < words; k++)
            {
                uint64_t t = ctx->mat[(size_t)rank * words + k];
                ctx->mat[(size_t)rank * words + k] =
                    ctx->mat[(size_t)p * words + k];
                ctx->mat[(size_t)p * words + k] = t;
            }
            tmp = ctx->rhs_rows[rank];
            ctx->rhs_rows[rank] = ctx->rhs_rows[p];
            ctx->rhs_rows[p] = tmp;
        }

        for (rr = 0; rr < e; rr++)
        {
            unsigned k;

            if (rr == rank)
            {
                continue;
            }
            if (!(ctx->mat[(size_t)rr * words + word] & bit))
            {
                continue;
            }

            for (k = 0; k < words; k++)
            {
                ctx->mat[(size_t)rr * words + k] ^=
                    ctx->mat[(size_t)rank * words + k];
            }
            xor_into(ctx->rhs_rows[rr], ctx->rhs_rows[rank], ctx->s);
        }

        ctx->pivot[rank] = col;
        rank++;
    }

    if (rank < u)
    {
        return RLNC_ERR_STUCK;
    }

    for (i = 0; i < u; i++)
    {
        unsigned idx = ctx->unknown[ctx->pivot[i]];

        memcpy(ctx->data[idx], ctx->rhs_rows[i], ctx->s);
        ctx->data_known[idx] = 1;
        ctx->recovered_count++;
    }

    return (int)u;
}

static void update_frontier(struct rlnc_ctx *ctx)
{
    while (ctx->frontier < ctx->L && ctx->data_known[ctx->frontier])
    {
        ctx->frontier++;
    }
    ctx->stuck_at = ctx->frontier;
}

int rlnc_decode(struct rlnc_ctx *ctx, uint8_t *data[], uint8_t *parity[],
                const uint8_t *data_present, const uint8_t *parity_present,
                unsigned L)
{
    unsigned i;
    int rc;

    if (!ctx || !data || !parity || !data_present || !parity_present ||
        L == 0)
    {
        return RLNC_ERR_PARAM;
    }

    rc = rlnc_bind(ctx, data, parity, L);
    if (rc != RLNC_OK)
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

    rc = rlnc_recover(ctx);
    update_frontier(ctx);

    if (rc < 0)
    {
        return rc;
    }

    return rc;
}

int rlnc_advance(struct rlnc_ctx *ctx, int kind, unsigned index)
{
    int rc;

    if (!ctx || ctx->L == 0 || !ctx->data || !ctx->parity)
    {
        return RLNC_ERR_PARAM;
    }

    if (kind == RLNC_KIND_DATA)
    {
        if (index >= ctx->L)
        {
            return RLNC_ERR_PARAM;
        }
        if (ctx->data_known[index])
        {
            return (int)ctx->frontier;
        }
        ctx->data_known[index] = 1;
    }
    else if (kind == RLNC_KIND_PARITY)
    {
        if (index >= ctx->n_parity)
        {
            return RLNC_ERR_PARAM;
        }
        if (ctx->parity_known[index])
        {
            return (int)ctx->frontier;
        }
        ctx->parity_known[index] = 1;
    }
    else
    {
        return RLNC_ERR_PARAM;
    }

    rc = rlnc_recover(ctx);
    update_frontier(ctx);

    if (rc < 0)
    {
        return (int)ctx->frontier;
    }

    return (int)ctx->frontier;
}

unsigned rlnc_frontier(const struct rlnc_ctx *ctx)
{
    return ctx ? ctx->frontier : 0;
}

unsigned rlnc_stuck_at(const struct rlnc_ctx *ctx)
{
    return ctx ? ctx->stuck_at : 0;
}

unsigned rlnc_recovered_packets(const struct rlnc_ctx *ctx)
{
    return ctx ? ctx->recovered_count : 0;
}

int rlnc_is_known(const struct rlnc_ctx *ctx, unsigned index)
{
    if (!ctx || index >= ctx->L)
    {
        return 0;
    }

    return ctx->data_known[index] != 0;
}

uint64_t rlnc_nonce(const struct rlnc_ctx *ctx)
{
    return ctx ? ctx->nonce : 0;
}

unsigned rlnc_retired_groups(const struct rlnc_ctx *ctx)
{
    unsigned g;
    unsigned count = 0;

    if (!ctx || ctx->L == 0)
    {
        return 0;
    }

    for (g = 0; g < ctx->n_groups; g++)
    {
        int we = (int)(g * ctx->k + ctx->k - 1);

        if (we < (int)ctx->frontier)
        {
            count++;
        }
    }

    return count;
}

size_t rlnc_memory_bytes(const struct rlnc_ctx *ctx, unsigned L)
{
    unsigned groups;
    unsigned parity_count;
    unsigned words;
    unsigned min_pu;

    if (!ctx || L == 0)
    {
        return 0;
    }

    groups = group_count(L, ctx->k);
    parity_count = groups * ctx->m;
    words = (L + 63) / 64;
    min_pu = parity_count < L ? parity_count : L;

    return (size_t)parity_count * words * sizeof(uint64_t) +
           (size_t)parity_count * ctx->s +
           (size_t)parity_count * sizeof(uint8_t *) + L + parity_count +
           (size_t)L * sizeof(unsigned) + (size_t)L * sizeof(int) +
           (size_t)min_pu * sizeof(unsigned) +
           (size_t)ctx->w * sizeof(unsigned);
}
