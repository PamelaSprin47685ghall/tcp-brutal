/* tbt_proto.h - the 32-byte frame header and its codec.
 *
 * Compiles into the module and into the host parser tests (tests/unit/test_fuzz.c),
 * so it carries no kernel state and no kernel-only type: tbt_uapi.h supplies
 * the fixed-width integers, tbt_proto.c is the only implementation.
 *
 * The wire layout is DESIGN.md §3.3; the offsets are TBT_H_* in tbt_uapi.h and
 * this struct must never disagree with them.
 */
#ifndef _TBT_PROTO_H
#define _TBT_PROTO_H

#include "tbt_uapi.h"

struct tbt_hdr
{
    u8 type, flags, path_id, path_ack_count;
    u16 path_ack_mask;
    u16 body_len; /* bytes after the header, 0..TBT_PAYLOAD_MAX */
    u64 seq, uack;
    u32 sid_lo;
    u8 sid_hi;
    u8 reserved2;
    u16 conv;
};
static inline u64 tbt_hdr_sid(const struct tbt_hdr *h)
{
    return ((u64)h->sid_hi << 32) | h->sid_lo;
}

static inline void tbt_hdr_set_sid(struct tbt_hdr *h, u64 sid)
{
    h->sid_lo = (u32)sid;
    h->sid_hi = (u8)(sid >> 32);
}


/* Little-endian wire accessors, shared by the module and the host build so
 * both run the same bytes. Shifts are correct on any host order; a compiler
 * folds them into one unaligned load on the targets this module builds for. */
static inline u16 tbt_le16(const u8 *p)
{
    return (u16)(p[0] | (u16)p[1] << 8);
}

static inline u32 tbt_le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static inline u64 tbt_le64(const u8 *p)
{
    return (u64)tbt_le32(p) | (u64)tbt_le32(p + 4) << 32;
}

static inline void tbt_put_le16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static inline void tbt_put_le32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static inline void tbt_put_le64(u8 *p, u64 v)
{
    tbt_put_le32(p, (u32)v);
    tbt_put_le32(p + 4, (u32)(v >> 32));
}

void tbt_hdr_decode(const u8 *p, struct tbt_hdr *h);
void tbt_hdr_encode(u8 *p, const struct tbt_hdr *h);
/* Validate every header field at once: conv, flags, path_id, path_ack_count,
 * path_ack_mask, body_len, reserved2, sid_hi, and the per-type body bounds.
 * Returns 0 or -EPROTO. A record that passes this is safe for any consumer. */
int tbt_hdr_check(const struct tbt_hdr *h);
/* Write one complete record (TBT_HDR_LEN + len bytes); body_len is set from
 * len, so the caller's header copy need not carry it. */
size_t tbt_raw_put(u8 *dst, const struct tbt_hdr *h, const void *payload, u16 len);
/* Header-only variant for senders that iovec the payload separately: writes
 * exactly TBT_HDR_LEN bytes (body_len is set from payload_len) and returns
 * TBT_HDR_LEN. Using tbt_raw_put() with a header-sized buffer would copy the
 * payload onto the caller's stack, which is what oopsed in QEMU. */
size_t tbt_raw_put_hdr(u8 *dst, const struct tbt_hdr *h, u16 payload_len);

/* Session-level HELLO gate: exact body length and wire version for this
 * side's data plane, then the peer role. tbt_rec_scan() has already bounded
 * the body, so this is the only place a 0.2.x/0.3.x HELLO is refused. The
 * caller passes `fec` because both data planes share one record format. */
static inline bool tbt_hello_admit(bool fec, u16 body_len, const u8 *body,
                                   u8 want_role)
{
    if (!body)
        return false;
    if (body_len != (fec ? TBT_HELLO_BODY_LEN_V6 : TBT_HELLO_BODY_LEN))
        return false;
    if (body[0] != (fec ? TBT_WIRE_VERSION_V6 : TBT_WIRE_VERSION))
        return false;
    return body[1] == want_role;
}

/* HELLO v11 per-path freshness. A PSK-proved HELLO names the epoch of the
 * near side's incarnation; the far side keeps the highest epoch it has
 * accepted on each path (`hwm_valid`, `hwm`) and the epoch of the carrier
 * still installed in that path's slot (`old_alive`, `old_epoch`).
 *
 * A same-or-older authenticated epoch is a replay: accepting it would let a
 * captured HELLO replace the live carrier, or reinstall a dead one under an
 * epoch the session has already left behind. A live slot therefore yields
 * only to a strictly newer epoch, and an empty or dead slot only to an epoch
 * above the path high-water. Non-PSK HELLOs carry no authenticated epoch
 * and never reach this predicate; they keep the path-tag/TOFU rules.
 *
 * Pure and host-testable: tests/unit/test_wire_compat.c pins the policy. */
static inline bool tbt_hello_epoch_admit(bool hwm_valid, u64 hwm,
                                         bool old_alive, u64 old_epoch,
                                         u64 epoch)
{
    if (hwm_valid && epoch <= hwm)
        return false;
    if (old_alive && epoch <= old_epoch)
        return false;
    return true;
}

#endif /* _TBT_PROTO_H */
