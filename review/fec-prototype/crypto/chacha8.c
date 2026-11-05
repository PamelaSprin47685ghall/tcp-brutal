/*
 * chacha8.c - ChaCha8/ChaCha12/ChaCha20 stream core for the FEC prototype.
 *
 * Ported from tiny-chacha by 0xNullll, commit 890a5f0:
 *   https://github.com/0xNullll/tiny-chacha
 *
 * The ChaCha quarter-round, the double-round permutation and the streaming
 * structure (init / process block / xor into output) are adapted from
 * tiny_chacha.c. Changes for this prototype:
 *   - 64-bit block counter (state[12..13]) and 8-byte nonce (state[14..15]),
 *     matching Bernstein's eSTREAM chacha/ref layout and test vectors;
 *   - 128-bit keys use the tau constants from Bernstein's reference; the
 *     original always uses sigma and repeats the key, which does not match
 *     the official 128-bit vectors;
 *   - counter-wrap guard, wipe helper, no bool/XChaCha/feature flags;
 *   - explicit little-endian load/store on every target (the original's
 *     big-endian branch references an undefined SECURE_MEMCPY macro).
 *
 * The permutation itself is unchanged from the original.
 *
 * ---------------------------------------------------------------------------
 * tiny-chacha MIT licence:
 *
 * MIT License
 *
 * Copyright (c) 2026 0xNullll
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 * ---------------------------------------------------------------------------
 *
 * The 128-bit key schedule and the test-vector semantics follow D. J.
 * Bernstein's public-domain eSTREAM chacha/ref implementation
 * (chacha-ref.c version 20080118).
 */

#include "chacha8.h"

#include <string.h>

#define QUARTER_ROUND(a, b, c, d)      \
    do {                               \
        (a) += (b);                    \
        (d) ^= (a);                    \
        (d) = ROTL32((d), 16);         \
        (c) += (d);                    \
        (b) ^= (c);                    \
        (b) = ROTL32((b), 12);         \
        (a) += (b);                    \
        (d) ^= (a);                    \
        (d) = ROTL32((d), 8);          \
        (c) += (d);                    \
        (b) ^= (c);                    \
        (b) = ROTL32((b), 7);          \
    } while (0)

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* "expand 32-byte k" */
static const uint32_t sigma[4] = {
    0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u
};

/* "expand 16-byte k" */
static const uint32_t tau[4] = {
    0x61707865u, 0x3120646eu, 0x79622d36u, 0x6b206574u
};

static uint32_t load32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void store32le(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
}

/*
 * One 64-byte keystream block. state is not modified; the caller advances
 * the counter after the block has been generated.
 */
static void chacha8_block(const uint32_t state[16], uint8_t out[64],
                          int rounds)
{
    uint32_t x[16];
    int i;

    for (i = 0; i < 16; i++)
        x[i] = state[i];

    for (i = 0; i < rounds; i += 2) {
        QUARTER_ROUND(x[0], x[4], x[8], x[12]);
        QUARTER_ROUND(x[1], x[5], x[9], x[13]);
        QUARTER_ROUND(x[2], x[6], x[10], x[14]);
        QUARTER_ROUND(x[3], x[7], x[11], x[15]);

        QUARTER_ROUND(x[0], x[5], x[10], x[15]);
        QUARTER_ROUND(x[1], x[6], x[11], x[12]);
        QUARTER_ROUND(x[2], x[7], x[8], x[13]);
        QUARTER_ROUND(x[3], x[4], x[9], x[14]);
    }

    for (i = 0; i < 16; i++)
        store32le(out + 4 * i, x[i] + state[i]);
}

int chacha8_init(chacha8_ctx *ctx,
                 const uint8_t *key, size_t key_len,
                 const uint8_t nonce[CHACHA8_NONCE_SIZE],
                 uint64_t counter, int rounds)
{
    const uint32_t *constants;
    uint32_t *w;

    if (ctx == NULL || key == NULL || nonce == NULL)
        return -1;
    if (key_len != CHACHA8_KEY_SIZE_128 && key_len != CHACHA8_KEY_SIZE_256)
        return -1;
    if (rounds != CHACHA8_ROUNDS_8 && rounds != CHACHA8_ROUNDS_12
        && rounds != CHACHA8_ROUNDS_20)
        return -1;

    w = ctx->state;
    constants = (key_len == CHACHA8_KEY_SIZE_256) ? sigma : tau;

    w[0] = constants[0];
    w[1] = constants[1];
    w[2] = constants[2];
    w[3] = constants[3];

    w[4] = load32le(key);
    w[5] = load32le(key + 4);
    w[6] = load32le(key + 8);
    w[7] = load32le(key + 12);

    if (key_len == CHACHA8_KEY_SIZE_256) {
        w[8] = load32le(key + 16);
        w[9] = load32le(key + 20);
        w[10] = load32le(key + 24);
        w[11] = load32le(key + 28);
    } else {
        /* Bernstein: a 128-bit key is repeated to fill eight words. */
        w[8] = w[4];
        w[9] = w[5];
        w[10] = w[6];
        w[11] = w[7];
    }

    w[12] = (uint32_t)counter;
    w[13] = (uint32_t)(counter >> 32);
    w[14] = load32le(nonce);
    w[15] = load32le(nonce + 4);

    ctx->pos = CHACHA8_BLOCK_SIZE;
    ctx->rounds = rounds;
    ctx->wrapped = 0;
    memset(ctx->block, 0, sizeof(ctx->block));

    return 0;
}

int chacha8_crypt(chacha8_ctx *ctx, const uint8_t *in, size_t len,
                  uint8_t *out)
{
    size_t i = 0;

    if (ctx == NULL)
        return -1;
    if (len == 0)
        return 0;
    if (in == NULL || out == NULL)
        return -1;

    while (i < len) {
        size_t take;
        size_t j;

        if (ctx->pos == CHACHA8_BLOCK_SIZE) {
            if (ctx->wrapped)
                return -1;

            chacha8_block(ctx->state, ctx->block, ctx->rounds);

            /*
             * Advance the 64-bit counter. The high word only moves when
             * the low word wraps. If both wrap, the next block would
             * reuse counter zero with the same key and nonce.
             */
            if (++ctx->state[12] == 0 && ++ctx->state[13] == 0)
                ctx->wrapped = 1;

            ctx->pos = 0;
        }

        take = CHACHA8_BLOCK_SIZE - ctx->pos;
        if (take > len - i)
            take = len - i;

        for (j = 0; j < take; j++)
            out[i + j] = in[i + j] ^ ctx->block[ctx->pos + j];

        ctx->pos += (uint32_t)take;
        i += take;
    }

    return 0;
}

int chacha8_xor(const uint8_t *key, size_t key_len,
                const uint8_t nonce[CHACHA8_NONCE_SIZE],
                uint64_t counter, int rounds,
                const uint8_t *in, size_t len, uint8_t *out)
{
    chacha8_ctx ctx;
    int rc;

    if (chacha8_init(&ctx, key, key_len, nonce, counter, rounds) != 0)
        return -1;

    rc = chacha8_crypt(&ctx, in, len, out);
    chacha8_wipe(&ctx);
    return rc;
}

void chacha8_wipe(chacha8_ctx *ctx)
{
    volatile uint8_t *p = (volatile uint8_t *)ctx;
    size_t i;

    if (ctx == NULL)
        return;

    for (i = 0; i < sizeof(*ctx); i++)
        p[i] = 0;
}
