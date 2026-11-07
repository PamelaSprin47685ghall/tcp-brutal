/* tbt_fec.h - FEC wire (TBT_FEC_CONV 0x1004), control frames, and the
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

/* S: one packet slot on the wire, the only packet size this cut encodes.
 * tbt_fec_data.c and tbt_fec_ack_bytes() must agree on it. */
#define TBT_FEC_S TBT_FEC_PAYLOAD_MAX

/* ------------------------------------------------------------------ *
 * 40-byte share header
 * ------------------------------------------------------------------ */

struct tbt_fec_hdr
{
    u8 type;    /* TBT_FEC_DATA .. TBT_FEC_FBK */
    u8 flags;   /* TBT_FEC_FF_LAST only */
    u16 share_index;
    u8 n;
    u8 m;
    u16 epoch; /* PARITY only: length of share win_len-1, 0 = full (S) */
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
 * DATA: n>=1, m>=1, n+m<=256, share_index < n, payload_len 1..max_payload
 *       (a short packet is legal anywhere: the packetizer flushes a partial
 *       packet when the application goes quiet, and the receiver places
 *       shares by the sum of the payload lengths, so only LAST marks the
 *       end), win_len == 0, epoch == 0.
 * PARITY: share_index >= n, seq < 2^63.
 *       win_len == 0 is the empty-block marker: share_index == n (parity 0),
 *       flags == 0, payload_len == 0, epoch == 0.
 *       win_len > 0: win_len <= n, payload_len == max_payload,
 *       epoch <= max_payload. The first PARITY of a block fixes
 *       (win_len, epoch); the receiver refuses a later PARITY that disagrees.
 * ACK/NACK/CFG/FBK: n==0, m==0, share_index==0, win_len==0, exact body
 *              lengths, payload_len <= max_payload.
 *
 * The caller must check header_crc32c before trusting any field; this
 * function is the second gate, not the first. */
int tbt_fec_hdr_check(const struct tbt_fec_hdr *h, u16 max_payload);

/* Packet frontier -> stream byte offset acknowledged, for one ACK.
 * `pkt_base` is the stream's first global FEC packet seq, `tx_seq` the byte
 * offset this side has submitted. The result never exceeds tx_seq and never
 * wraps, whatever a hostile peer puts in next_deliver. */
u64 tbt_fec_ack_bytes(u64 next_deliver, u64 pkt_base, u64 tx_seq);

/* FIN range check: pkt_end is the packet after the stream's last DATA packet,
 * pkt_base the first one, and both are absolute session packet seqs. The end
 * may not precede the base, and a non-zero span (groups*n) requires the base
 * to start a super-block. */
static inline bool tbt_fec_fin_valid(u64 pkt_end, u64 pkt_base, u64 span)
{
    if (pkt_end < pkt_base)
        return false;
    if (span && pkt_base % span)
        return false;
    return true;
}

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
    u64 next_deliver;   /* packet frontier: the sender's credit window */
    u32 credit_packets;
    u32 rtt_us;
    /* Stream-relative bytes the receiver has handed to its output socket.
     * The sender's tx_ack is this value, not next_deliver * S: a short share
     * or a block closed early makes the packet frontier name bytes that were
     * never delivered, and the multiplication would acknowledge them. */
    u64 deliver_bytes;
};

/* NACK body: block_id lives in the record header; these two bitmaps name
 * the groups and shares that are still missing in that super-block. A bit
 * is meaningful only below the session's groups and n+m. */
