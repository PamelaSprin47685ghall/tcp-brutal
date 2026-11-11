/* tbt_rcvq_wire.c - rebuild one FEC share's exact sender wire bytes.
 *
 * The sender's tbt_fec_make_record() seals the clear header, copies the
 * payload, then encrypts DATA/PARITY bodies in place; the carrier's profile
 * encoder wraps the record in one HTTP chunk, or sends it bare.  A repaired
 * share must reproduce those bytes exactly: the TCP sequence it fills names
 * them, so a different chunk framing or a different ciphertext would inject
 * bytes the peer never wrote.
 *
 * This unit performs the same steps on the recovered plaintext and writes
 * straight into the caller's buffer.  It never allocates and never needs a
 * full-record staging buffer.
 */
#include "tbt_rcvq.h"
#include "tbt_http.h"

/* The sender transforms only these bodies; the clear header and control
 * records stay untouched. */
static bool tbt_rcvq_body_type(u8 type)
{
    return type == TBT_FEC_DATA || type == TBT_FEC_PARITY;
}

int tbt_rcvq_rebuild_wire(const struct tbt_fec_hdr *h, const u8 *plain,
                          struct tbt_crypto_ctx *rx_crypto, u8 profile,
                          u8 *out, size_t cap, u16 *out_len)
{
    struct tbt_fec_hdr hh;
    u16 len;
    u16 rec_len;
    size_t hlen = 0;
    size_t need;

    if (!h || !out || !out_len)
        return -EINVAL;
    len = h->payload_len;
    if (len > TBT_FEC_S)
        return -EINVAL;
    if (len && !plain)
        return -EINVAL;
    /* 1 and 2 are the two HTTP profiles; 3 is the bare record. */
    if (profile != TBT_RCVQ_PROFILE_HTTP && profile != 2u &&
        profile != TBT_RCVQ_PROFILE_RAW)
        return -EINVAL;

    hh = *h;
    if (tbt_fec_hdr_seal(&hh, plain))
        return -EINVAL;
    rec_len = (u16)(TBT_FEC_HDR_LEN + len);

    if (profile == TBT_RCVQ_PROFILE_RAW)
    {
        need = rec_len;
        if (cap < need)
            return -ENOSPC;
        if (tbt_fec_hdr_pack(&hh, out))
            return -EINVAL;
    }
    else
    {
        /* The chunk size line precedes the record; its length depends on the
         * record length, so it is emitted before the capacity check for the
         * whole frame.  A zero return means the size line itself does not
         * fit (rec_len is already bounded by TBT_FEC_REC_MAX). */
        hlen = tbt_http_chunk_hdr((char *)out, cap, rec_len);
        if (!hlen)
            return -ENOSPC;
        need = hlen + rec_len + 2;
        if (need > cap)
            return -ENOSPC;
        if (tbt_fec_hdr_pack(&hh, out + hlen))
            return -EINVAL;
    }

    if (len)
        memcpy(out + hlen + TBT_FEC_HDR_LEN, plain, len);

    /* TX seals the clear payload CRC, then transforms the ciphertext.  The
     * receiver reverses that with its own direction's context. */
    if (rx_crypto && tbt_rcvq_body_type(hh.type) &&
        tbt_crypto_body_xor_flow(rx_crypto, hh.sid, hh.seq,
                                 hh.type == TBT_FEC_PARITY, hh.share_index,
                                 out + hlen + TBT_FEC_HDR_LEN, len))
        return -EINVAL;

    if (profile != TBT_RCVQ_PROFILE_RAW)
    {
        out[hlen + rec_len] = '\r';
        out[hlen + rec_len + 1] = '\n';
    }
    *out_len = (u16)need;
    return 0;
}
