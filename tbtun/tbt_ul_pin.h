/* tbt_ul_pin.h - deterministic uplink carrier pinning and the exact replay of
 * a parity-recovered share (0.8.0 Step 3).
 *
 * Scope: both ends of every carrier are this module. The near side dials the
 * far side's carrier port with its own socket, and the far side accepted that
 * connection in its own listener. No rule here ever looks at a socket the
 * module did not create, and the replay path only names `sess->car[]`.
 *
 * The near side sends one share per block on a fixed carrier:
 *
 *   carrier c carries group c/(n+m) share c%(n+m)
 *
 * A DATA share (share < n) therefore never leaves its carrier, and a control
 * frame (ACK, NACK, CFG, FBK, PAD, drip) never lands on a DATA carrier. Every
 * full DATA chunk on one carrier is the same length, so the far side can name
 * a lost chunk's byte position in that TCP stream:
 *
 *   chunk_seq = rx_body_start_seq + block_id * chunk_len
 *
 * The replay itself is a byte-identical copy of the segment the peer sent.
 * It is fed through the carrier socket's own input path, and TCP's duplicate
 * rules decide the rest: bytes already received are dropped, bytes that
 * arrived early are trimmed, and the hole is filled in place.
 *
 * Everything in this header is pure. The module and host/test_ul_pin_inject.c
 * compile the same code, so the host test proves the rule the data plane runs.
 * The rebuild in tbt_ul_pin.c is host-safe too; the kernel-only injection is
 * behind __KERNEL__.
 */
#ifndef _TBT_UL_PIN_H
#define _TBT_UL_PIN_H

#include "tbt_uapi.h"
#include "tbt_fec.h"
#include "tbt_crypto.h"

/* No carrier owns the requested slot. */
#define TBT_UL_PIN_NONE 0xffffu

/* One TCP header with a four-byte EOL option. The extra word makes the header
 * 24 bytes, so tcp_rcv_established() cannot take its header-prediction fast
 * path. That path queues in-order data without merging the out-of-order tree,
 * and merging that tree is what advances rcv_nxt past the hole. The option
 * parser stops at the EOL byte, so no option state changes. */
#define TBT_UL_PIN_TCP_HDR 24

/* Carrier index of one share. `group` is the local group index, `n`/`m` the
 * block's own split; both ends read them from the clear record header. */
static inline u32 tbt_ul_pin_carrier(u16 group, u16 share, u16 n, u16 m)
{
    return (u32)group * ((u32)n + (u32)m) + (u32)share;
}

/* The share slot a carrier index owns: index % (n+m). */
static inline u16 tbt_ul_pin_slot(u16 index, u16 n, u16 m)
{
    u16 shares = (u16)(n + m);

    return shares ? (u16)(index % shares) : 0;
}

/* A DATA carrier owns share < n; every other slot is a parity slot and the
 * only place a control or keepalive frame may ride. */
static inline bool tbt_ul_pin_is_data(u16 index, u16 n, u16 m)
{
    return tbt_ul_pin_slot(index, n, m) < n;
}

/* The first parity carrier at or after `rot`, wrapping once. Returns
 * TBT_UL_PIN_NONE when the session has no parity slot (m == 0, no carrier, or
 * fewer carriers than one group). The caller keeps the record queued. */
static inline u16 tbt_ul_pin_ctrl_next(u16 n_car, u16 n, u16 m, u16 rot)
{
    u16 i;

    if (!n_car || !m || !(u16)(n + m))
        return TBT_UL_PIN_NONE;
    for (i = 0; i < n_car; i++)
    {
        u16 idx = (u16)((rot + i) % n_car);

        if (!tbt_ul_pin_is_data(idx, n, m))
            return idx;
    }
    return TBT_UL_PIN_NONE;
}

/* Wire length of one HTTP chunk that carries a FEC record of `payload_len`
 * payload bytes: size line, record, CRLF. */
static inline u32 tbt_ul_pin_chunk_len(u16 payload_len)
{
    u32 body = (u32)TBT_FEC_HDR_LEN + (u32)payload_len;
    u32 v = body;
    u32 digits = 1;

    while (v >= 16)
    {
        v >>= 4;
        digits++;
    }
    return digits + 2 + body + 2;
}

