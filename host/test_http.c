/* test_http.c - host conformance test for the HTTP/1.1 carrier container.
 *
 * Links tbtun/tbt_http.c, the same object the module is built from, so a rule
 * that passes here is a rule the module enforces. No root, no module, no
 * kernel headers, no fixtures: `make host-test` runs it in well under a second
 * and exits non-zero on the first broken rule (it keeps going to list every
 * failure).
 *
 * Parser cross-check: this test carries its own strict request parser and its
 * own strict chunk decoder, written independently of tbt_http.c. They reject
 * exactly the canonical forms DESIGN.md §3.2 requires: one request line, CRLF
 * only, canonical header names in the documented order, lowercase hex chunk
 * sizes with no leading zeros. The chunk decoder is then compared with
 * tbt_http_chunk_parser on the same bytes.
 *
 * `make host-test` additionally feeds `--emit-req`/`--emit-resp` to h11 through
 * uv's offline cache (host/test_http_h11.py, h11 0.16.0 when this was
 * written). h11 is not a build dependency: when uv or a cached h11 is absent,
 * the Makefile prints a skip and the in-test parser is the check. python3 in
 * this environment has neither h11 nor httptools installed directly, and pip
 * is absent, so uv is the only route to a third-party parser here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_http.h"

static int checks;
static int failures;

static void ok(int cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
}

static int slice_eq(const char *s, size_t n, const char *lit)
{
    return strlen(lit) == n && memcmp(s, lit, n) == 0;
}

static int contains_ci(const char *s, size_t n, const char *needle)
{
    size_t m = strlen(needle), i, j;

    for (i = 0; i + m <= n; i++)
    {
        for (j = 0; j < m; j++)
        {
            char a = s[i + j], b = needle[j];

            if (a >= 'A' && a <= 'Z')
                a = (char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z')
                b = (char)(b + ('a' - 'A'));
            if (a != b)
                break;
        }
        if (j == m)
            return 1;
    }
    return 0;
}

static void cfg_set(struct tbt_http_cfg *c, const char *host, const char *id)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->host, sizeof(c->host), "%s", host);
    snprintf(c->request_id, sizeof(c->request_id), "%s", id);
}

/* ------------------------------------------------------------------------- */
/* Strict request parser, independent of tbt_http.c.                          */

struct hdr_view
{
    const char *name;
    size_t name_len;
    const char *val;
    size_t val_len;
};

struct req_view
{
    const char *method;
    size_t method_len;
    const char *target;
    size_t target_len;
    const char *version;
    size_t version_len;
    struct hdr_view h[16];
    int nh;
    size_t header_len;
};

static int strict_line(const char *s, size_t len, size_t from, size_t *n, size_t *next)
{
    size_t i;

    for (i = from; i + 1 < len; i++)
    {
        if (s[i] == '\n')
            return -1;
        if (s[i] == '\r')
        {
            if (s[i + 1] != '\n')
                return -1;
            *n = i - from;
            *next = i + 2;
            return 0;
        }
    }
    return -1;
}

