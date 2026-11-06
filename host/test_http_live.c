/* test_http_live.c - conformance check for a captured http-kal carrier stream.
 *
 * Reads one capture from stdin in either frozen format:
 *
 *   1. proc text (debug snapshot, line-oriented):
 *        req=<escaped request header block including final CRLFCRLF>
 *        resp=<escaped response header block>
 *        first_chunk=<escaped first chunk size line + up to 128 bytes>
 *        req_len=<unescaped req length>
 *        resp_len=<unescaped resp length>
 *      Escapes: \r, \n, \xNN; a literal backslash is \x5c (the harness also
 *      accepts \\). Lines without a known field are ignored (console noise).
 *
 *   2. raw binary (offline byte dump):
 *        TBT-HTTP-CAPTURE 1\n
 *        request <len>\n
 *        <len raw bytes: header block + chunked body + terminator>
 *        response <len>\n
 *        <len raw bytes>
 *
 * The text format proves headers and the first chunk; the binary format proves
 * the whole body. Both are documented in docs/http-profile-integration.md.
 *
 * The harness is host-only: it links the same tbt_http.c/tbt_rec.c the module
 * uses. Exit status is non-zero on any FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_http.h"

#define LIVE_MAX (16u * 1024u * 1024u)

static int checks;
static int failures;
static int skips;

static void check(int cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
    {
        printf("PASS %s\n", what);
        return;
    }
    failures++;
    printf("FAIL %s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
}

static void skip(const char *what, const char *why)
{
    skips++;
    printf("SKIP %s%s%s\n", what, why && *why ? ": " : "", why ? why : "");
}

/* ------------------------------------------------------------------------- */
/* Input and text escaping.                                                   */

static u8 *read_all(FILE *f, size_t *out_len)
{
    size_t cap = 4096, len = 0;
    u8 *buf = malloc(cap);

    if (!buf)
        return NULL;
    for (;;)
    {
        size_t got;

        if (len == cap)
        {
            u8 *grown;

            if (cap >= LIVE_MAX)
            {
                free(buf);
                return NULL;
            }
            cap *= 2;
            grown = realloc(buf, cap);
            if (!grown)
            {
                free(buf);
                return NULL;
            }
            buf = grown;
        }
        got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (!got)
        {
            if (ferror(f))
            {
                free(buf);
                return NULL;
            }
            break;
        }
    }
    *out_len = len;
    return buf;
}

/* Next LF-terminated line, with one trailing CR stripped. The last line does
 * not need a trailing LF. */
static int next_line(const u8 *buf, size_t len, size_t *pos, const char **line, size_t *line_len)
{
    size_t start = *pos, i;

    if (start >= len)
        return 0;
    for (i = start; i < len; i++)
    {
        if (buf[i] == '\n')
        {
            size_t end = i;

            if (end > start && buf[end - 1] == '\r')
                end--;
            *line = (const char *)buf + start;
            *line_len = end - start;
            *pos = i + 1;
            return 1;
        }
    }
    {
        size_t end = len;

        if (end > start && buf[end - 1] == '\r')
            end--;
        *line = (const char *)buf + start;
        *line_len = end - start;
        *pos = len;
    }
    return 1;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Unescape the frozen text format. Rejects raw control bytes and bad escapes. */
static int unescape_value(const char *s, size_t n, u8 **out, size_t *out_len)
{
    u8 *buf = malloc(n ? n : 1);
    size_t o = 0, i;

    if (!buf)
        return -1;
    for (i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];

        if (c != '\\')
        {
            if (c < 0x20 || c > 0x7e)
            {
                free(buf);
                return -1;
            }
            buf[o++] = c;
            continue;
        }
        i++;
        if (i >= n)
        {
            free(buf);
            return -1;
        }
        if (s[i] == 'r')
            buf[o++] = '\r';
        else if (s[i] == 'n')
            buf[o++] = '\n';
        else if (s[i] == '\\')
            buf[o++] = '\\';
        else if (s[i] == 'x')
        {
            int hi, lo;

            if (i + 2 >= n)
            {
                free(buf);
                return -1;
            }
            hi = hexval(s[i + 1]);
            lo = hexval(s[i + 2]);
            if (hi < 0 || lo < 0)
            {
                free(buf);
                return -1;
            }
            buf[o++] = (u8)(hi * 16 + lo);
            i += 2;
        }
        else
        {
            free(buf);
            return -1;
        }
    }
    *out = buf;
    *out_len = o;
    return 0;
}

