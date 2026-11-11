/* tbt_fec.h - FEC wire (TBT_FEC_CONV 0x100b), control frames, and the
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
 *   - 48-byte share header: pack/unpack, structural checks, both CRCs.
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
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#endif

/* The replay ring is serialised by the TX plane's replay spinlock. Host
 * tests are single-threaded and use a no-op type so the same signatures
 * compile on both sides. */
#ifdef __KERNEL__
typedef spinlock_t tbt_fec_spinlock_t;
#else
typedef int tbt_fec_spinlock_t;
#endif

/* S: one packet slot on the wire, the only packet size this cut encodes.
 * tbt_fec_data.c and tbt_fec_ack_bytes() must agree on it. */
#define TBT_FEC_S TBT_FEC_PAYLOAD_MAX

/* One FEC drain write carries at most this many complete HTTP frames. Three
 * frames are 24,780 B and stay in the kmalloc-32k cache (order-3); eight were
 * 66,080 B and needed an order-5 page block, whose allocation failed on the
 * 955 MB far host and stalled the drain under direct reclaim. */
#define TBT_FEC_BATCH_MAX 3

/* Per-carrier record assembler capacity: the larger of the ARQ and FEC record
 * maxima. Shared with host tests so the memory ledger cannot drift from the
 * buffers the carrier actually allocates. */
#define TBT_PARSER_BUF (TBT_FEC_REC_MAX > TBT_REC_MAX ? TBT_FEC_REC_MAX : TBT_REC_MAX)

/* Long-lived planes above this size are mapped by vmalloc. They are touched
 * in sequential bursts and never need physical contiguity; keeping them out
 * of the kmalloc caches stops a 256-carrier session from holding dozens of
 * order-6..9 page blocks, which is what fragmented the 955 MB far host.
 * Below the threshold kvzalloc stays the single small-object path. */
#define TBT_FEC_VMALLOC_MIN (32u * 1024u)

static inline bool tbt_fec_plane_uses_vmalloc(size_t len)
{
    return len >= TBT_FEC_VMALLOC_MIN;
}

/* Zeroed allocation for FEC planes. kvfree() releases either kind. Process
 * context only: vzalloc() may sleep. */
static inline void *tbt_fec_vzalloc(size_t len)
{
#ifdef __KERNEL__
    if (!len)
        len = 1;
    if (tbt_fec_plane_uses_vmalloc(len))
        return vzalloc(len);
    return kvzalloc(len, GFP_KERNEL);
#else
    return calloc(1, len ? len : 1);
#endif
}

/* ------------------------------------------------------------------ *
 * 48-byte share header
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
    u16 reserved; /* DATA/PARITY: group index; control records: 0 */
    /* DATA/PARITY only: the stream this share belongs to. The receiver drops
     * a share whose sid is not its current stream; control records carry 0. */
    u64 sid;
    u32 payload_crc32c; /* payload only */
    u32 header_crc32c;  /* header bytes +0..+43, this field excluded */
};

/* Pack h into out[40] (little-endian). Returns 0, or -EINVAL for a NULL
 * argument. Pack does not validate; call tbt_fec_hdr_check() before sending. */
int tbt_fec_hdr_pack(const struct tbt_fec_hdr *h, u8 out[TBT_FEC_HDR_LEN]);

/* Unpack in[40] into h. Returns 0, or -EINVAL for a NULL argument. It copies
 * bytes only; a caller must still run tbt_fec_hdr_check() and the CRC checks. */
int tbt_fec_hdr_unpack(const u8 in[TBT_FEC_HDR_LEN], struct tbt_fec_hdr *h);

/* Structural check, in this order: conv, type, flags, group, lengths,
 * share bounds. `max_payload` is the configured S. Returns 0 or -EPROTO.
 *
 * DATA: n>=1, m>=1, n+m<=256, share_index < n, payload_len 1..max_payload
 *       (a short packet is legal anywhere: the packetizer flushes a partial
 *       packet when the application goes quiet, and the receiver places
 *       shares by the sum of the payload lengths, so only LAST marks the
 *       end), win_len == 0, epoch == 0, seq >= group*n + share_index
 *       (block_seq0 must not underflow).
 * PARITY: share_index >= n, seq < 2^63.
 *       win_len == 0 is the empty-block marker: share_index == n (parity 0),
 *       flags == 0, payload_len == 0, epoch == 0.
 *       win_len > 0: win_len <= n, payload_len == max_payload,
 *       epoch <= max_payload. The first PARITY of a block fixes
 *       (win_len, epoch); the receiver refuses a later PARITY that disagrees.
 * ACK/NACK/CFG/FBK: n==0, m==0, share_index==0, win_len==0, group==0,
 *              exact body lengths, payload_len <= max_payload.
 *
 * The caller must check header_crc32c before trusting any field; this
 * function is the second gate, not the first. */
int tbt_fec_hdr_check(const struct tbt_fec_hdr *h, u16 max_payload);

/* Route-A block sequence. `block_seq0` is the absolute seq of the block's
 * group-0/share-0 DATA packet:
 *   DATA:   seq = block_seq0 + (u64)group * n + share_index
 *   PARITY: seq = block_seq0
 * One group: seq = block_seq0 + share_index. Eight groups: each group owns
 * n contiguous packets, so group g spans
 * [block_seq0 + g*n, block_seq0 + (g+1)*n).
 *
 * tbt_fec_data_seq0() refuses a DATA seq below its own offset; that is the
 * only underflow the formula admits. */
static inline bool tbt_fec_data_seq0(u64 seq, u16 group, u16 n, u16 share,
                                     u64 *block_seq0)
{
    u64 off = (u64)group * n + share;

    if (seq < off)
        return false;
    if (block_seq0)
        *block_seq0 = seq - off;
    return true;
}

static inline u64 tbt_fec_data_seq(u64 block_seq0, u16 group, u16 n,
                                   u16 share)
{
    return block_seq0 + (u64)group * n + share;
}

/* Packet frontier -> stream byte offset acknowledged, for one ACK.
 * `pkt_base` is the stream's first global FEC packet seq, `tx_seq` the byte
 * offset this side has submitted. The result never exceeds tx_seq and never
 * wraps, whatever a hostile peer puts in next_deliver. */
u64 tbt_fec_ack_bytes(u64 next_deliver, u64 pkt_base, u64 tx_seq);

/* FIN range check: pkt_end is the packet after the stream's last DATA packet,
 * pkt_base the first one, and both are absolute session packet seqs. The end
 * may not precede the base, and a non-zero span (groups*n) requires the base
 * to start a super-block. `base_pkt` is the current shape's origin: the
 * stream may start before it (the stream outlives a shape switch), so the
 * alignment is measured from the origin, not from absolute zero. */
static inline bool tbt_fec_fin_valid_base(u64 pkt_end, u64 pkt_base,
                                          u64 base_pkt, u64 span)
{
    if (pkt_end < pkt_base || pkt_base < base_pkt)
        return false;
    if (span && (pkt_base - base_pkt) % span)
        return false;
    return true;
}

static inline bool tbt_fec_fin_valid(u64 pkt_end, u64 pkt_base, u64 span)
{
    return tbt_fec_fin_valid_base(pkt_end, pkt_base, 0, span);
}

/* Standard CRC32C (Castagnoli), reflected, init ~0, final xor. A fresh CRC
 * uses seed 0; chaining uses the previous return value as the next seed. */
u32 tbt_fec_crc32c(u32 seed, const void *data, size_t len);

#ifndef __KERNEL__
/* Host-only halves of the CRC32C contract. The public call above dispatches
 * to the instruction path when the CPU has one, and to the portable table
 * otherwise. The host tests compare the two bit for bit; the module keeps
 * only crc32c(). */
u32 tbt_fec_crc32c_soft(u32 seed, const void *data, size_t len);
u32 tbt_fec_crc32c_hw(u32 seed, const void *data, size_t len);
int tbt_fec_crc32c_hw_available(void);
#endif

/* Fill payload_crc32c and header_crc32c. `payload` is payload_len bytes; it
 * may be NULL only when payload_len is 0. Returns 0 or -EINVAL. */
int tbt_fec_hdr_seal(struct tbt_fec_hdr *h, const u8 *payload);

/* Verify header_crc32c first, then payload_crc32c. Returns 0, -EBADMSG on a
 * CRC mismatch, or -EINVAL on a NULL payload with a nonzero length. */
int tbt_fec_hdr_verify(const struct tbt_fec_hdr *h, const u8 *payload);

/* Locate and validate one complete FEC record header in rec[0..len). This is
 * the receive gate: len < TBT_FEC_HDR_LEN is refused before any read, then
 * unpack, structural check and header_crc32c, in that order. Returns 0 and
 * fills *out when the header is safe to use, -EPROTO for framing, or
 * -EBADMSG for a header CRC failure. `out` may be NULL. The payload CRC is
 * separate because the body is not always buffered with the header. */
int tbt_fec_rec_check(const u8 *rec, size_t len, u16 max_payload,
                      struct tbt_fec_hdr *out);

/* ------------------------------------------------------------------ *
 * 256-bit carrier bitmap
 * ------------------------------------------------------------------ */

void tbt_fec_bitmap_set(u8 bm[TBT_FEC_BITMAP_BYTES], u16 index);
void tbt_fec_bitmap_clear(u8 bm[TBT_FEC_BITMAP_BYTES], u16 index);
bool tbt_fec_bitmap_test(const u8 bm[TBT_FEC_BITMAP_BYTES], u16 index);
u16 tbt_fec_bitmap_count(const u8 bm[TBT_FEC_BITMAP_BYTES]);

