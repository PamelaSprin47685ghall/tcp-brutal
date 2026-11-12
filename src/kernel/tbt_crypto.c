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
 *   - the fixed-round permutation is unrolled and a complete block is XORed
 *     straight into the payload, so only a partial tail buffers keystream;
 *     the session path is ChaCha8 (T0 §2.1);
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
#if defined(CONFIG_X86_64)
#include <asm/fpu/api.h>
#endif
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

/* 8 and 12 are unrolled because the session path and the KAT pin them; any
 * other even count runs the loop. The branch costs one test per call. */
#define TBT_CHACHA_ROUNDS(x, rounds)          \
    do {                                      \
        int rest_ = (int)(rounds);            \
        if (rest_ == 8)                       \
        {                                     \
            DOUBLE_ROUND(x);                  \
            DOUBLE_ROUND(x);                  \
            DOUBLE_ROUND(x);                  \
            DOUBLE_ROUND(x);                  \
        }                                     \
        else if (rest_ == 12)                 \
        {                                     \
            DOUBLE_ROUND(x);                  \
            DOUBLE_ROUND(x);                  \
            DOUBLE_ROUND(x);                  \
            DOUBLE_ROUND(x);                  \
            DOUBLE_ROUND(x);                  \
            DOUBLE_ROUND(x);                  \
        }                                     \
        else                                  \
        {                                     \
            for (rest_ = 0; rest_ < (int)(rounds); rest_ += 2) \
                DOUBLE_ROUND(x);              \
        }                                     \
    } while (0)

/* "expand 32-byte k" */
static const u32 tbt_sigma[4] = {0x61707865u, 0x3320646eu, 0x79622d32u,
                                 0x6b206574u};

/* "expand 16-byte k" */
static const u32 tbt_tau[4] = {0x61707865u, 0x3120646eu, 0x79622d36u,
                               0x6b206574u};

/* One load/store instead of four byte accesses. The kernel and every target
 * this module supports are little-endian; the shift form stays for a
 * big-endian host build, where a plain memcpy would swap the word. */
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
static u32 tbt_load32le(const u8 *p)
{
    u32 v;

    memcpy(&v, p, sizeof(v));
    return v;
}

static void tbt_store32le(u8 *p, u32 x)
{
    memcpy(p, &x, sizeof(x));
}

static u64 tbt_load64le(const u8 *p)
{
    u64 v;

    memcpy(&v, p, sizeof(v));
    return v;
}

static void tbt_store64le(u8 *p, u64 x)
{
    memcpy(p, &x, sizeof(x));
}
#else
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

static u64 tbt_load64le(const u8 *p)
{
    return (u64)tbt_load32le(p) | ((u64)tbt_load32le(p + 4) << 32);
}

static void tbt_store64le(u8 *p, u64 x)
{
    tbt_store32le(p, (u32)x);
    tbt_store32le(p + 4, (u32)(x >> 32));
}
#endif

/* Skip -ftrivial-auto-var-init for the two bulk scratch arrays: every element
 * is assigned before it is read, and the zero-init call blocks SLP
 * vectorisation of the column rounds. */
#if defined(__GNUC__) && (__GNUC__ >= 12) && !defined(__clang__)
#define TBT_CHACHA_UNINITIALIZED __attribute__((uninitialized))
#else
#define TBT_CHACHA_UNINITIALIZED
#endif

/* One 64-byte keystream block written to `out`. `state` is not modified. */
static void tbt_chacha8_block(const u32 state[16], u8 out[TBT_CRYPTO_BLOCK_LEN],
                              u32 rounds)
{
    u32 x[16];
    u32 i;

    for (i = 0; i < 16; i++)
        x[i] = state[i];

    TBT_CHACHA_ROUNDS(x, rounds);

    for (i = 0; i < 16; i++)
        tbt_store32le(out + 4 * i, x[i] + state[i]);

    /* x[] holds the expanded key schedule; do not leave it on the stack. */
    tbt_memzero_explicit(x, sizeof(x));
}

