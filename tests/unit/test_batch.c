/* test_batch.c - host unit test for the FEC batch wire layout.
 *
 * Links src/kernel/tbt_fec_batch.c, the same object the module builds, and pins
 * the byte layout of one batch write: segment count, segment bases, totals,
 * HTTP size lines, and the short-write tail copy. No socket, no kernel
 * headers, no fixtures.
 *
 * The carrier policy checks simulate tcp_sendmsg_locked() returns without a
 * socket: a send accepts an arbitrary prefix (or nothing on -EAGAIN), the
 * caller stages the unwritten tail in its part buffer, and the next batch
 * must wait until that part is empty. The byte stream built here is the
 * byte stream the far side must reconstruct.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_fec_batch.h"

/* Worst case HTTP batch: every record one maximal chunk. */
#define BATCH_BUF_MAX (TBT_BATCH_REC_MAX * TBT_HTTP_FEC_FRAME_MAX)
/* Two consecutive batches, for the ordering test. */
#define STREAM_BUF_MAX (2 * BATCH_BUF_MAX)

static int checks;
static int failures;

static u8 rec_store[TBT_BATCH_REC_MAX][TBT_FEC_REC_MAX];
static u8 buf_concat[BATCH_BUF_MAX];
static u8 buf_expect[BATCH_BUF_MAX];
static u8 buf_stream[STREAM_BUF_MAX];
static u8 buf_part[BATCH_BUF_MAX];
static u8 buf_a[BATCH_BUF_MAX];
static u8 buf_b[BATCH_BUF_MAX];
static u8 buf_dst[BATCH_BUF_MAX];

static void ok(int cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
}

static int all_bytes(const u8 *buf, size_t len, u8 value)
{
    size_t i;

    for (i = 0; i < len; i++)
        if (buf[i] != value)
            return 0;
    return 1;
}

/* Deterministic record content; tag separates batches and lengths. */
static void fill_rec(int index, u16 len, u8 tag)
{
    u16 i;

    for (i = 0; i < len; i++)
        rec_store[index][i] = (u8)(tag + index * 37 + i * 7 + (i >> 8));
}

static size_t concat_segs(const struct tbt_fec_batch_seg *seg, unsigned int nseg, u8 *out,
                          size_t cap)
{
    size_t off = 0;
    unsigned int i;

    for (i = 0; i < nseg; i++)
    {
        if (seg[i].len > cap - off)
            return 0;
        memcpy(out + off, seg[i].base, seg[i].len);
        off += seg[i].len;
    }
    return off;
}

/* Independent check of a chunk size line: lowercase hex, no leading zeros,
 * CRLF. Parses the bytes directly, without tbt_http.c. */
