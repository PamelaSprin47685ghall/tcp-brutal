/* tbt_fec.c - FEC wire v1 and the GF(2) sliding-window RLNC codec.
 *
 * The host test links this file directly; the module links the same object.
 * Keep it free of stdio, libc allocation, and kernel-only globals so both
 * builds stay honest. See tbt_fec.h for the contract.
 */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/crc32c.h>
#else
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#endif

#include "tbt_fec.h"

#ifdef __KERNEL__
#define tbt_fec_lock(l) spin_lock_bh(l)
#define tbt_fec_unlock(l) spin_unlock_bh(l)
#else
#define tbt_fec_lock(l) ((void)(l))
#define tbt_fec_unlock(l) ((void)(l))
#endif

/* The v8 wire lengths and extension offsets are a contract, not a comment.
 * A body that drifts from this block fails the build on both sides. The
 * kernel half lives in tbt_fec_hdr_check(): BUILD_BUG_ON is a statement. */
#ifndef __KERNEL__
_Static_assert(TBT_FEC_HDR_LEN == 48, "FEC header must stay 48 bytes");
_Static_assert(TBT_FEC_CFG_LEN == 64, "FEC_CFG body must be 64 bytes");
_Static_assert(TBT_FEC_FBK_LEN == 64, "FEC_FBK body must be 64 bytes");
_Static_assert(TBT_FEC_CFG_CRC_EXT == TBT_FEC_CFG_CODEC_VARIANT + 8,
               "FEC_CFG extension CRC follows the eight extension bytes");
_Static_assert(TBT_FEC_CFG_RESERVED2 == 60, "FEC_CFG reserved2 at +60");
_Static_assert(TBT_FEC_FBK_P_FAST_PPM == 48, "FBK p_fast at +48");
_Static_assert(TBT_FEC_FBK_RATE_TARGET == 60, "FBK rate target at +60");
#endif

/* ------------------------------------------------------------------ *
 * Allocation
 * ------------------------------------------------------------------ */

#ifndef __KERNEL__
static long tbt_fec_live_allocs;
static long tbt_fec_host_fail_after = -1;

long tbt_fec_host_live_allocs(void)
{
    return tbt_fec_live_allocs;
}

void tbt_fec_host_alloc_fail_after(long n)
{
    tbt_fec_host_fail_after = n;
}
#endif

static void *tbt_fec_alloc(size_t len)
{
    void *p;

#ifndef __KERNEL__
    if (tbt_fec_host_fail_after >= 0)
    {
        if (!tbt_fec_host_fail_after)
        {
            tbt_fec_host_fail_after = -1; /* fail exactly once */
            return NULL;
        }
        tbt_fec_host_fail_after--;
    }
#endif
    p = tbt_fec_vzalloc(len);

#ifndef __KERNEL__
    if (p)
        tbt_fec_live_allocs++;
#endif
    return p;
}

static void tbt_fec_free(void *p)
{
    if (!p)
        return;
#ifdef __KERNEL__
    kvfree(p);
#else
    tbt_fec_live_allocs--;
    free(p);
#endif
}

/* ------------------------------------------------------------------ *
 * CRC32C (Castagnoli). Standard reflected form: init ~0, final xor.
 * ------------------------------------------------------------------ */

#ifdef __KERNEL__

u32 tbt_fec_crc32c(u32 seed, const void *data, size_t len)
{
    if (!len)
        return seed;
    return crc32c(~seed, data, len) ^ ~0u;
}

#else

/* Portable fallback: one reflected Castagnoli lookup per byte.
 *
 * The table is compile-time constant. The host half is a library API: a
 * caller may run it from several threads, and a lazily filled table with a
 * plain "ready" flag is a C data race (a reader can observe the flag before
 * the stores that fill the table on a weakly ordered CPU, and two threads
 * may run the filler at once). A const table has no init state at all, so a
 * concurrent first call is already safe and the hot path loses a branch. */
static const u32 tbt_crc32c_table[256] = {
    0x00000000u, 0xf26b8303u, 0xe13b70f7u, 0x1350f3f4u,
    0xc79a971fu, 0x35f1141cu, 0x26a1e7e8u, 0xd4ca64ebu,
    0x8ad958cfu, 0x78b2dbccu, 0x6be22838u, 0x9989ab3bu,
    0x4d43cfd0u, 0xbf284cd3u, 0xac78bf27u, 0x5e133c24u,
    0x105ec76fu, 0xe235446cu, 0xf165b798u, 0x030e349bu,
    0xd7c45070u, 0x25afd373u, 0x36ff2087u, 0xc494a384u,
    0x9a879fa0u, 0x68ec1ca3u, 0x7bbcef57u, 0x89d76c54u,
    0x5d1d08bfu, 0xaf768bbcu, 0xbc267848u, 0x4e4dfb4bu,
    0x20bd8edeu, 0xd2d60dddu, 0xc186fe29u, 0x33ed7d2au,
    0xe72719c1u, 0x154c9ac2u, 0x061c6936u, 0xf477ea35u,
    0xaa64d611u, 0x580f5512u, 0x4b5fa6e6u, 0xb93425e5u,
    0x6dfe410eu, 0x9f95c20du, 0x8cc531f9u, 0x7eaeb2fau,
    0x30e349b1u, 0xc288cab2u, 0xd1d83946u, 0x23b3ba45u,
    0xf779deaeu, 0x05125dadu, 0x1642ae59u, 0xe4292d5au,
    0xba3a117eu, 0x4851927du, 0x5b016189u, 0xa96ae28au,
    0x7da08661u, 0x8fcb0562u, 0x9c9bf696u, 0x6ef07595u,
    0x417b1dbcu, 0xb3109ebfu, 0xa0406d4bu, 0x522bee48u,
    0x86e18aa3u, 0x748a09a0u, 0x67dafa54u, 0x95b17957u,
    0xcba24573u, 0x39c9c670u, 0x2a993584u, 0xd8f2b687u,
    0x0c38d26cu, 0xfe53516fu, 0xed03a29bu, 0x1f682198u,
    0x5125dad3u, 0xa34e59d0u, 0xb01eaa24u, 0x42752927u,
    0x96bf4dccu, 0x64d4cecfu, 0x77843d3bu, 0x85efbe38u,
    0xdbfc821cu, 0x2997011fu, 0x3ac7f2ebu, 0xc8ac71e8u,
    0x1c661503u, 0xee0d9600u, 0xfd5d65f4u, 0x0f36e6f7u,
    0x61c69362u, 0x93ad1061u, 0x80fde395u, 0x72966096u,
    0xa65c047du, 0x5437877eu, 0x4767748au, 0xb50cf789u,
    0xeb1fcbadu, 0x197448aeu, 0x0a24bb5au, 0xf84f3859u,
    0x2c855cb2u, 0xdeeedfb1u, 0xcdbe2c45u, 0x3fd5af46u,
    0x7198540du, 0x83f3d70eu, 0x90a324fau, 0x62c8a7f9u,
    0xb602c312u, 0x44694011u, 0x5739b3e5u, 0xa55230e6u,
    0xfb410cc2u, 0x092a8fc1u, 0x1a7a7c35u, 0xe811ff36u,
    0x3cdb9bddu, 0xceb018deu, 0xdde0eb2au, 0x2f8b6829u,
    0x82f63b78u, 0x709db87bu, 0x63cd4b8fu, 0x91a6c88cu,
    0x456cac67u, 0xb7072f64u, 0xa457dc90u, 0x563c5f93u,
    0x082f63b7u, 0xfa44e0b4u, 0xe9141340u, 0x1b7f9043u,
    0xcfb5f4a8u, 0x3dde77abu, 0x2e8e845fu, 0xdce5075cu,
    0x92a8fc17u, 0x60c37f14u, 0x73938ce0u, 0x81f80fe3u,
    0x55326b08u, 0xa759e80bu, 0xb4091bffu, 0x466298fcu,
    0x1871a4d8u, 0xea1a27dbu, 0xf94ad42fu, 0x0b21572cu,
    0xdfeb33c7u, 0x2d80b0c4u, 0x3ed04330u, 0xccbbc033u,
    0xa24bb5a6u, 0x502036a5u, 0x4370c551u, 0xb11b4652u,
    0x65d122b9u, 0x97baa1bau, 0x84ea524eu, 0x7681d14du,
    0x2892ed69u, 0xdaf96e6au, 0xc9a99d9eu, 0x3bc21e9du,
    0xef087a76u, 0x1d63f975u, 0x0e330a81u, 0xfc588982u,
    0xb21572c9u, 0x407ef1cau, 0x532e023eu, 0xa145813du,
    0x758fe5d6u, 0x87e466d5u, 0x94b49521u, 0x66df1622u,
    0x38cc2a06u, 0xcaa7a905u, 0xd9f75af1u, 0x2b9cd9f2u,
    0xff56bd19u, 0x0d3d3e1au, 0x1e6dcdeeu, 0xec064eedu,
    0xc38d26c4u, 0x31e6a5c7u, 0x22b65633u, 0xd0ddd530u,
    0x0417b1dbu, 0xf67c32d8u, 0xe52cc12cu, 0x1747422fu,
    0x49547e0bu, 0xbb3ffd08u, 0xa86f0efcu, 0x5a048dffu,
    0x8ecee914u, 0x7ca56a17u, 0x6ff599e3u, 0x9d9e1ae0u,
    0xd3d3e1abu, 0x21b862a8u, 0x32e8915cu, 0xc083125fu,
    0x144976b4u, 0xe622f5b7u, 0xf5720643u, 0x07198540u,
    0x590ab964u, 0xab613a67u, 0xb831c993u, 0x4a5a4a90u,
    0x9e902e7bu, 0x6cfbad78u, 0x7fab5e8cu, 0x8dc0dd8fu,
    0xe330a81au, 0x115b2b19u, 0x020bd8edu, 0xf0605beeu,
    0x24aa3f05u, 0xd6c1bc06u, 0xc5914ff2u, 0x37faccf1u,
    0x69e9f0d5u, 0x9b8273d6u, 0x88d28022u, 0x7ab90321u,
    0xae7367cau, 0x5c18e4c9u, 0x4f48173du, 0xbd23943eu,
    0xf36e6f75u, 0x0105ec76u, 0x12551f82u, 0xe03e9c81u,
    0x34f4f86au, 0xc69f7b69u, 0xd5cf889du, 0x27a40b9eu,
    0x79b737bau, 0x8bdcb4b9u, 0x988c474du, 0x6ae7c44eu,
    0xbe2da0a5u, 0x4c4623a6u, 0x5f16d052u, 0xad7d5351u,
};