/* The portable bulk body. x86_64 takes the explicit four-block SSE2 path
 * below and uses this only for a tail shorter than four blocks; other
 * architectures run it for the whole share.
 *
 * `blocks` consecutive keystream blocks XORed straight into `data`, advancing
 * the counter in `st` past them. Folding the final add and the XOR into the
 * permutation keeps the keystream off the stack and removes the store/load
 * round trip through `out`. The caller guarantees that every block lies at or
 * below 2^64-1.
 *
 * The counter lives in a local u64 for the whole run; advancing it in st[]
 * between blocks puts a read-modify-write in the loop's critical path.
 *
 * x[] and base[] are one frame slot each, reused by every block, so wiping
 * them once after the loop erases the only copy that outlives the run. */
#define TBT_CHACHA8_BULK(st, data, blocks, rounds)                    \
    do {                                                              \
        TBT_CHACHA_UNINITIALIZED u32 x[16];                           \
        TBT_CHACHA_UNINITIALIZED u32 base[16];                        \
        u64 ctr = (u64)(st)[12] | ((u64)(st)[13] << 32);              \
        size_t b;                                                     \
        u32 i;                                                        \
                                                                      \
        for (i = 0; i < 16; i++)                                      \
            base[i] = (st)[i];                                        \
        for (b = 0; b < (blocks); b++)                                \
        {                                                             \
            base[12] = (u32)ctr;                                      \
            base[13] = (u32)(ctr >> 32);                              \
            ctr++;                                                    \
            for (i = 0; i < 16; i++)                                  \
                x[i] = base[i];                                       \
            TBT_CHACHA_ROUNDS(x, rounds);                             \
            for (i = 0; i < 16; i++)                                  \
            {                                                         \
                u8 *p = (data) + b * TBT_CRYPTO_BLOCK_LEN + 4 * i;    \
                                                                      \
                tbt_store32le(p, tbt_load32le(p) ^ (x[i] + base[i])); \
            }                                                         \
        }                                                             \
        (st)[12] = (u32)ctr;                                          \
        (st)[13] = (u32)(ctr >> 32);                                  \
        tbt_memzero_explicit(x, sizeof(x));                           \
        tbt_memzero_explicit(base, sizeof(base));                     \
    } while (0)

static void tbt_chacha8_blocks_xor_scalar(u32 st[16], u8 *data, size_t blocks,
                                          u32 rounds)
{
    TBT_CHACHA8_BULK(st, data, blocks, rounds);
}

/* Four-block interleaved SSE2. Each block keeps its four state rows in four
 * 128-bit vectors, so one quarter-round advances the four columns of a block
 * at once and the diagonal round only rotates the lanes of B, C and D. Four
 * blocks give the scheduler independent chains to overlap.
 *
 * This is an explicit vector form, not a hint to the SLP pass: the kernel
 * builds with -mno-sse, and -ftrivial-auto-var-init turns the scalar scratch
 * arrays into .DEFERRED_INIT calls that stop SLP from vectorising them. The
 * target attribute re-enables SSE for this function only; the dispatcher owns
 * kernel_fpu_begin()/kernel_fpu_end(). Measured on the x86_64 test host:
 * 774 MB/s for one 8 KiB share against 370 MB/s for the scalar -mno-sse
 * build. */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define TBT_CHACHA_SSE2 1

#ifdef __KERNEL__
#define TBT_CHACHA_NOINLINE noinline
#else
#define TBT_CHACHA_NOINLINE __attribute__((noinline))
#endif

typedef u32 tbt_chacha_v4u32 __attribute__((vector_size(16)));
typedef u8 tbt_chacha_v16u8 __attribute__((vector_size(16)));

#define TBT_CHACHA_V4(a, b, c, d) \
    ((tbt_chacha_v4u32){ (a), (b), (c), (d) })

#define TBT_CHACHA_TARGET __attribute__((target("sse2")))

TBT_CHACHA_TARGET static inline tbt_chacha_v4u32
tbt_chacha_vrotl(tbt_chacha_v4u32 x, u32 n)
{
    return (x << n) | (x >> (32 - n));
}

/* Lane rotations for the diagonal round. */
TBT_CHACHA_TARGET static inline tbt_chacha_v4u32
tbt_chacha_vsh1(tbt_chacha_v4u32 x)
{
    return __builtin_shufflevector(x, x, 1, 2, 3, 0);
}

