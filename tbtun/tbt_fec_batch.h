/* tbt_fec_batch.h - the wire layout of one FEC batch write.
 *
 * Pure code: no allocation, no kernel call, no mutable state. The module and
 * host/test_batch.c compile the same functions, so the byte layout the host
 * test pins is the byte layout the module sends.
 *
 * A batch is 1..TBT_BATCH_REC_MAX FEC records committed with one socket
 * write. tbt_fec_batch_build() turns the records into a segment list:
 *
 *   raw profile:  record_0, record_1, ...
 *   HTTP profile: ["<hex len>\r\n", record_0, "\r\n"], ...
 *
 * The payload segments point into the caller's record buffers, so building
 * the list copies no record byte. Only the 4..6-byte chunk size line is
 * written into the caller's scratch. The module maps the list to struct kvec;
 * a host test can hash or concatenate it without a socket.
 *
 * The second function handles the one case that needs a copy:
 * tcp_sendmsg_locked() may accept a prefix and return a short count. The
 * unwritten tail must survive until the next writer flushes it, and
 * tbt_fec_batch_tail() copies exactly that tail into the carrier's part
 * buffer. That buffer is TBT_CAR_PART_MAX bytes, one worst-case batch, so a
 * staged tail always fits when the caller checked the total first.
 */
#ifndef _TBT_FEC_BATCH_H
#define _TBT_FEC_BATCH_H

#include "tbt_http.h"

/* The module's batch bound. tbt_carrier.c refuses a count above this, so the
 * segment array can live on the stack. */
#define TBT_BATCH_REC_MAX 3
/* Worst case, one HTTP record: size line + record + CRLF. A raw batch needs
 * one segment per record. */
#define TBT_BATCH_SEG_MAX (TBT_BATCH_REC_MAX * 3)

/* One byte range of the wire batch. base is never NULL and len is non-zero
 * for every segment a successful build returns. */
struct tbt_fec_batch_seg
{
    const u8 *base;
    u32 len;
};

/* Build the segment list for recs[0..count) in *seg and return the number of
 * segments, or 0 on invalid input.
 *
 * hdr_scratch must hold TBT_BATCH_REC_MAX * TBT_HTTP_CHUNK_HDR_MAX bytes; the
 * size line of record i is written at hdr_scratch + i * TBT_HTTP_CHUNK_HDR_MAX.
 *
 * http selects the container: on the HTTP profile each record is wrapped in
 * its own chunk (size line, record, CRLF); on the raw profile the record is
 * the wire bytes. A record must be non-empty and no larger than
 * TBT_FEC_REC_MAX; the HTTP profile additionally requires the FEC header
 * (TBT_FEC_HDR_LEN bytes), exactly what tbt_http_fec_chunk_encode() accepted.
 *
 * On success *total is the concatenated byte length. On failure it is 0.
 * The caller must still check *total against its own buffer bounds. */
unsigned int tbt_fec_batch_build(struct tbt_fec_batch_seg *seg, char *hdr_scratch,
                                 const u8 *const *recs, const u16 *lens, u16 count,
                                 bool http, size_t *total);

/* Stage the tail of a batch that a socket short-wrote.
 *
 * The first `done` bytes of the concatenated segment stream reached the
 * socket; the remaining total - done bytes are copied to dst in wire order.
 * dst is the carrier's part buffer and cap is its size.
 *
 * Returns true and sets *tail when the entire tail fits. Returns false, and
 * writes nothing, when done is past total, the segments do not add up to
 * total, or the tail is larger than cap. */
bool tbt_fec_batch_tail(const struct tbt_fec_batch_seg *seg, unsigned int nseg,
                        size_t total, size_t done, u8 *dst, size_t cap, u32 *tail);

#endif /* _TBT_FEC_BATCH_H */
