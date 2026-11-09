/*
 * tbt_crypto.c - ChaCha8/ChaCha12/ChaCha20 stream core and the tbtun v6
 * body-only share transform.
 *
 * Ported from tiny-chacha by 0xNullll, commit 890a5f0:
 *   https://github.com/0xNullll/tiny-chacha
 *
 * The ChaCha quarter-round, the double-round permutation and the streaming
 * structure (init / process block / xor into output) are adapted from
 * tiny_chacha.c. Changes for tbtun:
 *   - 64-bit block counter (state[12..13]) and 8-byte nonce (state[14..15]),
 *     matching Bernstein's eSTREAM chacha/ref layout and test vectors;
 *   - 128-bit keys use the tau constants from Bernstein's reference; the
 *     original always uses sigma and repeats the key, which does not match
 *     the official 128-bit vectors;
 *   - counter-wrap guard, wipe helper, no bool/XChaCha/feature flags;
 *   - explicit little-endian load/store on every target (the original's
 *     big-endian branch references an undefined SECURE_MEMCPY macro);
 *   - the fixed-round permutation is unrolled and the XOR runs eight bytes at
 *     a time; the session path is ChaCha8 (T0 §2.1);
 *   - the counter space of T0 §2.4, (seq << 8) | (parity << 7) | block, and
 *     the HELLO v6 key helpers.
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

#include "tbt_crypto.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

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

/* One double round: the four column rounds, then the four diagonal rounds.
 * Kept as a macro so a constant round count can be unrolled and the 16 state
 * words stay in registers; the permutation is the whole cost of the cipher. */
#define DOUBLE_ROUND(x)                          \
    do {                                         \
        QUARTER_ROUND(x[0], x[4], x[8], x[12]);  \
        QUARTER_ROUND(x[1], x[5], x[9], x[13]);  \
        QUARTER_ROUND(x[2], x[6], x[10], x[14]); \
        QUARTER_ROUND(x[3], x[7], x[11], x[15]); \
        QUARTER_ROUND(x[0], x[5], x[10], x[15]); \
        QUARTER_ROUND(x[1], x[6], x[11], x[12]); \
        QUARTER_ROUND(x[2], x[7], x[8], x[13]);  \
        QUARTER_ROUND(x[3], x[4], x[9], x[14]);  \
    } while (0)

/* "expand 32-byte k" */
static const u32 tbt_sigma[4] = {0x61707865u, 0x3320646eu, 0x79622d32u,
                                 0x6b206574u};

/* "expand 16-byte k" */
static const u32 tbt_tau[4] = {0x61707865u, 0x3120646eu, 0x79622d36u,
                               0x6b206574u};