TBT_CHACHA_TARGET static inline tbt_chacha_v4u32
tbt_chacha_vsh2(tbt_chacha_v4u32 x)
{
    return __builtin_shufflevector(x, x, 2, 3, 0, 1);
}

TBT_CHACHA_TARGET static inline tbt_chacha_v4u32
tbt_chacha_vsh3(tbt_chacha_v4u32 x)
{
    return __builtin_shufflevector(x, x, 3, 0, 1, 2);
}

#define TBT_CHACHA_VQR(a, b, c, d)            \
    do {                                       \
        (a) += (b);                            \
        (d) ^= (a);                            \
        (d) = tbt_chacha_vrotl((d), 16);       \
        (c) += (d);                            \
        (b) ^= (c);                            \
        (b) = tbt_chacha_vrotl((b), 12);       \
        (a) += (b);                            \
        (d) ^= (a);                            \
        (d) = tbt_chacha_vrotl((d), 8);        \
        (c) += (d);                            \
        (b) ^= (c);                            \
        (b) = tbt_chacha_vrotl((b), 7);        \
    } while (0)

#define TBT_CHACHA_VDOUBLE(a, b, c, d)  \
    do {                                 \
        TBT_CHACHA_VQR(a, b, c, d);      \
        (b) = tbt_chacha_vsh1(b);        \
        (c) = tbt_chacha_vsh2(c);        \
        (d) = tbt_chacha_vsh3(d);        \
        TBT_CHACHA_VQR(a, b, c, d);      \
        (b) = tbt_chacha_vsh3(b);        \
        (c) = tbt_chacha_vsh2(c);        \
        (d) = tbt_chacha_vsh1(d);        \
    } while (0)

TBT_CHACHA_TARGET TBT_CHACHA_NOINLINE
static void tbt_chacha8_blocks_xor_sse2(u32 st[16], u8 *data, size_t blocks,
                                        u32 rounds)
{
    tbt_chacha_v4u32 ba = TBT_CHACHA_V4(st[0], st[1], st[2], st[3]);
    tbt_chacha_v4u32 bb = TBT_CHACHA_V4(st[4], st[5], st[6], st[7]);
    tbt_chacha_v4u32 bc = TBT_CHACHA_V4(st[8], st[9], st[10], st[11]);
    tbt_chacha_v4u32 A[4], B[4], C[4], D[4];
    u64 ctr = (u64)st[12] | ((u64)st[13] << 32);
    size_t g;
    u32 l;

    for (g = 0; g < blocks; g += 4)
    {
        u32 r;

        for (l = 0; l < 4; l++)
        {
            u64 c = ctr + l;

            A[l] = ba;
            B[l] = bb;
            C[l] = bc;
            D[l] = TBT_CHACHA_V4((u32)c, (u32)(c >> 32), st[14], st[15]);
        }
        for (r = 0; r < rounds / 2; r++)
            for (l = 0; l < 4; l++)
                TBT_CHACHA_VDOUBLE(A[l], B[l], C[l], D[l]);
        for (l = 0; l < 4; l++)
        {
            u64 c = ctr + l;
            u8 *p = data + (g + l) * TBT_CRYPTO_BLOCK_LEN;
            tbt_chacha_v4u32 o;
            tbt_chacha_v16u8 q;

            o = A[l] + ba;
            memcpy(&q, p, 16);
            q ^= (tbt_chacha_v16u8)o;
            memcpy(p, &q, 16);
            o = B[l] + bb;
            memcpy(&q, p + 16, 16);
            q ^= (tbt_chacha_v16u8)o;
            memcpy(p + 16, &q, 16);
            o = C[l] + bc;
            memcpy(&q, p + 32, 16);
            q ^= (tbt_chacha_v16u8)o;
            memcpy(p + 32, &q, 16);
            o = D[l] + TBT_CHACHA_V4((u32)c, (u32)(c >> 32), st[14], st[15]);
            memcpy(&q, p + 48, 16);
            q ^= (tbt_chacha_v16u8)o;
            memcpy(p + 48, &q, 16);
        }
        ctr += 4;
    }
    st[12] = (u32)ctr;
    st[13] = (u32)(ctr >> 32);
    tbt_memzero_explicit(A, sizeof(A));
    tbt_memzero_explicit(B, sizeof(B));
    tbt_memzero_explicit(C, sizeof(C));
    tbt_memzero_explicit(D, sizeof(D));
    tbt_memzero_explicit(&ba, sizeof(ba));
    tbt_memzero_explicit(&bb, sizeof(bb));
    tbt_memzero_explicit(&bc, sizeof(bc));
}
#endif