/* Start sequence of the b-th chunk on one carrier. The arithmetic is modulo
 * 2^32 on purpose: TCP sequence numbers wrap. */
static inline u32 tbt_ul_pin_chunk_seq(u32 body_start, u64 block_id,
                                       u32 chunk_len)
{
    return body_start + (u32)(block_id * (u64)chunk_len);
}

/* True when rcv_nxt lies in [chunk_seq, chunk_seq + chunk_len). Wrap-safe:
 * the difference is the offset into the chunk, which must be < the length. */
static inline bool tbt_ul_pin_straddles(u32 rcv_nxt, u32 chunk_seq,
                                        u32 chunk_len)
{
    return chunk_len && (u32)(rcv_nxt - chunk_seq) < chunk_len;
}

/* End of the slice a replay may claim: the start of the out-of-order queue
 * when it lies inside the chunk, else the chunk's own end. The slice never
 * covers a byte the socket already has, so TCP only ever sees a gap filler. */
static inline u32 tbt_ul_pin_slice_end(u32 chunk_seq, u32 chunk_len,
                                       u32 ofo_start)
{
    u32 chunk_end = chunk_seq + chunk_len;

    return (u32)(ofo_start - chunk_seq) < chunk_len ? ofo_start : chunk_end;
}

/* Payload length of a DATA share in a block of `real` real shares. Only the
 * block's last real share can be short; every other share is S bytes. */
static inline u16 tbt_ul_pin_share_len(u16 s, u16 real, u16 share,
                                       u16 short_len)
{
    return (short_len && (u16)(share + 1) == real) ? short_len : s;
}

/* The LAST flag of a DATA record. The sender sets it on the stream's final
 * packet only, and neither parity nor the block carries it; the stream end
 * does. A replay is therefore only allowed once the end is known, and then
 * the flag is exact: the final packet has it, every earlier one does not. */
static inline bool tbt_ul_pin_flags_known(bool total_known, u64 seq,
                                          u64 total_pkts)
{
    return total_known && total_pkts && seq < total_pkts;
}

static inline u8 tbt_ul_pin_flags(u64 seq, u64 total_pkts)
{
    return seq + 1 == total_pkts ? TBT_FEC_FF_LAST : 0;
}

/* Everything the wire record needs that neither parity nor the decode state
 * carries. */
struct tbt_ul_pin_desc
{
    u64 sid;
    u64 block_id;
    u64 seq; /* block_seq0 + group*n + share */
    u16 group;
    u16 share;
    u16 n;
    u16 m;
    u16 payload_len;
    u8 flags; /* TBT_FEC_FF_LAST; tbt_ul_pin_flags() is the only source */
};

/* Rebuild one complete wire chunk exactly as the sender wrote it: chunk size
 * line, sealed 48-byte FEC header, payload, CRLF. With `crypto` set the
 * payload is XORed with the RX context's keystream, which is the sender's TX
 * keystream, so the bytes match the lost segment. Returns 0, -EINVAL on a bad
 * argument or crypto failure, -ENOSPC when cap is too small. */
int tbt_ul_pin_build(struct tbt_crypto_ctx *rx, bool crypto,
                     const struct tbt_ul_pin_desc *d, const u8 *payload,
                     u8 *out, size_t cap, u32 *out_len);

#ifdef __KERNEL__
struct tbt_carrier;
struct tbt_session;
struct tbt_fec_flow;

/* Body-stage bookkeeping on one carrier, written by its RX work only. */
void tbt_ul_pin_body_start(struct tbt_carrier *c, u32 seq);
void tbt_ul_pin_chunk_seen(struct tbt_carrier *c, u32 wire_len);
void tbt_ul_pin_body_end(struct tbt_carrier *c);

/* Replay one share the codec recovered from parity. Runs in w_fec_rx (process
 * context). It acts only when the rebuilt chunk covers this carrier's rcv_nxt
 * and the out-of-order queue proves the peer is already past the hole. */
int tbt_ul_pin_repair(struct tbt_session *sess, struct tbt_fec_flow *f,
                      u16 group, u16 share, u64 block_id, u64 seq0, u16 n,
                      u16 m, u16 real, u16 short_len, const u8 *payload);
#endif

#endif /* _TBT_UL_PIN_H */