static void check_size_line(const char *line, size_t n, size_t body_len, const char *what,
                            int *digits_out)
{
    size_t d = 0, i, value = 0, ndig = 0;
    size_t v = body_len;
    int lowercase = 1;

    do
    {
        d++;
        v >>= 4;
    } while (v);
    if (digits_out)
        *digits_out = (int)d;

    ok(n == d + 2, what, "size line byte count");
    ok(n >= 2 && line[n - 2] == '\r' && line[n - 1] == '\n', what, "size line CRLF");
    for (i = 0; n >= 2 && i < n - 2; i++)
    {
        unsigned char c = (unsigned char)line[i];
        size_t digit;

        if (c >= '0' && c <= '9')
            digit = (size_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = (size_t)(c - 'a' + 10);
        else
        {
            lowercase = 0;
            digit = 0;
        }
        value = value * 16 + digit;
        ndig++;
    }
    ok(lowercase, what, "lowercase hex digits only");
    ok(ndig == d, what, "hex digit count at boundary");
    ok(value == body_len, what, "size line value equals record length");
    ok(ndig < 2 || line[0] != '0', what, "no leading zero");
}

/* Fill count records of one length, build, and flatten the segments. */
static unsigned int build_batch(bool http, unsigned int count, u16 len, u8 tag,
                                struct tbt_fec_batch_seg *seg, char *scratch, u8 *out,
                                size_t *total)
{
    const u8 *recs[TBT_BATCH_REC_MAX];
    u16 lens[TBT_BATCH_REC_MAX];
    unsigned int nseg, i;

    *total = 0;
    for (i = 0; i < count && i < TBT_BATCH_REC_MAX; i++)
    {
        fill_rec((int)i, len, tag);
        recs[i] = rec_store[i];
        lens[i] = len;
    }
    nseg = tbt_fec_batch_build(seg, scratch, recs, lens, (u16)count, http, total);
    if (!nseg || !*total)
        return 0;
    if (concat_segs(seg, nseg, out, BATCH_BUF_MAX) != *total)
        return 0;
    return nseg;
}

static void check_reject(const char *what, struct tbt_fec_batch_seg *seg, char *scratch,
                         const u8 *const *recs, const u16 *lens, u16 count, bool http,
                         size_t *total)
{
    unsigned int nseg;

    if (total)
        *total = 12345;
    nseg = tbt_fec_batch_build(seg, scratch, recs, lens, count, http, total);
    ok(nseg == 0, what, "must reject");
    if (total)
        ok(*total == 0, what, "total zeroed on reject");
}

static void test_raw_build(void)
{
    static const u16 lens[TBT_BATCH_REC_MAX] = {1, TBT_FEC_HDR_LEN, TBT_FEC_REC_MAX};
    struct tbt_fec_batch_seg seg[TBT_BATCH_SEG_MAX];
    char scratch[TBT_BATCH_REC_MAX * TBT_HTTP_CHUNK_HDR_MAX];
    int count, i;

    for (count = 1; count <= TBT_BATCH_REC_MAX; count++)
    {
        const u8 *recs[TBT_BATCH_REC_MAX];
        u16 use[TBT_BATCH_REC_MAX];
        size_t total = 0, want = 0, got;
        unsigned int nseg;
        char what[96];

        for (i = 0; i < count; i++)
        {
            use[i] = lens[i];
            fill_rec(i, use[i], 0x10);
            recs[i] = rec_store[i];
            memcpy(buf_expect + want, rec_store[i], use[i]);
            want += use[i];
        }
        nseg = tbt_fec_batch_build(seg, scratch, recs, use, (u16)count, false, &total);

        snprintf(what, sizeof what, "raw count=%d segment count", count);
        ok(nseg == (unsigned int)count, what, NULL);
        snprintf(what, sizeof what, "raw count=%d total", count);
        ok(total == want, what, NULL);
        if (nseg != (unsigned int)count)
            continue;
        for (i = 0; i < count; i++)
        {
            snprintf(what, sizeof what, "raw count=%d record=%d", count, i);
            ok(seg[i].base == rec_store[i], what, "base points at record");
            ok(seg[i].len == (u32)use[i], what, "segment length");
        }
        got = concat_segs(seg, nseg, buf_concat, sizeof buf_concat);
        ok(got == total, what, "concat length");
        ok(total == want && memcmp(buf_concat, buf_expect, total) == 0, what,
           "concat bytes equal record bytes");
    }
}

static void test_http_build(void)
{
    static const u16 lens[TBT_BATCH_REC_MAX] = {TBT_FEC_HDR_LEN, 300, TBT_FEC_REC_MAX};
    struct tbt_fec_batch_seg seg[TBT_BATCH_SEG_MAX];
    char scratch[TBT_BATCH_REC_MAX * TBT_HTTP_CHUNK_HDR_MAX];
    int count, i;

    for (count = 1; count <= TBT_BATCH_REC_MAX; count++)
    {
        const u8 *recs[TBT_BATCH_REC_MAX];
        u16 use[TBT_BATCH_REC_MAX];
        size_t total = 0, want = 0, got;
        unsigned int nseg;
        char what[96];

        for (i = 0; i < count; i++)
        {
            use[i] = lens[i];
            fill_rec(i, use[i], 0x20);
            recs[i] = rec_store[i];
        }
        nseg = tbt_fec_batch_build(seg, scratch, recs, use, (u16)count, true, &total);
        snprintf(what, sizeof what, "http count=%d segment count", count);
        ok(nseg == (unsigned int)count * 3u, what, NULL);
        if (nseg != (unsigned int)count * 3u)
            continue;
        for (i = 0; i < count; i++)
        {
            const char *hdr = scratch + (size_t)i * TBT_HTTP_CHUNK_HDR_MAX;
            size_t enc;
            int digits = 0;

            snprintf(what, sizeof what, "http count=%d record=%d", count, i);
            ok(seg[3 * i].base == (const u8 *)hdr, what, "size line in stack scratch");
            ok(seg[3 * i + 1].base == rec_store[i], what, "payload points at record");
            ok(seg[3 * i + 1].len == (u32)use[i], what, "payload length");
            ok(seg[3 * i + 2].base != NULL && seg[3 * i + 2].len == 2 &&
                   seg[3 * i + 2].base[0] == '\r' && seg[3 * i + 2].base[1] == '\n',
               what, "CRLF segment");
            check_size_line(hdr, seg[3 * i].len, use[i], what, &digits);
            enc = tbt_http_fec_chunk_encode((char *)buf_expect + want,
                                            sizeof buf_expect - want, recs[i], use[i]);
            ok(enc > 0, what, "reference chunk encode");
            if (!enc)
                continue;
            ok((size_t)seg[3 * i].len + seg[3 * i + 1].len + seg[3 * i + 2].len == enc,
               what, "segment sum equals encoded chunk");
            want += enc;
        }
        snprintf(what, sizeof what, "http count=%d total", count);
        ok(total == want, what, NULL);
        got = concat_segs(seg, nseg, buf_concat, sizeof buf_concat);
        ok(got == total, what, "concat length");
        ok(total == want && memcmp(buf_concat, buf_expect, total) == 0, what,
           "concat bytes equal per-record chunk encoding");
    }
}

static void test_http_size_lines(void)
{
    static const struct
    {
        u16 len;
        unsigned int digits;
    } cases[] = {
        {TBT_FEC_HDR_LEN, 2}, /* 48  -> "30"      */
        {49, 2},              /* 49  -> "31"      */
        {255, 2},             /* 255 -> "ff"      */
        {256, 3},             /* 256 -> "100"     */
        {1023, 3},            /* 1023 -> "3ff"    */
        {1024, 3},            /* 1024 -> "400"    */
        {1071, 3},            /* 1071 -> "42f"    */
        {TBT_FEC_REC_MAX, 3}, /* 1072 -> "430"    */
    };
    struct tbt_fec_batch_seg seg[TBT_BATCH_SEG_MAX];
    char scratch[TBT_BATCH_REC_MAX * TBT_HTTP_CHUNK_HDR_MAX];
    size_t c;

    for (c = 0; c < sizeof cases / sizeof cases[0]; c++)
    {
        const u8 *recs[1];
        u16 lens1[1];
        size_t total = 0, want;
        unsigned int nseg;
        int digits = 0;
        char what[96];

        snprintf(what, sizeof what, "http size line len=%u", (unsigned int)cases[c].len);
        lens1[0] = cases[c].len;
        fill_rec(0, lens1[0], (u8)(0x30 + c));
        recs[0] = rec_store[0];
        nseg = tbt_fec_batch_build(seg, scratch, recs, lens1, 1, true, &total);
        ok(nseg == 3, what, "three segments");
        if (nseg != 3)
            continue;
        ok(seg[0].base == (const u8 *)scratch, what, "size line at scratch offset 0");
        ok(seg[0].len == cases[c].digits + 2, what, "size line byte count");
        check_size_line((const char *)seg[0].base, seg[0].len, lens1[0], what, &digits);
        ok((unsigned int)digits == cases[c].digits, what, "hex digit boundary");
        ok(seg[1].base == rec_store[0], what, "payload base");
        ok(seg[1].len == (u32)lens1[0], what, "payload length");
        ok(seg[2].len == 2, what, "CRLF length");
        want = tbt_http_fec_chunk_encode((char *)buf_expect, sizeof buf_expect, recs[0],
                                         lens1[0]);
        ok(want > 0, what, "reference chunk encode");
        ok(total == want, what, "total equals encoded chunk");
        ok(concat_segs(seg, nseg, buf_concat, sizeof buf_concat) == total, what, "concat length");
        ok(want > 0 && memcmp(buf_concat, buf_expect, want) == 0, what, "concat bytes");
    }
}

static void test_reject(void)
{
    struct tbt_fec_batch_seg seg[TBT_BATCH_SEG_MAX];
    char scratch[TBT_BATCH_REC_MAX * TBT_HTTP_CHUNK_HDR_MAX];
    const u8 *recs4[4];
    u16 lens4[4];
    const u8 *recs1[1];
    u16 lens1[1];
    const u8 *recs_null[1];
    u16 lens_null[1];
    u16 lens_zero[1] = {0};
    u16 lens_big[1] = {TBT_FEC_REC_MAX + 1};
    u16 lens_short[1] = {TBT_FEC_HDR_LEN - 1};
    size_t total;
    int i;

    for (i = 0; i < 4; i++)
    {
        fill_rec(0, 64, 0x50);
        recs4[i] = rec_store[0];
        lens4[i] = 64;
    }
    fill_rec(0, 64, 0x50);
    recs1[0] = rec_store[0];
    lens1[0] = 64;
    recs_null[0] = NULL;
    lens_null[0] = 64;

    check_reject("reject count=0", seg, scratch, recs1, lens1, 0, false, &total);
    check_reject("reject count=4", seg, scratch, recs4, lens4, 4, false, &total);
    check_reject("reject seg=NULL", NULL, scratch, recs1, lens1, 1, true, &total);
    check_reject("reject hdr_scratch=NULL", seg, NULL, recs1, lens1, 1, true, &total);
    check_reject("reject recs=NULL", seg, scratch, NULL, lens1, 1, false, &total);
    check_reject("reject lens=NULL", seg, scratch, recs1, NULL, 1, false, &total);
    check_reject("reject rec=NULL", seg, scratch, recs_null, lens_null, 1, false, &total);
    check_reject("reject len=0", seg, scratch, recs1, lens_zero, 1, false, &total);
    check_reject("reject len>TBT_FEC_REC_MAX", seg, scratch, recs1, lens_big, 1, false, &total);
    check_reject("reject http len<TBT_FEC_HDR_LEN", seg, scratch, recs1, lens_short, 1, true,
                 &total);
    ok(tbt_fec_batch_build(seg, scratch, recs1, lens1, 1, false, NULL) == 0,
       "reject total=NULL", NULL);
}

/* For every byte offset of a full 3-record batch, stage the tail and append
 * it to the bytes already "sent"; the stream must equal the batch. */
static void test_tail_offsets(bool http, const char *label)
{
    struct tbt_fec_batch_seg seg[TBT_BATCH_SEG_MAX];
    char scratch[TBT_BATCH_REC_MAX * TBT_HTTP_CHUNK_HDR_MAX];
    size_t total = 0, done;
    unsigned int nseg;
    u32 tail;
    char what[96];

    nseg = build_batch(http, TBT_BATCH_REC_MAX, TBT_FEC_REC_MAX, 0x40, seg, scratch,
                       buf_expect, &total);
    snprintf(what, sizeof what, "%s 3x8240 build", label);
    ok(nseg != 0 && total > 0 && total <= BATCH_BUF_MAX, what, NULL);
    if (!nseg)
        return;

    for (done = 0; done <= total; done++)
    {
        size_t remains = total - done;

        tail = 12345;
        ok(tbt_fec_batch_tail(seg, nseg, total, done, buf_dst, sizeof buf_dst, &tail), what,
           "tail returns true");
        ok((size_t)tail == remains, what, "tail length equals total-done");
        ok(memcmp(buf_dst, buf_expect + done, remains) == 0, what,
           "tail bytes equal expected at done offset");
        /* Flush: the first done bytes already reached the wire. */
        memcpy(buf_stream + done, buf_dst, remains);
        ok(memcmp(buf_stream, buf_expect, total) == 0, what, "flushed stream equals batch");
    }
}

/* Carrier policy: A's tail sits in part; B may not send a byte until part is
 * empty. Flush part in several write shapes, including partial ones. */
static size_t sim_flush(u8 *stream, size_t *stream_len, u8 *part, size_t *part_len,
                        size_t accept)
{
    if (accept > *part_len)
        accept = *part_len;
    memcpy(stream + *stream_len, part, accept);
    *stream_len += accept;
    memmove(part, part + accept, *part_len - accept);
    *part_len -= accept;
    return accept;
}

static void append_bytes(u8 *dst, size_t *len, const u8 *src, size_t n)
{
    memcpy(dst + *len, src, n);
    *len += n;
}

/* One socket send that accepts `accept` bytes (0 models -EAGAIN). */
static size_t sim_send(u8 *stream, size_t *stream_len, const u8 *src, size_t len,
                       size_t accept)
{
    if (accept > len)
        accept = len;
    memcpy(stream + *stream_len, src, accept);
    *stream_len += accept;
    return accept;
}

static void test_short_write_order(void)
{
    struct tbt_fec_batch_seg seg[TBT_BATCH_SEG_MAX];
    char scratch[TBT_BATCH_REC_MAX * TBT_HTTP_CHUNK_HDR_MAX];
    size_t atotal = 0, btotal = 0, part_len, stream_len = 0;
    size_t k, accepted, bsent;
    unsigned int nseg;
    u32 tail = 0;

    nseg = build_batch(true, TBT_BATCH_REC_MAX, TBT_FEC_REC_MAX, 0x11, seg, scratch, buf_a,
                       &atotal);
    ok(nseg != 0 && atotal > 0, "order: build A", NULL);
    if (!nseg)
        return;
    k = atotal / 2;

    ok(tbt_fec_batch_tail(seg, nseg, atotal, k, buf_part, sizeof buf_part, &tail),
       "order: stage A tail", NULL);
    part_len = tail;
    ok(part_len == atotal - k, "order: A tail length", NULL);
    ok(memcmp(buf_part, buf_a + k, part_len) == 0, "order: A tail bytes", NULL);
    memcpy(buf_stream, buf_a, k);
    stream_len = k;

    /* B is built and ready, but not one byte may precede A's tail. */
    nseg = build_batch(false, TBT_BATCH_REC_MAX, 1000, 0x77, seg, scratch, buf_b, &btotal);
    ok(nseg != 0 && btotal != 0, "order: build B", NULL);
    if (!nseg)
        return;
    ok(memcmp(buf_stream, buf_a, stream_len) == 0, "order: A prefix intact after B build", NULL);

    accepted = sim_flush(buf_stream, &stream_len, buf_part, &part_len, 1234);
    ok(accepted == 1234 && part_len > 0, "order: first flush is partial", NULL);
    ok(stream_len <= atotal && memcmp(buf_stream, buf_a, stream_len) == 0,
       "order: no B byte before A tail end", NULL);

    accepted = sim_flush(buf_stream, &stream_len, buf_part, &part_len, 1);
    ok(accepted == 1 && part_len > 0, "order: second flush is one byte", NULL);
    ok(stream_len <= atotal && memcmp(buf_stream, buf_a, stream_len) == 0,
       "order: prefix still A only", NULL);

    accepted = sim_flush(buf_stream, &stream_len, buf_part, &part_len, (size_t)-1);
    ok(accepted > 0 && part_len == 0, "order: final flush drains part", NULL);
    ok(stream_len == atotal && memcmp(buf_stream, buf_a, atotal) == 0,
       "order: A complete before B starts", NULL);

    bsent = 999;
    append_bytes(buf_stream, &stream_len, buf_b, bsent);
    ok(stream_len == atotal + bsent && memcmp(buf_stream, buf_a, atotal) == 0 &&
           memcmp(buf_stream + atotal, buf_b, bsent) == 0,
       "order: B follows A tail only", NULL);
    append_bytes(buf_stream, &stream_len, buf_b + bsent, btotal - bsent);
    ok(stream_len == atotal + btotal, "order: final stream length", NULL);
    ok(memcmp(buf_stream, buf_a, atotal) == 0 &&
           memcmp(buf_stream + atotal, buf_b, btotal) == 0,
       "order: final stream is A||B", NULL);
}

static void test_eagain(void)
{
    struct tbt_fec_batch_seg seg[TBT_BATCH_SEG_MAX];
    char scratch[TBT_BATCH_REC_MAX * TBT_HTTP_CHUNK_HDR_MAX];
    static u8 snapshot[BATCH_BUF_MAX];
    size_t total = 0, k, part_len = 0, stream_len = 0, before;
    unsigned int nseg;
    u32 tail = 0;

    nseg = build_batch(true, TBT_BATCH_REC_MAX, TBT_FEC_REC_MAX, 0x33, seg, scratch,
                       buf_expect, &total);
    ok(nseg != 0 && total > 0, "eagain: build batch", NULL);
    if (!nseg)
        return;
    k = total / 2;

    /* The first send fails with -EAGAIN: no byte leaves, no part exists. */
    ok(sim_send(buf_stream, &stream_len, buf_expect, total, 0) == 0,
       "eagain: zero-byte send accepted nothing", NULL);
    ok(stream_len == 0 && part_len == 0, "eagain: stream and part unchanged", NULL);
    ok(sim_send(buf_stream, &stream_len, buf_expect, total, total) == total,
       "eagain: whole batch retried", NULL);
    ok(stream_len == total && memcmp(buf_stream, buf_expect, total) == 0,
       "eagain: retry writes the whole batch once", NULL);

    /* Short write, then -EAGAIN on the staged tail. */
    stream_len = 0;
    part_len = 0;
    append_bytes(buf_stream, &stream_len, buf_expect, k);
    ok(tbt_fec_batch_tail(seg, nseg, total, k, buf_part, sizeof buf_part, &tail),
       "eagain: stage tail", NULL);
    part_len = tail;
    ok(part_len == total - k, "eagain: tail length", NULL);
    memcpy(snapshot, buf_part, part_len);

    before = stream_len;
    ok(sim_flush(buf_stream, &stream_len, buf_part, &part_len, 0) == 0,
       "eagain: send returns zero", NULL);
    ok(stream_len == before, "eagain: stream unchanged", NULL);
    ok(part_len == total - k && memcmp(buf_part, snapshot, part_len) == 0,
       "eagain: part unchanged", NULL);

    ok(sim_flush(buf_stream, &stream_len, buf_part, &part_len, (size_t)-1) == total - k,
       "eagain: retry consumes the tail", NULL);
    ok(part_len == 0, "eagain: part empty", NULL);
    ok(stream_len == total && memcmp(buf_stream, buf_expect, total) == 0,
       "eagain: no duplicate and no loss", NULL);
}

static void test_tail_rejects(void)
{
    struct tbt_fec_batch_seg seg[TBT_BATCH_SEG_MAX];
    struct tbt_fec_batch_seg bad[TBT_BATCH_SEG_MAX];
    char scratch[TBT_BATCH_REC_MAX * TBT_HTTP_CHUNK_HDR_MAX];
    size_t total = 0;
    unsigned int nseg;
    u32 tail = 0;

    nseg = build_batch(true, TBT_BATCH_REC_MAX, TBT_FEC_REC_MAX, 0x60, seg, scratch,
                       buf_expect, &total);
    ok(nseg != 0 && total > 0, "tail reject: build", NULL);
    if (!nseg)
        return;

    memset(buf_dst, 0xA5, sizeof buf_dst);
    ok(!tbt_fec_batch_tail(seg, nseg, total, total + 1, buf_dst, sizeof buf_dst, &tail),
       "tail rejects done>total", NULL);
    ok(all_bytes(buf_dst, sizeof buf_dst, 0xA5), "tail rejects done>total",
       "dst left untouched");

    memset(buf_dst, 0xA5, sizeof buf_dst);
    ok(!tbt_fec_batch_tail(seg, nseg, total, 0, buf_dst, total - 1, &tail),
       "tail rejects tail>cap", NULL);
    ok(all_bytes(buf_dst, sizeof buf_dst, 0xA5), "tail rejects tail>cap",
       "dst left untouched");

    memcpy(bad, seg, nseg * sizeof seg[0]);
    bad[1].len += 1;
    memset(buf_dst, 0xA5, sizeof buf_dst);
    ok(!tbt_fec_batch_tail(bad, nseg, total, 0, buf_dst, sizeof buf_dst, &tail),
       "tail rejects segment sum too large", NULL);
    ok(all_bytes(buf_dst, sizeof buf_dst, 0xA5), "tail rejects segment sum too large",
       "dst left untouched");

    memcpy(bad, seg, nseg * sizeof seg[0]);
    bad[nseg - 1].len -= 1;
    memset(buf_dst, 0xA5, sizeof buf_dst);
    ok(!tbt_fec_batch_tail(bad, nseg, total, 0, buf_dst, sizeof buf_dst, &tail),
       "tail rejects segment sum too small", NULL);
    ok(all_bytes(buf_dst, sizeof buf_dst, 0xA5), "tail rejects segment sum too small",
       "dst left untouched");

    ok(!tbt_fec_batch_tail(seg, 0, total, 0, buf_dst, sizeof buf_dst, &tail),
       "tail rejects nseg=0", NULL);
}

int main(void)
{
    test_raw_build();
    test_http_build();
    test_http_size_lines();
    test_reject();
    test_tail_offsets(false, "raw");
    test_tail_offsets(true, "http");
    test_short_write_order();
    test_eagain();
    test_tail_rejects();

    printf("test_batch: checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