/* Four-block interleaved NEON for the user-space aarch64 client. The shape
 * follows the SSE2 path line for line: each lane holds one block's four
 * state rows in four 128-bit vectors, so one quarter-round advances all four
 * columns of a block and the diagonal round only rotates the lanes of B, C
 * and D. Four blocks give the core independent chains to overlap.
 *
 * NEON is baseline on every AArch64 target, so this path needs no run-time
 * dispatch and no target attribute. The kernel build must never see
 * <arm_neon.h>: the guard keeps this block out of it, and the kernel keeps
 * the scalar path.
 *
 * The vector loads and stores move whole words, so this body would emit a
 * host-endian keystream on a big-endian ABI while the scalar body always
 * stores little-endian words. Keep that ABI on the scalar path, the same
 * rule the CRC32X block follows. */
#if !defined(__KERNEL__) && defined(__aarch64__) && defined(__ARM_NEON) && \
    defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && \
    (defined(__GNUC__) || defined(__clang__))
#define TBT_CHACHA_NEON 1

#include <arm_neon.h>

typedef uint32x4_t tbt_chacha_v4u32;

/* Rotate each 32-bit lane left by a compile-time count. */
#define TBT_CHACHA_NEON_ROTL(x, n) \
    vorrq_u32(vshlq_n_u32((x), (n)), vshrq_n_u32((x), 32 - (n)))

#define TBT_CHACHA_NEON_VQR(a, b, c, d)              \
    do {                                             \
        (a) = vaddq_u32((a), (b));                   \
        (d) = veorq_u32((d), (a));                   \
        (d) = TBT_CHACHA_NEON_ROTL((d), 16);         \
        (c) = vaddq_u32((c), (d));                   \
        (b) = veorq_u32((b), (c));                   \
        (b) = TBT_CHACHA_NEON_ROTL((b), 12);         \
        (a) = vaddq_u32((a), (b));                   \
        (d) = veorq_u32((d), (a));                   \
        (d) = TBT_CHACHA_NEON_ROTL((d), 8);          \
        (c) = vaddq_u32((c), (d));                   \
        (b) = veorq_u32((b), (c));                   \
        (b) = TBT_CHACHA_NEON_ROTL((b), 7);          \
    } while (0)

/* Lane rotations for the diagonal round. vextq_u32(x, x, k) yields lanes
 * k..3,0..k-1, the same permutation as the SSE2 shuffle helpers. */
#define TBT_CHACHA_NEON_VDOUBLE(a, b, c, d)  \
    do {                                     \
        TBT_CHACHA_NEON_VQR(a, b, c, d);     \
        (b) = vextq_u32((b), (b), 1);        \
        (c) = vextq_u32((c), (c), 2);        \
        (d) = vextq_u32((d), (d), 3);        \
        TBT_CHACHA_NEON_VQR(a, b, c, d);     \
        (b) = vextq_u32((b), (b), 3);        \
        (c) = vextq_u32((c), (c), 2);        \
        (d) = vextq_u32((d), (d), 1);        \
    } while (0)

/* State row D of block ctr + n: [counter low, counter high, nonce low,
 * nonce high]. bd already holds the last two lanes. */
static inline uint32x4_t tbt_chacha_neon_dvec(uint32x4_t bd, u64 c)
{
    uint32x4_t v = vsetq_lane_u32((u32)c, bd, 0);

    return vsetq_lane_u32((u32)(c >> 32), v, 1);
}