/* Every bit at or above `first` is clear. A wire bitmap describes only the
 * carriers the session owns; the bits beyond the session are reserved. */
static inline bool
tbt_fec_bitmap_tail_zero(const u8 bm[TBT_FEC_BITMAP_BYTES], u16 first)
{
    u16 i;

    if (first > TBT_FEC_MAX_CARRIERS)
        first = TBT_FEC_MAX_CARRIERS;
    for (i = first; i < TBT_FEC_MAX_CARRIERS; i++)
        if (bm[i / 8] & (u8)(1u << (i % 8)))
            return false;
    return true;
}

/* v7 has no cross-stream TX gate. Each stream owns a TX plane, a ring and a
 * block; a detached stream is replaced by its flow lifetime (lookup, purge,
 * and a record-held flow reference), not by comparing far_sid at send time. */

/* A flow accepts new work only while it is active. A detach publishes state 3
 * before it unlinks, so a snapshot, a push or an in-flight flush that already
 * holds a reference stops instead of producing work for a dead stream. */
static inline bool tbt_fec_flow_live(u8 state)
{
    return state == 1;
}

/* Take the queue reference exactly once. Purge and a flush can both reach a
 * record; only the winner unlinks it and charges the queue counters. */
static inline bool tbt_fec_out_unlink_once(bool *queued)
{
    if (!*queued)
        return false;
    *queued = false;
    return true;
}

/* The peer's FIN reached the application but our FIN never left the wire: the
 * far target never closed its half. After the close bound the stream must be
 * killed so its flow and target cannot pin the session until teardown. */
static inline bool tbt_tx_peer_fin_close_expired(bool rx_fin_done,
                                                 bool tx_fin_written,
                                                 u64 rx_fin_done_ns,
                                                 u64 now, u64 bound)
{
    return rx_fin_done && !tx_fin_written && rx_fin_done_ns &&
           now > rx_fin_done_ns && now - rx_fin_done_ns > bound;
}

/* A peer/target has closed its write side when TCP reports RCV_SHUTDOWN, or
 * when the socket reached TCP_CLOSE without a local kill. The latter catches a
 * target that closed fully and cleared sk_shutdown; a dead stream is ignored. */
static inline bool tbt_stream_peer_eof(bool rcv_shutdown, bool tcp_close,
                                       bool dead)
{
    return !dead && (rcv_shutdown || tcp_close);
}

/* Close bound: a peer FIN that never produced our FIN is a stuck target and
 * gets a short, dedicated bound. A written FIN waits for the peer's half and
 * keeps the old 60 s / two-budget bound. */
#define TBT_FEC_PEER_FIN_CLOSE_NS (3ULL * 1000000000ULL)

static inline u64 tbt_tx_close_bound(bool peer_fin_unwritten,
                                     u64 wait_budget_ns)
{
    u64 b;

    if (peer_fin_unwritten)
        return TBT_FEC_PEER_FIN_CLOSE_NS;
    b = 2 * wait_budget_ns;
    return b > 60ULL * 1000000000ULL ? b : 60ULL * 1000000000ULL;
}

/* Admission arithmetic: a flow needs at least one group, and its count must
 * fit the groups no live flow owns. The subtraction form cannot overflow. */
static inline bool tbt_fec_groups_fit(u16 total, u16 used, u16 count)
{
    return count && used <= total && count <= (u16)(total - used);
}

/* One refusal code for flow creation. A duplicate sid must win over every
 * admission rule: concurrent OPEN copies of the same stream are duplicates.
 * A session carries one FEC flow, so a create while another flow is live is
 * -ENOSPC: the caller refuses the second stream. */
static inline int tbt_fec_flow_create_code(bool sid_exists, bool sclc,
                                           u16 n_flows, u16 total,
                                           u16 count)
{
    if (sid_exists)
        return -EEXIST;
    if (sclc && n_flows)
        return -ENOSPC;
    if (n_flows)
        return -ENOSPC;
    if (!count || count > total)
        return -EPROTO;
    return 0;
}

/* Every carrier that receives the same OPEN resolves it to one flow, but
 * exactly one of them creates the stream. Only that carrier may move the
 * flow reference into the stream. A carrier that did not create it drops
 * its reference, even when it was the one that admitted the flow: the
 * winner is about to attach that plane. Unlinking is reserved for an
 * admission that found no stream at all, because nothing else can use it. */
static inline bool tbt_fec_open_unlink_ok(bool created, bool admitted,
                                          bool no_stream)
{
    return !created && no_stream && admitted;
}

/* A repeated OPEN must name the same bitmap the plane already bound. The
 * caller passes that bitmap (near: its own TX mask, far: the peer RX mask). */
static inline bool tbt_fec_primary_mask_match(bool sclc,
                                              const u8 *flow_mask,
                                              const u8 *open_mask)
{
    u16 i;

    (void)sclc;
    for (i = 0; i < TBT_FEC_BITMAP_BYTES; i++)
        if (flow_mask[i] != open_mask[i])
            return false;
    return true;
}

/* The SC-LDPC INIT shape names the same peer group count the OPEN bitmap
 * selected. A full-1 bitmap (256 groups) or a single group does not describe
 * an 8-group RX shape and must be refused when CFG INIT arrives. */
static inline bool tbt_fec_open_groups_match(bool sclc, const u8 *mask,
                                             u16 cfg_groups)
{
    return !sclc || tbt_fec_bitmap_count(mask) == cfg_groups;
}

/* A stream the TX work may use: not dead, still attached to the session, and
 * not inside its tombstone window.  A gracefully closed stream keeps state
 * OPEN after detach, so the state alone is not enough. */
static inline bool tbt_fec_tx_stream_usable(bool dead, bool active,
                                            u64 tombstone_until_ns)
{
    return !dead && active && !tombstone_until_ns;
}

/* Defense in depth: a block placed for one stream must never be delivered to
 * another. A v7 flow owns its blocks, so the two sids normally match; a
 * detached and rebound primary is the case this still catches. `stream_sid`
 * 0 means no stream has been adopted yet, so there is nothing to compare. */
static inline bool tbt_fec_blk_sid_match(u64 block_sid, u64 stream_sid)
{
    return !stream_sid || block_sid == stream_sid;
}

/* Where a DATA/PARITY record's block sits against the receiver's reorder
 * window. The window belongs to one stream, and the block id comes from the
 * session's packet sequence: a stream switch leaves the old cursor naming no
 * block the new stream will send. Resetting `next_super` to the old highest
 * block + 1 did exactly that, and every first-flight record of the new stream
 * landed beyond `next_super + slots` (field: fec_drop_window=1271 after an
 * aborted connection was replaced; the peer then waited on credit forever).
 *
 * `rebase` says this record belongs to a stream the window was not based on
 * (a reset is pending, or the record's sid is not the window's stream and the
 * delivery walk has drained the old one). A rebase moves the window to the
 * record's block. Before the stream's first delivery (`delivered` false: no
 * byte handed over and no saved partial cursor) a lower block lowers the
 * window to itself instead of being called late: the first record to arrive
 * need not be the stream's lowest block, and blocks the reset wiped are
 * repaired by NACK once they are inside the window. After the first delivery
 * the cursor never moves back; a lower block is late. */
enum tbt_fec_rx_win
{
    TBT_FEC_RX_WIN_REBASE = 0, /* move the window to this block, then place */
    TBT_FEC_RX_WIN_PLACE,      /* inside [next_super, next_super + slots) */
    TBT_FEC_RX_WIN_LATE,       /* below the window: already delivered */
    TBT_FEC_RX_WIN_FAR,        /* beyond the window: cannot buffer */
};

static inline enum tbt_fec_rx_win
tbt_fec_rx_window_step(u64 next_super, u64 block_id, u16 slots, bool rebase,
                       bool delivered)
{
    if (rebase)
        return TBT_FEC_RX_WIN_REBASE;
    if (block_id < next_super)
        return delivered ? TBT_FEC_RX_WIN_LATE : TBT_FEC_RX_WIN_REBASE;
    if (block_id >= next_super + slots)
        return TBT_FEC_RX_WIN_FAR;
    return TBT_FEC_RX_WIN_PLACE;
}

/* A share whose whole block is behind the delivery frontier: the block was
 * already handed over (or consumed as padding) and no byte may move again.
 * The caller counts fec_late_share and drops it before decrypting, placing
 * or delivering. `delivered` is the same first-delivery gate the reorder
 * window uses; before it, a lower block may still lower the window, so the
 * share is not late. `block_end_seq` is the first packet after the block,
 * which names "next_deliver has passed this block" without a block count. */
static inline bool tbt_fec_rx_block_late(bool rebase, bool delivered,
                                         u64 next_super, u64 block_id,
                                         u64 block_end_seq, u64 next_deliver)
{
    if (rebase || !delivered)
        return false;
    return block_id < next_super || block_end_seq <= next_deliver;
}

/* ------------------------------------------------------------------ *
 * Control bodies (payload after the 48-byte header)
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


/* Shape change request (0x100b, 64 bytes). See tbt_uapi.h for the byte
 * offsets; this struct is the host-side mirror, not the wire layout. The
 * extension fields name the codec explicitly; `codec_variant == 2` leaves
 * the wire w/k/d bytes zero, and w/k/d below carry the equivalent legacy
 * values only so the in-kernel consumers keep one shape vocabulary. */
