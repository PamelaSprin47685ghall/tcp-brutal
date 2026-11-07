/* tbt_uapi.h - constants shared by the module and whoever reads its output.
 *
 * No kernel headers: a userspace tool, a test script, or a shell can include
 * this file unchanged. Everything here is part of the operator contract:
 *   - the frame layout on the wire (DESIGN.md §3.3);
 *   - the /proc control surface and its file names (§8.4);
 *   - the module version (§11) and the wire revision TBT_CONV.
 *
 * Any change to a wire value is a protocol change: bump TBT_CONV with it.
 */
#ifndef _TBT_UAPI_H
#define _TBT_UAPI_H

/* Wire integer types. In the module they come from <linux/types.h>; a host
 * program that includes this header alone gets them from <stdint.h>. One
 * definition serves both builds, so a host test reads the same structs and
 * the same constants the module uses. */
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#define TBT_VERSION_MAJOR 0
#define TBT_VERSION_MINOR 2
#define TBT_VERSION_PATCH 0
#define TBT_VERSION ((TBT_VERSION_MAJOR << 16) | (TBT_VERSION_MINOR << 8) | TBT_VERSION_PATCH)

/* Roles, as carried in HELLO and in the `role=` key. */
#define TBT_ROLE_NEAR 2
#define TBT_ROLE_FAR 3

/* Wire protocol revision: u16 at frame offset +30, 0x0006. Independent of the
 * module version (DESIGN.md §11); bump it alone on any wire change.
 * 0x0002 moved body_len into the header (+6) and removed the 4-byte prefix.
 * 0x0003 carries RETIRE (24) and the HELLO profile|gen byte; a 0x0002 peer
 * cannot name a generation, so it must be refused rather than guessed at.
 * 0x0004 makes sid_hi meaningful (the full 40-bit sid, tbt_proto.c:89),
 * redefines the per-path FBK uack as the cumulative payload bytes received on
 * that path rather than a stream seq high-water mark, and adds SACK (25) so a
 * direction with no DATA to carry the ack still confirms a stream; HELLO
 * body[0] became TBT_WIRE_VERSION in the same revision. An old peer would read
 * the new uack and sid_hi under their old meaning, so the revision is bumped
 * instead of extended in place. Compatibility is hard, never silent:
 * tbt_hdr_check() requires conv == TBT_CONV (tbt_proto.c:77), the first
 * record (HELLO) fails with -EPROTO, and the carrier closes. Neither side
 * parses the other's bytes under the wrong layout; both ends must be upgraded
 * together.
 * 0x0005 removes the placeholder frames 0x11 (PREDICT) and 0x12 (CANCEL).
 * The receiver never advanced delivery or promoted a copy from them, so they
 * carried no application byte and bought no progress; a 0x0004 peer that
 * still sends one fails tbt_hdr_check() and its carrier closes. The sender's
 * real repair path (retained bytes plus duplicate) and the receiver's bounded
 * hole reset are unchanged.
 * 0x0006 (0.2.0) makes FIN carry a 16-byte body (TBT_FIN_BODY_LEN): the
 * absolute packet seq after the stream's last DATA packet and the aligned
 * packet seq of its first one. A 0.1.0 peer is refused rather than guessed
 * at: tbt_hdr_check() requires 0x0006 and tbt_body_bounds() requires exactly
 * 16 bytes for FIN, so the first HELLO fails with -EPROTO and the carrier
 * closes. Both ends must be upgraded together. */
#define TBT_CONV 0x0006u

/* HELLO body[0]: wire-protocol version, independent of TBT_VERSION. The FEC
 * v6 session uses version 2 and the extended body below; the old ARQ keeps
 * version 1 and 23 bytes. */
#define TBT_WIRE_VERSION 1
#define TBT_WIRE_VERSION_V6 2