/* XOR one 128-bit keystream word into the payload at p. */
static inline void tbt_chacha_neon_xor16(u8 *p, uint32x4_t o)
{
    vst1q_u8(p, vreinterpretq_u8_u32(
                    veorq_u32(vreinterpretq_u32_u8(vld1q_u8(p)), o)));
}

__attribute__((noinline)) static void
tbt_chacha8_blocks_xor_neon(u32 st[16], u8 *data, size_t blocks, u32 rounds)
{
    uint32x4_t ba = vld1q_u32(st);
    uint32x4_t bb = vld1q_u32(st + 4);
    uint32x4_t bc = vld1q_u32(st + 8);
    uint32x4_t bd = vld1q_u32(st + 12);
    uint32x4_t A[4], B[4], C[4], D[4];
    u64 ctr = (u64)st[12] | ((u64)st[13] << 32);
    size_t g;
    u32 l;

    for (g = 0; g < blocks; g += 4)
    {
        u32 r;

        for (l = 0; l < 4; l++)
        {
            u64 c = ctr + l;

            A[l] = ba;
            B[l] = bb;
            C[l] = bc;
            D[l] = tbt_chacha_neon_dvec(bd, c);
        }
        for (r = 0; r < rounds / 2; r++)
            for (l = 0; l < 4; l++)
                TBT_CHACHA_NEON_VDOUBLE(A[l], B[l], C[l], D[l]);
        for (l = 0; l < 4; l++)
        {
            u64 c = ctr + l;
            u8 *p = data + (g + l) * TBT_CRYPTO_BLOCK_LEN;
            uint32x4_t o;

            o = vaddq_u32(A[l], ba);
            tbt_chacha_neon_xor16(p, o);
            o = vaddq_u32(B[l], bb);
            tbt_chacha_neon_xor16(p + 16, o);
            o = vaddq_u32(C[l], bc);
            tbt_chacha_neon_xor16(p + 32, o);
            o = vaddq_u32(D[l], tbt_chacha_neon_dvec(bd, c));
            tbt_chacha_neon_xor16(p + 48, o);
        }
        ctr += 4;
    }
    st[12] = (u32)ctr;
    st[13] = (u32)(ctr >> 32);
    tbt_memzero_explicit(A, sizeof(A));
    tbt_memzero_explicit(B, sizeof(B));
    tbt_memzero_explicit(C, sizeof(C));
    tbt_memzero_explicit(D, sizeof(D));
    tbt_memzero_explicit(&ba, sizeof(ba));
    tbt_memzero_explicit(&bb, sizeof(bb));
    tbt_memzero_explicit(&bc, sizeof(bc));
    tbt_memzero_explicit(&bd, sizeof(bd));
}
#endif