struct tbt_fec_cfg
{
    u32 epoch;
    u16 n;
    u16 m;
    u16 w;
    u16 k;
    u8 d;
    u8 flags; /* TBT_FEC_CFG_F_APPLY / _ABORT */
    u16 groups;
    u64 switch_pkt; /* absolute seq of the first new-shape record */
    u64 block_base; /* block id of the first new-shape block */
    u64 seed;
    u32 crc32c;  /* CRC32C over the first 40 bytes of this body */
    u32 reserved;
    u8 codec_variant; /* 0 RLNC single, 1 RLNC grouped, 2 SC-LDPC */
    u8 L;             /* variant 2: 8 or 16; otherwise 0 */
    u16 block;        /* variant 2: 256 or 512; otherwise 0 */
    u32 rate_ppm;     /* variant 2: 250000..700000; otherwise 0 */
    u32 crc32c_ext;   /* CRC32C over the eight extension bytes +48..+55 */
    u32 reserved2;
};

/* Receiver report (0x1008, 64 bytes). `epoch` is the 32-bit generation
 * assembled from epoch_lo/epoch_hi; the wire keeps the two halves apart so
 * the first 12 bytes stay at their 0x1002 offsets. */
struct tbt_fec_fbk
{
    u16 epoch_lo;
    u8 m_current;
    u8 m_target;
    u32 p_ewma_ppm;
    u16 expected_shares;
    u16 received_shares;
    u16 epoch_hi;
    u8 flags; /* TBT_FEC_FBK_F_* */
    u8 n_current;
    u8 carrier_bitmap[TBT_FEC_BITMAP_BYTES];
    u32 p_fast_ppm; /* SC-LDPC fast-window loss */
    u8 codec_variant;
    u8 L;
    u16 block;
    u32 rate_ppm_current;
    u32 rate_ppm_target;
};

static inline u32 tbt_fec_fbk_epoch(const struct tbt_fec_fbk *f)
{
    return ((u32)f->epoch_hi << 16) | f->epoch_lo;
}

static inline void tbt_fec_fbk_set_epoch(struct tbt_fec_fbk *f, u32 epoch)
{
    f->epoch_lo = (u16)epoch;
    f->epoch_hi = (u16)(epoch >> 16);
}

/* The wire carries one variant value. A shape built internally may leave
 * variant 0 for an RLNC shape that has several groups; the wire form and
 * the peer's report both name variant 1 in that case. Normalize from the
 * group count, never from the caller's unset field. */
static inline u8 tbt_fec_variant_norm(u8 variant, u16 groups)
{
    if (variant == 2)
        return 2;
    return groups > 1 ? 1 : 0;
}

/* An FBK names the receive shape. It matches only the codec this side
 * committed: the normalized variant always, and L/block for SC-LDPC.
 * `committed_variant` may still be the internal (unnormalized) value. */
static inline bool
tbt_fec_fbk_variant_match(u8 fb_variant, u8 fb_L, u16 fb_block,
                          u8 committed_variant, u8 committed_L,
                          u16 committed_block, u16 groups)
{
    u8 want = tbt_fec_variant_norm(committed_variant, groups);

    if (fb_variant != want)
        return false;
    return want != 2 || (fb_L == committed_L && fb_block == committed_block);
}

/* 0.5.0 removes the SC-LDPC FBK overlay: p_fast_ppm moved to +48 and the
 * full 32-byte carrier bitmap is available again. codec_fail has no v8 wire
 * field (it is a local procfs diagnostic); the estimator computes it, but
 * only p_fast crosses the wire. */

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
 * The K logical slots are fixed, but each block's record plane is allocated
 * on its first store and returned to an 8-block hot pool when the peer's
 * ACK passes it; TBT_FEC_REPLAY_BUDGET bounds the descriptor plane plus that
 * pool, not K times the full record plane. No buffer grows with the stream
 * length.
 *
 * The ring is storage only. The caller serialises store/get: the data plane
 * uses one spinlock per TX direction. The host test calls it single-threaded.
 * ------------------------------------------------------------------ */

/* The sender window is the ACK credit in DATA packets, capped to K-2
 * super-blocks by the data plane. K must therefore hold the window plus two
 * super-blocks of slack: a block the receiver can still NACK is never
 * evicted. K is derived from the shape: the ring is allocated at the largest
 * K that fits TBT_FEC_REPLAY_BUDGET, up to TBT_FEC_REPLAY_K_MAX. 0.7.2 needs
 * a 64-block sliding window: at 400 ms RTT and 128 MB/s the source BDP is
 * 51.2 MB (about 50 source blocks), and the old 16-block/2048-packet cap
 * held the protocol to 35 MB/s. K=66 leaves the required two-block slack
 * above the 64-block credit. The record plane is block-granular, so K=66
 * costs 66 descriptors plus an 8-block hot pool (~17 MiB static); planes of
 * unacknowledged blocks are allocated on write and released on ACK, so the
 * steady resident set follows the ACK frontier rather than K times 2 MiB.
 *   static ring   <= TBT_FEC_REPLAY_BUDGET (20 MiB: descriptors + 8 blocks),
 *   arena         <= 66 slots, block planes lazy (8-block hot pool),
 *   credit        <= K_eff - 2 blocks, capped at TBT_FEC_CREDIT_MAX packets.
 * A shape that cannot hold K=1 gets no ring; the credit cap follows the
 * effective K. */
#define TBT_FEC_REPLAY_K_MAX 66
#define TBT_FEC_REPLAY_BUDGET (20u * 1024 * 1024)
/* The sliding window is 64 super-blocks of source data, the measured BDP
 * of 128 MB/s over 400 ms. The window must stay two blocks below the ring K
 * so a NACKed block is never evicted while the receiver can still ask for
 * it. tbt_fec_credit_packets() converts the window to DATA packets; neither
 * window multiplies again by the carrier count. */
#define TBT_FEC_WINDOW_DOWN (64u * 1024 * 1024)
#define TBT_FEC_WINDOW_UP (64u * 1024 * 1024)
/* The RLNC ACK grants at most this many DATA packets of FEC credit, and the
 * SC-LDPC downlink window (TBT_FEC_WINDOW_DOWN / TBT_FEC_S) resolves to the
 * same number. The credit bounds the sender's in-flight data; the ring
 * bounds the repair storage, and both are constant-size for a given shape. */
#define TBT_FEC_CREDIT_MAX 8192u

/* Block planes resident in the replay hot pool. Eight 2 MiB blocks are
 * 16 MiB; below TBT_FEC_REPLAY_MEM_LOW_KB the pool shrinks to four. */
#define TBT_FEC_REPLAY_HOT_MAX 8u
#define TBT_FEC_REPLAY_HOT_LOW 4u
#define TBT_FEC_REPLAY_MEM_LOW_KB (220L * 1024L)

struct tbt_fec_replay;

/* Returns NULL when the shape needs more than TBT_FEC_REPLAY_BUDGET even for
 * K = 1, or when an allocation fails. The effective K is then 0 and the data
 * plane falls back to parity and the TCP retransmission. */
struct tbt_fec_replay *tbt_fec_replay_new(u16 groups, u16 n, u16 m, u16 s);
/* Same ring with an explicit K. A multi-stream session shares one K across
 * flows and sizes each ring by its own group count. */
struct tbt_fec_replay *tbt_fec_replay_new_k(u16 groups, u16 n, u16 m, u16 s,
                                            u16 k);
void tbt_fec_replay_free(struct tbt_fec_replay *r);
u16 tbt_fec_replay_k(const struct tbt_fec_replay *r);
/* Effective K for this shape without allocating: the largest k <=
 * TBT_FEC_REPLAY_K_MAX whose ring fits TBT_FEC_REPLAY_BUDGET, or 0. The
 * sender ring, the receiver NACK window and the credit cap all use this
 * value. */
u16 tbt_fec_replay_k_for(u16 groups, u16 n, u16 m, u16 s);
size_t tbt_fec_replay_bytes(u16 groups, u16 n, u16 m, u16 s, u16 k);
#ifndef __KERNEL__
/* Host allocation ledger: every tbt_fec_alloc/free pair must balance. */
long tbt_fec_host_live_allocs(void);
#endif

/* The FEC in-flight window in DATA packets, derived from the effective K.
 *
 * The sender may run at most K-2 super-blocks past the delivered frontier:
 * the ring holds K, and two are slack for the ACK in flight. K_eff is the
 * smallest of the ring K, the receiver's arena `slots` and
 * TBT_FEC_REPLAY_K_MAX; `slots` may be 0 when the caller has no arena. The
 * result is rounded down to whole super-blocks so the sender never opens a
 * partial block the ring cannot cover, and capped to `max_packets`
 * (TBT_FEC_CREDIT_MAX) to bound memory. A K too small to hold the two-block
 * slack still grants one super-block: the stream must move, and repair is
 * already unavailable. */
static inline u32 tbt_fec_credit_packets(u16 groups, u16 n, u16 k_ring,
                                         u16 slots, u32 max_packets)
{
    u32 per_block = (u32)groups * n;
    u32 k_eff = k_ring;
    u32 blocks;

    if (!per_block)
        return 1;
    if (slots && k_eff > slots)
        k_eff = slots;
    if (k_eff > TBT_FEC_REPLAY_K_MAX)
        k_eff = TBT_FEC_REPLAY_K_MAX;
    blocks = k_eff > 2 ? k_eff - 2 : 1;
    if (max_packets)
    {
        u32 limit = max_packets / per_block;

        if (blocks > limit)
            blocks = limit;
    }
    return blocks ? blocks * per_block : 1;
}

