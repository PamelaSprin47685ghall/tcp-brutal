#define _POSIX_C_SOURCE 200809L

#include "rlnc.h"

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

static double bench_call(void (*fn)(void *), void *arg)
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
        if (t1 - t0 >= 0.2 || iters >= (1u << 22))
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

    return rates[1];
}

struct rlnc_arg
{
    struct rlnc_ctx *ctx;
    uint8_t **data;
    uint8_t **parity;
    uint8_t *dp;
    uint8_t *pp;
    unsigned L;
};

static void call_rlnc_encode(void *p)
{
    struct rlnc_arg *a = p;

    rlnc_encode(a->ctx, a->data, a->L, a->parity);
}

static void call_rlnc_decode(void *p)
{
    struct rlnc_arg *a = p;

    rlnc_decode(a->ctx, a->data, a->parity, a->dp, a->pp, a->L);
}

static void run_rlnc(unsigned n, unsigned m, unsigned k, unsigned w,
                     unsigned d, size_t S, unsigned L)
{
    unsigned G = L / k + (L % k != 0);
    unsigned P = G * m;
    struct rlnc_ctx *ctx =
        rlnc_new(n, m, k, w, S, d, 0x123456789abcdefull);
    uint8_t *dbuf;
    uint8_t *pbuf;
    uint8_t **data;
    uint8_t **parity;
    uint8_t *dp;
    uint8_t *pp;
    struct rlnc_arg arg;
    unsigned i;
    unsigned losses[] = {1, 8, 32};
    double enc_mbps;
    double dec_mbps[3];
    unsigned li;
    double enc_ops = (double)m * d / (double)k;

    if (!ctx)
    {
        printf("rlnc,%u,%u,%u,%u,%u,rejected,-,-,-,-,-,-,-,invalid\n", n, m,
               k, w, d);
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
    rlnc_encode(ctx, data, L, parity);

    memset(&arg, 0, sizeof(arg));
    arg.ctx = ctx;
    arg.data = data;
    arg.parity = parity;
    arg.dp = dp;
    arg.pp = pp;
    arg.L = L;

    enc_mbps = bench_call(call_rlnc_encode, &arg) * (double)L * (double)S / 1e6;

    for (li = 0; li < 3; li++)
    {
        unsigned lost = losses[li];
        unsigned j;

        memset(dp, 1, L);
        memset(pp, 1, P);
        for (j = 0; j < lost && j < L; j++)
        {
            dp[j] = 0;
            memset(data[j], 0xa5, S);
        }
        dec_mbps[li] = bench_call(call_rlnc_decode, &arg) *
                       (double)lost * (double)S / 1e6;
    }

    printf("rlnc,%u,%u,%u,%u,%u,%zu,%u,%.1f,%.1f,%.1f,%.1f,%.2f,%.1f\n", n,
           m, k, w, d, S, L, enc_mbps, dec_mbps[0], dec_mbps[1],
           dec_mbps[2], enc_ops, enc_mbps > 0 ? TARGET_MBPS / enc_mbps * 100.0
                                              : 0.0);

    free(dbuf);
    free(pbuf);
    free(data);
    free(parity);
    free(dp);
    free(pp);
    rlnc_free(ctx);
}

int main(void)
{
    printf("scheme,n,m,k,w,d,S,L,encode_MBps,decode1_MBps,decode8_MBps,"
           "decode32_MBps,encode_ops_per_byte,encode_core_pct\n");

    /* production shape: N+M=256, K=N, W=2K, d=16 */
    run_rlnc(224, 32, 224, 448, 16, 8192, 512);
    run_rlnc(200, 56, 200, 400, 16, 8192, 512);
    run_rlnc(192, 64, 192, 384, 16, 8192, 512);
    run_rlnc(16, 4, 16, 32, 16, 8192, 256);

    return 0;
}
