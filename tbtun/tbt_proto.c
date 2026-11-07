/* tbt_proto.c - frame encoding/decoding and the raw container.
 *
 * Compiles into the module and into the host parser tests (host/test_fuzz.c),
 * so it holds no state and uses no kernel facility a plain C program lacks.
 *
 * Owns:
 *   - the 32-byte header exactly as DESIGN.md §3.3 lays it out (offsets are
 *     the authority; this file must never disagree with it).
 *   - the `raw` container for C2: `header | payload`, one record with
 *     `body_len` at header offset +6 and no separate length prefix; the same
 *     record is the body of an http-kal chunk.
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

/* 0.2.0 wire target (TBT_CONV_NEXT 0x0006, TBT_FEC_CONV_NEXT 0x1002),
 * recorded here as a comment; the skeleton does not implement it.
 *
 * FIN body, exactly TBT_FIN_BODY_LEN (16) bytes, little-endian:
 *   +0  u64 pkt_end   absolute session packet seq after this stream's last
 *                     DATA packet
 *   +8  u64 pkt_base  absolute session packet seq of this stream's first DATA
 *                     packet, aligned to the super-block (groups*n)
 * tbt_body_bounds() then sets TBT_F_FIN to *min = *max = TBT_FIN_BODY_LEN.
 * A non-FEC session sends the same 16 zero bytes (pkt_end = pkt_base = 0),
 * so one length rule covers both. On receive, pkt_end < pkt_base is
 * fec_fin_bad++ plus -EPROTO; pkt_base % (groups*n) != 0 is fec_fin_bad++
 * plus ignore.
 *
 * PARITY marker: no 4-byte marker and no magic number exists. A marker is a
 * set of values inside the 40-byte tbt_fec_hdr: type TBT_FEC_PARITY (0x31),
 * flags 0, share_index n (parity 0), epoch 0, win_len 0, payload_len 0,
 * conv 0x1002, payload_crc32c 0 (CRC32C of an empty payload). The current
 * tbt_fec.c requires PARITY payload_len == max_payload and win_len != 0;
 * relaxing both is the core change of TBT_FEC_CONV 0x1002.
 *
 * 0.1.0 <-> 0.2.0 fail closed at this function: the conv check and the body
 * bounds below reject the other revision on the first record, so neither side
 * parses the other's bytes under the wrong layout. */

// Validate every field of the header, once, for every type: conv, flags,
// path_id, path_ack_count, path_ack_mask, body_len, sid_hi, reserved2, and the
// per-type body bounds. One type also has an exact length that depends on
// another header field: FBK carries exactly path_ack_count 18-byte entries.
//
// The old ARQ record names carriers with a u8 path_id, a u16 mask and a u8
// count, and its FBK/WINDOW bodies have 16 entries. The carrier layer grows to
// 256 slots, but this wire format stays frozen at TBT_LEGACY_MAX_CARRIERS: a
// session that needs more carriers uses the FEC record, not a wider old one.
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
    /* `path_id` is a carrier index and every u8 value fits the carrier layer's
     * 256 slots, so it needs no range check: a FEC v6 session with carriers=256
     * sends its HELLO/OPEN/FIN/RST with path_id 16..255. The 16-carrier limit
     * belongs to the old ARQ's per-path ack fields only (`path_ack_mask` is a
     * u16), so it stays on those. Capping path_id at 16 made every carrier from
     * 16 up fail its own HELLO encoding: the sender counted http_parse_err and
     * the carrier never came up (measured: 256 carriers, up=16,
     * http_parse_err=240). */
    if (h->path_ack_count > TBT_LEGACY_MAX_CARRIERS)
        return -EPROTO;
    if (h->path_ack_mask & ~((1u << TBT_LEGACY_MAX_CARRIERS) - 1))
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
