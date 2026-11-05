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
#define TBT_VERSION_MINOR 0
#define TBT_VERSION_PATCH 1
#define TBT_VERSION ((TBT_VERSION_MAJOR << 16) | (TBT_VERSION_MINOR << 8) | TBT_VERSION_PATCH)

/* Roles, as carried in HELLO and in the `role=` key. */
#define TBT_ROLE_NEAR 2
#define TBT_ROLE_FAR 3

/* Wire protocol revision: u16 at frame offset +30, 0x0005. Independent of the
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
 * hole reset are unchanged. */
#define TBT_CONV 0x0005u

/* HELLO body[0]: wire-protocol version, independent of TBT_VERSION. */
#define TBT_WIRE_VERSION 1

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
#define TBT_MAX_CARRIERS_LIMIT 16 /* path_ack_mask is 16 bits */

/* Body length bounds per frame type (DESIGN.md §3.3, "Payload layouts").
 * tbt_hdr_check() accepts a type only if tbt_body_bounds() knows it and the
 * body length is inside its range; FBK also has an exact length that depends
 * on another header field, and the checker enforces that too. */
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
#define TBT_FBK_MAX (TBT_FBK_ENTRY_LEN * TBT_MAX_CARRIERS_LIMIT) /* 288 */
#define TBT_FBK_PATH 0     /* offsets within one entry */
#define TBT_FBK_RXSTATE 1
#define TBT_FBK_UACK 2
#define TBT_FBK_RECVCNT 10
#define TBT_FBK_RTT 14
#define TBT_WINDOW_BODY_LEN (8 + 8 * TBT_MAX_CARRIERS_LIMIT + 2) /* 138 */
/* HELLO body: u8 ver; u8 role; u8 nonce[16]; u16 payload_max; u16 chunk_max;
 * u8 profile (1 http-kal, 2 http-kal-drip, 3 raw). The last byte used to be a
 * zero flags byte; it now names the container, and a peer with a different
 * profile is refused rather than mis-parsed. The frame layout is unchanged. */
#define TBT_HELLO_BODY_LEN (1 + 1 + 16 + 2 + 2 + 1)              /* 23 */

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
    case TBT_F_FIN:
    case TBT_F_RST:
    case TBT_F_PAD:
        *min = 0;
        *max = 0;
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
        *min = TBT_HELLO_BODY_LEN;
        *max = TBT_HELLO_BODY_LEN;
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
    "snd_wnd"

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
