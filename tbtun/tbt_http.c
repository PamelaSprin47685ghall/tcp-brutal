/* tbt_http.c - the HTTP/1.1 QoS carrier container (DESIGN.md §3).
 *
 * Pure code: the same object serves the module and host/test_http.c. There is
 * no allocation, no static mutable state, no timer and no kernel facility
 * here; tbt_carrier.c owns the clock, the socket and the policy.
 *
 * The wire shape is DESIGN.md §3.2 (request and response headers), §3.3 (body
 * framing) and §3.7 (PAD cadence). The far side's request-header validation
 * lives here too, so the near side's emitter and the far side's gate share one
 * definition of the accepted block. The rules that §3 leaves open are decided in the "implementation
 * ruling" notes at the end of §3.2 and §3.3 of DESIGN.md and repeated at the
 * decision point below, so a reader of this file does not have to hold two
 * documents in mind.
 */
#include "tbt_http.h"

#include "tbt_proto.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#define TBT_HTTP_REQ_LINE_NOCRLF "POST " TBT_HTTP_PATH " HTTP/1.1"
#define TBT_HTTP_REQ_LINE TBT_HTTP_REQ_LINE_NOCRLF "\r\n"
#define TBT_HTTP_NONE ((size_t)-1)

/* Append n bytes at off. A failed append is sticky: HTTP_NONE in, HTTP_NONE
 * out, so a builder can chain the calls and test once at the end. cap is
 * checked before every write, which is what makes a short buffer a clean 0
 * instead of a truncated header. */
static size_t http_put(char *buf, size_t cap, size_t off, const char *src, size_t n)
{
    if (off == TBT_HTTP_NONE)
        return TBT_HTTP_NONE;
    if (off > cap || n > cap - off)
        return TBT_HTTP_NONE;
    memcpy(buf + off, src, n);
    return off + n;
}

static size_t http_puts(char *buf, size_t cap, size_t off, const char *src)
{
    return http_put(buf, cap, off, src, strlen(src));
}

/* A canonical IPv4 literal with an optional ":port", which is what §3.2 means
 * by "peer address literal". Rejecting hostnames and IPv6 here is deliberate:
 * the module only carries IPv4 (tbt_parse.c refuses IPv6), and a Host header
 * that does not match the connection target is worse than no carrier. Leading
 * zeros are rejected so one address has exactly one spelling. */
static bool http_host_ok(const char *h)
{
    unsigned octet = 0, port = 0;
    unsigned digits = 0, port_digits = 0, dots = 0;
    bool in_port = false;
    size_t i;

    for (i = 0; i <= TBT_HTTP_HOST_MAX; i++)
    {
        char c = h[i];

        if (!c)
            break;
        if (in_port)
        {
            if (c < '0' || c > '9')
                return false;
            if (port_digits == 1 && port == 0)
                return false; /* "0..." beyond the single zero */
            if (port_digits >= 5)
                return false;
            port = port * 10 + (unsigned)(c - '0');
            port_digits++;
            if (port > 65535)
                return false;
            continue;
        }
        if (c == ':')
        {
            if (dots != 3 || digits == 0 || octet > 255)
                return false;
            in_port = true;
            continue;
        }
        if (c == '.')
        {
            if (digits == 0 || octet > 255)
                return false;
            dots++;
            if (dots > 3)
                return false;
            octet = 0;
            digits = 0;
            continue;
        }
        if (c < '0' || c > '9')
            return false;
        if (digits == 3)
            return false;
        if (digits == 1 && octet == 0)
            return false; /* a leading zero, e.g. "01" */
        octet = octet * 10 + (unsigned)(c - '0');
        digits++;
        if (octet > 255)
            return false;
    }

    if (i > TBT_HTTP_HOST_MAX)
        return false; /* no NUL inside the field */
    if (in_port)
        return port_digits > 0 && port > 0;
    return dots == 3 && digits > 0 && octet <= 255;
}

/* Exactly 26 characters from the lowercase base32 alphabet [a-z2-7]. The
 * alphabet is RFC 4648's, lowercased; "lowercase base32" in §3.2 cannot mean
 * letters only, because base32 is not an alphabet of 26 letters. */
static bool http_reqid_ok(const char *id)
{
    size_t i;

    for (i = 0; i < TBT_HTTP_REQID_LEN; i++)
    {
        char c = id[i];

        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7')))
            return false;
    }
    return id[TBT_HTTP_REQID_LEN] == '\0';
}