/* ------------------------------------------------------------------ *
 * FEC wire: an independent record format, TBT_FEC_CONV 0x1004.
 *
 * The old ARQ record above is frozen at 16 carriers: its path_ack_mask is
 * u16 and its FBK/WINDOW bodies have 16 entries. FEC never reuses those
 * fields. A receiver tells the two formats apart by the first header byte:
 * old records start with 0x10..0x25, FEC records with 0x30..0x35.
 * The conv field is checked too, so a wrong-format byte stream fails closed.
 *
 * T0 frozen layout (little-endian, 40 bytes):
 *   +0  u8  type        0x30 DATA, 0x31 PARITY, 0x32 ACK, 0x33 NACK,
 *                       0x34 FEC_CFG, 0x35 FEC_FBK
 *   +1  u8  flags       bit0 LAST; every other bit must be zero
 *   +2  u16 share_index 0..n+m-1; u16 so n+m=256 is representable
 *   +4  u8  n           data shares in the block
 *   +5  u8  m           parity shares in the block
 *   +6  u16 epoch       block-level, PARITY only: the payload length of share
 *                       win_len-1, 0 when that share is full (S). DATA must
 *                       carry 0. A receiver that recovers the last share from
 *                       parity has no payload_len for it, and the guess "S"
 *                       would deliver the block's zero padding. The first
 *                       PARITY of a block fixes (win_len, epoch); every later
 *                       PARITY must repeat them exactly.
 *   +8  u64 block_id    block/window identity chosen by the sender
 *   +16 u64 seq         DATA: packet sequence; PARITY: window end
 *   +24 u16 win_len     PARITY: covered packets; DATA: 0
 *   +26 u16 payload_len payload bytes following this header
 *   +28 u16 conv        0x1004
 *   +30 u16 reserved    must be 0
 *   +32 u32 payload_crc32c  CRC32C over the payload only
 *   +36 u32 header_crc32c   CRC32C over header bytes +0..+35
 *
 * A receiver checks conv, type and length first, then header_crc32c, then
 * payload_crc32c. It never uses a field from a header that failed either CRC.
 * Control frames are complete records of their own and must never be queued
 * behind a partial DATA record (the carrier writer emits each record whole).
 * ------------------------------------------------------------------ */
#define TBT_FEC_CONV 0x1004u
/* 0.2.0 (M2) raises TBT_FEC_CONV from 0x1001 to 0x1002. The core change is
 * that a PARITY record may be a block marker: win_len == 0 && payload_len ==
 * 0 && epoch == 0. The two convs share one carrier (32-byte control records
 * use TBT_CONV, 40-byte FEC records use TBT_FEC_CONV), so both are upgraded
 * in the same version.
 *
 * 0.2.1 raises it to 0x1003: the ACK body grows from 16 to 24 bytes and
 * carries deliver_bytes, the stream-relative byte count the receiver has
 * actually handed to its output socket. The old body let the sender derive
 * that number as next_deliver * S, which is exact only while every block is
 * full: a short share or a block closed early makes the packet frontier name
 * bytes that were never delivered, and tx_ack then runs past them.
 *
 * 0.2.2 raises it to 0x1004: NACK becomes a repair request. Its body is a
 * group bitmap followed by a share bitmap (TBT_FEC_NACK_LEN = 64 bytes), and
 * the record header's block_id names the super-block. The old 0x1003 NACK
 * body was a list of missing sequence ranges and had no caller.
 *
 * There is no 4-byte marker and no magic number on this wire. A marker is a
 * set of values inside the 40-byte tbt_fec_hdr: type TBT_FEC_PARITY (0x31),
 * flags 0, share_index n (parity 0), epoch 0, win_len 0, payload_len 0,
 * conv 0x1004, payload_crc32c 0 (CRC32C of an empty payload). */

/* FEC record types. */
#define TBT_FEC_DATA 0x30
#define TBT_FEC_PARITY 0x31
#define TBT_FEC_ACK 0x32
#define TBT_FEC_NACK 0x33
#define TBT_FEC_CFG 0x34
#define TBT_FEC_FBK 0x35

/* FEC header flags: bit0 LAST, all others reserved and refused. */
#define TBT_FEC_FF_LAST 0x01
#define TBT_FEC_FF_MASK 0x01