static int parse_decimal(const char *s, size_t n, size_t *out)
{
    size_t v = 0, i;

    if (!n)
        return -1;
    for (i = 0; i < n; i++)
    {
        unsigned digit;

        if (s[i] < '0' || s[i] > '9')
            return -1;
        digit = (unsigned)(s[i] - '0');
        if (v > ((size_t)-1 - digit) / 10)
            return -1;
        v = v * 10 + digit;
    }
    *out = v;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Header checks.                                                             */

static const char design_ua[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0 "
    "Safari/537.36";

static int line_is(const char *line, size_t n, const char *want)
{
    return strlen(want) == n && memcmp(line, want, n) == 0;
}

static int reqid_ok(const char *s, size_t n)
{
    size_t i;

    if (n != TBT_HTTP_REQID_LEN)
        return 0;
    for (i = 0; i < n; i++)
    {
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '2' && s[i] <= '7')))
            return 0;
    }
    return 1;
}

static void check_req_header(const u8 *buf, size_t len)
{
    size_t pos = 0, line_len;
    const char *line;
    int idx = 0;

    if (!next_line(buf, len, &pos, &line, &line_len))
    {
        check(0, "request line", "empty header block");
        return;
    }
    check(line_is(line, line_len, "POST /api/v2/upload HTTP/1.1"), "request line", NULL);

    while (next_line(buf, len, &pos, &line, &line_len))
    {
        if (!line_len)
            break;
        idx++;
        switch (idx)
        {
        case 1:
            check(line_len > 6 && !memcmp(line, "Host: ", 6), "request Host", NULL);
            break;
        case 2:
            check(line_len == 12 + strlen(design_ua) && !memcmp(line, "User-Agent: ", 12) &&
                      !memcmp(line + 12, design_ua, strlen(design_ua)),
                  "request User-Agent", NULL);
            break;
        case 3:
            check(line_is(line, line_len, "Accept: */*"), "request Accept", NULL);
            break;
        case 4:
            check(line_is(line, line_len, "Accept-Encoding: identity"), "request Accept-Encoding",
                  NULL);
            break;
        case 5:
            check(line_is(line, line_len, "Content-Type: application/gzip"),
                  "request Content-Type", NULL);
            break;
        case 6:
            check(line_is(line, line_len, "Transfer-Encoding: chunked"),
                  "request Transfer-Encoding", NULL);
            break;
        case 7:
            check(line_is(line, line_len, "Connection: keep-alive"), "request Connection", NULL);
            break;
        case 8:
            check(line_len == 14 + TBT_HTTP_REQID_LEN &&
                      !memcmp(line, "X-Request-Id: ", 14) &&
                      reqid_ok(line + 14, line_len - 14),
                  "request X-Request-Id", NULL);
            break;
        default:
            check(0, "request header count", "more than eight headers");
            break;
        }
    }
    check(idx == 8, "request header count", NULL);
    check(pos == len, "request header block length", NULL);
}