/* How many whole super-blocks a packet grant is worth. The credit arrives
 * in packets because that is what the receiver's arena counts; the sender
 * spends it in blocks, which is the unit that survives a short cut. */
static inline u32 tbt_fec_credit_blocks(u32 credit_packets, u16 groups,
                                        u16 n)
{
    u32 per_block = (u32)groups * n;
    u32 blocks;

    if (!per_block || !credit_packets)
        return 0;
    blocks = credit_packets / per_block;
    return blocks ? blocks : 1;
}

/* The sender's credit gate, in whole super-blocks.
 *
 * The old gate compared packet sequence numbers (`seq - peer_pkts <
 * credit`). That identity holds only while every block carries groups*n
 * shares: `tbt_fec_tx_close_block(advance)` moves seq to the next block
 * boundary when a block is cut early, so a short block spends a full
 * block of the packet window while carrying only win_len shares. The
 * window then no longer bounds blocks, and the packetizer can close it in
 * the middle of the next block. A block cut after one 2 KB share is the
 * field case: 0.7.0 delivered 383 MB and then spent 118 s sending
 * one-share blocks, the receiver's frontier stuck on a block with no
 * stored share (fec_replay_miss_never_stored +84, gap_us 28.8 s).
 *
 * The window is what the receiver actually buffers: `slots` super-blocks,
 * minus two for the ACK in flight, and the replay ring holds the same K.
 * Counting blocks makes a short block cost one block, exactly like a full
 * one, so the arena and the ring agree whatever the cut. It also lets the
 * packetizer finish a block it started: the block id does not change while
 * the block fills, so a gate that was open when the block began stays
 * open until the cut. Zero credit is a closed window, never an open one.
 */
static inline bool tbt_fec_credit_block_ok(u64 blk, u64 frontier,
                                           u32 credit_blocks)
{
    if (!credit_blocks)
        return false;
    if (blk < frontier)
        blk = frontier;
    return blk - frontier < credit_blocks;
}

/* Ensure the block's record plane exists. Process context: this may
 * allocate one block plane (or reuse a hot one) and must run before the
 * caller takes the TX replay spinlock. */
int tbt_fec_replay_ensure_block(struct tbt_fec_replay *r, u64 block_id,
                                tbt_fec_spinlock_t *lock);
/* Store one complete encoded record (header included). A share outside
 * [0, n+m) or a record longer than header+S is refused; a block whose plane
 * was not ensured is refused with -ESTALE. */
int tbt_fec_replay_store(struct tbt_fec_replay *r, u64 block_id, u16 group,
                         u16 share, const u8 *rec, u16 len);
/* Copy a stored record into out. Returns its length, -ESTALE when the block
 * plane is no longer resident, -ENODATA when that share was never stored, or
 * -ENOSPC when out is too small. */
int tbt_fec_replay_get(const struct tbt_fec_replay *r, u64 block_id,
                       u16 group, u16 share, u8 *out, u16 cap);
bool tbt_fec_replay_has(const struct tbt_fec_replay *r, u64 block_id,
                        u16 group, u16 share);
/* When this share was last queued for replay, 0 when never. The caller holds
 * the TX replay lock (the ring is storage only). */
u64 tbt_fec_replay_sent_ns(const struct tbt_fec_replay *r, u64 block_id,
                           u16 group, u16 share);
/* Stamp a share that was just queued for replay. A share the ring does not
 * hold is ignored: there is nothing in flight to deduplicate. */
void tbt_fec_replay_mark_sent(struct tbt_fec_replay *r, u64 block_id,
                              u16 group, u16 share, u64 now_ns);
/* Detach every resident plane under the TX replay lock. The caller frees
 * drop[] after unlocking; cap must hold k + hot_n entries. */
void tbt_fec_replay_reset_locked(struct tbt_fec_replay *r, u8 **drop,
                                 u32 *ndrop, u32 cap);
/* Drop every resident block and its stamps. Caller holds no replay lock
 * (host test and teardown). */
void tbt_fec_replay_reset(struct tbt_fec_replay *r);
/* Return block planes the peer has acknowledged to the hot pool (max 8, or
 * 4 when low_mem). Runs in process context and frees overflow. */
void tbt_fec_replay_ack(struct tbt_fec_replay *r, u64 frontier, bool low_mem,
                        tbt_fec_spinlock_t *lock);
/* Count resident planes at or above the ACK floor. */
u32 tbt_fec_replay_unacked(struct tbt_fec_replay *r, tbt_fec_spinlock_t *lock);
/* Reclaim planes under memory pressure or on a stream reset. keep_active
 * keeps the newest 8 unacknowledged planes; otherwise all active planes are
 * freed. Up to keep_hot free planes remain for reuse. */
void tbt_fec_replay_shrink(struct tbt_fec_replay *r, bool keep_active,
                           u32 keep_hot, tbt_fec_spinlock_t *lock);

/* NACK pacing. The first NACK for a block goes out once the gap has been
 * visible for one round trip (the carrier TCP still holds the share and the
 * block's parity may still decode it); an identical NACK is retried after
 * the longer interval. There is no total attempt cap: while a carrier TCP is
 * alive the repair continues, and only the replay window bounds it. */
#define TBT_FEC_NACK_MIN_MS 10
#define TBT_FEC_NACK_RETRY_MS 20
/* The retry interval grows with the observed repair latency, up to this cap.
 * A sender whose replay queue is behind cannot serve a faster stream of
 * NACKs; asking faster only adds duplicate work ahead of the shares already
 * queued. */
#define TBT_FEC_NACK_RETRY_MAX_MS 500
/* A block that stays on the frontier after several NACKs doubles its retry
 * interval up to this cap. The repair latency on the 300 ms WAN reached
 * 850 ms while the retry cadence stayed below it; every extra request put a
 * duplicate replay ahead of DATA. The cap is well above one repair so a
 * genuinely lost request still retries. */
#define TBT_FEC_NACK_BACKOFF_MAX_MS 2000
/* A shape probe asks the sender to describe a super-block it may not have
 * sent yet; repeating it at the repair cadence puts a NACK on the wire for
 * every retry interval of an idle connection. Pace it instead, and probe one
 * group per pass. */
#define TBT_FEC_NACK_PROBE_MS 250
/* How many probes one frontier block gets. The sender answers a block it has
 * built (marker or parity); silence means the block was never sent. Probing
 * an idle stream forever asked for the same absent shares 274k times on the
 * fix22 run, all of them never stored. After this many unanswered probes the
 * receiver stops and leaves the block to the stream's stall timer. */
#define TBT_FEC_NACK_PROBE_MAX 3
/* One NACK names at most this many shares: 64 slots of one 8 KiB share is
 * 512 KiB, one sixteenth of the 8 MiB replay queue, so a burst loss cannot
 * fill the queue with a single request. The receiver names the rest in its
 * next NACK. */
#define TBT_FEC_NACK_SHARES_MAX 64

/* How long a share that was just requested is left alone: one carrier round
 * trip, and never less than two retry intervals. Re-asking inside it only
 * duplicates a replay that is still in flight. */
static inline u64 tbt_fec_nack_hold_ns(u32 rtt_us, u32 cadence_ms)
{
    u64 rtt = (u64)rtt_us * 1000ULL;
    u64 floor = 2ULL * (u64)cadence_ms * 1000000ULL;

    return rtt > floor ? rtt : floor;
}

/* The first NACK for a block waits for one round trip after its gap was
 * first seen: the carrier TCP still holds the share, and the block's parity
 * may still decode it. A NACK sent before that returns a replay that is
 * almost always late (field: 338k late shares, 853 ms mean repair). */
static inline bool tbt_fec_nack_first_due(u64 now_ns, u64 first_gap_ns,
                                          u64 hold_ns)
{
    return first_gap_ns && now_ns - first_gap_ns >= hold_ns;
}

/* A shape probe for an unbuilt super-block: one interval, one group. */
static inline bool tbt_fec_nack_probe_due(u64 now_ns, u64 last_probe_ns)
{
    return !last_probe_ns ||
           now_ns - last_probe_ns >=
               (u64)TBT_FEC_NACK_PROBE_MS * 1000000ULL;
}

static inline bool tbt_fec_nack_probe_allowed(u8 tries)
{
    return tries < TBT_FEC_NACK_PROBE_MAX;
}

/* A frontier the sender has already moved past (a later block was seen) is
 * closed: the sender built it and must answer, so the probe cap that stops
 * an idle stream from storming does not apply. fix26: block 185 with
 * hi=186 was skipped after three probes, and its missing shares were never
 * asked for again. */
static inline bool tbt_fec_nack_probe_open(bool sender_ahead, u8 tries)
{
    return sender_ahead || tbt_fec_nack_probe_allowed(tries);
}

/* The NACK scan targets only the frontier super-block. A later block cannot
 * be delivered before it, so asking for its shares wastes the wire and
 * starves the block that holds the peer's ACK. fix26 skipped next=185 for
 * s=186 563 times: 185 was never asked again and the reorder window
 * stopped. */
static inline bool tbt_fec_nack_frontier_only(u64 s, u64 next_super)
{
    return s == next_super;
}

/* The retry cadence follows the observed repair latency: a quarter of it,
 * clamped to [retry, max]. Back off at once, recover one eighth at a time so
 * one fast repair does not collapse the window. */