/* FEC header field offsets and size. */
#define TBT_FEC_HDR_LEN 40
#define TBT_FEC_H_TYPE 0
#define TBT_FEC_H_FLAGS 1
#define TBT_FEC_H_SHARE 2
#define TBT_FEC_H_N 4
#define TBT_FEC_H_M 5
#define TBT_FEC_H_EPOCH 6
#define TBT_FEC_H_BLOCK 8
#define TBT_FEC_H_SEQ 16
#define TBT_FEC_H_WINLEN 24
#define TBT_FEC_H_PAYLEN 26
#define TBT_FEC_H_CONV 28
#define TBT_FEC_H_RESERVED 30
#define TBT_FEC_H_PAYCRC 32
#define TBT_FEC_H_HDRCRC 36

/* FEC payload and record bounds. S is the configured packet size, at most
 * TBT_FEC_PAYLOAD_MAX. A full record is header plus payload. */
#define TBT_FEC_PAYLOAD_MAX 8192
#define TBT_FEC_REC_MAX (TBT_FEC_HDR_LEN + TBT_FEC_PAYLOAD_MAX) /* 8232 */
#define TBT_FEC_CHUNK_MAX (TBT_FEC_REC_MAX + 4) /* hex size line + CRLF slack */

/* Share and carrier limits. shares = n + m <= 256; 1 share per carrier in
 * the production shape, so the carrier bitmap has one bit per share. */
#define TBT_FEC_MAX_SHARES 256
#define TBT_FEC_MAX_CARRIERS 256
#define TBT_FEC_BITMAP_BYTES (TBT_FEC_MAX_CARRIERS / 8) /* 32 */
#define TBT_FEC_MAX_W 1024
#define TBT_FEC_MAX_D 256
#define TBT_FEC_MAX_GROUPS 256

/* Control-frame payload lengths. */
/* next_deliver (u64) + credit_packets (u32) + rtt_us (u32) + deliver_bytes
 * (u64). The first 16 bytes keep their 0x1002 offsets so a reader that only
 * needs the frontier can still parse them. */
#define TBT_FEC_ACK_LEN 24
/* NACK body: group_mask[TBT_FEC_BITMAP_BYTES] then
 * share_mask[TBT_FEC_BITMAP_BYTES]. The record header's block_id names the
 * super-block. A sender replays every selected share for every selected
 * group; a share that is already present is a harmless duplicate. */
#define TBT_FEC_NACK_LEN (2 * TBT_FEC_BITMAP_BYTES)
#define TBT_FEC_CFG_LEN 32
#define TBT_FEC_FBK_LEN 48

/* Old ARQ keeps its 16-slot wire limit even though the carrier layer grows to
 * 256 slots. Every old wire field that was sized by the carrier count uses
 * this constant, never TBT_MAX_CARRIERS_LIMIT. */
#define TBT_LEGACY_MAX_CARRIERS 16

/* Frame types (DESIGN.md §3.3). */
enum tbt_frame_type
{
    TBT_F_DATA = 0x10,
    /* 0x11 (PREDICT) and 0x12 (CANCEL) were removed in TBT_CONV 0x0005. */
    TBT_F_OPEN = 0x13,
    TBT_F_FIN = 0x14,
    TBT_F_RST = 0x15,
    TBT_F_FBK = 0x20,
    TBT_F_WINDOW = 0x21,
    TBT_F_HELLO = 0x22,
    TBT_F_PAD = 0x23,
    /* Retire a far-side carrier slot before a replacement at the same index
     * joins: body = u8 index, seq = the retired generation. */
    TBT_F_RETIRE = 0x24,
    TBT_F_SACK = 0x25,
};

/* Frame flags, byte +1. */
#define TBT_FF_HEAD_OF_RUN 0x01
#define TBT_FF_DATA_VERIFIED 0x02
#define TBT_FF_KEEP 0x04
#define TBT_FF_LOW_PRIO 0x08
#define TBT_FF_MUST_ORDER 0x10
#define TBT_FF_HAS_TOKEN 0x20
/* Every defined flag; a receiver rejects any other bit. */
#define TBT_FF_MASK 0x3f

/* Header field offsets (DESIGN.md §3.3 is the authority; these must match it
 * byte for byte). */