static int strict_tchar(char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
        return 1;
    return c && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int strict_req(const char *s, size_t len, struct req_view *v)
{
    size_t pos = 0, n = 0, next = 0, sp1, sp2, colon, i, vs, ve;

    memset(v, 0, sizeof(*v));
    if (strict_line(s, len, pos, &n, &next))
        return -1;
    for (sp1 = 0; sp1 < n && s[sp1] != ' '; sp1++)
        ;
    if (sp1 == 0 || sp1 == n)
        return -1;
    for (sp2 = sp1 + 1; sp2 < n && s[sp2] != ' '; sp2++)
        ;
    if (sp2 == n || sp2 == sp1 + 1)
        return -1;
    v->method = s;
    v->method_len = sp1;
    v->target = s + sp1 + 1;
    v->target_len = sp2 - sp1 - 1;
    v->version = s + sp2 + 1;
    v->version_len = n - sp2 - 1;
    if (memchr(v->version, ' ', v->version_len))
        return -1;
    pos = next;

    while (pos < len)
    {
        if (strict_line(s, len, pos, &n, &next))
            return -1;
        if (n == 0)
        {
            v->header_len = next;
            return 0;
        }
        if (v->nh == (int)(sizeof(v->h) / sizeof(v->h[0])))
            return -1;
        for (colon = 0; colon < n && s[pos + colon] != ':'; colon++)
        {
            if (!strict_tchar(s[pos + colon]))
                return -1;
        }
        if (colon == 0 || colon == n)
            return -1;
        vs = colon + 1;
        while (vs < n && (s[pos + vs] == ' ' || s[pos + vs] == '\t'))
            vs++;
        ve = n;
        while (ve > vs && (s[pos + ve - 1] == ' ' || s[pos + ve - 1] == '\t'))
            ve--;
        for (i = vs; i < ve; i++)
        {
            unsigned char c = (unsigned char)s[pos + i];

            if ((c < 0x20 && c != '\t') || c == 0x7f)
                return -1;
        }
        v->h[v->nh].name = s + pos;
        v->h[v->nh].name_len = colon;
        v->h[v->nh].val = s + pos + vs;
        v->h[v->nh].val_len = ve - vs;
        v->nh++;
        pos = next;
    }
    return -1;
}

/* Strict chunk decoder for emitted streams: lowercase hex, no leading zeros,
 * CRLF only, trailers ignored. Returns bytes consumed or -1. */
static long strict_chunks(const char *s, size_t len, char *out, size_t cap, size_t *out_len)
{
    size_t i = 0, o = 0;

    for (;;)
    {
        size_t size = 0, digits = 0, first = i;

        while (i < len)
        {
            char c = s[i];

            if (c >= '0' && c <= '9')
                size = size * 16 + (size_t)(c - '0');
            else if (c >= 'a' && c <= 'f')
                size = size * 16 + (size_t)(c - 'a' + 10);
            else
                break;
            if (size > TBT_CHUNK_MAX)
                return -1;
            i++;
            digits++;
        }
        if (!digits)
            return -1;
        if (digits > 1 && s[first] == '0')
            return -1; /* emitted sizes have no leading zeros */
        if (i + 1 >= len || s[i] != '\r' || s[i + 1] != '\n')
            return -1;
        i += 2;
        if (size == 0)
        {
            while (i + 1 < len && !(s[i] == '\r' && s[i + 1] == '\n'))
            {
                while (i + 1 < len && !(s[i] == '\r' && s[i + 1] == '\n'))
                    i++;
                if (i + 1 >= len)
                    return -1;
                i += 2;
            }
            if (i + 1 >= len)
                return -1;
            i += 2;
            *out_len = o;
            return (long)i;
        }
        if (size > len - i || o + size > cap)
            return -1;
        memcpy(out + o, s + i, size);
        o += size;
        i += size;
        if (i + 1 >= len || s[i] != '\r' || s[i + 1] != '\n')
            return -1;
        i += 2;
    }
}

/* Generated with LC_ALL=C date -u -d @N for N = 1672531200 + i*86400,
 * i=0..730 (2023-01-01 through 2024-12-31: one non-leap and one leap year).
 * Every record is exactly 29 bytes. The test walks this table at run time; it
 * never shells out to date. */
static const char http_date_days[] =
    "Sun, 01 Jan 2023 00:00:00 GMT"
    "Mon, 02 Jan 2023 00:00:00 GMT"
    "Tue, 03 Jan 2023 00:00:00 GMT"
    "Wed, 04 Jan 2023 00:00:00 GMT"
    "Thu, 05 Jan 2023 00:00:00 GMT"
    "Fri, 06 Jan 2023 00:00:00 GMT"
    "Sat, 07 Jan 2023 00:00:00 GMT"
    "Sun, 08 Jan 2023 00:00:00 GMT"
    "Mon, 09 Jan 2023 00:00:00 GMT"
    "Tue, 10 Jan 2023 00:00:00 GMT"
    "Wed, 11 Jan 2023 00:00:00 GMT"
    "Thu, 12 Jan 2023 00:00:00 GMT"
    "Fri, 13 Jan 2023 00:00:00 GMT"
    "Sat, 14 Jan 2023 00:00:00 GMT"
    "Sun, 15 Jan 2023 00:00:00 GMT"
    "Mon, 16 Jan 2023 00:00:00 GMT"
    "Tue, 17 Jan 2023 00:00:00 GMT"
    "Wed, 18 Jan 2023 00:00:00 GMT"
    "Thu, 19 Jan 2023 00:00:00 GMT"
    "Fri, 20 Jan 2023 00:00:00 GMT"
    "Sat, 21 Jan 2023 00:00:00 GMT"
    "Sun, 22 Jan 2023 00:00:00 GMT"
    "Mon, 23 Jan 2023 00:00:00 GMT"
    "Tue, 24 Jan 2023 00:00:00 GMT"
    "Wed, 25 Jan 2023 00:00:00 GMT"
    "Thu, 26 Jan 2023 00:00:00 GMT"
    "Fri, 27 Jan 2023 00:00:00 GMT"
    "Sat, 28 Jan 2023 00:00:00 GMT"
    "Sun, 29 Jan 2023 00:00:00 GMT"
    "Mon, 30 Jan 2023 00:00:00 GMT"
    "Tue, 31 Jan 2023 00:00:00 GMT"
    "Wed, 01 Feb 2023 00:00:00 GMT"
    "Thu, 02 Feb 2023 00:00:00 GMT"
    "Fri, 03 Feb 2023 00:00:00 GMT"
    "Sat, 04 Feb 2023 00:00:00 GMT"
    "Sun, 05 Feb 2023 00:00:00 GMT"
    "Mon, 06 Feb 2023 00:00:00 GMT"
    "Tue, 07 Feb 2023 00:00:00 GMT"
    "Wed, 08 Feb 2023 00:00:00 GMT"
    "Thu, 09 Feb 2023 00:00:00 GMT"
    "Fri, 10 Feb 2023 00:00:00 GMT"
    "Sat, 11 Feb 2023 00:00:00 GMT"
    "Sun, 12 Feb 2023 00:00:00 GMT"
    "Mon, 13 Feb 2023 00:00:00 GMT"
    "Tue, 14 Feb 2023 00:00:00 GMT"
    "Wed, 15 Feb 2023 00:00:00 GMT"
    "Thu, 16 Feb 2023 00:00:00 GMT"
    "Fri, 17 Feb 2023 00:00:00 GMT"
    "Sat, 18 Feb 2023 00:00:00 GMT"
    "Sun, 19 Feb 2023 00:00:00 GMT"
    "Mon, 20 Feb 2023 00:00:00 GMT"
    "Tue, 21 Feb 2023 00:00:00 GMT"
    "Wed, 22 Feb 2023 00:00:00 GMT"
    "Thu, 23 Feb 2023 00:00:00 GMT"
    "Fri, 24 Feb 2023 00:00:00 GMT"
    "Sat, 25 Feb 2023 00:00:00 GMT"
    "Sun, 26 Feb 2023 00:00:00 GMT"
    "Mon, 27 Feb 2023 00:00:00 GMT"
    "Tue, 28 Feb 2023 00:00:00 GMT"
    "Wed, 01 Mar 2023 00:00:00 GMT"
    "Thu, 02 Mar 2023 00:00:00 GMT"
    "Fri, 03 Mar 2023 00:00:00 GMT"
    "Sat, 04 Mar 2023 00:00:00 GMT"
    "Sun, 05 Mar 2023 00:00:00 GMT"
    "Mon, 06 Mar 2023 00:00:00 GMT"
    "Tue, 07 Mar 2023 00:00:00 GMT"
    "Wed, 08 Mar 2023 00:00:00 GMT"
    "Thu, 09 Mar 2023 00:00:00 GMT"
    "Fri, 10 Mar 2023 00:00:00 GMT"
    "Sat, 11 Mar 2023 00:00:00 GMT"
    "Sun, 12 Mar 2023 00:00:00 GMT"
    "Mon, 13 Mar 2023 00:00:00 GMT"
    "Tue, 14 Mar 2023 00:00:00 GMT"
    "Wed, 15 Mar 2023 00:00:00 GMT"
    "Thu, 16 Mar 2023 00:00:00 GMT"
    "Fri, 17 Mar 2023 00:00:00 GMT"
    "Sat, 18 Mar 2023 00:00:00 GMT"
    "Sun, 19 Mar 2023 00:00:00 GMT"
    "Mon, 20 Mar 2023 00:00:00 GMT"
    "Tue, 21 Mar 2023 00:00:00 GMT"
    "Wed, 22 Mar 2023 00:00:00 GMT"
    "Thu, 23 Mar 2023 00:00:00 GMT"
    "Fri, 24 Mar 2023 00:00:00 GMT"
    "Sat, 25 Mar 2023 00:00:00 GMT"
    "Sun, 26 Mar 2023 00:00:00 GMT"
    "Mon, 27 Mar 2023 00:00:00 GMT"
    "Tue, 28 Mar 2023 00:00:00 GMT"
    "Wed, 29 Mar 2023 00:00:00 GMT"
    "Thu, 30 Mar 2023 00:00:00 GMT"
    "Fri, 31 Mar 2023 00:00:00 GMT"
    "Sat, 01 Apr 2023 00:00:00 GMT"
    "Sun, 02 Apr 2023 00:00:00 GMT"
    "Mon, 03 Apr 2023 00:00:00 GMT"
    "Tue, 04 Apr 2023 00:00:00 GMT"
    "Wed, 05 Apr 2023 00:00:00 GMT"
    "Thu, 06 Apr 2023 00:00:00 GMT"
    "Fri, 07 Apr 2023 00:00:00 GMT"
    "Sat, 08 Apr 2023 00:00:00 GMT"
    "Sun, 09 Apr 2023 00:00:00 GMT"
    "Mon, 10 Apr 2023 00:00:00 GMT"
    "Tue, 11 Apr 2023 00:00:00 GMT"
    "Wed, 12 Apr 2023 00:00:00 GMT"
    "Thu, 13 Apr 2023 00:00:00 GMT"
    "Fri, 14 Apr 2023 00:00:00 GMT"
    "Sat, 15 Apr 2023 00:00:00 GMT"
    "Sun, 16 Apr 2023 00:00:00 GMT"
    "Mon, 17 Apr 2023 00:00:00 GMT"
    "Tue, 18 Apr 2023 00:00:00 GMT"
    "Wed, 19 Apr 2023 00:00:00 GMT"
    "Thu, 20 Apr 2023 00:00:00 GMT"
    "Fri, 21 Apr 2023 00:00:00 GMT"
    "Sat, 22 Apr 2023 00:00:00 GMT"
    "Sun, 23 Apr 2023 00:00:00 GMT"
    "Mon, 24 Apr 2023 00:00:00 GMT"
    "Tue, 25 Apr 2023 00:00:00 GMT"
    "Wed, 26 Apr 2023 00:00:00 GMT"
    "Thu, 27 Apr 2023 00:00:00 GMT"
    "Fri, 28 Apr 2023 00:00:00 GMT"
    "Sat, 29 Apr 2023 00:00:00 GMT"
    "Sun, 30 Apr 2023 00:00:00 GMT"
    "Mon, 01 May 2023 00:00:00 GMT"
    "Tue, 02 May 2023 00:00:00 GMT"
    "Wed, 03 May 2023 00:00:00 GMT"
    "Thu, 04 May 2023 00:00:00 GMT"
    "Fri, 05 May 2023 00:00:00 GMT"
    "Sat, 06 May 2023 00:00:00 GMT"
    "Sun, 07 May 2023 00:00:00 GMT"
    "Mon, 08 May 2023 00:00:00 GMT"
    "Tue, 09 May 2023 00:00:00 GMT"
    "Wed, 10 May 2023 00:00:00 GMT"
    "Thu, 11 May 2023 00:00:00 GMT"
    "Fri, 12 May 2023 00:00:00 GMT"
    "Sat, 13 May 2023 00:00:00 GMT"
    "Sun, 14 May 2023 00:00:00 GMT"
    "Mon, 15 May 2023 00:00:00 GMT"
    "Tue, 16 May 2023 00:00:00 GMT"
    "Wed, 17 May 2023 00:00:00 GMT"
    "Thu, 18 May 2023 00:00:00 GMT"
    "Fri, 19 May 2023 00:00:00 GMT"
    "Sat, 20 May 2023 00:00:00 GMT"
    "Sun, 21 May 2023 00:00:00 GMT"
    "Mon, 22 May 2023 00:00:00 GMT"
    "Tue, 23 May 2023 00:00:00 GMT"
    "Wed, 24 May 2023 00:00:00 GMT"
    "Thu, 25 May 2023 00:00:00 GMT"
    "Fri, 26 May 2023 00:00:00 GMT"
    "Sat, 27 May 2023 00:00:00 GMT"
    "Sun, 28 May 2023 00:00:00 GMT"
    "Mon, 29 May 2023 00:00:00 GMT"
    "Tue, 30 May 2023 00:00:00 GMT"
    "Wed, 31 May 2023 00:00:00 GMT"
    "Thu, 01 Jun 2023 00:00:00 GMT"
    "Fri, 02 Jun 2023 00:00:00 GMT"
    "Sat, 03 Jun 2023 00:00:00 GMT"
    "Sun, 04 Jun 2023 00:00:00 GMT"
    "Mon, 05 Jun 2023 00:00:00 GMT"
    "Tue, 06 Jun 2023 00:00:00 GMT"
    "Wed, 07 Jun 2023 00:00:00 GMT"
    "Thu, 08 Jun 2023 00:00:00 GMT"
    "Fri, 09 Jun 2023 00:00:00 GMT"
    "Sat, 10 Jun 2023 00:00:00 GMT"
    "Sun, 11 Jun 2023 00:00:00 GMT"
    "Mon, 12 Jun 2023 00:00:00 GMT"
    "Tue, 13 Jun 2023 00:00:00 GMT"
    "Wed, 14 Jun 2023 00:00:00 GMT"
    "Thu, 15 Jun 2023 00:00:00 GMT"
    "Fri, 16 Jun 2023 00:00:00 GMT"
    "Sat, 17 Jun 2023 00:00:00 GMT"
    "Sun, 18 Jun 2023 00:00:00 GMT"
    "Mon, 19 Jun 2023 00:00:00 GMT"
    "Tue, 20 Jun 2023 00:00:00 GMT"
    "Wed, 21 Jun 2023 00:00:00 GMT"
    "Thu, 22 Jun 2023 00:00:00 GMT"
    "Fri, 23 Jun 2023 00:00:00 GMT"
    "Sat, 24 Jun 2023 00:00:00 GMT"
    "Sun, 25 Jun 2023 00:00:00 GMT"
    "Mon, 26 Jun 2023 00:00:00 GMT"
    "Tue, 27 Jun 2023 00:00:00 GMT"
    "Wed, 28 Jun 2023 00:00:00 GMT"
    "Thu, 29 Jun 2023 00:00:00 GMT"
    "Fri, 30 Jun 2023 00:00:00 GMT"
    "Sat, 01 Jul 2023 00:00:00 GMT"
    "Sun, 02 Jul 2023 00:00:00 GMT"
    "Mon, 03 Jul 2023 00:00:00 GMT"
    "Tue, 04 Jul 2023 00:00:00 GMT"
    "Wed, 05 Jul 2023 00:00:00 GMT"
    "Thu, 06 Jul 2023 00:00:00 GMT"
    "Fri, 07 Jul 2023 00:00:00 GMT"
    "Sat, 08 Jul 2023 00:00:00 GMT"
    "Sun, 09 Jul 2023 00:00:00 GMT"
    "Mon, 10 Jul 2023 00:00:00 GMT"
    "Tue, 11 Jul 2023 00:00:00 GMT"
    "Wed, 12 Jul 2023 00:00:00 GMT"
    "Thu, 13 Jul 2023 00:00:00 GMT"
    "Fri, 14 Jul 2023 00:00:00 GMT"
    "Sat, 15 Jul 2023 00:00:00 GMT"
    "Sun, 16 Jul 2023 00:00:00 GMT"
    "Mon, 17 Jul 2023 00:00:00 GMT"
    "Tue, 18 Jul 2023 00:00:00 GMT"
    "Wed, 19 Jul 2023 00:00:00 GMT"
    "Thu, 20 Jul 2023 00:00:00 GMT"
    "Fri, 21 Jul 2023 00:00:00 GMT"
    "Sat, 22 Jul 2023 00:00:00 GMT"
    "Sun, 23 Jul 2023 00:00:00 GMT"
    "Mon, 24 Jul 2023 00:00:00 GMT"
    "Tue, 25 Jul 2023 00:00:00 GMT"
    "Wed, 26 Jul 2023 00:00:00 GMT"
    "Thu, 27 Jul 2023 00:00:00 GMT"
    "Fri, 28 Jul 2023 00:00:00 GMT"
    "Sat, 29 Jul 2023 00:00:00 GMT"
    "Sun, 30 Jul 2023 00:00:00 GMT"
    "Mon, 31 Jul 2023 00:00:00 GMT"
    "Tue, 01 Aug 2023 00:00:00 GMT"
    "Wed, 02 Aug 2023 00:00:00 GMT"
    "Thu, 03 Aug 2023 00:00:00 GMT"
    "Fri, 04 Aug 2023 00:00:00 GMT"
    "Sat, 05 Aug 2023 00:00:00 GMT"
    "Sun, 06 Aug 2023 00:00:00 GMT"
    "Mon, 07 Aug 2023 00:00:00 GMT"
    "Tue, 08 Aug 2023 00:00:00 GMT"
    "Wed, 09 Aug 2023 00:00:00 GMT"
    "Thu, 10 Aug 2023 00:00:00 GMT"
    "Fri, 11 Aug 2023 00:00:00 GMT"
    "Sat, 12 Aug 2023 00:00:00 GMT"
    "Sun, 13 Aug 2023 00:00:00 GMT"
    "Mon, 14 Aug 2023 00:00:00 GMT"
    "Tue, 15 Aug 2023 00:00:00 GMT"
    "Wed, 16 Aug 2023 00:00:00 GMT"
    "Thu, 17 Aug 2023 00:00:00 GMT"
    "Fri, 18 Aug 2023 00:00:00 GMT"
    "Sat, 19 Aug 2023 00:00:00 GMT"
    "Sun, 20 Aug 2023 00:00:00 GMT"
    "Mon, 21 Aug 2023 00:00:00 GMT"
    "Tue, 22 Aug 2023 00:00:00 GMT"
    "Wed, 23 Aug 2023 00:00:00 GMT"
    "Thu, 24 Aug 2023 00:00:00 GMT"
    "Fri, 25 Aug 2023 00:00:00 GMT"
    "Sat, 26 Aug 2023 00:00:00 GMT"
    "Sun, 27 Aug 2023 00:00:00 GMT"
    "Mon, 28 Aug 2023 00:00:00 GMT"
    "Tue, 29 Aug 2023 00:00:00 GMT"
    "Wed, 30 Aug 2023 00:00:00 GMT"
    "Thu, 31 Aug 2023 00:00:00 GMT"
    "Fri, 01 Sep 2023 00:00:00 GMT"
    "Sat, 02 Sep 2023 00:00:00 GMT"
    "Sun, 03 Sep 2023 00:00:00 GMT"
    "Mon, 04 Sep 2023 00:00:00 GMT"
    "Tue, 05 Sep 2023 00:00:00 GMT"
    "Wed, 06 Sep 2023 00:00:00 GMT"
    "Thu, 07 Sep 2023 00:00:00 GMT"
    "Fri, 08 Sep 2023 00:00:00 GMT"
    "Sat, 09 Sep 2023 00:00:00 GMT"
    "Sun, 10 Sep 2023 00:00:00 GMT"
    "Mon, 11 Sep 2023 00:00:00 GMT"
    "Tue, 12 Sep 2023 00:00:00 GMT"
    "Wed, 13 Sep 2023 00:00:00 GMT"
    "Thu, 14 Sep 2023 00:00:00 GMT"
    "Fri, 15 Sep 2023 00:00:00 GMT"
    "Sat, 16 Sep 2023 00:00:00 GMT"
    "Sun, 17 Sep 2023 00:00:00 GMT"
    "Mon, 18 Sep 2023 00:00:00 GMT"
    "Tue, 19 Sep 2023 00:00:00 GMT"
    "Wed, 20 Sep 2023 00:00:00 GMT"
    "Thu, 21 Sep 2023 00:00:00 GMT"
    "Fri, 22 Sep 2023 00:00:00 GMT"
    "Sat, 23 Sep 2023 00:00:00 GMT"
    "Sun, 24 Sep 2023 00:00:00 GMT"
    "Mon, 25 Sep 2023 00:00:00 GMT"
    "Tue, 26 Sep 2023 00:00:00 GMT"
    "Wed, 27 Sep 2023 00:00:00 GMT"
    "Thu, 28 Sep 2023 00:00:00 GMT"
    "Fri, 29 Sep 2023 00:00:00 GMT"
    "Sat, 30 Sep 2023 00:00:00 GMT"
    "Sun, 01 Oct 2023 00:00:00 GMT"
    "Mon, 02 Oct 2023 00:00:00 GMT"
    "Tue, 03 Oct 2023 00:00:00 GMT"
    "Wed, 04 Oct 2023 00:00:00 GMT"
    "Thu, 05 Oct 2023 00:00:00 GMT"
    "Fri, 06 Oct 2023 00:00:00 GMT"
    "Sat, 07 Oct 2023 00:00:00 GMT"
    "Sun, 08 Oct 2023 00:00:00 GMT"
    "Mon, 09 Oct 2023 00:00:00 GMT"
    "Tue, 10 Oct 2023 00:00:00 GMT"
    "Wed, 11 Oct 2023 00:00:00 GMT"
    "Thu, 12 Oct 2023 00:00:00 GMT"
    "Fri, 13 Oct 2023 00:00:00 GMT"
    "Sat, 14 Oct 2023 00:00:00 GMT"
    "Sun, 15 Oct 2023 00:00:00 GMT"
    "Mon, 16 Oct 2023 00:00:00 GMT"
    "Tue, 17 Oct 2023 00:00:00 GMT"
    "Wed, 18 Oct 2023 00:00:00 GMT"
    "Thu, 19 Oct 2023 00:00:00 GMT"
    "Fri, 20 Oct 2023 00:00:00 GMT"
    "Sat, 21 Oct 2023 00:00:00 GMT"
    "Sun, 22 Oct 2023 00:00:00 GMT"
    "Mon, 23 Oct 2023 00:00:00 GMT"
    "Tue, 24 Oct 2023 00:00:00 GMT"
    "Wed, 25 Oct 2023 00:00:00 GMT"
    "Thu, 26 Oct 2023 00:00:00 GMT"
    "Fri, 27 Oct 2023 00:00:00 GMT"
    "Sat, 28 Oct 2023 00:00:00 GMT"
    "Sun, 29 Oct 2023 00:00:00 GMT"
    "Mon, 30 Oct 2023 00:00:00 GMT"
    "Tue, 31 Oct 2023 00:00:00 GMT"
    "Wed, 01 Nov 2023 00:00:00 GMT"
    "Thu, 02 Nov 2023 00:00:00 GMT"
    "Fri, 03 Nov 2023 00:00:00 GMT"
    "Sat, 04 Nov 2023 00:00:00 GMT"
    "Sun, 05 Nov 2023 00:00:00 GMT"
    "Mon, 06 Nov 2023 00:00:00 GMT"
    "Tue, 07 Nov 2023 00:00:00 GMT"
    "Wed, 08 Nov 2023 00:00:00 GMT"
    "Thu, 09 Nov 2023 00:00:00 GMT"
    "Fri, 10 Nov 2023 00:00:00 GMT"
    "Sat, 11 Nov 2023 00:00:00 GMT"
    "Sun, 12 Nov 2023 00:00:00 GMT"
    "Mon, 13 Nov 2023 00:00:00 GMT"
    "Tue, 14 Nov 2023 00:00:00 GMT"
    "Wed, 15 Nov 2023 00:00:00 GMT"
    "Thu, 16 Nov 2023 00:00:00 GMT"
    "Fri, 17 Nov 2023 00:00:00 GMT"
    "Sat, 18 Nov 2023 00:00:00 GMT"
    "Sun, 19 Nov 2023 00:00:00 GMT"
    "Mon, 20 Nov 2023 00:00:00 GMT"
    "Tue, 21 Nov 2023 00:00:00 GMT"
    "Wed, 22 Nov 2023 00:00:00 GMT"
    "Thu, 23 Nov 2023 00:00:00 GMT"
    "Fri, 24 Nov 2023 00:00:00 GMT"
    "Sat, 25 Nov 2023 00:00:00 GMT"
    "Sun, 26 Nov 2023 00:00:00 GMT"
    "Mon, 27 Nov 2023 00:00:00 GMT"
    "Tue, 28 Nov 2023 00:00:00 GMT"
    "Wed, 29 Nov 2023 00:00:00 GMT"
    "Thu, 30 Nov 2023 00:00:00 GMT"
    "Fri, 01 Dec 2023 00:00:00 GMT"
    "Sat, 02 Dec 2023 00:00:00 GMT"
    "Sun, 03 Dec 2023 00:00:00 GMT"
    "Mon, 04 Dec 2023 00:00:00 GMT"
    "Tue, 05 Dec 2023 00:00:00 GMT"
    "Wed, 06 Dec 2023 00:00:00 GMT"
    "Thu, 07 Dec 2023 00:00:00 GMT"
    "Fri, 08 Dec 2023 00:00:00 GMT"
    "Sat, 09 Dec 2023 00:00:00 GMT"
    "Sun, 10 Dec 2023 00:00:00 GMT"
    "Mon, 11 Dec 2023 00:00:00 GMT"
    "Tue, 12 Dec 2023 00:00:00 GMT"
    "Wed, 13 Dec 2023 00:00:00 GMT"
    "Thu, 14 Dec 2023 00:00:00 GMT"
    "Fri, 15 Dec 2023 00:00:00 GMT"
    "Sat, 16 Dec 2023 00:00:00 GMT"
    "Sun, 17 Dec 2023 00:00:00 GMT"
    "Mon, 18 Dec 2023 00:00:00 GMT"
    "Tue, 19 Dec 2023 00:00:00 GMT"
    "Wed, 20 Dec 2023 00:00:00 GMT"
    "Thu, 21 Dec 2023 00:00:00 GMT"
    "Fri, 22 Dec 2023 00:00:00 GMT"
    "Sat, 23 Dec 2023 00:00:00 GMT"
    "Sun, 24 Dec 2023 00:00:00 GMT"
    "Mon, 25 Dec 2023 00:00:00 GMT"
    "Tue, 26 Dec 2023 00:00:00 GMT"
    "Wed, 27 Dec 2023 00:00:00 GMT"
    "Thu, 28 Dec 2023 00:00:00 GMT"
    "Fri, 29 Dec 2023 00:00:00 GMT"
    "Sat, 30 Dec 2023 00:00:00 GMT"
    "Sun, 31 Dec 2023 00:00:00 GMT"
    "Mon, 01 Jan 2024 00:00:00 GMT"
    "Tue, 02 Jan 2024 00:00:00 GMT"
    "Wed, 03 Jan 2024 00:00:00 GMT"
    "Thu, 04 Jan 2024 00:00:00 GMT"
    "Fri, 05 Jan 2024 00:00:00 GMT"
    "Sat, 06 Jan 2024 00:00:00 GMT"
    "Sun, 07 Jan 2024 00:00:00 GMT"
    "Mon, 08 Jan 2024 00:00:00 GMT"
    "Tue, 09 Jan 2024 00:00:00 GMT"
    "Wed, 10 Jan 2024 00:00:00 GMT"
    "Thu, 11 Jan 2024 00:00:00 GMT"
    "Fri, 12 Jan 2024 00:00:00 GMT"
    "Sat, 13 Jan 2024 00:00:00 GMT"
    "Sun, 14 Jan 2024 00:00:00 GMT"
    "Mon, 15 Jan 2024 00:00:00 GMT"
    "Tue, 16 Jan 2024 00:00:00 GMT"
    "Wed, 17 Jan 2024 00:00:00 GMT"
    "Thu, 18 Jan 2024 00:00:00 GMT"
    "Fri, 19 Jan 2024 00:00:00 GMT"
    "Sat, 20 Jan 2024 00:00:00 GMT"
    "Sun, 21 Jan 2024 00:00:00 GMT"
    "Mon, 22 Jan 2024 00:00:00 GMT"
    "Tue, 23 Jan 2024 00:00:00 GMT"
    "Wed, 24 Jan 2024 00:00:00 GMT"
    "Thu, 25 Jan 2024 00:00:00 GMT"
    "Fri, 26 Jan 2024 00:00:00 GMT"
    "Sat, 27 Jan 2024 00:00:00 GMT"
    "Sun, 28 Jan 2024 00:00:00 GMT"
    "Mon, 29 Jan 2024 00:00:00 GMT"
    "Tue, 30 Jan 2024 00:00:00 GMT"
    "Wed, 31 Jan 2024 00:00:00 GMT"
    "Thu, 01 Feb 2024 00:00:00 GMT"
    "Fri, 02 Feb 2024 00:00:00 GMT"
    "Sat, 03 Feb 2024 00:00:00 GMT"
    "Sun, 04 Feb 2024 00:00:00 GMT"
    "Mon, 05 Feb 2024 00:00:00 GMT"
    "Tue, 06 Feb 2024 00:00:00 GMT"
    "Wed, 07 Feb 2024 00:00:00 GMT"
    "Thu, 08 Feb 2024 00:00:00 GMT"
    "Fri, 09 Feb 2024 00:00:00 GMT"
    "Sat, 10 Feb 2024 00:00:00 GMT"
    "Sun, 11 Feb 2024 00:00:00 GMT"
    "Mon, 12 Feb 2024 00:00:00 GMT"
    "Tue, 13 Feb 2024 00:00:00 GMT"
    "Wed, 14 Feb 2024 00:00:00 GMT"
    "Thu, 15 Feb 2024 00:00:00 GMT"
    "Fri, 16 Feb 2024 00:00:00 GMT"
    "Sat, 17 Feb 2024 00:00:00 GMT"
    "Sun, 18 Feb 2024 00:00:00 GMT"
    "Mon, 19 Feb 2024 00:00:00 GMT"
    "Tue, 20 Feb 2024 00:00:00 GMT"
    "Wed, 21 Feb 2024 00:00:00 GMT"
    "Thu, 22 Feb 2024 00:00:00 GMT"
    "Fri, 23 Feb 2024 00:00:00 GMT"
    "Sat, 24 Feb 2024 00:00:00 GMT"
    "Sun, 25 Feb 2024 00:00:00 GMT"
    "Mon, 26 Feb 2024 00:00:00 GMT"
    "Tue, 27 Feb 2024 00:00:00 GMT"
    "Wed, 28 Feb 2024 00:00:00 GMT"
    "Thu, 29 Feb 2024 00:00:00 GMT"
    "Fri, 01 Mar 2024 00:00:00 GMT"
    "Sat, 02 Mar 2024 00:00:00 GMT"
    "Sun, 03 Mar 2024 00:00:00 GMT"
    "Mon, 04 Mar 2024 00:00:00 GMT"
    "Tue, 05 Mar 2024 00:00:00 GMT"
    "Wed, 06 Mar 2024 00:00:00 GMT"
    "Thu, 07 Mar 2024 00:00:00 GMT"
    "Fri, 08 Mar 2024 00:00:00 GMT"
    "Sat, 09 Mar 2024 00:00:00 GMT"
    "Sun, 10 Mar 2024 00:00:00 GMT"
    "Mon, 11 Mar 2024 00:00:00 GMT"
    "Tue, 12 Mar 2024 00:00:00 GMT"
    "Wed, 13 Mar 2024 00:00:00 GMT"
    "Thu, 14 Mar 2024 00:00:00 GMT"
    "Fri, 15 Mar 2024 00:00:00 GMT"
    "Sat, 16 Mar 2024 00:00:00 GMT"
    "Sun, 17 Mar 2024 00:00:00 GMT"
    "Mon, 18 Mar 2024 00:00:00 GMT"
    "Tue, 19 Mar 2024 00:00:00 GMT"
    "Wed, 20 Mar 2024 00:00:00 GMT"
    "Thu, 21 Mar 2024 00:00:00 GMT"
    "Fri, 22 Mar 2024 00:00:00 GMT"
    "Sat, 23 Mar 2024 00:00:00 GMT"
    "Sun, 24 Mar 2024 00:00:00 GMT"
    "Mon, 25 Mar 2024 00:00:00 GMT"
    "Tue, 26 Mar 2024 00:00:00 GMT"
    "Wed, 27 Mar 2024 00:00:00 GMT"
    "Thu, 28 Mar 2024 00:00:00 GMT"
    "Fri, 29 Mar 2024 00:00:00 GMT"
    "Sat, 30 Mar 2024 00:00:00 GMT"
    "Sun, 31 Mar 2024 00:00:00 GMT"
    "Mon, 01 Apr 2024 00:00:00 GMT"
    "Tue, 02 Apr 2024 00:00:00 GMT"
    "Wed, 03 Apr 2024 00:00:00 GMT"
    "Thu, 04 Apr 2024 00:00:00 GMT"
    "Fri, 05 Apr 2024 00:00:00 GMT"
    "Sat, 06 Apr 2024 00:00:00 GMT"
    "Sun, 07 Apr 2024 00:00:00 GMT"
    "Mon, 08 Apr 2024 00:00:00 GMT"
    "Tue, 09 Apr 2024 00:00:00 GMT"
    "Wed, 10 Apr 2024 00:00:00 GMT"
    "Thu, 11 Apr 2024 00:00:00 GMT"
    "Fri, 12 Apr 2024 00:00:00 GMT"
    "Sat, 13 Apr 2024 00:00:00 GMT"
    "Sun, 14 Apr 2024 00:00:00 GMT"
    "Mon, 15 Apr 2024 00:00:00 GMT"
    "Tue, 16 Apr 2024 00:00:00 GMT"
    "Wed, 17 Apr 2024 00:00:00 GMT"
    "Thu, 18 Apr 2024 00:00:00 GMT"
    "Fri, 19 Apr 2024 00:00:00 GMT"
    "Sat, 20 Apr 2024 00:00:00 GMT"
    "Sun, 21 Apr 2024 00:00:00 GMT"
    "Mon, 22 Apr 2024 00:00:00 GMT"
    "Tue, 23 Apr 2024 00:00:00 GMT"
    "Wed, 24 Apr 2024 00:00:00 GMT"
    "Thu, 25 Apr 2024 00:00:00 GMT"
    "Fri, 26 Apr 2024 00:00:00 GMT"
    "Sat, 27 Apr 2024 00:00:00 GMT"
    "Sun, 28 Apr 2024 00:00:00 GMT"
    "Mon, 29 Apr 2024 00:00:00 GMT"
    "Tue, 30 Apr 2024 00:00:00 GMT"
    "Wed, 01 May 2024 00:00:00 GMT"
    "Thu, 02 May 2024 00:00:00 GMT"
    "Fri, 03 May 2024 00:00:00 GMT"
    "Sat, 04 May 2024 00:00:00 GMT"
    "Sun, 05 May 2024 00:00:00 GMT"
    "Mon, 06 May 2024 00:00:00 GMT"
    "Tue, 07 May 2024 00:00:00 GMT"
    "Wed, 08 May 2024 00:00:00 GMT"
    "Thu, 09 May 2024 00:00:00 GMT"
    "Fri, 10 May 2024 00:00:00 GMT"
    "Sat, 11 May 2024 00:00:00 GMT"
    "Sun, 12 May 2024 00:00:00 GMT"
    "Mon, 13 May 2024 00:00:00 GMT"
    "Tue, 14 May 2024 00:00:00 GMT"
    "Wed, 15 May 2024 00:00:00 GMT"
    "Thu, 16 May 2024 00:00:00 GMT"
    "Fri, 17 May 2024 00:00:00 GMT"
    "Sat, 18 May 2024 00:00:00 GMT"
    "Sun, 19 May 2024 00:00:00 GMT"
    "Mon, 20 May 2024 00:00:00 GMT"
    "Tue, 21 May 2024 00:00:00 GMT"
    "Wed, 22 May 2024 00:00:00 GMT"
    "Thu, 23 May 2024 00:00:00 GMT"
    "Fri, 24 May 2024 00:00:00 GMT"
    "Sat, 25 May 2024 00:00:00 GMT"
    "Sun, 26 May 2024 00:00:00 GMT"
    "Mon, 27 May 2024 00:00:00 GMT"
    "Tue, 28 May 2024 00:00:00 GMT"
    "Wed, 29 May 2024 00:00:00 GMT"
    "Thu, 30 May 2024 00:00:00 GMT"
    "Fri, 31 May 2024 00:00:00 GMT"
    "Sat, 01 Jun 2024 00:00:00 GMT"
    "Sun, 02 Jun 2024 00:00:00 GMT"
    "Mon, 03 Jun 2024 00:00:00 GMT"
    "Tue, 04 Jun 2024 00:00:00 GMT"
    "Wed, 05 Jun 2024 00:00:00 GMT"
    "Thu, 06 Jun 2024 00:00:00 GMT"
    "Fri, 07 Jun 2024 00:00:00 GMT"
    "Sat, 08 Jun 2024 00:00:00 GMT"
    "Sun, 09 Jun 2024 00:00:00 GMT"
    "Mon, 10 Jun 2024 00:00:00 GMT"
    "Tue, 11 Jun 2024 00:00:00 GMT"
    "Wed, 12 Jun 2024 00:00:00 GMT"
    "Thu, 13 Jun 2024 00:00:00 GMT"
    "Fri, 14 Jun 2024 00:00:00 GMT"
    "Sat, 15 Jun 2024 00:00:00 GMT"
    "Sun, 16 Jun 2024 00:00:00 GMT"
    "Mon, 17 Jun 2024 00:00:00 GMT"
    "Tue, 18 Jun 2024 00:00:00 GMT"
    "Wed, 19 Jun 2024 00:00:00 GMT"
    "Thu, 20 Jun 2024 00:00:00 GMT"
    "Fri, 21 Jun 2024 00:00:00 GMT"
    "Sat, 22 Jun 2024 00:00:00 GMT"
    "Sun, 23 Jun 2024 00:00:00 GMT"
    "Mon, 24 Jun 2024 00:00:00 GMT"
    "Tue, 25 Jun 2024 00:00:00 GMT"
    "Wed, 26 Jun 2024 00:00:00 GMT"
    "Thu, 27 Jun 2024 00:00:00 GMT"
    "Fri, 28 Jun 2024 00:00:00 GMT"
    "Sat, 29 Jun 2024 00:00:00 GMT"
    "Sun, 30 Jun 2024 00:00:00 GMT"
    "Mon, 01 Jul 2024 00:00:00 GMT"
    "Tue, 02 Jul 2024 00:00:00 GMT"
    "Wed, 03 Jul 2024 00:00:00 GMT"
    "Thu, 04 Jul 2024 00:00:00 GMT"
    "Fri, 05 Jul 2024 00:00:00 GMT"
    "Sat, 06 Jul 2024 00:00:00 GMT"
    "Sun, 07 Jul 2024 00:00:00 GMT"
    "Mon, 08 Jul 2024 00:00:00 GMT"
    "Tue, 09 Jul 2024 00:00:00 GMT"
    "Wed, 10 Jul 2024 00:00:00 GMT"
    "Thu, 11 Jul 2024 00:00:00 GMT"
    "Fri, 12 Jul 2024 00:00:00 GMT"
    "Sat, 13 Jul 2024 00:00:00 GMT"
    "Sun, 14 Jul 2024 00:00:00 GMT"
    "Mon, 15 Jul 2024 00:00:00 GMT"
    "Tue, 16 Jul 2024 00:00:00 GMT"
    "Wed, 17 Jul 2024 00:00:00 GMT"
    "Thu, 18 Jul 2024 00:00:00 GMT"
    "Fri, 19 Jul 2024 00:00:00 GMT"
    "Sat, 20 Jul 2024 00:00:00 GMT"
    "Sun, 21 Jul 2024 00:00:00 GMT"
    "Mon, 22 Jul 2024 00:00:00 GMT"
    "Tue, 23 Jul 2024 00:00:00 GMT"
    "Wed, 24 Jul 2024 00:00:00 GMT"
    "Thu, 25 Jul 2024 00:00:00 GMT"
    "Fri, 26 Jul 2024 00:00:00 GMT"
    "Sat, 27 Jul 2024 00:00:00 GMT"
    "Sun, 28 Jul 2024 00:00:00 GMT"
    "Mon, 29 Jul 2024 00:00:00 GMT"
    "Tue, 30 Jul 2024 00:00:00 GMT"
    "Wed, 31 Jul 2024 00:00:00 GMT"
    "Thu, 01 Aug 2024 00:00:00 GMT"
    "Fri, 02 Aug 2024 00:00:00 GMT"
    "Sat, 03 Aug 2024 00:00:00 GMT"
    "Sun, 04 Aug 2024 00:00:00 GMT"
    "Mon, 05 Aug 2024 00:00:00 GMT"
    "Tue, 06 Aug 2024 00:00:00 GMT"
    "Wed, 07 Aug 2024 00:00:00 GMT"
    "Thu, 08 Aug 2024 00:00:00 GMT"
    "Fri, 09 Aug 2024 00:00:00 GMT"
    "Sat, 10 Aug 2024 00:00:00 GMT"
    "Sun, 11 Aug 2024 00:00:00 GMT"
    "Mon, 12 Aug 2024 00:00:00 GMT"
    "Tue, 13 Aug 2024 00:00:00 GMT"
    "Wed, 14 Aug 2024 00:00:00 GMT"
    "Thu, 15 Aug 2024 00:00:00 GMT"
    "Fri, 16 Aug 2024 00:00:00 GMT"
    "Sat, 17 Aug 2024 00:00:00 GMT"
    "Sun, 18 Aug 2024 00:00:00 GMT"
    "Mon, 19 Aug 2024 00:00:00 GMT"
    "Tue, 20 Aug 2024 00:00:00 GMT"
    "Wed, 21 Aug 2024 00:00:00 GMT"
    "Thu, 22 Aug 2024 00:00:00 GMT"
    "Fri, 23 Aug 2024 00:00:00 GMT"
    "Sat, 24 Aug 2024 00:00:00 GMT"
    "Sun, 25 Aug 2024 00:00:00 GMT"
    "Mon, 26 Aug 2024 00:00:00 GMT"
    "Tue, 27 Aug 2024 00:00:00 GMT"
    "Wed, 28 Aug 2024 00:00:00 GMT"
    "Thu, 29 Aug 2024 00:00:00 GMT"
    "Fri, 30 Aug 2024 00:00:00 GMT"
    "Sat, 31 Aug 2024 00:00:00 GMT"
    "Sun, 01 Sep 2024 00:00:00 GMT"
    "Mon, 02 Sep 2024 00:00:00 GMT"
    "Tue, 03 Sep 2024 00:00:00 GMT"
    "Wed, 04 Sep 2024 00:00:00 GMT"
    "Thu, 05 Sep 2024 00:00:00 GMT"
    "Fri, 06 Sep 2024 00:00:00 GMT"
    "Sat, 07 Sep 2024 00:00:00 GMT"
    "Sun, 08 Sep 2024 00:00:00 GMT"
    "Mon, 09 Sep 2024 00:00:00 GMT"
    "Tue, 10 Sep 2024 00:00:00 GMT"
    "Wed, 11 Sep 2024 00:00:00 GMT"
    "Thu, 12 Sep 2024 00:00:00 GMT"
    "Fri, 13 Sep 2024 00:00:00 GMT"
    "Sat, 14 Sep 2024 00:00:00 GMT"
    "Sun, 15 Sep 2024 00:00:00 GMT"
    "Mon, 16 Sep 2024 00:00:00 GMT"
    "Tue, 17 Sep 2024 00:00:00 GMT"
    "Wed, 18 Sep 2024 00:00:00 GMT"
    "Thu, 19 Sep 2024 00:00:00 GMT"
    "Fri, 20 Sep 2024 00:00:00 GMT"
    "Sat, 21 Sep 2024 00:00:00 GMT"
    "Sun, 22 Sep 2024 00:00:00 GMT"
    "Mon, 23 Sep 2024 00:00:00 GMT"
    "Tue, 24 Sep 2024 00:00:00 GMT"
    "Wed, 25 Sep 2024 00:00:00 GMT"
    "Thu, 26 Sep 2024 00:00:00 GMT"
    "Fri, 27 Sep 2024 00:00:00 GMT"
    "Sat, 28 Sep 2024 00:00:00 GMT"
    "Sun, 29 Sep 2024 00:00:00 GMT"
    "Mon, 30 Sep 2024 00:00:00 GMT"
    "Tue, 01 Oct 2024 00:00:00 GMT"
    "Wed, 02 Oct 2024 00:00:00 GMT"
    "Thu, 03 Oct 2024 00:00:00 GMT"
    "Fri, 04 Oct 2024 00:00:00 GMT"
    "Sat, 05 Oct 2024 00:00:00 GMT"
    "Sun, 06 Oct 2024 00:00:00 GMT"
    "Mon, 07 Oct 2024 00:00:00 GMT"
    "Tue, 08 Oct 2024 00:00:00 GMT"
    "Wed, 09 Oct 2024 00:00:00 GMT"
    "Thu, 10 Oct 2024 00:00:00 GMT"
    "Fri, 11 Oct 2024 00:00:00 GMT"
    "Sat, 12 Oct 2024 00:00:00 GMT"
    "Sun, 13 Oct 2024 00:00:00 GMT"
    "Mon, 14 Oct 2024 00:00:00 GMT"
    "Tue, 15 Oct 2024 00:00:00 GMT"
    "Wed, 16 Oct 2024 00:00:00 GMT"
    "Thu, 17 Oct 2024 00:00:00 GMT"
    "Fri, 18 Oct 2024 00:00:00 GMT"
    "Sat, 19 Oct 2024 00:00:00 GMT"
    "Sun, 20 Oct 2024 00:00:00 GMT"
    "Mon, 21 Oct 2024 00:00:00 GMT"
    "Tue, 22 Oct 2024 00:00:00 GMT"
    "Wed, 23 Oct 2024 00:00:00 GMT"
    "Thu, 24 Oct 2024 00:00:00 GMT"
    "Fri, 25 Oct 2024 00:00:00 GMT"
    "Sat, 26 Oct 2024 00:00:00 GMT"
    "Sun, 27 Oct 2024 00:00:00 GMT"
    "Mon, 28 Oct 2024 00:00:00 GMT"
    "Tue, 29 Oct 2024 00:00:00 GMT"
    "Wed, 30 Oct 2024 00:00:00 GMT"
    "Thu, 31 Oct 2024 00:00:00 GMT"
    "Fri, 01 Nov 2024 00:00:00 GMT"
    "Sat, 02 Nov 2024 00:00:00 GMT"
    "Sun, 03 Nov 2024 00:00:00 GMT"
    "Mon, 04 Nov 2024 00:00:00 GMT"
    "Tue, 05 Nov 2024 00:00:00 GMT"
    "Wed, 06 Nov 2024 00:00:00 GMT"
    "Thu, 07 Nov 2024 00:00:00 GMT"
    "Fri, 08 Nov 2024 00:00:00 GMT"
    "Sat, 09 Nov 2024 00:00:00 GMT"
    "Sun, 10 Nov 2024 00:00:00 GMT"
    "Mon, 11 Nov 2024 00:00:00 GMT"
    "Tue, 12 Nov 2024 00:00:00 GMT"
    "Wed, 13 Nov 2024 00:00:00 GMT"
    "Thu, 14 Nov 2024 00:00:00 GMT"
    "Fri, 15 Nov 2024 00:00:00 GMT"
    "Sat, 16 Nov 2024 00:00:00 GMT"
    "Sun, 17 Nov 2024 00:00:00 GMT"
    "Mon, 18 Nov 2024 00:00:00 GMT"
    "Tue, 19 Nov 2024 00:00:00 GMT"
    "Wed, 20 Nov 2024 00:00:00 GMT"
    "Thu, 21 Nov 2024 00:00:00 GMT"
    "Fri, 22 Nov 2024 00:00:00 GMT"
    "Sat, 23 Nov 2024 00:00:00 GMT"
    "Sun, 24 Nov 2024 00:00:00 GMT"
    "Mon, 25 Nov 2024 00:00:00 GMT"
    "Tue, 26 Nov 2024 00:00:00 GMT"
    "Wed, 27 Nov 2024 00:00:00 GMT"
    "Thu, 28 Nov 2024 00:00:00 GMT"
    "Fri, 29 Nov 2024 00:00:00 GMT"
    "Sat, 30 Nov 2024 00:00:00 GMT"
    "Sun, 01 Dec 2024 00:00:00 GMT"
    "Mon, 02 Dec 2024 00:00:00 GMT"
    "Tue, 03 Dec 2024 00:00:00 GMT"
    "Wed, 04 Dec 2024 00:00:00 GMT"
    "Thu, 05 Dec 2024 00:00:00 GMT"
    "Fri, 06 Dec 2024 00:00:00 GMT"
    "Sat, 07 Dec 2024 00:00:00 GMT"
    "Sun, 08 Dec 2024 00:00:00 GMT"
    "Mon, 09 Dec 2024 00:00:00 GMT"
    "Tue, 10 Dec 2024 00:00:00 GMT"
    "Wed, 11 Dec 2024 00:00:00 GMT"
    "Thu, 12 Dec 2024 00:00:00 GMT"
    "Fri, 13 Dec 2024 00:00:00 GMT"
    "Sat, 14 Dec 2024 00:00:00 GMT"
    "Sun, 15 Dec 2024 00:00:00 GMT"
    "Mon, 16 Dec 2024 00:00:00 GMT"
    "Tue, 17 Dec 2024 00:00:00 GMT"
    "Wed, 18 Dec 2024 00:00:00 GMT"
    "Thu, 19 Dec 2024 00:00:00 GMT"
    "Fri, 20 Dec 2024 00:00:00 GMT"
    "Sat, 21 Dec 2024 00:00:00 GMT"
    "Sun, 22 Dec 2024 00:00:00 GMT"
    "Mon, 23 Dec 2024 00:00:00 GMT"
    "Tue, 24 Dec 2024 00:00:00 GMT"
    "Wed, 25 Dec 2024 00:00:00 GMT"
    "Thu, 26 Dec 2024 00:00:00 GMT"
    "Fri, 27 Dec 2024 00:00:00 GMT"
    "Sat, 28 Dec 2024 00:00:00 GMT"
    "Sun, 29 Dec 2024 00:00:00 GMT"
    "Mon, 30 Dec 2024 00:00:00 GMT"
    "Tue, 31 Dec 2024 00:00:00 GMT";

#define HTTP_DATE_DAY_COUNT 731

static void test_http_date(void)
{
    static const struct
    {
        u64 epoch;
        const char *want;
    } known[] = {
        {0, "Thu, 01 Jan 1970 00:00:00 GMT"},
        {951782400, "Tue, 29 Feb 2000 00:00:00 GMT"},
        {1700000000, "Tue, 14 Nov 2023 22:13:20 GMT"},
        {2147483647, "Tue, 19 Jan 2038 03:14:07 GMT"},
        {4102444800, "Fri, 01 Jan 2100 00:00:00 GMT"},
        {4107456000, "Sun, 28 Feb 2100 00:00:00 GMT"},
        {4107542400, "Mon, 01 Mar 2100 00:00:00 GMT"},
        {1709164800, "Thu, 29 Feb 2024 00:00:00 GMT"},
        {253402300799ULL, "Fri, 31 Dec 9999 23:59:59 GMT"},
    };
    char buf[64];
    size_t i, n;
    int good;

    good = 1;
    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++)
    {
        n = tbt_http_http_date(buf, sizeof(buf), known[i].epoch);
        if (n != TBT_HTTP_DATE_LEN || memcmp(buf, known[i].want, TBT_HTTP_DATE_LEN) != 0)
            good = 0;
    }
    ok(good, "IMF-fixdate known epochs", NULL);

    good = 1;
    for (i = 0; i < HTTP_DATE_DAY_COUNT; i++)
    {
        u64 epoch = 1672531200ULL + (u64)i * 86400ULL;

        n = tbt_http_http_date(buf, sizeof(buf), epoch);
        if (n != TBT_HTTP_DATE_LEN ||
            memcmp(buf, http_date_days + i * TBT_HTTP_DATE_LEN, TBT_HTTP_DATE_LEN) != 0)
            good = 0;
    }
    ok(good, "every day boundary of 2023-2024 matches date -u", NULL);

    ok(tbt_http_http_date(buf, TBT_HTTP_DATE_LEN - 1, 0) == 0, "date cap 28 is refused", NULL);
    memset(buf, 0xa5, sizeof(buf));
    ok(tbt_http_http_date(buf, TBT_HTTP_DATE_LEN, 0) == TBT_HTTP_DATE_LEN &&
           (unsigned char)buf[TBT_HTTP_DATE_LEN] == 0xa5,
       "date cap 29 writes exactly 29 bytes", NULL);
    ok(tbt_http_http_date(buf, sizeof(buf), 253402300800ULL) == 0, "year 10000 is refused",
       NULL);
    ok(tbt_http_http_date(buf, sizeof(buf), (u64)-1) == 0, "huge epoch is refused", NULL);
    ok(tbt_http_http_date(NULL, TBT_HTTP_DATE_LEN, 0) == 0, "NULL date buffer is refused", NULL);
}