size_t tbt_http_req_header(char *buf, size_t cap, const struct tbt_http_cfg *cfg)
{
    size_t off = 0;

    if (!buf || !cfg || !http_host_ok(cfg->host) || !http_reqid_ok(cfg->request_id))
        return 0;

    off = http_puts(buf, cap, off, TBT_HTTP_REQ_LINE);
    off = http_puts(buf, cap, off, "Host: ");
    off = http_puts(buf, cap, off, cfg->host);
    off = http_puts(buf, cap, off, "\r\n");
    off = http_puts(buf, cap, off, "User-Agent: " TBT_HTTP_USER_AGENT "\r\n");
    off = http_puts(buf, cap, off, "Accept: */*\r\n");
    off = http_puts(buf, cap, off, "Accept-Encoding: identity\r\n");
    off = http_puts(buf, cap, off, "Content-Type: " TBT_HTTP_CONTENT_TYPE "\r\n");
    off = http_puts(buf, cap, off, "Transfer-Encoding: chunked\r\n");
    off = http_puts(buf, cap, off, "Connection: keep-alive\r\n");
    off = http_puts(buf, cap, off, "X-Request-Id: ");
    off = http_puts(buf, cap, off, cfg->request_id);
    off = http_puts(buf, cap, off, "\r\n\r\n");

    return off == TBT_HTTP_NONE || off > TBT_HTTP_REQ_MAX ? 0 : off;
}

/* ------------------------------------------------------------------------- */
/* IMF-fixdate and the response header block.                                 */