static void check_resp_header(const u8 *buf, size_t len)
{
    size_t pos = 0, line_len;
    const char *line;
    int idx = 0;

    if (!next_line(buf, len, &pos, &line, &line_len))
    {
        check(0, "response status line", "empty header block");
        return;
    }
    check(line_is(line, line_len, "HTTP/1.1 200 OK"), "response status line", NULL);

    while (next_line(buf, len, &pos, &line, &line_len))
    {
        if (!line_len)
            break;
        idx++;
        switch (idx)
        {
        case 1:
            check(line_is(line, line_len, "Content-Type: application/gzip"),
                  "response Content-Type", NULL);
            break;
        case 2:
            check(line_is(line, line_len, "Transfer-Encoding: chunked"),
                  "response Transfer-Encoding", NULL);
            break;
        case 3:
            check(line_is(line, line_len, "Connection: keep-alive"), "response Connection", NULL);
            break;
        case 4:
            check(line_is(line, line_len, "Server: nginx"), "response Server", NULL);
            break;
        case 5:
            check(line_len == 6 + TBT_HTTP_DATE_LEN && !memcmp(line, "Date: ", 6) &&
                      !memcmp(line + 6 + TBT_HTTP_DATE_LEN - 4, " GMT", 4),
                  "response Date", NULL);
            break;
        default:
            check(0, "response header count", "more than five headers");
            break;
        }
    }
    check(idx == 5, "response header count", NULL);
    check(pos == len, "response header block length", NULL);
}

/* ------------------------------------------------------------------------- */
/* Chunk/record checks.                                                       */

static void scan_chunks(const char *name, const u8 *body, size_t len, int expect_pad,
                        int expect_drip, size_t *records_out)
{
    struct tbt_http_chunk_parser p = {};
    size_t pos = 0, chunk_start = 0, chunks = 0, pads = 0, drips = 0;
    const u8 *body_start = NULL;
    int started = 0;

    while (pos < len)
    {
        const u8 *b = NULL;
        size_t bl = 0, n;
        int err;

        n = tbt_http_chunk_feed(&p, body + pos, len - pos, &b, &bl, &err);
        if (n > len - pos)
        {
            check(0, "chunk consumed more than the capture", name);
            return;
        }
        if (err == TBT_HTTP_PROTO)
        {
            check(0, "chunk stream parses", name);
            return;
        }
        if (err == TBT_HTTP_NEED_MORE)
        {
            check(0, "chunk stream complete", name);
            return;
        }
        if (p.state == TBT_HTTP_CHUNK_DONE)
        {
            pos += n;
            break;
        }
        if (!bl || !n)
        {
            check(0, "chunk progress", name);
            return;
        }
        if (!started)
        {
            started = 1;
            body_start = b;
        }
        pos += n;

        if (p.state == TBT_HTTP_CHUNK_DATA_END)
        {
            size_t wire = pos - chunk_start + 2;

            if (p.chunk_size == 1)
            {
                drips++;
                check(wire == TBT_HTTP_DRIP_CHUNK_LEN, "drip chunk wire length 6", name);
            }
            else if (p.chunk_size == TBT_HDR_LEN)
            {
                struct tbt_hdr h;

                tbt_hdr_decode(body_start, &h);
                if (h.type == TBT_F_PAD)
                {
                    pads++;
                    check(wire == TBT_HTTP_PAD_CHUNK_LEN, "PAD chunk wire length 38", name);
                }
            }
            chunks++;
            started = 0;
            chunk_start = pos + 2;
        }
    }

    check(pos == len, "chunk stream consumed exactly", name);
    check(tbt_http_chunk_done(&p), "chunk terminator", name);
    if (!chunks)
        skip("chunks", "empty segment (terminator only)");
    if (expect_pad)
        check(pads > 0, "PAD chunk present", name);
    else if (!pads)
        skip("PAD chunk", "none in this capture");
    if (expect_drip)
        check(drips > 0, "drip chunk present", name);
    else if (!drips)
        skip("drip chunk", "none in this capture");

    *records_out = chunks - drips;
}

