/* test_http_fuzz.c - hostile-input tests for the HTTP container parsers.
 *
 * Deterministic: xorshift64 seeded with 1, no fixtures, no external
 * dependencies. It links tbtun/tbt_http.c and tbtun/tbt_proto.c, the same
 * objects the module is built from, so a rule that passes here is a rule the
 * kernel enforces.
 *
 * What is asserted for 200k random byte streams:
 *   - tbt_http_resp_parse() and tbt_http_req_parse() never consume more than
 *     len, return only OK/NEED_MORE/PROTO, write *out only on OK, and report a
 *     header_len no larger than the input.
 *   - tbt_http_skip_request_header() never consumes more than len, returns
 *     only OK/NEED_MORE/PROTO, and on OK ends at a CRLFCRLF.
 *   - tbt_http_chunk_feed() never consumes more than it was given, never
 *     points a body run outside the input, makes progress on every OK result,
 *     consumes all input on NEED_MORE, and always terminates (the step cap is
 *     len + 2).
 *   - every buffer is an exact-size heap copy, so -fsanitize=address turns any
 *     read past len into a failure instead of a silent pass inside a larger
 *     static buffer.
 *
 * A third of the cases start with a valid request, response or chunk prefix;
 * the rest is random bytes. The prefixes push the parsers past the first line
 * so the header and chunk state machines, not just their first branch, see
 * hostile input.
 *
 * The 0.4.0 v7 OPEN body (32-byte physical group bitmap) gets its own pass:
 * every mask shape is carried as one chunk per record, the byte stream is
 * split again at every 1..7 byte boundary and must reassemble to the same
 * records; body lengths 0/1/31/33/64 and chunks whose size disagrees with the
 * header's body_len must end in PROTO, never in a record.
 *
 *   make host-test-san
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_http.h"

#define FUZZ_BUFFERS 200000

static u64 rng_state = 1;
static u64 fuzz_seed = 1;
static unsigned long case_no;
static long checks;
static int failures;
static unsigned long open_cases;
static unsigned long open_feeds;

static u64 rnd64(void)
{
    u64 x = rng_state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static size_t rnd_below(size_t n)
{
    return n ? (size_t)(rnd64() % n) : 0;
}

static void check(int cond, const char *what)
{
    checks++;
    if (cond)
        return;
    failures++;
    if (failures <= 20)
        printf("FAIL seed=1 case=%lu: %s\n", case_no, what);
}

static int valid_status(int err)
{
    return err == TBT_HTTP_OK || err == TBT_HTTP_NEED_MORE || err == TBT_HTTP_PROTO;
}

static void fuzz_case(const u8 *buf, size_t len)
{
    struct tbt_http_resp resp;
    struct tbt_http_req req;
    size_t used;
    int err;

    /* Response header parser. */
    memset(&resp, 0, sizeof(resp));
    resp.header_len = 0xdeadbeef;
    used = tbt_http_resp_parse((const char *)buf, len, &resp, &err);
    check(used <= len, "resp consumed more than len");
    check(valid_status(err), "resp returned an unknown status");
    if (err == TBT_HTTP_OK)
    {
        check(used == resp.header_len, "resp OK header_len differs");
        check(used > 0 && used <= len, "resp OK consumed an impossible length");
    }
    else
    {
        check(used == 0, "resp refusal consumed bytes");
        check(resp.header_len == 0xdeadbeef, "resp wrote out on refusal");
    }

    /* Request header parser. */
    memset(&req, 0, sizeof(req));
    req.header_len = 0xdeadbeef;
    req.host = (const char *)0x1;
    used = tbt_http_req_parse((const char *)buf, len, &req, &err);
    check(used <= len, "req consumed more than len");
    check(valid_status(err), "req returned an unknown status");
    if (err == TBT_HTTP_OK)
    {
        check(used == req.header_len, "req OK header_len differs");
        check(used > 0 && used <= len, "req OK consumed an impossible length");
        check(req.host_len <= len, "req host_len exceeds input");
        check(req.host >= (const char *)buf &&
                  req.host + req.host_len <= (const char *)buf + len,
              "req host points outside the input");
    }
    else
    {
        check(used == 0, "req refusal consumed bytes");
        check(req.header_len == 0xdeadbeef && req.host == (const char *)0x1,
              "req wrote out on refusal");
    }

    /* Request-header skip helper. */
    used = tbt_http_skip_request_header(buf, len, &err);
    check(used <= len, "skip consumed more than len");
    check(valid_status(err), "skip returned an unknown status");
    if (err == TBT_HTTP_OK)
    {
        check(used >= 4 && used <= len, "skip OK returned an impossible length");
        check(used >= 4 && buf[used - 4] == '\r' && buf[used - 3] == '\n' &&
                  buf[used - 2] == '\r' && buf[used - 1] == '\n',
              "skip OK does not end in CRLFCRLF");
    }
    else
    {
        check(used == 0, "skip refusal consumed bytes");
    }

    /* Chunk parser. */
    {
        struct tbt_http_chunk_parser p = {};
        size_t pos = 0;
        unsigned long steps = 0;

        for (;;)
        {
            const u8 *body = NULL;
            size_t blen = 0, n;

            if (steps++ > len + 2)
            {
                check(0, "chunk parser did not terminate");
                break;
            }
            n = tbt_http_chunk_feed(&p, buf + pos, len - pos, &body, &blen, &err);
            check(n <= len - pos, "chunk consumed more than it was given");
            check(valid_status(err), "chunk returned an unknown status");
            if (body)
            {
                check(body >= buf + pos && body + blen <= buf + len,
                      "chunk body run points outside the input");
            }
            if (err == TBT_HTTP_PROTO)
            {
                check(n == 0, "chunk PROTO consumed bytes");
                break;
            }
            if (err == TBT_HTTP_NEED_MORE)
            {
                check(n == len - pos, "chunk NEED_MORE did not consume all input");
                break;
            }
            if (p.state == TBT_HTTP_CHUNK_DONE)
                break;
            check(n > 0, "chunk OK made no progress");
            if (!n)
                break;
            pos += n;
        }
    }

    /* Frame decoder: chunk stream -> one record at a time. */
    {
        struct tbt_http_rx rx = {};
        size_t pos = 0;
        unsigned long steps = 0;

        for (;;)
        {
            struct tbt_rec rec;
            size_t n;

            if (steps++ > len + 2)
            {
                check(0, "frame decoder did not terminate");
                break;
            }
            n = tbt_http_frame_decode(&rx, buf + pos, len - pos, &rec, &err);
            check(n <= len - pos, "frame consumed more than len");
            check(err == TBT_RX_OK || err == TBT_RX_NEED_MORE || err == TBT_RX_PROTO,
                  "frame returned an unknown status");
            if (err == TBT_RX_OK)
            {
                check(rec.body_len <= TBT_PAYLOAD_MAX, "frame body_len out of range");
                check(rec.body >= rx.rec && rec.body + rec.body_len <= rx.rec + sizeof(rx.rec),
                      "frame body points outside the assembler");
                check(rec.hdr.body_len == rec.body_len, "frame header/body length mismatch");
                check(tbt_hdr_check(&rec.hdr) == 0, "frame header failed validation");
            }
            else if (err == TBT_RX_PROTO)
            {
                check(n == 0, "frame PROTO consumed bytes");
                break;
            }
            else
            {
                if (tbt_http_rx_done(&rx))
                    break;
                if (!n)
                {
                    if (pos < len)
                        check(0, "frame NEED_MORE made no progress with input left");
                    break;
                }
            }
            if (!n)
            {
                check(0, "frame decoder made no progress");
                break;
            }
            pos += n;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 0.4.0 v7 OPEN bodies through the one-chunk-per-record container. */

static size_t open_rec_make(u8 *dst, u16 body_len, const u8 *mask)
{
    size_t len = TBT_HDR_LEN + (size_t)body_len;

    memset(dst, 0, len);
    dst[TBT_H_TYPE] = TBT_F_OPEN;
    tbt_put_le16(dst + TBT_H_BODYLEN, body_len);
    tbt_put_le16(dst + TBT_H_CONV, TBT_CONV);
    if (body_len)
    {
        memset(dst + TBT_HDR_LEN, 0xa5, body_len);
        if (mask)
            memcpy(dst + TBT_HDR_LEN, mask,
                   body_len < TBT_OPEN_BODY_LEN ? body_len : TBT_OPEN_BODY_LEN);
    }
    return len;
}

static size_t open_add_chunk(u8 *dst, size_t cap, size_t off, const u8 *rec,
                             size_t rec_len)
{
    size_t n = tbt_http_chunk_hdr((char *)dst + off, cap - off, rec_len);

    if (!n || off + n + rec_len + 2 > cap)
        return 0;
    memcpy(dst + off + n, rec, rec_len);
    if (!tbt_http_chunk_tail((char *)dst + off + n + rec_len,
                             cap - off - n - rec_len))
        return 0;
    return off + n + rec_len + 2;
}

static size_t open_add_term(u8 *dst, size_t cap, size_t off)
{
    size_t n = tbt_http_chunk_hdr((char *)dst + off, cap - off, 0);

    if (!n || off + n + 2 > cap)
        return 0;
    if (!tbt_http_chunk_tail((char *)dst + off + n, cap - off - n))
        return 0;
    return off + n + 2;
}

/* Feed the stream in pieces of `step` bytes. A record is valid only if its
 * type, body length and 32 mask bytes match the source; PROTO is accepted
 * only when the case expects it. */
static void open_feed(const u8 *stream, size_t len, size_t step,
                      const u8 masks[][TBT_OPEN_BODY_LEN],
                      unsigned want_records, int want_done, int want_proto,
                      const char *what)
{
    struct tbt_http_rx rx = {};
    u8 *buf = malloc(len ? len : 1);
    size_t pos = 0, fed = 0;
    unsigned records = 0;
    int saw_proto = 0;

    open_cases++;
    if (!buf)
    {
        printf("FAIL seed=1 case=%lu: out of memory\n", case_no);
        exit(2);
    }
    memcpy(buf, stream, len);

    while (fed < len || pos < fed)
    {
        struct tbt_rec rec;
        size_t n;
        int err = TBT_RX_NEED_MORE;

        if (fed == pos && fed < len)
        {
            size_t take = len - fed;

            if (take > step)
                take = step;
            fed += take;
        }
        if (fed == pos)
            break;
        open_feeds++;
        n = tbt_http_frame_decode(&rx, buf + pos, fed - pos, &rec, &err);
        checks++;
        if (n > fed - pos)
        {
            failures++;
            printf("FAIL seed=1 case=%lu: %s: consumed %zu of %zu\n", case_no,
                   what, n, fed - pos);
            break;
        }
        if (err == TBT_RX_OK)
        {
            checks++;
            if (want_proto || records >= want_records ||
                rec.hdr.type != TBT_F_OPEN ||
                rec.body_len != TBT_OPEN_BODY_LEN ||
                memcmp(rec.body, masks[records], TBT_OPEN_BODY_LEN))
            {
                failures++;
                printf("FAIL seed=1 case=%lu: %s: bad record %u\n", case_no,
                       what, records);
                break;
            }
            records++;
            pos += n;
            continue;
        }
        if (err == TBT_RX_PROTO)
        {
            saw_proto = 1;
            break;
        }
        /* NEED_MORE consumed every byte it was given. */
        pos += n;
        if (tbt_http_rx_done(&rx))
            break;
        if (!n)
        {
            failures++;
            printf("FAIL seed=1 case=%lu: %s: no progress\n", case_no, what);
            break;
        }
    }

    checks++;
    if (want_proto ? !saw_proto : saw_proto)
    {
        failures++;
        printf("FAIL seed=1 case=%lu: %s: PROTO verdict %d\n", case_no, what,
               saw_proto);
    }
    if (!want_proto)
    {
        checks++;
        if (records != want_records)
        {
            failures++;
            printf("FAIL seed=1 case=%lu: %s: decoded %u of %u records\n",
                   case_no, what, records, want_records);
        }
        checks++;
        if (want_done != (tbt_http_rx_done(&rx) ? 1 : 0))
        {
            failures++;
            printf("FAIL seed=1 case=%lu: %s: done=%d want %d\n", case_no,
                   what, tbt_http_rx_done(&rx), want_done);
        }
        checks++;
        if (want_done && pos != len)
        {
            failures++;
            printf("FAIL seed=1 case=%lu: %s: done at %zu of %zu\n", case_no,
                   what, pos, len);
        }
    }
    free(buf);
}

static void test_open_chunks(void)
{
    u8 base[TBT_OPEN_BODY_LEN];
    u8 masks[3][TBT_OPEN_BODY_LEN];
    u8 rec[TBT_HDR_LEN + 64];
    u8 stream[1024];
    unsigned c, j, step;

    for (c = 0; c < 4; c++)
    {
        size_t off = 0;

        case_no++;
        if (c == 0)
            memset(base, 0, sizeof(base));
        else if (c == 1)
            memset(base, 0xff, sizeof(base));
        else if (c == 2)
            memset(base, 0x55, sizeof(base));
        else
            for (j = 0; j < sizeof(base); j += 8)
            {
                u64 v = rnd64();

                memcpy(base + j, &v, 8);
            }
        for (j = 0; j < sizeof(base); j++)
        {
            masks[0][j] = base[j];
            masks[1][j] = (u8)(base[j] ^ 0x01);
            masks[2][j] = (u8)~base[j];
        }
        for (j = 0; j < 3; j++)
        {
            size_t n = open_rec_make(rec, TBT_OPEN_BODY_LEN, masks[j]);

            off = open_add_chunk(stream, sizeof(stream), off, rec, n);
            if (!off)
            {
                printf("FAIL seed=1 case=%lu: OPEN chunk build\n", case_no);
                exit(2);
            }
        }
        off = open_add_term(stream, sizeof(stream), off);
        if (!off)
        {
            printf("FAIL seed=1 case=%lu: OPEN terminator build\n", case_no);
            exit(2);
        }

        for (step = 1; step <= 7; step++)
            open_feed(stream, off, step, masks, 3, 1, 0,
                      "OPEN chunks split at 1..7 bytes");
        /* Cutting the terminator leaves every record decoded but not DONE. */
        open_feed(stream, off - 2, 2, masks, 3, 0, 0,
                  "OPEN stream with a short terminator");
        open_feed(stream, off - 4, 5, masks, 3, 0, 0,
                  "OPEN stream with a shorter terminator");
    }

    /* A body length other than 32, or a chunk whose size disagrees with the
     * header, is a framing error whichever boundary the split lands on. */
    {
        static const struct
        {
            u16 hdr_body;
            size_t wire_len;
        } bad[] = {
            { 0, 32 },  { 1, 33 },  { 31, 63 }, { 33, 65 }, { 64, 96 },
            { 32, 63 }, { 32, 65 }, { 32, 66 }, { 32, 31 },
        };

        for (c = 0; c < sizeof(bad) / sizeof(bad[0]); c++)
        {
            size_t off;

            case_no++;
            memset(rec, 0, bad[c].wire_len);
            if (bad[c].wire_len >= TBT_HDR_LEN)
            {
                rec[TBT_H_TYPE] = TBT_F_OPEN;
                tbt_put_le16(rec + TBT_H_BODYLEN, bad[c].hdr_body);
                tbt_put_le16(rec + TBT_H_CONV, TBT_CONV);
                memset(rec + TBT_HDR_LEN, 0xa5,
                       bad[c].wire_len - TBT_HDR_LEN);
            }
            off = open_add_chunk(stream, sizeof(stream), 0, rec,
                                 bad[c].wire_len);
            if (!off)
            {
                printf("FAIL seed=1 case=%lu: hostile chunk build\n", case_no);
                exit(2);
            }
            off = open_add_term(stream, sizeof(stream), off);
            if (!off)
            {
                printf("FAIL seed=1 case=%lu: hostile terminator build\n",
                       case_no);
                exit(2);
            }
            for (step = 1; step <= 7; step++)
                open_feed(stream, off, step, NULL, 0, 0, 1,
                          "hostile OPEN body or chunk size");
        }
    }
}

int main(void)
{
    static const char *const prefixes[] = {
        "POST /api/v2/upload HTTP/1.1\r\nHost: 203.0.113.9\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: application/gzip\r\n"
        "Transfer-Encoding: chunked\r\n",
        "a\r\n0123456789\r\n",
    };
    const char *env = getenv("TBT_HTTP_SEED");
    u8 pad_prefix[TBT_HTTP_PAD_CHUNK_LEN];
    size_t pad_len = tbt_http_pad_chunk((char *)pad_prefix, sizeof(pad_prefix));
    unsigned long i;

    if (env && *env)
    {
        char *end = NULL;
        u64 v = strtoull(env, &end, 0);

        if (end && end != env && v)
            fuzz_seed = v;
    }
    rng_state = fuzz_seed;

    test_open_chunks();

    for (i = 0; i < FUZZ_BUFFERS; i++)
    {
        size_t len = (i % 1000) == 0 ? rnd_below(9000) : rnd_below(513);
        u8 *buf = malloc(len ? len : 1);
        size_t j;

        if (!buf)
        {
            printf("FAIL seed=1 case=%lu: out of memory\n", case_no);
            return 2;
        }
        for (j = 0; j < len; j++)
            buf[j] = (u8)rnd64();
        if (len)
        {
            if ((i % 4) == 3)
            {
                size_t plen = pad_len < len ? pad_len : len;

                memcpy(buf, pad_prefix, plen);
            }
            else
            {
                const char *prefix = prefixes[i % 3];
                size_t plen = strlen(prefix);

                if (plen > len)
                    plen = len;
                memcpy(buf, prefix, plen);
            }
        }

        case_no = i + 1;
        fuzz_case(buf, len);
        free(buf);
    }

    printf("test_http_fuzz: seed=0x%016llx checks=%ld failures=%d buffers=%d"
           " open_cases=%lu open_feeds=%lu\n",
           (unsigned long long)fuzz_seed, checks, failures, FUZZ_BUFFERS,
           open_cases, open_feeds);
    return failures ? 1 : 0;
}