static void tbt_chacha8_blocks_xor(u32 st[16], u8 *data, size_t blocks,
                                   u32 rounds)
{
    if (blocks == 0)
        return;
#if defined(TBT_CHACHA_SSE2) || defined(TBT_CHACHA_NEON)
    {
        size_t n = blocks & ~(size_t)3;

#if defined(TBT_CHACHA_SSE2) && defined(__KERNEL__)
        /* kernel_fpu_begin() may only run where the live FPU state can be
         * saved. A hardirq or softirq can interrupt a task inside its own
         * FPU section; using the vector body there corrupts state the
         * interrupted context owns. Fall back to scalar ChaCha8 when
         * irq_fpu_usable() says this context cannot take the FPU. */
        if (n && !irq_fpu_usable())
            n = 0;
#endif
        if (n)
        {
#ifdef __KERNEL__
            kernel_fpu_begin();
#endif
#ifdef TBT_CHACHA_SSE2
            tbt_chacha8_blocks_xor_sse2(st, data, n, rounds);
#else
            tbt_chacha8_blocks_xor_neon(st, data, n, rounds);
#endif
#ifdef __KERNEL__
            kernel_fpu_end();
#endif
        }
        if (n != blocks)
            tbt_chacha8_blocks_xor_scalar(st, data + n * TBT_CRYPTO_BLOCK_LEN,
                                          blocks - n, rounds);
    }
#else
    tbt_chacha8_blocks_xor_scalar(st, data, blocks, rounds);
#endif
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

/* Generate whole blocks at the context counter straight into `data` and
 * advance both copies of the counter. `blocks` may be clamped to the counter
 * space; `done` receives the number written. Returns 0, or -EINVAL once the
 * 64-bit counter has wrapped. Nothing is buffered. */
static int tbt_crypto_run_blocks(struct tbt_crypto_ctx *c, u8 *data,
                                 size_t blocks, size_t *done)
{
    u64 ctr = (u64)c->state[12] | ((u64)c->state[13] << 32);
    /* Blocks up to and including 2^64-1. From counter 0 that is 2^64, which
     * does not fit; a size_t byte count cannot reach it, so saturate. */
    u64 room = ctr ? ~ctr + 1 : ~(u64)0;
    size_t n = blocks;

    if (c->wrapped)
        return -EINVAL;
    if ((u64)n > room)
        n = (size_t)room;

    tbt_chacha8_blocks_xor(c->state, data, n, c->rounds);
    c->counter += (u64)n;
    if (n != 0 && ctr + (u64)n == 0)
        c->wrapped = 1; /* the run ended on block 2^64-1 */
    *done = n;
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
            int rc;

            /* Whole blocks go straight into the caller's buffer; a shorter
             * request takes the buffered-block path below. */
            if (len - i >= TBT_CRYPTO_BLOCK_LEN)
            {
                size_t done = 0;

                rc = tbt_crypto_run_blocks(c, data + i,
                                           (len - i) / TBT_CRYPTO_BLOCK_LEN,
                                           &done);
                if (rc)
                    return rc;
                i += done * TBT_CRYPTO_BLOCK_LEN;
                continue;
            }

            rc = tbt_crypto_next_block(c);
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

/* Absolute-counter XOR over `st`. The caller owns the schedule copy and
 * wipes it; this function only needs the round count and the counter words.
 * A whole block goes straight into the payload; only the tail needs the
 * keystream buffer, so the wrap report lands on the same byte. */
static int tbt_crypto_xor_state(u32 st[16], u32 rounds, u8 *data, size_t len)
{
    u8 block[TBT_CRYPTO_BLOCK_LEN];
    size_t i = 0;
    int rc = 0;
    bool block_used = false;

    while (i < len)
    {
        size_t take = len - i;

        if (take >= TBT_CRYPTO_BLOCK_LEN)
        {
            u64 ctr = (u64)st[12] | ((u64)st[13] << 32);
            /* Blocks up to and including 2^64-1; saturate from counter 0. */
            u64 room = ctr ? ~ctr + 1 : ~(u64)0;
            size_t blocks = take / TBT_CRYPTO_BLOCK_LEN;

            if ((u64)blocks > room)
                blocks = (size_t)room;

            tbt_chacha8_blocks_xor(st, data + i, blocks, rounds);
            i += blocks * TBT_CRYPTO_BLOCK_LEN;
            if (i < len && ctr + (u64)blocks == 0)
            {
                /* The request crosses past block 2^64-1. The output written
                 * so far is not a valid stream; the caller must discard it. */
                rc = -EINVAL;
                goto out;
            }
            continue;
        }

        tbt_chacha8_block(st, block, rounds);
        block_used = true;
        tbt_xor_bytes(data + i, data + i, block, take);
        i += take;
        /* i == len here: the tail consumed the rest. No counter advance. */
    }

out:
    /* block is the last keystream and may not be left on the stack, on the
     * success path or the wrap path. A whole-block-only run never generated
     * it, so do not pay 64 stores for an uninitialised buffer. */
    if (block_used)
        tbt_memzero_explicit(block, sizeof(block));
    return rc;
}

int tbt_crypto_xor_at(struct tbt_crypto_ctx *c, u64 counter, u8 *data, size_t len)
{
    u32 st[16];
    int rc;

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

    rc = tbt_crypto_xor_state(st, c->rounds, data, len);
    tbt_memzero_explicit(st, sizeof(st));
    return rc;
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
    u32 st[16];
    u64 ctr;
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

    /* Copy only the schedule, fold share_index into the nonce word, and use
     * the same counter space as body_xor. c is read-only here. */
    memcpy(st, c->state, sizeof(st));
    st[15] ^= (u32)share_index;
    ctr = (seq << 8) | 0x80u;
    st[12] = (u32)ctr;
    st[13] = (u32)(ctr >> 32);
    rc = tbt_crypto_xor_state(st, c->rounds, payload, len);
    tbt_memzero_explicit(st, sizeof(st));
    return rc;
}

int tbt_crypto_body_xor_flow(struct tbt_crypto_ctx *c, u64 sid, u64 seq,
                             bool parity, u16 share_index, u8 *payload,
                             u16 len)
{
    u32 st[16];
    u64 ctr;
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
    if (sid > TBT_CRYPTO_SID_MAX)
        return -EINVAL;

    /* The nonce words are state[14] (sid bits 0..31) and state[15]. The
     * low 8 bits of state[15] carry sid bits 32..39, exactly as before. For
     * PARITY, share_index occupies bits 8..23 and bit 31 is a domain marker,
     * so (sid_hi, share_index) is injective and a nonzero-sid parity stream
     * can never collide with the sid==0 share domain (share_index is in
     * bits 0..15 there). DATA keeps the old mapping: its seq already names
     * the share, and the parity counter bit separates it from PARITY. */
    memcpy(st, c->state, sizeof(st));
    st[14] ^= (u32)sid;
    st[15] ^= (u32)(sid >> 32) & 0xffu;
    if (parity)
        st[15] ^= ((u32)share_index << 8) | 0x80000000u;
    ctr = (seq << 8) | (parity ? 0x80u : 0u);
    st[12] = (u32)ctr;
    st[13] = (u32)(ctr >> 32);
    rc = tbt_crypto_xor_state(st, c->rounds, payload, len);
    tbt_memzero_explicit(st, sizeof(st));
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

/* ------------------------------------------------------------------------- */
/* HELLO v9/v10 authenticated generation.                                   */

static u64 tbt_rotl64(u64 x, unsigned int n)
{
    return (x << n) | (x >> (64 - n));
}

/* One SipHash round (Aumasson & Bernstein, "SipHash: a fast short-input
 * PRF", the 2-4 variant). */
static void tbt_sip_round(u64 v[4])
{
    v[0] += v[1];
    v[1] = tbt_rotl64(v[1], 13);
    v[1] ^= v[0];
    v[0] = tbt_rotl64(v[0], 32);
    v[2] += v[3];
    v[3] = tbt_rotl64(v[3], 16);
    v[3] ^= v[2];
    v[0] += v[3];
    v[3] = tbt_rotl64(v[3], 21);
    v[3] ^= v[0];
    v[2] += v[1];
    v[1] = tbt_rotl64(v[1], 17);
    v[1] ^= v[2];
    v[2] = tbt_rotl64(v[2], 32);
}

u64 tbt_siphash24(const u8 key[16], const u8 *msg, size_t len)
{
    u64 k0 = tbt_load64le(key);
    u64 k1 = tbt_load64le(key + 8);
    u64 v[4];
    u64 b;
    u64 out;
    size_t i = 0;

    v[0] = k0 ^ 0x736f6d6570736575ULL;
    v[1] = k1 ^ 0x646f72616e646f6dULL;
    v[2] = k0 ^ 0x6c7967656e657261ULL;
    v[3] = k1 ^ 0x7465646279746573ULL;

    for (; i + 8 <= len; i += 8)
    {
        u64 m = tbt_load64le(msg + i);

        v[3] ^= m;
        tbt_sip_round(v);
        tbt_sip_round(v);
        v[0] ^= m;
    }

    /* Final block: the remaining bytes, little-endian, under a length byte in
     * the top octet. Bytes above the real length stay zero. */
    b = (u64)(len & 0xff) << 56;
    {
        size_t rem = len - i;

        while (rem)
        {
            rem--;
            b |= (u64)msg[i + rem] << (8 * rem);
        }
    }
    v[3] ^= b;
    tbt_sip_round(v);
    tbt_sip_round(v);
    v[0] ^= b;
    v[2] ^= 0xff;
    tbt_sip_round(v);
    tbt_sip_round(v);
    tbt_sip_round(v);
    tbt_sip_round(v);

    out = v[0] ^ v[1] ^ v[2] ^ v[3];
    tbt_memzero_explicit(v, sizeof(v));
    tbt_memzero_explicit(&k0, sizeof(k0));
    tbt_memzero_explicit(&k1, sizeof(k1));
    tbt_memzero_explicit(&b, sizeof(b));
    return out;
}

/* 128 tag bits from two SipHash keys: the two 128-bit halves of the 256-bit
 * PSK are independent, so an attacker has to forge both halves. No key
 * derivation and no second primitive, so there is nothing to get wrong.
 *
 * The MAC input is path_id || hello[0..73]. path_id is in the clear outer
 * record header, outside the HELLO body, so leaving it out let a captured
 * body be replayed under any other carrier index. The fixed one-byte prefix
 * keeps the binding unambiguous and does not change the body layout. */
static void tbt_hello_tag_path(const u8 *hello, u8 path_id,
                               const u8 key[TBT_CRYPTO_KEY_LEN],
                               u8 out[TBT_CRYPTO_HELLO_TAG_LEN])
{
    u8 msg[1 + TBT_CRYPTO_HELLO_TAG];

    msg[0] = path_id;
    memcpy(msg + 1, hello, TBT_CRYPTO_HELLO_TAG);
    tbt_store64le(out, tbt_siphash24(key, msg, sizeof(msg)));
    tbt_store64le(out + 8,
                  tbt_siphash24(key + 16, msg, sizeof(msg)));
    tbt_memzero_explicit(msg, sizeof(msg));
}

void tbt_crypto_hello_auth_fill_path(u8 *hello, size_t len, u64 epoch,
                                     const u8 key[TBT_CRYPTO_KEY_LEN],
                                     u8 path_id)
{
    if (hello == NULL || len < TBT_CRYPTO_HELLO_AUTH_LEN)
        return;

    tbt_store64le(hello + TBT_CRYPTO_HELLO_EPOCH, epoch);
    if (key != NULL)
        tbt_hello_tag_path(hello, path_id, key,
                           hello + TBT_CRYPTO_HELLO_TAG);
    else
        memset(hello + TBT_CRYPTO_HELLO_TAG, 0, TBT_CRYPTO_HELLO_TAG_LEN);
}

void tbt_crypto_hello_auth_fill(u8 hello[TBT_CRYPTO_HELLO_AUTH_LEN], u64 epoch,
                                const u8 key[TBT_CRYPTO_KEY_LEN])
{
    tbt_crypto_hello_auth_fill_path(hello, TBT_CRYPTO_HELLO_AUTH_LEN, epoch,
                                    key, 0);
}

bool tbt_crypto_hello_auth_ok_path(const u8 *hello, size_t len,
                                   const u8 key[TBT_CRYPTO_KEY_LEN],
                                   u8 path_id)
{
    u8 want[TBT_CRYPTO_HELLO_TAG_LEN];
    u8 diff = 0;
    u8 nonzero = 0;
    size_t i;

    if (hello == NULL || key == NULL || len < TBT_CRYPTO_HELLO_AUTH_LEN)
        return false;

    tbt_hello_tag_path(hello, path_id, key, want);
    for (i = 0; i < TBT_CRYPTO_HELLO_TAG_LEN; i++)
    {
        nonzero |= hello[TBT_CRYPTO_HELLO_TAG + i];
        diff |= (u8)(hello[TBT_CRYPTO_HELLO_TAG + i] ^ want[i]);
    }
    tbt_memzero_explicit(want, sizeof(want));
    /* A zero tag means "no PSK was configured on the sender"; it must never
     * compare equal to a computed tag, even if a key somehow produced one.
     * The check is folded into the constant-time result, not an early exit. */
    return diff == 0 && nonzero != 0;
}

bool tbt_crypto_hello_auth_ok(const u8 *hello, size_t len,
                              const u8 key[TBT_CRYPTO_KEY_LEN])
{
    return tbt_crypto_hello_auth_ok_path(hello, len, key, 0);
}