static inline u32 tbt_fec_nack_cadence_ms(u32 prev_ms, u64 repair_ns)
{
    u64 want = repair_ns / 4000000ULL;
    u32 next;

    if (want < TBT_FEC_NACK_RETRY_MS)
        want = TBT_FEC_NACK_RETRY_MS;
    if (want > TBT_FEC_NACK_RETRY_MAX_MS)
        want = TBT_FEC_NACK_RETRY_MAX_MS;
    next = (u32)want;
    if (prev_ms < TBT_FEC_NACK_RETRY_MS)
        prev_ms = TBT_FEC_NACK_RETRY_MS;
    if (next >= prev_ms)
        return next;
    return (prev_ms * 7 + next) / 8;
}

/* Why one share is left out of a NACK bitmap. */
enum tbt_fec_nack_skip
{
    TBT_FEC_NACK_KEEP = 0,
    TBT_FEC_NACK_SKIP_PRESENT,  /* already in the block: nothing to ask */
    TBT_FEC_NACK_SKIP_INFLIGHT, /* asked within the hold window */
};

static inline enum tbt_fec_nack_skip
tbt_fec_nack_share_skip(bool present, u64 now_ns, u64 last_req_ns,
                        u64 hold_ns)
{
    if (present)
        return TBT_FEC_NACK_SKIP_PRESENT;
    if (last_req_ns && now_ns - last_req_ns < hold_ns)
        return TBT_FEC_NACK_SKIP_INFLIGHT;
    return TBT_FEC_NACK_KEEP;
}

/* A replay for this share is already queued or on the wire. */
static inline bool tbt_fec_replay_dup(u64 now_ns, u64 last_sent_ns,
                                      u64 hold_ns)
{
    return last_sent_ns && now_ns - last_sent_ns < hold_ns;
}

/* The whole block lies below the peer's delivered packet frontier. The
 * frontier is absolute, so the comparison is against the shape origin. */
static inline bool tbt_fec_block_delivered_base(u64 block_id, u64 peer_pkts,
                                                u64 base_pkt,
                                                u64 base_block, u16 groups,
                                                u16 n)
{
    u64 span = (u64)groups * n;

    if (!span || peer_pkts < base_pkt)
        return false;
    return base_block + (peer_pkts - base_pkt) / span > block_id;
}

static inline bool tbt_fec_block_delivered(u64 block_id, u64 peer_pkts,
                                           u16 groups, u16 n)
{
    return tbt_fec_block_delivered_base(block_id, peer_pkts, 0, 0, groups, n);
}

/* The FIN may be queued once the packetizer and every new DATA record are
 * out. Replay records are duplicates of shares already encoded and sent
 * once; they cannot extend the stream, so a replay backlog must not hold
 * the FIN. Waiting for it let an 8 MiB backlog outlive the 5 s close budget
 * and reset a 99.63% transfer (fix20 1 GiB, close_timeouts=1). */
static inline bool tbt_fec_fin_ready(bool pkt_idle, u32 new_data_queued)
{
    return pkt_idle && !new_data_queued;
}

/* While a stream is closing, the replay queue is capped lower so the last
 * block's repair fits inside the close budget. The normal bound is passed in
 * by the caller (TBT_FEC_REPLAY_BYTES_MAX), which keeps this header free of
 * session state. */
#define TBT_FEC_REPLAY_CLOSE_BYTES_MAX (2u * 1024 * 1024)

static inline u32 tbt_fec_replay_cap(bool closing, u32 normal_max)
{
    if (closing && normal_max > TBT_FEC_REPLAY_CLOSE_BYTES_MAX)
        return TBT_FEC_REPLAY_CLOSE_BYTES_MAX;
    return normal_max;
}

/* The queue is above this mark: replay is throttled so a repair burst
 * cannot own every carrier while new DATA waits (fix22: an 8 MiB queue and
 * 4.6M flush skips left the download with no progress for 120 s). The
 * throttle still leaves enough drain bandwidth: one record per pass made
 * fix23 worse (flush skips x116, HOL wait x1278). */
#define TBT_FEC_REPLAY_PASS_THROTTLED 4u

static inline u32 tbt_fec_replay_high_water(u32 cap)
{
    return cap - cap / 4; /* 75% */
}

/* Above the mark four replay records per flush pass, then the bound data
 * queues get their turn; below it the pass keeps its normal limit. The
 * throttle never raises a smaller limit. */
static inline u32 tbt_fec_replay_pass_limit(bool over_high, u32 normal_limit)
{
    if (over_high && normal_limit > TBT_FEC_REPLAY_PASS_THROTTLED)
        return TBT_FEC_REPLAY_PASS_THROTTLED;
    return normal_limit;
}

/* A NACK named share n of a group that never wrote into the block. No record
 * is resident, so the replay ring cannot answer; the empty marker is the
 * truth. The block must be closed (its shape is final) and still inside the
 * ring: a block the ring has wrapped past is not empty, its shares existed
 * and are gone. `group_blk_id` is the block the group is building now, so it
 * differs from `block_id` for every closed block; the ring test is what
 * separates "never built for this group" from "evicted". */
static inline bool tbt_fec_nack_empty_group(bool closed, bool in_window,
                                            u64 group_blk_id, u64 block_id)
{
    return closed && in_window && group_blk_id != block_id;
}

/* What the sender can honestly answer when a NACK names a share it does not
 * hold. A covering parity row answers a short block. A resident block proves
 * this group wrote into it, so the group is not empty and a marker would be
 * a lie: the share is padding that will never exist. Only a block the ring
 * does not hold (never built and still inside the window) is an empty group. */
enum tbt_fec_absent_reply
{
    TBT_FEC_ABSENT_PARITY = 0, /* a covering parity row answers the share */
    TBT_FEC_ABSENT_MARKER,     /* the group is empty in this block */
    TBT_FEC_ABSENT_INVALID,    /* padding/never stored: no answer exists */
};

static inline enum tbt_fec_absent_reply
tbt_fec_absent_reply(bool block_resident, bool covering_parity)
{
    if (covering_parity)
        return TBT_FEC_ABSENT_PARITY;
    if (block_resident)
        return TBT_FEC_ABSENT_INVALID;
    return TBT_FEC_ABSENT_MARKER;
}

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
 * state. `now_ns` is monotonic; `retry_ms` is the adaptive retry interval
 * (tbt_fec_nack_cadence_ms), never below TBT_FEC_NACK_RETRY_MS. */
bool tbt_fec_nack_due_at(struct tbt_fec_nack_state *st, u64 now_ns,
                         u64 block_id, const struct tbt_fec_nack *n,
                         u32 retry_ms);

/* The fixed-cadence form, kept for callers that do not track the repair
 * latency (and for the host codec tests). */
static inline bool tbt_fec_nack_due(struct tbt_fec_nack_state *st, u64 now_ns,
                                    u64 block_id, const struct tbt_fec_nack *n)
{
    return tbt_fec_nack_due_at(st, now_ns, block_id, n,
                               TBT_FEC_NACK_RETRY_MS);
}
void tbt_fec_nack_reset(struct tbt_fec_nack_state *st);

int tbt_fec_cfg_pack(const struct tbt_fec_cfg *c, u8 out[TBT_FEC_CFG_LEN]);
/* `carriers` is the receiving session's effective carrier count. The body's
 * groups must name exactly `carriers / (n + m)` groups, one share per
 * carrier; the old fixed 256 rejected every 32/64/128-carrier INIT/CFG. */
int tbt_fec_cfg_unpack(const u8 in[TBT_FEC_CFG_LEN], u16 carriers,
                       struct tbt_fec_cfg *c);

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
struct tbt_fec_ops;

/* groups in 1..TBT_FEC_MAX_GROUPS; the same (n,m,k,w,d) for every group.
 * The caller binds each group independently because each has its own L.
 * Group g uses seed_g = nonce ^ ((u64)g << 32); sender and receiver derive it
 * the same way from FEC_CFG.seed, so no per-group seed travels on the wire. */
struct tbt_fec_grouped *tbt_fec_grouped_new(u16 groups, u16 n, u16 m, u16 k, u16 w,
                                            size_t s, u16 d, u64 nonce);

/* Same container with an explicit codec table. The RLNC constructor above is
 * the mode-1 special case. A mode-3 table builds one context per group with
 * that table's new() and the caller's parameters, so the data plane keeps one
 * codec object whatever the codec is. `new` returning NULL fails the whole
 * construction. */
struct tbt_fec_grouped *tbt_fec_grouped_new_ops(const struct tbt_fec_ops *ops,
                                                u16 groups, u16 n, u16 m,
                                                u16 k, u16 w, size_t s,
                                                u16 d, u64 nonce);
void tbt_fec_grouped_free(struct tbt_fec_grouped *g);
u16 tbt_fec_grouped_count(const struct tbt_fec_grouped *g);
struct tbt_fec_rlnc *tbt_fec_grouped_ctx(struct tbt_fec_grouped *g, u16 group);

/* Codec dispatch for the data plane. Every call is a no-op or an error when
 * `group` is out of range; none allocates. */
u8 tbt_fec_grouped_mode(const struct tbt_fec_grouped *g);
int tbt_fec_grouped_encode(struct tbt_fec_grouped *g, u16 group, u8 *data[],
                           u32 L, u8 *parity[]);
int tbt_fec_grouped_decode(struct tbt_fec_grouped *g, u16 group, u8 *data[],
                           u8 *parity[], const u8 *data_present,
                           const u8 *parity_present, u32 L);
/* Re-seeds an RLNC context. An SC-LDPC graph is fixed at construction, so
 * this is a no-op there and the data plane may call it unconditionally. */
void tbt_fec_grouped_set_nonce(struct tbt_fec_grouped *g, u16 group,
                               u64 nonce);
/* RLNC parity band of a group; 0 for a codec that has none (SC-LDPC), which
 * makes the NACK reply replay-only instead of regenerating a parity row. */
