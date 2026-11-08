/* tbt_fec.h - FEC wire (TBT_FEC_CONV 0x1006), control frames, and the
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
#else
#include <errno.h>
#endif

/* S: one packet slot on the wire, the only packet size this cut encodes.
 * tbt_fec_data.c and tbt_fec_ack_bytes() must agree on it. */
#define TBT_FEC_S TBT_FEC_PAYLOAD_MAX

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
    u16 reserved;
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

/* May a DATA/PARITY record with sid `rec` enter the current stream? `cur` is
 * the session's current far_sid; `barrier` is the old stream id installed by
 * tbt_session_fec_rx_barrier() before the new far_sid. A record from the old
 * sid must be dropped even while `cur` still names it, and a record from any
 * other sid after the switch must be dropped too. */
static inline bool tbt_fec_sid_accept(u64 cur, u64 barrier, u64 rec)
{
    if (!cur || (barrier && cur == barrier))
        return false;
    return rec == cur;
}

/* The session carries one stream at a time.  A record for a new sid whose
 * old stream is already gone must reset the data plane here: the session's
 * barrier is skipped when far_sid was cleared before the new stream started,
 * and the old blocks would otherwise stay at the delivery cursor and be
 * handed to the new stream. */
static inline bool tbt_fec_rx_stream_changed(u64 stream_sid, u64 rec_sid,
                                             bool old_stream_alive)
{
    return stream_sid && rec_sid && rec_sid != stream_sid &&
           !old_stream_alive;
}

/* The shared FEC TX ring carries no per-byte sid, so a stream switch must
 * drop the whole ring before the new stream's bytes enter it.  While the
 * packetizer still owns the old block -- or the last submit came from the
 * old stream before any block existed -- the new stream's first submit is
 * refused (backpressure) until the switch work has flushed the ring.  The
 * old stream is not queued at all: its bytes are consumed and discarded. */
static inline bool tbt_fec_tx_wait_switch(u64 far_sid, u64 cur_block_sid,
                                          u64 last_submit_sid, u64 st_sid)
{
    if (st_sid != far_sid)
        return false;
    return (cur_block_sid && cur_block_sid != st_sid) ||
           (last_submit_sid && last_submit_sid != st_sid);
}

/* A pending switch that names a stream the session has already left can
 * never be applied: the packetizer only clears it when it names the current
 * stream.  The submit path drops it so the current stream's first submit
 * records the old prefix boundary instead of being blocked forever. */
static inline bool tbt_fec_tx_switch_stale(u64 switch_sid, u64 far_sid)
{
    return switch_sid && switch_sid != far_sid;
}

/* The packetizer must own a switch when the block it built belongs to
 * another stream, when the ring's last submit came from another stream
 * (including the case where no block was built yet), or when a submit
 * already refused bytes for the stream taking over. */
static inline bool tbt_fec_tx_need_switch(u64 cur_block_sid,
                                          u64 last_submit_sid,
                                          u64 switch_sid, u64 st_sid)
{
    return (cur_block_sid && cur_block_sid != st_sid) ||
           (last_submit_sid && last_submit_sid != st_sid) ||
           (switch_sid && switch_sid == st_sid);
}

/* A queued record may be sent only for the session's current stream.  A
 * control record with sid 0 is session-scoped and stays; a DATA/PARITY or
 * replay record must name the current sid exactly. */
static inline bool tbt_fec_tx_record_sid_ok(u64 rec_sid, u64 far_sid,
                                            bool control)
{
    if (control)
        return !rec_sid || rec_sid == far_sid;
    return rec_sid == far_sid;
}

/* An empty marker answers a NACK for `nack_sid`; it may be sent only while
 * that stream is still the session's current one.  Stamping an old block's
 * marker with the new stream's sid lets the receiver rebase its window to an
 * old block id. */
static inline bool tbt_fec_tx_marker_sid_ok(u64 nack_sid, u64 far_sid)
{
    return !nack_sid || nack_sid == far_sid;
}

/* A stream the TX work may use: not dead, still attached to the session, and
 * not inside its tombstone window.  A gracefully closed stream keeps state
 * OPEN after detach, so the state alone is not enough. */
