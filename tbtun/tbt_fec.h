/* tbt_fec.h - FEC wire v1 (TBT_FEC_CONV 0x1001), control frames, and the
 * GF(2) sliding-window RLNC codec.
 *
 * This file and tbt_fec.c compile twice: into the module and into the host
 * tests. They use no kernel facility a plain C program lacks. Allocation goes
 * through tbt_fec_alloc()/tbt_fec_free(), which map to kvzalloc/kvfree in the
 * module and calloc/free on the host. Those may sleep: create/free a codec
 * from process context, never from softirq. The per-packet encode/decode paths
 * allocate nothing after bind().
 *
 * The wire layout is frozen by T0 (review/goal-v6-interfaces.md). Any change
 * to an offset, a length, or a type byte is an ABI change: bump TBT_FEC_CONV
 * and update the host test vectors in the same commit.
 *
 * Scope:
 *   - 40-byte share header: pack/unpack, structural checks, both CRCs.
 *   - 256-bit carrier bitmap (32 bytes), independent of the old u16
 *     path_ack_mask.
 *   - ACK / NACK / FEC_CFG / FEC_FBK control bodies.
 *   - RLNC encode/decode over GF(2), one group; grouped mode composes one
 *     codec per group; dynamic parameters choose M from the measured p.
 *
 * Not in scope: SC-LDPC. The codec enum reserves the value; D owns the
 * implementation and its wire mapping. No fake stub lives here.
 */
#ifndef _TBT_FEC_H
#define _TBT_FEC_H

#include "tbt_uapi.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

/* ------------------------------------------------------------------ *
 * 40-byte share header
 * ------------------------------------------------------------------ */

struct tbt_fec_hdr
{
    u8 type;    /* TBT_FEC_DATA .. TBT_FEC_FBK, TBT_FEC_PAD */
    u8 flags;   /* TBT_FEC_FF_LAST only */
    u16 share_index;
    u8 n;
    u8 m;
    u16 epoch; /* this cut: the block's last real share length, 0 = full */
    u64 block_id;
    u64 seq;
    u16 win_len;
    u16 payload_len;
    u16 conv;
    u16 reserved;
    u32 payload_crc32c; /* payload only */
    u32 header_crc32c;  /* header bytes +0..+35, this field excluded */
};

/* Pack h into out[40] (little-endian). Returns 0, or -EINVAL for a NULL
 * argument. Pack does not validate; call tbt_fec_hdr_check() before sending. */
int tbt_fec_hdr_pack(const struct tbt_fec_hdr *h, u8 out[TBT_FEC_HDR_LEN]);

/* Unpack in[40] into h. Returns 0, or -EINVAL for a NULL argument. It copies
 * bytes only; a caller must still run tbt_fec_hdr_check() and the CRC checks. */
int tbt_fec_hdr_unpack(const u8 in[TBT_FEC_HDR_LEN], struct tbt_fec_hdr *h);

/* Structural check, in this order: conv, type, flags, reserved, lengths,
 * share bounds. `max_payload` is the configured S. Returns 0 or -EPROTO.
 *
 * DATA/PARITY: n>=1, m>=1, n+m<=256, share_index < (u16)(n+m),
 *              DATA share_index < n, PARITY share_index >= n,
 *              DATA payload_len 1..max_payload (a short packet is legal
 *              anywhere: the packetizer flushes a partial packet when the
 *              application goes quiet, and the receiver places shares by the
 *              sum of the payload lengths, so only LAST marks the end),
 *              PARITY payload_len == max_payload, win_len rules
 *              (DATA 0; PARITY 1..TBT_FEC_MAX_W), seq < 2^63.
 * ACK/NACK/CFG/FBK/PAD: n==0, m==0, share_index==0, win_len==0, exact body
 *              lengths, payload_len <= max_payload.
 *
 * The caller must check header_crc32c before trusting any field; this
 * function is the second gate, not the first. */
int tbt_fec_hdr_check(const struct tbt_fec_hdr *h, u16 max_payload);

/* Standard CRC32C (Castagnoli), reflected, init ~0, final xor. A fresh CRC
 * uses seed 0; chaining uses the previous return value as the next seed. */
u32 tbt_fec_crc32c(u32 seed, const void *data, size_t len);

/* Fill payload_crc32c and header_crc32c. `payload` is payload_len bytes; it
 * may be NULL only when payload_len is 0. Returns 0 or -EINVAL. */
int tbt_fec_hdr_seal(struct tbt_fec_hdr *h, const u8 *payload);