u32 tbt_fec_crc32c_soft(u32 seed, const void *data, size_t len)
{
    const u8 *p = data;
    u32 crc = ~seed;

    if (!len)
        return seed;
    while (len--)
        crc = tbt_crc32c_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/*
 * Hardware path. The instruction updates the same reflected Castagnoli
 * register as the table, so the init complement and the final xor stay in the
 * caller and both paths chain with any seed.
 *
 * x86_64 uses SSE4.2 crc32q behind a target attribute: the host build keeps
 * its baseline ISA and the dispatcher checks the CPU at run time. aarch64
 * uses the optional ARMv8 CRC32X extension behind HWCAP_CRC32; a CPU without
 * it, or a compiler without the target attribute, keeps the table path.
 */
#if defined(__x86_64__)

#include <nmmintrin.h>

#define TBT_CRC32C_HW 1

__attribute__((target("sse4.2"))) static u32
tbt_crc32c_hw_update(u32 crc, const u8 *p, size_t len)
{
    /* crc32q reads memory, but an aligned pointer lets the core feed it one
     * load per instruction instead of splitting a cache line. */
    while (((uintptr_t)p & 7) && len)
    {
        crc = (u32)_mm_crc32_u8(crc, *p++);
        len--;
    }
    while (len >= 8)
    {
        u64 v;

        memcpy(&v, p, sizeof(v));
        crc = (u32)_mm_crc32_u64(crc, v);
        p += 8;
        len -= 8;
    }
    while (len--)
        crc = (u32)_mm_crc32_u8(crc, *p++);
    return crc;
}

static int tbt_crc32c_hw_detect(void)
{
    return __builtin_cpu_supports("sse4.2") ? 1 : 0;
}

/* GCC documents the extension-only target string from 9 on; Clang accepts
 * the same +crc target attribute and defines __GNUC__ as 4, so the version
 * test is skipped for it. A non-Linux libc without getauxval and a
 * big-endian ABI keep the table path (CRC32X consumes the register's native
 * byte order). */
#elif defined(__aarch64__) && defined(__linux__) && defined(__GNUC__) && \
    (defined(__clang__) || __GNUC__ >= 9) && defined(__BYTE_ORDER__) &&    \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

#include <arm_acle.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>

#define TBT_CRC32C_HW 1

__attribute__((target("+crc"))) static u32
tbt_crc32c_hw_update(u32 crc, const u8 *p, size_t len)
{
    while (((uintptr_t)p & 7) && len)
    {
        crc = __crc32cb(crc, *p++);
        len--;
    }
    while (len >= 8)
    {
        u64 v;

        memcpy(&v, p, sizeof(v));
        crc = __crc32cd(crc, v);
        p += 8;
        len -= 8;
    }
    while (len--)
        crc = __crc32cb(crc, *p++);
    return crc;
}

static int tbt_crc32c_hw_detect(void)
{
    return (getauxval(AT_HWCAP) & (unsigned long)HWCAP_CRC32) != 0;
}

#endif

int tbt_fec_crc32c_hw_available(void)
{
#ifdef TBT_CRC32C_HW
    /* -1 unknown, 0 table, 1 instruction. The host half is a library API and
     * may be entered from several threads; a plain static is a C data race
     * even when every writer stores the same value. The __atomic builtins
     * stay available under the host's -std=gnu99 (the kernel build never
     * compiles this half: it uses crc32c()). Relaxed is enough: the value
     * has no dependent data, only two writers racing to publish the same
     * result. */
    static int state = -1;
    int v = __atomic_load_n(&state, __ATOMIC_RELAXED);

    if (v < 0)
    {
        v = tbt_crc32c_hw_detect();
        __atomic_store_n(&state, v, __ATOMIC_RELAXED);
    }
    return v;
#else
    return 0;
#endif
}

u32 tbt_fec_crc32c_hw(u32 seed, const void *data, size_t len)
{
    if (!len)
        return seed;
#ifdef TBT_CRC32C_HW
    return ~tbt_crc32c_hw_update(~seed, data, len);
#else
    return tbt_fec_crc32c_soft(seed, data, len);
#endif
}

u32 tbt_fec_crc32c(u32 seed, const void *data, size_t len)
{
    if (!len)
        return seed;
    if (tbt_fec_crc32c_hw_available())
        return tbt_fec_crc32c_hw(seed, data, len);
    return tbt_fec_crc32c_soft(seed, data, len);
}

#endif

/* ------------------------------------------------------------------ *
 * Little-endian field helpers. The wire is little-endian regardless of the
 * host byte order; every field is read and written one byte at a time.
 * ------------------------------------------------------------------ */

static void tbt_put_u16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void tbt_put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void tbt_put_u64(u8 *p, u64 v)
{
    tbt_put_u32(p, (u32)v);
    tbt_put_u32(p + 4, (u32)(v >> 32));
}

static u16 tbt_get_u16(const u8 *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static u32 tbt_get_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 tbt_get_u64(const u8 *p)
{
    return (u64)tbt_get_u32(p) | ((u64)tbt_get_u32(p + 4) << 32);
}

/* ------------------------------------------------------------------ *
 * 48-byte header
 * ------------------------------------------------------------------ */

int tbt_fec_hdr_pack(const struct tbt_fec_hdr *h, u8 out[TBT_FEC_HDR_LEN])
{
    if (!h || !out)
        return -EINVAL;
    out[TBT_FEC_H_TYPE] = h->type;
    out[TBT_FEC_H_FLAGS] = h->flags;
    tbt_put_u16(out + TBT_FEC_H_SHARE, h->share_index);
    out[TBT_FEC_H_N] = h->n;
    out[TBT_FEC_H_M] = h->m;
    tbt_put_u16(out + TBT_FEC_H_EPOCH, h->epoch);
    tbt_put_u64(out + TBT_FEC_H_BLOCK, h->block_id);
    tbt_put_u64(out + TBT_FEC_H_SEQ, h->seq);
    tbt_put_u16(out + TBT_FEC_H_WINLEN, h->win_len);
    tbt_put_u16(out + TBT_FEC_H_PAYLEN, h->payload_len);
    tbt_put_u16(out + TBT_FEC_H_CONV, h->conv);
    tbt_put_u16(out + TBT_FEC_H_RESERVED, h->reserved);
    tbt_put_u64(out + TBT_FEC_H_SID, h->sid);
    tbt_put_u32(out + TBT_FEC_H_PAYCRC, h->payload_crc32c);
    tbt_put_u32(out + TBT_FEC_H_HDRCRC, h->header_crc32c);
    return 0;
}

int tbt_fec_hdr_unpack(const u8 in[TBT_FEC_HDR_LEN], struct tbt_fec_hdr *h)
{
    if (!in || !h)
        return -EINVAL;
    h->type = in[TBT_FEC_H_TYPE];
    h->flags = in[TBT_FEC_H_FLAGS];
    h->share_index = tbt_get_u16(in + TBT_FEC_H_SHARE);
    h->n = in[TBT_FEC_H_N];
    h->m = in[TBT_FEC_H_M];
    h->epoch = tbt_get_u16(in + TBT_FEC_H_EPOCH);
    h->block_id = tbt_get_u64(in + TBT_FEC_H_BLOCK);
    h->seq = tbt_get_u64(in + TBT_FEC_H_SEQ);
    h->win_len = tbt_get_u16(in + TBT_FEC_H_WINLEN);
    h->payload_len = tbt_get_u16(in + TBT_FEC_H_PAYLEN);
    h->conv = tbt_get_u16(in + TBT_FEC_H_CONV);
    h->reserved = tbt_get_u16(in + TBT_FEC_H_RESERVED);
    h->sid = tbt_get_u64(in + TBT_FEC_H_SID);
    h->payload_crc32c = tbt_get_u32(in + TBT_FEC_H_PAYCRC);
    h->header_crc32c = tbt_get_u32(in + TBT_FEC_H_HDRCRC);
    return 0;
}

/* Control frames carry no share. DATA and PARITY must name one. */
static bool tbt_fec_is_share_type(u8 type)
{
    return type == TBT_FEC_DATA || type == TBT_FEC_PARITY;
}

static bool tbt_fec_is_control_type(u8 type)
{
    return type == TBT_FEC_ACK || type == TBT_FEC_NACK || type == TBT_FEC_CFG ||
           type == TBT_FEC_FBK;
}

int tbt_fec_hdr_check(const struct tbt_fec_hdr *h, u16 max_payload)
{
    u16 shares;

#ifdef __KERNEL__
    BUILD_BUG_ON(TBT_FEC_HDR_LEN != 48);
    BUILD_BUG_ON(TBT_FEC_CFG_LEN != 64);
    BUILD_BUG_ON(TBT_FEC_FBK_LEN != 64);
    BUILD_BUG_ON(TBT_FEC_CFG_CRC_EXT != TBT_FEC_CFG_CODEC_VARIANT + 8);
    BUILD_BUG_ON(TBT_FEC_FBK_P_FAST_PPM != 48);
#endif
    if (!h)
        return -EPROTO;
    if (h->conv != TBT_FEC_CONV)
        return -EPROTO;
    if (h->flags & ~TBT_FEC_FF_MASK)
        return -EPROTO;
    if (max_payload > TBT_FEC_PAYLOAD_MAX)
        return -EPROTO;
    if (h->payload_len > TBT_FEC_PAYLOAD_MAX)
        return -EPROTO;

    if (tbt_fec_is_share_type(h->type))
    {
        if (!h->n || !h->m)
            return -EPROTO;
        /* +30 is the group for a share. The wire has at most 256 groups, so
         * a larger index can never name one. */
        if (h->reserved >= TBT_FEC_MAX_GROUPS)
            return -EPROTO;
        shares = (u16)h->n + (u16)h->m; /* cast first: 256 must not wrap */
        if (shares > TBT_FEC_MAX_SHARES)
            return -EPROTO;
        if (h->share_index >= shares)
            return -EPROTO;
        /* DATA names a data share, PARITY a parity share. */
        if (h->type == TBT_FEC_DATA)
        {
            if (h->share_index >= h->n)
                return -EPROTO;
        }
        else if (h->share_index < h->n)
        {
            return -EPROTO;
        }
        if (!max_payload || h->payload_len > max_payload)
            return -EPROTO;
        if (h->seq >= (1ULL << 63))
            return -EPROTO;
        if (h->type == TBT_FEC_DATA)
        {
            /* A short DATA packet is legal anywhere. The packetizer emits one
             * when the application goes quiet and the packet cannot fill, so
             * an interactive request is not held behind S bytes that never
             * come. The receiver places a share by the sum of the payload
             * lengths before it, not by the packet index times S, so a short
             * share mid-stream is unambiguous; LAST alone marks the end of
             * the stream. With PADDED the wire body is a full S-byte slot
             * (zero tail) and win_len is the real length; without it the body
             * is exactly payload_len bytes and win_len must stay zero. */
            if (h->flags & TBT_FEC_FF_PADDED)
            {
                if (h->payload_len != max_payload)
                    return -EPROTO;
                if (!h->win_len || h->win_len > max_payload)
                    return -EPROTO;
            }
            else
            {
                if (!h->payload_len || h->payload_len > max_payload)
                    return -EPROTO;
                if (h->win_len != 0)
                    return -EPROTO;
            }
            /* block_seq0 = seq - group*n - share_index must not underflow. */
            if (h->seq < (u64)h->reserved * h->n + h->share_index)
                return -EPROTO;
            /* epoch is block-level and belongs to PARITY only. A DATA record
             * that carried it let a mid-block short share overwrite the
             * block's last-share length, and a share recovered from parity
             * was then delivered with the wrong cut. */
            if (h->epoch != 0)
                return -EPROTO;
        }
        else
        {
            /* PADDED describes a DATA body length; a PARITY slot is always
             * full S bytes and carries no per-share length. */
            if (h->flags & TBT_FEC_FF_PADDED)
                return -EPROTO;
            if (h->win_len == 0)
            {
                /* Empty-block marker: no payload, no epoch, parity 0, and no
                 * flags. It is the only way a group says "I have no packet in
                 * this super-block", so the receiver can stop waiting for a
                 * block that will never arrive. */
                if (h->share_index != h->n || h->flags != 0 ||
                    h->payload_len != 0 || h->epoch != 0)
                    return -EPROTO;
            }
            else
            {
                if (h->win_len > h->n)
                    return -EPROTO;
                if (h->payload_len != max_payload)
                    return -EPROTO;
                /* epoch is the length of share win_len-1, never more than S.
                 * The receiver stores it in short_len and uses it as a
                 * delivery length; an unbounded value would read past the
                 * block slot. 0 means that share is full. */
                if (h->epoch > max_payload)
                    return -EPROTO;
            }
        }
        return 0;
    }

    if (!tbt_fec_is_control_type(h->type))
        return -EPROTO;
    if (h->n || h->m || h->share_index || h->win_len || h->reserved)
        return -EPROTO;
    /* The NACK names its super-block in the header. Every other control
     * record leaves block_id zero. */
    if (h->block_id && h->type != TBT_FEC_NACK)
        return -EPROTO;
    /* ACK carries its frontier in header.seq; the others must leave it zero. */
    if ((h->type == TBT_FEC_CFG || h->type == TBT_FEC_FBK ||
         h->type == TBT_FEC_NACK) && h->seq)
        return -EPROTO;

    switch (h->type)
    {
    case TBT_FEC_ACK:
        return h->payload_len == TBT_FEC_ACK_LEN ? 0 : -EPROTO;
    case TBT_FEC_NACK:
        return h->payload_len == TBT_FEC_NACK_LEN ? 0 : -EPROTO;
    case TBT_FEC_CFG:
        return h->payload_len == TBT_FEC_CFG_LEN ? 0 : -EPROTO;
    case TBT_FEC_FBK:
        return h->payload_len == TBT_FEC_FBK_LEN ? 0 : -EPROTO;
    default:
        return -EPROTO;
    }
}

/* Fallback only. A 0x1003 ACK carries deliver_bytes, which is the byte count
 * the receiver actually delivered; this multiplication is kept for a caller
 * that holds no such field and for the host contract tests. It is exact only
 * while every block is full and no block was closed early, because
 * next_deliver is a packet index and a short share or an empty slot leaves a
 * gap between that index and the byte count. */
u64 tbt_fec_ack_bytes(u64 next_deliver, u64 pkt_base, u64 tx_seq)
{
    u64 rel;

    /* Packets before the stream's first one are not this stream's bytes. */
    rel = next_deliver > pkt_base ? next_deliver - pkt_base : 0;
    /* Saturate before multiplying: rel * S would wrap for a hostile frontier
     * even when rel is only one past tx_seq / S. */
    if (rel > tx_seq / TBT_FEC_S)
        return tx_seq;
    rel *= TBT_FEC_S;
    return rel > tx_seq ? tx_seq : rel;
}

int tbt_fec_hdr_seal(struct tbt_fec_hdr *h, const u8 *payload)
{
    u8 buf[TBT_FEC_HDR_LEN];

    if (!h || (h->payload_len && !payload))
        return -EINVAL;
    h->payload_crc32c = tbt_fec_crc32c(0, payload, h->payload_len);
    h->header_crc32c = 0;
    if (tbt_fec_hdr_pack(h, buf))
        return -EINVAL;
    h->header_crc32c = tbt_fec_crc32c(0, buf, TBT_FEC_H_HDRCRC);
    return 0;
}

int tbt_fec_hdr_verify(const struct tbt_fec_hdr *h, const u8 *payload)
{
    u8 buf[TBT_FEC_HDR_LEN];
    u32 want;

    if (!h)
        return -EPROTO;
    if (tbt_fec_hdr_pack(h, buf))
        return -EINVAL;
    want = tbt_fec_crc32c(0, buf, TBT_FEC_H_HDRCRC);
    if (want != h->header_crc32c)
        return -EBADMSG;
    if (h->payload_len && !payload)
        return -EINVAL;
    if (tbt_fec_crc32c(0, payload, h->payload_len) != h->payload_crc32c)
        return -EBADMSG;
    return 0;
}

int tbt_fec_rec_check(const u8 *rec, size_t len, u16 max_payload,
                      struct tbt_fec_hdr *out)
{
    struct tbt_fec_hdr h;
    u8 buf[TBT_FEC_HDR_LEN];

    if (!rec || len < TBT_FEC_HDR_LEN)
        return -EPROTO;
    if (tbt_fec_hdr_unpack(rec, &h) || tbt_fec_hdr_check(&h, max_payload))
        return -EPROTO;
    if (tbt_fec_hdr_pack(&h, buf))
        return -EPROTO;
    if (tbt_fec_crc32c(0, buf, TBT_FEC_H_HDRCRC) != h.header_crc32c)
        return -EBADMSG;
    if (out)
        *out = h;
    return 0;
}

/* ------------------------------------------------------------------ *
 * 256-bit carrier bitmap
 * ------------------------------------------------------------------ */

void tbt_fec_bitmap_set(u8 bm[TBT_FEC_BITMAP_BYTES], u16 index)
{
    if (!bm || index >= TBT_FEC_MAX_CARRIERS)
        return;
    bm[index >> 3] |= (u8)(1u << (index & 7));
}

void tbt_fec_bitmap_clear(u8 bm[TBT_FEC_BITMAP_BYTES], u16 index)
{
    if (!bm || index >= TBT_FEC_MAX_CARRIERS)
        return;
    bm[index >> 3] &= (u8)~(1u << (index & 7));
}

bool tbt_fec_bitmap_test(const u8 bm[TBT_FEC_BITMAP_BYTES], u16 index)
{
    if (!bm || index >= TBT_FEC_MAX_CARRIERS)
        return false;
    return (bm[index >> 3] & (u8)(1u << (index & 7))) != 0;
}

u16 tbt_fec_bitmap_count(const u8 bm[TBT_FEC_BITMAP_BYTES])
{
    u16 i;
    u16 n = 0;

    if (!bm)
        return 0;
    for (i = 0; i < TBT_FEC_BITMAP_BYTES; i++)
    {
        u8 v = bm[i];

        while (v)
        {
            n += (u16)(v & 1);
            v >>= 1;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ *
 * Control bodies
 * ------------------------------------------------------------------ */

int tbt_fec_ack_pack(const struct tbt_fec_ack *a, u8 out[TBT_FEC_ACK_LEN])
{
    if (!a || !out)
        return -EINVAL;
    tbt_put_u64(out + 0, a->next_deliver);
    tbt_put_u32(out + 8, a->credit_packets);
    tbt_put_u32(out + 12, a->rtt_us);
    tbt_put_u64(out + 16, a->deliver_bytes);
    return 0;
}

int tbt_fec_ack_unpack(const u8 in[TBT_FEC_ACK_LEN], struct tbt_fec_ack *a)
{
    if (!in || !a)
        return -EINVAL;
    a->next_deliver = tbt_get_u64(in + 0);
    a->credit_packets = tbt_get_u32(in + 8);
    a->rtt_us = tbt_get_u32(in + 12);
    a->deliver_bytes = tbt_get_u64(in + 16);
    return 0;
}

int tbt_fec_nack_pack(const struct tbt_fec_nack *n, u8 out[TBT_FEC_NACK_LEN])
{
    if (!n || !out)
        return -EINVAL;
    memcpy(out, n->group_mask, TBT_FEC_BITMAP_BYTES);
    memcpy(out + TBT_FEC_BITMAP_BYTES, n->share_mask, TBT_FEC_BITMAP_BYTES);
    return TBT_FEC_NACK_LEN;
}

int tbt_fec_nack_unpack(const u8 in[TBT_FEC_NACK_LEN], struct tbt_fec_nack *n)
{
    if (!in || !n)
        return -EPROTO;
    memcpy(n->group_mask, in, TBT_FEC_BITMAP_BYTES);
    memcpy(n->share_mask, in + TBT_FEC_BITMAP_BYTES, TBT_FEC_BITMAP_BYTES);
    return 0;
}

/* ------------------------------------------------------------------ *
 * Bounded replay ring
 * ------------------------------------------------------------------ */

struct tbt_fec_replay
{
    u16 groups, n, m, s, k;
    u16 stride; /* TBT_FEC_HDR_LEN + s */
    u32 slots;       /* k * groups * (n + m): record descriptors */
    u32 bslots;      /* k * groups: one resident-block id per group */
    u32 block_bytes; /* groups * (n + m) * stride: one block plane */
    u64 *block_id;
    u64 *block_gen;   /* block id of the resident block, or ~0 */
    u64 *block_start; /* [k] block id owning blk_rec[i], or ~0 */
    /* Shape and origin of the block in each slot. A block may use any split
     * of the direction's n+m, so the NACK reply must interpret its share
     * indices with that block's n/m, not the sender's current shape. */
    u64 *shape_id;   /* [k] block id the shape fields describe, or ~0 */
    u16 *shape_n;    /* [k] DATA shares of that block */
    u16 *shape_m;    /* [k] parity shares of that block */
    u64 *shape_seq0; /* [k] its group-0/share-0 packet seq */
    /* When a share was last queued as a replay. The NACK handler stamps it
     * and skips a share whose reply is still in flight; storage and this
     * stamp are both protected by the caller's TX replay lock. */
    u64 *sent_ns;
    u16 *len;
    /* Highest block id the peer has acknowledged. A slot holding an id at or
     * above this floor is still repair-relevant and must never be reused. */
    u64 ack_floor;
    /* Only resident block planes are allocated. blk_rec[b] is NULL until the
     * first record of that block is stored; an ACK that passes the block
     * returns it to hot[] (bounded by TBT_FEC_REPLAY_HOT_MAX) or frees it. */
    u8 *blk_rec[TBT_FEC_REPLAY_K_MAX];
    u8 *hot[TBT_FEC_REPLAY_HOT_MAX];
    u32 hot_n;
};

u16 tbt_fec_replay_k(const struct tbt_fec_replay *r)
{
    return r ? r->k : 0;
}

size_t tbt_fec_replay_bytes(u16 groups, u16 n, u16 m, u16 s, u16 k)
{
    size_t shares = (size_t)n + m;
    size_t stride = TBT_FEC_HDR_LEN + s;
    size_t block_bytes;
    size_t desc;

    if (!groups || !n || !m || !k || !s)
        return 0;
    block_bytes = (size_t)groups * shares * stride;
    desc = sizeof(struct tbt_fec_replay) +
           (size_t)k * groups * shares * (sizeof(u64) + sizeof(u16)) +
           (size_t)k * groups * shares * sizeof(u64) +
           (size_t)k * groups * sizeof(u64) + (size_t)k * sizeof(u64) +
           (size_t)k * (sizeof(u64) + 2u * sizeof(u16) + sizeof(u64));
    return desc +
           (size_t)(k < TBT_FEC_REPLAY_HOT_MAX ? k : TBT_FEC_REPLAY_HOT_MAX) *
               block_bytes;
}

u16 tbt_fec_replay_k_for(u16 groups, u16 n, u16 m, u16 s)
{
    u16 k = TBT_FEC_REPLAY_K_MAX;

    if (!groups || groups > TBT_FEC_MAX_GROUPS || !n || !m ||
        (u32)n + m > TBT_FEC_MAX_SHARES || !s || s > TBT_FEC_PAYLOAD_MAX)
        return 0;
    /* Start at the cap and shrink until the descriptor plane plus the hot
     * block pool fits the budget. The 64-block window needs K=66; a shape
     * that cannot hold one block gets no ring and falls back to parity and
     * the carrier TCP retransmission. */
    while (k > 0 && tbt_fec_replay_bytes(groups, n, m, s, k) >
                        TBT_FEC_REPLAY_BUDGET)
        k--;
    return k;
}

/* Build a ring with an explicit K. A multi-stream session keeps one K for the
 * whole session and gives each flow a slice of the group budget, so the sum
 * of the flow rings is the single-stream ring (design §3.1). */
struct tbt_fec_replay *tbt_fec_replay_new_k(u16 groups, u16 n, u16 m, u16 s,
                                            u16 k)
{
    struct tbt_fec_replay *r;
    size_t shares = (size_t)n + m;
    size_t stride, slots;
    size_t block_bytes;
    u32 i;

    /* The ring's share count and group count are stored as u16 fields and
     * every later share bound casts n + m to u16, so an over-limit shape
     * would wrap the bound (n=65535, m=5 looks like 4 shares) and allocate
     * a plane far past the replay budget. tbt_fec_replay_k_for() already
     * applies these limits; the explicit-K constructor must not bypass
     * them. */
    if (!groups || groups > TBT_FEC_MAX_GROUPS || !n || !m ||
        (u32)n + m > TBT_FEC_MAX_SHARES || !s ||
        s > TBT_FEC_PAYLOAD_MAX || !k || k > TBT_FEC_REPLAY_K_MAX)
        return NULL;
    /* The ring is a bounded-memory object: an explicit K may not buy a
     * descriptor plane and hot pool above TBT_FEC_REPLAY_BUDGET. k_for()
     * picks K by the same rule, so every accepted K here has k <= that K. */
    if (tbt_fec_replay_bytes(groups, n, m, s, k) >
        TBT_FEC_REPLAY_BUDGET)
        return NULL;
    stride = TBT_FEC_HDR_LEN + s;
    slots = (size_t)k * groups * shares;
    block_bytes = (size_t)groups * shares * stride;
    if (slots > 0xffffffffu)
        return NULL;
    if (block_bytes > 0xffffffffu)
        return NULL;
    r = tbt_fec_alloc(sizeof(*r));
    if (!r)
        return NULL;
    r->groups = groups;
    r->n = n;
    r->m = m;
    r->s = s;
    r->k = k;
    r->stride = (u16)stride;
    r->slots = (u32)slots;
    r->bslots = (u32)((size_t)k * groups);
    r->block_bytes = (u32)block_bytes;
    r->block_id = tbt_fec_alloc(slots * sizeof(*r->block_id));
    r->block_gen = tbt_fec_alloc((size_t)r->bslots * sizeof(*r->block_gen));
    r->block_start = tbt_fec_alloc((size_t)k * sizeof(*r->block_start));
    r->sent_ns = tbt_fec_alloc(slots * sizeof(*r->sent_ns));
    r->len = tbt_fec_alloc(slots * sizeof(*r->len));
    r->shape_id = tbt_fec_alloc((size_t)k * sizeof(*r->shape_id));
    r->shape_n = tbt_fec_alloc((size_t)k * sizeof(*r->shape_n));
    r->shape_m = tbt_fec_alloc((size_t)k * sizeof(*r->shape_m));
    r->shape_seq0 = tbt_fec_alloc((size_t)k * sizeof(*r->shape_seq0));
    if (!r->block_id || !r->block_gen || !r->block_start || !r->sent_ns ||
        !r->len || !r->shape_id || !r->shape_n || !r->shape_m ||
        !r->shape_seq0)
    {
        tbt_fec_replay_free(r);
        return NULL;
    }
    for (i = 0; i < r->slots; i++)
        r->block_id[i] = ~0ULL;
    for (i = 0; i < r->bslots; i++)
        r->block_gen[i] = ~0ULL;
    for (i = 0; i < r->k; i++)
    {
        r->block_start[i] = ~0ULL;
        r->shape_id[i] = ~0ULL;
        r->shape_n[i] = 0;
        r->shape_m[i] = 0;
        r->shape_seq0[i] = 0;
    }
    r->ack_floor = 0;
    return r;
}

struct tbt_fec_replay *tbt_fec_replay_new(u16 groups, u16 n, u16 m, u16 s)
{
    return tbt_fec_replay_new_k(groups, n, m, s,
                                tbt_fec_replay_k_for(groups, n, m, s));
}

void tbt_fec_replay_free(struct tbt_fec_replay *r)
{
    u32 i;

    if (!r)
        return;
    tbt_fec_free(r->block_id);
    tbt_fec_free(r->block_gen);
    tbt_fec_free(r->block_start);
    tbt_fec_free(r->sent_ns);
    tbt_fec_free(r->len);
    tbt_fec_free(r->shape_id);
    tbt_fec_free(r->shape_n);
    tbt_fec_free(r->shape_m);
    tbt_fec_free(r->shape_seq0);
    for (i = 0; i < r->k; i++)
        tbt_fec_free(r->blk_rec[i]);
    for (i = 0; i < r->hot_n; i++)
        tbt_fec_free(r->hot[i]);
    tbt_fec_free(r);
}

static u32 tbt_fec_replay_slot(const struct tbt_fec_replay *r, u64 block_id,
                               u16 group, u16 share)
{
    u64 b = block_id % r->k;
    u16 shares = (u16)(r->n + r->m);

    return (u32)((b * r->groups + group) * shares + share);
}

static u32 tbt_fec_replay_bslot(const struct tbt_fec_replay *r, u64 block_id,
                                u16 group)
{
    return (u32)((block_id % r->k) * r->groups + group);
}

static void tbt_fec_replay_shape_set(struct tbt_fec_replay *r, u32 b,
                                     u64 block_id, u16 n, u16 m, u64 seq0)
{
    r->shape_id[b] = block_id;
    r->shape_n[b] = n;
    r->shape_m[b] = m;
    r->shape_seq0[b] = seq0;
}

/* Ensure the ring has a record plane for block_id and remember the shape it
 * was built with. Called in process context before the caller takes
 * replay_lock: the plane is allocated here, never under the spinlock. A
 * block's plane is reused from hot[] when possible. */
bool tbt_fec_replay_reclaimable(const struct tbt_fec_replay *r,
                                u64 block_id, u64 peer_pkts,
                                tbt_fec_spinlock_t *lock)
{
    u32 b;
    u64 end;
    bool ok;

    if (!r || !r->k)
        return false;
    b = (u32)(block_id % r->k);
    tbt_fec_lock(lock);
    end = r->shape_seq0[b] + (u64)r->groups * r->shape_n[b];
    ok = r->blk_rec[b] && r->block_start[b] != ~0ULL && r->shape_n[b] &&
         end <= peer_pkts;
    tbt_fec_unlock(lock);
    return ok;
}

int tbt_fec_replay_ensure_block(struct tbt_fec_replay *r, u64 block_id,
                                u16 n, u16 m, u64 seq0, u64 peer_pkts,
                                tbt_fec_spinlock_t *lock)
{
    u32 b, i, row, per_block;
    u8 *buf;

    if (!r || !r->k || !n || !m ||
        (u16)(n + m) != (u16)(r->n + r->m))
        return -EINVAL;
    b = (u32)(block_id % r->k);
    tbt_fec_lock(lock);
    if (r->block_start[b] == block_id && r->blk_rec[b])
    {
        tbt_fec_replay_shape_set(r, b, block_id, n, m, seq0);
        tbt_fec_unlock(lock);
        return 0;
    }
    /* A different block in this slot is reusable only after the peer has
     * acknowledged it. An unacknowledged block still needs its repair plane;
     * with credit K-2 the sender should never reach this branch, but if it
     * does, refuse instead of silently evicting NACK coverage. */
    if (r->blk_rec[b])
    {
#ifndef __KERNEL__
        /* Host tests model the circular ring's eviction directly; the
         * kernel path below uses the slot's own packet span. */
        (void)peer_pkts;
        buf = r->blk_rec[b];
        r->blk_rec[b] = NULL;
        r->block_start[b] = ~0ULL;
#else
        u64 end = r->shape_seq0[b] +
                  (u64)r->groups * r->shape_n[b];

        if (!r->shape_n[b] || end > peer_pkts)
        {
            tbt_fec_unlock(lock);
            return -ENOSPC;
        }
        buf = r->blk_rec[b];
        r->blk_rec[b] = NULL;
        r->block_start[b] = ~0ULL;
#endif
    }
    else
        buf = r->hot_n ? r->hot[--r->hot_n] : NULL;
    tbt_fec_unlock(lock);
    if (!buf)
    {
        buf = tbt_fec_alloc(r->block_bytes);
        if (!buf)
            return -ENOMEM;
    }
    tbt_fec_lock(lock);
    if (r->block_start[b] != ~0ULL || r->blk_rec[b])
    {
        /* Racing ensure: keep this plane for the next block. The winner
         * stored its own block's shape; only stamp ours when it is the same
         * block. */
        if (r->block_start[b] == block_id && r->blk_rec[b])
            tbt_fec_replay_shape_set(r, b, block_id, n, m, seq0);
        if (r->hot_n < TBT_FEC_REPLAY_HOT_MAX)
            r->hot[r->hot_n++] = buf;
        else
        {
            tbt_fec_unlock(lock);
            tbt_fec_free(buf);
            return 0;
        }
        tbt_fec_unlock(lock);
        return 0;
    }
    /* The plane's bytes are never read without a descriptor: replay_get()
     * checks block_id[]/len[] for this share, and store() writes both before
     * the record. A recycled hot plane may therefore keep stale bytes; only
     * the descriptor rows below must be reset. Zeroing the whole plane cost
     * groups * (n + m) * stride (274 KiB for 8x32) per TX block to protect
     * bytes no reader can reach. */
    r->blk_rec[b] = buf;
    r->block_start[b] = block_id;
    tbt_fec_replay_shape_set(r, b, block_id, n, m, seq0);
    per_block = (u32)r->groups * (u32)(r->n + r->m);
    for (i = 0; i < per_block; i++)
    {
        row = b * per_block + i;
        r->block_id[row] = ~0ULL;
        r->sent_ns[row] = 0;
        r->len[row] = 0;
    }
    for (i = 0; i < r->groups; i++)
        r->block_gen[b * r->groups + i] = ~0ULL;
    tbt_fec_unlock(lock);
    return 0;
}

bool tbt_fec_replay_shape(const struct tbt_fec_replay *r, u64 block_id,
                          u16 *n, u16 *m, u64 *seq0)
{
    u32 b;

    if (!r || !n || !m)
        return false;
    b = (u32)(block_id % r->k);
    if (r->block_start[b] != block_id || r->shape_id[b] != block_id)
        return false;
    *n = r->shape_n[b];
    *m = r->shape_m[b];
    if (seq0)
        *seq0 = r->shape_seq0[b];
    return true;
}

int tbt_fec_replay_store(struct tbt_fec_replay *r, u64 block_id, u16 group,
                         u16 share, const u8 *rec, u16 len)
{
    u32 b, slot;

    if (!r || !rec || group >= r->groups || share >= (u16)(r->n + r->m) ||
        len > r->stride)
        return -EINVAL;
#ifndef __KERNEL__
    /* The host tests drive the ring single-threaded and have no separate
     * process-context phase; allocate the block plane on first store. */
    {
        static tbt_fec_spinlock_t host_lock;
        u32 hb = (u32)(block_id % r->k);

        if (!r->blk_rec[hb] || r->block_start[hb] != block_id)
            tbt_fec_replay_ensure_block(r, block_id, r->n, r->m, 0, 0,
                                        &host_lock);
    }
#endif
    b = (u32)(block_id % r->k);
    if (r->block_start[b] != block_id || !r->blk_rec[b])
        return -ESTALE;
    slot = tbt_fec_replay_slot(r, block_id, group, share);
    memcpy(r->blk_rec[b] +
               (size_t)((size_t)group * (r->n + r->m) + share) * r->stride,
           rec, len);
    r->block_id[slot] = block_id;
    r->block_gen[tbt_fec_replay_bslot(r, block_id, group)] = block_id;
    r->sent_ns[slot] = 0; /* a fresh share has no replay in flight */
    r->len[slot] = len;
    return 0;
}

int tbt_fec_replay_get(const struct tbt_fec_replay *r, u64 block_id,
                       u16 group, u16 share, u8 *out, u16 cap)
{
    u32 b, slot;
    u16 len;

    if (!r || !out || group >= r->groups || share >= (u16)(r->n + r->m))
        return -EINVAL;
    b = (u32)(block_id % r->k);
    if (r->block_start[b] != block_id || !r->blk_rec[b])
        return -ESTALE;
    slot = tbt_fec_replay_slot(r, block_id, group, share);
    if (r->block_id[slot] != block_id)
        return -ENODATA;
    len = r->len[slot];
    if (!len)
        return -ENODATA;
    if (cap < len)
        return -ENOSPC;
    memcpy(out, r->blk_rec[b] +
                    (size_t)((size_t)group * (r->n + r->m) + share) *
                        r->stride,
           len);
    return len;
}

bool tbt_fec_replay_has(const struct tbt_fec_replay *r, u64 block_id,
                        u16 group, u16 share)
{
    u32 b, slot;

    if (!r || group >= r->groups || share >= (u16)(r->n + r->m))
        return false;
    b = (u32)(block_id % r->k);
    if (r->block_start[b] != block_id || !r->blk_rec[b])
        return false;
    slot = tbt_fec_replay_slot(r, block_id, group, share);
    return r->block_id[slot] == block_id && r->len[slot] != 0;
}

u64 tbt_fec_replay_sent_ns(const struct tbt_fec_replay *r, u64 block_id,
                           u16 group, u16 share)
{
    u32 b, slot;

    if (!r || group >= r->groups || share >= (u16)(r->n + r->m))
        return 0;
    b = (u32)(block_id % r->k);
    if (r->block_start[b] != block_id || !r->blk_rec[b])
        return 0;
    slot = tbt_fec_replay_slot(r, block_id, group, share);
    if (r->block_id[slot] != block_id)
        return 0;
    return r->sent_ns[slot];
}

void tbt_fec_replay_mark_sent(struct tbt_fec_replay *r, u64 block_id,
                              u16 group, u16 share, u64 now_ns)
{
    u32 b, slot;

    if (!r || group >= r->groups || share >= (u16)(r->n + r->m))
        return;
    b = (u32)(block_id % r->k);
    if (r->block_start[b] != block_id || !r->blk_rec[b])
        return;
    slot = tbt_fec_replay_slot(r, block_id, group, share);
    if (r->block_id[slot] != block_id || !r->len[slot])
        return;
    r->sent_ns[slot] = now_ns;
}

/* Detach every resident plane under replay_lock. The caller frees drop[]
 * after unlocking. */
void tbt_fec_replay_reset_locked(struct tbt_fec_replay *r, u8 **drop,
                                 u32 *ndrop, u32 cap)
{
    u32 i, n = *ndrop;

    if (!r)
        return;
    for (i = 0; i < r->k; i++)
    {
        if (!r->blk_rec[i])
            continue;
        if (n < cap)
            drop[n++] = r->blk_rec[i];
        r->blk_rec[i] = NULL;
        r->block_start[i] = ~0ULL;
    }
    for (i = 0; i < r->hot_n; i++)
    {
        if (n < cap)
            drop[n++] = r->hot[i];
    }
    r->hot_n = 0;
    for (i = 0; i < r->slots; i++)
    {
        r->block_id[i] = ~0ULL;
        r->sent_ns[i] = 0;
        r->len[i] = 0;
    }
    for (i = 0; i < r->bslots; i++)
        r->block_gen[i] = ~0ULL;
    *ndrop = n;
}


/* Number of resident block planes the peer has not acknowledged. The
 * packetizer uses this to keep opening blocks under the 180 MiB floor. */
u32 tbt_fec_replay_unacked(struct tbt_fec_replay *r, tbt_fec_spinlock_t *lock)
{
    u32 i, n = 0;

    if (!r)
        return 0;
    tbt_fec_lock(lock);
    for (i = 0; i < r->k; i++)
        if (r->blk_rec[i] && r->block_start[i] != ~0ULL &&
            r->block_start[i] >= r->ack_floor)
            n++;
    tbt_fec_unlock(lock);
    return n;
}

/* Low-memory and stream-reset reclaim. Free every hot plane; when
 * keep_active is false (stream reset) free every active plane and keep only
 * keep_hot planes for reuse. When keep_active is true, ACKed planes are
 * freed and the newest 8 unacknowledged planes are kept (older ones are
 * evicted only under memory pressure). */
void tbt_fec_replay_shrink(struct tbt_fec_replay *r, bool keep_active,
                           u32 keep_hot, tbt_fec_spinlock_t *lock)
{
    u8 *drop[TBT_FEC_REPLAY_K_MAX + TBT_FEC_REPLAY_HOT_MAX];
    u32 i, n = 0;

    if (!r)
        return;
    if (keep_hot > TBT_FEC_REPLAY_HOT_MAX)
        keep_hot = TBT_FEC_REPLAY_HOT_MAX;
    tbt_fec_lock(lock);
    for (i = 0; i < r->hot_n; i++)
        drop[n++] = r->hot[i];
    r->hot_n = 0;
    for (i = 0; i < r->k; i++)
    {
        bool keep = false;

        if (!r->blk_rec[i] || r->block_start[i] == ~0ULL)
            continue;
        /* Never evict an unacknowledged block: the frontier block is exactly
         * the one the receiver is NACKing, and dropping it turns the repair
         * into -ESTALE and a 30 s stall kill. Only hot/ACKed planes go. */
        if (keep_active)
            keep = r->block_start[i] >= r->ack_floor;
        if (keep)
            continue;
        drop[n++] = r->blk_rec[i];
        r->blk_rec[i] = NULL;
        r->block_start[i] = ~0ULL;
    }
    while (n > 0 && r->hot_n < keep_hot)
        r->hot[r->hot_n++] = drop[--n];
    tbt_fec_unlock(lock);
    for (i = 0; i < n; i++)
        tbt_fec_free(drop[i]);
}

#ifndef __KERNEL__
/* Host-test form: no lock is held by the caller. The kernel resets through
 * tbt_fec_replay_reset_locked() or tbt_fec_replay_shrink() with the TX
 * replay spinlock. */
void tbt_fec_replay_reset(struct tbt_fec_replay *r)
{
    tbt_fec_spinlock_t host_lock = 0;

    tbt_fec_replay_shrink(r, false, 2, &host_lock);
}
#endif

/* Return detached planes to the hot pool or free them. Both ACK forms drop
 * under the replay lock and release outside it. */
static void tbt_fec_replay_drop_planes(struct tbt_fec_replay *r, u8 **drop,
                                       u32 n, bool low_mem,
                                       tbt_fec_spinlock_t *lock)
{
    u32 i, hot_max = low_mem ? TBT_FEC_REPLAY_HOT_LOW
                             : TBT_FEC_REPLAY_HOT_MAX;

    for (i = 0; i < n; i++)
    {
        bool kept = false;

        tbt_fec_lock(lock);
        if (r->hot_n < hot_max)
        {
            r->hot[r->hot_n++] = drop[i];
            kept = true;
        }
        tbt_fec_unlock(lock);
        if (!kept)
            tbt_fec_free(drop[i]);
    }
}

/* Release block planes the peer has acknowledged. Blocks below frontier are
 * delivered; keeping their records would only hold memory. The hot pool is
 * capped at 8 (4 under the 220 MiB memory floor). */
void tbt_fec_replay_ack(struct tbt_fec_replay *r, u64 frontier, bool low_mem,
                        tbt_fec_spinlock_t *lock)
{
    u8 *drop[TBT_FEC_REPLAY_K_MAX];
    u32 i, n = 0;

    if (!r)
        return;
    tbt_fec_lock(lock);
    if (frontier > r->ack_floor)
        r->ack_floor = frontier;
    for (i = 0; i < r->k; i++)
    {
        if (r->blk_rec[i] && r->block_start[i] != ~0ULL &&
            r->block_start[i] < frontier)
        {
            drop[n++] = r->blk_rec[i];
            r->blk_rec[i] = NULL;
            r->block_start[i] = ~0ULL;
        }
    }
    tbt_fec_unlock(lock);
    tbt_fec_replay_drop_planes(r, drop, n, low_mem, lock);
}

/* Release by each block's own packet interval. A rate switch moves the shape
 * origin to the next block boundary while the blocks built before it stay
 * resident; a block-id frontier derived from the new origin is 0 until the
 * peer's ACK catches up, which stalled eviction for the whole switch-over
 * window. The recorded (seq0, n) span answers directly, whatever shape built
 * the block. */
void tbt_fec_replay_ack_seq(struct tbt_fec_replay *r, u64 peer_pkts,
                            bool low_mem, tbt_fec_spinlock_t *lock)
{
    u8 *drop[TBT_FEC_REPLAY_K_MAX];
    u32 i, n = 0;
    u64 floor = 0;

    if (!r)
        return;
    tbt_fec_lock(lock);
    for (i = 0; i < r->k; i++)
    {
        u64 id, end;

        if (!r->blk_rec[i] || r->block_start[i] == ~0ULL ||
            !r->shape_n[i])
            continue;
        id = r->block_start[i];
        end = r->shape_seq0[i] + (u64)r->groups * r->shape_n[i];
        if (end > peer_pkts)
            continue;
        if (id + 1 > floor)
            floor = id + 1;
        drop[n++] = r->blk_rec[i];
        r->blk_rec[i] = NULL;
        r->block_start[i] = ~0ULL;
    }
    if (floor > r->ack_floor)
        r->ack_floor = floor;
    tbt_fec_unlock(lock);
    tbt_fec_replay_drop_planes(r, drop, n, low_mem, lock);
}

/* ------------------------------------------------------------------ *
 * NACK pacing
 * ------------------------------------------------------------------ */

static bool tbt_fec_nack_same(const struct tbt_fec_nack_state *st,
                              u64 block_id, const struct tbt_fec_nack *n)
{
    return st->valid && st->block_id == block_id &&
           !memcmp(st->group_mask, n->group_mask, TBT_FEC_BITMAP_BYTES) &&
           !memcmp(st->share_mask, n->share_mask, TBT_FEC_BITMAP_BYTES);
}

bool tbt_fec_nack_due_at(struct tbt_fec_nack_state *st, u64 now_ns,
                         u64 block_id, const struct tbt_fec_nack *n,
                         u32 retry_ms)
{
    u64 interval;

    if (!st || !n)
        return false;
    if (!tbt_fec_nack_same(st, block_id, n))
    {
        interval = (u64)TBT_FEC_NACK_MIN_MS * 1000000ULL;
        if (st->valid && now_ns - st->last_ns < interval)
            return false;
        st->block_id = block_id;
        memcpy(st->group_mask, n->group_mask, TBT_FEC_BITMAP_BYTES);
        memcpy(st->share_mask, n->share_mask, TBT_FEC_BITMAP_BYTES);
        st->first_ns = now_ns;
        st->last_ns = now_ns;
        st->tries = 1;
        st->valid = true;
        return true;
    }
    /* An identical NACK is a retry: the previous one may have been lost.
     * There is no total cap; the replay window and the carrier TCP lifetime
     * bound the repair, and the caller keeps the rate. */
    if (retry_ms < TBT_FEC_NACK_RETRY_MS)
        retry_ms = TBT_FEC_NACK_RETRY_MS;
    interval = (u64)retry_ms * 1000000ULL;
    if (now_ns - st->last_ns < interval)
        return false;
    st->last_ns = now_ns;
    st->tries++;
    return true;
}

void tbt_fec_nack_reset(struct tbt_fec_nack_state *st)
{
    if (st)
        memset(st, 0, sizeof(*st));
}

int tbt_fec_cfg_pack(const struct tbt_fec_cfg *c, u8 out[TBT_FEC_CFG_LEN])
{
    u8 variant;

    if (!c || !out)
        return -EINVAL;
    memset(out, 0, TBT_FEC_CFG_LEN);
    tbt_put_u32(out + TBT_FEC_CFG_EPOCH, c->epoch);
    tbt_put_u16(out + TBT_FEC_CFG_N, c->n);
    tbt_put_u16(out + TBT_FEC_CFG_M, c->m);
    variant = c->codec_variant;
    /* An SC-LDPC body carries its graph in the extension; the original
     * w/k/d bytes stay zero on the wire (0.5.0 §5). The in-kernel mirror in
     * the struct is not a second wire encoding. */
    tbt_put_u16(out + TBT_FEC_CFG_W, variant == 2 ? 0 : c->w);
    tbt_put_u16(out + TBT_FEC_CFG_K, variant == 2 ? 0 : c->k);
    out[TBT_FEC_CFG_D] = variant == 2 ? 0 : c->d;
    out[TBT_FEC_CFG_FLAGS] = c->flags;
    tbt_put_u16(out + TBT_FEC_CFG_GROUPS, c->groups);
    tbt_put_u64(out + TBT_FEC_CFG_SWITCH_PKT, c->switch_pkt);
    tbt_put_u64(out + TBT_FEC_CFG_BLOCK_BASE, c->block_base);
    tbt_put_u64(out + TBT_FEC_CFG_SEED, c->seed);
    tbt_put_u32(out + TBT_FEC_CFG_CRC,
                tbt_fec_crc32c(0, out, TBT_FEC_CFG_CRC_BYTES));
    tbt_put_u32(out + TBT_FEC_CFG_RESERVED, 0);
    out[TBT_FEC_CFG_CODEC_VARIANT] = variant;
    out[TBT_FEC_CFG_L] = c->L;
    tbt_put_u16(out + TBT_FEC_CFG_BLOCK, c->block);
    tbt_put_u32(out + TBT_FEC_CFG_RATE_PPM, c->rate_ppm);
    tbt_put_u32(out + TBT_FEC_CFG_CRC_EXT,
                tbt_fec_crc32c(0, out + TBT_FEC_CFG_CODEC_VARIANT,
                               TBT_FEC_CFG_CRC_EXT_BYTES));
    tbt_put_u32(out + TBT_FEC_CFG_RESERVED2, 0);
    return 0;
}

int tbt_fec_cfg_unpack(const u8 in[TBT_FEC_CFG_LEN], u16 carriers,
                       struct tbt_fec_cfg *c)
{
    u32 crc;
    u32 shares;
    u16 n, m, groups;
    u8 variant, L;
    u16 block;
    u32 rate_ppm;

    if (!in || !c || !carriers || carriers > TBT_FEC_MAX_CARRIERS)
        return -EINVAL;
    /* The CRC covers every field before the reserved word, so verify it
     * before trusting n/m/groups or the switch coordinates. */
    crc = tbt_fec_crc32c(0, in, TBT_FEC_CFG_CRC_BYTES);
    if (crc != tbt_get_u32(in + TBT_FEC_CFG_CRC))
        return -EPROTO;
    if (tbt_get_u32(in + TBT_FEC_CFG_RESERVED) != 0)
        return -EPROTO;
    if (in[TBT_FEC_CFG_FLAGS] & ~TBT_FEC_CFG_F_MASK)
        return -EPROTO;
    /* The extension has its own CRC, so a truncated or v7-shaped body that
     * happened to pass the prefix CRC cannot smuggle a variant in. */
    if (tbt_get_u32(in + TBT_FEC_CFG_RESERVED2) != 0)
        return -EPROTO;
    if (tbt_get_u32(in + TBT_FEC_CFG_CRC_EXT) !=
        tbt_fec_crc32c(0, in + TBT_FEC_CFG_CODEC_VARIANT,
                       TBT_FEC_CFG_CRC_EXT_BYTES))
        return -EPROTO;
    c->epoch = tbt_get_u32(in + TBT_FEC_CFG_EPOCH);
    n = tbt_get_u16(in + TBT_FEC_CFG_N);
    m = tbt_get_u16(in + TBT_FEC_CFG_M);
    c->w = tbt_get_u16(in + TBT_FEC_CFG_W);
    c->k = tbt_get_u16(in + TBT_FEC_CFG_K);
    c->d = in[TBT_FEC_CFG_D];
    c->flags = in[TBT_FEC_CFG_FLAGS];
    groups = tbt_get_u16(in + TBT_FEC_CFG_GROUPS);
    c->switch_pkt = tbt_get_u64(in + TBT_FEC_CFG_SWITCH_PKT);
    c->block_base = tbt_get_u64(in + TBT_FEC_CFG_BLOCK_BASE);
    c->seed = tbt_get_u64(in + TBT_FEC_CFG_SEED);
    c->n = n;
    c->m = m;
    c->groups = groups;
    c->crc32c = crc;
    c->reserved = 0;
    variant = in[TBT_FEC_CFG_CODEC_VARIANT];
    L = in[TBT_FEC_CFG_L];
    block = tbt_get_u16(in + TBT_FEC_CFG_BLOCK);
    rate_ppm = tbt_get_u32(in + TBT_FEC_CFG_RATE_PPM);
    c->codec_variant = variant;
    c->L = L;
    c->block = block;
    c->rate_ppm = rate_ppm;
    c->crc32c_ext = tbt_get_u32(in + TBT_FEC_CFG_CRC_EXT);
    c->reserved2 = 0;
    shares = (u32)n + m; /* u32 first: 65535 + 65535 must not wrap */
    if (!n || !m || shares > TBT_FEC_MAX_SHARES)
        return -EPROTO;
    if (!groups || groups > TBT_FEC_MAX_GROUPS ||
        (u32)groups * shares > TBT_FEC_MAX_CARRIERS)
        return -EPROTO;
    if (variant == 2)
    {
        u32 want_n;

        /* SC-LDPC: the extension names the codec, and the wire w/k/d are
         * zero. The sender and receiver recompute n from the rate, so a
         * body whose split does not follow its rate is refused. */
        if (c->w || c->k || c->d)
            return -EPROTO;
        if (L != 8 && L != 16)
            return -EPROTO;
        /* The v8 block ladder is 256/512; the uplink GF(16) codec's
         * 64-share block is a legal variant-2 combination, not a
         * transitional exception. It is valid only with L=8 and more than
         * one group. */
        if (block != 256 && block != 512 &&
            !(block == TBT_FEC_SCLC_BLOCK_UP && groups > 1 &&
              L == TBT_FEC_SCLC_L_UP))
            return -EPROTO;
        if (rate_ppm < 250000u || rate_ppm > 700000u)
            return -EPROTO;
        want_n = (u32)(((u64)rate_ppm * shares) / 1000000u);
        if (want_n != n)
            return -EPROTO;
        /* The uplink GF(16) codec builds only its 0.8.0 shape's splits. A
         * wire body that names 22/10 or 5/27 would install a codec the
         * receiver can never build. */
        if (block == TBT_FEC_SCLC_BLOCK_UP &&
            !tbt_fec_sclc_up_buildable_64_8((u16)n, (u16)m))
            return -EPROTO;
        /* Keep one internal shape vocabulary for the mapping helpers and
         * the codec factory. The wire bytes above were already checked to
         * be zero; these fields are the host-side mirror only. */
        if (groups == 1)
        {
            c->w = block;
            c->k = (u16)((u32)n * 1000u / shares);
            c->d = 0;
        }
        else
        {
            c->w = L;
            c->k = block;
            c->d = (u8)((u32)n * 256u / shares);
        }
        return 0;
    }
    if (variant != 0 && variant != 1)
        return -EPROTO;
    /* RLNC single/grouped: the extension is zero and w/k/d keep their v7
     * meaning (k is the window's data count, d the band width). */
    if (L || block || rate_ppm)
        return -EPROTO;
    if (!c->k || !c->w || c->k > c->w || c->w > TBT_FEC_MAX_W ||
        !c->d || c->d > c->w)
        return -EPROTO;
    if (c->w > 1 && c->d == c->w)
        return -EPROTO;
    {
        /* A grouped RLNC shape carries one codec per group; the group
         * geometry is bounded above. */
        u32 total = (u32)groups * shares;

        if (!total || total > TBT_FEC_MAX_CARRIERS)
            return -EPROTO;
    }
    return 0;
}

int tbt_fec_fbk_pack(const struct tbt_fec_fbk *f, u8 out[TBT_FEC_FBK_LEN])
{
    if (!f || !out)
        return -EINVAL;
    memset(out, 0, TBT_FEC_FBK_LEN);
    tbt_put_u16(out + TBT_FEC_FBK_EPOCH_LO, f->epoch_lo);
    out[TBT_FEC_FBK_M_CURRENT] = f->m_current;
    out[TBT_FEC_FBK_M_TARGET] = f->m_target;
    tbt_put_u32(out + TBT_FEC_FBK_P_EWMA, f->p_ewma_ppm);
    tbt_put_u16(out + TBT_FEC_FBK_EXPECTED, f->expected_shares);
    tbt_put_u16(out + TBT_FEC_FBK_RECEIVED, f->received_shares);
    tbt_put_u16(out + TBT_FEC_FBK_EPOCH_HI, f->epoch_hi);
    out[TBT_FEC_FBK_FLAGS] = f->flags;
    out[TBT_FEC_FBK_N_CURRENT] = f->n_current;
    memcpy(out + TBT_FEC_FBK_BITMAP, f->carrier_bitmap,
           TBT_FEC_BITMAP_BYTES);
    tbt_put_u32(out + TBT_FEC_FBK_P_FAST_PPM, f->p_fast_ppm);
    out[TBT_FEC_FBK_CODEC_VARIANT] = f->codec_variant;
    out[TBT_FEC_FBK_L] = f->L;
    tbt_put_u16(out + TBT_FEC_FBK_BLOCK, f->block);
    tbt_put_u32(out + TBT_FEC_FBK_RATE_CURRENT, f->rate_ppm_current);
    tbt_put_u32(out + TBT_FEC_FBK_RATE_TARGET, f->rate_ppm_target);
    return 0;
}

int tbt_fec_fbk_unpack(const u8 in[TBT_FEC_FBK_LEN], struct tbt_fec_fbk *f)
{
    u8 variant, L;
    u16 block;

    if (!in || !f)
        return -EINVAL;
    if (in[TBT_FEC_FBK_FLAGS] & ~TBT_FEC_FBK_F_MASK)
        return -EPROTO;
    variant = in[TBT_FEC_FBK_CODEC_VARIANT];
    L = in[TBT_FEC_FBK_L];
    block = tbt_get_u16(in + TBT_FEC_FBK_BLOCK);
    if (variant > 2)
        return -EPROTO;
    if (variant == 2)
    {
        /* Same block ladder as FEC_CFG: 256/512, plus 64 for the uplink
         * GF(16) report. */
        if ((L != 8 && L != 16) ||
            (block != 256 && block != 512 &&
             !(block == TBT_FEC_SCLC_BLOCK_UP && L == TBT_FEC_SCLC_L_UP)))
            return -EPROTO;
    }
    else if (L || block)
    {
        return -EPROTO;
    }
    f->epoch_lo = tbt_get_u16(in + TBT_FEC_FBK_EPOCH_LO);
    f->m_current = in[TBT_FEC_FBK_M_CURRENT];
    f->m_target = in[TBT_FEC_FBK_M_TARGET];
    f->p_ewma_ppm = tbt_get_u32(in + TBT_FEC_FBK_P_EWMA);
    f->expected_shares = tbt_get_u16(in + TBT_FEC_FBK_EXPECTED);
    f->received_shares = tbt_get_u16(in + TBT_FEC_FBK_RECEIVED);
    f->epoch_hi = tbt_get_u16(in + TBT_FEC_FBK_EPOCH_HI);
    f->flags = in[TBT_FEC_FBK_FLAGS];
    f->n_current = in[TBT_FEC_FBK_N_CURRENT];
    memcpy(f->carrier_bitmap, in + TBT_FEC_FBK_BITMAP,
           TBT_FEC_BITMAP_BYTES);
    f->p_fast_ppm = tbt_get_u32(in + TBT_FEC_FBK_P_FAST_PPM);
    f->codec_variant = variant;
    f->L = L;
    f->block = block;
    f->rate_ppm_current = tbt_get_u32(in + TBT_FEC_FBK_RATE_CURRENT);
    f->rate_ppm_target = tbt_get_u32(in + TBT_FEC_FBK_RATE_TARGET);
    return 0;
}

/* ------------------------------------------------------------------ *
 * RLNC codec
 * ------------------------------------------------------------------ */

struct tbt_fec_rlnc
{
    u16 n;
    u16 m;
    u16 k;
    u16 w;
    size_t s;
    u16 d;
    u16 band;
    u64 nonce;

    u32 L;
    u32 n_groups;
    u32 n_parity;

    u8 *data_known;
    u8 *parity_known;

    u32 frontier;
    u32 stuck_at;
    u32 recovered_count;

    u8 **data;
    u8 **parity;

    /* Decode workspace, sized in bind(). */
    u64 *mat;
    u8 *rhs_buf;
    u8 **rhs_rows;
    u32 *unknown;
    int *pos;
    u32 *pivot;
    u32 *idx;
};

static u32 tbt_group_count(u32 L, u16 k)
{
    return L / k + (L % k != 0);
}

static void tbt_xor_into(u8 *dst, const u8 *src, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        dst[i] ^= src[i];
}

/* Row r covers the own-data columns c with (c - r) mod m < band. The band
 * is chosen once in tbt_fec_rlnc_new(): it is the widest one whose rows fit
 * d and whose m x k matrix has rank min(m, k). Every column therefore sits
 * in at least one row (band >= 1), so a single lost share is always covered
 * by a parity row. This replaces the old splitmix selection, which could
 * leave a column in no row at all: with n=4, m=4, w=8, d=4, four random
 * rows missed a given share with probability 1/16, and a block with 3 DATA
 * and all 4 parity stayed undecodable. */
static bool tbt_fec_band_has(u16 m, u16 r, u16 c, u16 band)
{
    return (u16)((c + m - (r % m)) % m) < band;
}

static u32 tbt_fec_band_weight(u16 k, u16 m, u16 r, u16 band)
{
    u32 c;
    u32 n = 0;

    for (c = 0; c < k; c++)
        if (tbt_fec_band_has(m, r, (u16)c, band))
            n++;
    return n;
}

/* Rank of the m x k band matrix over GF(2), capped at min(m, k), or
 * TBT_FEC_BAND_RANK_FAIL when the matrix cannot be allocated. Zero is a
 * real rank only for an empty matrix, and every band >= 1 covers every
 * column, so returning 0 on allocation failure would be indistinguishable
 * from "this band has no rank" and silently select a different matrix. */
#define TBT_FEC_BAND_RANK_FAIL (~0u)

static u32 tbt_fec_band_rank(u16 k, u16 m, u16 band)
{
    u32 words = ((u32)k + 63) / 64;
    u64 *mat;
    u32 rank = 0;
    u32 col;

    mat = tbt_fec_alloc((size_t)m * words * sizeof(*mat));
    if (!mat)
        return TBT_FEC_BAND_RANK_FAIL;
    memset(mat, 0, (size_t)m * words * sizeof(*mat));
    for (col = 0; col < m; col++)
    {
        u32 c;

        for (c = 0; c < k; c++)
            if (tbt_fec_band_has(m, (u16)col, (u16)c, band))
                mat[(size_t)col * words + (c >> 6)] |= 1ull << (c & 63);
    }
    for (col = 0; col < k && rank < m; col++)
    {
        u32 word = col >> 6;
        u64 bit = 1ull << (col & 63);
        u32 p;
        u32 r;

        for (p = rank; p < m; p++)
            if (mat[(size_t)p * words + word] & bit)
                break;
        if (p == m)
            continue;
        if (p != rank)
        {
            u32 w;

            for (w = 0; w < words; w++)
            {
                u64 t = mat[(size_t)rank * words + w];

                mat[(size_t)rank * words + w] = mat[(size_t)p * words + w];
                mat[(size_t)p * words + w] = t;
            }
        }
        for (r = 0; r < m; r++)
        {
            u32 w;

            if (r == rank || !(mat[(size_t)r * words + word] & bit))
                continue;
            for (w = 0; w < words; w++)
                mat[(size_t)r * words + w] ^= mat[(size_t)rank * words + w];
        }
        rank++;
    }
    tbt_fec_free(mat);
    return rank;
}

/* Widest band that fits d terms per row and keeps rank min(m, k), or 0 when
 * the rank matrix cannot be allocated. b == 1 is the partition matrix
 * (column c belongs to row c mod m), which always fits: its row weight is
 * ceil(k/m), at most d in every configured shape. The b == 1 fallback needs
 * no rank probe, so an allocation failure cannot change the chosen band. */
static u16 tbt_fec_band_pick(u16 k, u16 m, u16 d)
{
    u32 target = m < k ? m : k;
    u16 band;

    for (band = (u16)(m > 1 ? m - 1 : 1); band > 1; band--)
    {
        u32 r;
        u32 wmax = 0;
        u32 rank;

        for (r = 0; r < m; r++)
        {
            u32 wt = tbt_fec_band_weight(k, m, (u16)r, band);

            if (wt > wmax)
                wmax = wt;
        }
        if (wmax > d)
            continue;
        rank = tbt_fec_band_rank(k, m, band);
        if (rank == TBT_FEC_BAND_RANK_FAIL)
            return 0;
        if (rank == target)
            return band;
    }
    return 1;
}

u32 tbt_fec_rlnc_coeffs(const struct tbt_fec_rlnc *ctx, u32 g, u32 r, u32 *idx, u32 max)
{
    u32 half;
    u32 c;
    u32 n = 0;

    if (!ctx || !idx || max < ctx->d)
        return 0;
    (void)g;
    half = ctx->w - ctx->k;
    for (c = 0; c < ctx->k && n < ctx->d; c++)
        if (tbt_fec_band_has(ctx->m, (u16)r, (u16)c, ctx->band))
            idx[n++] = half + c;
    /* Fill the rest of the row with the window's leading positions. The data
     * plane binds L == k, so those positions fall outside [0, L) and both
     * sides skip them; keeping them means a row still names d positions and
     * the wire format is unchanged. */
    for (c = 0; c < half && n < ctx->d; c++)
        idx[n++] = c;
    return n;
}

struct tbt_fec_rlnc *tbt_fec_rlnc_new(u16 n, u16 m, u16 k, u16 w, size_t s, u16 d,
                                      u64 nonce)
{
    struct tbt_fec_rlnc *ctx;

    if (!n || !m || !k || w < k || w > TBT_FEC_MAX_W || !s || !d || d > w ||
        d > TBT_FEC_MAX_D)
        return NULL;
    /* Widen before the add: n=65535, m=5 must not wrap to 4 and pass. */
    if ((u32)n + m > TBT_FEC_MAX_SHARES)
        return NULL;
    /* d == w makes every parity row the all-ones row (rank 1). Refuse it
     * unless the window itself is one packet, where it is the only choice. */
    if (w > 1 && d == w)
        return NULL;
    ctx = tbt_fec_alloc(sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->n = n;
    ctx->m = m;
    ctx->k = k;
    ctx->w = w;
    ctx->s = s;
    ctx->d = d;
    ctx->nonce = nonce;
    ctx->band = tbt_fec_band_pick(k, m, d);
    if (!ctx->band)
    {
        /* No codec may be built with a band that was never verified: two
         * ends could then select different matrices under memory pressure. */
        tbt_fec_rlnc_free(ctx);
        return NULL;
    }
    ctx->idx = tbt_fec_alloc((size_t)w * sizeof(*ctx->idx));
    if (!ctx->idx)
    {
        tbt_fec_rlnc_free(ctx);
        return NULL;
    }
    return ctx;
}

void tbt_fec_rlnc_free(struct tbt_fec_rlnc *ctx)
{
    if (!ctx)
        return;
    tbt_fec_free(ctx->data_known);
    tbt_fec_free(ctx->parity_known);
    tbt_fec_free(ctx->mat);
    tbt_fec_free(ctx->rhs_buf);
    tbt_fec_free(ctx->rhs_rows);
    tbt_fec_free(ctx->unknown);
    tbt_fec_free(ctx->pos);
    tbt_fec_free(ctx->pivot);
    tbt_fec_free(ctx->idx);
    tbt_fec_free(ctx);
}

void tbt_fec_rlnc_set_nonce(struct tbt_fec_rlnc *ctx, u64 nonce)
{
    if (ctx)
        ctx->nonce = nonce;
}

u16 tbt_fec_rlnc_band(const struct tbt_fec_rlnc *ctx)
{
    return ctx ? ctx->band : 0;
}

u64 tbt_fec_rlnc_nonce(const struct tbt_fec_rlnc *ctx)
{
    return ctx ? ctx->nonce : 0;
}

u32 tbt_fec_rlnc_parity_count(const struct tbt_fec_rlnc *ctx, u32 L)
{
    if (!ctx || !L)
        return 0;
    return tbt_group_count(L, ctx->k) * ctx->m;
}

static int tbt_fec_rlnc_bind(struct tbt_fec_rlnc *ctx, u8 *data[], u8 *parity[], u32 L)
{
    u32 groups;
    u32 parity_count;
    u32 words;
    u32 e;

    if (!ctx || !data || !parity || !L || L > TBT_FEC_MAX_W)
        return TBT_FEC_RLNC_ERR_PARAM;
    groups = tbt_group_count(L, ctx->k);
    parity_count = groups * ctx->m;
    words = (L + 63) / 64;

    if (ctx->L != L)
    {
        tbt_fec_free(ctx->data_known);
        tbt_fec_free(ctx->parity_known);
        tbt_fec_free(ctx->mat);
        tbt_fec_free(ctx->rhs_buf);
        tbt_fec_free(ctx->rhs_rows);
        tbt_fec_free(ctx->unknown);
        tbt_fec_free(ctx->pos);
        tbt_fec_free(ctx->pivot);
        ctx->data_known = NULL;
        ctx->parity_known = NULL;
        ctx->mat = NULL;
        ctx->rhs_buf = NULL;
        ctx->rhs_rows = NULL;
        ctx->unknown = NULL;
        ctx->pos = NULL;
        ctx->pivot = NULL;

        ctx->data_known = tbt_fec_alloc(L);
        ctx->parity_known = tbt_fec_alloc(parity_count);
        ctx->mat = tbt_fec_alloc((size_t)parity_count * words * sizeof(u64));
        ctx->rhs_buf = tbt_fec_alloc((size_t)parity_count * ctx->s);
        ctx->rhs_rows = tbt_fec_alloc((size_t)parity_count * sizeof(u8 *));
        ctx->unknown = tbt_fec_alloc((size_t)L * sizeof(u32));
        ctx->pos = tbt_fec_alloc((size_t)L * sizeof(int));
        ctx->pivot = tbt_fec_alloc((size_t)(parity_count < L ? parity_count : L) *
                                   sizeof(u32));
        if (!ctx->data_known || !ctx->parity_known || !ctx->mat || !ctx->rhs_buf ||
            !ctx->rhs_rows || !ctx->unknown || !ctx->pos || !ctx->pivot)
        {
            ctx->L = 0;
            return TBT_FEC_RLNC_ERR_NOMEM;
        }
        for (e = 0; e < parity_count; e++)
            ctx->rhs_rows[e] = ctx->rhs_buf + (size_t)e * ctx->s;
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
    return TBT_FEC_RLNC_OK;
}

int tbt_fec_rlnc_encode(struct tbt_fec_rlnc *ctx, u8 *data[], u32 L, u8 *parity[])
{
    u32 groups;
    u32 g;
    u32 r;
    u32 i;

    if (!ctx || !data || !parity || !L)
        return TBT_FEC_RLNC_ERR_PARAM;
    for (i = 0; i < L; i++)
        if (!data[i])
            return TBT_FEC_RLNC_ERR_PARAM;
    groups = tbt_group_count(L, ctx->k);
    for (i = 0; i < groups * ctx->m; i++)
        if (!parity[i])
            return TBT_FEC_RLNC_ERR_PARAM;

    for (g = 0; g < groups; g++)
    {
        int ws = (int)(g * ctx->k) + (int)ctx->k - (int)ctx->w;

        for (r = 0; r < ctx->m; r++)
        {
            u8 *dst = parity[(size_t)g * ctx->m + r];
            u32 nidx;

            memset(dst, 0, ctx->s);
            nidx = tbt_fec_rlnc_coeffs(ctx, g, r, ctx->idx, ctx->w);
            for (i = 0; i < nidx; i++)
            {
                int abs = ws + (int)ctx->idx[i];

                if (abs >= 0 && abs < (int)L)
                    tbt_xor_into(dst, data[abs], ctx->s);
            }
        }
    }
    return (int)(groups * ctx->m);
}

static int tbt_fec_rlnc_recover(struct tbt_fec_rlnc *ctx)
{
    u32 L = ctx->L;
    u32 words = (L + 63) / 64;
    u32 u = 0;
    u32 e = 0;
    u32 g;
    u32 r;
    u32 i;
    u32 rank;
    u32 col;

    for (i = 0; i < L; i++)
    {
        ctx->pos[i] = -1;
        if (!ctx->data_known[i])
        {
            ctx->pos[i] = (int)u;
            ctx->unknown[u++] = i;
        }
    }
    if (!u)
        return 0;

    for (g = 0; g < ctx->n_groups; g++)
    {
        int ws = (int)(g * ctx->k) + (int)ctx->k - (int)ctx->w;

        for (r = 0; r < ctx->m; r++)
        {
            u32 nidx;
            u64 *row;
            u8 *rhs;

            if (!ctx->parity_known[(size_t)g * ctx->m + r])
                continue;
            row = ctx->mat + (size_t)e * words;
            rhs = ctx->rhs_rows[e];
            memset(row, 0, (size_t)words * sizeof(u64));
            memcpy(rhs, ctx->parity[(size_t)g * ctx->m + r], ctx->s);

            nidx = tbt_fec_rlnc_coeffs(ctx, g, r, ctx->idx, ctx->w);
            for (i = 0; i < nidx; i++)
            {
                int abs = ws + (int)ctx->idx[i];
                int p;

                if (abs < 0 || abs >= (int)L)
                    continue;
                p = ctx->pos[abs];
                if (p < 0)
                    tbt_xor_into(rhs, ctx->data[abs], ctx->s);
                else
                    row[(u32)p >> 6] |= 1ull << (p & 63);
            }
            e++;
        }
    }
    if (!e)
        return TBT_FEC_RLNC_ERR_STUCK;

    rank = 0;
    for (col = 0; col < u && rank < e; col++)
    {
        u32 p;
        u64 bit = 1ull << (col & 63);
        u32 word = col >> 6;
        u32 rr;

        for (p = rank; p < e; p++)
            if (ctx->mat[(size_t)p * words + word] & bit)
                break;
        if (p == e)
            continue;
        if (p != rank)
        {
            u32 k;
            u8 *tmp;

            for (k = 0; k < words; k++)
            {
                u64 t = ctx->mat[(size_t)rank * words + k];

                ctx->mat[(size_t)rank * words + k] = ctx->mat[(size_t)p * words + k];
                ctx->mat[(size_t)p * words + k] = t;
            }
            tmp = ctx->rhs_rows[rank];
            ctx->rhs_rows[rank] = ctx->rhs_rows[p];
            ctx->rhs_rows[p] = tmp;
        }
        for (rr = 0; rr < e; rr++)
        {
            u32 k;

            if (rr == rank)
                continue;
            if (!(ctx->mat[(size_t)rr * words + word] & bit))
                continue;
            for (k = 0; k < words; k++)
                ctx->mat[(size_t)rr * words + k] ^= ctx->mat[(size_t)rank * words + k];
            tbt_xor_into(ctx->rhs_rows[rr], ctx->rhs_rows[rank], ctx->s);
        }
        ctx->pivot[rank] = col;
        rank++;
    }
    if (rank < u)
        return TBT_FEC_RLNC_ERR_STUCK;

    for (i = 0; i < u; i++)
    {
        u32 idx = ctx->unknown[ctx->pivot[i]];

        memcpy(ctx->data[idx], ctx->rhs_rows[i], ctx->s);
        ctx->data_known[idx] = 1;
        ctx->recovered_count++;
    }
    return (int)u;
}

static void tbt_fec_rlnc_update_frontier(struct tbt_fec_rlnc *ctx)
{
    while (ctx->frontier < ctx->L && ctx->data_known[ctx->frontier])
        ctx->frontier++;
    ctx->stuck_at = ctx->frontier;
}

int tbt_fec_rlnc_decode(struct tbt_fec_rlnc *ctx, u8 *data[], u8 *parity[],
                        const u8 *data_present, const u8 *parity_present, u32 L)
{
    u32 i;
    int rc;

    if (!ctx || !data || !parity || !data_present || !parity_present || !L ||
        L > TBT_FEC_MAX_W)
        return TBT_FEC_RLNC_ERR_PARAM;
    rc = tbt_fec_rlnc_bind(ctx, data, parity, L);
    if (rc != TBT_FEC_RLNC_OK)
        return rc;
    for (i = 0; i < L; i++)
        ctx->data_known[i] = data_present[i] ? 1 : 0;
    for (i = 0; i < ctx->n_parity; i++)
        ctx->parity_known[i] = parity_present[i] ? 1 : 0;
    rc = tbt_fec_rlnc_recover(ctx);
    tbt_fec_rlnc_update_frontier(ctx);
    return rc;
}

int tbt_fec_rlnc_advance(struct tbt_fec_rlnc *ctx, int kind, u32 index)
{
    int rc;

    if (!ctx || !ctx->L || !ctx->data || !ctx->parity || ctx->L > TBT_FEC_MAX_W)
        return TBT_FEC_RLNC_ERR_PARAM;
    if (kind == TBT_FEC_RLNC_DATA_KNOWN)
    {
        if (index >= ctx->L)
            return TBT_FEC_RLNC_ERR_PARAM;
        if (ctx->data_known[index])
            return (int)ctx->frontier;
        ctx->data_known[index] = 1;
    }
    else if (kind == TBT_FEC_RLNC_DATA_MISSING)
    {
        /* The caller reports a hole. Keep the packet unknown and let the
         * parity equations already present try to fill it. */
        if (index >= ctx->L)
            return TBT_FEC_RLNC_ERR_PARAM;
        if (ctx->data_known[index])
            return (int)ctx->frontier;
    }
    else if (kind == TBT_FEC_RLNC_PARITY_KNOWN)
    {
        if (index >= ctx->n_parity)
            return TBT_FEC_RLNC_ERR_PARAM;
        if (ctx->parity_known[index])
            return (int)ctx->frontier;
        ctx->parity_known[index] = 1;
    }
    else
    {
        return TBT_FEC_RLNC_ERR_PARAM;
    }
    rc = tbt_fec_rlnc_recover(ctx);
    tbt_fec_rlnc_update_frontier(ctx);
    return rc < 0 ? (int)ctx->frontier : (int)ctx->frontier;
}

u32 tbt_fec_rlnc_frontier(const struct tbt_fec_rlnc *ctx)
{
    return ctx ? ctx->frontier : 0;
}

u32 tbt_fec_rlnc_stuck_at(const struct tbt_fec_rlnc *ctx)
{
    return ctx ? ctx->stuck_at : 0;
}

u32 tbt_fec_rlnc_recovered(const struct tbt_fec_rlnc *ctx)
{
    return ctx ? ctx->recovered_count : 0;
}

int tbt_fec_rlnc_is_known(const struct tbt_fec_rlnc *ctx, u32 index)
{
    if (!ctx || index >= ctx->L)
        return 0;
    return ctx->data_known[index] != 0;
}

/* ------------------------------------------------------------------ *
 * Grouped mode
 * ------------------------------------------------------------------ */

struct tbt_fec_grouped
{
    const struct tbt_fec_ops *ops;
    u16 groups;
    u16 n;
    u16 m;
    u16 k;
    u16 w;
    size_t s;
    u16 d;
    u64 nonce;
    void *ctx[TBT_FEC_MAX_GROUPS];
};

struct tbt_fec_grouped *tbt_fec_grouped_new_ops(const struct tbt_fec_ops *ops,
                                                u16 groups, u16 n, u16 m,
                                                u16 k, u16 w, size_t s,
                                                u16 d, u64 nonce)
{
    struct tbt_fec_grouped *g;
    u16 i;

    if (!ops || !ops->new || !ops->free || !ops->encode || !ops->decode)
        return NULL;
    if (!groups || groups > TBT_FEC_MAX_GROUPS)
        return NULL;
    g = tbt_fec_alloc(sizeof(*g));
    if (!g)
        return NULL;
    g->ops = ops;
    g->groups = groups;
    g->n = n;
    g->m = m;
    g->k = k;
    g->w = w;
    g->s = s;
    g->d = d;
    g->nonce = nonce;
    for (i = 0; i < groups; i++)
    {
        g->ctx[i] = ops->new(n, m, k, w, s, d, nonce ^ ((u64)i << 32));
        if (!g->ctx[i])
        {
            tbt_fec_grouped_free(g);
            return NULL;
        }
    }
    return g;
}

struct tbt_fec_grouped *tbt_fec_grouped_new(u16 groups, u16 n, u16 m, u16 k, u16 w,
                                            size_t s, u16 d, u64 nonce)
{
    return tbt_fec_grouped_new_ops(&tbt_fec_ops_rlnc, groups, n, m, k, w, s,
                                   d, nonce);
}

void tbt_fec_grouped_free(struct tbt_fec_grouped *g)
{
    u16 i;

    if (!g)
        return;
    for (i = 0; i < g->groups; i++)
        if (g->ctx[i])
            g->ops->free(g->ctx[i]);
    tbt_fec_free(g);
}

u16 tbt_fec_grouped_count(const struct tbt_fec_grouped *g)
{
    return g ? g->groups : 0;
}

struct tbt_fec_rlnc *tbt_fec_grouped_ctx(struct tbt_fec_grouped *g, u16 group)
{
    if (!g || group >= g->groups || g->ops->mode != 1)
        return NULL;
    return g->ctx[group];
}

u8 tbt_fec_grouped_mode(const struct tbt_fec_grouped *g)
{
    return g ? g->ops->mode : 0;
}

int tbt_fec_grouped_encode(struct tbt_fec_grouped *g, u16 group, u8 *data[],
                           u32 L, u8 *parity[])
{
    if (!g || group >= g->groups)
        return -EINVAL;
    return g->ops->encode(g->ctx[group], data, L, parity);
}

int tbt_fec_grouped_decode(struct tbt_fec_grouped *g, u16 group, u8 *data[],
                           u8 *parity[], const u8 *data_present,
                           const u8 *parity_present, u32 L)
{
    if (!g || group >= g->groups)
        return -EINVAL;
    return g->ops->decode(g->ctx[group], data, parity, data_present,
                          parity_present, L);
}

void tbt_fec_grouped_set_nonce(struct tbt_fec_grouped *g, u16 group,
                               u64 nonce)
{
    if (!g || group >= g->groups || g->ops->mode != 1)
        return;
    tbt_fec_rlnc_set_nonce(g->ctx[group], nonce);
}

u16 tbt_fec_grouped_band(const struct tbt_fec_grouped *g, u16 group)
{
    if (!g || group >= g->groups || g->ops->mode != 1)
        return 0;
    return tbt_fec_rlnc_band(g->ctx[group]);
}

int tbt_fec_grouped_is_known(const struct tbt_fec_grouped *g, u16 group,
                             u32 index)
{
    if (!g || group >= g->groups || !g->ops->is_known)
        return 0;
    return g->ops->is_known(g->ctx[group], index);
}

/* ------------------------------------------------------------------ *
 * Dynamic parameters
 * ------------------------------------------------------------------ */

u16 tbt_fec_m_for_p(u32 p_ppm, u16 group_size)
{
    u16 m;

    if (group_size < 2)
        return 0;
    if (p_ppm <= 10000)
        m = 3;
    else if (p_ppm <= 200000)
        m = 4;
    else if (p_ppm <= 300000)
        m = 5;
    else
        m = 6;
    if (m >= group_size)
        m = (u16)(group_size - 1);
    if (!m)
        m = 1;
    return m;
}

int tbt_fec_params_single(u32 p_ppm, u16 carriers, struct tbt_fec_params *out)
{
    if (!out || carriers != TBT_FEC_MAX_CARRIERS || p_ppm > 200000)
        return -ERANGE;
    out->codec = TBT_FEC_CODEC_RLNC_SINGLE;
    out->groups = 1;
    out->group_size = TBT_FEC_MAX_CARRIERS;
    out->m = 64;
    out->n = (u16)(TBT_FEC_MAX_CARRIERS - out->m);
    out->k = out->n;
    out->w = (u16)(2 * out->n);
    out->d = 64;
    out->nonce = 0;
    return 0;
}

int tbt_fec_params_grouped(u32 p_ppm, u16 groups, u16 group_size,
                           struct tbt_fec_params *out)
{
    u16 m;

    if (!out || !groups || groups > TBT_FEC_MAX_GROUPS || group_size < 2 ||
        (u32)groups * group_size > TBT_FEC_MAX_SHARES)
        return -EINVAL;
    m = tbt_fec_m_for_p(p_ppm, group_size);
    if (!m || m >= group_size)
        return -EINVAL;
    out->codec = TBT_FEC_CODEC_RLNC_GROUPED;
    out->groups = groups;
    out->group_size = group_size;
    out->m = m;
    out->n = (u16)(group_size - m);
    out->k = out->n;
    out->w = (u16)(2 * out->n);
    if (out->w > TBT_FEC_MAX_W)
        return -EINVAL;
    out->d = (u16)(out->w / 2);
    if (!out->d)
        out->d = 1;
    if (out->w > 1 && out->d >= out->w)
        out->d = (u16)(out->w - 1);
    out->nonce = 0;
    return 0;
}

int tbt_fec_params_dynamic(u32 p_ppm, u16 carriers, struct tbt_fec_params *out)
{
    u16 group_size;
    u16 groups;

    if (!out || carriers < 2 || carriers > TBT_FEC_MAX_CARRIERS)
        return -EINVAL;
    if (carriers == TBT_FEC_MAX_CARRIERS && p_ppm <= 200000)
        return tbt_fec_params_single(p_ppm, carriers, out);
    group_size = carriers < 8 ? carriers : 8;
    groups = (u16)(carriers / group_size);
    if (!groups || (u32)groups * group_size != carriers)
    {
        group_size = carriers;
        groups = 1;
    }
    return tbt_fec_params_grouped(p_ppm, groups, group_size, out);
}

int tbt_fec_params_check(const struct tbt_fec_params *p)
{
    u32 shares;

    if (!p)
        return -EINVAL;
    /* SC-LDPC shares the n/m/groups geometry and reuses k/w/d for the code
     * rate and the codeword block, so it needs its own rules rather than the
     * RLNC window ones. */
    if (p->codec == TBT_FEC_CODEC_SC_LDPC)
    {
        if (!p->groups || p->groups > TBT_FEC_MAX_GROUPS || !p->group_size)
            return -EINVAL;
        if (!p->n || !p->m ||
            (u32)p->n + p->m != p->group_size ||
            (u32)p->groups * p->group_size > TBT_FEC_MAX_SHARES)
            return -EINVAL;
        if (!p->k || p->k < 100 || p->k > 1000)
            return -EINVAL;
        if (p->d || !p->w || p->w > TBT_FEC_MAX_W)
            return -EINVAL;
        return 0;
    }
    if (p->codec != TBT_FEC_CODEC_RLNC_SINGLE &&
        p->codec != TBT_FEC_CODEC_RLNC_GROUPED)
        return -EINVAL;
    if (!p->groups || p->groups > TBT_FEC_MAX_GROUPS || p->group_size < 2)
        return -EINVAL;
    if ((u32)p->groups * p->group_size > TBT_FEC_MAX_SHARES)
        return -EINVAL;
    if (!p->n || !p->m)
        return -EINVAL;
    /* Widen before the add: with u16 arithmetic n=65535, m=5 sums to 4 and
     * matches a 4-share group_size, bypassing the share bound below. */
    shares = (u32)p->n + p->m;
    if (shares != p->group_size || shares > TBT_FEC_MAX_SHARES)
        return -EINVAL;
    if (!p->k || p->w < p->k || p->w > TBT_FEC_MAX_W)
        return -EINVAL;
    if (!p->d || p->d > p->w || p->d > TBT_FEC_MAX_D ||
        (p->w > 1 && p->d == p->w))
        return -EINVAL;
    if (p->codec == TBT_FEC_CODEC_RLNC_SINGLE && p->groups != 1)
        return -EINVAL;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Codec ops table (T0 §1.7). SC-LDPC supplies the same shape from D.
 * ------------------------------------------------------------------ */

static void *tbt_fec_ops_rlnc_new(u16 n, u16 m, u16 k, u16 w, size_t s, u16 d,
                                  u64 nonce)
{
    return tbt_fec_rlnc_new(n, m, k, w, s, d, nonce);
}

static void tbt_fec_ops_rlnc_free(void *ctx)
{
    tbt_fec_rlnc_free(ctx);
}

static u32 tbt_fec_ops_rlnc_parity_count(const void *ctx, u32 L)
{
    return tbt_fec_rlnc_parity_count(ctx, L);
}

static u32 tbt_fec_ops_rlnc_coeffs(const void *ctx, u32 g, u32 r, u32 *idx, u32 max)
{
    return tbt_fec_rlnc_coeffs(ctx, g, r, idx, max);
}

static int tbt_fec_ops_rlnc_encode(void *ctx, u8 *data[], u32 L, u8 *parity[])
{
    return tbt_fec_rlnc_encode(ctx, data, L, parity);
}

static int tbt_fec_ops_rlnc_decode(void *ctx, u8 *data[], u8 *parity[],
                                   const u8 *data_present, const u8 *parity_present, u32 L)
{
    return tbt_fec_rlnc_decode(ctx, data, parity, data_present, parity_present, L);
}

static int tbt_fec_ops_rlnc_advance(void *ctx, int kind, u32 index)
{
    return tbt_fec_rlnc_advance(ctx, kind, index);
}

static u32 tbt_fec_ops_rlnc_frontier(const void *ctx)
{
    return tbt_fec_rlnc_frontier(ctx);
}

static u32 tbt_fec_ops_rlnc_stuck_at(const void *ctx)
{
    return tbt_fec_rlnc_stuck_at(ctx);
}

static u32 tbt_fec_ops_rlnc_recovered(const void *ctx)
{
    return tbt_fec_rlnc_recovered(ctx);
}

static int tbt_fec_ops_rlnc_is_known(const void *ctx, u32 index)
{
    return tbt_fec_rlnc_is_known(ctx, index);
}

const struct tbt_fec_ops tbt_fec_ops_rlnc = {
    .name = "rlnc",
    .mode = 1,
    .new = tbt_fec_ops_rlnc_new,
    .free = tbt_fec_ops_rlnc_free,
    .parity_count = tbt_fec_ops_rlnc_parity_count,
    .coeffs = tbt_fec_ops_rlnc_coeffs,
    .encode = tbt_fec_ops_rlnc_encode,
    .decode = tbt_fec_ops_rlnc_decode,
    .advance = tbt_fec_ops_rlnc_advance,
    .frontier = tbt_fec_ops_rlnc_frontier,
    .stuck_at = tbt_fec_ops_rlnc_stuck_at,
    .recovered = tbt_fec_ops_rlnc_recovered,
    .is_known = tbt_fec_ops_rlnc_is_known,
};

/* ------------------------------------------------------------------ *
 * SC-LDPC shape selection (mode 3)
 *
 * The shape alone names the variant: one group is the binary downlink block
 * of 256 shares; more than one group is the GF(16) uplink, one block per
 * group. The rate is derived from n/(n+m) and never configured, so a shape
 * can never claim more parity capacity than its graph produces.
 * ------------------------------------------------------------------ */

int tbt_fec_sclc_shape_of_cfg(const struct tbt_fec_cfg *cfg, u16 carriers,
                              struct tbt_fec_sclc_shape *out)
{
    u32 shares;

    if (!cfg || !out)
        return -EINVAL;
    /* v8 names the codec in the extension; the legacy w/k/d classification
     * is gone from the wire. */
    if (cfg->codec_variant != 2 || !cfg->n || !cfg->m)
        return -EINVAL;
    shares = (u32)cfg->n + cfg->m;
    if (!shares || shares > TBT_FEC_MAX_SHARES)
        return -EINVAL;
    if (cfg->L != 8 && cfg->L != 16)
        return -EINVAL;
    /* 256/512 are the v8 ladder; 64 is the uplink GF(16) block. */
    if (cfg->block != 256 && cfg->block != 512 &&
        !(cfg->block == TBT_FEC_SCLC_BLOCK_UP && cfg->groups > 1 &&
          cfg->L == TBT_FEC_SCLC_L_UP))
        return -EINVAL;
    if (cfg->rate_ppm < 250000u || cfg->rate_ppm > 700000u)
        return -EINVAL;
    if ((u32)(((u64)cfg->rate_ppm * shares) / 1000000u) != cfg->n)
        return -EINVAL;
    if (cfg->block == TBT_FEC_SCLC_BLOCK_UP && shares == 32 &&
        !tbt_fec_sclc_up_buildable_64_8(cfg->n, cfg->m))
        return -EINVAL;
    if (tbt_fec_sclc_shape_of(cfg->n, cfg->m, carriers, cfg->block,
                              cfg->L, out))
        return -EINVAL;
    if (out->block != cfg->block || out->L != cfg->L)
        return -EINVAL;
    return 0;
}

int tbt_fec_sclc_shape_of(u16 n, u16 m, u16 carriers, u16 block, u16 L,
                          struct tbt_fec_sclc_shape *out)
{
    u32 shares = (u32)n + m;
    u16 groups;

    if (!out || !n || !m || !carriers || !shares ||
        shares > TBT_FEC_MAX_SHARES || carriers % shares)
        return -EINVAL;
    groups = (u16)(carriers / shares);
    if (!groups || groups > TBT_FEC_MAX_GROUPS)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->n = n;
    out->m = m;
    out->groups = groups;
    out->rate_ppm = (u16)((u32)n * 1000u / shares);
    out->rate_256 = (u16)((u32)n * 256u / shares);
    if (groups == 1 && shares == TBT_FEC_MAX_SHARES)
    {
        /* The binary codec builds the six ladder splits of the 256 shares
         * (25..70 percent). Above 50 percent it adds one check per parity
         * share and lowers the payload degree, so every point on the ladder
         * is a real encoded shape with no punctured parity, not a
         * share-count guess. */
        if (!tbt_fec_sclc_down_split(n, m))
            return -EINVAL;
        out->variant = TBT_FEC_SCLC_DOWN;
        out->block = block ? block : TBT_FEC_SCLC_BLOCK_DOWN;
        out->L = L ? L : TBT_FEC_SCLC_L_DOWN;
    }
    else if (groups > 1)
    {
        out->variant = TBT_FEC_SCLC_UP;
        out->block = block ? block : TBT_FEC_SCLC_BLOCK_UP;
        out->L = L ? L : TBT_FEC_SCLC_L_UP;
        if (!tbt_fec_sclc_up_buildable_64_8(n, m))
            return -EINVAL;
    }
    else
    {
        /* One group of fewer than 256 shares is neither shipped shape. The
         * binary codec is defined on the 256-share block; a smaller single
         * group has no wire block to carry its shares. */
        return -EINVAL;
    }
    if (!out->block || (out->block & 1) || !out->L)
        return -EINVAL;
    if (!out->rate_ppm || !out->rate_256)
        return -EINVAL;
    return 0;
}