static inline bool tbt_fec_tx_stream_usable(bool dead, bool active,
                                            u64 tombstone_until_ns)
{
    return !dead && active && !tombstone_until_ns;
}

/* A block placed for one stream must never be delivered to another.  The
 * block carries the sid it was placed under; the delivery walk compares it
 * with the stream it is about to hand bytes to.  `stream_sid` 0 means no
 * stream has been adopted yet, so there is nothing to compare. */
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

/* Shape change request (0x1006, 48 bytes). See tbt_uapi.h for the byte
 * offsets; this struct is the host-side mirror, not the wire layout. */
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
};

/* Receiver report (0x1006, 48 bytes). `epoch` is the 32-bit generation
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
 * evicted. K is derived from the shape: the ring is allocated at the largest
 * K that fits TBT_FEC_REPLAY_BUDGET, up to TBT_FEC_REPLAY_K_MAX, so a shape
 * with small super-blocks (32/64 carriers) gets a window that covers the
 * carrier aggregate BDP, while the 256-carrier shape still lands on K=7.
 * The bounds do not depend on the stream length:
 *   ring   <= TBT_FEC_REPLAY_BUDGET (15 MiB of records),
 *   arena  <= TBT_FEC_RX_BUDGET (16 MiB, tbt_fec_data.c),
 *   credit <= TBT_FEC_CREDIT_MAX packets (8 MiB at S=8192).
 * A shape that cannot hold K=1 gets no ring; the credit cap follows the
 * effective K. */
#define TBT_FEC_REPLAY_K_MAX 64
#define TBT_FEC_REPLAY_BUDGET (15u * 1024 * 1024)
/* The ACK grants at most this many DATA packets of FEC credit. The credit
 * bounds the sender's in-flight data; the ring bounds the repair storage,
 * and both are constant-size for a given shape. */
#define TBT_FEC_CREDIT_MAX 1024u

struct tbt_fec_replay;

/* Returns NULL when the shape needs more than TBT_FEC_REPLAY_BUDGET even for
 * K = 1, or when an allocation fails. The effective K is then 0 and the data
 * plane falls back to parity and the TCP retransmission. */
struct tbt_fec_replay *tbt_fec_replay_new(u16 groups, u16 n, u16 m, u16 s);
void tbt_fec_replay_free(struct tbt_fec_replay *r);
u16 tbt_fec_replay_k(const struct tbt_fec_replay *r);
/* Effective K for this shape without allocating: the largest k <=
 * TBT_FEC_REPLAY_K_MAX whose ring fits TBT_FEC_REPLAY_BUDGET, or 0. The
 * sender ring, the receiver NACK window and the credit cap all use this
 * value. */
u16 tbt_fec_replay_k_for(u16 groups, u16 n, u16 m, u16 s);
size_t tbt_fec_replay_bytes(u16 groups, u16 n, u16 m, u16 s, u16 k);

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
/* When this share was last queued for replay, 0 when never. The caller holds
 * the TX replay lock (the ring is storage only). */
u64 tbt_fec_replay_sent_ns(const struct tbt_fec_replay *r, u64 block_id,
                           u16 group, u16 share);
/* Stamp a share that was just queued for replay. A share the ring does not
 * hold is ignored: there is nothing in flight to deduplicate. */
void tbt_fec_replay_mark_sent(struct tbt_fec_replay *r, u64 block_id,
                              u16 group, u16 share, u64 now_ns);
/* Drop every resident block and its stamps. A shape switch calls this after
 * it stops the old block: a replay for a block below the new base must not
 * survive, and the ring geometry (groups, n+m, K) is unchanged in Phase 1.
 * The caller holds the TX replay lock. */
void tbt_fec_replay_reset(struct tbt_fec_replay *r);

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

/* Application seq is striped across groups: group = seq % groups. */
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

/* block_id is the group-local generation: base_block + ((seq - base_pkt) /
 * groups) / n. It does not encode the group and does not demux streams. */