u16 tbt_fec_grouped_band(const struct tbt_fec_grouped *g, u16 group);
/* Whether share `index` of the group's last decode is complete. For a block
 * codec this is the only safe success oracle: decode() returns a count, not
 * a whole-block status. */
int tbt_fec_grouped_is_known(const struct tbt_fec_grouped *g, u16 group,
                             u32 index);

/* Adaptive mapping (0.3.0, TBT_FEC_CONV 0x1006). `base_pkt` and
 * `base_block` are the relative origin of the current shape: the first
 * record of the shape has seq base_pkt and block id base_block. A switch
 * only moves the origin, so one set of formulas covers every shape.
 * base_pkt == base_block == 0 reproduces the 0.2.x mapping exactly.
 *
 * The caller guarantees seq >= base_pkt. A record below the origin belongs
 * to a block the switch already delivered; the receiver drops it before
 * these helpers run. */
static inline u16 tbt_fec_group_of_seq_base(u64 seq, u64 base_pkt, u16 groups)
{
    return groups ? (u16)((seq - base_pkt) % groups) : 0;
}

/* Legacy stripe helpers. Route-A DATA placement uses h.reserved and
 * h.share_index instead; these remain for the old host models and the
 * carrier-of-seq map. group = seq % groups. */
static inline u16 tbt_fec_group_of_seq(u64 seq, u16 groups)
{
    return tbt_fec_group_of_seq_base(seq, 0, groups);
}

/* Share inside the group: (relative seq / groups) % n. The packetizer uses
 * it to pick the DATA share, so a shape switch only changes the origin. */