/* ------------------------------------------------------------------------- */
/* Request block.                                                             */

static const char ua_string[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0 "
    "Safari/537.36";

static void test_request(void)
{
    struct tbt_http_cfg cfg;
    struct req_view v;
    char buf[TBT_HTTP_REQ_MAX + 32];
    static const char *const names[] = {
        "Host", "User-Agent", "Accept", "Accept-Encoding",
        "Content-Type", "Transfer-Encoding", "Connection", "X-Request-Id",
    };
    static const char *const values[] = {
        "203.0.113.9", ua_string, "*/*", "identity",
        "application/gzip", "chunked", "keep-alive",
        "abcdefghijklmnopqrstuvwxyz",
    };
    size_t n, i, m;
    int good;

    cfg_set(&cfg, "203.0.113.9", "abcdefghijklmnopqrstuvwxyz");
    n = tbt_http_req_header(buf, sizeof(buf), &cfg);
    ok(n > 0, "request header is emitted", NULL);
    ok(n < TBT_HTTP_REQ_MAX, "request header fits TBT_HTTP_REQ_MAX", NULL);
    if (!n)
        return;

    ok(strict_req(buf, n, &v) == 0, "request block is one strict HTTP/1.1 request", NULL);
    ok(slice_eq(v.method, v.method_len, "POST"), "method is POST", NULL);
    ok(slice_eq(v.target, v.target_len, "/api/v2/upload"), "target is /api/v2/upload", NULL);
    ok(slice_eq(v.version, v.version_len, "HTTP/1.1"), "version is HTTP/1.1", NULL);
    ok(v.nh == 8, "request has exactly eight headers", NULL);
    good = v.nh == 8;
    for (i = 0; good && i < 8; i++)
        good = slice_eq(v.h[i].name, v.h[i].name_len, names[i]);
    ok(good, "header names and order match DESIGN 3.2", NULL);
    good = v.nh == 8;
    for (i = 0; good && i < 8; i++)
        good = slice_eq(v.h[i].val, v.h[i].val_len, values[i]);
    ok(good, "header values match DESIGN 3.2", NULL);
    ok(v.header_len == n, "strict parser consumes the whole block", NULL);
    ok(n >= 2 && !memcmp(buf + n - 2, "\r\n", 2), "block ends with CRLF", NULL);
    ok(!memcmp(buf + n - 4, "\r\n\r\n", 4), "block ends with a blank line", NULL);

    good = 1;
    for (i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)buf[i];

        if (c == '\r' || c == '\n')
            continue;
        if (c < 0x20 || c >= 0x7f)
            good = 0;
    }
    ok(good, "header block is printable ASCII, no binary", NULL);
    ok(!contains_ci(buf, n, "tbtun"), "header block never says tbtun", NULL);
    ok(!contains_ci(buf, n, "date:"), "request carries no Date header (3.2 ruling)", NULL);

    /* Host with an explicit port; everything else stays the same. */
    cfg_set(&cfg, "203.0.113.9:8443", "abcdefghijklmnopqrstuvwxyz");
    m = tbt_http_req_header(buf, sizeof(buf), &cfg);
    ok(m == n + 5, "host with port adds exactly :8443", NULL);
    ok(strict_req(buf, m, &v) == 0 && v.nh == 8 &&
           slice_eq(v.h[0].val, v.h[0].val_len, "203.0.113.9:8443"),
       "Host carries the IPv4 literal and port", NULL);

    /* Base32 digits are legal in the id; the longest IPv4:port literal fits. */
    cfg_set(&cfg, "255.255.255.255:65535", "abcd234567abcdefghijklmnop");
    m = tbt_http_req_header(buf, sizeof(buf), &cfg);
    ok(m > 0 && m < TBT_HTTP_REQ_MAX, "maximal Host literal fits TBT_HTTP_REQ_MAX", NULL);
    ok(strict_req(buf, m, &v) == 0 && v.nh == 8 &&
           slice_eq(v.h[0].val, v.h[0].val_len, "255.255.255.255:65535") &&
           slice_eq(v.h[7].val, v.h[7].val_len, "abcd234567abcdefghijklmnop"),
       "base32 digits and the longest Host literal are accepted", NULL);

    /* A short buffer is a clean failure, never a truncated header. */
    cfg_set(&cfg, "203.0.113.9", "abcdefghijklmnopqrstuvwxyz");
    n = tbt_http_req_header(buf, sizeof(buf), &cfg);
    good = 1;
    for (i = 0; i < n; i++)
    {
        size_t k;

        memset(buf, 0xa5, sizeof(buf));
        if (tbt_http_req_header(buf, i, &cfg) != 0)
            good = 0;
        for (k = i; k < i + 8 && k < sizeof(buf); k++)
        {
            if ((unsigned char)buf[k] != 0xa5)
                good = 0;
        }
    }
    ok(good, "short cap returns 0 and never writes past cap", NULL);
    memset(buf, 0xa5, sizeof(buf));
    ok(tbt_http_req_header(buf, n, &cfg) == n, "exact cap succeeds", NULL);
    ok((unsigned char)buf[n] == 0xa5, "exact cap writes no NUL", NULL);

    /* Invalid hosts are refused before any byte is written. */
    {
        static const char *const bad[] = {
            "", "example.com", "10.0.0.256", "10.0.0.01", "10.0.0.1:0",
            "10.0.0.1:", "10.0.0.1:65536", "[::1]:443", "10.0.0.1:080",
            "10.0.0", "10.0.0.1.2", "10.0.0.-1", "10.0.0.1 x",
            "1.2.3.4:5:6", "010.0.0.1",
        };

        good = 1;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        {
            cfg_set(&cfg, bad[i], "abcdefghijklmnopqrstuvwxyz");
            if (tbt_http_req_header(buf, sizeof(buf), &cfg) != 0)
                good = 0;
        }
        ok(good, "invalid Host literals are refused", NULL);
    }

    /* A host field without a NUL inside its bound is refused, not scanned. */
    memset(&cfg, 0, sizeof(cfg));
    memset(cfg.host, '1', sizeof(cfg.host));
    memcpy(cfg.request_id, "abcdefghijklmnopqrstuvwxyz", 27);
    ok(tbt_http_req_header(buf, sizeof(buf), &cfg) == 0, "unterminated host is refused", NULL);

    /* Request id rules: 26 chars, lowercase base32 only. */
    {
        static const char *const bad[] = {
            "", "abcdefghijklmnopqrstuvwxy", "abcdefghijklmnopqrstuvwxy0",
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxy1",
            "abcdefghijklmnopqrstuvwxy8",
        };

        good = 1;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        {
            cfg_set(&cfg, "203.0.113.9", bad[i]);
            if (tbt_http_req_header(buf, sizeof(buf), &cfg) != 0)
                good = 0;
        }
        ok(good, "invalid X-Request-Id values are refused", NULL);
    }
    cfg_set(&cfg, "203.0.113.9", "abcdefghijklmnopqrstuvwxyz");
    cfg.request_id[TBT_HTTP_REQID_LEN] = 'x';
    ok(tbt_http_req_header(buf, sizeof(buf), &cfg) == 0, "over-long request id is refused", NULL);

    ok(tbt_http_req_header(NULL, sizeof(buf), &cfg) == 0, "NULL buffer is refused", NULL);
    ok(tbt_http_req_header(buf, sizeof(buf), NULL) == 0, "NULL cfg is refused", NULL);
}

