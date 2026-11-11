/* tbt_mux.h - tbtun 0.8.0 single-FEC-stream multiplexing.
 *
 * One FEC stream carries many short logical streams. Every subframe is an
 * 8-byte header followed by at most 1024 payload bytes. The module and a host
 * program compile this header and tbt_mux.c unchanged: the encoder is a pure
 * function, the parser never allocates, and the flow table is caller-owned.
 *
 * Wire layout, network byte order:
 *
 *     +0      cmd
 *     +1      slot
 *     +2..3   len        big-endian u16
 *     +4..7   stream_id  big-endian u32
 *
 * Commands:
 *
 *   OPEN (1)  len is 0 and slot is the near-side mapping slot (0..7).
 *             stream_id names the new stream. No payload.
 *   DATA (2)  len is 1..1024 payload bytes and slot is 0.
 *   FIN  (3)  len is 0, slot is 0. No payload.
 *   RST  (4)  len is 0, slot is 0. No payload.
 *   WIN  (5)  len is the credit granted to the peer (0..65535), slot is 0.
 *             No payload; the parser never treats this len as a body size.
 *
 * tbt_mux_hdr_pack() and tbt_mux_hdr_unpack() own the byte order: the struct
 * fields hold host-order values, and the helpers convert to and from the
 * big-endian wire order. The __be types mark the wire-width fields; the
 * conversion stays inside these two functions.
 *
 * Flow control is separate from framing. A receiver charges every DATA byte
 * to rx_unacked. When that sum exceeds half of rx_window, it grants back as
 * much as one WIN can carry (at most TBT_MUX_WINDOW_MAX) and subtracts that
 * from rx_unacked. A sum wider than the u16 credit field drains across
 * successive charges, so no credited byte is lost; smaller sums wait for the
 * next DATA frame. A sender may send a DATA frame only while tx_credit covers
 * its length; WIN adds credit and saturates at 0xffffffff.
 *
 * Stream states:
 *
 *   FREE    slot is unused and stream_id is not valid.
 *   OPEN    the stream is usable in both directions.
 *   TX_FIN  this end sent FIN; reserved for the session layer.
 *   RX_FIN  the peer sent FIN; reserved for the session layer.
 *   RST     the stream was reset; reserved for the session layer.
 */
#ifndef _TBT_MUX_H
#define _TBT_MUX_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint16_t __be16;
typedef uint32_t __be32;
#endif

/* Fixed on-wire header: cmd, slot, big-endian len, big-endian stream_id. */
#define TBT_MUX_HDR_LEN 8
/* Largest DATA body one subframe may carry. */
#define TBT_MUX_PAYLOAD_MAX 1024
/* Largest subframe on the wire, header included. */
#define TBT_MUX_FRAME_MAX (TBT_MUX_HDR_LEN + TBT_MUX_PAYLOAD_MAX)
/* Concurrent logical streams in one flow table. */
#define TBT_MUX_STREAMS 64
/* Largest credit a WIN command can express. */
#define TBT_MUX_WINDOW_MAX 65535
/* Highest near-side mapping slot an OPEN may name. */
#define TBT_MUX_SLOT_MAX 7

/* Command values as they appear at wire byte +0. */
#define TBT_MUX_OPEN 1
#define TBT_MUX_DATA 2
#define TBT_MUX_FIN 3
#define TBT_MUX_RST 4
#define TBT_MUX_WIN 5

/* Stream states for struct tbt_mux_stream.state. The values are shared with
 * the commands where the numbers coincide: OPEN is 1, TX_FIN is 2, RX_FIN is
 * 3, RST is 4. */
#define TBT_MUX_FREE 0
#define TBT_MUX_TX_FIN 2
#define TBT_MUX_RX_FIN 3

/* The logical subframe header. Fields hold host-order values. */
struct tbt_mux_hdr {
    u8 cmd;
    u8 slot;
    __be16 len;
    __be32 stream_id;
};

/* Serialize h into out[0..7] in wire order. Returns 0 or -EINVAL. */
int tbt_mux_hdr_pack(const struct tbt_mux_hdr *h, u8 out[TBT_MUX_HDR_LEN]);

