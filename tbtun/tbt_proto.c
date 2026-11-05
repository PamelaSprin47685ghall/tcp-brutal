/* tbt_proto.c - frame encoding/decoding and the raw container.
 *
 * Compiles into the module and into the host parser tests (host/test_fuzz.c),
 * so it holds no state and uses no kernel facility a plain C program lacks.
 *
 * Owns:
 *   - the 32-byte header exactly as DESIGN.md §3.3 lays it out (offsets are
 *     the authority; this file must never disagree with it).
 *   - the `raw` container for C2: `u32 len (LE) | header | payload`, which is
 *     also the body of an http-kal chunk when the QoS layer lands in C8.
 *   - header validation strict enough that a corrupt or hostile stream is
 *     rejected per carrier instead of being parsed into a wrong action.
 *
 * Must never:
 *   - trust a length field. Every decode checks the advertised length against
 *     the bytes actually buffered and against TBT_PAYLOAD_MAX.
 *   - read or write past `TBT_HDR_LEN` + payload; all offsets come from the
 *     TBT_H_* constants below, never from arithmetic on a wire field.
 *   - carry per-session state; this file is stateless.
 */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#include "tbt_proto.h"

void tbt_hdr_decode(const u8 *p, struct tbt_hdr *h)
{
    h->type = p[TBT_H_TYPE];
    h->flags = p[TBT_H_FLAGS];
    h->path_id = p[TBT_H_PATH];
    h->path_ack_count = p[TBT_H_ACKC];
    h->path_ack_mask = tbt_le16(p + TBT_H_ACKM);
    h->body_len = tbt_le16(p + TBT_H_BODYLEN);
    h->seq = tbt_le64(p + TBT_H_SEQ);
    h->uack = tbt_le64(p + TBT_H_UACK);
    h->sid_lo = tbt_le32(p + TBT_H_SIDLO);
    h->sid_hi = p[TBT_H_SIDHI];
    h->reserved2 = p[TBT_H_RES2];
    h->conv = tbt_le16(p + TBT_H_CONV);
}

void tbt_hdr_encode(u8 *p, const struct tbt_hdr *h)
{
    memset(p, 0, TBT_HDR_LEN); // reserved bytes stay zero on the wire
    p[TBT_H_TYPE] = h->type;
    p[TBT_H_FLAGS] = h->flags;
    p[TBT_H_PATH] = h->path_id;
    p[TBT_H_ACKC] = h->path_ack_count;
    tbt_put_le16(p + TBT_H_ACKM, h->path_ack_mask);
    tbt_put_le16(p + TBT_H_BODYLEN, h->body_len);
    tbt_put_le64(p + TBT_H_SEQ, h->seq);
    tbt_put_le64(p + TBT_H_UACK, h->uack);
    tbt_put_le32(p + TBT_H_SIDLO, h->sid_lo);
    p[TBT_H_SIDHI] = h->sid_hi;
    p[TBT_H_RES2] = h->reserved2;
    tbt_put_le16(p + TBT_H_CONV, h->conv);
}

// Validate every field of the header, once, for every type: conv, flags,
// path_id, path_ack_count, path_ack_mask, body_len, sid_hi, reserved2, and the
// per-type body bounds. Two types also have an exact length that depends on
// another header field:
//   - PREDICT carries the 16-byte resume token iff MUST_ORDER is set,
//   - FBK carries exactly path_ack_count 8-byte entries.
// Returns 0 or -EPROTO. Body *contents* are not interpreted here: OPEN and
// HELLO carry fields their consumer reads later, and that consumer validates
// them. This function makes the framing total, not the payloads.
int tbt_hdr_check(const struct tbt_hdr *h)
{
    u16 min, max;

    if (h->conv != TBT_CONV)
        return -EPROTO;
    if (h->flags & ~TBT_FF_MASK)
        return -EPROTO;
    if (h->path_id >= TBT_MAX_CARRIERS_LIMIT)
        return -EPROTO;
    if (h->path_ack_count > TBT_MAX_CARRIERS_LIMIT)
        return -EPROTO;
    if (h->path_ack_mask & ~((1u << TBT_MAX_CARRIERS_LIMIT) - 1))
        return -EPROTO;
    if (h->body_len > TBT_PAYLOAD_MAX)
        return -EPROTO;
    /* v4 uses the full 40-bit sid; sid_hi is meaningful. */
    if (h->reserved2 != 0)
        return -EPROTO;
    if (!tbt_body_bounds(h->type, &min, &max))
        return -EPROTO;
    if (h->body_len < min || h->body_len > max)
        return -EPROTO;

    switch (h->type)
    {
    case TBT_F_PREDICT:
        return h->body_len == ((h->flags & TBT_FF_MUST_ORDER) ? TBT_PREDICT_TOKEN_LEN : 0) ? 0
                                                                                           : -EPROTO;
    case TBT_F_FBK:
        return h->body_len == (u16)(h->path_ack_count * TBT_FBK_ENTRY_LEN) ? 0 : -EPROTO;
    default:
        return 0;
    }
}

size_t tbt_raw_put_hdr(u8 *dst, const struct tbt_hdr *h, u16 payload_len)
{
    struct tbt_hdr hh = *h;

    hh.body_len = payload_len;
    tbt_hdr_encode(dst, &hh);
    return TBT_HDR_LEN;
}

// Write one complete record into `dst` (at least TBT_HDR_LEN + len bytes; the
// caller owns that much space). Returns the number of bytes written.
size_t tbt_raw_put(u8 *dst, const struct tbt_hdr *h, const void *payload, u16 len)
{
    struct tbt_hdr hh = *h;

    hh.body_len = len;
    tbt_hdr_encode(dst, &hh);
    if (len)
        memcpy(dst + TBT_HDR_LEN, payload, len);
    return TBT_HDR_LEN + len;
}
