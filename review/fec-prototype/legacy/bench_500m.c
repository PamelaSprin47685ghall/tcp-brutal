#define _POSIX_C_SOURCE 200809L

#include "fec.h"
#include "sliding.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TARGET_MBPS 125.0

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

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

static double bench_call(void (*fn)(void *), void *arg, unsigned *calls)
{
    unsigned iters = 1;
    unsigned i;
    unsigned r;
    double t0;
    double t1;
    double rates[3];
    double tmp;

    fn(arg);

    for (;;)
    {
        t0 = now_sec();
        for (i = 0; i < iters; i++)
        {
            fn(arg);
        }
        t1 = now_sec();

        if (t1 - t0 >= 0.2 || iters >= (1u << 24))
        {
            break;
        }

        iters *= 2;
    }

    for (r = 0; r < 3; r++)
    {
        t0 = now_sec();
        for (i = 0; i < iters; i++)
        {
            fn(arg);
        }
        t1 = now_sec();
        rates[r] = (double)iters / (t1 - t0);
    }

    if (rates[0] > rates[1])
    {
        tmp = rates[0];
        rates[0] = rates[1];
        rates[1] = tmp;
    }
    if (rates[1] > rates[2])
    {
        tmp = rates[1];
        rates[1] = rates[2];
        rates[2] = tmp;
    }
    if (rates[0] > rates[1])
    {
        tmp = rates[0];
        rates[0] = rates[1];
        rates[1] = tmp;
    }

    *calls = iters;
    return rates[1];
}

static double core_pct(double mbps)
{
    return mbps > 0.0 ? TARGET_MBPS / mbps * 100.0 : 0.0;
}

struct sliding_arg
{
    struct sliding_ctx *ctx;
    uint8_t **data;
    uint8_t **parity;
    uint8_t *dp;
    uint8_t *pp;
    unsigned L;
};

static void call_sliding_encode(void *p)
{
    struct sliding_arg *a = p;

    sliding_encode(a->ctx, a->data, a->L, a->parity, NULL);
}

static void call_sliding_decode(void *p)
{
    struct sliding_arg *a = p;

    sliding_decode(a->ctx, a->data, a->parity, a->dp, a->pp, a->L);
}

static void run_sliding_case(unsigned n, unsigned m, unsigned k, unsigned w,
                             unsigned q, enum sliding_scheme scheme)
{
    unsigned L = 2048;
    size_t S = 8192;
    unsigned G = L / k + (L % k != 0);
    unsigned P = G * m;
    struct sliding_ctx *ctx = sliding_new(n, m, k, w, S, scheme);
    uint8_t *dbuf;
    uint8_t *pbuf;
    uint8_t **data;
    uint8_t **parity;
    uint8_t *dp;
    uint8_t *pp;
    struct sliding_arg arg;
    unsigned i;
    unsigned calls;
    double enc_rate;
    double dec1_rate;
    double dec2_rate;
    double enc_mbps;
    double dec1_mbps;
    double dec2_mbps;
    unsigned enc_ops;
    unsigned dec1_ops;
    unsigned dec2_ops;
    unsigned losses;

    if (!ctx)
    {
        printf("%s,%u,%u,%u,%u,%u,rejected,-,-,-,-,-,-,-,-,-,-,-,-,invalid "
               "parameters\n",
               scheme == SLIDING_L0_XOR      ? "L0"
               : scheme == SLIDING_L1_XOR_PART ? "L1"
               : scheme == SLIDING_L2_RS4      ? "L2"
                                                : "L3",
               n, m, k, w, q);
        return;
    }

    dbuf = malloc((size_t)L * S);
    pbuf = malloc((size_t)P * S);
    data = malloc((size_t)L * sizeof(uint8_t *));
    parity = malloc((size_t)P * sizeof(uint8_t *));
    dp = malloc(L);
    pp = malloc(P);
    if (!dbuf || !pbuf || !data || !parity || !dp || !pp)
    {
        exit(2);
    }

    for (i = 0; i < L; i++)
    {
        data[i] = dbuf + (size_t)i * S;
    }
    for (i = 0; i < P; i++)
    {
        parity[i] = pbuf + (size_t)i * S;
    }

    fill_random(dbuf, (size_t)L * S);
    sliding_encode(ctx, data, L, parity, NULL);

    memset(&arg, 0, sizeof(arg));
    arg.ctx = ctx;
    arg.data = data;
    arg.parity = parity;
    arg.dp = dp;
    arg.pp = pp;
    arg.L = L;

    enc_rate = bench_call(call_sliding_encode, &arg, &calls);
    enc_mbps = enc_rate * (double)L * (double)S / 1e6;

    /* One loss. */
    memset(dp, 1, L);
    memset(pp, 1, P);
    dp[0] = 0;
    memset(data[0], 0xa5, S);
    dec1_rate = bench_call(call_sliding_decode, &arg, &calls);
    dec1_mbps = dec1_rate * (double)S / 1e6;

    /* Two losses. L0 cannot recover two losses in one window. */
    losses = m >= 2 ? 2 : 0;
    if (losses == 2)
    {
        memset(dp, 1, L);
        memset(pp, 1, P);
        dp[0] = 0;
        dp[1] = 0;
        memset(data[0], 0xa5, S);
        memset(data[1], 0xa5, S);
        dec2_rate = bench_call(call_sliding_decode, &arg, &calls);
        dec2_mbps = dec2_rate * 2.0 * (double)S / 1e6;
    }
    else
    {
        dec2_rate = 0.0;
        dec2_mbps = 0.0;
    }

    enc_ops = scheme == SLIDING_L2_RS4 || scheme == SLIDING_L3_RS8 ? m : 1;
    if (scheme == SLIDING_L1_XOR_PART)
    {
        dec1_ops = (w + m - 1) / m;
        dec2_ops = (w + m - 1) / m;
    }
    else
    {
        dec1_ops = w;
        dec2_ops = w;
    }

    printf("%s,%u,%u,%u,%u,%u,%u,%zu,%.1f,%.1f,%.1f,%u,%u,%u,%.1f,%.1f,%.1f\n",
           scheme == SLIDING_L0_XOR      ? "L0"
           : scheme == SLIDING_L1_XOR_PART ? "L1"
           : scheme == SLIDING_L2_RS4      ? "L2"
                                            : "L3",
           n, m, k, w, q, L, S, enc_mbps, dec1_mbps, dec2_mbps, enc_ops,
           dec1_ops, dec2_ops, core_pct(enc_mbps), core_pct(dec1_mbps),
           losses ? core_pct(dec2_mbps) : 0.0);

    free(dbuf);
    free(pbuf);
    free(data);
    free(parity);
    free(dp);
    free(pp);
    sliding_free(ctx);
}

