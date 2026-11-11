/* tbt_rec.c - the pure record framing, shared by the module and the host test.
 *
 * One job: turn "some bytes" into "at most one complete, validated record", or
 * say why it cannot. The caller owns the buffer and the policy (tbt_rx.c keeps
 * the per-carrier partial state and decides what a frame means).
 *
 * Never allocates, never reads past len, never trusts a peer length: the
 * 32-byte header is decoded and fully checked first, and only then is body_len
 * used to decide whether the body has arrived.
 */
#include "tbt_rec.h"

size_t tbt_rec_scan(const u8 *buf, size_t len, struct tbt_rec *out, int *err)
{
    struct tbt_hdr hdr;
    size_t need;

    if (len < TBT_HDR_LEN)
    {
        *err = TBT_REC_NEED_MORE;
        return 0;
    }

    tbt_hdr_decode(buf, &hdr);
    if (tbt_hdr_check(&hdr))
    {
        *err = TBT_REC_PROTO;
        return 0;
    }

    need = (size_t)TBT_HDR_LEN + hdr.body_len;
    if (len < need)
    {
        *err = TBT_REC_NEED_MORE;
        return 0;
    }

    out->hdr = hdr;
    out->body = buf + TBT_HDR_LEN;
    out->body_len = hdr.body_len;
    *err = TBT_REC_OK;
    return need;
}