struct tbt_fec_nack
{
    u8 group_mask[TBT_FEC_BITMAP_BYTES];
    u8 share_mask[TBT_FEC_BITMAP_BYTES];
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

/* NACK pack returns TBT_FEC_NACK_LEN or -EINVAL. Unpack returns 0 or
 * -EPROTO for a NULL argument. */
int tbt_fec_nack_pack(const struct tbt_fec_nack *n, u8 out[TBT_FEC_NACK_LEN]);
int tbt_fec_nack_unpack(const u8 in[TBT_FEC_NACK_LEN], struct tbt_fec_nack *n);

/* ------------------------------------------------------------------ *
 * Bounded replay ring and NACK pacing
 *
 * The sender keeps the last K super-blocks of encoded DATA+PARITY records.
 * The receiver may ask for any missing share in that window with a NACK.
 * Both buffers are constant-size: the ring is K * groups * (n+m) records,
 * capped by TBT_FEC_REPLAY_BUDGET, and the receive window is bounded by the
 * credit the ACK grants. No buffer grows with the stream length.
 *
 * The ring is storage only. The caller serialises store/get: the data plane
 * uses one spinlock per TX direction. The host test calls it single-threaded.
 * ------------------------------------------------------------------ */

/* The sender window is the ACK credit in DATA packets, capped to K-2
 * super-blocks by the data plane. K must therefore hold the window plus two
 * super-blocks of slack: a block the receiver can still NACK is never
 * evicted. K=7 with the production 2 MiB super-block is 14.08 MiB of records;
 * the 15 MiB cap keeps the total FEC memory (16 MiB RX arena + replay) under
 * 32 MiB. A shape that cannot hold K=7 shrinks; the credit cap follows the
 * effective K. */
#define TBT_FEC_REPLAY_K 7
#define TBT_FEC_REPLAY_BUDGET (15u * 1024 * 1024)

struct tbt_fec_replay;

/* Returns NULL when the shape needs more than TBT_FEC_REPLAY_BUDGET even for
 * K = 1, or when an allocation fails. The effective K is then 0 and the data
 * plane falls back to parity and the TCP retransmission. */
struct tbt_fec_replay *tbt_fec_replay_new(u16 groups, u16 n, u16 m, u16 s);
void tbt_fec_replay_free(struct tbt_fec_replay *r);
u16 tbt_fec_replay_k(const struct tbt_fec_replay *r);
/* Effective K for this shape without allocating: the largest k <=
 * TBT_FEC_REPLAY_K whose ring fits TBT_FEC_REPLAY_BUDGET, or 0. The sender
 * ring, the receiver NACK window and the credit cap all use this value. */
u16 tbt_fec_replay_k_for(u16 groups, u16 n, u16 m, u16 s);
size_t tbt_fec_replay_bytes(u16 groups, u16 n, u16 m, u16 s, u16 k);

/* Store one complete encoded record (header included). A share outside
 * [0, n+m) or a record longer than header+S is refused. */
int tbt_fec_replay_store(struct tbt_fec_replay *r, u64 block_id, u16 group,
                         u16 share, const u8 *rec, u16 len);
/* Copy a stored record into out. Returns its length, -ENOENT when the block
 * is no longer in the ring or that share was never stored, or -ENOSPC when
 * out is too small. */
int tbt_fec_replay_get(const struct tbt_fec_replay *r, u64 block_id,
                       u16 group, u16 share, u8 *out, u16 cap);
bool tbt_fec_replay_has(const struct tbt_fec_replay *r, u64 block_id,
                        u16 group, u16 share);

/* NACK pacing. The first NACK for a block goes out at once, subject to a
 * global minimum interval; an identical NACK is retried after the longer
 * interval. There is no total attempt cap: while a carrier TCP is alive the
 * repair continues, and only the replay window bounds it. */
#define TBT_FEC_NACK_MIN_MS 10
#define TBT_FEC_NACK_RETRY_MS 20
/* A shape probe asks the sender to describe a super-block it may not have
 * sent yet; repeating it at the repair cadence puts a NACK on the wire for
 * every retry interval of an idle connection. Pace it instead. */
#define TBT_FEC_NACK_PROBE_MS 100

struct tbt_fec_nack_state
{
    u64 first_ns; /* first NACK for this block/mask */
    u64 last_ns;  /* last NACK sent, any block/mask */
    u64 block_id;
    u32 tries;
    bool valid;
    u8 group_mask[TBT_FEC_BITMAP_BYTES];
    u8 share_mask[TBT_FEC_BITMAP_BYTES];
};

/* Returns true when the caller should send this NACK now and updates the
 * state. `now_ns` is monotonic. */
bool tbt_fec_nack_due(struct tbt_fec_nack_state *st, u64 now_ns,
                      u64 block_id, const struct tbt_fec_nack *n);
void tbt_fec_nack_reset(struct tbt_fec_nack_state *st);

int tbt_fec_cfg_pack(const struct tbt_fec_cfg *c, u8 out[TBT_FEC_CFG_LEN]);
int tbt_fec_cfg_unpack(const u8 in[TBT_FEC_CFG_LEN], struct tbt_fec_cfg *c);

int tbt_fec_fbk_pack(const struct tbt_fec_fbk *f, u8 out[TBT_FEC_FBK_LEN]);
int tbt_fec_fbk_unpack(const u8 in[TBT_FEC_FBK_LEN], struct tbt_fec_fbk *f);

/* ------------------------------------------------------------------ *
 * GF(2) sliding-window RLNC, one group
 *
 * Data packets d_0..d_{L-1} stream in order. Group g covers
 *     [g*k + k - w, g*k + k - 1] clipped to [0, L).
 * Parity p(g, r) is the XOR of positions selected by a deterministic band:
 * own-data column c belongs to row r when (c - r) mod m < band. The band is
 * the widest one that fits d terms per row and keeps the m x k matrix at
 * rank min(m, k), so every column sits in at least one row and a single lost
 * share is always covered. The leading w - k positions of the window (the
 * previous packets) pad a row to d; a codec bound to L == k skips them.
 * No coefficients travel. The nonce no longer selects coefficients; it
 * remains in the API because the grouped codec keeps one seed per group.
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

/* Set the codec nonce. Coefficients are a fixed band now, so the value is
 * informational; the data plane still derives one per block and reuses a
 * preallocated context, so encode/decode never allocates after bind. */
void tbt_fec_rlnc_set_nonce(struct tbt_fec_rlnc *ctx, u64 nonce);
u64 tbt_fec_rlnc_nonce(const struct tbt_fec_rlnc *ctx);
/* Band width of the parity matrix. Row r covers DATA column c when
 * (c + m - (r % m)) % m < band; row c % m always covers column c. */
u16 tbt_fec_rlnc_band(const struct tbt_fec_rlnc *ctx);

/* Fill idx with distinct positions in [0, w). Returns the count, at most d,
 * or 0 on a bad argument. */
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

/* Real share count for a block. The parity's win_len is authoritative; the
 * final-block tail formula is only the fallback when every parity row was
 * lost. A tail of 0 with a present DATA share is a stale stream end (FIN
 * froze pkt_end before that share was packetized): do not treat the share as
 * padding; keep n and let parity/NACK state the real cut. */
static inline u16 tbt_fec_blk_real_len_from_tail(u16 nreal, u16 n,
                                                 bool total_known,
                                                 u64 total_pkts, u16 groups,
                                                 u64 block_id, u16 group,
                                                 bool has_data)
{
    u16 len = nreal ? nreal : n;

    if (nreal || !total_known)
        return len;
    if (block_id == tbt_fec_final_block_of_total(total_pkts, groups, n))
    {
        u16 tail = tbt_fec_group_tail_len(total_pkts, groups, n, group);

        if (tail < len && !(tail == 0 && has_data))
            len = tail;
    }
    return len;
}

/* A block that holds only parity: its DATA shares are the missing ones. The
 * NACK scan must request them, not probe parity 0 again. `used` may be stale
 * false on a path that placed parity without the block reset; `nparity` is
 * the authoritative evidence. A block with any DATA share present is not
 * parity-only. */
static inline bool tbt_fec_nack_parity_only(bool used, u16 nparity,
                                            u64 block_id, u64 block,
                                            u16 ndata, bool ready)
{
    return block_id == block && nparity && !ready && (!used || ndata == 0);
}

/* A group with no block at all in a started super-block. Before the stream
 * end is known, the group may be legitimately empty (a short or sparse
 * block): the parity's win_len, the empty marker or FIN will state the real
 * share count, so do not NACK the whole range. Once total_pkts is known, a
 * final-block group with no packet is known empty; a group that has packets
 * in a non-final block is a real whole-block loss and may be requested. */
static inline bool tbt_fec_nack_unstarted_group(u64 total_pkts, u16 groups,
                                                u16 n, u64 block, u16 group,
                                                bool total_known)
{
    if (!total_known)
        return false;
    if (block == tbt_fec_final_block_of_total(total_pkts, groups, n) &&
        !tbt_fec_group_tail_len(total_pkts, groups, n, group))
        return false;
    return true;
}

/* Delivery length of share p in a block whose real share count is `real`.
 *
 * A share that arrived carries its own payload_len (dlen); a share recovered
 * from parity has none. Only the block's last real position may be shorter
 * than S, and the parity's epoch names that length (short_len). A recovered
 * share before that position is S bytes.
 *
 * Returns 0 when the slot holds no stream byte. The caller must not deliver
 * such a slot, and must refuse the block outright when the slot holds a
 * present share (the cut and the shares disagree).
 *
 * The one shape this cannot name is a short share written mid-block and then
 * recovered: its length never reached the wire. tbt_fec_blk_decode() refuses
 * that block while the short share is still visible; once it is the missing
 * one, no record states its length. The packetizer never writes such a share
 * -- it closes the block after every short packet -- so the case is a hostile
 * or foreign peer, and the block-level refusal is the bound. */
static inline u16 tbt_fec_rx_share_len(u16 dlen, u16 short_len, u16 p,
                                       u16 real, u16 s)
{
    if (p >= real)
        return 0; /* padding: no stream byte lives here */
    if (dlen)
        return dlen;
    if (p + 1 == real && short_len)
        return short_len;
    return s;
}

/* True when a PARITY record has already fixed the block's cut and `share`
 * sits at or past it. Placing such a share would make it invisible to the
 * delivery walk, which skips every slot at or past the cut, while the packet
 * frontier and the ACK ran past it -- a silent 8192-byte deletion. The
 * receiver refuses the share instead. Before the cut is fixed any share is
 * legal, because win_len is not yet known. */
static inline bool tbt_fec_share_past_cut(bool win_len_set, u16 nreal,
                                          u16 share)
{
    return win_len_set && share >= nreal;
}

/* One delivery step against the stream socket's accepted frontier.
 * `off` is stream-relative (already minus stream_base), `rx_next` is what
 * that stream has accepted. Returns:
 *   0        deliver this share and advance the cursor by len
 *   1        duplicate/expired: its bytes are already in the stream; do not
 *            advance the caller's cursor, realign it to rx_next instead
 *  -EPROTO   offset overflow
 *
 * The FEC walk is ordered, so a duplicate here means the cursor and the
 * socket disagree. Treating it as delivered would advance deliver_bytes a
 * second time and the ACK would name bytes the application never received. */
static inline int tbt_fec_deliver_step(u64 off, u16 len, u64 rx_next)
{
    if (off > ~(u64)0 - len)
        return -EPROTO;
    if (off + len <= rx_next)
        return 1;
    return 0;
}

/* A block contributes stream bytes only when it belongs to the super-block
 * being delivered and is ready. A group with no packet in the final
 * super-block has no block at all (used == false); the delivery walk must
 * skip it, not read its stale buffer. */
static inline bool tbt_fec_blk_deliverable(bool used, bool ready,
                                           u64 block_id, u64 s)
{
    return used && ready && block_id == s;
}

/* A blocked walk still has to notice a share that arrives later. The slow
 * cadence keeps an idle connection from waking the workqueue 100 times a
 * second; a share that arrives after it costs at most that delay, far below
 * the sender's stall timer. */
#define TBT_FEC_RX_POLL_SLOW_MS 100

/* Delay w_fec_rx arms itself with while the next super-block is not ready.
 *
 * tbt_session_arm() never retimes an item that is already pending: it keeps
 * the reference accounting sound, so whatever delay this walk arms is the
 * maximum latency before an arriving share or marker is noticed. The stall
 * budget is 30 s, and the field stall was exactly that: the next
 * super-block's shares and markers all arrived and became ready, but the
 * pending pass was still 30 s away, so the bytes were never delivered and
 * the ACK never moved. Poll at the NACK cadence while the repair is young,
 * then back off; progress_ns still bounds the stall decision, so the reset
 * check keeps its deadline. */
static inline unsigned long tbt_fec_rx_poll_ms(u64 left_ms, u64 waited_ns)
{
    u64 cadence = waited_ns < 1000000000ULL ? (u64)TBT_FEC_NACK_MIN_MS
                                            : (u64)TBT_FEC_RX_POLL_SLOW_MS;

    if (left_ms < cadence)
        cadence = left_ms;
    return (unsigned long)cadence;
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