static inline u64 tbt_fec_block_of_seq_base(u64 seq, u64 base_pkt,
                                            u64 base_block, u16 groups,
                                            u16 n)
{
    if (!groups || !n)
        return 0;
    return base_block + ((seq - base_pkt) / groups) / n;
}

static inline u64 tbt_fec_block_of_seq(u64 seq, u16 groups, u16 k)
{
    return tbt_fec_block_of_seq_base(seq, 0, 0, groups, (u16)k);
}

/* Global seq of packet p (0..n-1) of group `group` inside `block_id`. The
 * stripe is group = seq % groups, so a block of every group covers one
 * contiguous span of groups*n global packets: the "super-block". */
static inline u64 tbt_fec_seq_of_block_pkt_base(u64 block_id, u16 group,
                                                u16 p, u64 base_pkt,
                                                u64 base_block, u16 groups,
                                                u16 n)
{
    return base_pkt + ((block_id - base_block) * n + p) * groups + group;
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
 * parity; the receiver marks them known-zero and never delivers them. */
static inline u16 tbt_fec_group_tail_len_base(u64 total_pkts, u64 base_pkt,
                                              u64 base_block, u16 groups,
                                              u16 n, u16 group)
{
    u64 final_block;
    u64 first;
    u64 last_local;

    if (!total_pkts || total_pkts <= base_pkt || !groups || !n ||
        group >= groups)
        return 0;
    final_block = tbt_fec_final_block_of_total_base(total_pkts, base_pkt,
                                                    base_block, groups, n);
    first = tbt_fec_seq_of_block_pkt_base(final_block, group, 0, base_pkt,
                                          base_block, groups, n);
    if (total_pkts <= first)
        return 0;
    last_local = (total_pkts - 1 - base_pkt - group) / groups;
    return (u16)(last_local - (final_block - base_block) * n + 1);
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
 * An SC-LDPC block code does not repair a lost DATA share from one parity
 * row: the erased share touches checks across five coupling blocks, and a
 * short block's prefix of rows recovers a different share than a full set
 * does. Trimming a short block to `win_len` rows would therefore leave real
 * losses unrecoverable while still paying tens of rows per short write. The
 * short block instead sends row 0 alone as a cut announcement (win_len and
 * epoch) and leaves the few real shares to the NACK replay, which is a
 * round trip on a share that was lost, not a megabyte of parity per block
 * that was not. Every other codec keeps all m rows: there m is small and
 * the rows themselves are the repair.
 */
static inline u16 tbt_fec_tx_parity_count(u16 win_len, u16 n, u16 m,
                                          bool block_code)
{
    if (!m)
        return 0;
    if (block_code && win_len && win_len < n)
        return 1;
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
 * Before win_len arrives only row 0 is named: it states the cut, and a short
 * block stored no other row. After the cut is known, a short SC-LDPC block
 * still has row 0 only, while a full block has all m. Every other codec
 * keeps m, because there the rows are the repair.
 */
static inline u16 tbt_fec_nack_parity_limit(bool win_len_set, u16 real,
                                            u16 n, u16 m, bool block_code)
{
    if (!m)
        return 0;
    if (!block_code)
        return m;
    if (!win_len_set)
        return 1;
    return real < n ? 1 : m;
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

/* The NACK scan must not ask while a block codec's first settle window is
 * open: the decode pass runs before the scan in the same work body, so the
 * block will be attempted before the scan would ask, and a replay would
 * restart the settle window (fix45: 482 NACKs, 27,465 replayed shares and
 * 226 MB in the first connect).  Once the pass has attempted and failed
 * (decode_tries != 0), the scan must ask: the next body's pass re-arms the
 * backoff before the scan runs, so gating the post-failure backoff would
 * suppress the only repair.  `s` is the super-block the scan is
 * considering; only the block at the delivery frontier is gated. */
static inline bool tbt_fec_nack_wait_decode(bool block_code, bool used,
                                            u64 block_id, u64 s,
                                            u16 nparity, u8 decode_tries,
                                            u64 now,
                                            u64 decode_next_ns)
{
    return block_code && used && block_id == s && nparity > 0 &&
           !decode_tries && now < decode_next_ns;
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