static void decode_records(const char *name, const u8 *body, size_t len, size_t expect_records)
{
    struct tbt_http_rx rx = {};
    struct tbt_rec rec;
    size_t pos = 0, records = 0, pads = 0;
    int err;

    while (pos < len)
    {
        size_t n = tbt_http_frame_decode(&rx, body + pos, len - pos, &rec, &err);

        if (n > len - pos)
        {
            check(0, "record consumed more than the capture", name);
            return;
        }
        pos += n;
        if (err == TBT_RX_OK)
        {
            records++;
            check(rec.body_len <= TBT_PAYLOAD_MAX, "record body_len bound", name);
            check(rec.hdr.body_len == rec.body_len, "record header/body length", name);
            check(tbt_hdr_check(&rec.hdr) == 0, "record header valid", name);
            check(rec.body >= rx.rec && rec.body + rec.body_len <= rx.rec + sizeof(rx.rec),
                  "record body inside the assembler", name);
            if (rec.hdr.type == TBT_F_PAD)
                pads++;
        }
        else if (err == TBT_RX_PROTO)
        {
            check(0, "record stream parses", name);
            return;
        }
        else
        {
            if (tbt_http_rx_done(&rx))
                break;
            if (!n)
            {
                check(0, "record stream progress", name);
                return;
            }
        }
    }
    check(tbt_http_rx_done(&rx), "record terminator", name);
    check(pos == len, "record stream consumed exactly", name);
    check(records == expect_records, "record count equals non-drip chunks", name);
    if (pads)
        check(1, "PAD record decoded", name);
    else
        skip("PAD record", "none in this capture");
}

/* ------------------------------------------------------------------------- */
/* Text capture.                                                              */

struct text_capture
{
    u8 *req;
    size_t req_len;
    u8 *resp;
    size_t resp_len;
    u8 *first;
    size_t first_len;
    size_t req_declared;
    size_t resp_declared;
    int have_req;
    int have_resp;
    int have_first;
    int have_req_len;
    int have_resp_len;
};

static int parse_text(const u8 *buf, size_t len, struct text_capture *cap)
{
    size_t pos = 0, line_len;
    const char *line;

    memset(cap, 0, sizeof(*cap));
    while (next_line(buf, len, &pos, &line, &line_len))
    {
        const char *eq = memchr(line, '=', line_len);
        size_t name_len, value_len;
        const char *value;

        if (!eq)
            continue; /* console noise */
        name_len = (size_t)(eq - line);
        value = eq + 1;
        value_len = line_len - name_len - 1;

        if (name_len == 3 && !memcmp(line, "req", 3))
        {
            if (cap->have_req || unescape_value(value, value_len, &cap->req, &cap->req_len))
                return -1;
            cap->have_req = 1;
        }
        else if (name_len == 4 && !memcmp(line, "resp", 4))
        {
            if (cap->have_resp || unescape_value(value, value_len, &cap->resp, &cap->resp_len))
                return -1;
            cap->have_resp = 1;
        }
        else if (name_len == 11 && !memcmp(line, "first_chunk", 11))
        {
            if (cap->have_first || unescape_value(value, value_len, &cap->first, &cap->first_len))
                return -1;
            cap->have_first = 1;
        }
        else if (name_len == 7 && !memcmp(line, "req_len", 7))
        {
            if (cap->have_req_len || parse_decimal(value, value_len, &cap->req_declared))
                return -1;
            cap->have_req_len = 1;
        }
        else if (name_len == 8 && !memcmp(line, "resp_len", 8))
        {
            if (cap->have_resp_len || parse_decimal(value, value_len, &cap->resp_declared))
                return -1;
            cap->have_resp_len = 1;
        }
    }
    return cap->have_req ? 0 : -1;
}