/* Parse in[0..7] into h in host order. Returns 0 or -EINVAL. */
int tbt_mux_hdr_unpack(const u8 in[TBT_MUX_HDR_LEN], struct tbt_mux_hdr *h);

/* Build one complete subframe. DATA carries len payload bytes; every other
 * command has no payload and len is 0 except for WIN, where len is credit.
 * Returns the number of bytes written, -EINVAL for a bad command or length,
 * or -ENOSPC when cap cannot hold the frame. */
int tbt_mux_encode(u8 cmd, u8 slot, u32 stream_id, const u8 *payload, u16 len,
                   u8 *out, size_t cap);

/* Called once per complete subframe. payload is NULL for every command with
 * no body and points at body_cap bytes of caller storage for DATA. A non-zero
 * return stops the parser and becomes the parser's *err. */
typedef int (*tbt_mux_frame_cb)(void *ctx, const struct tbt_mux_hdr *h,
                                const u8 *payload);

/* Streaming parser state. hdr holds the partial or complete 8-byte header;
 * body holds a partial DATA body between pushes. The caller owns body and
 * body_cap, and the parser never allocates. */
struct tbt_mux_parser {
    u8 hdr[TBT_MUX_HDR_LEN];
    u8 hdr_have;
    u16 body_len;
    u16 body_have;
    u8 *body;
    u16 body_cap;
};

/* Reset p to the start of a frame. body may be NULL when body_cap is 0. */
void tbt_mux_parser_init(struct tbt_mux_parser *p, u8 *body, u16 cap);

/* Consume as much of buf as forms complete frames. Returns the number of
 * bytes consumed. *err starts at 0 and is set to -EPROTO on a malformed
 * header or oversized DATA body, or to the callback's non-zero return.
 * Parsing stops at the first error; the unconsumed bytes stay with the
 * caller. */
size_t tbt_mux_parser_push(struct tbt_mux_parser *p, const u8 *buf, size_t len,
                           tbt_mux_frame_cb cb, void *ctx, int *err);

/* One logical stream and its credit state. */
struct tbt_mux_stream {
    u32 stream_id;
    u8 state;
    u8 slot;
    u32 tx_credit;
    u32 rx_unacked;
};

/* A fixed table of TBT_MUX_STREAMS streams plus the receive window shared by
 * every stream in this flow. rx_window is the credit this end is willing to
 * have outstanding. */
struct tbt_mux_flows {
    struct tbt_mux_stream s[TBT_MUX_STREAMS];
    u16 count;
    u32 rx_window;
};

/* Clear the table. A zero rx_window selects TBT_MUX_WINDOW_MAX. */
void tbt_mux_flows_init(struct tbt_mux_flows *f, u32 rx_window);

/* Install a new stream. Returns NULL for a duplicate id, a bad slot, or a
 * full table. The new stream starts with no transmit credit. */
struct tbt_mux_stream *tbt_mux_flow_open(struct tbt_mux_flows *f, u32 id,
                                         u8 slot);

/* Find an installed stream, or NULL. */
struct tbt_mux_stream *tbt_mux_flow_find(struct tbt_mux_flows *f, u32 id);

/* Release a stream. Returns 0, or -ENOENT when the id is not installed. */
int tbt_mux_flow_close(struct tbt_mux_flows *f, u32 id);

/* Charge len DATA bytes against the stream's transmit credit. Returns 0,
 * -ENOENT for an unknown id, or -EAGAIN when credit is short. */
int tbt_mux_tx_charge(struct tbt_mux_flows *f, u32 id, u16 len);

/* Add credit granted by a WIN. The sum saturates at 0xffffffff. Returns 0 or
 * -ENOENT. */
int tbt_mux_tx_credit(struct tbt_mux_flows *f, u32 id, u16 credit);

/* Charge len received DATA bytes. When rx_unacked exceeds rx_window / 2,
 * store one WIN of credit in *grant, subtract it from rx_unacked, and return
 * 1. A sum above TBT_MUX_WINDOW_MAX drains over successive calls instead of
 * discarding the remainder. Otherwise store 0 in *grant and return 0.
 * Returns -ENOENT for an unknown id and -EINVAL when grant is NULL. */
int tbt_mux_rx_charge(struct tbt_mux_flows *f, u32 id, u16 len, u16 *grant);

#endif /* _TBT_MUX_H */