static const char http_wday[7][4] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char http_mon[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void http_put2(char *p, unsigned v)
{
    p[0] = (char)('0' + v / 10);
    p[1] = (char)('0' + v % 10);
}

static void http_put4(char *p, unsigned v)
{
    p[0] = (char)('0' + (v / 1000) % 10);
    p[1] = (char)('0' + (v / 100) % 10);
    p[2] = (char)('0' + (v / 10) % 10);
    p[3] = (char)('0' + v % 10);
}

size_t tbt_http_http_date(char *buf, size_t cap, u64 unix_secs)
{
    u64 days, rem, z, era, y_;
    unsigned doe, yoe, doy, mp, day, month, year, wday, hour, min, sec;

    if (!buf || cap < TBT_HTTP_DATE_LEN)
        return 0;
    if (unix_secs > 253402300799ULL) /* 9999-12-31 23:59:59 UTC */
        return 0;

    days = unix_secs / 86400;
    rem = unix_secs % 86400;
    wday = (unsigned)((days + 4) % 7); /* 1970-01-01 was a Thursday */
    hour = (unsigned)(rem / 3600);
    min = (unsigned)((rem % 3600) / 60);
    sec = (unsigned)(rem % 60);

    /* Howard Hinnant's civil_from_days, with the epoch shifted to 1970-01-01.
     * days >= 0 because unix_secs >= 0, so the negative branch is omitted. */
    z = days + 719468;
    era = z / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y_ = (u64)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = (unsigned)(y_ + (month <= 2));

    memcpy(buf, http_wday[wday], 3);
    buf[3] = ',';
    buf[4] = ' ';
    http_put2(buf + 5, day);
    buf[7] = ' ';
    memcpy(buf + 8, http_mon[month - 1], 3);
    buf[11] = ' ';
    http_put4(buf + 12, year);
    buf[16] = ' ';
    http_put2(buf + 17, hour);
    buf[19] = ':';
    http_put2(buf + 20, min);
    buf[22] = ':';
    http_put2(buf + 23, sec);
    memcpy(buf + 25, " GMT", 4);
    return TBT_HTTP_DATE_LEN;
}

size_t tbt_http_resp_header(char *buf, size_t cap, const struct tbt_http_resp_cfg *cfg)
{
    char date[TBT_HTTP_DATE_LEN];
    size_t off = 0;

    if (!buf || !cfg || !tbt_http_http_date(date, sizeof(date), cfg->unix_secs))
        return 0;

    off = http_puts(buf, cap, off, "HTTP/1.1 200 OK\r\n");
    off = http_puts(buf, cap, off, "Content-Type: " TBT_HTTP_CONTENT_TYPE "\r\n");
    off = http_puts(buf, cap, off, "Transfer-Encoding: chunked\r\n");
    off = http_puts(buf, cap, off, "Connection: keep-alive\r\n");
    off = http_puts(buf, cap, off, "Server: nginx\r\n");
    off = http_puts(buf, cap, off, "Date: ");
    off = http_put(buf, cap, off, date, TBT_HTTP_DATE_LEN);
    off = http_puts(buf, cap, off, "\r\n\r\n");

    return off == TBT_HTTP_NONE || off > TBT_HTTP_RESP_MAX ? 0 : off;
}

/* ------------------------------------------------------------------------- */
/* Response status line and headers.                                          */

struct http_resp_state
{
    struct tbt_http_resp pub;
    bool te_seen;
    bool connection_keepalive;
};

static bool http_tchar(char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
        return true;
    switch (c)
    {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

static char http_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

/* Case-insensitive compare of a counted slice with a NUL-terminated literal. */
static bool http_ieq(const char *s, size_t n, const char *lit)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        if (!lit[i] || http_lower(s[i]) != http_lower(lit[i]))
            return false;
    }
    return lit[n] == '\0';
}

/* Locate the next CRLF-terminated line. `max` bounds the bytes from `from`
 * that this line plus its CRLF may occupy, which is how a hostile peer cannot
 * make the parser scan a whole receive buffer for a terminator that never
 * comes. Returns OK, NEED_MORE or PROTO; on OK *end is the CR offset and
 * *next the offset after the LF. A bare LF is always PROTO. */
static int http_line(const char *buf, size_t len, size_t from, size_t max, size_t *end,
                     size_t *next)
{
    size_t avail;
    size_t scan;
    size_t i;

    if (from > len)
        return TBT_HTTP_PROTO;
    avail = len - from;
    scan = avail < max ? avail : max;

    for (i = 0; i < scan; i++)
    {
        char c = buf[from + i];

        if (c == '\n')
            return TBT_HTTP_PROTO;
        if (c != '\r')
            continue;
        if (i + 2 > max)
            return TBT_HTTP_PROTO;
        if (i + 1 >= avail)
            return TBT_HTTP_NEED_MORE;
        if (buf[from + i + 1] != '\n')
            return TBT_HTTP_PROTO;
        *end = from + i;
        *next = from + i + 2;
        return TBT_HTTP_OK;
    }
    return avail < max ? TBT_HTTP_NEED_MORE : TBT_HTTP_PROTO;
}

/* HTTP-version SP status-code [ SP reason-phrase ]. The reason phrase is
 * ignored; the code must be exactly three digits and exactly 200. */
static bool http_status_line(const char *line, size_t n, u16 *status, u8 *minor)
{
    size_t i;

    if (n < 12 || memcmp(line, "HTTP/1.", 7) != 0)
        return false;
    if (line[7] != '0' && line[7] != '1')
        return false;
    if (line[8] != ' ')
        return false;
    for (i = 9; i < 12; i++)
    {
        if (line[i] < '0' || line[i] > '9')
            return false;
    }
    if (n > 12 && line[12] != ' ')
        return false;
    for (i = 13; i < n; i++)
    {
        unsigned char c = (unsigned char)line[i];

        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return false;
    }

    *status = (u16)((line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0'));
    *minor = (u8)(line[7] - '0');
    return true;
}

/* Comma-separated Connection tokens. Only the two tokens this design can act
 * on are recognised; anything else is ignored, as RFC 9110 permits. */
static void http_connection_value(const char *val, size_t n, struct http_resp_state *st)
{
    size_t i = 0;

    while (i < n)
    {
        size_t start, end;

        while (i < n && (val[i] == ' ' || val[i] == '\t' || val[i] == ','))
            i++;
        start = i;
        while (i < n && val[i] != ',')
            i++;
        end = i;
        while (end > start && (val[end - 1] == ' ' || val[end - 1] == '\t'))
            end--;

        if (http_ieq(val + start, end - start, "close"))
            st->pub.connection_close = true;
        else if (http_ieq(val + start, end - start, "keep-alive"))
            st->connection_keepalive = true;
    }
}

/* Split one header line (already stripped of its CRLF) into a token name and a
 * trimmed value. Rejects a name that is not a token, whitespace before the
 * colon, obs-fold and control bytes in the value. Shared by the response and
 * request parsers so both enforce the same syntax. */
static bool http_header_split(const char *line, size_t n, const char **name, size_t *name_len,
                              const char **val, size_t *val_len)
{
    size_t colon, v, ve, i;

    for (colon = 0; colon < n && line[colon] != ':'; colon++)
    {
        if (!http_tchar(line[colon]))
            return false;
    }
    if (colon == 0 || colon == n)
        return false;

    for (v = colon + 1; v < n && (line[v] == ' ' || line[v] == '\t'); v++)
        ;
    ve = n;
    while (ve > v && (line[ve - 1] == ' ' || line[ve - 1] == '\t'))
        ve--;
    for (i = v; i < ve; i++)
    {
        unsigned char c = (unsigned char)line[i];

        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return false;
    }

    *name = line;
    *name_len = colon;
    *val = line + v;
    *val_len = ve - v;
    return true;
}

/* One response header line. Acts only on the four headers this design reads;
 * everything else is skipped. */
static bool http_header_line(const char *line, size_t n, struct http_resp_state *st)
{
    const char *name, *val;
    size_t name_len, val_len, i;

    if (!http_header_split(line, n, &name, &name_len, &val, &val_len))
        return false;

    if (http_ieq(name, name_len, "transfer-encoding"))
    {
        if (st->te_seen)
            return false;
        st->te_seen = true;
        if (!http_ieq(val, val_len, "chunked"))
            return false;
        st->pub.chunked = true;
    }
    else if (http_ieq(name, name_len, "content-length"))
    {
        if (st->pub.has_content_length || val_len == 0)
            return false;
        for (i = 0; i < val_len; i++)
        {
            if (val[i] < '0' || val[i] > '9')
                return false;
        }
        st->pub.has_content_length = true;
    }
    else if (http_ieq(name, name_len, "connection"))
    {
        http_connection_value(val, val_len, st);
    }
    return true;
}

size_t tbt_http_resp_parse(const char *buf, size_t len, struct tbt_http_resp *out, int *err)
{
    struct http_resp_state st = {};
    u8 minor = 0;
    size_t pos, end, next;
    int rc;

    if (err)
        *err = TBT_HTTP_NEED_MORE;
    if (!buf || !out)
    {
        if (err)
            *err = TBT_HTTP_PROTO;
        return 0;
    }

    rc = http_line(buf, len, 0, TBT_HTTP_HDR_MAX, &end, &next);
    if (rc != TBT_HTTP_OK)
    {
        if (err)
            *err = rc;
        return 0;
    }
    if (!http_status_line(buf, end, &st.pub.status, &minor) || st.pub.status != 200)
    {
        if (err)
            *err = TBT_HTTP_PROTO;
        return 0;
    }

    pos = next;
    for (;;)
    {
        size_t budget = pos < TBT_HTTP_HDR_MAX ? TBT_HTTP_HDR_MAX - pos : 0;

        rc = http_line(buf, len, pos, budget, &end, &next);
        if (rc != TBT_HTTP_OK)
        {
            if (err)
                *err = rc;
            return 0;
        }
        if (end == pos)
        {
            pos = next;
            break;
        }
        if (!http_header_line(buf + pos, end - pos, &st))
        {
            if (err)
                *err = TBT_HTTP_PROTO;
            return 0;
        }
        pos = next;
    }

    /* RFC 9112 §6.3: a message with both is ambiguous and must be rejected. */
    if (st.pub.chunked && st.pub.has_content_length)
    {
        if (err)
            *err = TBT_HTTP_PROTO;
        return 0;
    }

    st.pub.keep_alive = !st.pub.connection_close && (minor == 1 || st.connection_keepalive);
    st.pub.header_len = pos;
    *out = st.pub;
    if (err)
        *err = TBT_HTTP_OK;
    return pos;
}

/* ------------------------------------------------------------------------- */
/* Request header validation.                                                 */

size_t tbt_http_req_parse(const char *buf, size_t len, struct tbt_http_req *out, int *err)
{
    static const char *const names[8] = {
        "Host", "User-Agent", "Accept", "Accept-Encoding",
        "Content-Type", "Transfer-Encoding", "Connection", "X-Request-Id",
    };
    static const char *const values[8] = {
        NULL, TBT_HTTP_USER_AGENT, "*/*", "identity",
        TBT_HTTP_CONTENT_TYPE, "chunked", "keep-alive", NULL,
    };
    struct tbt_http_req r = {};
    size_t pos, end, next;
    unsigned h;
    int rc;

    if (err)
        *err = TBT_HTTP_NEED_MORE;
    if (!buf || !out)
    {
        if (err)
            *err = TBT_HTTP_PROTO;
        return 0;
    }

    rc = http_line(buf, len, 0, TBT_HTTP_HDR_MAX, &end, &next);
    if (rc != TBT_HTTP_OK)
    {
        if (err)
            *err = rc;
        return 0;
    }
    if (end != sizeof(TBT_HTTP_REQ_LINE_NOCRLF) - 1 ||
        memcmp(buf, TBT_HTTP_REQ_LINE_NOCRLF, end) != 0)
    {
        if (err)
            *err = TBT_HTTP_PROTO;
        return 0;
    }
    pos = next;

    for (h = 0; h < 8; h++)
    {
        const char *name, *val;
        size_t name_len, val_len;
        size_t budget = pos < TBT_HTTP_HDR_MAX ? TBT_HTTP_HDR_MAX - pos : 0;

        rc = http_line(buf, len, pos, budget, &end, &next);
        if (rc != TBT_HTTP_OK)
        {
            if (err)
                *err = rc;
            return 0;
        }
        if (end == pos)
        {
            if (err)
                *err = TBT_HTTP_PROTO;
            return 0;
        }
        if (!http_header_split(buf + pos, end - pos, &name, &name_len, &val, &val_len) ||
            name_len != strlen(names[h]) || memcmp(name, names[h], name_len) != 0)
        {
            if (err)
                *err = TBT_HTTP_PROTO;
            return 0;
        }

        if (h == 0)
        {
            char host[TBT_HTTP_HOST_MAX + 1];

            if (val_len > TBT_HTTP_HOST_MAX)
            {
                if (err)
                    *err = TBT_HTTP_PROTO;
                return 0;
            }
            memcpy(host, val, val_len);
            host[val_len] = '\0';
            if (!http_host_ok(host))
            {
                if (err)
                    *err = TBT_HTTP_PROTO;
                return 0;
            }
            r.host = val;
            r.host_len = val_len;
        }
        else if (h == 7)
        {
            char id[TBT_HTTP_REQID_LEN + 1];

            if (val_len != TBT_HTTP_REQID_LEN)
            {
                if (err)
                    *err = TBT_HTTP_PROTO;
                return 0;
            }
            memcpy(id, val, val_len);
            id[val_len] = '\0';
            if (!http_reqid_ok(id))
            {
                if (err)
                    *err = TBT_HTTP_PROTO;
                return 0;
            }
        }
        else if (val_len != strlen(values[h]) || memcmp(val, values[h], val_len) != 0)
        {
            if (err)
                *err = TBT_HTTP_PROTO;
            return 0;
        }
        pos = next;
    }

    /* Exactly eight headers: the next line must be the blank line, not a
     * ninth header and not the body. */
    {
        size_t budget = pos < TBT_HTTP_HDR_MAX ? TBT_HTTP_HDR_MAX - pos : 0;

        rc = http_line(buf, len, pos, budget, &end, &next);
        if (rc != TBT_HTTP_OK)
        {
            if (err)
                *err = rc;
            return 0;
        }
        if (end != pos)
        {
            if (err)
                *err = TBT_HTTP_PROTO;
            return 0;
        }
        pos = next;
    }

    r.chunked = true;
    r.keep_alive = true;
    r.header_len = pos;
    *out = r;
    if (err)
        *err = TBT_HTTP_OK;
    return pos;
}

/* ------------------------------------------------------------------------- */
/* Request-header skip and message bookkeeping.                               */

size_t tbt_http_skip_request_header(const u8 *buf, size_t len, int *err)
{
    size_t limit, i;

    if (err)
        *err = TBT_HTTP_NEED_MORE;
    if (!buf && len)
    {
        if (err)
            *err = TBT_HTTP_PROTO;
        return 0;
    }

    limit = len < TBT_HTTP_HDR_MAX ? len : TBT_HTTP_HDR_MAX;
    for (i = 0; i + 3 < limit; i++)
    {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
        {
            if (err)
                *err = TBT_HTTP_OK;
            return i + 4;
        }
    }
    if (len >= TBT_HTTP_HDR_MAX && err)
        *err = TBT_HTTP_PROTO;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Chunk encoding.                                                            */

size_t tbt_http_chunk_hdr(char *buf, size_t cap, size_t body_len)
{
    static const char hex[] = "0123456789abcdef";
    char tmp[TBT_HTTP_CHUNK_HDR_MAX];
    size_t n = 0, i;

    if (!buf || body_len > TBT_FEC_REC_MAX)
        return 0;

    /* Lowercase hex, most significant digit first, no leading zeros. */
    do
    {
        tmp[n++] = hex[body_len & 0xf];
        body_len >>= 4;
    } while (body_len);
    if (n + 2 > cap)
        return 0;
    for (i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = '\r';
    buf[n + 1] = '\n';
    return n + 2;
}

size_t tbt_http_chunk_tail(char *buf, size_t cap)
{
    if (!buf || cap < 2)
        return 0;
    buf[0] = '\r';
    buf[1] = '\n';
    return 2;
}

size_t tbt_http_pad_chunk(char *buf, size_t cap)
{
    struct tbt_hdr h = {};
    size_t n;

    if (!buf || cap < TBT_HTTP_PAD_CHUNK_LEN)
        return 0;

    n = tbt_http_chunk_hdr(buf, cap, TBT_HTTP_PAD_DATA_LEN);
    if (!n)
        return 0;
    h.type = TBT_F_PAD;
    h.body_len = 0;
    h.conv = TBT_CONV;
    tbt_hdr_encode((u8 *)buf + n, &h);
    if (!tbt_http_chunk_tail(buf + n + TBT_HTTP_PAD_DATA_LEN, cap - n - TBT_HTTP_PAD_DATA_LEN))
        return 0;
    return n + TBT_HTTP_PAD_DATA_LEN + 2;
}

size_t tbt_http_drip_chunk(char *buf, size_t cap)
{
    static const char drip[] = "1\r\n#\r\n"; /* '#' is TBT_F_PAD's type byte */

    if (!buf || cap < TBT_HTTP_DRIP_CHUNK_LEN)
        return 0;
    memcpy(buf, drip, sizeof(drip) - 1);
    return sizeof(drip) - 1;
}

/* ------------------------------------------------------------------------- */
/* Chunk decoding.                                                            */

void tbt_http_chunk_reset(struct tbt_http_chunk_parser *p)
{
    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    p->state = TBT_HTTP_CHUNK_SIZE;
}

void tbt_http_msg_reset(struct tbt_http_msg *m)
{
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
    tbt_http_chunk_reset(&m->chunk);
}

/* -1 for a non-hex byte, else its value. Uppercase is accepted on receive:
 * RFC 9112 defines chunk-size as 1*HEXDIG, and a real sender may use either
 * case. The encoder only ever emits lowercase. */
static int http_hexval(u8 c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

size_t tbt_http_chunk_feed(struct tbt_http_chunk_parser *p, const u8 *buf, size_t len,
                           const u8 **out_body, size_t *out_body_len, int *err)
{
    size_t i = 0;

    if (out_body)
        *out_body = NULL;
    if (out_body_len)
        *out_body_len = 0;
    if (!p || !out_body || !out_body_len || (!buf && len))
    {
        if (err)
            *err = TBT_HTTP_PROTO;
        return 0;
    }
    if (p->state == TBT_HTTP_CHUNK_BAD)
    {
        if (err)
            *err = TBT_HTTP_PROTO;
        return 0;
    }
    if (p->state == TBT_HTTP_CHUNK_DONE)
    {
        if (err)
            *err = TBT_HTTP_OK;
        return 0;
    }
    if (err)
        *err = TBT_HTTP_NEED_MORE;

    while (i < len)
    {
        u8 c = buf[i];

        switch (p->state)
        {
        case TBT_HTTP_CHUNK_SIZE:
            if (p->line_bytes >= TBT_HTTP_CHUNK_LINE_MAX)
                goto bad;
            p->line_bytes++;
            if (c == '\r')
            {
                if (!p->digits)
                    goto bad;
                i++;
                p->state = TBT_HTTP_CHUNK_SIZE_END;
                break;
            }
            if (c == ';')
            {
                if (!p->digits)
                    goto bad;
                i++;
                p->state = TBT_HTTP_CHUNK_EXT;
                break;
            }
            {
                int v = http_hexval(c);

                if (v < 0)
                    goto bad;
                p->size = p->size * 16 + (u32)v;
                if (p->size > TBT_FEC_REC_MAX)
                    goto bad;
                p->digits++;
                i++;
            }
            break;

        case TBT_HTTP_CHUNK_SIZE_END:
            if (c != '\n')
                goto bad;
            i++;
            p->chunk_size = p->size;
            if (!p->size)
            {
                p->state = TBT_HTTP_CHUNK_TRAILER;
                p->trailer_bytes = 0;
            }
            else
            {
                p->remaining = p->size;
                p->state = TBT_HTTP_CHUNK_DATA;
            }
            p->size = 0;
            p->digits = 0;
            p->line_bytes = 0;
            break;

        case TBT_HTTP_CHUNK_EXT:
            if (p->line_bytes >= TBT_HTTP_CHUNK_LINE_MAX)
                goto bad;
            p->line_bytes++;
            if (c == '\r')
            {
                i++;
                p->state = TBT_HTTP_CHUNK_EXT_END;
            }
            else if (c == '\n')
            {
                goto bad;
            }
            else
            {
                i++;
            }
            break;

        case TBT_HTTP_CHUNK_EXT_END:
            if (c != '\n')
                goto bad;
            i++;
            p->chunk_size = p->size;
            if (!p->size)
            {
                p->state = TBT_HTTP_CHUNK_TRAILER;
                p->trailer_bytes = 0;
            }
            else
            {
                p->remaining = p->size;
                p->state = TBT_HTTP_CHUNK_DATA;
            }
            p->size = 0;
            p->digits = 0;
            p->line_bytes = 0;
            break;

        case TBT_HTTP_CHUNK_DATA:
        {
            size_t n = p->remaining < len - i ? p->remaining : len - i;

            if (!n)
                goto bad;
            *out_body = buf + i;
            *out_body_len = n;
            p->remaining -= (u32)n;
            if (!p->remaining)
                p->state = TBT_HTTP_CHUNK_DATA_END;
            if (err)
                *err = TBT_HTTP_OK;
            return i + n;
        }

        case TBT_HTTP_CHUNK_DATA_END:
            if (c != '\r')
                goto bad;
            i++;
            p->state = TBT_HTTP_CHUNK_DATA_END_LF;
            break;

        case TBT_HTTP_CHUNK_DATA_END_LF:
            if (c != '\n')
                goto bad;
            i++;
            p->state = TBT_HTTP_CHUNK_SIZE;
            p->chunks++;
            break;

        case TBT_HTTP_CHUNK_TRAILER:
            if (p->trailer_bytes >= TBT_HTTP_HDR_MAX)
                goto bad;
            p->trailer_bytes++;
            if (c == '\r')
            {
                i++;
                p->state = TBT_HTTP_CHUNK_TRAILER_BLANK_LF;
            }
            else if (c == '\n')
            {
                goto bad;
            }
            else
            {
                i++;
                p->state = TBT_HTTP_CHUNK_TRAILER_LINE;
            }
            break;

        case TBT_HTTP_CHUNK_TRAILER_LINE:
            if (p->trailer_bytes >= TBT_HTTP_HDR_MAX)
                goto bad;
            p->trailer_bytes++;
            if (c == '\r')
            {
                i++;
                p->state = TBT_HTTP_CHUNK_TRAILER_LINE_LF;
            }
            else if (c == '\n')
            {
                goto bad;
            }
            else
            {
                i++;
            }
            break;

        case TBT_HTTP_CHUNK_TRAILER_LINE_LF:
            if (c != '\n')
                goto bad;
            i++;
            p->state = TBT_HTTP_CHUNK_TRAILER;
            break;

        case TBT_HTTP_CHUNK_TRAILER_BLANK_LF:
            if (c != '\n')
                goto bad;
            i++;
            p->state = TBT_HTTP_CHUNK_DONE;
            if (err)
                *err = TBT_HTTP_OK;
            return i;

        default:
            goto bad;
        }
    }

    if (err)
        *err = TBT_HTTP_NEED_MORE;
    return i;

bad:
    p->state = TBT_HTTP_CHUNK_BAD;
    if (err)
        *err = TBT_HTTP_PROTO;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Carrier profile: one chunk per record, and the receive assembler.          */

size_t tbt_http_frame_encode(char *buf, size_t cap, const struct tbt_http_out *out)
{
    struct tbt_hdr h;
    size_t rec_len, n;

    if (!buf || !out || !out->hdr)
        return 0;
    h = *out->hdr;
    if (tbt_hdr_check(&h))
        return 0;

    /* A PAD is the canonical empty frame; its other header fields do not
     * survive because the frame exists only to keep the carrier warm. */
    if (h.type == TBT_F_PAD)
    {
        if (h.body_len != 0)
            return 0;
        return tbt_http_pad_chunk(buf, cap);
    }

    rec_len = TBT_HDR_LEN + h.body_len;
    if (rec_len > TBT_REC_MAX || rec_len > TBT_CHUNK_MAX)
        return 0;
    if (h.body_len && !out->body)
        return 0;

    n = tbt_http_chunk_hdr(buf, cap, rec_len);
    if (!n || n + rec_len + 2 > cap)
        return 0;
    tbt_hdr_encode((u8 *)buf + n, &h);
    if (h.body_len)
        memcpy(buf + n + TBT_HDR_LEN, out->body, h.body_len);
    if (!tbt_http_chunk_tail(buf + n + rec_len, cap - n - rec_len))
        return 0;
    return n + rec_len + 2;
}

size_t tbt_http_fec_chunk_encode(char *buf, size_t cap, const u8 *rec,
                                 size_t rec_len)
{
    size_t n;

    if (!buf || !rec || rec_len < TBT_FEC_HDR_LEN || rec_len > TBT_FEC_REC_MAX)
        return 0;
    n = tbt_http_chunk_hdr(buf, cap, rec_len);
    if (!n || n + rec_len + 2 > cap)
        return 0;
    memcpy(buf + n, rec, rec_len);
    if (!tbt_http_chunk_tail(buf + n + rec_len, cap - n - rec_len))
        return 0;
    return n + rec_len + 2;
}

/* FEC-over-HTTP receiver: one chunk body is one 40-byte record. The HTTP
 * header block and chunk framing are parsed by the same chunk parser; the
 * 32-byte record checks in tbt_http_frame_decode do not apply. */
size_t tbt_http_fec_frame_decode(struct tbt_http_rx *rx, const u8 *buf, size_t len,
                                 const u8 **rec, size_t *rec_len, int *err)
{
    size_t used = 0;

    if (rec)
        *rec = NULL;
    if (rec_len)
        *rec_len = 0;
    if (err)
        *err = TBT_RX_NEED_MORE;
    if (!rx || !rec || !rec_len || !err || (!buf && len))
    {
        if (err)
            *err = TBT_RX_PROTO;
        return 0;
    }
    if (rx->chunk.state == TBT_HTTP_CHUNK_DONE)
        return 0;

    for (;;)
    {
        const u8 *body = NULL;
        size_t blen = 0, n;
        int cerr;

        if (used >= len)
            return used;
        n = tbt_http_chunk_feed(&rx->chunk, buf + used, len - used, &body, &blen,
                                &cerr);
        if (cerr == TBT_HTTP_PROTO)
        {
            *err = TBT_RX_PROTO;
            return 0;
        }
        if (cerr == TBT_HTTP_NEED_MORE)
            return used + n;
        if (rx->chunk.state == TBT_HTTP_CHUNK_DONE)
            return used + n;
        used += n;

        if (!rx->chunk_started)
        {
            rx->chunk_started = true;
            rx->chunk_size = rx->chunk.chunk_size;
            rx->rec_len = 0;
        }
        if (blen)
        {
            if (rx->rec_len + blen > sizeof(rx->rec))
            {
                *err = TBT_RX_PROTO;
                return 0;
            }
            memcpy(rx->rec + rx->rec_len, body, blen);
            rx->rec_len += (u32)blen;
        }
        if (rx->chunk.state == TBT_HTTP_CHUNK_DATA_END)
        {
            if (rx->rec_len == rx->chunk_size)
            {
                bool ok = false;

                if (rx->chunk_size >= TBT_FEC_HDR_LEN &&
                    tbt_fec_record_type(rx->rec[0]))
                {
                    u16 plen = (u16)(rx->rec[TBT_FEC_H_PAYLEN] |
                                     ((u16)rx->rec[TBT_FEC_H_PAYLEN + 1] << 8));

                    ok = plen <= TBT_FEC_PAYLOAD_MAX &&
                         TBT_FEC_HDR_LEN + (size_t)plen == rx->rec_len;
                }
                else if (rx->chunk_size >= TBT_HDR_LEN)
                {
                    /* HELLO/OPEN/FIN/RST and PAD keep the old 32-byte record
                     * and share the container with FEC chunks. */
                    struct tbt_hdr h;

                    tbt_hdr_decode(rx->rec, &h);
                    ok = !tbt_hdr_check(&h) &&
                         TBT_HDR_LEN + (size_t)h.body_len == rx->rec_len;
                }
                if (ok)
                {
                    *rec = rx->rec;
                    *rec_len = rx->rec_len;
                    *err = TBT_RX_OK;
                    rx->chunk_started = false;
                    rx->rec_len = 0;
                    return used;
                }
            }
            /* Drip (1 byte) is skipped; a chunk that claims a record size
             * but carries neither a valid FEC nor a valid old record is a
             * framing error and closes the carrier. */
            if (rx->chunk_size >= TBT_HDR_LEN)
            {
                *err = TBT_RX_PROTO;
                return 0;
            }
            rx->chunk_started = false;
            rx->rec_len = 0;
        }
    }
}

size_t tbt_http_frame_decode(struct tbt_http_rx *rx, const u8 *buf, size_t len,
                             struct tbt_rec *out, int *err)
{
    size_t used = 0;
    int status = TBT_RX_NEED_MORE;

    if (!rx || !out || (!buf && len))
    {
        status = TBT_RX_PROTO;
        used = 0;
        goto out;
    }
    if (rx->chunk.state == TBT_HTTP_CHUNK_DONE)
        goto out;

    for (;;)
    {
        const u8 *body = NULL;
        size_t blen = 0, n;
        int cerr;

        if (used >= len)
            goto out;
        n = tbt_http_chunk_feed(&rx->chunk, buf + used, len - used, &body, &blen, &cerr);
        if (cerr == TBT_HTTP_PROTO)
        {
            status = TBT_RX_PROTO;
            used = 0;
            goto out;
        }
        if (cerr == TBT_HTTP_NEED_MORE)
        {
            used += n;
            goto out;
        }
        if (rx->chunk.state == TBT_HTTP_CHUNK_DONE)
        {
            used += n;
            goto out; /* NEED_MORE: message done, caller resets after the next header */
        }
        if (!blen || !n)
        {
            status = TBT_RX_PROTO;
            used = 0;
            goto out;
        }
        used += n;

        if (!rx->chunk_started)
        {
            rx->chunk_started = true;
            rx->chunk_size = rx->chunk.chunk_size;
            if (rx->chunk_size == 1)
            {
                rx->chunk_kind = TBT_HTTP_RX_DRIP;
            }
            else if (rx->chunk_size < TBT_HDR_LEN || rx->chunk_size > TBT_REC_MAX)
            {
                status = TBT_RX_PROTO;
                used = 0;
                goto out;
            }
            else
            {
                rx->chunk_kind = TBT_HTTP_RX_RECORD;
                rx->rec_len = 0;
                rx->rec_need = 0;
            }
        }

        if (rx->chunk_kind == TBT_HTTP_RX_RECORD)
        {
            if (rx->rec_len + blen > sizeof(rx->rec))
            {
                status = TBT_RX_PROTO;
                used = 0;
                goto out;
            }
            memcpy(rx->rec + rx->rec_len, body, blen);
            rx->rec_len += (u32)blen;

            /* One chunk == one record. As soon as the header is complete,
             * require the announced chunk size to be exactly this record.
             * This rejects two records in one chunk and a record split across
             * two chunks before any of the body is trusted. */
            if (rx->rec_len >= TBT_HDR_LEN && !rx->rec_need)
            {
                struct tbt_hdr h;

                tbt_hdr_decode(rx->rec, &h);
                if (tbt_hdr_check(&h))
                {
                    status = TBT_RX_PROTO;
                    used = 0;
                    goto out;
                }
                rx->rec_need = TBT_HDR_LEN + h.body_len;
                if (rx->rec_need != rx->chunk_size)
                {
                    status = TBT_RX_PROTO;
                    used = 0;
                    goto out;
                }
            }
            if (rx->rec_need && rx->rec_len > rx->rec_need)
            {
                status = TBT_RX_PROTO;
                used = 0;
                goto out;
            }
        }

        if (rx->chunk.state == TBT_HTTP_CHUNK_DATA_END)
        {
            if (rx->chunk_kind == TBT_HTTP_RX_RECORD)
            {
                int rerr;
                size_t rn;

                if (rx->rec_len != rx->chunk_size || rx->rec_len != rx->rec_need)
                {
                    status = TBT_RX_PROTO;
                    used = 0;
                    goto out;
                }
                rn = tbt_rec_scan(rx->rec, rx->rec_len, out, &rerr);
                if (rerr != TBT_REC_OK || rn != rx->rec_len)
                {
                    status = TBT_RX_PROTO;
                    used = 0;
                    goto out;
                }
                rx->chunk_started = false;
                status = TBT_RX_OK;
                goto out;
            }
            rx->chunk_started = false; /* drip complete; no record */
        }
    }

out:
    if (err)
        *err = status;
    return used;
}

void tbt_http_rx_new_message(struct tbt_http_rx *rx)
{
    if (!rx)
        return;
    memset(rx, 0, sizeof(*rx));
    tbt_http_chunk_reset(&rx->chunk);
}

/* ------------------------------------------------------------------------- */
/* Keep-alive cadence.                                                        */

static bool http_elapsed(u64 last, u64 now, u32 ms)
{
    if (!ms)
        return false;
    return now - last >= (u64)ms * 1000000u;
}

bool tbt_http_need_pad(u64 last_tx_ns, u64 now_ns, u32 keepalive_ms)
{
    return http_elapsed(last_tx_ns, now_ns, keepalive_ms);
}

bool tbt_http_need_drip(u64 last_tx_ns, u64 now_ns, u32 drip_ms)
{
    return http_elapsed(last_tx_ns, now_ns, drip_ms);
}
