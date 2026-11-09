/* tbt_fec_ms.h - stream multiplexing inside one SC-LDPC FEC plane.
 *
 * SC-LDPC carries the whole session on one 1x256 downlink block (or one
 * 8x32 grouped uplink block). There is no spare group to give a second
 * stream, so the payload byte stream inside that single FEC plane is shared:
 * a sequence of self-delimiting subframes, each naming its stream id, its
 * stream byte offset, and whether it closes the stream. FEC repair still
 * works on shares; the receiver parses whatever shares the decoder recovers
 * and routes each subframe to its own stream window.
 *
 * Subframe (little-endian, 32 bytes, payload follows immediately):
 *   +0  u32 magic   TBT_MS_MAGIC
 *   +4  u8  ver     TBT_MS_VER
 *   +5  u8  type    TBT_MS_DATA / TBT_MS_FIN / TBT_MS_RST
 *   +6  u16 flags   must be 0
 *   +8  u32 len     payload bytes; 0 for FIN and RST
 *   +12 u64 sid     40-bit stream id, the main-wire domain
 *   +20 u64 off     stream byte offset of this payload (FIN: end offset)
 *   +28 u32 crc32c  CRC32C over bytes +0..+27
 *
 * A DATA subframe's payload is at most TBT_MS_DATA_MAX bytes, so one
 * subframe plus its header fits a share and a short stream never waits for
 * a full block. Subframes are packed back to back; a packer never splits a
 * subframe across blocks. Every byte the FEC plane delivers is a subframe
 * header or payload, so there is no padding on the wire.
 *
 * The two halves are pure C and own no lock. The data plane serialises TX
 * packing with one lock and RX feeding with another; delivery reads are
 * single-writer (the RX work item). Streams within one half are isolated:
 * a blocked or reset stream never changes another stream's bytes.
 */
#ifndef _TBT_FEC_MS_H
#define _TBT_FEC_MS_H

#include "tbt_fec.h"

#define TBT_MS_MAGIC 0x31534d54u /* "TMS1" in little-endian bytes */
#define TBT_MS_VER 1
#define TBT_MS_HDR_LEN 32
#define TBT_MS_DATA_MAX TBT_FEC_S
#define TBT_MS_MAX_STREAMS 32
/* Default flow-control window per stream. One 1x256 SC-LDPC block is 1 MiB,
 * so a single stream may fill four complete blocks before the window stops
 * it. This covers a 320 ms WAN BDP and keeps short blocks to the final tail
 * instead of turning every 256 KiB into one. */
#define TBT_MS_WINDOW (4u * 1024u * 1024u)
/* Bytes one stream may contribute per visit inside a block. */
#define TBT_MS_QUANTUM 64u

enum tbt_ms_type
{
    TBT_MS_DATA = 1,
    TBT_MS_FIN = 2,
    TBT_MS_RST = 3,
};

/* ------------------------------------------------------------------ *
 * Subframe header
 * ------------------------------------------------------------------ */

struct tbt_ms_hdr
{
    u32 magic;
    u8 ver;
    u8 type;
    u16 flags;
    u32 len;
    u64 sid;
    u64 off;
    u32 crc32c;
};

/* Pack/unpack the 32-byte header. Pack fills the CRC; unpack copies bytes
 * only. Both return 0, or -EINVAL for a NULL argument. */
int tbt_ms_hdr_pack(const struct tbt_ms_hdr *h, u8 out[TBT_MS_HDR_LEN]);
int tbt_ms_hdr_unpack(const u8 in[TBT_MS_HDR_LEN], struct tbt_ms_hdr *h);
/* Structural check plus CRC. DATA: len 1..max_payload, off+len must not
 * wrap. FIN/RST: len == 0. flags == 0, sid != 0. Returns 0 or -EPROTO. */
int tbt_ms_hdr_check(const struct tbt_ms_hdr *h, u32 max_payload);

/* ------------------------------------------------------------------ *
 * Shared-plane admission ledger
 *
 * The one SC-LDPC plane is referenced by carriers that are still admitting
 * a stream (pending) and by streams whose mux leg is attached (refs). The
 * plane is unlinked only when both reach zero. The helpers are plain
 * arithmetic: every caller serialises updates with the flow's allocation
 * lock (fec_alloc_lock + sess->lock). They live here, not in the kernel
 * file, so host tests can drive the attach/abort interleavings directly.
 * ------------------------------------------------------------------ */