static inline u16 tbt_fec_share_of_seq_base(u64 seq, u64 base_pkt, u16 groups,
                                            u16 n)
{
    if (!groups || !n)
        return 0;
    return (u16)(((seq - base_pkt) / groups) % n);
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

/* block_id is the group-local generation. Route-A lays every block's
 * groups*n packets contiguously, so the block is the span quotient:
 *   block_id = base_block + (seq - base_pkt) / (groups * n).
 * It does not encode the group and does not demux streams. */
static inline u64 tbt_fec_block_of_seq_base(u64 seq, u64 base_pkt,
                                            u64 base_block, u16 groups,
                                            u16 n)
{
    u64 span = (u64)groups * n;

    if (!span || seq < base_pkt)
        return 0;
    return base_block + (seq - base_pkt) / span;
}

static inline u64 tbt_fec_block_of_seq(u64 seq, u16 groups, u16 k)
{
    return tbt_fec_block_of_seq_base(seq, 0, 0, groups, (u16)k);
}

/* The same gate over the fields the TX stall path can read under RCU. Both
 * the packetizer and tbt_tx.c's reset decision must use this one definition:
 * a mirror that still compared packet numbers would call a block-credit wait
 * a dead ACK path and reset a stream the receiver is still draining. */
static inline bool tbt_fec_credit_waiting(u64 peer_pkts, u64 seq,
                                          u32 credit, u64 base_pkt,
                                          u64 base_block, u16 groups, u16 n)
{
    u32 credit_blocks;
    u64 frontier, blk;

    if (!credit || !groups || !n)
        return true;
    credit_blocks = tbt_fec_credit_blocks(credit, groups, n);
    if (peer_pkts < base_pkt)
        peer_pkts = base_pkt;
    if (seq < base_pkt)
        seq = base_pkt;
    frontier = tbt_fec_block_of_seq_base(peer_pkts, base_pkt, base_block,
                                         groups, n);
    blk = tbt_fec_block_of_seq_base(seq, base_pkt, base_block, groups, n);
    return !tbt_fec_credit_block_ok(blk, frontier, credit_blocks);
}

/* A stream owns a subset of the session's physical groups. `mask` is the
 * 256-bit set (bit p = physical group p); `map[local]` is the physical group
 * of local index 0..count-1. Bits at or above total_groups are invalid, and
 * an empty set is invalid: every stream needs at least one group. */
static inline bool tbt_fec_group_mask_valid(const u8 mask[TBT_FEC_BITMAP_BYTES],
                                            u16 total_groups, u16 *count)
{
    u16 p, n = 0;

    if (!mask || !count || !total_groups ||
        total_groups > TBT_FEC_MAX_GROUPS)
        return false;
    for (p = 0; p < TBT_FEC_MAX_GROUPS; p++)
    {
        bool set = (mask[p / 8] & (u8)(1u << (p % 8))) != 0;

        if (set && p >= total_groups)
            return false;
        if (set)
            n++;
    }
    if (!n)
        return false;
    *count = n;
    return true;
}

/* SC-LDPC has one flow and asymmetric directions: the OPEN bitmap describes
 * the peer's TX groups, which can exceed this side's local TX group count.
 * Validate it against the 256-bit wire ceiling instead of the local pool. */
static inline bool tbt_fec_open_mask_valid(bool sclc, u16 total,
                                           const u8 *mask, u16 *count)
{
    return tbt_fec_group_mask_valid(mask,
                                    sclc ? TBT_FEC_MAX_GROUPS : total,
                                    count);
}

/* Fill map[] with the physical groups named by mask, in ascending order.
 * Returns the count, or 0 for an invalid mask. */
static inline u16 tbt_fec_group_map_build(const u8 mask[TBT_FEC_BITMAP_BYTES],
                                          u16 total_groups,
                                          u16 map[TBT_FEC_MAX_GROUPS])
{
    u16 p, n = 0;

    if (!tbt_fec_group_mask_valid(mask, total_groups, &n))
        return 0;
    n = 0;
    for (p = 0; p < total_groups; p++)
        if (mask[p / 8] & (u8)(1u << (p % 8)))
            map[n++] = p;
    return n;
}

static inline u16 tbt_fec_phys_group(u16 local,
                                     const u16 map[TBT_FEC_MAX_GROUPS])
{
    return map[local];
}

/* Carrier of the share seq names, for a flow whose local group g maps to
 * map[g]. The record does not carry the carrier; both ends derive it from
 * seq and the same bitmap. */
static inline u16 tbt_fec_carrier_of_seq_map(u64 seq, u64 base_pkt, u16 groups,
                                             u16 n, u16 m,
                                             const u16 map[TBT_FEC_MAX_GROUPS])
{
    u16 g = tbt_fec_group_of_seq_base(seq, base_pkt, groups);
    u16 share = tbt_fec_share_of_seq_base(seq, base_pkt, groups, n);

    return (u16)(tbt_fec_phys_group(g, map) * (n + m) + share);
}

/* Global seq of packet p (0..n-1) of group `group` inside `block_id`.
 * Route-A packs a block's groups*n packets contiguously, group-major:
 *   seq = base_pkt + (block-base_block)*groups*n + group*n + p.
 * This is the inverse of tbt_fec_block_of_seq_base() for one share. */
static inline u64 tbt_fec_seq_of_block_pkt_base(u64 block_id, u16 group,
                                                u16 p, u64 base_pkt,
                                                u64 base_block, u16 groups,
                                                u16 n)
{
    return base_pkt + (block_id - base_block) * (u64)groups * n +
           (u64)group * n + p;
}

static inline u64 tbt_fec_seq_of_block_pkt(u64 block_id, u16 group, u16 p,
                                           u16 groups, u16 n)
{
    return tbt_fec_seq_of_block_pkt_base(block_id, group, p, 0, 0, groups, n);
}

/* Last super-block that holds at least one packet, given the stream's total
 * packet count P (an absolute pkt_end). Undefined for P == 0 or P <=
 * base_pkt; callers check that first. */
static inline u64 tbt_fec_final_block_of_total_base(u64 total_pkts,
                                                    u64 base_pkt,
                                                    u64 base_block,
                                                    u16 groups, u16 n)
{
    if (!total_pkts || total_pkts <= base_pkt || !groups || !n)
        return 0;
    return base_block + (total_pkts - base_pkt - 1) / ((u64)groups * n);
}

static inline u64 tbt_fec_final_block_of_total(u64 total_pkts, u16 groups,
                                               u16 n)
{
    return tbt_fec_final_block_of_total_base(total_pkts, 0, 0, groups, n);
}

/* Number of real DATA packets group `group` has in its final block when the
 * stream holds `total_pkts` packets. 0 means the group has no packet in that
 * super-block. The sender zero-fills the remaining positions before encoding
 * parity; the receiver marks them known-zero and never delivers them.
 * Route-A's group-major layout makes the group's share run contiguous:
 * [first, first+n) with first = block_seq0 + group*n. */
static inline u16 tbt_fec_group_tail_len_base(u64 total_pkts, u64 base_pkt,
                                              u64 base_block, u16 groups,
                                              u16 n, u16 group)
{
    u64 final_block;
    u64 first;
    u64 avail;

    if (!total_pkts || total_pkts <= base_pkt || !groups || !n ||
        group >= groups)
        return 0;
    final_block = tbt_fec_final_block_of_total_base(total_pkts, base_pkt,
                                                    base_block, groups, n);
    first = tbt_fec_seq_of_block_pkt_base(final_block, group, 0, base_pkt,
                                          base_block, groups, n);
    if (total_pkts <= first)
        return 0;
    avail = total_pkts - first;
    return (u16)(avail < n ? avail : n);
}

static inline u16 tbt_fec_group_tail_len(u64 total_pkts, u16 groups, u16 n,
                                         u16 group)
{
    return tbt_fec_group_tail_len_base(total_pkts, 0, 0, groups, n, group);
}

/* Real share count for a block. The parity's win_len is authoritative; the
 * final-block tail formula is only the fallback when every parity row was
 * lost. A tail of 0 with a present DATA share is a stale stream end (FIN
 * froze pkt_end before that share was packetized): do not treat the share as
 * padding; keep n and let parity/NACK state the real cut. */
static inline u16 tbt_fec_blk_real_len_from_tail_base(
    u16 nreal, u16 n, bool total_known, u64 total_pkts, u64 base_pkt,
    u64 base_block, u16 groups, u64 block_id, u16 group, bool has_data)
{
    u16 len = nreal ? nreal : n;

    if (nreal || !total_known)
        return len;
    if (block_id == tbt_fec_final_block_of_total_base(total_pkts, base_pkt,
                                                      base_block, groups, n))
    {
        u16 tail = tbt_fec_group_tail_len_base(total_pkts, base_pkt,
                                               base_block, groups, n, group);

        if (tail < len && !(tail == 0 && has_data))
            len = tail;
    }
    return len;
}

static inline u16 tbt_fec_blk_real_len_from_tail(u16 nreal, u16 n,
                                                 bool total_known,
                                                 u64 total_pkts, u16 groups,
                                                 u64 block_id, u16 group,
                                                 bool has_data)
{
    return tbt_fec_blk_real_len_from_tail_base(
        nreal, n, total_known, total_pkts, 0, 0, groups, block_id, group,
        has_data);
}

/* Route-A real length from the block's own origin. The group's share run is
 * [seq0 + group*n, seq0 + (group+1)*n); only the part before FIN's packet
 * end is real. A present DATA share in a block the stream end predates keeps
 * n: a stale LAST must not turn a real share into padding. */
static inline u16 tbt_fec_blk_real_len_seq0(u16 nreal, u16 n,
                                            bool total_known, u64 total_pkts,
                                            u64 seq0, u16 group,
                                            bool has_data)
{
    u64 first, avail;

    if (nreal)
        return nreal;
    if (!total_known || !n)
        return n;
    first = seq0 + (u64)group * n;
    if (total_pkts <= first)
        return has_data ? n : 0;
    avail = total_pkts - first;
    return (u16)(avail < n ? avail : n);
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
static inline bool tbt_fec_nack_unstarted_group_base(
    u64 total_pkts, u64 base_pkt, u64 base_block, u16 groups, u16 n,
    u64 block, u16 group, bool total_known)
{
    if (!total_known)
        return false;
    if (block == tbt_fec_final_block_of_total_base(total_pkts, base_pkt,
                                                   base_block, groups, n) &&
        !tbt_fec_group_tail_len_base(total_pkts, base_pkt, base_block, groups,
                                     n, group))
        return false;
    return true;
}

static inline bool tbt_fec_nack_unstarted_group(u64 total_pkts, u16 groups,
                                                u16 n, u64 block, u16 group,
                                                bool total_known)
{
    return tbt_fec_nack_unstarted_group_base(total_pkts, 0, 0, groups, n,
                                             block, group, total_known);
}

/* How many parity rows the sender emits for one block.
 *
 * A short block must state its cut (win_len and epoch) and cover the shares
 * it actually wrote. Both SC-LDPC codecs route DATA share s to parity row
 * s mod m, so rows 0..win_len-1 each cover one of the first win_len shares.
 * The old row-0-only rule left shares 1..win_len-1 with no covering row at
 * all, and a lost one could only return through a NACK replay. A full set of
 * m rows would put a megabyte of parity behind a one-share interactive
 * write, so a short SC-LDPC block sends min(m, win_len); a block that
 * reached win_len == n is full and keeps all m rows. The coupled graph can
 * still need more equations than one row per share; the NACK replay remains
 * the fallback for the rest. Every other codec keeps all m rows.
 */
static inline u16 tbt_fec_tx_parity_count(u16 win_len, u16 n, u16 m,
                                          bool block_code)
{
    if (!m || !win_len)
        return 0;
    if (block_code)
        return win_len >= n ? m : (win_len < m ? win_len : m);
    return m;
}

/* How many DATA shares a NACK may name for one block.
 *
 * A present share j proves the block has at least j + 1 real shares: real
 * shares are 0..real-1 with no hole, so a missing share below the highest
 * present one is a real gap. A missing share above it may be zero padding
 * the sender never wrote -- the tail of a short block -- and naming it costs
 * a round trip that answers nothing (field: fec_replay_miss_never_stored
 * 42,963 on the 1 GiB run). Until the parity's win_len states the cut, only
 * the proven range is requested. `seen_hi` is the highest present DATA
 * index plus one, 0 when none is present.
 */
static inline u16 tbt_fec_nack_data_limit(bool win_len_set, u16 real,
                                          u16 seen_hi)
{
    if (win_len_set)
        return real;
    return seen_hi < real ? seen_hi : real;
}

/* How many parity rows a NACK may name for one block.
 *
 * Before win_len arrives only row 0 is named: it states the cut, and until
 * then the block may hold any number of rows. After the cut is known, a
 * short SC-LDPC block has min(m, real) rows, while a full block has all m.
 * Every other codec keeps m, because there the rows are the repair.
 */
static inline u16 tbt_fec_nack_parity_limit(bool win_len_set, u16 real,
                                            u16 n, u16 m, bool block_code)
{
    (void)n;
    if (!m)
        return 0;
    if (!block_code)
        return m;
    if (!win_len_set)
        return 1;
    return real < m ? real : m;
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

/* What to do with a record that arrives for a block that is already settled.
 *
 * A ready block is immutable: a duplicate or late record must not change a
 * byte that may already be on its way to the stream. Treating those records
 * as protocol errors killed the carrier that carried them -- one replayed
 * empty marker per NACK became rx_proto=93 and 102 carrier replacements on
 * the 1 GiB run -- so the arrival is classified here and the caller only
 * counts it.
 *
 * `ready` and `empty` describe the resident block; `has_real` says it already
 * holds a DATA or PARITY share (the marker-after-data test). */
enum tbt_fec_arrival
{
    TBT_FEC_ARR_PLACE = 0, /* the record may still be placed */
    TBT_FEC_ARR_LATE,      /* ready block: a late or duplicate share */
    TBT_FEC_ARR_DUP_MARKER,/* an empty marker for a block already empty */
    TBT_FEC_ARR_CONFLICT,  /* marker and real shares disagree: ignore */
};

/* A PARITY record with win_len 0 says "this group has no packet in this
 * block". It carries no payload, so receiving it again is idempotent. */
static inline bool tbt_fec_is_marker(u8 type, u16 win_len)
{
    return type == TBT_FEC_PARITY && win_len == 0;
}

static inline enum tbt_fec_arrival
tbt_fec_arrival_kind(bool ready, bool empty, u8 type, u16 win_len,
                     bool has_real)
{
    bool marker = tbt_fec_is_marker(type, win_len);

    if (ready)
    {
        if (marker)
            return TBT_FEC_ARR_DUP_MARKER;
        /* A real share for a block the sender marked empty: the two shapes
         * disagree. The block is already delivered, so ignore the share. */
        return empty ? TBT_FEC_ARR_CONFLICT : TBT_FEC_ARR_LATE;
    }
    if (empty)
        return marker ? TBT_FEC_ARR_DUP_MARKER : TBT_FEC_ARR_CONFLICT;
    if (marker && has_real)
        return TBT_FEC_ARR_CONFLICT;
    return TBT_FEC_ARR_PLACE;
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
    TBT_FEC_CODEC_SC_LDPC = 2, /* downlink rate 0.50, see tbt_fec_sclc.h */
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
extern const struct tbt_fec_ops tbt_fec_ops_sclc;

/* ------------------------------------------------------------------ *
 * SC-LDPC shape (mode 3, TBT_FEC_CONV 0x1006)
 *
 * The wire carries no codec field, so the shape itself names the codec
 * variant and its graph parameters. Both variants are block codes over one
 * FEC block of n data shares and m parity shares:
 *
 *   downlink   groups == 1: one binary SC-IRA block of n+m = 256 shares.
 *              block = w, L = L, rate = n/(n+m) in parts per thousand.
 *              tbt_fec_ops_sclc.new(n, m, rate_ppm, block, S, 0, seed).
 *   uplink     groups  > 1: one GF(16) SC-IRA block per group, n+m shares
 *              each. block = w, L = L, rate = n/(n+m) scaled to 1/256.
 *              tbt_fec_ops_sclc_gf16.new(n, m, block, L, S, rate_256, seed).
 *
 * `rate` is derived from n/(n+m), never configured: a shape whose share
 * ratio disagrees with the graph rate would silently lose parity capacity.
 * block and L come from the session configuration; 0 selects the defaults
 * below.
 * ------------------------------------------------------------------ */

#define TBT_FEC_SCLC_BLOCK_DOWN 256
#define TBT_FEC_SCLC_L_DOWN 8
#define TBT_FEC_SCLC_BLOCK_UP 128
#define TBT_FEC_SCLC_L_UP 16

/* The binary downlink builds exactly these n/m splits of the 256 shares;
 * the transmitted rate is n / 256. The order is the rate ladder used by
 * tbt_fec_adapt_sclc.c: 25, 30, 40, 50, 60 and 70 percent. */
static inline bool tbt_fec_sclc_down_split(u16 n, u16 m)
{
    switch (n)
    {
    case 64:
        return m == 192;
    case 76:
        return m == 180;
    case 102:
        return m == 154;
    case 128:
        return m == 128;
    case 151:
        return m == 105;
    case 178:
        return m == 78;
    default:
        return false;
    }
}

/* The uplink GF(16) codec builds exactly these splits of 32 shares. The
 * 8/24 fallback is the same point as the 25% ladder step. */
static inline bool tbt_fec_sclc_up_split(u16 n, u16 m)
{
    switch (n)
    {
    case 8:
        return m == 24;
    case 9:
        return m == 23;
    case 12:
        return m == 20;
    case 16:
        return m == 16;
    case 18:
        return m == 14;
    case 22:
        return m == 10;
    default:
        return false;
    }
}

/* A block shape the receiver may place. `shares` is the direction's fixed
 * n+m (256 downlink, 32 uplink); `groups` selects the direction. The
 * SC-LDPC downlink binary codec builds any split with n <= 192 (m >= 64);
 * the SC-LDPC uplink GF(16) codec builds only its six ladder splits. An
 * RLNC codec builds any non-empty split of the same share count. */
static inline bool tbt_fec_block_shape_ok(u16 n, u16 m, u16 shares,
                                          u16 groups, bool block_code)
{
    if (!n || !m || (u16)(n + m) != shares)
        return false;
    if (!block_code)
        return true;
    if (groups == 1)
        return n <= 192;
    return tbt_fec_sclc_up_split(n, m);
}

/* The extra present shares beyond the block's real DATA length that an
 * SC-LDPC attempt needs before it is worth waking. The downlink margin comes
 * from tbt_fec_sclc_wake_extra_down() below (delta0 of the measured split);
 * the uplink 8x32 block keeps two. A decode that fails is retried at
 * TBT_FEC_DECODE_RETRY_MS, and a new share clears that backoff: an arriving
 * share is new evidence, not a reason to wait. */
#define TBT_FEC_SCLC_WAKE_EXTRA_UP 2u
#define TBT_FEC_DECODE_RETRY_MS 50u
#define TBT_FEC_DECODE_RETRY_MAX_MS 400u

/* Minimum present parity a downlink block needs before an SC-LDPC attempt,
 * by DATA-share count. The values are the measured delta0 of the sclc-down
 * table:
 *   64:13, 76:14, 102:15, 128:16, 151:13, 178:12.
 * A split between two points takes the larger neighbour; a split outside
 * the measured range takes the largest measured value. Both choices delay
 * an attempt rather than start one the graph cannot finish. */
static inline u16 tbt_fec_sclc_wake_extra_down(u16 n)
{
    static const u16 split[6] = {64, 76, 102, 128, 151, 178};
    static const u16 extra[6] = {13, 14, 15, 16, 13, 12};
    u16 i;

    for (i = 0; i < 6; i++)
        if (n == split[i])
            return extra[i];
    if (n < split[0] || n > split[5])
        return 16;
    for (i = 1; i < 6; i++)
        if (n < split[i])
            return extra[i] > extra[i - 1] ? extra[i] : extra[i - 1];
    return 16;
}

/* All real DATA shares are present, or enough shares are present to cover
 * the missing ones with the variant's spare-row margin. No time input: the
 * caller decodes on the share that crosses this line, in the same pass. */
static inline bool tbt_fec_sclc_decode_wake(u16 real, u16 n, u16 m,
                                            u16 groups, u16 ndata,
                                            u16 nparity)
{
    u16 extra;

    if (!real || !m)
        return false;
    if (ndata >= real)
        return true;
    if (ndata + nparity < real)
        return false;
    extra = groups > 1 ? TBT_FEC_SCLC_WAKE_EXTRA_UP
                       : tbt_fec_sclc_wake_extra_down(n);
    return (u32)ndata + nparity >= (u32)real + extra;
}

/* The wait a failed block decode arms before another attempt. A new share
 * clears it to zero; only a block that receives nothing keeps escalating. */
static inline u32 tbt_fec_decode_backoff_ms(u8 tries)
{
    if (tries >= 4)
        return TBT_FEC_DECODE_RETRY_MAX_MS;
    return TBT_FEC_DECODE_RETRY_MS << tries;
}

enum tbt_fec_sclc_variant
{
    TBT_FEC_SCLC_NONE = 0,
    TBT_FEC_SCLC_DOWN = 1, /* binary, one group of 256 shares */
    TBT_FEC_SCLC_UP = 2,   /* GF(16), one block per group */
};

struct tbt_fec_sclc_shape
{
    u8 variant; /* enum tbt_fec_sclc_variant */
    u16 n;
    u16 m;
    u16 groups;
    u16 block;    /* codeword block, the codec's w */
    u16 L;
    u16 rate_ppm; /* n / (n+m) in parts per thousand, for FEC_CFG.k */
    u16 rate_256; /* the same rate in 1/256 units, the GF(16) factory's d */
};

/* Resolve a configured (mode, n, m, carriers, block, L) into the codec
 * variant and its graph parameters. `block`/`L` of 0 take the variant
 * default. Returns 0, or -EINVAL when the shape is not an SC-LDPC shape
 * (bad carrier multiple, n or m zero, unsupported block). */
int tbt_fec_sclc_shape_of(u16 n, u16 m, u16 carriers, u16 block, u16 L,
                          struct tbt_fec_sclc_shape *out);

/* Resolve the shape a peer announced in FEC_CFG. The two variants reuse the
 * fields differently, and the receiver must take every graph parameter from
 * the wire, never from its own configuration: the peer's transmit L is not
 * this side's L (a downlink L=8 peer receives an uplink L=16 block).
 *
 *   binary  d == 0, k in 100..1000:  block = w, rate = k, L unused
 *   GF(16)  k > w:                   block = k, L = w, rate_256 = d
 *
 * Returns 0, or -EINVAL when the body is not an SC-LDPC shape. */
int tbt_fec_sclc_shape_of_cfg(const struct tbt_fec_cfg *cfg, u16 carriers,
                              struct tbt_fec_sclc_shape *out);

/* Build one grouped container for a resolved shape. The downlink variant is
 * a single binary context; the uplink variant is one GF(16) context per
 * group. Returns NULL when the shape or the codec rejects the parameters. */
struct tbt_fec_grouped *tbt_fec_sclc_new(const struct tbt_fec_sclc_shape *sh,
                                         size_t s, u64 nonce);

/* ------------------------------------------------------------------ *
 * Adaptive shape (0.3.0, TBT_FEC_CONV 0x1006)
 *
 * The shape was frozen per session in 0.2.x. From 0.3.0 the sender may move
 * it at a super-block boundary; `base_pkt`/`base_block` are the relative
 * origin of the shape currently in force (see the mapping helpers above).
 * `slots` is the receiver's reorder depth in blocks per group, not a wire
 * field: the receiver derives it from the arena and a switch keeps it.
 * ------------------------------------------------------------------ */

struct tbt_fec_shape
{
    u16 n;
    u16 m;
    u16 groups;
    u16 slots;
    u16 w;
    u16 k;
    u8 d;
    /* v8 codec identity. `w/k/d` above stay the RLNC window (or, for an
     * SC-LDPC shape, the legacy mirror derived by cfg_unpack) so the
     * existing mapping helpers and codec selection read one vocabulary. */
    u8 codec_variant; /* 0 RLNC single, 1 RLNC grouped, 2 SC-LDPC */
    u8 L;             /* variant 2: 8 or 16; otherwise 0 */
    u16 block;        /* variant 2: 256 or 512; otherwise 0 */
    u32 rate_ppm;     /* variant 2: 250000..700000; otherwise 0 */
    u64 base_pkt;
    u64 base_block;
};

enum tbt_fec_switch_state
{
    TBT_FEC_SW_STEADY = 0, /* no switch in flight */
    TBT_FEC_SW_CFG_SENT,   /* sender: CFG sent, waiting for CFG_ACK */
    TBT_FEC_SW_DRAIN,      /* sender: packetizer stopped at P0 */
    TBT_FEC_SW_PREP,       /* receiver: allocating the new codec */
    TBT_FEC_SW_ARMED,      /* receiver: CFG_ACK sent, waiting for P0 */
    TBT_FEC_SW_APPLIED,    /* receiver: new shape installed */
    TBT_FEC_SW_COOLDOWN,   /* sender: abort finished, cooling down */
};

/* One shape change in flight. The wire form is FEC_CFG; this struct is the
 * in-kernel record both sides keep while the two-phase commit runs. `flags`
 * is reserved for the WP6/WP7 state machine. */
struct tbt_fec_switch
{
    u32 epoch;
    u8 state;
    u8 flags;
    u64 switch_pkt;
    u64 block_base;
    struct tbt_fec_shape shape;
};

/* Control and estimator entry points. Phase 1 freezes the interface only;
 * WP4-WP7 provide the bodies. */
struct tbt_session;
struct tbt_fec_blk;

int tbt_fec_switch_begin(struct tbt_session *sess, struct tbt_fec_shape shape);
void tbt_fec_switch_on_cfg(struct tbt_session *sess,
                           const struct tbt_fec_cfg *cfg);
void tbt_fec_switch_on_fbk(struct tbt_session *sess,
                           const struct tbt_fec_fbk *fbk);
void tbt_fec_switch_on_frontier(struct tbt_session *sess, u64 next_deliver);

/* Called only from w_fec_rx, the single writer of the estimator state. */
void tbt_fec_est_recovered(struct tbt_session *sess, u16 group,
                           struct tbt_fec_blk *b);
void tbt_fec_est_requested(struct tbt_session *sess, u16 group, u64 block_id,
                           u16 share);
void tbt_fec_est_super_done(struct tbt_session *sess, u64 super);

#endif /* _TBT_FEC_H */