/* Verify header_crc32c first, then payload_crc32c. Returns 0, -EBADMSG on a
 * CRC mismatch, or -EINVAL on a NULL payload with a nonzero length. */
int tbt_fec_hdr_verify(const struct tbt_fec_hdr *h, const u8 *payload);

/* ------------------------------------------------------------------ *
 * 256-bit carrier bitmap
 * ------------------------------------------------------------------ */

void tbt_fec_bitmap_set(u8 bm[TBT_FEC_BITMAP_BYTES], u16 index);
void tbt_fec_bitmap_clear(u8 bm[TBT_FEC_BITMAP_BYTES], u16 index);
bool tbt_fec_bitmap_test(const u8 bm[TBT_FEC_BITMAP_BYTES], u16 index);
u16 tbt_fec_bitmap_count(const u8 bm[TBT_FEC_BITMAP_BYTES]);

/* ------------------------------------------------------------------ *
 * Control bodies (payload after the 40-byte header)
 * ------------------------------------------------------------------ */

struct tbt_fec_ack
{
    u64 next_deliver;
    u32 credit_packets;
    u32 rtt_us;
};

struct tbt_fec_nack_item
{
    u64 missing_seq;
    u16 count;
    u16 reserved;
};

struct tbt_fec_cfg
{
    u32 epoch;
    u16 n;
    u16 m;
    u16 w;
    u16 k;
    u8 d;
    u8 reserved[3];
    u64 seed;
    u32 crc32c; /* covers the first 24 bytes of this body */
    u32 reserved2;
};

struct tbt_fec_fbk
{
    u16 epoch;
    u8 m_current;
    u8 m_target;
    u32 p_ewma_ppm;
    u16 expected_shares;
    u16 received_shares;
    u32 reserved;
    u8 carrier_bitmap[TBT_FEC_BITMAP_BYTES];
};

int tbt_fec_ack_pack(const struct tbt_fec_ack *a, u8 out[TBT_FEC_ACK_LEN]);
int tbt_fec_ack_unpack(const u8 in[TBT_FEC_ACK_LEN], struct tbt_fec_ack *a);

/* NACK pack returns the body length (nr * 12) or -EINVAL/-ENOSPC. Unpack
 * returns the body length and fills up to max items, or -EPROTO for a
 * non-multiple length / nonzero reserved field / zero count. */
int tbt_fec_nack_pack(const struct tbt_fec_nack_item *e, u16 nr, u8 *out, size_t cap);
int tbt_fec_nack_unpack(const u8 *in, size_t len, struct tbt_fec_nack_item *e, u16 max);

int tbt_fec_cfg_pack(const struct tbt_fec_cfg *c, u8 out[TBT_FEC_CFG_LEN]);
int tbt_fec_cfg_unpack(const u8 in[TBT_FEC_CFG_LEN], struct tbt_fec_cfg *c);

int tbt_fec_fbk_pack(const struct tbt_fec_fbk *f, u8 out[TBT_FEC_FBK_LEN]);
int tbt_fec_fbk_unpack(const u8 in[TBT_FEC_FBK_LEN], struct tbt_fec_fbk *f);

/* ------------------------------------------------------------------ *
 * GF(2) sliding-window RLNC, one group
 *
 * Data packets d_0..d_{L-1} stream in order. Group g covers
 *     [g*k + k - w, g*k + k - 1] clipped to [0, L).
 * Parity p(g, r) is the XOR of d selected positions in that window. The
 * selection is deterministic: seed = nonce ^ ((g*m + r) * golden), then
 * splitmix64 picks d distinct positions in [0, w). No coefficients travel.
 *
 * d == w is refused for w > 1: every row would be the all-ones row, rank 1,
 * and M would be ineffective. Production uses d = w/2.
 * ------------------------------------------------------------------ */

#define TBT_FEC_RLNC_DATA_KNOWN 0
#define TBT_FEC_RLNC_DATA_MISSING 1
#define TBT_FEC_RLNC_PARITY_KNOWN 2

enum tbt_fec_rlnc_status
{
    TBT_FEC_RLNC_OK = 0,
    TBT_FEC_RLNC_ERR_PARAM = -1,
    TBT_FEC_RLNC_ERR_NOMEM = -2,
    TBT_FEC_RLNC_ERR_STUCK = -EAGAIN,
};

struct tbt_fec_rlnc;

/* n+m <= 256, k>=1, w>=k, w<=TBT_FEC_MAX_W, d>=1, d<=w, and d<w when
 * w>1. Returns NULL on an invalid parameter or allocation failure. */