#define TBT_HDR_LEN 32
#define TBT_H_TYPE 0
#define TBT_H_FLAGS 1
#define TBT_H_PATH 2
#define TBT_H_ACKC 3
#define TBT_H_ACKM 4
#define TBT_H_BODYLEN 6
#define TBT_H_SEQ 8
#define TBT_H_UACK 16
#define TBT_H_SIDLO 24
#define TBT_H_SIDHI 28
#define TBT_H_RES2 29
#define TBT_H_CONV 30

#define TBT_PAYLOAD_MAX 8192
#define TBT_REC_MAX (TBT_HDR_LEN + TBT_PAYLOAD_MAX) /* one record on the wire */
#define TBT_CHUNK_MAX 8228 /* chunk-data ceiling; a full record is TBT_REC_MAX */
#define TBT_MAX_CARRIERS_LIMIT 256 /* carrier slots; old ARQ caps at 16 */
/* Duplicate copies per DATA frame, the ceiling of the `dup` config key. M1
 * replaces the literal v > 3 in tbt_parse.c with this bound; the TX path
 * sizes its dup array by it. */
#define TBT_DUP_MAX 3

/* Body length bounds per frame type (DESIGN.md §3.3, "Payload layouts").
 * tbt_hdr_check() accepts a type only if tbt_body_bounds() knows it and the
 * body length is inside its range; FBK also has an exact length that depends
 * on another header field, and the checker enforces that too. */
/* FIN body (0.2.0, TBT_CONV 0x0006): exactly TBT_FIN_BODY_LEN bytes, little-
 * endian.
 *   +0  u64 pkt_end   absolute session packet seq after this stream's last
 *                     DATA packet
 *   +8  u64 pkt_base  absolute session packet seq of this stream's first DATA
 *                     packet, aligned to the super-block (groups*n)
 * tbt_body_bounds() sets TBT_F_FIN to *min = *max = TBT_FIN_BODY_LEN. A
 * non-FEC session sends the same 16 zero bytes (pkt_end = pkt_base = 0), so
 * one length rule covers both. */
#define TBT_FIN_BODY_LEN 16

/* FBK body: nr entries, each 18 bytes, no padding. nr is path_ack_count
 * (0..16), so the body is 0..288 bytes and the checker bounds it by
 * TBT_FBK_MAX. Fields are read byte by byte; uack at +2 is not 8-aligned on
 * the wire.
 *
 *   +0  u8  path_id
 *   +1  u8  rx_state
 *   +2  u64 uack
 *   +10 u32 recv_cnt
 *   +14 u32 rtt_us
 */
#define TBT_FBK_ENTRY_LEN 18
#define TBT_FBK_MAX (TBT_FBK_ENTRY_LEN * TBT_LEGACY_MAX_CARRIERS) /* 288 */
#define TBT_FBK_PATH 0     /* offsets within one entry */
#define TBT_FBK_RXSTATE 1
#define TBT_FBK_UACK 2
#define TBT_FBK_RECVCNT 10
#define TBT_FBK_RTT 14
#define TBT_WINDOW_BODY_LEN (8 + 8 * TBT_LEGACY_MAX_CARRIERS + 2) /* 138 */
/* HELLO body: u8 ver; u8 role; u8 nonce[16]; u16 payload_max; u16 chunk_max;
 * u8 profile (1 http-kal, 2 http-kal-drip, 3 raw). The last byte used to be a
 * zero flags byte; it now names the container, and a peer with a different
 * profile is refused rather than mis-parsed. The frame layout is unchanged. */
#define TBT_HELLO_BODY_LEN (1 + 1 + 16 + 2 + 2 + 1)              /* 23 */
/* FEC v6 HELLO: the 23 legacy bytes, then crypto (1), key_id (1),
 * reserved (1), crypto_nonce[8], key[32] at +23..+65 (T0 §2.3). The old ARQ
 * never sends this length. */
#define TBT_HELLO_BODY_LEN_V6 (TBT_HELLO_BODY_LEN + 43)           /* 66 */

/* Fill *min and *max with the body bounds of `type`. Returns false for a type
 * this revision does not define, so an unknown byte can never pass a bound
 * check by accident. */