static u32 tbt_load32le(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void tbt_store32le(u8 *p, u32 x)
{
    p[0] = (u8)x;
    p[1] = (u8)(x >> 8);
    p[2] = (u8)(x >> 16);
    p[3] = (u8)(x >> 24);
}

/* One 64-byte keystream block. `state` is not modified. */
static void tbt_chacha8_block(const u32 state[16], u8 out[TBT_CRYPTO_BLOCK_LEN],
                              u32 rounds)
{
    u32 x[16];
    u32 i;

    for (i = 0; i < 16; i++)
        x[i] = state[i];

    /* A constant count lets the compiler keep x[] in registers and schedule
     * the dependency chains. The session path always uses 8; 12 and 20 exist
     * so the KAT can pin the shared permutation. */
    if (rounds == 8)
    {
        DOUBLE_ROUND(x);
        DOUBLE_ROUND(x);
        DOUBLE_ROUND(x);
        DOUBLE_ROUND(x);
    }
    else if (rounds == 12)
    {
        DOUBLE_ROUND(x);
        DOUBLE_ROUND(x);
        DOUBLE_ROUND(x);
        DOUBLE_ROUND(x);
        DOUBLE_ROUND(x);
        DOUBLE_ROUND(x);
    }
    else
    {
        for (i = 0; i < rounds; i += 2)
            DOUBLE_ROUND(x);
    }

    for (i = 0; i < 16; i++)
        tbt_store32le(out + 4 * i, x[i] + state[i]);

    /* x[] holds the expanded key schedule; do not leave it on the stack. */
    tbt_memzero_explicit(x, sizeof(x));
}

/* XOR n bytes with a keystream run. Eight bytes at a time through a local
 * u64: the loads happen before the store, so in == out is safe, and the
 * compiler folds each memcpy into one load/store on the wide targets and two
 * on 32-bit ones. */
static void tbt_xor_bytes(const u8 *in, u8 *out, const u8 *ks, size_t n)
{
    size_t j = 0;

    while (j + 8 <= n)
    {
        u64 a;
        u64 b;

        memcpy(&a, in + j, sizeof(a));
        memcpy(&b, ks + j, sizeof(b));
        a ^= b;
        memcpy(out + j, &a, sizeof(a));
        j += 8;
    }
    while (j < n)
    {
        out[j] = in[j] ^ ks[j];
        j++;
    }
}

/* Advance a 64-bit block counter held in two little-endian words. Returns
 * false when the increment wrapped past 2^64-1. */
static bool tbt_counter_inc(u32 st[16])
{
    if (++st[12] != 0)
        return true;
    if (++st[13] != 0)
        return true;
    return false;
}

int tbt_chacha8_init(struct tbt_crypto_ctx *c, const u8 *key, size_t key_len,
                     const u8 nonce[TBT_CRYPTO_NONCE_LEN], u64 counter,
                     int rounds)
{
    const u32 *constants;
    u32 *w;

    if (c == NULL || key == NULL || nonce == NULL)
        return -EINVAL;
    if (key_len != 16u && key_len != 32u)
        return -EINVAL;
    if (rounds != 8 && rounds != 12 && rounds != 20)
        return -EINVAL;

    memset(c, 0, sizeof(*c));
    w = c->state;
    constants = (key_len == 32u) ? tbt_sigma : tbt_tau;

    w[0] = constants[0];
    w[1] = constants[1];
    w[2] = constants[2];
    w[3] = constants[3];

    w[4] = tbt_load32le(key);
    w[5] = tbt_load32le(key + 4);
    w[6] = tbt_load32le(key + 8);
    w[7] = tbt_load32le(key + 12);

    if (key_len == 32u)
    {
        w[8] = tbt_load32le(key + 16);
        w[9] = tbt_load32le(key + 20);
        w[10] = tbt_load32le(key + 24);
        w[11] = tbt_load32le(key + 28);
    }
    else
    {
        /* Bernstein: a 128-bit key is repeated to fill eight words. */
        w[8] = w[4];
        w[9] = w[5];
        w[10] = w[6];
        w[11] = w[7];
    }

    w[12] = (u32)counter;
    w[13] = (u32)(counter >> 32);
    w[14] = tbt_load32le(nonce);
    w[15] = tbt_load32le(nonce + 4);

    memcpy(c->nonce, nonce, TBT_CRYPTO_NONCE_LEN);
    c->counter = counter;
    c->pos = TBT_CRYPTO_BLOCK_LEN; /* no block generated yet */
    c->rounds = (u32)rounds;
    c->ready = true;
    c->wrapped = 0;

    return 0;
}

int tbt_crypto_init(struct tbt_crypto_ctx *c, const u8 key[TBT_CRYPTO_KEY_LEN],
                    const u8 nonce[TBT_CRYPTO_NONCE_LEN], u64 counter)
{
    if (c == NULL)
        return -EINVAL;
    if (key == NULL)
    {
        /* crypto=off: a not-ready context makes body_xor a no-op. */
        memset(c, 0, sizeof(*c));
        c->pos = TBT_CRYPTO_BLOCK_LEN;
        c->ready = false;
        return 0;
    }
    return tbt_chacha8_init(c, key, TBT_CRYPTO_KEY_LEN, nonce, counter,
                            TBT_CRYPTO_ROUNDS);
}

int tbt_crypto_pair_init(struct tbt_crypto_pair *p,
                         const u8 key[TBT_CRYPTO_KEY_LEN],
                         const u8 tx_nonce[TBT_CRYPTO_NONCE_LEN],
                         const u8 rx_nonce[TBT_CRYPTO_NONCE_LEN])
{
    int rc;

    if (p == NULL)
        return -EINVAL;

    memset(p, 0, sizeof(*p));
    p->tx.pos = TBT_CRYPTO_BLOCK_LEN;
    p->rx.pos = TBT_CRYPTO_BLOCK_LEN;

    if (key == NULL)
        return 0; /* crypto=off: both directions are no-ops */

    if (tx_nonce == NULL || rx_nonce == NULL)
        return -EINVAL;
    if (memcmp(tx_nonce, rx_nonce, TBT_CRYPTO_NONCE_LEN) == 0)
        return -EINVAL; /* one nonce for both directions repeats the stream */

    rc = tbt_crypto_init(&p->tx, key, tx_nonce, 0);
    if (rc)
        return rc;
    return tbt_crypto_init(&p->rx, key, rx_nonce, 0);
}

void tbt_crypto_pair_wipe(struct tbt_crypto_pair *p)
{
    if (p == NULL)
        return;

    tbt_crypto_wipe(&p->tx);
    tbt_crypto_wipe(&p->rx);
}

void tbt_crypto_set_counter(struct tbt_crypto_ctx *c, u64 counter)
{
    if (c == NULL)
        return;

    c->state[12] = (u32)counter;
    c->state[13] = (u32)(counter >> 32);
    c->counter = counter;
    c->pos = TBT_CRYPTO_BLOCK_LEN; /* discard the buffered partial block */
    c->wrapped = 0;
}

u64 tbt_crypto_get_counter(const struct tbt_crypto_ctx *c)
{
    return c ? c->counter : 0;
}

/* Generate the block at the context counter and advance both copies of it. */
static int tbt_crypto_next_block(struct tbt_crypto_ctx *c)
{
    if (c->wrapped)
        return -EINVAL;

    tbt_chacha8_block(c->state, c->block, c->rounds);
    c->counter++;
    if (!tbt_counter_inc(c->state))
        c->wrapped = 1;
    c->pos = 0;
    return 0;
}

int tbt_crypto_xor(struct tbt_crypto_ctx *c, u8 *data, size_t len)
{
    size_t i = 0;

    if (c == NULL || !c->ready)
        return -EINVAL;
    if (len == 0)
        return 0;
    if (data == NULL)
        return -EINVAL;

    while (i < len)
    {
        size_t take;

        if (c->pos == TBT_CRYPTO_BLOCK_LEN)
        {
            int rc = tbt_crypto_next_block(c);

            if (rc)
                return rc;
        }

        take = TBT_CRYPTO_BLOCK_LEN - c->pos;
        if (take > len - i)
            take = len - i;

        tbt_xor_bytes(data + i, data + i, c->block + c->pos, take);
        c->pos += (u32)take;
        i += take;
    }

    return 0;
}

int tbt_crypto_xor_at(struct tbt_crypto_ctx *c, u64 counter, u8 *data, size_t len)
{
    u32 st[16];
    u8 block[TBT_CRYPTO_BLOCK_LEN];
    size_t i = 0;

    if (c == NULL || !c->ready)
        return -EINVAL;
    if (len == 0)
        return 0;
    if (data == NULL)
        return -EINVAL;

    /* Copy the schedule, replace only the counter words. The context's
     * buffered block and next-block number are left untouched. */
    memcpy(st, c->state, sizeof(st));
    st[12] = (u32)counter;
    st[13] = (u32)(counter >> 32);

    while (i < len)
    {
        size_t take = len - i;

        if (take > TBT_CRYPTO_BLOCK_LEN)
            take = TBT_CRYPTO_BLOCK_LEN;

        tbt_chacha8_block(st, block, c->rounds);
        tbt_xor_bytes(data + i, data + i, block, take);
        i += take;

        if (i < len && !tbt_counter_inc(st))
        {
            /* The request crosses past block 2^64-1. The output written so
             * far is not a valid stream; the caller must discard it. */
            tbt_memzero_explicit(block, sizeof(block));
            tbt_memzero_explicit(st, sizeof(st));
            return -EINVAL;
        }
    }

    tbt_memzero_explicit(block, sizeof(block));
    tbt_memzero_explicit(st, sizeof(st));
    return 0;
}

int tbt_crypto_body_xor(struct tbt_crypto_ctx *c, u64 seq, bool parity,
                        u8 *payload, u16 len)
{
    if (c == NULL)
        return -EINVAL;
    if (!c->ready)
        return 0; /* crypto=off is a no-op */
    if (len > TBT_CRYPTO_BODY_MAX || seq > TBT_CRYPTO_SEQ_MAX)
        return -EINVAL;
    if (len != 0 && payload == NULL)
        return -EINVAL;

    /* T0 §2.4: (seq << 8) | (parity << 7) | block. The block index is the
     * low 7 bits and advances with the stream; len <= 8192 = 128 blocks.
     * Use xor_at, not set_counter+xor: several carriers parse RX records
     * concurrently and share this context, so the call must not mutate the
     * streaming counter or the buffered block. */
    return tbt_crypto_xor_at(c, (seq << 8) | (parity ? 0x80u : 0u), payload,
                             len);
}

int tbt_crypto_body_xor_share(struct tbt_crypto_ctx *c, u64 seq, bool parity,
                              u16 share_index, u8 *payload, u16 len)
{
    struct tbt_crypto_ctx tmp;
    int rc;

    if (c == NULL)
        return -EINVAL;
    if (!parity)
        return tbt_crypto_body_xor(c, seq, false, payload, len);
    if (!c->ready)
        return 0;
    if (len > TBT_CRYPTO_BODY_MAX || seq > TBT_CRYPTO_SEQ_MAX)
        return -EINVAL;
    if (len != 0 && payload == NULL)
        return -EINVAL;

    /* Copy the key schedule, fold share_index into the nonce word, and use
     * the same counter space as body_xor. xor_at() does not touch c. */
    tmp = *c;
    tmp.state[15] ^= (u32)share_index;
    rc = tbt_crypto_xor_at(&tmp, (seq << 8) | 0x80u, payload, len);
    tbt_memzero_explicit(&tmp, sizeof(tmp));
    return rc;
}

int tbt_crypto_body_xor_flow(struct tbt_crypto_ctx *c, u64 sid, u64 seq,
                             bool parity, u16 share_index, u8 *payload,
                             u16 len)
{
    struct tbt_crypto_ctx tmp;
    int rc;

    if (c == NULL)
        return -EINVAL;
    if (!c->ready)
        return 0;
    if (len > TBT_CRYPTO_BODY_MAX || seq > TBT_CRYPTO_SEQ_MAX)
        return -EINVAL;
    if (len != 0 && payload == NULL)
        return -EINVAL;
    /* sid == 0 is the old single-stream domain: keep it byte-for-byte
     * identical so a non-multi-stream caller does not change its ciphertext. */
    if (!sid)
        return tbt_crypto_body_xor_share(c, seq, parity, share_index, payload,
                                         len);

    /* The nonce words are state[14] (low 32 bits) and state[15] (high 8 bits
     * of the 40-bit sid, plus the parity share index). A 40-bit sid is
     * injective into these 40 bits, so two flows never share a keystream. */
    tmp = *c;
    tmp.state[14] ^= (u32)sid;
    tmp.state[15] ^= (u32)(sid >> 32);
    if (parity)
        tmp.state[15] ^= (u32)share_index;
    rc = tbt_crypto_xor_at(&tmp, (seq << 8) | (parity ? 0x80u : 0u),
                           payload, len);
    tbt_memzero_explicit(&tmp, sizeof(tmp));
    return rc;
}

void tbt_crypto_wipe(struct tbt_crypto_ctx *c)
{
    if (c == NULL)
        return;

    tbt_memzero_explicit(c, sizeof(*c));
}

int tbt_chacha8_xor(const u8 *key, size_t key_len,
                    const u8 nonce[TBT_CRYPTO_NONCE_LEN], u64 counter,
                    int rounds, u8 *data, size_t len)
{
    struct tbt_crypto_ctx ctx;
    int rc;

    if (tbt_chacha8_init(&ctx, key, key_len, nonce, counter, rounds) != 0)
        return -EINVAL;

    rc = tbt_crypto_xor(&ctx, data, len);
    tbt_crypto_wipe(&ctx);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* HELLO v6 crypto fields (T0 §2.3).                                        */

void tbt_crypto_hello_fill(u8 hello[TBT_CRYPTO_HELLO_LEN], bool enabled,
                           u8 key_id,
                           const u8 crypto_nonce[TBT_CRYPTO_NONCE_LEN],
                           const u8 key[TBT_CRYPTO_KEY_LEN])
{
    if (hello == NULL)
        return;

    hello[TBT_CRYPTO_HELLO_CRYPTO] =
        (u8)((enabled ? TBT_CRYPTO_HELLO_ENABLED : 0) |
             (key_id == TBT_CRYPTO_KEY_ID_HELLO ? TBT_CRYPTO_HELLO_KEY_IN_HELLO : 0));
    hello[TBT_CRYPTO_HELLO_KEY_ID] = key_id;
    hello[TBT_CRYPTO_HELLO_RESERVED] = 0;

    if (crypto_nonce != NULL)
        memcpy(hello + TBT_CRYPTO_HELLO_NONCE, crypto_nonce,
               TBT_CRYPTO_NONCE_LEN);
    else
        memset(hello + TBT_CRYPTO_HELLO_NONCE, 0, TBT_CRYPTO_NONCE_LEN);

    if (key_id == TBT_CRYPTO_KEY_ID_HELLO && key != NULL)
        memcpy(hello + TBT_CRYPTO_HELLO_KEY, key, TBT_CRYPTO_KEY_LEN);
    else
        memset(hello + TBT_CRYPTO_HELLO_KEY, 0, TBT_CRYPTO_KEY_LEN);
}

int tbt_crypto_hello_parse(const u8 *hello, size_t len, bool *enabled,
                           u8 *key_id,
                           u8 crypto_nonce[TBT_CRYPTO_NONCE_LEN],
                           u8 key[TBT_CRYPTO_KEY_LEN])
{
    u8 crypto;
    u8 id;

    if (hello == NULL || len < TBT_CRYPTO_HELLO_LEN)
        return -EPROTO;
    if (hello[TBT_CRYPTO_HELLO_RESERVED] != 0)
        return -EPROTO;

    crypto = hello[TBT_CRYPTO_HELLO_CRYPTO];
    if (crypto & (u8)~(TBT_CRYPTO_HELLO_ENABLED | TBT_CRYPTO_HELLO_KEY_IN_HELLO))
        return -EPROTO;

    id = hello[TBT_CRYPTO_HELLO_KEY_ID];
    if (id != TBT_CRYPTO_KEY_ID_NONE && id != TBT_CRYPTO_KEY_ID_HELLO)
        return -EPROTO;
    if ((id == TBT_CRYPTO_KEY_ID_HELLO) !=
        ((crypto & TBT_CRYPTO_HELLO_KEY_IN_HELLO) != 0))
        return -EPROTO;

    if (enabled != NULL)
        *enabled = (crypto & TBT_CRYPTO_HELLO_ENABLED) != 0;
    if (key_id != NULL)
        *key_id = id;
    if (crypto_nonce != NULL)
        memcpy(crypto_nonce, hello + TBT_CRYPTO_HELLO_NONCE,
               TBT_CRYPTO_NONCE_LEN);
    if (key != NULL)
    {
        if (id == TBT_CRYPTO_KEY_ID_HELLO)
            memcpy(key, hello + TBT_CRYPTO_HELLO_KEY, TBT_CRYPTO_KEY_LEN);
        else
            memset(key, 0, TBT_CRYPTO_KEY_LEN);
    }

    return 0;
}

int tbt_crypto_resolve_key(bool local_key_wins,
                           const u8 local_key[TBT_CRYPTO_KEY_LEN],
                           bool enabled, u8 key_id,
                           const u8 hello_key[TBT_CRYPTO_KEY_LEN],
                           u8 out[TBT_CRYPTO_KEY_LEN])
{
    if (out == NULL)
        return -EINVAL;

    if (!enabled)
    {
        memset(out, 0, TBT_CRYPTO_KEY_LEN);
        return 0;
    }

    if (local_key_wins)
    {
        /* Our key is the session key: configured on both ends, or generated
         * here and announced in our own HELLO. The peer learned it from us,
         * so its HELLO names key_id 0; a peer that generated one instead has
         * a different key, so refuse. */
        if (local_key == NULL || key_id != TBT_CRYPTO_KEY_ID_NONE)
            return -EPROTO;
        memcpy(out, local_key, TBT_CRYPTO_KEY_LEN);
        return 0;
    }

    /* The key is the peer's: take the HELLO key (TOFU). */
    if (hello_key == NULL || key_id != TBT_CRYPTO_KEY_ID_HELLO)
        return -EPROTO;
    memcpy(out, hello_key, TBT_CRYPTO_KEY_LEN);
    return 0;
}