struct tbt_fec_rlnc *tbt_fec_rlnc_new(u16 n, u16 m, u16 k, u16 w, size_t s, u16 d,
                                      u64 nonce);
void tbt_fec_rlnc_free(struct tbt_fec_rlnc *ctx);

u32 tbt_fec_rlnc_parity_count(const struct tbt_fec_rlnc *ctx, u32 L);

/* Change the PRNG nonce in place. The data plane derives one nonce per block
 * (base ^ block_id*golden) and reuses a preallocated context, so encode/decode
 * never allocates after bind. */
void tbt_fec_rlnc_set_nonce(struct tbt_fec_rlnc *ctx, u64 nonce);
u64 tbt_fec_rlnc_nonce(const struct tbt_fec_rlnc *ctx);

/* Fill idx[0..d-1] with distinct positions in [0, w). Returns d, or 0 on a
 * bad argument. */
u32 tbt_fec_rlnc_coeffs(const struct tbt_fec_rlnc *ctx, u32 g, u32 r, u32 *idx,
                        u32 max);

/* Encode all parity packets for L data packets. Returns the parity count or a
 * negative status. data[i] and parity[p] must each be s bytes. */
int tbt_fec_rlnc_encode(struct tbt_fec_rlnc *ctx, u8 *data[], u32 L, u8 *parity[]);

/* Decode in place. data_present[i] / parity_present[p] are one byte per
 * packet (not a packed bitmap); nonzero means the packet arrived. Returns the
 * number of recovered packets, 0 when nothing was missing, or a negative
 * status. On STUCK no packet is written. */
int tbt_fec_rlnc_decode(struct tbt_fec_rlnc *ctx, u8 *data[], u8 *parity[],
                        const u8 *data_present, const u8 *parity_present, u32 L);

/* Mark one packet as received (kind 0/2) or report a hole (kind 1) and try to
 * advance. Returns the frontier, or a negative parameter error. A rank
 * shortage is reported by the frontier staying below the hole, not by a
 * negative return. */
int tbt_fec_rlnc_advance(struct tbt_fec_rlnc *ctx, int kind, u32 index);

u32 tbt_fec_rlnc_frontier(const struct tbt_fec_rlnc *ctx);
u32 tbt_fec_rlnc_stuck_at(const struct tbt_fec_rlnc *ctx);
u32 tbt_fec_rlnc_recovered(const struct tbt_fec_rlnc *ctx);
int tbt_fec_rlnc_is_known(const struct tbt_fec_rlnc *ctx, u32 index);

/* ------------------------------------------------------------------ *
 * Grouped mode: 32 groups x 8 shares in production.
 * ------------------------------------------------------------------ */


struct tbt_fec_grouped;

/* groups in 1..TBT_FEC_MAX_GROUPS; the same (n,m,k,w,d) for every group.
 * The caller binds each group independently because each has its own L.
 * Group g uses seed_g = nonce ^ ((u64)g << 32); sender and receiver derive it
 * the same way from FEC_CFG.seed, so no per-group seed travels on the wire. */
struct tbt_fec_grouped *tbt_fec_grouped_new(u16 groups, u16 n, u16 m, u16 k, u16 w,
                                            size_t s, u16 d, u64 nonce);
void tbt_fec_grouped_free(struct tbt_fec_grouped *g);
u16 tbt_fec_grouped_count(const struct tbt_fec_grouped *g);
struct tbt_fec_rlnc *tbt_fec_grouped_ctx(struct tbt_fec_grouped *g, u16 group);

/* Application seq is striped across groups: group = seq % groups. */
static inline u16 tbt_fec_group_of_seq(u64 seq, u16 groups)
{
    return groups ? (u16)(seq % groups) : 0;
}

/* 1 share/carrier mapping (T0 §3.6): carriers = groups * (n+m). */
static inline u16 tbt_fec_group_of_carrier(u16 carrier, u16 shares)
{
    return shares ? (u16)(carrier / shares) : 0;
}

static inline u16 tbt_fec_share_of_carrier(u16 carrier, u16 shares)
{
    return shares ? (u16)(carrier % shares) : 0;
}

/* block_id is the group-local generation: (seq / groups) / k. It does not
 * encode the group and does not demux streams. */
static inline u64 tbt_fec_block_of_seq(u64 seq, u16 groups, u16 k)
{
    return (groups && k) ? (seq / groups) / k : 0;
}

/* Global seq of packet p (0..n-1) of group `group` inside `block_id`. The
 * stripe is group = seq % groups, so a block of every group covers one
 * contiguous span of groups*n global packets: the "super-block". */