struct tbt_ms_admit
{
    u16 pending;
    u16 refs;
};

static inline void tbt_ms_admit_claim(struct tbt_ms_admit *a)
{
    if (a->pending != (u16)~0u)
        a->pending++;
}

static inline void tbt_ms_admit_abort(struct tbt_ms_admit *a)
{
    if (a->pending)
        a->pending--;
}

static inline void tbt_ms_admit_attach(struct tbt_ms_admit *a)
{
    if (a->pending)
        a->pending--;
    if (a->refs != (u16)~0u)
        a->refs++;
}

static inline void tbt_ms_admit_detach(struct tbt_ms_admit *a)
{
    if (a->refs)
        a->refs--;
}

static inline bool tbt_ms_admit_last(const struct tbt_ms_admit *a)
{
    return !a->pending && !a->refs;
}

/* ------------------------------------------------------------------ *
 * Sender
 *
 * One sender owns every outgoing stream of one session direction. Bytes are
 * reserved in a per-stream ring from the capture path, then packed into
 * whole blocks by the FEC packetizer. A stream may have at most `window`
 * bytes beyond the peer's last window report in flight; FIN and RST are
 * control and are never held by that gate.
 * ------------------------------------------------------------------ */

struct tbt_ms_tx;

/* max_streams 1..TBT_MS_MAX_STREAMS, block_bytes >= TBT_MS_HDR_LEN +
 * TBT_MS_DATA_MAX, window >= 2 * TBT_MS_DATA_MAX. Block staging memory is
 * block_bytes; each stream ring is window + one maximum subframe. */
struct tbt_ms_tx *tbt_ms_tx_new(u16 max_streams, u32 block_bytes, u32 window,
                                u32 quantum);
void tbt_ms_tx_free(struct tbt_ms_tx *tx);

/* Create a stream starting at offset 0 with a full initial window. */
int tbt_ms_tx_open(struct tbt_ms_tx *tx, u64 sid);
/* Drop every queued byte and the stream state. */
int tbt_ms_tx_close(struct tbt_ms_tx *tx, u64 sid);
/* Allocation-free lifecycle for the kernel path: allocate the per-stream
 * ring outside any spinlock, then install/remove only the pointer under the
 * lock. install fails with -EEXIST/-ENOSPC and leaves the ring owned by the
 * caller; remove fills `out` for a free outside the lock. */
struct tbt_ms_tx_ring
{
    u64 sid;
    u8 *ring;
    u32 cap;
};
int tbt_ms_tx_ring_new(struct tbt_ms_tx *tx, u64 sid,
                       struct tbt_ms_tx_ring *out);
void tbt_ms_tx_ring_free(struct tbt_ms_tx_ring *ring);
int tbt_ms_tx_install(struct tbt_ms_tx *tx, const struct tbt_ms_tx_ring *ring);
int tbt_ms_tx_remove(struct tbt_ms_tx *tx, u64 sid,
                     struct tbt_ms_tx_ring *out);
/* Queue FIN after the bytes already accepted. Idempotent. */
int tbt_ms_tx_fin(struct tbt_ms_tx *tx, u64 sid);
/* Drop queued bytes (not yet sent) and queue RST at the current offset. */
int tbt_ms_tx_rst(struct tbt_ms_tx *tx, u64 sid);
/* Peer window report: delivered is the stream offset the peer has handed
 * over, credit the bytes it will buffer beyond that. */
int tbt_ms_tx_window(struct tbt_ms_tx *tx, u64 sid, u64 delivered, u32 credit);

/* Bytes the stream can accept now. */
u32 tbt_ms_tx_space(struct tbt_ms_tx *tx, u64 sid);
/* Reserve up to want contiguous bytes at the ring tail. Returns the length
 * available (0 when full), and stores the pointer. The caller then commits
 * exactly that many bytes. No member of a subframe is written until commit. */
u32 tbt_ms_tx_reserve(struct tbt_ms_tx *tx, u64 sid, u32 want, u8 **p);
void tbt_ms_tx_commit(struct tbt_ms_tx *tx, u64 sid, u32 len);
/* Copy at most len bytes; returns the accepted count. */
u32 tbt_ms_tx_write(struct tbt_ms_tx *tx, u64 sid, const u8 *src, u32 len);

/* Build the next block of the shared byte stream. Packs whole subframes from
 * all streams with credit, round-robin from the persistent cursor, at most
 * `quantum` bytes per stream per visit. Returns the block length in *len and
 * its buffer in *buf; *len == 0 when no stream has sendable bytes. The
 * returned bytes are exactly what the FEC plane must carry; the caller
 * repeats until 0. `*len` never exceeds block_bytes. */
