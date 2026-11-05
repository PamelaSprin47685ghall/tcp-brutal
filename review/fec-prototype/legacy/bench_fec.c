#define _POSIX_C_SOURCE 200809L

#include "fec.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

struct bench_arg
{
    struct fec_ctx *ctx;
    uint8_t **data;
    uint8_t **parity;
    uint8_t **shares;
    uint8_t mask[FEC_MASK_BYTES];
    size_t len;
};

static void call_encode(void *p)
{
    struct bench_arg *a = p;

    fec_encode(a->ctx, a->data, a->parity, a->len);
}

static void call_decode(void *p)
{
    struct bench_arg *a = p;

    fec_decode(a->ctx, a->shares, a->mask, a->len);
}

/*
 * Run fn until one batch takes at least 200 ms. Then run three batches and
 * return the median rate in calls per second.
 */
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

        if (t1 - t0 >= 0.2 || iters >= (1u << 26))
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

static void run_case(unsigned n, unsigned m, size_t len)
{
    struct fec_ctx *ctx = fec_new(n, m);
    unsigned total = n + m;
    uint8_t *buf;
    uint8_t *data[FEC_MAX_SHARES];
    uint8_t *parity[FEC_MAX_SHARES];
    uint8_t *shares[FEC_MAX_SHARES];
    struct bench_arg arg;
    double enc_rate;
    double dec_rate;
    double enc_mbps;
    double dec_mbps;
    unsigned i;

    if (!ctx)
    {
        fprintf(stderr, "fec_new(%u,%u) failed\n", n, m);
        exit(2);
    }

    buf = malloc((size_t)total * len);
    if (!buf)
    {
        fprintf(stderr, "out of memory\n");
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
    if (fec_encode(ctx, data, parity, len) != FEC_OK)
    {
        fprintf(stderr, "encode failed\n");
        exit(2);
    }

    memset(&arg, 0, sizeof(arg));
    arg.ctx = ctx;
    arg.data = data;
    arg.parity = parity;
    arg.shares = shares;
    arg.len = len;

    enc_rate = bench_call(call_encode, &arg);
    enc_mbps = enc_rate * (double)n * (double)len / 1e6;

    /*
     * Lose the first m data shares. This is the worst case for this code:
     * the decoder rebuilds m shares from n present shares.
     */
    for (i = 0; i < total; i++)
    {
        arg.mask[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
    for (i = 0; i < m; i++)
    {
        arg.mask[i >> 3] &= (uint8_t)~(1u << (i & 7));
    }

    dec_rate = bench_call(call_decode, &arg);
    dec_mbps = dec_rate * (double)m * (double)len / 1e6;

    printf("%u,%u,%zu,%.1f,%.1f,%u,%u,%u\n", n, m, len, enc_mbps, dec_mbps, m,
           n, m);

    free(buf);
    fec_free(ctx);
}

int main(void)
{
    static const unsigned pairs[][2] = {
        {4, 1},
        {4, 2},
        {8, 2},
    };
    static const size_t lens[] = {1024, 16384, 65536};
    unsigned i;
    unsigned j;

    printf("N,M,share_len,encode_MBps,decode_MBps,encode_XOR_per_byte,"
           "decode_XOR_per_byte,decode_missing_shares\n");

    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
    {
        for (j = 0; j < sizeof(lens) / sizeof(lens[0]); j++)
        {
            run_case(pairs[i][0], pairs[i][1], lens[j]);
        }
    }

    return 0;
}