static size_t make_req(char *buf, size_t cap, const char *host, const char *id, const char *extra)
{
    int n = snprintf(buf, cap,
                     "POST /api/v2/upload HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: %s\r\n"
                     "Accept: */*\r\n"
                     "Accept-Encoding: identity\r\n"
                     "Content-Type: application/gzip\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "Connection: keep-alive\r\n"
                     "X-Request-Id: %s\r\n"
                     "%s"
                     "\r\n",
                     host, ua_string, id, extra ? extra : "");
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static void expect_req_bad(const char *what, const char *text)
{
    struct tbt_http_req r;
    int err;

    memset(&r, 0, sizeof(r));
    r.header_len = 777;
    ok(tbt_http_req_parse(text, strlen(text), &r, &err) == 0 && err == TBT_HTTP_PROTO &&
           r.header_len == 777,
       what, "not rejected");
}

static void test_req_parse(void)
{
    struct tbt_http_cfg cfg;
    struct tbt_http_req r;
    char buf[TBT_HTTP_REQ_MAX];
    char mut[TBT_HTTP_REQ_MAX + 32];
    char big[TBT_HTTP_HDR_MAX + 64];
    size_t n, i;
    int err, good;

    cfg_set(&cfg, "203.0.113.9:8443", "abcd234567abcdefghijklmnop");
    n = tbt_http_req_header(buf, sizeof(buf), &cfg);
    ok(n > 0, "request parser fixture builds", NULL);
    if (!n)
        return;

    memset(&r, 0, sizeof(r));
    ok(tbt_http_req_parse(buf, n, &r, &err) == n && err == TBT_HTTP_OK && r.chunked &&
           r.keep_alive && r.host_len == strlen(cfg.host) &&
           !memcmp(r.host, cfg.host, r.host_len) && r.header_len == n,
       "request parser accepts the emitted block", NULL);

    /* Every prefix is NEED_MORE and leaves out untouched. */
    good = 1;
    for (i = 0; i < n; i++)
    {
        struct tbt_http_req p;
        int e;

        memset(&p, 0, sizeof(p));
        p.header_len = 777;
        if (tbt_http_req_parse(buf, i, &p, &e) != 0 || e != TBT_HTTP_NEED_MORE ||
            p.header_len != 777)
            good = 0;
    }
    ok(good, "request parser split at every byte is NEED_MORE", NULL);

    /* Body bytes after the header block are not consumed. */
    memcpy(mut, buf, n);
    memcpy(mut + n, "hello", 5);
    memset(&r, 0, sizeof(r));
    ok(tbt_http_req_parse(mut, n + 5, &r, &err) == n && err == TBT_HTTP_OK,
       "request parser stops at the header end", NULL);

    /* Host without a port, and a base32 id with digits. */
    n = make_req(buf, sizeof(buf), "203.0.113.9", "abcd234567abcdefghijklmnop", NULL);
    memset(&r, 0, sizeof(r));
    ok(n && tbt_http_req_parse(buf, n, &r, &err) == n && err == TBT_HTTP_OK &&
           r.host_len == 11 && !memcmp(r.host, "203.0.113.9", 11),
       "request parser accepts Host without a port", NULL);

    /* Mutations of the emitted block. */
    n = tbt_http_req_header(buf, sizeof(buf), &cfg);
    memcpy(mut, buf, n);
    mut[n] = '\0';

    memcpy(mut, "PUTT", 4);
    expect_req_bad("request: wrong method", mut);

    memcpy(mut, buf, n);
    mut[n] = '\0';
    {
        char *v = strstr(mut, "HTTP/1.1");

        memcpy(v, "HTTP/1.0", 8);
    }
    expect_req_bad("request: wrong version", mut);

    memcpy(mut, buf, n);
    mut[n] = '\0';
    {
        char *t = strstr(mut, "/api/v2/upload");

        memcpy(t, "/api/v2/uploaX", 14);
    }
    expect_req_bad("request: wrong target", mut);

    memcpy(mut, buf, n);
    mut[n] = '\0';
    memcpy(mut, "host", 4);
    expect_req_bad("request: lowercase Host name", mut);

    memcpy(mut, buf, n);
    mut[n] = '\0';
    {
        char *lf = strstr(mut, "\r\n");

        *lf = '\n'; /* the next byte is still LF: a bare LF in the request line */
    }
    expect_req_bad("request: bare LF", mut);

    /* Remove the Host line entirely. */
    memcpy(mut, buf, n);
    mut[n] = '\0';
    {
        char *host = strstr(mut, "Host: ");
        char *end = host ? strstr(host, "\r\n") : NULL;

        if (host && end)
            memmove(host, end + 2, strlen(end + 2) + 1);
    }
    expect_req_bad("request: missing Host", mut);

    /* Remove the Transfer-Encoding line. */
    memcpy(mut, buf, n);
    mut[n] = '\0';
    {
        char *te = strstr(mut, "Transfer-Encoding: chunked\r\n");
        size_t len = strlen("Transfer-Encoding: chunked\r\n");

        memmove(te, te + len, strlen(te + len) + 1);
    }
    expect_req_bad("request: missing Transfer-Encoding", mut);

    /* A non-chunked value and a closed connection, same byte lengths. */
    memcpy(mut, buf, n);
    mut[n] = '\0';
    memcpy(strstr(mut, "chunked"), "gzip   ", 7);
    expect_req_bad("request: non-chunked", mut);

    memcpy(mut, buf, n);
    mut[n] = '\0';
    memcpy(strstr(mut, "keep-alive"), "close     ", 10);
    expect_req_bad("request: Connection close", mut);

    expect_req_bad("request: empty Host",
                   make_req(mut, sizeof(mut), "", "abcd234567abcdefghijklmnop", NULL) ? mut : "");
    expect_req_bad("request: hostname Host",
                   make_req(mut, sizeof(mut), "example.com", "abcd234567abcdefghijklmnop", NULL)
                       ? mut
                       : "");
    expect_req_bad("request: invalid Host octet",
                   make_req(mut, sizeof(mut), "10.0.0.256", "abcd234567abcdefghijklmnop", NULL)
                       ? mut
                       : "");
    expect_req_bad("request: short X-Request-Id",
                   make_req(mut, sizeof(mut), "203.0.113.9", "short", NULL) ? mut : "");
    expect_req_bad("request: uppercase X-Request-Id",
                   make_req(mut, sizeof(mut), "203.0.113.9", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", NULL)
                       ? mut
                       : "");
    expect_req_bad("request: extra header",
                   make_req(mut, sizeof(mut), "203.0.113.9", "abcd234567abcdefghijklmnop",
                            "X-Foo: bar\r\n")
                       ? mut
                       : "");
    expect_req_bad("request: duplicate Host",
                   make_req(mut, sizeof(mut), "203.0.113.9", "abcd234567abcdefghijklmnop",
                            "Host: 203.0.113.9\r\n")
                       ? mut
                       : "");
    expect_req_bad("request: Content-Length",
                   make_req(mut, sizeof(mut), "203.0.113.9", "abcd234567abcdefghijklmnop",
                            "Content-Length: 0\r\n")
                       ? mut
                       : "");
    expect_req_bad("request: Expect",
                   make_req(mut, sizeof(mut), "203.0.113.9", "abcd234567abcdefghijklmnop",
                            "Expect: 100-continue\r\n")
                       ? mut
                       : "");

    /* Header order is part of the shape. */
    snprintf(mut, sizeof(mut),
             "POST /api/v2/upload HTTP/1.1\r\n"
             "User-Agent: %s\r\n"
             "Host: 203.0.113.9\r\n"
             "Accept: */*\r\n"
             "Accept-Encoding: identity\r\n"
             "Content-Type: application/gzip\r\n"
             "Transfer-Encoding: chunked\r\n"
             "Connection: keep-alive\r\n"
             "X-Request-Id: abcd234567abcdefghijklmnop\r\n"
             "\r\n",
             ua_string);
    expect_req_bad("request: header order", mut);

    /* A header block that never terminates inside TBT_HTTP_HDR_MAX. */
    {
        int p = snprintf(big, sizeof(big), "POST /api/v2/upload HTTP/1.1\r\nHost: 203.0.113.9\r\nX: ");

        memset(big + p, 'a', sizeof(big) - (size_t)p);
        big[sizeof(big) - 1] = '\0';
        expect_req_bad("request: over-long header block", big);
    }

    ok(tbt_http_req_parse(NULL, 0, &r, &err) == 0 && err == TBT_HTTP_PROTO,
       "request parser refuses a NULL buffer", NULL);
    ok(tbt_http_req_parse(buf, n, NULL, &err) == 0 && err == TBT_HTTP_PROTO,
       "request parser refuses a NULL out", NULL);
}

static void test_skip_header(void)
{
    struct tbt_http_cfg cfg;
    char buf[TBT_HTTP_REQ_MAX + 16];
    char big[TBT_HTTP_HDR_MAX + 16];
    size_t n, i, used;
    int err, good;

    cfg_set(&cfg, "203.0.113.9", "abcdefghijklmnopqrstuvwxyz");
    n = tbt_http_req_header(buf, sizeof(buf), &cfg);
    ok(n > 0, "skip fixture builds", NULL);
    if (!n)
        return;

    memcpy(buf + n, "body", 4);
    used = tbt_http_skip_request_header((const u8 *)buf, n + 4, &err);
    ok(used == n && err == TBT_HTTP_OK, "skip returns the header length, not the body", NULL);

    good = 1;
    for (i = 0; i < n; i++)
    {
        if (tbt_http_skip_request_header((const u8 *)buf, i, &err) != 0 ||
            err != TBT_HTTP_NEED_MORE)
            good = 0;
    }
    ok(good, "skip split at every byte is NEED_MORE", NULL);

    /* A bare LF is not a terminator; the later CRLFCRLF still is. */
    {
        static const char mixed[] = "GET / HTTP/1.1\nHost: x\r\n\r\n";

        used = tbt_http_skip_request_header((const u8 *)mixed, sizeof(mixed) - 1, &err);
        ok(used == sizeof(mixed) - 1 && err == TBT_HTTP_OK,
           "skip finds CRLFCRLF and ignores a bare LF", NULL);
    }

    ok(tbt_http_skip_request_header((const u8 *)"\r\n\r\n", 4, &err) == 4 &&
           err == TBT_HTTP_OK,
       "skip accepts an immediate blank line", NULL);
    ok(tbt_http_skip_request_header((const u8 *)"\r\n\r", 3, &err) == 0 &&
           err == TBT_HTTP_NEED_MORE,
       "skip needs the last LF", NULL);

    /* The terminator must start within the first TBT_HTTP_HDR_MAX bytes. */
    memset(big, 'a', sizeof(big));
    memcpy(big + TBT_HTTP_HDR_MAX - 4, "\r\n\r\n", 4);
    ok(tbt_http_skip_request_header((const u8 *)big, TBT_HTTP_HDR_MAX, &err) ==
               TBT_HTTP_HDR_MAX &&
           err == TBT_HTTP_OK,
       "skip accepts a terminator ending at the bound", NULL);
    memcpy(big + TBT_HTTP_HDR_MAX - 3, "\r\n\r\n", 4);
    ok(tbt_http_skip_request_header((const u8 *)big, TBT_HTTP_HDR_MAX + 1, &err) == 0 &&
           err == TBT_HTTP_PROTO,
       "skip rejects a terminator that starts past the bound", NULL);
    ok(tbt_http_skip_request_header((const u8 *)big, TBT_HTTP_HDR_MAX, &err) == 0 &&
           err == TBT_HTTP_PROTO,
       "skip rejects an unterminated block at the bound", NULL);

    memset(big, 'a', sizeof(big));
    ok(tbt_http_skip_request_header((const u8 *)big, TBT_HTTP_HDR_MAX - 1, &err) == 0 &&
           err == TBT_HTTP_NEED_MORE,
       "skip needs more below the bound", NULL);
    ok(tbt_http_skip_request_header(NULL, 0, &err) == 0 && err == TBT_HTTP_NEED_MORE,
       "skip accepts an empty prefix", NULL);
    ok(tbt_http_skip_request_header(NULL, 1, &err) == 0 && err == TBT_HTTP_PROTO,
       "skip refuses a NULL buffer with bytes", NULL);
}

/* The far's response builder and the near's response parser are the two ends of
 * one wire contract, and the media type is the part of it that only has to look
 * like a real upload. This is the module-to-module case: no busybox, no socket,
 * just the block the far emits read back by the parser the near runs. */
static void test_far_response_to_near_parser(void)
{
    struct tbt_http_resp_cfg cfg = { .unix_secs = 1767225600 }; /* 2026-01-01 */
    struct tbt_http_resp r = {};
    char hdr[TBT_HTTP_RESP_MAX];
    size_t h;
    int err = -1;

    h = tbt_http_resp_header(hdr, sizeof(hdr), &cfg);
    ok(h > 0 && h <= sizeof(hdr), "far response builder emits a block", NULL);
    ok(strstr(hdr, "Content-Type: " TBT_HTTP_CONTENT_TYPE "\r\n") != NULL,
       "response block names the container media type", TBT_HTTP_CONTENT_TYPE);
    ok(strstr(hdr, "Content-Encoding") == NULL,
       "response block sends no Content-Encoding", NULL);
    ok(tbt_http_resp_parse(hdr, h, &r, &err) == h && err == TBT_HTTP_OK,
       "near parser accepts the far's response block", NULL);
    ok(r.status == 200 && r.chunked && r.keep_alive,
       "accepted response is 200, chunked and keep-alive", NULL);
    ok(r.header_len == h, "accepted response consumes the whole block", NULL);
}

static size_t make_resp_msg(char *buf, size_t cap, u64 epoch, const char *body, size_t blen)
{
    struct tbt_http_resp_cfg cfg = { .unix_secs = epoch };
    char hdr[TBT_HTTP_RESP_MAX];
    size_t h = tbt_http_resp_header(hdr, sizeof(hdr), &cfg);
    size_t o = 0, n;

    if (!h || h > cap)
        return 0;
    memcpy(buf, hdr, h);
    o = h;
    n = tbt_http_chunk_hdr(buf + o, cap - o, blen);
    if (!n)
        return 0;
    o += n;
    memcpy(buf + o, body, blen);
    o += blen;
    n = tbt_http_chunk_tail(buf + o, cap - o);
    if (!n)
        return 0;
    o += n;
    n = tbt_http_chunk_hdr(buf + o, cap - o, 0);
    if (!n)
        return 0;
    o += n;
    n = tbt_http_chunk_tail(buf + o, cap - o);
    if (!n)
        return 0;
    return o + n;
}

static void test_msg_loop(void)
{
    struct tbt_http_msg m = {};
    char m1[TBT_HTTP_RESP_MAX + 64];
    char m2[TBT_HTTP_RESP_MAX + 64];
    char both[2 * (TBT_HTTP_RESP_MAX + 64)];
    char got[2][8] = { { 0 } };
    size_t got_len[2] = { 0, 0 };
    size_t n1, n2, total, pos = 0;
    int err, msgs = 0, good = 1;

    n1 = make_resp_msg(m1, sizeof(m1), 1700000000, "a", 1);
    n2 = make_resp_msg(m2, sizeof(m2), 1700000001, "b", 1);
    ok(n1 && n2, "message loop fixtures build", NULL);
    if (!n1 || !n2)
        return;
    memcpy(both, m1, n1);
    memcpy(both + n1, m2, n2);
    total = n1 + n2;

    while (pos < total && msgs < 2 && good)
    {
        if (m.hdr_len == 0)
        {
            size_t n = tbt_http_resp_parse(both + pos, total - pos, &m.resp, &err);

            if (n == 0 || err != TBT_HTTP_OK || !m.resp.chunked)
            {
                good = 0;
                break;
            }
            m.hdr_len = n;
            pos += n;
        }
        else
        {
            const u8 *b = NULL;
            size_t bl = 0;
            size_t n = tbt_http_chunk_feed(&m.chunk, (const u8 *)both + pos, total - pos, &b, &bl,
                                           &err);

            if (n > total - pos || err == TBT_HTTP_PROTO || err == TBT_HTTP_NEED_MORE || !n)
            {
                good = 0;
                break;
            }
            if (bl)
            {
                memcpy(got[msgs] + got_len[msgs], b, bl);
                got_len[msgs] += bl;
            }
            pos += n;
            if (m.chunk.state == TBT_HTTP_CHUNK_DONE)
            {
                msgs++;
                tbt_http_msg_reset(&m);
            }
        }
    }

    ok(good && msgs == 2 && pos == total && got_len[0] == 1 && got[0][0] == 'a' &&
           got_len[1] == 1 && got[1][0] == 'b',
       "documented receiver loop decodes two responses", NULL);
    ok(m.hdr_len == 0 && m.chunk.state == TBT_HTTP_CHUNK_SIZE,
       "reset returns the message to the header stage", NULL);

    m.hdr_len = 123;
    m.resp.status = 500;
    m.chunk.state = TBT_HTTP_CHUNK_BAD;
    tbt_http_msg_reset(&m);
    ok(m.hdr_len == 0 && m.resp.status == 0 && m.chunk.state == TBT_HTTP_CHUNK_SIZE,
       "message reset clears all stages", NULL);
    tbt_http_msg_reset(NULL);
    ok(1, "message reset tolerates NULL", NULL);
}

/* ------------------------------------------------------------------------- */
/* Response status line and headers.                                          */

static void expect_resp_bad(const char *what, const char *s)
{
    struct tbt_http_resp r;
    int err;

    memset(&r, 0, sizeof(r));
    r.header_len = 12345;
    ok(tbt_http_resp_parse(s, strlen(s), &r, &err) == 0 && err == TBT_HTTP_PROTO,
       what, "not rejected");
    ok(r.header_len == 12345, what, "out was written on PROTO");
}

static void test_response(void)
{
    static const char good[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/gzip\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: keep-alive\r\n"
        "Server: nginx\r\n"
        "Date: Wed, 21 Oct 2015 07:28:00 GMT\r\n"
        "X-Unknown: whatever\r\n"
        "\r\n";
    static const char body[] = "1\r\na\r\n0\r\n\r\n";
    struct tbt_http_resp r;
    char *joined;
    size_t len = sizeof(good) - 1, i;
    int err;

    memset(&r, 0, sizeof(r));
    ok(tbt_http_resp_parse(good, len, &r, &err) == (size_t)len && err == TBT_HTTP_OK,
       "response header block parses", NULL);
    ok(r.status == 200 && r.chunked && r.keep_alive && !r.connection_close,
       "response fields are 200/chunked/keep-alive", NULL);
    ok(r.header_len == len, "header_len covers status line and blank line", NULL);

    memset(&r, 0, sizeof(r));
    r.header_len = 777;
    ok(tbt_http_resp_parse(good, 10, &r, &err) == 0 && err == TBT_HTTP_NEED_MORE &&
           r.header_len == 777,
       "NEED_MORE leaves out untouched", NULL);

    /* Every prefix must be NEED_MORE, never PROTO or a guess. */
    for (i = 0; i < len; i++)
    {
        struct tbt_http_resp p;
        int e;

        memset(&p, 0, sizeof(p));
        p.header_len = 777;
        if (tbt_http_resp_parse(good, i, &p, &e) != 0 || e != TBT_HTTP_NEED_MORE || p.header_len != 777)
        {
            ok(0, "response prefix split at every byte is NEED_MORE", NULL);
            return;
        }
    }
    ok(1, "response prefix split at every byte is NEED_MORE", NULL);

    /* Body bytes after the header block are not consumed by the parser. */
    joined = malloc(len + sizeof(body));
    if (!joined)
    {
        ok(0, "response with body allocates", NULL);
        return;
    }
    memcpy(joined, good, len);
    memcpy(joined + len, body, sizeof(body) - 1);
    memset(&r, 0, sizeof(r));
    ok(tbt_http_resp_parse(joined, len + sizeof(body) - 1, &r, &err) == len && err == TBT_HTTP_OK,
       "parser stops at the end of the header block", NULL);
    free(joined);

    /* Header names are case-insensitive on receive; values are trimmed. */
    {
        static const char s[] =
            "HTTP/1.1 200 OK\r\ntRaNsFeR-EnCoDiNg:   CHUNKED  \r\n\r\n";

        memset(&r, 0, sizeof(r));
        ok(tbt_http_resp_parse(s, sizeof(s) - 1, &r, &err) == sizeof(s) - 1 &&
               err == TBT_HTTP_OK && r.chunked,
           "Transfer-Encoding is case-insensitive and trims OWS", NULL);
    }
    {
        static const char s[] = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";

        memset(&r, 0, sizeof(r));
        ok(tbt_http_resp_parse(s, sizeof(s) - 1, &r, &err) == sizeof(s) - 1 &&
               err == TBT_HTTP_OK && !r.keep_alive && r.connection_close,
           "Connection: close disables keep-alive", NULL);
    }
    {
        static const char s[] = "HTTP/1.1 200 OK\r\nConnection: keep-alive, close\r\n\r\n";

        memset(&r, 0, sizeof(r));
        ok(tbt_http_resp_parse(s, sizeof(s) - 1, &r, &err) == sizeof(s) - 1 &&
               err == TBT_HTTP_OK && !r.keep_alive && r.connection_close,
           "Connection: close wins over keep-alive", NULL);
    }
    {
        static const char s[] = "HTTP/1.0 200 OK\r\n\r\n";

        memset(&r, 0, sizeof(r));
        ok(tbt_http_resp_parse(s, sizeof(s) - 1, &r, &err) == sizeof(s) - 1 &&
               err == TBT_HTTP_OK && !r.keep_alive,
           "HTTP/1.0 defaults to close", NULL);
    }
    {
        static const char s[] = "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\n";

        memset(&r, 0, sizeof(r));
        ok(tbt_http_resp_parse(s, sizeof(s) - 1, &r, &err) == sizeof(s) - 1 &&
               err == TBT_HTTP_OK && r.keep_alive,
           "HTTP/1.0 with Connection: keep-alive", NULL);
    }
    {
        static const char s[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";

        memset(&r, 0, sizeof(r));
        ok(tbt_http_resp_parse(s, sizeof(s) - 1, &r, &err) == sizeof(s) - 1 &&
               err == TBT_HTTP_OK && !r.chunked && r.has_content_length,
           "Content-Length alone parses, chunked is false", NULL);
    }
    {
        static const char s[] = "HTTP/1.1 200 OK\r\n\r\n";

        memset(&r, 0, sizeof(r));
        ok(tbt_http_resp_parse(s, sizeof(s) - 1, &r, &err) == sizeof(s) - 1 &&
               err == TBT_HTTP_OK && !r.chunked && !r.has_content_length,
           "empty header set parses", NULL);
    }

    expect_resp_bad("non-200 is rejected", "HTTP/1.1 404 Not Found\r\n\r\n");
    expect_resp_bad("204 is rejected", "HTTP/1.1 204 No Content\r\n\r\n");
    expect_resp_bad("bad status digit count", "HTTP/1.1 2000 OK\r\n\r\n");
    expect_resp_bad("bad version", "HTTP/2 200 OK\r\n\r\n");
    expect_resp_bad("bare LF status line", "HTTP/1.1 200 OK\n\n");
    expect_resp_bad("space before colon", "HTTP/1.1 200 OK\r\nBad Name: x\r\n\r\n");
    expect_resp_bad("obs-fold", "HTTP/1.1 200 OK\r\nX: a\r\n b\r\n\r\n");
    expect_resp_bad("TE with a coding before chunked",
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n");
    expect_resp_bad("TE chunked plus Content-Length",
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n");
    expect_resp_bad("non-numeric Content-Length",
                    "HTTP/1.1 200 OK\r\nContent-Length: 5x\r\n\r\n");
    expect_resp_bad("duplicate Transfer-Encoding",
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n");
    expect_resp_bad("control byte in a header value",
                    "HTTP/1.1 200 OK\r\nX: a\x01" "b\r\n\r\n");

    /* A block that never terminates inside TBT_HTTP_HDR_MAX is a protocol
     * error, not an invitation to scan the receive buffer forever. */
    {
        char *big = malloc(TBT_HTTP_HDR_MAX + 64);

        if (big)
        {
            int p = snprintf(big, TBT_HTTP_HDR_MAX + 64, "HTTP/1.1 200 OK\r\nX: ");

            memset(big + p, 'a', TBT_HTTP_HDR_MAX + 64 - (size_t)p);
            memset(&r, 0, sizeof(r));
            ok(tbt_http_resp_parse(big, TBT_HTTP_HDR_MAX + 64, &r, &err) == 0 &&
                   err == TBT_HTTP_PROTO,
               "over-long header block is rejected", NULL);
            free(big);
        }
    }
}

static void test_resp_header(void)
{
    struct tbt_http_resp_cfg cfg = { .unix_secs = 1700000000 };
    static const char want[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/gzip\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: keep-alive\r\n"
        "Server: nginx\r\n"
        "Date: Tue, 14 Nov 2023 22:13:20 GMT\r\n"
        "\r\n";
    char buf[TBT_HTTP_RESP_MAX + 32];
    struct tbt_http_resp r;
    size_t n, i;
    int err, good;

    n = tbt_http_resp_header(buf, sizeof(buf), &cfg);
    ok(n == sizeof(want) - 1, "response header has the DESIGN length", NULL);
    ok(n && !memcmp(buf, want, n), "response header matches DESIGN 3.2", NULL);
    ok(n < TBT_HTTP_RESP_MAX, "response header fits TBT_HTTP_RESP_MAX", NULL);

    memset(&r, 0, sizeof(r));
    ok(tbt_http_resp_parse(buf, n, &r, &err) == n && err == TBT_HTTP_OK && r.status == 200 &&
           r.chunked && r.keep_alive,
       "emitted response parses as 200/chunked/keep-alive", NULL);

    /* Split at every byte boundary. */
    good = 1;
    for (i = 0; i < n; i++)
    {
        struct tbt_http_resp p;
        int e;

        memset(&p, 0, sizeof(p));
        p.header_len = 777;
        if (tbt_http_resp_parse(buf, i, &p, &e) != 0 || e != TBT_HTTP_NEED_MORE ||
            p.header_len != 777)
            good = 0;
    }
    ok(good, "emitted response split at every byte is NEED_MORE", NULL);

    /* A short cap is a clean failure and never writes past the cap. */
    good = 1;
    for (i = 0; i < n; i++)
    {
        size_t k;

        memset(buf, 0xa5, sizeof(buf));
        if (tbt_http_resp_header(buf, i, &cfg) != 0)
            good = 0;
        for (k = i; k < i + 8 && k < sizeof(buf); k++)
        {
            if ((unsigned char)buf[k] != 0xa5)
                good = 0;
        }
    }
    ok(good, "short cap refuses the response header without overflow", NULL);
    memset(buf, 0xa5, sizeof(buf));
    ok(tbt_http_resp_header(buf, n, &cfg) == n, "exact cap accepts the response header", NULL);
    ok((unsigned char)buf[n] == 0xa5, "response header writes no NUL", NULL);

    /* The Date line follows the epoch input. */
    cfg.unix_secs = 0;
    n = tbt_http_resp_header(buf, sizeof(buf) - 1, &cfg);
    if (n)
        buf[n] = '\0';
    ok(n && strstr(buf, "Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n") != NULL,
       "response Date follows the epoch", NULL);

    cfg.unix_secs = 253402300800ULL;
    ok(tbt_http_resp_header(buf, sizeof(buf), &cfg) == 0,
       "out-of-range response Date is refused", NULL);
    ok(tbt_http_resp_header(NULL, sizeof(buf), &cfg) == 0, "NULL response buffer is refused",
       NULL);
    ok(tbt_http_resp_header(buf, sizeof(buf), NULL) == 0, "NULL response cfg is refused", NULL);
}

/* ------------------------------------------------------------------------- */
/* Chunk encoding.                                                            */

static void test_chunk_encode(void)
{
    static const struct
    {
        size_t n;
        const char *want;
    } cases[] = {
        {0, "0\r\n"},
        {1, "1\r\n"},
        {0x24, "24\r\n"},
        {0x800, "800\r\n"},
        {TBT_CHUNK_MAX, "2024\r\n"},
    };
    char buf[64];
    size_t i, n;
    int good;

    good = 1;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        n = tbt_http_chunk_hdr(buf, sizeof(buf), cases[i].n);
        if (n != strlen(cases[i].want) || memcmp(buf, cases[i].want, n))
            good = 0;
    }
    ok(good, "chunk size lines are lowercase hex with no leading zeros", NULL);

    /* Exhaustive canonical spelling over the whole legal range. */
    good = 1;
    for (i = 0; i <= TBT_CHUNK_MAX; i++)
    {
        size_t digits, j;

        n = tbt_http_chunk_hdr(buf, sizeof(buf), i);
        if (n < 3 || buf[n - 2] != '\r' || buf[n - 1] != '\n')
        {
            good = 0;
            break;
        }
        digits = n - 2;
        if (digits > 1 && buf[0] == '0')
            good = 0;
        for (j = 0; j < digits; j++)
        {
            if (!((buf[j] >= '0' && buf[j] <= '9') || (buf[j] >= 'a' && buf[j] <= 'f')))
                good = 0;
        }
        if (strtoul(buf, NULL, 16) != i)
            good = 0;
    }
    ok(good, "every legal chunk size has one canonical spelling", NULL);

    ok(tbt_http_chunk_hdr(buf, sizeof(buf), TBT_FEC_REC_MAX + 1) == 0,
       "chunk size above the v6 FEC record ceiling is refused", NULL);
    ok(tbt_http_chunk_hdr(buf, sizeof(buf), TBT_FEC_REC_MAX) > 0,
       "v6 FEC record ceiling is accepted", NULL);
    ok(tbt_http_chunk_hdr(buf, sizeof(buf), (size_t)-1) == 0,
       "oversized chunk size cannot wrap into range", NULL);
    ok(tbt_http_chunk_hdr(buf, 3, 0x24) == 0, "short cap refuses the size line", NULL);
    ok(tbt_http_chunk_hdr(buf, 4, 0x24) == 4, "exact cap accepts the size line", NULL);

    memcpy(buf, "\xa5\xa5", 2);
    ok(tbt_http_chunk_tail(buf, 2) == 2 && !memcmp(buf, "\r\n", 2), "chunk tail is CRLF", NULL);
    ok(tbt_http_chunk_tail(buf, 1) == 0, "short cap refuses the tail", NULL);

}

/* ------------------------------------------------------------------------- */
/* Chunk decoding.                                                            */

static void run_stream_p(struct tbt_http_chunk_parser *p, const char *s, size_t len, char *out,
                         size_t cap, size_t *out_len, u32 *state, int *err)
{
    size_t pos = 0, o = 0;

    *out_len = 0;
    *state = p->state;
    *err = TBT_HTTP_NEED_MORE;
    for (;;)
    {
        const u8 *b = NULL;
        size_t bl = 0, n;

        n = tbt_http_chunk_feed(p, (const u8 *)s + pos, len - pos, &b, &bl, err);
        *state = p->state;
        if (*err == TBT_HTTP_PROTO)
        {
            *out_len = o;
            return;
        }
        if (bl)
        {
            if (o + bl > cap)
            {
                *err = TBT_HTTP_PROTO;
                *out_len = o;
                return;
            }
            memcpy(out + o, b, bl);
            o += bl;
        }
        pos += n;
        *out_len = o;
        if (p->state == TBT_HTTP_CHUNK_DONE)
            return;
        if (*err == TBT_HTTP_NEED_MORE)
            return;
        if (!n)
        {
            *err = TBT_HTTP_PROTO; /* a parser that does not advance is a bug */
            return;
        }
    }
    /* The last body run consumed all input while the chunk was still open.
     * Ask once with no bytes so the reported status is the parser's real one
     * (NEED_MORE) rather than the OK of the body yield. */
    {
        const u8 *b = NULL;
        size_t bl = 0;

        (void)tbt_http_chunk_feed(p, NULL, 0, &b, &bl, err);
        *state = p->state;
    }
}

static void run_stream(const char *s, size_t len, char *out, size_t cap, size_t *out_len,
                       u32 *state, int *err)
{
    struct tbt_http_chunk_parser p = {};

    run_stream_p(&p, s, len, out, cap, out_len, state, err);
}

struct stream_case
{
    const char *in;
    size_t want_len;
    const char *want;
    u32 want_state;
    int want_err;
};

static void test_chunk_decode(void)
{
    static const struct stream_case cases[] = {
        {"0\r\n\r\n", 0, "", TBT_HTTP_CHUNK_DONE, TBT_HTTP_OK},
        {"00\r\n\r\n", 0, "", TBT_HTTP_CHUNK_DONE, TBT_HTTP_OK},
        {"08\r\nabcdefgh\r\n0\r\n\r\n", 8, "abcdefgh", TBT_HTTP_CHUNK_DONE, TBT_HTTP_OK},
        {"8;foo=bar\r\nabcdefgh\r\n0\r\n\r\n", 8, "abcdefgh", TBT_HTTP_CHUNK_DONE, TBT_HTTP_OK},
        {"0;x=y\r\n\r\n", 0, "", TBT_HTTP_CHUNK_DONE, TBT_HTTP_OK},
        {"1\r\na\r\n0\r\nX-T: v\r\nAnother: 2\r\n\r\n", 1, "a", TBT_HTTP_CHUNK_DONE, TBT_HTTP_OK},
        {"A\r\n0123456789\r\n0\r\n\r\n", 10, "0123456789", TBT_HTTP_CHUNK_DONE, TBT_HTTP_OK},
        {"1\r\na\r\n0\r\n\r\n", 1, "a", TBT_HTTP_CHUNK_DONE, TBT_HTTP_OK},
        {"0", 0, "", TBT_HTTP_CHUNK_SIZE, TBT_HTTP_NEED_MORE},
        {"8", 0, "", TBT_HTTP_CHUNK_SIZE, TBT_HTTP_NEED_MORE},
        {"8\r", 0, "", TBT_HTTP_CHUNK_SIZE_END, TBT_HTTP_NEED_MORE},
        {"8\r\n", 0, "", TBT_HTTP_CHUNK_DATA, TBT_HTTP_NEED_MORE},
        {"8\r\nab", 2, "ab", TBT_HTTP_CHUNK_DATA, TBT_HTTP_NEED_MORE},
        {"2024\r\n", 0, "", TBT_HTTP_CHUNK_DATA, TBT_HTTP_NEED_MORE},
        {"0\r\n", 0, "", TBT_HTTP_CHUNK_TRAILER, TBT_HTTP_NEED_MORE},
        {"0\r\n\r", 0, "", TBT_HTTP_CHUNK_TRAILER_BLANK_LF, TBT_HTTP_NEED_MORE},
        {"", 0, "", TBT_HTTP_CHUNK_SIZE, TBT_HTTP_NEED_MORE},
        {"ffffffffffffffff\r\n", 0, "", TBT_HTTP_CHUNK_BAD, TBT_HTTP_PROTO},
        {"100000\r\n", 0, "", TBT_HTTP_CHUNK_BAD, TBT_HTTP_PROTO},
        {"2031\r\n", 0, "", TBT_HTTP_CHUNK_BAD, TBT_HTTP_PROTO},
        {"8g\r\n", 0, "", TBT_HTTP_CHUNK_BAD, TBT_HTTP_PROTO},
        {" 8\r\n", 0, "", TBT_HTTP_CHUNK_BAD, TBT_HTTP_PROTO},
        {"8 \r\n", 0, "", TBT_HTTP_CHUNK_BAD, TBT_HTTP_PROTO},
        {"\r\n", 0, "", TBT_HTTP_CHUNK_BAD, TBT_HTTP_PROTO},
        {"-1\r\n", 0, "", TBT_HTTP_CHUNK_BAD, TBT_HTTP_PROTO},
        {"0x10\r\n", 0, "", TBT_HTTP_CHUNK_BAD, TBT_HTTP_PROTO},
        {"8\n", 0, "", TBT_HTTP_CHUNK_BAD, TBT_HTTP_PROTO},
        {"8\rX", 0, "", TBT_HTTP_CHUNK_BAD, TBT_HTTP_PROTO},
        {"8;foo\nbar\r\n", 0, "", TBT_HTTP_CHUNK_BAD, TBT_HTTP_PROTO},
    };
    char out[256];
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        size_t got_len = 0;
        u32 state = 0;
        int err = 0;
        char what[96];

        run_stream(cases[i].in, strlen(cases[i].in), out, sizeof(out), &got_len, &state, &err);
        snprintf(what, sizeof(what), "chunk stream case %zu", i);
        ok(got_len == cases[i].want_len && !memcmp(out, cases[i].want, got_len), what,
           "decoded body differs");
        ok(state == cases[i].want_state && err == cases[i].want_err, what,
           "parser state or error differs");
    }

    /* The terminator ends one message; reset starts the next on the same
     * carrier, and a feed while DONE consumes nothing. */
    {
        struct tbt_http_chunk_parser p = {};
        char a[8], b[8];
        size_t alen, blen;
        u32 st;
        int e;

        run_stream_p(&p, "1\r\na\r\n0\r\n\r\n", 11, a, sizeof(a), &alen, &st, &e);
        ok(st == TBT_HTTP_CHUNK_DONE && e == TBT_HTTP_OK && alen == 1 && a[0] == 'a',
           "first keep-alive message decodes", NULL);
        {
            const u8 *bb = NULL;
            size_t bbl = 0;
            size_t n = tbt_http_chunk_feed(&p, (const u8 *)"x", 1, &bb, &bbl, &e);

            ok(n == 0 && e == TBT_HTTP_OK && p.state == TBT_HTTP_CHUNK_DONE,
               "feed at DONE consumes nothing", NULL);
        }
        tbt_http_chunk_reset(&p);
        run_stream_p(&p, "1\r\nb\r\n0\r\n\r\n", 11, b, sizeof(b), &blen, &st, &e);
        ok(st == TBT_HTTP_CHUNK_DONE && e == TBT_HTTP_OK && blen == 1 && b[0] == 'b',
           "reset parses the next keep-alive message", NULL);
    }

    /* A size line longer than the decoder's bound is refused even if every
     * byte is a legal hex digit. */
    {
        char *longline = malloc(1200);

        if (longline)
        {
            size_t got_len;
            u32 state;
            int err;

            memset(longline, '0', 1100);
            memcpy(longline + 1100, "\r\n\r\n", 4);
            run_stream(longline, 1104, out, sizeof(out), &got_len, &state, &err);
            ok(state == TBT_HTTP_CHUNK_BAD && err == TBT_HTTP_PROTO,
               "over-long size/extension line is rejected", NULL);
            free(longline);
        }
    }
}

/* Decode s[0..len) split at the given cut points. Each window is consumed to
 * its end before the next window starts; within a window the parser may yield
 * a body run and leave the rest for the next feed. */
static int decode_cuts(const char *s, size_t len, const size_t *cuts, size_t ncuts, char *out,
                       size_t cap, size_t *out_len)
{
    struct tbt_http_chunk_parser p = {};
    size_t pos = 0, o = 0, w = 0;
    size_t window_end = ncuts ? cuts[0] : len;

    while (pos < len)
    {
        const u8 *b = NULL;
        size_t bl = 0, n;
        int err;

        if (window_end > len)
            window_end = len;
        if (pos >= window_end)
        {
            window_end = ++w < ncuts ? cuts[w] : len;
            continue;
        }
        n = tbt_http_chunk_feed(&p, (const u8 *)s + pos, window_end - pos, &b, &bl, &err);
        if (err == TBT_HTTP_PROTO)
            return -1;
        if (bl)
        {
            if (o + bl > cap)
                return -1;
            memcpy(out + o, b, bl);
            o += bl;
        }
        pos += n;
        if (p.state == TBT_HTTP_CHUNK_DONE)
        {
            *out_len = o;
            return pos == len ? 0 : -1;
        }
        if (err == TBT_HTTP_NEED_MORE)
        {
            if (pos != window_end)
                return -1;
            window_end = ++w < ncuts ? cuts[w] : len;
        }
        if (!n)
            return -1; /* a parser that does not advance is a bug */
    }
    *out_len = o;
    return p.state == TBT_HTTP_CHUNK_DONE ? 0 : -1;
}

static size_t encode_chunks(const char *data, size_t len, size_t chunk, char *out, size_t cap)
{
    size_t pos = 0, o = 0;

    while (pos < len)
    {
        size_t n = len - pos < chunk ? len - pos : chunk;
        size_t h = tbt_http_chunk_hdr(out + o, cap - o, n);

        if (!h || o + h + n + 2 > cap)
            return 0;
        memcpy(out + o + h, data + pos, n);
        o += h + n;
        if (!tbt_http_chunk_tail(out + o, cap - o))
            return 0;
        o += 2;
        pos += n;
    }
    {
        size_t h = tbt_http_chunk_hdr(out + o, cap - o, 0);

        if (!h || o + h + 2 > cap)
            return 0;
        o += h;
        if (!tbt_http_chunk_tail(out + o, cap - o))
            return 0;
        o += 2;
    }
    return o;
}

static void test_chunk_split(void)
{
    static const char stream[] = "a\r\n0123456789\r\n5\r\nabcde\r\n0\r\n\r\n";
    static const char want[] = "0123456789abcde";
    size_t len = sizeof(stream) - 1, i;
    char out[64];
    size_t got;
    size_t cuts[256];
    int good = 1;

    for (i = 0; i <= len; i++)
    {
        cuts[0] = i;
        if (decode_cuts(stream, len, cuts, 1, out, sizeof(out), &got) != 0 ||
            got != sizeof(want) - 1 || memcmp(out, want, got))
            good = 0;
    }
    ok(good, "chunk parser split at every byte boundary", NULL);

    for (i = 0; i < len; i++)
        cuts[i] = i;
    good = decode_cuts(stream, len, cuts, len, out, sizeof(out), &got) == 0 &&
           got == sizeof(want) - 1 && !memcmp(out, want, got);
    ok(good, "chunk parser fed one byte at a time", NULL);

    /* The strict independent decoder must agree with the container decoder. */
    {
        char strict_out[64];
        size_t strict_len = 0;
        long used = strict_chunks(stream, len, strict_out, sizeof(strict_out), &strict_len);

        good = used == (long)len && strict_len == got && !memcmp(strict_out, out, got);
        ok(good, "independent strict chunk decoder agrees", NULL);
    }
}

static void test_chunk_big(void)
{
    const size_t total = 1024 * 1024;
    const size_t chunk = 8192;
    char *data = malloc(total);
    char *enc = malloc(total + (total / chunk + 4) * 8 + 16);
    char *dec = malloc(total);
    size_t *cuts = NULL;
    size_t enc_len, dec_len = 0, i, ncuts = 0;
    int good = 0;

    if (!data || !enc || !dec)
        goto done;
    for (i = 0; i < total; i++)
        data[i] = (char)(i * 31 + 7);
    enc_len = encode_chunks(data, total, chunk, enc, total + (total / chunk + 4) * 8 + 16);
    if (!enc_len)
        goto done;

    cuts = malloc((enc_len / 7 + 2) * sizeof(*cuts));
    if (!cuts)
        goto done;
    for (i = 7; i < enc_len; i += 7)
        cuts[ncuts++] = i;
    good = decode_cuts(enc, enc_len, cuts, ncuts, dec, total, &dec_len) == 0 && dec_len == total &&
           !memcmp(dec, data, total);
done:
    ok(good, "1 MiB body round-trips through 7-byte feeds", NULL);
    free(data);
    free(enc);
    free(dec);
    free(cuts);
}

static void test_pad_drip(void)
{
    char pad[TBT_HTTP_PAD_CHUNK_LEN];
    char drip[TBT_HTTP_DRIP_CHUNK_LEN];
    char stream[TBT_HTTP_PAD_CHUNK_LEN + TBT_HTTP_DRIP_CHUNK_LEN + 16];
    char out[128];
    size_t n, got;
    u32 state;
    int err;

    n = tbt_http_pad_chunk(pad, sizeof(pad));
    ok(n == TBT_HTTP_PAD_CHUNK_LEN, "PAD chunk is 38 bytes", NULL);
    ok(!memcmp(pad, "20\r\n", 4), "PAD chunk size line is 0x20 (32)", NULL);
    ok(pad[4] == TBT_F_PAD, "PAD chunk data starts with the frame header", NULL);
    ok(pad[4 + TBT_H_BODYLEN] == 0 && pad[4 + TBT_H_BODYLEN + 1] == 0,
       "PAD body_len is zero", NULL);
    ok(pad[4 + TBT_H_CONV] == (TBT_CONV & 0xff) && pad[4 + TBT_H_CONV + 1] == (TBT_CONV >> 8),
       "PAD frame carries TBT_CONV", NULL);
    ok(pad[36] == '\r' && pad[37] == '\n', "PAD chunk ends with CRLF", NULL);
    ok(tbt_http_pad_chunk(pad, TBT_HTTP_PAD_CHUNK_LEN - 1) == 0,
       "short cap refuses the PAD chunk", NULL);

    n = tbt_http_drip_chunk(drip, sizeof(drip));
    ok(n == TBT_HTTP_DRIP_CHUNK_LEN && !memcmp(drip, "1\r\n#\r\n", 6),
       "drip chunk is a 1-byte chunk carrying TBT_F_PAD", NULL);
    ok(tbt_http_drip_chunk(drip, TBT_HTTP_DRIP_CHUNK_LEN - 1) == 0,
       "short cap refuses the drip chunk", NULL);

    /* A 1-byte body run is not enough to call a chunk a drip: the announced
     * size decides, because a PAD chunk can be split across reads. */
    {
        struct tbt_http_chunk_parser p = {};
        const u8 *b = NULL;
        size_t bl = 0, used;
        int e;

        used = tbt_http_chunk_feed(&p, (const u8 *)"20\r\n", 4, &b, &bl, &e);
        ok(used == 4 && e == TBT_HTTP_NEED_MORE && p.chunk_size == 32 && p.remaining == 32,
           "PAD announces 32 before its body", NULL);
        used = tbt_http_chunk_feed(&p, (const u8 *)"\x20", 1, &b, &bl, &e);
        ok(used == 1 && e == TBT_HTTP_OK && bl == 1 && p.chunk_size == 32 && p.remaining == 31,
           "a 1-byte run of a PAD chunk is not a drip", NULL);
    }
    {
        struct tbt_http_chunk_parser p = {};
        const u8 *b = NULL;
        size_t bl = 0, used;
        int e;

        used = tbt_http_chunk_feed(&p, (const u8 *)"1\r\n", 3, &b, &bl, &e);
        ok(used == 3 && e == TBT_HTTP_NEED_MORE && p.chunk_size == 1 && p.remaining == 1,
           "drip announces size 1", NULL);
        used = tbt_http_chunk_feed(&p, (const u8 *)"#", 1, &b, &bl, &e);
        ok(used == 1 && e == TBT_HTTP_OK && bl == 1 && b[0] == '#' && p.chunk_size == 1 &&
               p.remaining == 0,
           "drip body run completes a size-1 chunk", NULL);
    }

    /* Decode PAD + drip + terminator as one message. */
    memcpy(stream, pad, TBT_HTTP_PAD_CHUNK_LEN);
    memcpy(stream + TBT_HTTP_PAD_CHUNK_LEN, drip, TBT_HTTP_DRIP_CHUNK_LEN);
    memcpy(stream + TBT_HTTP_PAD_CHUNK_LEN + TBT_HTTP_DRIP_CHUNK_LEN, "0\r\n\r\n", 5);
    run_stream(stream, TBT_HTTP_PAD_CHUNK_LEN + TBT_HTTP_DRIP_CHUNK_LEN + 5, out, sizeof(out), &got,
               &state, &err);
    ok(state == TBT_HTTP_CHUNK_DONE && err == TBT_HTTP_OK, "PAD + drip message terminates", NULL);
    ok(got == TBT_HTTP_PAD_DATA_LEN + 1, "PAD + drip yields 32 + 1 body bytes", NULL);
    ok(got >= 5 && out[0] == TBT_F_PAD && out[32] == '#',
       "decoded PAD frame and drip filler", NULL);
}

/* ------------------------------------------------------------------------- */
/* Cadence.                                                                   */

static u8 open_body[TBT_OPEN_BODY_LEN];

static int make_valid_hdr(u8 type, u16 len, struct tbt_hdr *h)
{
    memset(h, 0, sizeof(*h));
    h->type = type;
    h->conv = TBT_CONV;
    h->body_len = len;
    h->seq = 1;
    h->uack = 2;
    h->sid_lo = 3;

    switch (type)
    {
    case TBT_F_DATA:
        break;
    case TBT_F_OPEN:
        if (len != TBT_OPEN_BODY_LEN)
            return 0;
        break;
    case TBT_F_FIN:
        if (len != TBT_FIN_BODY_LEN)
            return 0;
        break;
    case TBT_F_RST:
        if (len != 0)
            return 0;
        break;
    case TBT_F_FBK:
        if (len % TBT_FBK_ENTRY_LEN)
            return 0;
        h->path_ack_count = (u8)(len / TBT_FBK_ENTRY_LEN);
        if (h->path_ack_count > TBT_LEGACY_MAX_CARRIERS)
            return 0;
        break;
    case TBT_F_WINDOW:
        if (len != TBT_WINDOW_BODY_LEN)
            return 0;
        break;
    case TBT_F_HELLO:
        if (len != TBT_HELLO_BODY_LEN)
            return 0;
        break;
    case TBT_F_PAD:
        if (len != 0)
            return 0;
        h->seq = 0;
        h->uack = 0;
        h->sid_lo = 0;
        break;
    default:
        return 0;
    }
    return tbt_hdr_check(h) == 0;
}

static void test_frame_codec(void)
{
    static const u8 types[] = {
        TBT_F_DATA, TBT_F_OPEN, TBT_F_FIN,   TBT_F_RST,    TBT_F_FBK,
        TBT_F_WINDOW, TBT_F_HELLO, TBT_F_PAD,
    };
    struct tbt_http_out out;
    struct tbt_http_rx rx;
    struct tbt_rec rec;
    struct tbt_hdr h;
    u8 body[TBT_PAYLOAD_MAX];
    char enc[TBT_HTTP_FRAME_MAX];
    char tail[8];
    size_t ti, len, i, enc_len, used, used2, tail_len, n;
    char why[96] = "";
    int err, good;

    for (i = 0; i < sizeof(body); i++)
        body[i] = (u8)(i * 7 + 3);

    good = 1;
    for (ti = 0; ti < sizeof(types) / sizeof(types[0]) && good; ti++)
    {
        for (len = 0; len <= TBT_PAYLOAD_MAX; len++)
        {
            if (!make_valid_hdr(types[ti], (u16)len, &h))
                continue;
            out.hdr = &h;
            out.body = len ? body : NULL;
            enc_len = tbt_http_frame_encode(enc, sizeof(enc), &out);
            if (!enc_len)
            {
                good = 0;
                break;
            }
            if (h.type == TBT_F_PAD &&
                (enc_len != TBT_HTTP_PAD_CHUNK_LEN || memcmp(enc, "20\r\n", 4) != 0))
            {
                good = 0;
                break;
            }

            tbt_http_rx_new_message(&rx);
            memset(&rec, 0, sizeof(rec));
            used = tbt_http_frame_decode(&rx, (const u8 *)enc, enc_len, &rec, &err);
            if (err != TBT_RX_OK || used + 2 != enc_len)
            {
                good = 0;
                break;
            }
            if (memcmp(&rec.hdr, &h, sizeof(h)) != 0 || rec.body_len != len ||
                (len && memcmp(rec.body, body, len) != 0))
            {
                good = 0;
                break;
            }

            /* The chunk's trailing CRLF, then the terminator. */
            tail_len = 0;
            n = tbt_http_chunk_hdr(tail, sizeof(tail), 0);
            tail_len = n;
            n = tbt_http_chunk_tail(tail + tail_len, sizeof(tail) - tail_len);
            tail_len += n;
            used2 = tbt_http_frame_decode(&rx, (const u8 *)enc + used, enc_len - used, &rec, &err);
            if (err != TBT_RX_NEED_MORE || used2 != 2)
            {
                good = 0;
                break;
            }
            used2 = tbt_http_frame_decode(&rx, (const u8 *)tail, tail_len, &rec, &err);
            if (err != TBT_RX_NEED_MORE || used2 != tail_len || !tbt_http_rx_done(&rx))
            {
                good = 0;
                break;
            }
        }
    }
    if (!good)
        snprintf(why, sizeof(why), "type=0x%02x len=%zu", types[ti], len);
    ok(good, "encode/decode identity for every frame type and legal body length", why);

    /* Two messages on one carrier: terminator, next request header, reset. */
    {
        struct tbt_http_cfg cfg;
        char stream[1024];
        char req_hdr[TBT_HTTP_REQ_MAX];
        size_t o = 0, hlen, pos = 0;

        make_valid_hdr(TBT_F_DATA, 4, &h);
        out.hdr = &h;
        out.body = body;
        n = tbt_http_frame_encode(stream + o, sizeof(stream) - o, &out);
        o += n;
        n = tbt_http_chunk_hdr(stream + o, sizeof(stream) - o, 0);
        o += n;
        n = tbt_http_chunk_tail(stream + o, sizeof(stream) - o);
        o += n;

        cfg_set(&cfg, "203.0.113.9", "abcdefghijklmnopqrstuvwxyz");
        hlen = tbt_http_req_header(req_hdr, sizeof(req_hdr), &cfg);
        memcpy(stream + o, req_hdr, hlen);
        o += hlen;

        make_valid_hdr(TBT_F_OPEN, TBT_OPEN_BODY_LEN, &h);
        out.hdr = &h;
        out.body = open_body;
        n = tbt_http_frame_encode(stream + o, sizeof(stream) - o, &out);
        o += n;
        n = tbt_http_chunk_hdr(stream + o, sizeof(stream) - o, 0);
        o += n;
        n = tbt_http_chunk_tail(stream + o, sizeof(stream) - o);
        o += n;

        tbt_http_rx_new_message(&rx);
        used = tbt_http_frame_decode(&rx, (const u8 *)stream, o, &rec, &err);
        pos = used;
        if (err != TBT_RX_OK || rec.hdr.type != TBT_F_DATA)
            good = 0;
        while (good && !tbt_http_rx_done(&rx))
        {
            used = tbt_http_frame_decode(&rx, (const u8 *)stream + pos, o - pos, &rec, &err);
            if (err != TBT_RX_NEED_MORE || !used)
            {
                good = 0;
                break;
            }
            pos += used;
        }
        hlen = tbt_http_skip_request_header((const u8 *)stream + pos, o - pos, &err);
        if (err != TBT_HTTP_OK || !hlen)
        {
            good = 0;
        }
        else
        {
            pos += hlen;
            tbt_http_rx_new_message(&rx);
            used = tbt_http_frame_decode(&rx, (const u8 *)stream + pos, o - pos, &rec, &err);
            if (err != TBT_RX_OK || rec.hdr.type != TBT_F_OPEN)
                good = 0;
        }
        ok(good, "record, terminator, next request header, reset, record", NULL);
    }

    /* Every type against every length 0..8192: invalid header combinations
     * must be refused by the encoder, not emitted. */
    good = 1;
    for (ti = 0; ti < sizeof(types) / sizeof(types[0]) && good; ti++)
    {
        for (len = 0; len <= TBT_PAYLOAD_MAX; len++)
        {
            memset(&h, 0, sizeof(h));
            h.type = types[ti];
            h.conv = TBT_CONV;
            h.body_len = (u16)len;
            h.seq = 1;
            if (tbt_hdr_check(&h) == 0)
                continue;
            out.hdr = &h;
            out.body = body;
            if (tbt_http_frame_encode(enc, sizeof(enc), &out) != 0)
            {
                good = 0;
                break;
            }
        }
    }
    ok(good, "invalid frame headers are refused by the encoder", NULL);

    /* Cap and invalid-input checks. */
    make_valid_hdr(TBT_F_DATA, 100, &h);
    out.hdr = &h;
    out.body = body;
    enc_len = tbt_http_frame_encode(enc, sizeof(enc), &out);
    ok(enc_len > 0 && tbt_http_frame_encode(enc, enc_len - 1, &out) == 0,
       "short cap refuses the frame", NULL);
    out.hdr = NULL;
    ok(tbt_http_frame_encode(enc, sizeof(enc), &out) == 0, "NULL frame header is refused", NULL);
    out.hdr = &h;
    out.body = NULL;
    ok(tbt_http_frame_encode(enc, sizeof(enc), &out) == 0, "missing body is refused", NULL);
    h.type = TBT_F_PAD;
    h.body_len = 1;
    out.hdr = &h;
    out.body = body;
    ok(tbt_http_frame_encode(enc, sizeof(enc), &out) == 0, "PAD with a body is refused", NULL);
    make_valid_hdr(TBT_F_DATA, 100, &h);
    h.body_len = TBT_PAYLOAD_MAX + 1;
    out.hdr = &h;
    out.body = body;
    ok(tbt_http_frame_encode(enc, sizeof(enc), &out) == 0, "oversized body is refused", NULL);

    /* Two records in one chunk. */
    {
        struct tbt_hdr h1, h2;
        char r1[TBT_HDR_LEN], r2[TBT_HDR_LEN + TBT_OPEN_BODY_LEN];
        char two[160];
        size_t o = 0;

        make_valid_hdr(TBT_F_DATA, 0, &h1);
        make_valid_hdr(TBT_F_OPEN, TBT_OPEN_BODY_LEN, &h2);
        tbt_hdr_encode((u8 *)r1, &h1);
        tbt_hdr_encode((u8 *)r2, &h2);
        memset(r2 + TBT_HDR_LEN, 0, TBT_OPEN_BODY_LEN);
        n = tbt_http_chunk_hdr(two, sizeof(two),
                               2 * TBT_HDR_LEN + TBT_OPEN_BODY_LEN);
        o = n;
        memcpy(two + o, r1, TBT_HDR_LEN);
        o += TBT_HDR_LEN;
        memcpy(two + o, r2, TBT_HDR_LEN + TBT_OPEN_BODY_LEN);
        o += TBT_HDR_LEN + TBT_OPEN_BODY_LEN;
        n = tbt_http_chunk_tail(two + o, sizeof(two) - o);
        o += n;
        tbt_http_rx_new_message(&rx);
        used = tbt_http_frame_decode(&rx, (const u8 *)two, o, &rec, &err);
        ok(used == 0 && err == TBT_RX_PROTO, "two records in one chunk are rejected", NULL);
    }

    /* A record split across two chunks, both after and before the header. */
    {
        char recbuf[132];
        char split[256];
        size_t o = 0;

        make_valid_hdr(TBT_F_DATA, 100, &h);
        tbt_hdr_encode((u8 *)recbuf, &h);
        memcpy(recbuf + TBT_HDR_LEN, body, 100);

        n = tbt_http_chunk_hdr(split + o, sizeof(split) - o, 82);
        o += n;
        memcpy(split + o, recbuf, 82);
        o += 82;
        n = tbt_http_chunk_tail(split + o, sizeof(split) - o);
        o += n;
        n = tbt_http_chunk_hdr(split + o, sizeof(split) - o, 50);
        o += n;
        memcpy(split + o, recbuf + 82, 50);
        o += 50;
        n = tbt_http_chunk_tail(split + o, sizeof(split) - o);
        o += n;
        tbt_http_rx_new_message(&rx);
        used = tbt_http_frame_decode(&rx, (const u8 *)split, o, &rec, &err);
        ok(used == 0 && err == TBT_RX_PROTO, "record split after the header is rejected", NULL);

        o = 0;
        n = tbt_http_chunk_hdr(split + o, sizeof(split) - o, 16);
        o += n;
        memcpy(split + o, recbuf, 16);
        o += 16;
        n = tbt_http_chunk_tail(split + o, sizeof(split) - o);
        o += n;
        n = tbt_http_chunk_hdr(split + o, sizeof(split) - o, 116);
        o += n;
        memcpy(split + o, recbuf + 16, 116);
        o += 116;
        n = tbt_http_chunk_tail(split + o, sizeof(split) - o);
        o += n;
        tbt_http_rx_new_message(&rx);
        used = tbt_http_frame_decode(&rx, (const u8 *)split, o, &rec, &err);
        ok(used == 0 && err == TBT_RX_PROTO, "record split before the header is rejected", NULL);
    }

    /* Drip and PAD interleaved with records. */
    {
        char stream[512];
        struct tbt_rec got[4];
        u8 data_body[16];
        size_t o = 0, pos = 0, data_len = 0;
        int ngot = 0;

        n = tbt_http_drip_chunk(stream + o, sizeof(stream) - o);
        o += n;
        n = tbt_http_pad_chunk(stream + o, sizeof(stream) - o);
        o += n;
        make_valid_hdr(TBT_F_DATA, 10, &h);
        out.hdr = &h;
        out.body = body;
        n = tbt_http_frame_encode(stream + o, sizeof(stream) - o, &out);
        o += n;
        n = tbt_http_drip_chunk(stream + o, sizeof(stream) - o);
        o += n;
        make_valid_hdr(TBT_F_OPEN, TBT_OPEN_BODY_LEN, &h);
        out.hdr = &h;
        out.body = open_body;
        n = tbt_http_frame_encode(stream + o, sizeof(stream) - o, &out);
        o += n;
        n = tbt_http_chunk_hdr(stream + o, sizeof(stream) - o, 0);
        o += n;
        n = tbt_http_chunk_tail(stream + o, sizeof(stream) - o);
        o += n;

        tbt_http_rx_new_message(&rx);
        good = 1;
        while (pos < o)
        {
            used = tbt_http_frame_decode(&rx, (const u8 *)stream + pos, o - pos, &rec, &err);
            pos += used;
            if (err == TBT_RX_OK)
            {
                if (ngot < 4)
                {
                    got[ngot].hdr = rec.hdr;
                    got[ngot].body_len = rec.body_len;
                    ngot++;
                }
                if (rec.hdr.type == TBT_F_DATA)
                {
                    data_len = rec.body_len;
                    memcpy(data_body, rec.body, data_len);
                }
            }
            else if (err == TBT_RX_NEED_MORE)
            {
                if (tbt_http_rx_done(&rx))
                    break;
                if (!used)
                {
                    good = 0;
                    break;
                }
            }
            else
            {
                good = 0;
                break;
            }
        }
        ok(good && tbt_http_rx_done(&rx) && ngot == 3 && got[0].hdr.type == TBT_F_PAD &&
               got[1].hdr.type == TBT_F_DATA && got[1].body_len == 10 && data_len == 10 &&
               !memcmp(data_body, body, 10) && got[2].hdr.type == TBT_F_OPEN,
           "drip and PAD interleave with records", NULL);
    }
}

static int frame_stream_decode(const char *s, size_t total, size_t piece, u64 *rng, size_t nrec)
{
    struct tbt_http_rx rx = {};
    struct tbt_rec rec;
    size_t pos = 0, got = 0, i, wpos;
    int err;

    while (pos < total)
    {
        size_t take = piece;

        if (!take)
            take = 1 + (size_t)((*rng) % 4096);
        if (take > total - pos)
            take = total - pos;
        {
            wpos = 0;
            while (wpos < take)
            {
                size_t used =
                    tbt_http_frame_decode(&rx, (const u8 *)s + pos + wpos, take - wpos, &rec, &err);

                if (used > take - wpos)
                    return -1;
                wpos += used;
                if (err == TBT_RX_OK)
                {
                    if (rec.hdr.type != TBT_F_DATA || rec.body_len != TBT_PAYLOAD_MAX)
                        return -1;
                    for (i = 0; i < rec.body_len; i++)
                    {
                        if (rec.body[i] != (u8)(got + i))
                            return -1;
                    }
                    got++;
                }
                else if (err == TBT_RX_NEED_MORE)
                {
                    if (tbt_http_rx_done(&rx))
                    {
                        pos += wpos;
                        return pos == total && got == nrec ? 0 : -1;
                    }
                    if (!used)
                        return -1;
                }
                else
                {
                    return -1;
                }
            }
        }
        pos += wpos;
    }
    return tbt_http_rx_done(&rx) && got == nrec ? 0 : -1;
}

static void test_frame_stream(void)
{
    const size_t nrec = 128; /* 128 * 8192 = 1 MiB */
    const size_t cap = nrec * TBT_HTTP_FRAME_MAX + 8;
    char *stream = malloc(cap);
    u8 *body = malloc(TBT_PAYLOAD_MAX);
    struct tbt_http_out out;
    struct tbt_hdr h;
    size_t r, i, o = 0, n;
    u64 rng = 0x123456789abcdef0ULL;

    if (!stream || !body)
    {
        ok(0, "frame stream allocations", "out of memory");
        free(stream);
        free(body);
        return;
    }

    for (r = 0; r < nrec; r++)
    {
        for (i = 0; i < TBT_PAYLOAD_MAX; i++)
            body[i] = (u8)(r + i);
        make_valid_hdr(TBT_F_DATA, TBT_PAYLOAD_MAX, &h);
        h.seq = r;
        out.hdr = &h;
        out.body = body;
        n = tbt_http_frame_encode(stream + o, cap - o, &out);
        if (!n)
        {
            ok(0, "frame stream encode", "encode failed");
            free(stream);
            free(body);
            return;
        }
        o += n;
    }
    n = tbt_http_chunk_hdr(stream + o, cap - o, 0);
    o += n;
    n = tbt_http_chunk_tail(stream + o, cap - o);
    o += n;

    ok(frame_stream_decode(stream, o, 7, NULL, nrec) == 0, "1 MiB of records in 7-byte feeds",
       NULL);
    ok(frame_stream_decode(stream, o, 1, NULL, nrec) == 0, "1 MiB of records in 1-byte feeds",
       NULL);
    ok(frame_stream_decode(stream, o, 0, &rng, nrec) == 0,
       "1 MiB of records in random 1..4096 splits", NULL);

    free(stream);
    free(body);
}

static void test_cadence(void)
{
    const u64 ms = 1000000ULL;
    u64 last;

    ok(!tbt_http_need_pad(1000, 1000 + 5000 * ms - 1, 5000), "PAD not due before keepalive_ms",
       NULL);
    ok(tbt_http_need_pad(1000, 1000 + 5000 * ms, 5000), "PAD due at keepalive_ms", NULL);
    ok(tbt_http_need_pad(1000, 1000 + 5001 * ms, 5000), "PAD due after keepalive_ms", NULL);
    ok(!tbt_http_need_pad(1000, 1000, 0), "keepalive_ms 0 disables PAD", NULL);
    ok(!tbt_http_need_pad(1000, 1000 + 999999, 1), "1 ms interval does not fire at 999999 ns", NULL);
    ok(tbt_http_need_pad(1000, 1000 + 1000000, 1), "1 ms interval fires at 1000000 ns", NULL);
    ok(tbt_http_need_pad(1000, 0, 5000), "backwards clock asks for a PAD", NULL);

    last = (u64)-1 - 500 * ms;
    ok(tbt_http_need_pad(last, last + 6000 * ms, 5000), "PAD arithmetic is wrap-safe", NULL);
    ok(!tbt_http_need_pad(last, last + 4999 * ms, 5000), "PAD wrap does not fire early", NULL);

    ok(!tbt_http_need_drip(1000, 1000 + 1000 * ms - 1, 1000), "drip not due before drip_ms", NULL);
    ok(tbt_http_need_drip(1000, 1000 + 1000 * ms, 1000), "drip due at drip_ms", NULL);
    ok(!tbt_http_need_drip(1000, 1000, 0), "drip_ms 0 disables drip", NULL);
    ok(tbt_http_need_drip(2000, 1000, 1000), "drip asks after a backwards clock", NULL);
}

/* ------------------------------------------------------------------------- */
/* Round-trip property: random bodies, random chunk splits, both directions.  */

static u64 prop_seed = 0x9e3779b97f4a7c15ULL;
static u64 prop_rng;

static u64 prop_rnd64(void)
{
    u64 x = prop_rng;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    prop_rng = x;
    return x;
}

static size_t prop_below(size_t n)
{
    return n ? (size_t)(prop_rnd64() % n) : 0;
}

/* Encode `len` body bytes as a random number of chunks, each 1..TBT_CHUNK_MAX,
 * followed by the terminator. The chunk count is between ceil(len/CHUNK_MAX)
 * and min(len, 256), so a 1 MiB body gets 128..256 chunks and a small body can
 * be split as finely as one byte per chunk. */
static size_t encode_random_chunks(const char *body, size_t len, char *out, size_t cap,
                                   size_t *sizes, size_t *nsizes)
{
    size_t pos = 0, o = 0, remaining = len;
    size_t min_chunks, max_chunks, nchunks, k, h;

    *nsizes = 0;
    if (len == 0)
    {
        h = tbt_http_chunk_hdr(out, cap, 0);
        if (!h || h + 2 > cap)
            return 0;
        o = h;
        if (!tbt_http_chunk_tail(out + o, cap - o))
            return 0;
        return o + 2;
    }

    min_chunks = (len + TBT_CHUNK_MAX - 1) / TBT_CHUNK_MAX;
    max_chunks = len < 256 ? len : 256;
    if (max_chunks < min_chunks)
        max_chunks = min_chunks;
    nchunks = min_chunks + prop_below(max_chunks - min_chunks + 1);

    for (k = nchunks; k > 0; k--)
    {
        size_t max_n = remaining - (k - 1);
        size_t min_n = (remaining + k - 1) / k;
        size_t min_needed =
            remaining > (k - 1) * TBT_CHUNK_MAX ? remaining - (k - 1) * TBT_CHUNK_MAX : 1;
        size_t n;

        if (max_n > TBT_CHUNK_MAX)
            max_n = TBT_CHUNK_MAX;
        if (min_n < min_needed)
            min_n = min_needed;
        if (min_n > max_n)
            return 0;
        n = min_n + prop_below(max_n - min_n + 1);
        sizes[nchunks - k] = n;

        h = tbt_http_chunk_hdr(out + o, cap - o, n);
        if (!h || h + n + 2 > cap - o)
            return 0;
        memcpy(out + o + h, body + pos, n);
        o += h + n;
        if (!tbt_http_chunk_tail(out + o, cap - o))
            return 0;
        o += 2;
        pos += n;
        remaining -= n;
    }

    if (remaining)
        return 0;
    *nsizes = nchunks;
    h = tbt_http_chunk_hdr(out + o, cap - o, 0);
    if (!h || h + 2 > cap - o)
        return 0;
    o += h;
    if (!tbt_http_chunk_tail(out + o, cap - o))
        return 0;
    return o + 2;
}

static size_t first_diff(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        if (a[i] != b[i])
            return i;
    }
    return n;
}

/* Decode one chunk stream, either in one-shot feeds or one input byte per
 * feed. Returns 0 only when the terminator is reached exactly at len. */
static int decode_all(const char *s, size_t len, char *out, size_t cap, size_t *out_len,
                      int bytewise)
{
    struct tbt_http_chunk_parser p = {};
    size_t pos = 0, o = 0;

    *out_len = 0;
    for (;;)
    {
        size_t take = bytewise ? (pos < len ? 1 : 0) : len - pos;
        const u8 *body = NULL;
        size_t blen = 0, n;
        int err;

        n = tbt_http_chunk_feed(&p, (const u8 *)s + pos, take, &body, &blen, &err);
        if (n > take)
            return -1;
        if (body && (body < (const u8 *)s + pos || body + blen > (const u8 *)s + len))
            return -1;
        if (err == TBT_HTTP_PROTO)
            return -1;
        if (blen)
        {
            if (o + blen > cap)
                return -1;
            memcpy(out + o, body, blen);
            o += blen;
        }
        pos += n;
        *out_len = o;
        if (p.state == TBT_HTTP_CHUNK_DONE)
            return pos == len ? 0 : -1;
        if (err == TBT_HTTP_NEED_MORE)
        {
            if (!bytewise || pos == len)
                return -1; /* incomplete at the end of the stream */
            continue;
        }
        if (!n)
            return -1;
    }
}

static void roundtrip_fail(const char *stage, int iter, size_t len, const size_t *split,
                           size_t nsplit, size_t diff)
{
    size_t j;

    printf("FAIL roundtrip seed=0x%016llx iter=%d len=%zu chunks=%zu first_diff=%zu stage=%s\n",
           (unsigned long long)prop_seed, iter, len, nsplit, diff, stage);
    printf("split:");
    for (j = 0; j < nsplit; j++)
        printf(" %zu", split[j]);
    printf("\n");
}

static void test_roundtrip(void)
{
    const size_t max_body = 1024 * 1024;
    const size_t max_chunks = 256;
    char *body = malloc(max_body);
    char *chunks = malloc(max_body + max_chunks * 8 + 64);
    char *full = malloc(max_body + max_chunks * 8 + TBT_HTTP_REQ_MAX + 64);
    char *dec = malloc(max_body);
    char req_hdr[TBT_HTTP_REQ_MAX];
    char resp_hdr[TBT_HTTP_RESP_MAX];
    size_t split[256];
    size_t nsplit = 0;
    static const char b32[] = "abcdefghijklmnopqrstuvwxyz234567";
    struct tbt_http_cfg cfg;
    struct tbt_http_resp_cfg rcfg;
    char why[128] = "";
    int good = 1;
    int iter;

    prop_rng = prop_seed;
    if (!body || !chunks || !full || !dec)
    {
        ok(0, "round-trip allocations", "out of memory");
        goto out;
    }

    for (iter = 0; iter < 10000 && good; iter++)
    {
        size_t len, j, hdr_len, ch_len, full_len, dec_len, used, diff;
        struct tbt_http_req rq;
        struct tbt_http_resp rp;
        const char *stage = "encode";
        int err;

        /* 1% of bodies cover the full 0..1 MiB range; the rest stay small so
         * 10k iterations and their byte-at-a-time feeds fit in the 5 s budget. */
        len = (iter % 1000) == 0 ? prop_below(max_body + 1) : prop_below(2049);
        for (j = 0; j < len; j++)
            body[j] = (char)prop_rnd64();

        memset(&cfg, 0, sizeof(cfg));
        if (prop_rnd64() & 1)
            memcpy(cfg.host, "203.0.113.9", sizeof("203.0.113.9"));
        else
            memcpy(cfg.host, "203.0.113.9:8443", sizeof("203.0.113.9:8443"));
        for (j = 0; j < TBT_HTTP_REQID_LEN; j++)
            cfg.request_id[j] = b32[prop_below(sizeof(b32) - 1)];
        cfg.request_id[TBT_HTTP_REQID_LEN] = '\0';

        hdr_len = tbt_http_req_header(req_hdr, sizeof(req_hdr), &cfg);
        ch_len = encode_random_chunks(body, len, chunks, max_body + max_chunks * 8 + 64, split,
                                      &nsplit);
        if (!hdr_len || !ch_len)
        {
            snprintf(why, sizeof(why), "encode");
            roundtrip_fail(stage, iter, len, split, nsplit, 0);
            good = 0;
            break;
        }
        full_len = hdr_len + ch_len;
        memcpy(full, req_hdr, hdr_len);
        memcpy(full + hdr_len, chunks, ch_len);

        stage = "request-parse";
        memset(&rq, 0, sizeof(rq));
        used = tbt_http_req_parse(full, full_len, &rq, &err);
        if (used != hdr_len || err != TBT_HTTP_OK || !rq.chunked || !rq.keep_alive ||
            rq.host_len != strlen(cfg.host) || memcmp(rq.host, cfg.host, rq.host_len) != 0)
        {
            snprintf(why, sizeof(why), "request-parse");
            roundtrip_fail(stage, iter, len, split, nsplit, 0);
            good = 0;
            break;
        }

        stage = "request-one-shot";
        if (decode_all(full + used, full_len - used, dec, max_body, &dec_len, 0) != 0 ||
            dec_len != len || (len && memcmp(dec, body, len) != 0))
        {
            diff = first_diff(dec, body, dec_len < len ? dec_len : len);
            snprintf(why, sizeof(why), "request-one-shot");
            roundtrip_fail(stage, iter, len, split, nsplit, diff);
            good = 0;
            break;
        }

        stage = "request-bytewise";
        if (decode_all(full + used, full_len - used, dec, max_body, &dec_len, 1) != 0 ||
            dec_len != len || (len && memcmp(dec, body, len) != 0))
        {
            diff = first_diff(dec, body, dec_len < len ? dec_len : len);
            snprintf(why, sizeof(why), "request-bytewise");
            roundtrip_fail(stage, iter, len, split, nsplit, diff);
            good = 0;
            break;
        }

        stage = "response-header";
        rcfg.unix_secs = 1700000000ULL + prop_below(1000000000);
        hdr_len = tbt_http_resp_header(resp_hdr, sizeof(resp_hdr), &rcfg);
        if (!hdr_len)
        {
            snprintf(why, sizeof(why), "response-header");
            roundtrip_fail(stage, iter, len, split, nsplit, 0);
            good = 0;
            break;
        }
        full_len = hdr_len + ch_len;
        memcpy(full, resp_hdr, hdr_len);
        memcpy(full + hdr_len, chunks, ch_len);

        stage = "response-parse";
        memset(&rp, 0, sizeof(rp));
        used = tbt_http_resp_parse(full, full_len, &rp, &err);
        if (used != hdr_len || err != TBT_HTTP_OK || rp.status != 200 || !rp.chunked ||
            !rp.keep_alive)
        {
            snprintf(why, sizeof(why), "response-parse");
            roundtrip_fail(stage, iter, len, split, nsplit, 0);
            good = 0;
            break;
        }

        stage = "response-one-shot";
        if (decode_all(full + used, full_len - used, dec, max_body, &dec_len, 0) != 0 ||
            dec_len != len || (len && memcmp(dec, body, len) != 0))
        {
            diff = first_diff(dec, body, dec_len < len ? dec_len : len);
            snprintf(why, sizeof(why), "response-one-shot");
            roundtrip_fail(stage, iter, len, split, nsplit, diff);
            good = 0;
            break;
        }

        stage = "response-bytewise";
        if (decode_all(full + used, full_len - used, dec, max_body, &dec_len, 1) != 0 ||
            dec_len != len || (len && memcmp(dec, body, len) != 0))
        {
            diff = first_diff(dec, body, dec_len < len ? dec_len : len);
            snprintf(why, sizeof(why), "response-bytewise");
            roundtrip_fail(stage, iter, len, split, nsplit, diff);
            good = 0;
            break;
        }
    }

    ok(good, "10k random bodies round-trip in both directions", why);
out:
    free(body);
    free(chunks);
    free(full);
    free(dec);
}

/* A deterministic random pass: every parser must stay inside its buffer, and
 * every return value must be one of the three documented statuses. ASan makes
 * an out-of-bounds read fatal, so this doubles as the totality check. */
static void test_totality(void)
{
    u32 rng = 0x9e3779b9u;
    int good = 1, iter;

    for (iter = 0; iter < 5000 && good; iter++)
    {
        size_t len, j, pos = 0;
        u8 *buf;
        char *sbuf;
        struct tbt_http_chunk_parser p = {};
        struct tbt_http_resp r;
        int err;

        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        len = rng % 128;
        buf = malloc(len ? len : 1);
        sbuf = malloc(len ? len : 1);
        if (!buf || !sbuf)
        {
            free(buf);
            free(sbuf);
            good = 0;
            break;
        }
        for (j = 0; j < len; j++)
        {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            buf[j] = (u8)rng;
            sbuf[j] = (char)rng;
        }

        for (;;)
        {
            const u8 *body = NULL;
            size_t blen = 0, n = tbt_http_chunk_feed(&p, buf + pos, len - pos, &body, &blen, &err);

            if (n > len - pos || (err != TBT_HTTP_OK && err != TBT_HTTP_NEED_MORE &&
                                  err != TBT_HTTP_PROTO))
                good = 0;
            if (body && (body < buf + pos || body + blen > buf + len))
                good = 0;
            if (err == TBT_HTTP_PROTO || err == TBT_HTTP_NEED_MORE ||
                p.state == TBT_HTTP_CHUNK_DONE)
                break;
            if (!n)
            {
                good = 0;
                break;
            }
            pos += n;
        }

        memset(&r, 0, sizeof(r));
        {
            size_t n = tbt_http_resp_parse(sbuf, len, &r, &err);

            if (n > len || (err != TBT_HTTP_OK && err != TBT_HTTP_NEED_MORE &&
                            err != TBT_HTTP_PROTO))
                good = 0;
            if (err != TBT_HTTP_OK && n != 0)
                good = 0;
        }
        free(buf);
        free(sbuf);
    }
    ok(good, "parsers are total on deterministic random input", NULL);
}

/* Wire samples for the h11 cross-check and for humans/QEMU captures. Each
 * message is the header block, a PAD chunk, a drip chunk, two DATA records
 * and the terminator; --emit writes the request followed by the response,
 * while --emit-req and --emit-resp write one direction each. Everything is
 * fixed (host, request id, epoch), so the bytes are reproducible. */
static size_t build_capture_body(char *buf, size_t cap);

static int emit_req_wire(void)
{
    struct tbt_http_cfg cfg;
    char buf[TBT_HTTP_REQ_MAX];
    char body[512];
    size_t n, blen;

    cfg_set(&cfg, "203.0.113.9:8443", "abcd234567abcdefghijklmnop");
    n = tbt_http_req_header(buf, sizeof(buf), &cfg);
    if (!n || fwrite(buf, 1, n, stdout) != n)
        return 1;
    blen = build_capture_body(body, sizeof(body));
    if (!blen || fwrite(body, 1, blen, stdout) != blen)
        return 1;
    return fflush(stdout) == 0 ? 0 : 1;
}

static int emit_resp_wire(void)
{
    struct tbt_http_resp_cfg cfg = { .unix_secs = 1700000000 };
    char buf[TBT_HTTP_RESP_MAX];
    char body[512];
    size_t n, blen;

    n = tbt_http_resp_header(buf, sizeof(buf), &cfg);
    if (!n || fwrite(buf, 1, n, stdout) != n)
        return 1;
    blen = build_capture_body(body, sizeof(body));
    if (!blen || fwrite(body, 1, blen, stdout) != blen)
        return 1;
    return fflush(stdout) == 0 ? 0 : 1;
}

/* Deterministic sample with PAD, drip, two DATA records and a terminator.
 * Used by the live capture modes; the first chunk is the PAD chunk. */
static size_t build_capture_body(char *buf, size_t cap)
{
    static const char d1[] = "0123456789";
    static const char d2[] = "abcde";
    struct tbt_http_out out;
    struct tbt_hdr h;
    size_t o = 0, n;

    n = tbt_http_pad_chunk(buf + o, cap - o);
    if (!n)
        return 0;
    o += n;
    n = tbt_http_drip_chunk(buf + o, cap - o);
    if (!n)
        return 0;
    o += n;

    memset(&h, 0, sizeof(h));
    h.type = TBT_F_DATA;
    h.conv = TBT_CONV;
    h.body_len = sizeof(d1) - 1;
    h.seq = 1;
    h.sid_lo = 1;
    out.hdr = &h;
    out.body = (const u8 *)d1;
    n = tbt_http_frame_encode(buf + o, cap - o, &out);
    if (!n)
        return 0;
    o += n;

    memset(&h, 0, sizeof(h));
    h.type = TBT_F_DATA;
    h.conv = TBT_CONV;
    h.body_len = sizeof(d2) - 1;
    h.seq = 2;
    h.sid_lo = 1;
    out.hdr = &h;
    out.body = (const u8 *)d2;
    n = tbt_http_frame_encode(buf + o, cap - o, &out);
    if (!n)
        return 0;
    o += n;

    n = tbt_http_chunk_hdr(buf + o, cap - o, 0);
    if (!n)
        return 0;
    o += n;
    n = tbt_http_chunk_tail(buf + o, cap - o);
    if (!n)
        return 0;
    return o + n;
}

/* Frozen capture escaping: \r, \n, \xNN for non-printable bytes; a literal
 * backslash is written as \x5c so the stream is unambiguous. */
static size_t escape_text(char *out, size_t cap, const u8 *in, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    size_t o = 0, i;

    for (i = 0; i < len; i++)
    {
        unsigned char c = in[i];

        if (c == '\r' || c == '\n' || c == '\\')
        {
            if (o + 4 > cap)
                return 0;
            out[o++] = '\\';
            if (c == '\r')
                out[o++] = 'r';
            else if (c == '\n')
                out[o++] = 'n';
            else
            {
                out[o++] = 'x';
                out[o++] = '5';
                out[o++] = 'c';
            }
        }
        else if (c >= 0x20 && c <= 0x7e)
        {
            if (o + 1 > cap)
                return 0;
            out[o++] = (char)c;
        }
        else
        {
            if (o + 4 > cap)
                return 0;
            out[o++] = '\\';
            out[o++] = 'x';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xf];
        }
    }
    return o;
}

static int emit_capture_binary(void)
{
    char body[512];
    char req[TBT_HTTP_REQ_MAX + 512];
    char resp[TBT_HTTP_RESP_MAX + 512];
    struct tbt_http_cfg cfg;
    struct tbt_http_resp_cfg rcfg = { .unix_secs = 1700000000 };
    size_t blen, req_len, resp_len, hlen;

    blen = build_capture_body(body, sizeof(body));
    if (!blen)
        return 1;
    cfg_set(&cfg, "203.0.113.9:8443", "abcd234567abcdefghijklmnop");
    hlen = tbt_http_req_header(req, sizeof(req), &cfg);
    if (!hlen)
        return 1;
    memcpy(req + hlen, body, blen);
    req_len = hlen + blen;
    hlen = tbt_http_resp_header(resp, sizeof(resp), &rcfg);
    if (!hlen)
        return 1;
    memcpy(resp + hlen, body, blen);
    resp_len = hlen + blen;

    printf("TBT-HTTP-CAPTURE 1\n");
    printf("request %zu\n", req_len);
    if (fwrite(req, 1, req_len, stdout) != req_len)
        return 1;
    printf("response %zu\n", resp_len);
    if (fwrite(resp, 1, resp_len, stdout) != resp_len)
        return 1;
    return fflush(stdout) == 0 ? 0 : 1;
}

static int emit_capture_text(void)
{
    char body[512];
    char req[TBT_HTTP_REQ_MAX];
    char resp[TBT_HTTP_RESP_MAX];
    char esc[4096];
    struct tbt_http_cfg cfg;
    struct tbt_http_resp_cfg rcfg = { .unix_secs = 1700000000 };
    size_t blen, req_len, resp_len, first_len, n;

    cfg_set(&cfg, "203.0.113.9:8443", "abcd234567abcdefghijklmnop");
    req_len = tbt_http_req_header(req, sizeof(req), &cfg);
    resp_len = tbt_http_resp_header(resp, sizeof(resp), &rcfg);
    blen = build_capture_body(body, sizeof(body));
    if (!req_len || !resp_len || !blen)
        return 1;
    first_len = TBT_HTTP_PAD_CHUNK_LEN - 2; /* size line + 32-byte PAD frame */
    if (blen < first_len)
        return 1;

    printf("req=");
    n = escape_text(esc, sizeof(esc), (const u8 *)req, req_len);
    if (!n || fwrite(esc, 1, n, stdout) != n)
        return 1;
    printf("\nresp=");
    n = escape_text(esc, sizeof(esc), (const u8 *)resp, resp_len);
    if (!n || fwrite(esc, 1, n, stdout) != n)
        return 1;
    printf("\nfirst_chunk=");
    n = escape_text(esc, sizeof(esc), (const u8 *)body, first_len);
    if (!n || fwrite(esc, 1, n, stdout) != n)
        return 1;
    printf("\nreq_len=%zu\nresp_len=%zu\n", req_len, resp_len);
    return fflush(stdout) == 0 ? 0 : 1;
}

static int emit_both_wire(void)
{
    if (emit_req_wire())
        return 1;
    if (emit_resp_wire())
        return 1;
    return fflush(stdout) == 0 ? 0 : 1;
}

static void test_fec_http(void)
{
    struct tbt_http_rx rx;
    u8 rec[TBT_FEC_HDR_LEN + 64];
    u8 chunk[TBT_HTTP_FEC_FRAME_MAX];
    u8 two[2 * TBT_HTTP_FEC_FRAME_MAX];
    const u8 *out;
    size_t out_len, n, m;
    int err;
    unsigned i;

    memset(&rx, 0, sizeof(rx));
    tbt_http_chunk_reset(&rx.chunk);
    memset(rec, 0, sizeof(rec));
    rec[0] = TBT_FEC_DATA;
    rec[4] = 4;
    rec[5] = 2;
    rec[26] = 64;
    rec[28] = 0x01;
    rec[29] = 0x10;
    for (i = 0; i < 64; i++)
        rec[TBT_FEC_HDR_LEN + i] = (u8)(i * 7 + 3);

    n = tbt_http_fec_chunk_encode((char *)chunk, sizeof(chunk), rec, sizeof(rec));
    ok(n > 0, "fec chunk encodes", NULL);

    /* Split the chunk: no record until the last body byte arrives. */
    m = tbt_http_fec_frame_decode(&rx, chunk, n / 2, &out, &out_len, &err);
    ok(m == n / 2 && err == TBT_RX_NEED_MORE, "fec chunk split waits", NULL);
    m = tbt_http_fec_frame_decode(&rx, chunk + n / 2, n - n / 2, &out, &out_len, &err);
    ok(err == TBT_RX_OK && out_len == sizeof(rec) && !memcmp(out, rec, sizeof(rec)),
       "fec chunk round-trips", NULL);

    /* Two complete chunks in one buffer. */
    tbt_http_chunk_reset(&rx.chunk);
    rx.chunk_started = false;
    rx.rec_len = 0;
    memcpy(two, chunk, n);
    memcpy(two + n, chunk, n);
    m = tbt_http_fec_frame_decode(&rx, two, 2 * n, &out, &out_len, &err);
    ok(err == TBT_RX_OK && out_len == sizeof(rec), "first of two fec chunks", NULL);
    m += tbt_http_fec_frame_decode(&rx, two + m, 2 * n - m, &out, &out_len, &err);
    ok(err == TBT_RX_OK && out_len == sizeof(rec), "second of two fec chunks", NULL);

    /* A drip chunk (1 byte) before the record is skipped. */
    tbt_http_chunk_reset(&rx.chunk);
    rx.chunk_started = false;
    rx.rec_len = 0;
    {
        char drip[8];
        size_t dn = tbt_http_drip_chunk(drip, sizeof(drip));

        memcpy(two, drip, dn);
        memcpy(two + dn, chunk, n);
        m = tbt_http_fec_frame_decode(&rx, two, dn + n, &out, &out_len, &err);
        ok(err == TBT_RX_OK && out_len == sizeof(rec), "drip before fec is skipped", NULL);
    }

    /* Control record (ACK, 16-byte body) uses the same chunk path. */
    tbt_http_chunk_reset(&rx.chunk);
    rx.chunk_started = false;
    rx.rec_len = 0;
    memset(rec, 0, sizeof(rec));
    rec[0] = TBT_FEC_ACK;
    rec[26] = 16;
    rec[28] = 0x01;
    rec[29] = 0x10;
    n = tbt_http_fec_chunk_encode((char *)chunk, sizeof(chunk), rec,
                                  TBT_FEC_HDR_LEN + 16);
    m = tbt_http_fec_frame_decode(&rx, chunk, n, &out, &out_len, &err);
    ok(err == TBT_RX_OK && out_len == TBT_FEC_HDR_LEN + 16 && out[0] == TBT_FEC_ACK,
       "fec control frame in a chunk", NULL);

    /* Batch layout: eight records concatenated into one HTTP write. The
     * carrier batch path encodes each record back-to-back; one decoder pass
     * must recover all eight in order. */
    {
        u8 recs[8][TBT_FEC_HDR_LEN + 64];
        u16 lens[8];
        u8 batch[8 * TBT_HTTP_FEC_FRAME_MAX];
        size_t flen[8];
        size_t off = 0, pos = 0;
        unsigned b;

        for (b = 0; b < 8; b++)
        {
            u16 plen = (u16)(8 + b * 7);

            memset(recs[b], 0, sizeof(recs[b]));
            recs[b][0] = TBT_FEC_DATA;
            recs[b][26] = (u8)plen;
            recs[b][27] = 0;
            recs[b][28] = 0x01;
            recs[b][29] = 0x10;
            for (i = 0; i < plen; i++)
                recs[b][TBT_FEC_HDR_LEN + i] = (u8)(b * 31 + i);
            lens[b] = (u16)(TBT_FEC_HDR_LEN + plen);
        }
        for (b = 0; b < 8; b++)
        {
            size_t bn = tbt_http_fec_chunk_encode((char *)batch + off,
                                                  sizeof(batch) - off,
                                                  recs[b], lens[b]);

            ok(bn > 0, "fec batch chunk encodes", NULL);
            flen[b] = bn;
            off += bn;
        }
        ok(off <= sizeof(batch), "fec batch fits the per-carrier cap", NULL);

        tbt_http_chunk_reset(&rx.chunk);
        rx.chunk_started = false;
        rx.rec_len = 0;
        for (b = 0; b < 8; b++)
        {
            size_t used = tbt_http_fec_frame_decode(&rx, batch + pos,
                                                    off - pos, &out, &out_len,
                                                    &err);

            ok(err == TBT_RX_OK && out_len == lens[b] &&
               !memcmp(out, recs[b], lens[b]),
               "fec batch record round-trips", NULL);
            pos += used;
        }
        /* The last OK returns at DATA_END; consume its trailing CRLF too. */
        pos += tbt_http_fec_frame_decode(&rx, batch + pos, off - pos,
                                         &out, &out_len, &err);
        ok(pos == off, "fec batch consumes the whole write", NULL);

        /* Short socket write: the kernel accepted `cut` bytes, the rest goes
         * to c->part and is flushed before the next record. Cut at a frame
         * boundary and inside a frame; the tail must fit the part buffer and
         * the reconstructed byte stream must decode all eight records. */
        {
            size_t cuts[4];
            u8 part[8 * TBT_HTTP_FEC_FRAME_MAX];
            u8 joined[8 * TBT_HTTP_FEC_FRAME_MAX];
            unsigned c;

            cuts[0] = flen[0];
            cuts[1] = flen[0] + flen[1] + 17;
            cuts[2] = off - 1;
            cuts[3] = off;
            for (c = 0; c < 4; c++)
            {
                size_t cut = cuts[c], tail = off - cut;

                ok(tail <= sizeof(part),
                   "fec batch short-write tail fits part", NULL);
                memcpy(part, batch + cut, tail);
                memcpy(joined, batch, cut);
                memcpy(joined + cut, part, tail);
                ok(!memcmp(joined, batch, off),
                   "fec batch short-write reconstructs the stream", NULL);

                tbt_http_chunk_reset(&rx.chunk);
                rx.chunk_started = false;
                rx.rec_len = 0;
                pos = 0;
                for (b = 0; b < 8; b++)
                {
                    size_t used = tbt_http_fec_frame_decode(&rx, joined + pos,
                                                            off - pos, &out,
                                                            &out_len, &err);

                    ok(err == TBT_RX_OK && out_len == lens[b] &&
                       !memcmp(out, recs[b], lens[b]),
                       "fec batch short-write record round-trips", NULL);
                    pos += used;
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    const char *seed_arg = NULL;
    const char *seed_env = getenv("TBT_HTTP_SEED");
    int i;

    if (argc == 2 && !strcmp(argv[1], "--emit"))
        return emit_both_wire();
    if (argc == 2 && !strcmp(argv[1], "--emit-req"))
        return emit_req_wire();
    if (argc == 2 && !strcmp(argv[1], "--emit-resp"))
        return emit_resp_wire();
    if (argc == 2 && !strcmp(argv[1], "--capture"))
        return emit_capture_binary();
    if (argc == 2 && !strcmp(argv[1], "--capture-text"))
        return emit_capture_text();

    for (i = 1; i < argc; i++)
    {
        if (!strncmp(argv[i], "--seed=", 7))
            seed_arg = argv[i] + 7;
    }
    if (seed_arg || (seed_env && *seed_env))
    {
        const char *text = seed_arg ? seed_arg : seed_env;
        char *end = NULL;
        u64 v = strtoull(text, &end, 0);

        if (end && end != text && v)
            prop_seed = v;
    }

    test_http_date();
    test_request();
    test_req_parse();
    test_skip_header();
    test_msg_loop();
    test_far_response_to_near_parser();
    test_response();
    test_resp_header();
    test_chunk_encode();
    test_chunk_decode();
    test_chunk_split();
    test_chunk_big();
    test_pad_drip();
    test_frame_codec();
    test_fec_http();
    test_frame_stream();
    test_cadence();
    test_roundtrip();
    test_totality();

    printf("test_http: seed=0x%016llx checks=%d failures=%d\n",
           (unsigned long long)prop_seed, checks, failures);
    return failures ? 1 : 0;
}