int tbt_ms_tx_block(struct tbt_ms_tx *tx, const u8 **buf, u32 *len);

/* Whether the stream still has bytes queued or a control frame pending. */
bool tbt_ms_tx_pending(struct tbt_ms_tx *tx, u64 sid);
/* Bytes queued across every stream, and whether any FIN/RST frame is waiting
 * to be packed. The packetizer uses both to coalesce a partial block. */
u32 tbt_ms_tx_queued(struct tbt_ms_tx *tx);
bool tbt_ms_tx_has_control(struct tbt_ms_tx *tx);

/* ------------------------------------------------------------------ *
 * Receiver
 *
 * Feed arbitrary byte runs (shares, or any prefix of them) in FEC delivery
 * order. The parser keeps a header/payload state machine, so a subframe may
 * span feeds. Each stream has a ring of `window` bytes; its app side peeks
 * contiguous bytes and consumes them. A full ring or an out-of-order
 * subframe marks that stream broken and drops its payload; other streams
 * are untouched.
 * ------------------------------------------------------------------ */

struct tbt_ms_rx;

struct tbt_ms_rx *tbt_ms_rx_new(u16 max_streams, u32 window);
void tbt_ms_rx_free(struct tbt_ms_rx *rx);

/* Optional: mark a stream before its first subframe. feed() never creates a
 * leg for an unknown sid; only this call does, bounded by max_streams. */
int tbt_ms_rx_open(struct tbt_ms_rx *rx, u64 sid);
/* Drop the leg and its bytes. A later subframe for the sid is ignored until
 * an explicit open creates a new leg. */
int tbt_ms_rx_close(struct tbt_ms_rx *rx, u64 sid);

/* Consume up to len bytes of the shared stream. Returns the consumed count;
 * a header or payload that is not complete stays buffered. On a bad header,
 * CRC, or stream offset, the offending stream is marked broken and the rest
 * of its payload in that run is skipped; the return still names what was
 * consumed so the caller can make progress. */
u32 tbt_ms_rx_feed(struct tbt_ms_rx *rx, const u8 *data, u32 len);

/* Bytes buffered for this stream and not yet consumed. */
u32 tbt_ms_rx_avail(struct tbt_ms_rx *rx, u64 sid);
/* Stream offset consumed by the application so far. */
u64 tbt_ms_rx_delivered(const struct tbt_ms_rx *rx, u64 sid);
/* Pointer to the contiguous prefix; *len is at most max. Returns 0 on
 * success (len may be 0), -ENOENT for an unknown/closed stream. */
int tbt_ms_rx_peek(struct tbt_ms_rx *rx, u64 sid, const u8 **p, u32 max,
                   u32 *len);
/* Mark len bytes consumed. */
int tbt_ms_rx_consume(struct tbt_ms_rx *rx, u64 sid, u32 len);

/* FIN state: true once a FIN subframe arrived and every earlier byte was
 * consumed. *end_off receives the FIN offset. */
bool tbt_ms_rx_fin(struct tbt_ms_rx *rx, u64 sid, u64 *end_off);
/* RST state: true once an RST subframe arrived; the buffer is empty from
 * then on. */
bool tbt_ms_rx_reset(struct tbt_ms_rx *rx, u64 sid);
/* True once the stream is broken and all its later bytes are dropped. */
bool tbt_ms_rx_broken(struct tbt_ms_rx *rx, u64 sid);

/* Window report. delivered is the consumed offset, credit the current ring
 * size. `force` reports even when the change is small. Returns 1 when a
 * report is due, 0 otherwise. */
int tbt_ms_rx_win(struct tbt_ms_rx *rx, u64 sid, u64 *delivered, u32 *credit,
                  bool force);
/* Mark the report accepted (updates the last-reported offsets). */
void tbt_ms_rx_win_ack(struct tbt_ms_rx *rx, u64 sid, u64 delivered);

/* Round-robin cursor for the delivery walk: returns 1 and a sid when a leg
 * has bytes (or a FIN to observe), advancing the cursor; 0 when none. */
int tbt_ms_rx_next(struct tbt_ms_rx *rx, u64 *sid);
u16 tbt_ms_rx_streams(const struct tbt_ms_rx *rx);

#endif /* _TBT_FEC_MS_H */