static void check_first_chunk(const u8 *first, size_t len)
{
    struct tbt_http_chunk_parser p = {};
    const u8 *b = NULL;
    size_t bl = 0, n, size_line = 0;
    int err;

    if (!len)
    {
        skip("first chunk", "empty in this capture");
        return;
    }
    n = tbt_http_chunk_feed(&p, first, len, &b, &bl, &err);
    if (n > len)
    {
        check(0, "first chunk consumed", NULL);
        return;
    }
    while (size_line + 1 < len && !(first[size_line] == '\r' && first[size_line + 1] == '\n'))
        size_line++;
    if (size_line + 1 >= len)
    {
        check(0, "first chunk size line", "no CRLF");
        return;
    }
    size_line += 2;
    check(p.chunk_size > 0, "first chunk size line parsed", NULL);

    if (p.chunk_size == 1)
    {
        check(err == TBT_HTTP_OK && bl == 1 && p.state == TBT_HTTP_CHUNK_DATA_END,
              "first chunk is a complete drip", NULL);
        {
            struct tbt_http_rx rx = {};
            struct tbt_rec rec;
            size_t used = tbt_http_frame_decode(&rx, first, len, &rec, &err);

            check(err == TBT_RX_NEED_MORE && used == len, "drip chunk is discarded", NULL);
        }
        return;
    }
    if (p.chunk_size < TBT_HDR_LEN)
    {
        check(0, "first chunk size", "2..31 is a framing error");
        return;
    }
    if (len < size_line + TBT_HDR_LEN)
    {
        skip("first chunk record header", "prefix shorter than 32 bytes");
        return;
    }
    {
        struct tbt_hdr h;

        tbt_hdr_decode(first + size_line, &h);
        check(tbt_hdr_check(&h) == 0, "first chunk record header valid", NULL);
        check((size_t)TBT_HDR_LEN + h.body_len == p.chunk_size,
              "first chunk carries exactly one record", NULL);
    }
    if (p.chunk_size == TBT_HDR_LEN)
    {
        check(size_line + TBT_HDR_LEN + 2 == TBT_HTTP_PAD_CHUNK_LEN,
              "first PAD chunk wire length 38", NULL);
        check(first[size_line] == TBT_F_PAD, "first chunk is a PAD frame", NULL);
        {
            struct tbt_http_rx rx = {};
            struct tbt_rec rec;
            size_t used = tbt_http_frame_decode(&rx, first, len, &rec, &err);

            check(err == TBT_RX_OK && used == len && rec.hdr.type == TBT_F_PAD,
                  "first PAD record decodes", NULL);
        }
        return;
    }
    if (len >= size_line + p.chunk_size)
    {
        struct tbt_http_rx rx = {};
        struct tbt_rec rec;
        size_t used = tbt_http_frame_decode(&rx, first, len, &rec, &err);

        check(err == TBT_RX_OK && used > 0, "first record decodes", NULL);
    }
    else
    {
        skip("first record body", "proc snapshot has only a prefix");
    }
}