static inline u64 tbt_fec_seq_of_block_pkt(u64 block_id, u16 group, u16 p,
                                           u16 groups, u16 n)
{
    return ((block_id * n + p) * groups) + group;
}

/* Last super-block that holds at least one packet, given the stream's total
 * packet count P. Undefined for P == 0; callers check that first. */
static inline u64 tbt_fec_final_block_of_total(u64 total_pkts, u16 groups, u16 n)
{
    if (!total_pkts || !groups || !n)
        return 0;
    return (total_pkts - 1) / ((u64)groups * n);
}

/* Number of real DATA packets group `group` has in its final block when the
 * stream holds `total_pkts` packets. 0 means the group has no packet in that
 * super-block. The sender zero-fills the remaining positions before encoding
 * parity; the receiver marks them known-zero and never delivers them. */
static inline u16 tbt_fec_group_tail_len(u64 total_pkts, u16 groups, u16 n,
                                         u16 group)
{
    u64 final_block;
    u64 first;
    u64 last_local;

    if (!total_pkts || !groups || !n || group >= groups)
        return 0;
    final_block = tbt_fec_final_block_of_total(total_pkts, groups, n);
    first = (u64)group + (u64)groups * final_block * n;
    if (total_pkts <= first)
        return 0;
    last_local = (total_pkts - 1 - group) / groups;
    return (u16)(last_local - final_block * n + 1);
}

/* ------------------------------------------------------------------ *
 * Dynamic parameters
 * ------------------------------------------------------------------ */

enum tbt_fec_codec
{
    TBT_FEC_CODEC_RLNC_SINGLE = 0,
    TBT_FEC_CODEC_RLNC_GROUPED = 1,
    TBT_FEC_CODEC_SC_LDPC = 2, /* reserved for D; no implementation here */
};

struct tbt_fec_params
{
    u8 codec;
    u16 groups;
    u16 group_size;
    u16 n;
    u16 m;
    u16 k;
    u16 w;
    u16 d;
    u64 nonce;
};

/* M for one group from p in parts per million. Thresholds (hysteresis is the
 * caller's job): p<=1% -> 3, <=20% -> 4, <=30% -> 5, else 6; clamped to
 * [1, group_size-1]. */
u16 tbt_fec_m_for_p(u32 p_ppm, u16 group_size);

/* Single-code 256-share profile: p<=20%, M=64/N=192/K=192/W=384/d=64.
 * Returns -ERANGE for carriers != 256 or p > 20%. */
int tbt_fec_params_single(u32 p_ppm, u16 carriers, struct tbt_fec_params *out);

/* Grouped profile: per group K=N, W=2N, d=W/2. Requires
 * groups*group_size <= 256 and group_size >= 2. */
int tbt_fec_params_grouped(u32 p_ppm, u16 groups, u16 group_size,
                           struct tbt_fec_params *out);

/* Policy: 256 carriers and p<=20% -> single; otherwise grouped 32x8 (or one
 * group when carriers < 8). SC-LDPC is reserved for D and is never returned
 * by this function. Returns 0 or -EINVAL. */
int tbt_fec_params_dynamic(u32 p_ppm, u16 carriers, struct tbt_fec_params *out);

/* Check that a params set is internally consistent and within the codec
 * limits. Returns 0 or -EINVAL. */
int tbt_fec_params_check(const struct tbt_fec_params *p);

/* Codec ops: B's RLNC and D's SC-LDPC share this shape. Host tests use it. */
struct tbt_fec_ops
{
    const char *name;
    u8 mode; /* 1=rlnc, 2=grouped, 3=sc-ldpc */
    void *(*new)(u16 n, u16 m, u16 k, u16 w, size_t s, u16 d, u64 nonce);
    void (*free)(void *ctx);
    u32 (*parity_count)(const void *ctx, u32 L);
    u32 (*coeffs)(const void *ctx, u32 g, u32 r, u32 *idx, u32 max);
    int (*encode)(void *ctx, u8 *data[], u32 L, u8 *parity[]);
    int (*decode)(void *ctx, u8 *data[], u8 *parity[], const u8 *data_present,
                  const u8 *parity_present, u32 L);
    int (*advance)(void *ctx, int kind, u32 index);
    u32 (*frontier)(const void *ctx);
    u32 (*stuck_at)(const void *ctx);
    u32 (*recovered)(const void *ctx);
    int (*is_known)(const void *ctx, u32 index);
};

extern const struct tbt_fec_ops tbt_fec_ops_rlnc;

#endif /* _TBT_FEC_H */