struct block_arg
{
    struct fec_ctx *ctx;
    uint8_t **data;
    uint8_t **parity;
    uint8_t **shares;
    uint8_t mask[FEC_MASK_BYTES];
    size_t len;
};

static void call_block_encode(void *p)
{
    struct block_arg *a = p;

    fec_encode(a->ctx, a->data, a->parity, a->len);
}

static void call_block_decode(void *p)
{
    struct block_arg *a = p;

    fec_decode(a->ctx, a->shares, a->mask, a->len);
}

static void run_block_case(unsigned n, unsigned m, size_t len, unsigned lost,
                           double init_ms)
{
    struct fec_ctx *ctx = fec_new(n, m);
    unsigned total = n + m;
    uint8_t *buf;
    uint8_t **data;
    uint8_t **parity;
    uint8_t **shares;
    struct block_arg arg;
    unsigned i;
    unsigned calls;
    double enc_rate;
    double dec_rate;
    double enc_mbps;
    double dec_mbps;
    unsigned ops_enc = m;
    unsigned ops_dec = n;

    if (!ctx)
    {
        printf("block,%u,%u,%zu,%u,rejected,-,-,-,-,-,-,invalid parameters\n",
               n, m, len, lost);
        return;
    }

    buf = malloc((size_t)total * len);
    data = malloc((size_t)n * sizeof(uint8_t *));
    parity = malloc((size_t)m * sizeof(uint8_t *));
    shares = malloc((size_t)total * sizeof(uint8_t *));
    if (!buf || !data || !parity || !shares)
    {
        exit(2);
    }

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
    fec_encode(ctx, data, parity, len);

    memset(&arg, 0, sizeof(arg));
    arg.ctx = ctx;
    arg.data = data;
    arg.parity = parity;
    arg.shares = shares;
    arg.len = len;
    for (i = 0; i < total; i++)
    {
        arg.mask[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
    for (i = 0; i < lost; i++)
    {
        arg.mask[i >> 3] &= (uint8_t)~(1u << (i & 7));
        memset(shares[i], 0xa5, len);
    }

    enc_rate = bench_call(call_block_encode, &arg, &calls);
    enc_mbps = enc_rate * (double)n * (double)len / 1e6;

    dec_rate = bench_call(call_block_decode, &arg, &calls);
    dec_mbps = dec_rate * (double)lost * (double)len / 1e6;

    printf("block,%u,%u,%zu,%u,%zu,%.1f,%.1f,%u,%u,%.1f,%.1f,%.1f\n", n, m,
           len, lost, len, enc_mbps, dec_mbps, ops_enc, ops_dec,
           core_pct(enc_mbps), core_pct(dec_mbps), init_ms);

    free(buf);
    free(data);
    free(parity);
    free(shares);
    fec_free(ctx);
}

int main(void)
{
    static const struct
    {
        unsigned n, m, k, w, q;
        enum sliding_scheme scheme;
    } cases[] = {
        {4, 1, 8, 8, 1, SLIDING_L0_XOR},
        {4, 1, 8, 16, 2, SLIDING_L0_XOR},
        {4, 2, 8, 8, 1, SLIDING_L1_XOR_PART},
        {4, 2, 8, 16, 2, SLIDING_L1_XOR_PART},
        {4, 2, 8, 8, 1, SLIDING_L2_RS4},
        {4, 2, 4, 8, 2, SLIDING_L2_RS4},
        {4, 2, 8, 8, 1, SLIDING_L3_RS8},
        {4, 2, 8, 16, 2, SLIDING_L3_RS8},
    };
    unsigned i;

    printf("scheme,n,m,k,w,q,L,S,encode_MBps,decode1_MBps,decode2_MBps,"
           "encode_ops_per_byte,decode1_ops_per_byte,decode2_ops_per_byte,"
           "encode_core_pct,decode1_core_pct,decode2_core_pct\n");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        run_sliding_case(cases[i].n, cases[i].m, cases[i].k, cases[i].w,
                         cases[i].q, cases[i].scheme);
    }

    printf("block,n,m,share_len,lost,share_len,encode_MBps,decode_MBps,"
           "encode_ops_per_byte,decode_ops_per_byte,encode_core_pct,"
           "decode_core_pct,init_ms\n");

    {
        double t0 = now_sec();
        struct fec_ctx *ctx = fec_new(96, 32);

        fec_free(ctx);
        run_block_case(96, 32, 8192, 1, (now_sec() - t0) * 1e3);
    }
    {
        double t0 = now_sec();
        struct fec_ctx *ctx = fec_new(16, 4);

        fec_free(ctx);
        run_block_case(16, 4, 8192, 1, (now_sec() - t0) * 1e3);
    }

    return 0;
}