static int run_text(const u8 *buf, size_t len)
{
    struct text_capture cap;
    struct tbt_http_req rq;
    struct tbt_http_resp rp;
    size_t hlen;
    int err;

    if (parse_text(buf, len, &cap))
    {
        check(0, "text capture parses", "missing req field");
        return -1;
    }
    printf("test_http_live: format=text\n");

    if (cap.have_req_len)
        check(cap.req_declared == cap.req_len, "req_len matches the unescaped request", NULL);
    else
        skip("req_len", "field missing");

    hlen = tbt_http_req_parse((const char *)cap.req, cap.req_len, &rq, &err);
    check(err == TBT_HTTP_OK && hlen == cap.req_len, "request header parses", NULL);
    if (err == TBT_HTTP_OK)
        check_req_header(cap.req, rq.header_len);
    else
        check(0, "request header checks", "parser refused");

    if (cap.have_resp)
    {
        if (cap.have_resp_len)
            check(cap.resp_declared == cap.resp_len, "resp_len matches the unescaped response",
                  NULL);
        else
            skip("resp_len", "field missing");

        hlen = tbt_http_resp_parse((const char *)cap.resp, cap.resp_len, &rp, &err);
        check(err == TBT_HTTP_OK && hlen == cap.resp_len, "response header parses", NULL);
        if (err == TBT_HTTP_OK)
        {
            check(rp.status == 200 && rp.chunked && rp.keep_alive,
                  "response 200/chunked/keep-alive", NULL);
            check_resp_header(cap.resp, rp.header_len);
        }
        else
        {
            check(0, "response header checks", "parser refused");
        }
        skip("response body", "proc snapshot captures headers only");
    }
    else
    {
        skip("response header", "field missing");
        skip("response body", "field missing");
    }

    if (cap.have_first)
        check_first_chunk(cap.first, cap.first_len);
    else
        skip("first chunk", "field missing");
    skip("request body beyond first_chunk", "proc snapshot captures first_chunk only");

    free(cap.req);
    free(cap.resp);
    free(cap.first);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Binary capture.                                                            */

struct bin_capture
{
    const u8 *req;
    size_t req_len;
    const u8 *resp;
    size_t resp_len;
};

static int parse_binary(const u8 *buf, size_t len, struct bin_capture *cap)
{
    size_t pos = 0, line_len, n;
    const char *line;

    memset(cap, 0, sizeof(*cap));
    if (!next_line(buf, len, &pos, &line, &line_len) ||
        !line_is(line, line_len, "TBT-HTTP-CAPTURE 1"))
        return -1;
    if (!next_line(buf, len, &pos, &line, &line_len) || line_len < 8 ||
        memcmp(line, "request ", 8))
        return -1;
    if (parse_decimal(line + 8, line_len - 8, &n) || n > len - pos)
        return -1;
    cap->req = buf + pos;
    cap->req_len = n;
    pos += n;
    if (!next_line(buf, len, &pos, &line, &line_len) || line_len < 9 ||
        memcmp(line, "response ", 9))
        return -1;
    if (parse_decimal(line + 9, line_len - 9, &n) || n > len - pos)
        return -1;
    cap->resp = buf + pos;
    cap->resp_len = n;
    pos += n;
    while (pos < len && (buf[pos] == '\r' || buf[pos] == '\n' || buf[pos] == ' ' || buf[pos] == '\t'))
        pos++;
    return pos == len ? 0 : -1;
}

static int run_binary(const u8 *buf, size_t len)
{
    struct bin_capture cap;
    struct tbt_http_req rq;
    struct tbt_http_resp rp;
    size_t hlen, scan_records = 0;
    int err;

    if (parse_binary(buf, len, &cap))
    {
        check(0, "binary capture parses", "bad magic or framing");
        return -1;
    }
    printf("test_http_live: format=binary\n");

    hlen = tbt_http_req_parse((const char *)cap.req, cap.req_len, &rq, &err);
    check(err == TBT_HTTP_OK && hlen == rq.header_len && hlen <= cap.req_len,
          "request header parses", NULL);
    if (err == TBT_HTTP_OK)
    {
        check(rq.chunked && rq.keep_alive, "request chunked/keep-alive", NULL);
        check_req_header(cap.req, rq.header_len);
        scan_chunks("request body", cap.req + hlen, cap.req_len - hlen, 0, 0, &scan_records);
        decode_records("request body", cap.req + hlen, cap.req_len - hlen, scan_records);
    }

    hlen = tbt_http_resp_parse((const char *)cap.resp, cap.resp_len, &rp, &err);
    check(err == TBT_HTTP_OK && hlen == rp.header_len && hlen <= cap.resp_len,
          "response header parses", NULL);
    if (err == TBT_HTTP_OK)
    {
        check(rp.status == 200 && rp.chunked && rp.keep_alive,
              "response 200/chunked/keep-alive", NULL);
        check_resp_header(cap.resp, rp.header_len);
        scan_chunks("response body", cap.resp + hlen, cap.resp_len - hlen, 0, 0, &scan_records);
        decode_records("response body", cap.resp + hlen, cap.resp_len - hlen, scan_records);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Current proc surface: raw request header block only.                       */

static u8 *normalize_raw(const u8 *buf, size_t len, size_t *out_len)
{
    size_t i, end = 0;

    for (i = 0; i + 3 < len; i++)
    {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
            buf[i + 3] == '\n')
        {
            end = i + 4;
            break;
        }
    }
    if (end)
    {
        u8 *out = malloc(end);

        if (!out)
            return NULL;
        memcpy(out, buf, end);
        *out_len = end;
        return out;
    }

    /* Console capture: CR and LF may have produced blank lines between the
     * header lines. Drop empty lines and rebuild the block with CRLF. */
    {
        size_t pos = 0, line_len, cap = len * 2 + 4, o = 0;
        const char *line;
        u8 *out = malloc(cap);

        if (!out)
            return NULL;
        while (next_line(buf, len, &pos, &line, &line_len))
        {
            if (!line_len)
                continue;
            if (o + line_len + 2 > cap)
            {
                free(out);
                return NULL;
            }
            memcpy(out + o, line, line_len);
            o += line_len;
            out[o++] = '\r';
            out[o++] = '\n';
        }
        if (!o || o + 2 > cap)
        {
            free(out);
            return NULL;
        }
        out[o++] = '\r';
        out[o++] = '\n';
        *out_len = o;
        return out;
    }
}

static int run_raw_header(const u8 *buf, size_t len)
{
    struct tbt_http_req rq;
    u8 *header;
    size_t hdr_len = 0, hlen;
    int err;

    printf("test_http_live: format=proc-raw\n");
    header = normalize_raw(buf, len, &hdr_len);
    if (!header || !hdr_len)
    {
        check(0, "raw request header present", "no header block");
        free(header);
        return -1;
    }
    hlen = tbt_http_req_parse((const char *)header, hdr_len, &rq, &err);
    check(err == TBT_HTTP_OK && hlen == hdr_len, "request header parses", NULL);
    if (err == TBT_HTTP_OK)
        check_req_header(header, rq.header_len);
    else
        check(0, "request header checks", "parser refused");

    skip("response header", "current proc surface writes request headers only");
    skip("first chunk", "current proc surface writes request headers only");
    skip("response body", "current proc surface writes request headers only");
    free(header);
    return 0;
}

static int looks_like_text(const u8 *buf, size_t len)
{
    size_t pos = 0, line_len;
    const char *line;

    while (next_line(buf, len, &pos, &line, &line_len))
    {
        if (line_len >= 4 && !memcmp(line, "req=", 4))
            return 1;
    }
    return 0;
}

static int run_capture(const u8 *buf, size_t len)
{
    if (len >= 17 && !memcmp(buf, "TBT-HTTP-CAPTURE 1", 17))
        return run_binary(buf, len);
    if (looks_like_text(buf, len))
        return run_text(buf, len);
    if (len >= 5 && !memcmp(buf, "POST ", 5))
        return run_raw_header(buf, len);
    check(0, "capture format", "not binary, frozen text, or a raw request header");
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Entry.                                                                     */

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [--from-proc [path]]\n", argv0);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    FILE *f = stdin;
    size_t len = 0;
    u8 *buf;
    int rc;

    if (argc == 2 && !strcmp(argv[1], "--from-proc"))
        path = "/proc/net/tbtun/http";
    else if (argc == 3 && !strcmp(argv[1], "--from-proc"))
        path = argv[2];
    else if (argc != 1)
    {
        usage(argv[0]);
        return 2;
    }
    if (path)
    {
        f = fopen(path, "rb");
        if (!f)
        {
            perror(path);
            return 2;
        }
    }
    buf = read_all(f, &len);
    if (path)
        fclose(f);
    if (!buf)
    {
        printf("FAIL read input\n");
        return 1;
    }
    rc = run_capture(buf, len);
    printf("test_http_live: checks=%d failures=%d skips=%d\n", checks, failures, skips);
    free(buf);
    return failures ? 1 : (rc ? 1 : 0);
}