static inline bool tbt_body_bounds(u8 type, u16 *min, u16 *max)
{
    switch (type)
    {
    case TBT_F_DATA:
        /* A zero-length DATA frame wedges the receiver; the emitter never
         * sends one, so the receiver refuses it. */
        *min = 1;
        *max = TBT_PAYLOAD_MAX;
        return true;
    case TBT_F_OPEN:
    case TBT_F_RST:
    case TBT_F_PAD:
        *min = 0;
        *max = 0;
        return true;
    case TBT_F_FIN:
        *min = TBT_FIN_BODY_LEN;
        *max = TBT_FIN_BODY_LEN;
        return true;
    case TBT_F_RETIRE:
    case TBT_F_SACK:
        *min = 1;
        *max = 1;
        return true;
    case TBT_F_FBK:
        *min = 0;
        *max = TBT_FBK_MAX;
        return true;
    case TBT_F_WINDOW:
        *min = TBT_WINDOW_BODY_LEN;
        *max = TBT_WINDOW_BODY_LEN;
        return true;
    case TBT_F_HELLO:
        /* Both the legacy 23-byte and the FEC v6 66-byte body are valid at
         * the framing layer; tbt_rx_hello() enforces the exact length for the
         * session's data plane. */
        *min = TBT_HELLO_BODY_LEN;
        *max = TBT_HELLO_BODY_LEN_V6;
        return true;
    default:
        return false;
    }
}

/* /proc action verbs (DESIGN.md §8.3). They are runtime actions: accepted on
 * the write side, rendered in the tunnels/peers state view, never part of the
 * canonical config. */
#define TBT_ACT_CARRIER "carrier"
#define TBT_ACT_SESSION "session"
#define TBT_ACT_DOWN "down"
#define TBT_ACT_UP "up"
#define TBT_ACT_DROP_PPM "drop_ppm"
#define TBT_ACT_NONCE "nonce" /* key of the session action */
#define TBT_DROP_PPM_MAX 1000000u
/* Refusal text when drop_ppm is written with debug_loss=0. */
#define TBT_DROP_PPM_GATE_MSG \
    "drop_ppm refused: load the module with debug_loss=1 to enable loss injection"

/* /proc/net/tbtun/peers: one line per carrier, columns in exactly this order.
 * Both the renderer and the host test use this string, so a silent format
 * change fails the build. */
#define TBT_PEERS_COLUMNS \
    "id carrier sport peer uack rtt_us inflight state class recv_cnt rx_lag_ppm " \
    "minrtt_us tx_frames tx_bytes dropped retx_ppm delivery_rate_bps cwnd_bytes " \
    "snd_wnd wscale rcv_wnd window_clamp rmem_alloc"

/* /proc/net/tbtun/tunnels: one line per tunnel, columns in exactly this order.
 * A near tunnel prints "-" in the target column, so every row has 9 columns. */
#define TBT_TUNNELS_COLUMNS "id role state listen peer target n m streams"

/* The /proc control surface (DESIGN.md §8.4).
 *
 * The directory lives under the net namespace's proc subtree, so the operator
 * path is /proc/net/tbtun. It is NOT created with proc_mkdir("net/tbtun", NULL):
 * procfs does not split a slash-separated name under a NULL parent on 7.x and
 * silently creates nothing. The parent must be init_net.proc_net (or
 * net->proc_net), and the name passed to proc_mkdir() is the single component
 * below it. TBT_PROC_ROOT is the one place the operator-visible path appears. */
#define TBT_PROC_SUBDIR "tbtun"
#define TBT_PROC_ROOT "/proc/net/" TBT_PROC_SUBDIR
#define TBT_PROC_CONFIG "config"
#define TBT_PROC_TUNNELS "tunnels"
#define TBT_PROC_PORTS "ports"
#define TBT_PROC_PEERS "peers"
#define TBT_PROC_STATS "stats"
/* Debug-only (debug >= 2): the last request header block this side emitted,
 * verbatim, headers only. Read-only. DESIGN.md §8.3/§8.4. */
#define TBT_PROC_HTTP "http"

#endif /* _TBT_UAPI_H */
