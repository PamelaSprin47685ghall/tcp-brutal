/* tbt_rec.h - one record, located and validated without state.
 *
 * Wire form (DESIGN.md §3.3):
 *
 *   32-byte frame header | body_len bytes of body
 *
 * body_len is the u16 at header offset +6. There is no length prefix in front
 * of the header: the header is the first byte of the record on every profile.
 *
 * Used by the kernel (tbt_rx.c, over its per-carrier buffer) and by
 * host/test_fuzz.c, so the host test exercises the same code the module runs.
 * tbt_rec_scan() allocates nothing, keeps no state, never reads a byte past
 * `len`, and never trusts a peer length: the header is validated in full
 * before body_len is used to wait for a body.
 *
 * Validation is total. Any record that tbt_rec_scan() reports as TBT_REC_OK
 * has conv, flags, path_id, path_ack_count, path_ack_mask, body_len, sid_hi,
 * reserved2 and the per-type body bounds already in range, so every consumer
 * may act on it without re-checking the header. Body contents are the
 * exception: OPEN and HELLO payload fields are validated by whoever reads
 * them.
 */
#ifndef _TBT_REC_H
#define _TBT_REC_H

#include "tbt_proto.h"

struct tbt_rec
{
    struct tbt_hdr hdr; /* decoded header at buf + 0 */
    const u8 *body;     /* body_len bytes at buf + TBT_HDR_LEN */
    u16 body_len;
};

enum tbt_rec_status
{
    TBT_REC_OK = 0,
    TBT_REC_NEED_MORE,
    TBT_REC_PROTO,
};

/* Locate and validate the first record in buf[0..len).
 *
 * Returns the bytes it occupies (TBT_HDR_LEN + body_len) and sets *err to
 * TBT_REC_OK, filling *out. Returns 0 and sets *err to TBT_REC_NEED_MORE when
 * len does not yet hold the header or the whole body, or to TBT_REC_PROTO when
 * the header is invalid; *out is written only on TBT_REC_OK. */
size_t tbt_rec_scan(const u8 *buf, size_t len, struct tbt_rec *out, int *err);

#endif /* _TBT_REC_H */
