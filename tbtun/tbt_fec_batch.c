/* tbt_fec_batch.c - pure wire layout for one FEC batch write.
 *
 * See tbt_fec_batch.h for the contract. This file owns no socket, no buffer
 * and no policy: it turns records into byte ranges and stages a short-write
 * tail. tbt_carrier.c owns when to send and what a send error means.
 */
#include "tbt_fec_batch.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static const u8 tbt_fec_batch_crlf[2] = {'\r', '\n'};

unsigned int tbt_fec_batch_build(struct tbt_fec_batch_seg *seg, char *hdr_scratch,
                                 const u8 *const *recs, const u16 *lens, u16 count,
                                 bool http, size_t *total)
{
    size_t sum = 0;
    unsigned int nseg = 0;
    u16 i;

    if (!seg || !hdr_scratch || !recs || !lens || !total || !count ||
        count > TBT_BATCH_REC_MAX)
        goto bad;
    for (i = 0; i < count; i++)
    {
        const u8 *rec = recs[i];
        size_t len = lens[i];
        char *hdr = hdr_scratch + (size_t)i * TBT_HTTP_CHUNK_HDR_MAX;
        size_t hlen;

        if (!rec || !len || len > TBT_FEC_REC_MAX)
            goto bad;
        if (!http)
        {
            seg[nseg].base = rec;
            seg[nseg].len = (u32)len;
            nseg++;
            sum += len;
            continue;
        }
        if (len < TBT_FEC_HDR_LEN)
            goto bad;
        hlen = tbt_http_chunk_hdr(hdr, TBT_HTTP_CHUNK_HDR_MAX, len);
        if (!hlen)
            goto bad;
        seg[nseg].base = (const u8 *)hdr;
        seg[nseg].len = (u32)hlen;
        nseg++;
        seg[nseg].base = rec;
        seg[nseg].len = (u32)len;
        nseg++;
        seg[nseg].base = tbt_fec_batch_crlf;
        seg[nseg].len = sizeof(tbt_fec_batch_crlf);
        nseg++;
        sum += hlen + len + sizeof(tbt_fec_batch_crlf);
    }
    *total = sum;
    return nseg;

bad:
    if (total)
        *total = 0;
    return 0;
}

bool tbt_fec_batch_tail(const struct tbt_fec_batch_seg *seg, unsigned int nseg,
                        size_t total, size_t done, u8 *dst, size_t cap, u32 *tail)
{
    size_t sum = 0, skip, copied = 0;
    unsigned int i;

    if (!seg || !dst || !tail || done > total || total - done > cap)
        return false;
    for (i = 0; i < nseg; i++)
    {
        if (seg[i].len > total - sum)
            return false;
        sum += seg[i].len;
    }
    if (sum != total)
        return false;

    skip = done;
    for (i = 0; i < nseg; i++)
    {
        size_t len = seg[i].len;

        if (skip >= len)
        {
            skip -= len;
            continue;
        }
        memcpy(dst + copied, seg[i].base + skip, len - skip);
        copied += len - skip;
        skip = 0;
    }
    if (copied != total - done)
        return false;
    *tail = (u32)copied;
    return true;
}
