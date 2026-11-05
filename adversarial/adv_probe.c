/* adv_probe.c - the hostile peer, the abusive application, and the unprivileged
 * operator, in one guest-side program.
 *
 * It is deliberately not part of the module's test suite: it attacks the
 * module from outside, over the wire and through /proc, and prints one
 *   ADV_RESULT <attack> PASS|FAIL|SKIP <detail>
 * line per attack.  adversarial/run.sh turns those lines into a table.
 *
 * Every wire constant comes from tbtun/tbt_uapi.h, so a protocol change cannot
 * silently leave this attacker testing a layout the module no longer speaks.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "tbt_uapi.h"

#define UA                                                                                     \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0 "     \
    "Safari/537.36"
#define REQID "aaaaaaaaaaaaaaaaaaaaaaaaaa"

static int failures;
static int klog_last = -1;

static void result(const char *name, int ok, const char *fmt, ...)
{
    va_list ap;

    printf("ADV_RESULT %s %s ", name, ok ? "PASS" : "FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    if (!ok)
        failures++;
}

static void skip(const char *name, const char *fmt, ...)
{
    va_list ap;

    printf("ADV_RESULT %s SKIP ", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ------------------------------------------------------------------ */
/* Kernel-state sanity: every attack must leave dmesg quiet.           */

static int klog_count(void)
{
    FILE *p = popen("dmesg 2>/dev/null", "r");
    char line[512];
    int bad = 0;

    if (!p)
        return -1;
    while (fgets(line, sizeof(line), p))
    {
        if (strstr(line, "WARNING:") || strstr(line, "BUG:") || strstr(line, "Oops") ||
            strstr(line, "INFO: task") || strstr(line, "general protection fault") ||
            strstr(line, "refcount_t:") || strstr(line, "KASAN") || strstr(line, "UBSAN") ||
            strstr(line, "Call Trace:") || strstr(line, "deprecated workqueue"))
            bad++;
    }
    pclose(p);
    return bad;
}

static void klog_start(void)
{
    klog_last = klog_count();
}

static int klog_check(const char *name)
{
    int n = klog_count();

    if (n < 0 || klog_last < 0)
        return 0;
    if (n > klog_last)
    {
        printf("ADV_KLOG %s new=%d\n", name, n - klog_last);
        fflush(stdout);
        system("dmesg | tail -n 30");
        klog_last = n;
        failures++;
        return -1;
    }
    klog_last = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Sockets                                                             */

static int connect_to(const char *ip, int port, const char *bindip)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { 5, 0 };
    struct sockaddr_in a = {};

    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (bindip)
    {
        struct sockaddr_in l = { .sin_family = AF_INET };

        inet_pton(AF_INET, bindip, &l.sin_addr);
        if (bind(fd, (struct sockaddr *)&l, sizeof(l)))
        {
            close(fd);
            return -1;
        }
    }
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)))
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;

    while (len)
    {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!n)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* -2 timeout, -1 error/EOF-with-error, 0 EOF, >0 bytes */
static ssize_t read_to(int fd, void *buf, size_t len, int ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    int r = poll(&p, 1, ms);

    if (r == 0)
        return -2;
    if (r < 0)
        return errno == EINTR ? -2 : -1;
    /* A teardown may surface as POLLERR before the pending socket error is
     * read.  Report the real errno; without this the caller prints errno=0
     * and the reset contract (104/32/103) cannot be checked.  A zero
     * SO_ERROR is not an error: recv() below then reports data or EOF. */
    if (p.revents & (POLLERR | POLLNVAL))
    {
        int e = 0;
        socklen_t el = sizeof(e);

        if (p.revents & POLLNVAL)
        {
            errno = EBADF;
            return -1;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) == 0 && e)
        {
            errno = e;
            return -1;
        }
    }
    return recv(fd, buf, len, MSG_DONTWAIT);
}

/* Read exactly len bytes or fail.  The isolation and recovery probes must
 * not mistake a short read for a matching echo. */
static int recv_exact(int fd, void *buf, size_t len, int ms)
{
    unsigned char *p = buf;
    size_t got = 0;
    long end = now_ms() + ms;

    while (got < len)
    {
        long left = end - now_ms();
        ssize_t n;

        if (left <= 0)
            return -1;
        n = read_to(fd, p + got, len - got, (int)left);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

static void close_rst(int fd)
{
    struct linger lg = { 1, 0 };

    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(fd);
}

/* Read until the peer closes. Returns 1 on EOF/RST, 0 on timeout. */
static int expect_close(int fd, int ms)
{
    long end = now_ms() + ms;

    while (now_ms() < end)
    {
        char b[4096];
        ssize_t n = read_to(fd, b, sizeof(b), 200);

        if (n == 0 || n == -1)
            return 1;
        if (n == -2)
            continue;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wire frames                                                         */

struct fr
{
    uint8_t type, flags, path, ackc;
    uint16_t ackm, blen;
    uint64_t seq, uack;
    uint32_t sid;
    uint8_t sidhi, res2;
    uint16_t conv;
};

static void p16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void p32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void p64(unsigned char *p, uint64_t v)
{
    p32(p, (uint32_t)v);
    p32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t g16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t g32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t g64(const unsigned char *p)
{
    return (uint64_t)g32(p) | (uint64_t)g32(p + 4) << 32;
}

static void fr_encode(unsigned char *p, const struct fr *f)
{
    memset(p, 0, 32);
    p[0] = f->type;
    p[1] = f->flags;
    p[2] = f->path;
    p[3] = f->ackc;
    p16(p + 4, f->ackm);
    p16(p + 6, f->blen);
    p64(p + 8, f->seq);
    p64(p + 16, f->uack);
    p32(p + 24, f->sid);
    p[28] = f->sidhi;
    p[29] = f->res2;
    p16(p + 30, f->conv);
}

static void fr_decode(const unsigned char *p, struct fr *f)
{
    f->type = p[0];
    f->flags = p[1];
    f->path = p[2];
    f->ackc = p[3];
    f->ackm = g16(p + 4);
    f->blen = g16(p + 6);
    f->seq = g64(p + 8);
    f->uack = g64(p + 16);
    f->sid = g32(p + 24);
    f->sidhi = p[28];
    f->res2 = p[29];
    f->conv = g16(p + 30);
}

static int send_chunk(int fd, const void *data, size_t len)
{
    char line[32];
    int n = snprintf(line, sizeof(line), "%zx\r\n", len);

    if (send_all(fd, line, (size_t)n))
        return -1;
    if (len && send_all(fd, data, len))
        return -1;
    return send_all(fd, "\r\n", 2);
}

static int send_chunk_sizeline(int fd, const char *size, const void *data, size_t len)
{
    char line[64];
    int n = snprintf(line, sizeof(line), "%s\r\n", size);

    if (send_all(fd, line, (size_t)n))
        return -1;
    if (len && send_all(fd, data, len))
        return -1;
    return send_all(fd, "\r\n", 2);
}

static int send_frame(int fd, const struct fr *f, const void *body, size_t blen)
{
    unsigned char rec[32 + 8192];
    struct fr h = *f;

    h.blen = (uint16_t)blen;
    fr_encode(rec, &h);
    if (blen)
        memcpy(rec + 32, body, blen);
    return send_chunk(fd, rec, 32 + blen);
}

/* The raw profile has no HTTP container: one record follows another.  The
 * near side's raw carrier keeps a partial record in the module and returns
 * -EAGAIN for the *next* frame, which is the narrow path review 22 is about;
 * the http container reports a short write as -EIO and retires the carrier,
 * so the backpressure test must use raw. */
static int raw_send_frame(int fd, const struct fr *f, const void *body, size_t blen)
{
    unsigned char rec[32 + 8192];
    struct fr h = *f;

    h.blen = (uint16_t)blen;
    fr_encode(rec, &h);
    if (blen)
        memcpy(rec + 32, body, blen);
    return send_all(fd, rec, 32 + blen);
}

static int raw_send_pad(int fd, int idx, uint32_t sid, uint8_t sidhi, uint64_t uack)
{
    struct fr p = { .type = TBT_F_PAD,
                    .path = (uint8_t)idx,
                    .sid = sid,
                    .sidhi = sidhi,
                    .uack = uack,
                    .conv = TBT_CONV };

    return raw_send_frame(fd, &p, NULL, 0);
}

static int raw_send_fin(int fd, int idx, uint32_t sid, uint8_t sidhi, uint64_t seq)
{
    struct fr f = { .type = TBT_F_FIN,
                    .path = (uint8_t)idx,
                    .sid = sid,
                    .sidhi = sidhi,
                    .seq = seq,
                    .conv = TBT_CONV };

    return raw_send_frame(fd, &f, NULL, 0);
}


/* One DATA record.  The far side orders it by `seq` in the stream, not by the
 * order the records arrive on the wire; the overlap attack depends on that. */
static int send_data(int fd, int idx, uint32_t sid, uint8_t sidhi, uint64_t seq,
                     const void *buf, size_t len)
{
    struct fr d = { .type = TBT_F_DATA,
                    .path = (uint8_t)idx,
                    .sid = sid,
                    .sidhi = sidhi,
                    .seq = seq,
                    .conv = TBT_CONV };

    return send_frame(fd, &d, buf, len);
}

/* A PAD with a stream id and a cumulative ack.  The header uack is applied to
 * the named stream before the frame type is dispatched (tbt_rx_frame()), so a
 * PAD is the cheapest way to advance the peer's tx_ack without carrying data
 * in the reverse direction. */
static int send_pad(int fd, int idx, uint32_t sid, uint8_t sidhi, uint64_t uack)
{
    struct fr p = { .type = TBT_F_PAD,
                    .path = (uint8_t)idx,
                    .sid = sid,
                    .sidhi = sidhi,
                    .uack = uack,
                    .conv = TBT_CONV };

    return send_frame(fd, &p, NULL, 0);
}


/* Byte at stream offset i is (i * 7 + 3) & 0xff, so a delivered run can be
 * checked against its own offsets rather than against a copy of the payload. */
static void pattern_fill(unsigned char *p, size_t n, uint64_t off)
{
    size_t i;

    for (i = 0; i < n; i++)
        p[i] = (unsigned char)((off + i) * 7u + 3u);
}

/* Assemble a byte stream from DATA frames that may coalesce, split, or be
 * retransmitted.  `have` is the contiguous prefix already assembled.
 * Returns 1 when the frame extended the stream, 0 when it repeated delivered
 * bytes that matched, -1 when it contradicted them, skipped a gap, or ran
 * past the buffer.  A retransmission is normal on a lossy or slow path; only
 * a contradiction is corruption. */
static int acc_feed(unsigned char *buf, size_t cap, size_t *have,
                    const unsigned char *body, size_t blen, uint64_t seq)
{
    size_t overlap;

    if (seq > *have || seq + blen > cap)
        return -1;
    if (seq + blen <= *have)
        return memcmp(buf + seq, body, blen) ? -1 : 0;
    overlap = (seq < *have) ? *have - (size_t)seq : 0;
    if (overlap && memcmp(buf + seq, body, overlap))
        return -1;
    memcpy(buf + seq, body, blen);
    *have = (size_t)seq + blen;
    return 1;
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ------------------------------------------------------------------ */
/* The exact request header block tbtun emits (DESIGN.md 3.2).         */

static int send_request(int fd, int no_host, int big_header)
{
    char buf[12000];
    size_t off = 0;

    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "POST /api/v2/upload HTTP/1.1\r\n");
    if (!no_host)
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "Host: 127.0.0.1\r\n");
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "User-Agent: %s\r\n", UA);
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "Accept: */*\r\n");
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "Accept-Encoding: identity\r\n");
    off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                            "Content-Type: application/octet-stream\r\n");
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "Transfer-Encoding: chunked\r\n");
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "Connection: keep-alive\r\n");
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "X-Request-Id: %s\r\n", REQID);
    if (big_header)
    {
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "X-Big: ");
        memset(buf + off, 'a', 9000);
        off += 9000;
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "\r\n");
    }
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "\r\n");
    return send_all(fd, buf, off);
}

static int send_response_header(int fd, int bad_kind)
{
    const char *h;

    switch (bad_kind)
    {
    case 0: /* non-chunked, Content-Length */
        h = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
            "Content-Length: 10\r\nConnection: keep-alive\r\n\r\n0123456789";
        break;
    case 1: /* 500 */
        h = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n"
            "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n0\r\n\r\n";
        break;
    case 2: /* header block far over the carrier buffer */
    {
        static char big[10000];
        size_t off = (size_t)snprintf(big, sizeof(big),
                                      "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream"
                                      "\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive"
                                      "\r\nX-Big: ");
        memset(big + off, 'b', 9000);
        off += 9000;
        memcpy(big + off, "\r\n\r\n", 4);
        off += 4;
        return send_all(fd, big, off);
    }
    case 3: /* keep-alive refused */
        h = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
            "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
        break;
    default: /* good */
        h = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
            "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\nServer: nginx\r\n"
            "Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n";
        break;
    }
    return send_all(fd, h, strlen(h));
}

/* ------------------------------------------------------------------ */
/* Chunked reader with a response/request header stage.                */

struct rb
{
    int fd;
    unsigned char b[131072];
    size_t have;
    int hdr_done;
};

static void rb_consume(struct rb *r, size_t n)
{
    if (n > r->have)
        n = r->have;
    memmove(r->b, r->b + n, r->have - n);
    r->have -= n;
}

static int rb_fill(struct rb *r, int ms)
{
    ssize_t n;

    if (r->have == sizeof(r->b))
        return -1;
    n = read_to(r->fd, r->b + r->have, sizeof(r->b) - r->have, ms);
    if (n <= 0)
        return -1;
    r->have += (size_t)n;
    return 0;
}

static unsigned char *memfind(unsigned char *hay, size_t n, const char *needle, size_t m)
{
    size_t i;

    if (m > n)
        return NULL;
    for (i = 0; i + m <= n; i++)
        if (!memcmp(hay + i, needle, m))
            return hay + i;
    return NULL;
}

/* Consume the HTTP header block (request on the far side, response on the
 * near side). Returns 0 when consumed. */
static int rb_header(struct rb *r, int ms)
{
    for (;;)
    {
        unsigned char *p;

        if (!r->hdr_done)
        {
            p = memfind(r->b, r->have, "\r\n\r\n", 4);
            if (!p)
            {
                if (r->have > 70000)
                    return -1;
                if (rb_fill(r, ms))
                    return -1;
                continue;
            }
            rb_consume(r, (size_t)(p - r->b) + 4);
            r->hdr_done = 1;
        }
        return 0;
    }
}

/* Next frame in the chunked body. 0 = frame, -1 = closed/proto. */
static int rb_frame(struct rb *r, struct fr *f, unsigned char *body, size_t cap, int ms)
{
    for (;;)
    {
        unsigned char *crlf;
        size_t line;
        char sizebuf[32];
        unsigned long size;
        char *end;
        size_t need;

        if (!r->hdr_done && rb_header(r, ms))
            return -1;
        crlf = memfind(r->b, r->have, "\r\n", 2);
        if (!crlf)
        {
            if (r->have > 4096)
                return -1;
            if (rb_fill(r, ms))
                return -1;
            continue;
        }
        line = (size_t)(crlf - r->b);
        if (line >= sizeof(sizebuf))
            return -1;
        memcpy(sizebuf, r->b, line);
        sizebuf[line] = '\0';
        errno = 0;
        size = strtoul(sizebuf, &end, 16);
        if (errno || end == sizebuf)
            return -1;
        need = line + 2 + size + 2;
        if (r->have < need)
        {
            if (size > sizeof(r->b) || rb_fill(r, ms))
                return -1;
            continue;
        }
        if (size == 0)
        {
            rb_consume(r, line + 2);
            r->hdr_done = 0; /* next segment starts with a header */
            continue;
        }
        if (size == 1)
        {
            rb_consume(r, need);
            continue;
        }
        if (size < 32)
            return -1;
        fr_decode(r->b + line + 2, f);
        if ((size_t)f->blen + 32 != size)
            return -1;
        if (f->blen > cap)
            return -1;
        if (f->blen)
            memcpy(body, r->b + line + 2 + 32, f->blen);
        rb_consume(r, need);
        return 0;
    }
}

static int raw_read_frame(struct rb *r, struct fr *f, unsigned char *body,
                          size_t cap, int ms)
{
    while (r->have < 32)
    {
        if (rb_fill(r, ms))
            return -1;
    }
    fr_decode(r->b, f);
    if (f->blen > cap)
        return -1;
    while (r->have < (size_t)32 + f->blen)
    {
        if (rb_fill(r, ms))
            return -1;
    }
    if (f->blen)
        memcpy(body, r->b + 32, f->blen);
    rb_consume(r, (size_t)32 + f->blen);
    return 0;
}

static void hello_body(unsigned char *b, int role, const unsigned char *nonce, unsigned char pg)
{
    b[0] = 1; /* version major */
    b[1] = (unsigned char)role;
    memcpy(b + 2, nonce, 16);
    p16(b + 18, 8192);
    p16(b + 20, 8228);
    b[22] = pg;
}

static int send_hello(int fd, const unsigned char *nonce, int idx, unsigned char pg, int role)
{
    unsigned char body[23];
    struct fr f = { .type = TBT_F_HELLO, .path = (uint8_t)idx, .conv = TBT_CONV };

    hello_body(body, role, nonce, pg);
    return send_frame(fd, &f, body, sizeof(body));
}

/* Read frames until a HELLO; fill gen from body[22] high nibble. */
static int read_hello(struct rb *r, struct fr *f, unsigned char *body, size_t cap, int *gen,
                      int *idx)
{
    for (;;)
    {
        if (rb_frame(r, f, body, cap, 3000))
            return -1;
        if (f->type == TBT_F_HELLO)
        {
            if (f->blen != 23)
                return -1;
            if (gen)
                *gen = (body[22] >> 2) & 0xf;
            if (idx)
                *idx = f->path;
            return 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* /proc state readers                                                 */

static unsigned long long stat_key(int tunnel, const char *key)
{
    FILE *p = fopen("/proc/net/tbtun/stats", "r");
    char line[1024];
    char want[64];
    unsigned long long v = 0;

    if (!p)
        return 0;
    snprintf(want, sizeof(want), "tunnel %d ", tunnel);
    while (fgets(line, sizeof(line), p))
    {
        char *tok;

        if (!strstr(line, want))
            continue;
        tok = strstr(line, key);
        if (tok && tok[strlen(key)] == '=')
            v = strtoull(tok + strlen(key) + 1, NULL, 10);
    }
    fclose(p);
    return v;
}

/* Like stat_key(), but a missing key is distinguishable from a zero value. */
static long stat_key_opt(int tunnel, const char *key, long dflt)
{
    FILE *p = fopen("/proc/net/tbtun/stats", "r");
    char line[1024];
    char want[64];
    size_t klen = strlen(key);
    long v = dflt;

    if (!p)
        return dflt;
    snprintf(want, sizeof(want), "tunnel %d ", tunnel);
    while (fgets(line, sizeof(line), p))
    {
        char *tok;

        if (!strstr(line, want))
            continue;
        tok = strstr(line, key);
        if (tok && tok[klen] == '=')
        {
            v = strtol(tok + klen + 1, NULL, 10);
            break;
        }
    }
    fclose(p);
    return v;
}

/* The pinned global counter: cumulative accepted streams, not active ones. */
static long stat_streams_total(void)
{
    static const char key[] = "streams_total=";
    FILE *p = fopen("/proc/net/tbtun/stats", "r");
    char line[1024];
    long v = -1;

    if (!p)
        return -1;
    while (fgets(line, sizeof(line), p))
    {
        char *tok = strstr(line, key);

        if (tok)
        {
            v = strtol(tok + sizeof(key) - 1, NULL, 10);
            break;
        }
    }
    fclose(p);
    return v;
}

static unsigned long long peer_uack(int tunnel, int carrier)
{
    FILE *p = fopen("/proc/net/tbtun/peers", "r");
    char line[512];
    unsigned long long v = 0;

    if (!p)
        return 0;
    while (fgets(line, sizeof(line), p))
    {
        unsigned long long id, uack;
        unsigned int car;

        if (line[0] == '#')
            continue;
        if (sscanf(line, "%llu %u %*s %*s %llu", &id, &car, &uack) == 3 &&
            id == (unsigned long long)tunnel && car == (unsigned int)carrier)
            v = uack;
    }
    fclose(p);
    return v;
}

/* Does the canonical config contain `needle` on the line for `tunnel`?  This
 * is the operator-visible half of the nonce/anchor contract: a key that is
 * accepted but not rendered cannot survive a save/restore. */
static int cfg_line_has(int tunnel, const char *needle)
{
    FILE *p = fopen("/proc/net/tbtun/config", "r");
    char line[2048];
    char want[32];
    int found = 0;

    if (!p)
        return 0;
    snprintf(want, sizeof(want), "tunnel %d ", tunnel);
    while (fgets(line, sizeof(line), p))
    {
        if (strstr(line, want) && strstr(line, needle))
        {
            found = 1;
            break;
        }
    }
    fclose(p);
    return found;
}

/* A labelled block from a /proc table on the console.  The host ignores
 * non-ADV lines; the block is evidence for where a frame went, which no
 * single result line can carry. */
static void dump_table(const char *tag, const char *cmd)
{
    printf("FAR_TABLE_%s_BEGIN\n", tag);
    fflush(stdout);
    system(cmd);
    printf("FAR_TABLE_%s_END\n", tag);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Attack 1: hostile carrier on the far listen port                    */

static int far_conn(int port)
{
    int fd = connect_to("127.0.0.1", port, NULL);

    if (fd < 0)
        return -1;
    if (send_request(fd, 0, 0))
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int far_join_rb(int port, const unsigned char *nonce, int idx, unsigned char pg, int *gen,
                       int *outidx, struct rb *r)
{
    struct fr f;
    unsigned char body[128];
    int fd = far_conn(port);

    if (fd < 0)
        return -1;
    memset(r, 0, sizeof(*r));
    r->fd = fd;
    if (send_hello(fd, nonce, idx, pg, TBT_ROLE_NEAR))
    {
        close(fd);
        return -1;
    }
    if (read_hello(r, &f, body, sizeof(body), gen, outidx))
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int far_join(int port, const unsigned char *nonce, int idx, unsigned char pg, int *gen,
                    int *outidx)
{
    struct rb r;

    return far_join_rb(port, nonce, idx, pg, gen, outidx, &r);
}

static int far_suite(int port, int tunnel)
{
    unsigned char nonce[16], nonce2[16];
    unsigned char body[8192];
    struct fr f;
    static struct rb r;
    static struct rb ar;
    int fd, i;
    int have_session = 0, anchor = -1, anchor_gen = 0;
    uint32_t sid = 0x13572468u;
    unsigned long long jb, jd;

    for (i = 0; i < 16; i++)
    {
        nonce[i] = (unsigned char)(0xa0 + i);
        nonce2[i] = (unsigned char)(0xa0 + i) ^ 1;
    }
    klog_start();

    /* 1. Plain garbage: no request header at all. */
    fd = connect_to("127.0.0.1", port, NULL);
    if (fd < 0)
    {
        result("far.garbage", 0, "cannot connect to the carrier port");
    }
    else
    {
        char g[9000];

        memset(g, 'G', sizeof(g));
        send_all(fd, g, sizeof(g));
        result("far.garbage", expect_close(fd, 3000),
               "9000 bytes of non-HTTP garbage; carrier must close, not crash");
        close(fd);
    }
    klog_check("far.garbage");

    /* 2. A valid request header and HELLO, then garbage. This is the first
     *    HELLO the far session sees, so it adopts our nonce. */
    fd = far_conn(port);
    if (fd >= 0)
    {
        memset(&r, 0, sizeof(r));
        r.fd = fd;
        if (send_hello(fd, nonce, 0, 1, TBT_ROLE_NEAR) == 0 &&
            read_hello(&r, &f, body, sizeof(body), &anchor_gen, NULL) == 0)
        {
            char g[64];

            have_session = 1;
            memset(g, 'Z', sizeof(g));
            send_all(fd, g, sizeof(g));
            result("far.hello_garbage", expect_close(fd, 3000),
                   "HELLO joined (nonce adopted), then 64 garbage bytes; carrier must close");
        }
        else
        {
            result("far.hello_garbage", 0, "the far side did not answer a valid HELLO");
        }
        close(fd);
    }
    else
    {
        result("far.hello_garbage", 0, "cannot connect");
    }
    klog_check("far.hello_garbage");

    if (!have_session)
    {
        const char *names[] = { "far.wrong_nonce", "far.dup_index", "far.srcaddr_dup",
                                "far.srcaddr_free", "far.chunk_huge", "far.bodylen_tail",
                                "far.split_record", "far.drip_split", "far.chunk_oversize",
                                "far.no_host", "far.headers_big", "far.open_other_sid",
                                "far.fbk_forged", "far.hello_profile0", "far.hello_profile9",
                                "far.data_path" };

        for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
            skip(names[i], "the session nonce was never established");
        return failures;
    }
    msleep(400);

    /* 3. Wrong nonce, same index. */
    jb = stat_key(tunnel, "join_bad") + stat_key(tunnel, "join_nonce_reject");
    fd = far_conn(port);
    if (fd >= 0)
    {
        memset(&r, 0, sizeof(r));
        r.fd = fd;
        send_hello(fd, nonce2, 0, 1, TBT_ROLE_NEAR);
        result("far.wrong_nonce", expect_close(fd, 3000),
               "HELLO with the wrong 128-bit nonce: carrier closed; "
               "join_bad+nonce_reject %llu -> %llu", jb,
               stat_key(tunnel, "join_bad") + stat_key(tunnel, "join_nonce_reject"));
        close(fd);
    }
    else
        result("far.wrong_nonce", 0, "cannot connect");
    klog_check("far.wrong_nonce");
    msleep(400);

    /* 4. The anchor: a live carrier at index 0 with the adopted nonce. */
    anchor = far_join_rb(port, nonce, 0, 1, &anchor_gen, NULL, &ar);
    if (anchor < 0)
    {
        result("far.anchor", 0, "a legitimate carrier could not join after the attacks");
        return failures;
    }
    result("far.anchor", 1, "carrier joined index 0, far generation %d", anchor_gen);
    {
        struct fr o = { .type = TBT_F_OPEN, .path = 0, .sid = sid, .conv = TBT_CONV };

        send_frame(anchor, &o, NULL, 0);
    }
    msleep(300);
    klog_check("far.anchor");

    /* 5. Duplicate (nonce, index) while the anchor is live. */
    jd = stat_key(tunnel, "join_dup");
    fd = far_conn(port);
    if (fd >= 0)
    {
        memset(&r, 0, sizeof(r));
        r.fd = fd;
        send_hello(fd, nonce, 0, 1, TBT_ROLE_NEAR);
        result("far.dup_index", expect_close(fd, 3000),
               "duplicate (nonce,index=0): carrier closed; join_dup %llu -> %llu; "
               "anchor must survive", jd, stat_key(tunnel, "join_dup"));
        close(fd);
    }
    else
        result("far.dup_index", 0, "cannot connect");
    klog_check("far.dup_index");
    msleep(300);

    /* 6. Same nonce from a different source address, same index: duplicate. */
    fd = connect_to("127.0.0.1", port, "127.0.0.2");
    if (fd >= 0)
    {
        memset(&r, 0, sizeof(r));
        r.fd = fd;
        send_request(fd, 0, 0);
        send_hello(fd, nonce, 0, 1, TBT_ROLE_NEAR);
        result("far.srcaddr_dup", expect_close(fd, 3000),
               "same nonce and index from 127.0.0.2: refused as a duplicate");
        close(fd);
    }
    else
        result("far.srcaddr_dup", 0, "cannot bind 127.0.0.2");
    klog_check("far.srcaddr_dup");
    msleep(300);

    /* 7. Same nonce from a different source address, free index.  The safe
     *    result is a refusal; a join means the nonce is replayable from any
     *    address, which is the session-hijack property to record. */
    fd = connect_to("127.0.0.1", port, "127.0.0.2");
    if (fd >= 0)
    {
        memset(&r, 0, sizeof(r));
        r.fd = fd;
        send_request(fd, 0, 0);
        send_hello(fd, nonce, 1, 1, TBT_ROLE_NEAR);
        {
            int accepted = read_hello(&r, &f, body, sizeof(body), NULL, NULL) == 0;

            result("far.srcaddr_free", !accepted,
                   "same nonce from 127.0.0.2 on a free index: %s",
                   accepted ? "ACCEPTED (no source binding; replayed nonce hijacks a slot)"
                            : "refused");
        }
        close(fd);
    }
    else
        result("far.srcaddr_free", 0, "cannot bind 127.0.0.2");
    klog_check("far.srcaddr_free");
    msleep(400);

    /* 8. Chunk size 0xFFFFFFFF. */
    fd = far_join(port, nonce, 1, 1, NULL, NULL);
    if (fd >= 0)
    {
        send_chunk_sizeline(fd, "ffffffff", NULL, 0);
        result("far.chunk_huge", expect_close(fd, 3000), "chunk size 0xFFFFFFFF must be PROTO");
        close(fd);
    }
    else
        result("far.chunk_huge", 0, "carrier join failed");
    klog_check("far.chunk_huge");
    msleep(400);

    /* 9. A record claiming body_len 8192 with a 3-byte tail, then FIN. */
    fd = far_join(port, nonce, 1, 1, NULL, NULL);
    if (fd >= 0)
    {
        unsigned char rec[35];
        struct fr h = { .type = TBT_F_DATA, .path = 1, .sid = sid, .seq = 0,
                        .conv = TBT_CONV };

        fr_encode(rec, &h);
        p16(rec + 6, 8192); /* claim 8192, send 3 */
        rec[32] = 'a';
        rec[33] = 'b';
        rec[34] = 'c';
        send_chunk(fd, rec, sizeof(rec));
        shutdown(fd, SHUT_WR);
        msleep(700);
        result("far.bodylen_tail", klog_check("far.bodylen_tail") == 0,
               "8224-byte chunk, 35 bytes sent, then FIN: no warning, no overread");
        close(fd);
    }
    else
        result("far.bodylen_tail", 0, "carrier join failed");
    msleep(400);

    /* 10. One record split across two chunks: the first chunk already lies
     *     about its size, so it must be PROTO before any body is trusted. */
    fd = far_join(port, nonce, 1, 1, NULL, NULL);
    if (fd >= 0)
    {
        unsigned char rec[32];
        struct fr h = { .type = TBT_F_DATA, .path = 1, .sid = sid, .seq = 0,
                        .conv = TBT_CONV };

        fr_encode(rec, &h);
        p16(rec + 6, 100);
        send_chunk(fd, rec, 32);
        result("far.split_record", expect_close(fd, 3000),
               "chunk 32 with body_len 100 (record split across chunks) must be PROTO");
        close(fd);
    }
    else
        result("far.split_record", 0, "carrier join failed");
    klog_check("far.split_record");
    msleep(400);

    /* 11. A drip chunk inside a record boundary. */
    fd = far_join(port, nonce, 1, 1, NULL, NULL);
    if (fd >= 0)
    {
        unsigned char rec[32];
        struct fr h = { .type = TBT_F_DATA, .path = 1, .sid = sid, .seq = 0,
                        .conv = TBT_CONV };

        fr_encode(rec, &h);
        p16(rec + 6, 100);
        send_chunk(fd, "x", 1);
        send_chunk(fd, rec, 32);
        result("far.drip_split", expect_close(fd, 3000),
               "drip chunk between header and body must still be PROTO");
        close(fd);
    }
    else
        result("far.drip_split", 0, "carrier join failed");
    klog_check("far.drip_split");
    msleep(400);

    /* 12. A chunk larger than TBT_CHUNK_MAX. */
    fd = far_join(port, nonce, 1, 1, NULL, NULL);
    if (fd >= 0)
    {
        send_chunk_sizeline(fd, "2025", NULL, 0); /* 8229 */
        result("far.chunk_oversize", expect_close(fd, 3000),
               "chunk 8229 > TBT_CHUNK_MAX (8228) must be PROTO");
        close(fd);
    }
    else
        result("far.chunk_oversize", 0, "carrier join failed");
    klog_check("far.chunk_oversize");
    msleep(400);

    /* 13. A request with no Host. */
    fd = connect_to("127.0.0.1", port, NULL);
    if (fd >= 0)
    {
        send_request(fd, 1, 0);
        result("far.no_host", expect_close(fd, 3000),
               "request without Host must be PROTO");
        close(fd);
    }
    else
        result("far.no_host", 0, "cannot connect");
    klog_check("far.no_host");
    msleep(300);

    /* 14. A request header block far over the carrier buffer. */
    fd = connect_to("127.0.0.1", port, NULL);
    if (fd >= 0)
    {
        send_request(fd, 0, 1);
        result("far.headers_big", expect_close(fd, 3000),
               "9000-byte header block must be PROTO (effective cap: carrier buffer)");
        close(fd);
    }
    else
        result("far.headers_big", 0, "cannot connect");
    klog_check("far.headers_big");
    msleep(300);

    /* 15. Multiconn: an OPEN for a second sid on a second carrier is legal.
     *     It must be adopted as its own stream, its DATA must come back under
     *     that sid, and stream 1 must not see its bytes. */
    {
        uint32_t sid2 = sid ^ 0x55;
        static struct rb r2;
        int fd2 = far_join_rb(port, nonce, 1, 1, NULL, NULL, &r2);

        if (fd2 < 0)
        {
            result("far.open_other_sid", 0, "the second carrier did not join");
        }
        else
        {
            struct fr o = { .type = TBT_F_OPEN, .path = 1, .sid = sid2,
                            .conv = TBT_CONV };
            unsigned char s2body[8192];
            int echoed = 0, crossed = 0, closed = 0;
            long end2;

            send_frame(fd2, &o, NULL, 0);
            msleep(300);
            send_data(fd2, 1, sid2, 0, 0, "two!", 4);
            end2 = now_ms() + 4000;
            while (now_ms() < end2 && !echoed)
            {
                struct pollfd p[2];
                int pr, k;

                p[0].fd = anchor;
                p[0].events = POLLIN;
                p[0].revents = 0;
                p[1].fd = fd2;
                p[1].events = POLLIN;
                p[1].revents = 0;
                pr = poll(p, 2, 200);
                if (pr <= 0)
                {
                    /* A closed carrier is the old contract; the second
                     * stream must not cost the carrier. */
                    if (recv(fd2, s2body, 1, MSG_DONTWAIT | MSG_PEEK) == 0)
                    {
                        closed = 1;
                        break;
                    }
                    continue;
                }
                for (k = 0; k < 2; k++)
                {
                    struct rb *rr = k ? &r2 : &ar;
                    int rc;

                    if (!(p[k].revents & POLLIN))
                        continue;
                    rc = rb_frame(rr, &f, s2body, sizeof(s2body), 200);
                    if (rc)
                        continue;
                    if (f.type == TBT_F_DATA && f.blen)
                    {
                        if (f.sid == sid2 && f.seq == 0 && f.blen == 4 &&
                            !memcmp(s2body, "two!", 4))
                            echoed = 1;
                        else if (f.sid == sid)
                            crossed = 1; /* stream 1 saw stream 2's payload */
                    }
                }
            }
            /* Retire the second stream; stream 1 must survive. */
            {
                struct fr rst = { .type = TBT_F_RST, .path = 1, .sid = sid2,
                                  .conv = TBT_CONV };

                send_frame(fd2, &rst, NULL, 0);
            }
            result("far.open_other_sid", echoed && !crossed && !closed,
                   "second OPEN (sid %08x) on carrier 1: adopted and echoed=%d, "
                   "stream 1 crossed=%d, carrier closed=%d (multiconn accepts "
                   "a second stream; it must not be a PROTO close)",
                   sid2, echoed, crossed, closed);
            close(fd2);
        }
    }
    klog_check("far.open_other_sid");
    msleep(400);

    /* 16. Forged FBK: huge uack/recv_cnt for a generation that does not exist,
     *     and path index 15 on a 4-carrier session. */
    fd = far_join(port, nonce, 1, 1, NULL, NULL);
    if (fd >= 0)
    {
        unsigned char fb[18];
        struct fr h = { .type = TBT_F_FBK, .path = 1, .ackc = 1, .conv = TBT_CONV };
        unsigned long long before, after;

        memset(fb, 0, sizeof(fb));
        fb[0] = (unsigned char)((anchor_gen & 0xf) << 4); /* path 0, real gen */
        p64(fb + 2, 5000);
        send_frame(fd, &h, fb, sizeof(fb));
        msleep(200);
        before = peer_uack(tunnel, 0);

        fb[0] = (unsigned char)((((anchor_gen + 1) & 0xf) << 4) | 0);
        p64(fb + 2, 999999);
        p32(fb + 10, 0xffffffffu);
        send_frame(fd, &h, fb, sizeof(fb));
        fb[0] = (unsigned char)((anchor_gen & 0xf) << 4 | 15); /* path 15 */
        send_frame(fd, &h, fb, sizeof(fb));
        msleep(300);
        after = peer_uack(tunnel, 0);
        result("far.fbk_forged", before == 5000 && after == 5000,
               "FBK uack 999999 with gen+1 and path 15: carrier 0 uack %llu -> %llu "
               "(must stay 5000)", before, after);
        close(fd);
    }
    else
        result("far.fbk_forged", 0, "carrier join failed");
    klog_check("far.fbk_forged");
    msleep(400);

    /* 17. HELLO profile 0 (invalid) and 9 (which is profile 1, gen 2). */
    fd = far_join(port, nonce, 1, 0, NULL, NULL);
    result("far.hello_profile0", fd < 0,
           "HELLO with profile 0 must be refused");
    if (fd >= 0)
        close(fd);
    klog_check("far.hello_profile0");
    msleep(400);

    fd = far_join(port, nonce, 1, 9, NULL, NULL);
    result("far.hello_profile9", fd >= 0,
           "HELLO byte 9 = profile 1, generation 2: accepted as a generation, not a profile");
    if (fd >= 0)
        close(fd);
    klog_check("far.hello_profile9");
    msleep(400);

    /* 18. The anchor must still carry data, and a duplicate seq must not
     *     deliver a second time. */
    if (anchor >= 0)
    {
        struct fr d = { .type = TBT_F_DATA, .path = 0, .sid = sid, .conv = TBT_CONV };
        unsigned char stream[16];
        size_t have = 0;
        int ok = 1, corrupt = 0;
        long end18, idle = 0;

        memset(stream, 0, sizeof(stream));
        dump_table("PEERS_BEFORE_DATA", "cat /proc/net/tbtun/peers");
        d.seq = 0;
        send_frame(anchor, &d, "ping", 4);
        d.seq = 4;
        send_frame(anchor, &d, "pong", 4);
        d.seq = 0;
        send_frame(anchor, &d, "XXXX", 4); /* duplicate seq: must be dropped */
        /* Five seconds, not one: the far may have to announce the stream on
         * the carrier again after the preceding carrier churn, and a single
         * 1 s rb_frame() timeout would blame the wrong layer. */
        end18 = now_ms() + 5000;
        while (now_ms() < end18)
        {
            struct pollfd p = { .fd = anchor, .events = POLLIN };
            int pr = poll(&p, 1, 200);

            if (pr <= 0)
            {
                if (have >= 8 && ++idle >= 3)
                    break;
                continue;
            }
            if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
                break;
            if (rb_frame(&ar, &f, body, sizeof(body), 200))
                continue;
            idle = 0;
            if (f.type == TBT_F_DATA && f.sid == sid && f.blen)
            {
                /* The far may coalesce the two 4-byte writes into one frame
                 * and retransmit it until the echo is acked; both are legal.
                 * The delivered byte stream is what must be exact. */
                if (acc_feed(stream, sizeof(stream), &have, body, f.blen,
                             f.seq) < 0)
                    corrupt = 1;
            }
        }
        dump_table("PEERS_AFTER_DATA", "cat /proc/net/tbtun/peers");
        if (corrupt || have != 8 || memcmp(stream, "pingpong", 8))
            ok = 0;
        result("far.data_path", ok,
               "anchor echo: %zu bytes '%c%c%c%c%c%c%c%c' corrupt=%d "
               "(ping+pong, coalescing/retransmit allowed; duplicate seq must "
               "not add XXXX); far stats capture_runs=%llu actor_calls=%llu "
               "submit_bytes=%llu tx_ack=%llu",
               have, have > 0 ? stream[0] : '.', have > 1 ? stream[1] : '.',
               have > 2 ? stream[2] : '.', have > 3 ? stream[3] : '.',
               have > 4 ? stream[4] : '.', have > 5 ? stream[5] : '.',
               have > 6 ? stream[6] : '.', have > 7 ? stream[7] : '.',
               corrupt,
               stat_key(tunnel, "capture_runs"), stat_key(tunnel, "actor_calls"),
               stat_key(tunnel, "submit_bytes"), stat_key(tunnel, "tx_ack"));
        klog_check("far.data_path");
        close(anchor);
    }
    else
        result("far.data_path", 0, "no anchor");

    return failures;
}

/* ------------------------------------------------------------------ */
/* Attack 1b: the far-side contract (nonce= and peer=) and carrier     */
/* input after the first connection.                                   */

/* `nonce=` pins the session nonce: the far must not adopt it, a wrong nonce
 * is refused even on the first HELLO, and only the first authenticated source
 * may join carriers.  The last property is what makes a leaked nonce
 * unreplayable from another address. */
static int mode_noncepinned(int port, int tunnel)
{
    static const unsigned char pinned[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
                                              0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
                                              0xcc, 0xdd, 0xee, 0xff };
    unsigned char wrong[16];
    unsigned char body[256];
    struct fr f;
    static struct rb r;
    int fd, joined = 0, wrong_refused = 0, stranger_refused = 0;
    long jsr0, jsr1;
    unsigned long long jb0, jb1;

    memcpy(wrong, pinned, sizeof(wrong));
    wrong[0] ^= 0x80;
    klog_start();
    jsr0 = stat_key_opt(tunnel, "join_src_reject", -1);
    jb0 = stat_key(tunnel, "join_bad");

    /* 1. The configured nonce from the first source: the far must answer. */
    fd = far_conn(port);
    if (fd < 0)
    {
        result("far.nonce_pinned", 0, "cannot connect to the carrier port");
        return failures;
    }
    memset(&r, 0, sizeof(r));
    r.fd = fd;
    send_hello(fd, pinned, 0, 1, TBT_ROLE_NEAR);
    joined = read_hello(&r, &f, body, sizeof(body), NULL, NULL) == 0;
    close(fd);
    msleep(300);

    /* 2. A wrong nonce from the same source, free index: refused. */
    fd = far_conn(port);
    if (fd >= 0)
    {
        memset(&r, 0, sizeof(r));
        r.fd = fd;
        send_hello(fd, wrong, 1, 1, TBT_ROLE_NEAR);
        wrong_refused = expect_close(fd, 3000);
        close(fd);
    }
    msleep(300);

    /* 3. The correct pinned nonce from a stranger address: refused.  A
     *    replay that joins here would hijack a carrier slot without knowing
     *    anything but the (cleartext) nonce. */
    fd = connect_to("127.0.0.1", port, "127.0.0.2");
    if (fd >= 0)
    {
        int accepted;

        memset(&r, 0, sizeof(r));
        r.fd = fd;
        send_request(fd, 0, 0);
        send_hello(fd, pinned, 1, 1, TBT_ROLE_NEAR);
        accepted = read_hello(&r, &f, body, sizeof(body), NULL, NULL) == 0;
        stranger_refused = !accepted;
        close(fd);
    }
    msleep(300);
    jsr1 = stat_key_opt(tunnel, "join_src_reject", -1);
    jb1 = stat_key(tunnel, "join_bad");
    result("far.nonce_pinned", joined && wrong_refused && stranger_refused,
           "nonce= pinned: first source joined=%d, wrong nonce refused=%d, "
           "stranger 127.0.0.2 with the correct nonce refused=%d "
           "(join_src_reject %ld -> %ld, join_bad %llu -> %llu)",
           joined, wrong_refused, stranger_refused, jsr0, jsr1, jb0, jb1);
    klog_check("far.nonce_pinned");
    return failures;
}

/* `peer=` on a far tunnel anchors the only near source allowed to join.  The
 * anchored source may join any index; a different source must be refused even
 * with the right nonce, and the key must survive the canonical config. */
static int mode_srcanchor(int port, int tunnel)
{
    unsigned char nonce[16];
    unsigned char body[256];
    struct fr f;
    static struct rb r;
    int fd, i, first = 0, anchored = 0, stranger_refused = 0, rendered;
    long jsr0, jsr1;

    for (i = 0; i < 16; i++)
        nonce[i] = (unsigned char)(0x30 + i);
    klog_start();
    jsr0 = stat_key_opt(tunnel, "join_src_reject", -1);

    /* 1. The anchored source opens the session; it adopts the nonce (no
     *    nonce= on this tunnel) and becomes the anchor. */
    fd = far_conn(port);
    if (fd < 0)
    {
        result("far.src_anchor", 0, "cannot connect to the carrier port");
        return failures;
    }
    memset(&r, 0, sizeof(r));
    r.fd = fd;
    send_hello(fd, nonce, 0, 1, TBT_ROLE_NEAR);
    first = read_hello(&r, &f, body, sizeof(body), NULL, NULL) == 0;
    close(fd);
    msleep(300);

    /* 2. The same source on a second index: normal, must join. */
    fd = far_conn(port);
    if (fd >= 0)
    {
        memset(&r, 0, sizeof(r));
        r.fd = fd;
        send_hello(fd, nonce, 1, 1, TBT_ROLE_NEAR);
        anchored = read_hello(&r, &f, body, sizeof(body), NULL, NULL) == 0;
        close(fd);
    }
    msleep(300);

    /* 3. A different source with the same nonce, free index: refused. */
    fd = connect_to("127.0.0.1", port, "127.0.0.2");
    if (fd >= 0)
    {
        int accepted;

        memset(&r, 0, sizeof(r));
        r.fd = fd;
        send_request(fd, 0, 0);
        send_hello(fd, nonce, 2, 1, TBT_ROLE_NEAR);
        accepted = read_hello(&r, &f, body, sizeof(body), NULL, NULL) == 0;
        stranger_refused = !accepted;
        close(fd);
    }
    msleep(300);
    jsr1 = stat_key_opt(tunnel, "join_src_reject", -1);
    rendered = cfg_line_has(tunnel, "peer=127.0.0.1:1");
    result("far.src_anchor", first && anchored && stranger_refused && rendered,
           "peer= anchor: anchored source joined=%d and joined index 1=%d, "
           "stranger 127.0.0.2 refused=%d, config renders peer=%d "
           "(join_src_reject %ld -> %ld)",
           first, anchored, stranger_refused, rendered, jsr0, jsr1);
    klog_check("far.src_anchor");
    return failures;
}

/* review 20: a tree node that a later contiguous frame has already delivered
 * past must leave the tree, and a frame that straddles rx_next must keep only
 * its tail.  Three rounds of [seq+4096 len 8192][seq+12288 len 4096]
 * [seq len 8192] cover 48 KiB.  Without the three-branch pull the first round
 * delivers 8 KiB and leaves 12 KiB in the tree forever; the hole budget then
 * kills the stream (or the reorder budget does).  With the fix every round
 * delivers 16 KiB, the tree returns to zero, and no budget fires. */
static int mode_overlap(int port, int tunnel)
{
    unsigned char nonce[16];
    unsigned char body[8192];
    unsigned char want[3 * 16384];
    unsigned char echo[3 * 16384];
    struct fr f;
    static struct rb r;
    int fd, i, round, bad = 0;
    uint32_t sid = 0x51d0b00bu;
    size_t have = 0;
    unsigned long long ooo0, of0, ro0, hu0, ooo1, of1, ro1, hu1;
    long end;

    for (i = 0; i < 16; i++)
        nonce[i] = (unsigned char)(0x60 + i);
    pattern_fill(want, sizeof(want), 0);
    klog_start();
    ooo0 = stat_key(tunnel, "ooo_bytes");
    of0 = stat_key(tunnel, "ooo_frames");
    ro0 = stat_key(tunnel, "reorder_overflow");
    hu0 = stat_key(tunnel, "holes_unrecoverable");

    fd = far_join_rb(port, nonce, 0, 1, NULL, NULL, &r);
    if (fd < 0)
    {
        result("far.overlap", 0, "carrier join failed");
        return failures;
    }
    {
        struct fr o = { .type = TBT_F_OPEN, .path = 0, .sid = sid, .conv = TBT_CONV };

        send_frame(fd, &o, NULL, 0);
    }
    msleep(300);

    for (round = 0; round < 3; round++)
    {
        uint64_t base = (uint64_t)round * 16384;
        unsigned char buf[8192];

        /* The tail first, then the head: the head's direct delivery advances
         * rx_next past the tail's start, which is the straddle case. */
        pattern_fill(buf, sizeof(buf), base + 4096);
        send_data(fd, 0, sid, 0, base + 4096, buf, sizeof(buf));
        pattern_fill(buf, 4096, base + 12288);
        send_data(fd, 0, sid, 0, base + 12288, buf, 4096);
        pattern_fill(buf, sizeof(buf), base);
        send_data(fd, 0, sid, 0, base, buf, sizeof(buf));
    }

    end = now_ms() + 6000;
    while (have < sizeof(want) && now_ms() < end)
    {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int pr = poll(&p, 1, 300);

        if (pr == 0)
            continue;
        if (pr < 0)
            break;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if (rb_frame(&r, &f, body, sizeof(body), 200))
            break;
        if (f.type == TBT_F_DATA && f.sid == sid && f.blen)
        {
            if (acc_feed(echo, sizeof(echo), &have, body, f.blen, f.seq) < 0)
            {
                bad = 1;
                break;
            }
            /* The far's tx window is half this tunnel's reorder budget
             * (16 KiB); ack so the 48 KiB echo can leave. */
            send_pad(fd, 0, sid, 0, have);
        }
    }
    if (have == sizeof(want) && memcmp(echo, want, sizeof(want)))
        bad = 1;
    ooo1 = stat_key(tunnel, "ooo_bytes");
    of1 = stat_key(tunnel, "ooo_frames");
    ro1 = stat_key(tunnel, "reorder_overflow");
    hu1 = stat_key(tunnel, "holes_unrecoverable");
    result("far.overlap",
           have == sizeof(want) && !bad && ooo1 == 0 && of1 == 0 && ro1 == ro0 &&
               hu1 == hu0 && klog_check("far.overlap") == 0,
           "3 rounds of [seq+4096 len 8192][seq+12288 len 4096][seq len 8192]: "
           "echoed %zu/%zu bytes corrupt=%d; ooo_bytes %llu -> %llu, ooo_frames "
           "%llu -> %llu, reorder_overflow %llu -> %llu, holes_unrecoverable "
           "%llu -> %llu (a stale node leaves ooo_bytes at 8192+ and the hole "
           "budget kills the stream)",
           have, sizeof(want), bad, ooo0, ooo1, of0, of1, ro0, ro1, hu0, hu1);
    close(fd);
    return failures;
}

/* F6: the far must keep accepting and reading carriers after the first one is
 * established.  The first carrier opens the stream; the second, a fresh TCP
 * connection on a free index, carries a PAD and the next DATA record.  The
 * echo proves the far's receive work ran on the second carrier; a missing
 * HELLO answer proves it never parsed it at all. */
static int mode_secondcarrier(int port, int tunnel)
{
    unsigned char nonce[16];
    unsigned char body[8192];
    unsigned char echo[16];
    struct fr f;
    static struct rb r0, r1;
    int fd0, fd1 = -1, i, hello1 = 0, pad_ok = 0, bad = 0;
    uint32_t sid = 0x2c0a7710u;
    uint8_t sidhi = 0;
    size_t got = 0; /* contiguous bytes assembled in echo[] */
    long end;
    static const char want[] = "alphabeta!";
    const size_t want_len = sizeof(want) - 1; /* no NUL on the wire */

    for (i = 0; i < 16; i++)
        nonce[i] = (unsigned char)(0x70 + i);
    klog_start();
    fd0 = far_join_rb(port, nonce, 0, 1, NULL, NULL, &r0);
    if (fd0 < 0)
    {
        result("far.second_carrier", 0, "the first carrier did not join");
        return failures;
    }
    {
        struct fr o = { .type = TBT_F_OPEN, .path = 0, .sid = sid, .conv = TBT_CONV };

        send_frame(fd0, &o, NULL, 0);
    }
    msleep(300);
    send_data(fd0, 0, sid, 0, 0, "alpha", 5);

    /* The first 5 bytes must come back before the second carrier is used, so
     * the stream's reverse offset is known. */
    end = now_ms() + 3000;
    while (got < 5 && now_ms() < end)
    {
        struct pollfd p = { .fd = fd0, .events = POLLIN };

        if (poll(&p, 1, 200) <= 0)
            continue;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if (rb_frame(&r0, &f, body, sizeof(body), 200))
            break;
        if (f.type == TBT_F_DATA && f.sid == sid && f.blen)
        {
            if (acc_feed(echo, sizeof(echo), &got, body, f.blen, f.seq) < 0)
            {
                bad = 1;
                break;
            }
            send_pad(fd0, 0, sid, sidhi, got);
        }
    }

    /* A new TCP connection on a free index, same nonce.  The far must answer
     * the HELLO and then process records from it. */
    fd1 = far_conn(port);
    if (fd1 >= 0)
    {
        memset(&r1, 0, sizeof(r1));
        r1.fd = fd1;
        send_hello(fd1, nonce, 1, 1, TBT_ROLE_NEAR);
        hello1 = read_hello(&r1, &f, body, sizeof(body), NULL, NULL) == 0;
        if (hello1)
        {
            pad_ok = send_pad(fd1, 1, sid, sidhi, got) == 0;
            send_data(fd1, 1, sid, sidhi, 5, "beta!", 5);
        }
    }

    end = now_ms() + 5000;
    while (hello1 && got < want_len && now_ms() < end)
    {
        struct pollfd p[2];
        int pr, k;

        p[0].fd = fd0;
        p[0].events = POLLIN;
        p[0].revents = 0;
        p[1].fd = fd1;
        p[1].events = POLLIN;
        p[1].revents = 0;
        pr = poll(p, 2, 200);
        if (pr <= 0)
            continue;
        for (k = 0; k < 2; k++)
        {
            struct rb *rr = k ? &r1 : &r0;
            int rc;

            if (!(p[k].revents & POLLIN))
                continue;
            rc = rb_frame(rr, &f, body, sizeof(body), 200);
            if (rc)
                continue;
            if (f.type == TBT_F_DATA && f.sid == sid && f.blen)
            {
                if (acc_feed(echo, sizeof(echo), &got, body, f.blen, f.seq) < 0)
                {
                    bad = 1;
                    break;
                }
                send_pad(k ? fd1 : fd0, k, sid, sidhi, got);
            }
        }
        if (bad)
            break;
    }
    if (got == want_len && memcmp(echo, want, want_len))
        bad = 1;
    result("far.second_carrier",
           hello1 && pad_ok && got == want_len && !bad &&
               klog_check("far.second_carrier") == 0,
           "second carrier after the first: HELLO answered=%d, PAD sent=%d, "
           "echo %zu/%zu corrupt=%d, rx_adopt_events=%llu join_dup=%llu "
           "(a far that never reads the new carrier leaves it unanswered)",
           hello1, pad_ok, got, want_len, bad,
           stat_key(tunnel, "rx_adopt_events"), stat_key(tunnel, "join_dup"));
    if (fd1 >= 0)
        close(fd1);
    close(fd0);
    return failures;
}

/* ------------------------------------------------------------------ */
/* Attack 1 (near side): a fake far listener attacks the response      */
/* parser, feedback handling, and delivery.                            */

static int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET };
    int one = 1;

    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    /* 100 concurrent public streams become 100 near-side target dials at
     * once; a backlog of 8 drops SYNs and turns the tunnel into the suspect. */
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 256))
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int accept_to(int ls, int ms)
{
    struct pollfd p = { .fd = ls, .events = POLLIN };
    int r = poll(&p, 1, ms);

    if (r <= 0)
        return -1;
    return accept(ls, NULL, NULL);
}

static void app_child(int pubport, int want_data)
{
    int fd, i;
    char b[64];
    size_t got = 0;

    for (i = 3; i < 64; i++)
        close(i);
    fd = connect_to("127.0.0.1", pubport, NULL);
    if (fd < 0)
        _exit(2);
    if (!want_data)
    {
        /* Sequential reuse: the tunnel must accept a new application after
         * the previous one closed.  Exit 0 when it was accepted and echoed,
         * 1 when the listener refused it. */
        send_all(fd, "x", 1);
        ssize_t n = read_to(fd, b, 1, 2000);

        _exit(n == 1 ? 0 : 1);
    }
    while (got < 16)
    {
        ssize_t n = read_to(fd, b + got, 16 - got, 5000);

        if (n <= 0)
            _exit(3);
        got += (size_t)n;
    }
    _exit(memcmp(b, "FAKE-FAR-DATA!!!", 16) ? 4 : 0);
}

static int wait_child(pid_t pid)
{
    int st = 0;

    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int fakefar_suite(int port, int pubport, int tunnel)
{
    int ls = listen_on(port);
    int scen, failures0 = failures;
    unsigned char nonce[16];
    int near_gen = 0, near_idx = 0, have_hello = 0;
    static struct rb r;
    struct fr f;
    unsigned char body[8192];

    if (ls < 0)
    {
        result("near.listener", 0, "cannot bind the fake far port %d", port);
        return failures;
    }
    klog_start();

    for (scen = 0; scen < 9; scen++)
    {
        int fd = accept_to(ls, 8000);
        int bad = 0;

        if (fd < 0)
        {
            result("near.scenario", 0, "the near side never dialled scenario %d", scen);
            break;
        }
        memset(&r, 0, sizeof(r));
        r.fd = fd;
        have_hello = 0;

        if (scen < 4)
        {
            /* The response header is parsed before any body; a bad header
             * poisons the carrier without needing the near's HELLO. */
            if (rb_header(&r, 3000))
            {
                result("near.scenario", 0, "no request header on scenario %d", scen);
                close(fd);
                continue;
            }
            send_response_header(fd, scen);
            bad = expect_close(fd, 3000);
            if (scen == 0)
                result("near.resp_not_chunked", bad,
                       "200 with Content-Length instead of chunked: carrier poisoned");
            else if (scen == 1)
                result("near.resp_500", bad, "500 response: carrier poisoned");
            else if (scen == 2)
                result("near.resp_hdr_big", bad,
                       "response header > carrier buffer: carrier poisoned");
            else
                result("near.resp_close", bad,
                       "Connection: close response: carrier poisoned");
            close(fd);
            klog_check("near.resp");
            msleep(600);
            continue;
        }

        /* From here we need the near's own HELLO: it carries the nonce, the
         * carrier index, and the generation. */
        if (rb_header(&r, 3000))
        {
            result("near.scenario", 0, "no request header on scenario %d", scen);
            close(fd);
            continue;
        }
        {
            int tries;

            for (tries = 0; tries < 8; tries++)
            {
                if (rb_frame(&r, &f, body, sizeof(body), 3000))
                    break;
                if (f.type == TBT_F_HELLO)
                {
                    memcpy(nonce, body + 2, 16);
                    near_gen = (body[22] >> 2) & 0xf;
                    near_idx = f.path;
                    have_hello = 1;
                    break;
                }
            }
        }
        if (!have_hello)
        {
            result("near.scenario", 0, "no HELLO from the near side on scenario %d", scen);
            close(fd);
            continue;
        }

        if (scen == 4)
        {
            unsigned char wrong[16];

            memcpy(wrong, nonce, 16);
            wrong[0] ^= 1;
            send_response_header(fd, 4);
            send_hello(fd, wrong, near_idx, (unsigned char)(1 | near_gen << 2), TBT_ROLE_FAR);
            result("near.hello_wrong_nonce", expect_close(fd, 3000),
                   "response HELLO with a wrong nonce: carrier poisoned");
            close(fd);
        }
        else if (scen == 5)
        {
            send_response_header(fd, 4);
            send_hello(fd, nonce, near_idx, (unsigned char)(1 | near_gen << 2), TBT_ROLE_FAR);
            send_chunk_sizeline(fd, "ffffffff", NULL, 0);
            result("near.chunk_huge", expect_close(fd, 3000),
                   "chunk size 0xFFFFFFFF in a response: carrier poisoned");
            close(fd);
        }
        else if (scen == 6)
        {
            unsigned char rec[32];
            struct fr h = { .type = TBT_F_DATA, .sid = 0x1111, .conv = TBT_CONV };

            fr_encode(rec, &h);
            p16(rec + 6, 100);
            send_response_header(fd, 4);
            send_hello(fd, nonce, near_idx, (unsigned char)(1 | near_gen << 2), TBT_ROLE_FAR);
            send_chunk(fd, rec, 32);
            result("near.split_record", expect_close(fd, 3000),
                   "chunk 32 with body_len 100 in a response: carrier poisoned");
            close(fd);
        }
        else if (scen == 7)
        {
            send_response_header(fd, 4);
            send_hello(fd, nonce, near_idx, (unsigned char)(1 | near_gen << 2), TBT_ROLE_FAR);
            send_chunk_sizeline(fd, "2025", NULL, 0);
            result("near.chunk_oversize", expect_close(fd, 3000),
                   "chunk 8229 in a response: carrier poisoned");
            close(fd);
        }
        else
        {
            /* Scenario 8: a good carrier. Drip and PAD are legal, forged FBK
             * must be ignored, and real DATA must reach the application. */
            unsigned char fb[18];
            struct fr h = { .type = TBT_F_FBK, .ackc = 1, .conv = TBT_CONV };
            unsigned long long before, after;
            uint32_t sid = 0;
            uint8_t sidhi = 0;
            int have_open = 0, i;
            pid_t child;

            send_response_header(fd, 4);
            send_hello(fd, nonce, near_idx, (unsigned char)(1 | near_gen << 2), TBT_ROLE_FAR);
            send_chunk(fd, "d", 1); /* drip */
            {
                unsigned char pad[32];
                struct fr p = { .type = TBT_F_PAD, .conv = TBT_CONV };

                fr_encode(pad, &p);
                send_chunk(fd, pad, 32);
            }
            msleep(400);
            {
                char c;

                if (recv(fd, &c, 1, MSG_DONTWAIT) == 0)
                {
                    result("near.drip_pad", 0, "drip + PAD poisoned the carrier");
                    close(fd);
                    continue;
                }
            }
            result("near.drip_pad", 1, "a drip chunk and a PAD chunk between records accepted");

            memset(fb, 0, sizeof(fb));
            fb[0] = (unsigned char)((near_gen & 0xf) << 4);
            p64(fb + 2, 5000);
            send_frame(fd, &h, fb, sizeof(fb));
            msleep(200);
            before = peer_uack(tunnel, near_idx);
            fb[0] = (unsigned char)((((near_gen + 1) & 0xf) << 4) | (near_idx & 0xf));
            p64(fb + 2, 999999);
            p32(fb + 10, 0xffffffffu);
            send_frame(fd, &h, fb, sizeof(fb));
            fb[0] = (unsigned char)((near_gen & 0xf) << 4 | 15);
            send_frame(fd, &h, fb, sizeof(fb));
            msleep(300);
            after = peer_uack(tunnel, near_idx);
            result("near.fbk_forged", before == 5000 && after == 5000,
                   "forged FBK gen+1 and path 15: uack %llu -> %llu (must stay 5000)",
                   before, after);

            child = fork();
            if (child == 0)
                app_child(pubport, 1);
            for (i = 0; i < 40 && !have_open; i++)
            {
                if (rb_frame(&r, &f, body, sizeof(body), 1000))
                    break;
                if (f.type == TBT_F_OPEN)
                {
                    sid = f.sid;
                    sidhi = f.sidhi;
                    have_open = 1;
                }
            }
            if (have_open)
            {
                struct fr d = { .type = TBT_F_DATA, .sid = sid, .sidhi = sidhi,
                                .conv = TBT_CONV };
                int rc;

                send_frame(fd, &d, "FAKE-FAR-DATA!!!", 16);
                rc = wait_child(child);
                result("near.data_path", rc == 0,
                       "DATA delivered to the app after forged FBK (child rc=%d)", rc);
            }
            else
            {
                kill(child, SIGKILL);
                wait_child(child);
                result("near.data_path", 0, "the near side sent no OPEN for the app");
            }
            klog_check("near.data_path");

            /* The first app is gone; the session must accept a second one. */
            child = fork();
            if (child == 0)
                app_child(pubport, 0);
            {
                int echoed = 0;
                int rc;

                /* The fake far never echoed the second app's byte, so the
                 * old test could not pass even when the connection was
                 * accepted.  Read the new stream's DATA and echo it back;
                 * that is what "usable" means here. */
                for (i = 0; i < 20 && !echoed; i++)
                {
                    if (rb_frame(&r, &f, body, sizeof(body), 100))
                        break;
                    if (f.type == TBT_F_DATA)
                    {
                        send_frame(fd, &f, body, f.blen);
                        echoed = 1;
                    }
                }
                rc = wait_child(child);
                if (!echoed && rc == 0)
                    rc = 1;
                result("near.second_conn", rc == 0,
                       "second public connection after the first closed: rc=%d "
                       "echoed=%d (0 = accepted and usable, 1 = refused)",
                       rc, echoed);
            }
            close(fd);
            klog_check("near.second_conn");
        }
        klog_check("near.scenario");
        msleep(600);
    }
    close(ls);
    return failures - failures0;
}

/* ------------------------------------------------------------------ */
/* review 22: a pinned FIN or RST freed on -EAGAIN.                    */

/* Write until the application's own socket pushes back, or `want` bytes were
 * accepted.  The near side's accepted socket buffers 1 MiB and this child's
 * send buffer another 2 MiB, so the loop reaches EAGAIN well after the
 * carrier's 2 MiB send queue is full. */
static int write_until_eagain(int fd, size_t want)
{
    unsigned char buf[65536];
    size_t sent = 0;

    memset(buf, 'F', sizeof(buf));
    while (sent < want)
    {
        size_t n = want - sent < sizeof(buf) ? want - sent : sizeof(buf);
        ssize_t r = send(fd, buf, n, MSG_NOSIGNAL);

        if (r > 0)
        {
            sent += (size_t)r;
            continue;
        }
        break; /* EAGAIN or a dead socket: the bytes already accepted count */
    }
    return (int)sent;
}

/* read_to() is recv(2), so it cannot read the status pipes. */
static int pipe_read(int fd, void *buf, size_t len, int ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    int r = poll(&p, 1, ms);

    if (r == 0)
        return -2;
    if (r < 0)
        return -1;
    return (int)read(fd, buf, len);
}

/* The abusive application.  `rst` selects the close shape: a clean half-close
 * (FIN) or SO_LINGER 0 (RST).  The child reports through pipes, never through
 * the console, so its exit cannot interleave with the parent's verdict. */
/* The carrier's send queue is 2 MiB: tbt_car_setsockbuf() asks for 1 MiB and
 * the kernel doubles it, and the raw record header adds 32 bytes per 8 KiB.
 * A 512 KiB write could never fill that queue, so hiwat stayed flat and the
 * verdict measured the application, not review 22.  The tunnel line under
 * test sets tx_window_bytes=8388608, so the near admits the whole 4 MiB write
 * (the stream's reorder window no longer caps it) and the pump must meet
 * -EAGAIN once the 2 MiB queue is full.  The RST variant waits for the pump's
 * hiwat proof before it closes, because a fixed ramp raced the fill and could
 * kill the stream while the carrier was still below its line. */
#define BP_BYTES (4 * 1024 * 1024)

static void app_backpressure_child(int pubport, int rst, int sigfd, int donefd)
{
    int fd = connect_to("127.0.0.1", pubport, NULL);
    char b[4096];
    int sb = 2 * 1024 * 1024;

    if (fd < 0)
        _exit(2);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof(sb));
    set_nonblock(fd);
    write_until_eagain(fd, BP_BYTES);
    if (rst)
    {
        char c;

        /* The parent fills the carrier first; closing with SO_LINGER 0 then
         * makes the near emit RST, not FIN. */
        if (pipe_read(sigfd, &c, 1, 8000) != 1)
            _exit(4);
        close_rst(fd);
        _exit(0);
    }
    shutdown(fd, SHUT_WR);
    {
        char c = 'S';

        if (write(donefd, &c, 1) != 1)
            _exit(4);
    }
    for (;;)
    {
        ssize_t n = read_to(fd, b, sizeof(b), 6000);

        if (n == 0)
            _exit(0); /* EOF: the peer's FIN crossed the tunnel */
        if (n == -2)
            _exit(3); /* timeout: no FIN, no reset, just silence */
        if (n < 0)
            _exit(2); /* reset or other socket error */
    }
}

static int wait_child_timeout(pid_t pid, int ms)
{
    long end = now_ms() + ms;
    int st = 0;

    while (now_ms() < end)
    {
        pid_t r = waitpid(pid, &st, WNOHANG);

        if (r == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (r < 0)
            return -1;
        msleep(20);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return 4;
}

/* A SACK carries our delivery state as one byte; state 2 means "my delivery
 * queue is full, do not count my silence as loss".  The fake far is not
 * reading its application side either, so it reports the same state the real
 * far would: the near's stall/retransmit timers are suppressed while a peer
 * is paused, and a truthful ack can be used without racing them. */
static int raw_send_sack(int fd, int idx, uint32_t sid, uint8_t sidhi,
                         uint8_t state)
{
    struct fr s = { .type = TBT_F_SACK,
                    .path = (uint8_t)idx,
                    .sid = sid,
                    .sidhi = sidhi,
                    .conv = TBT_CONV };

    return raw_send_frame(fd, &s, &state, 1);
}

/* The near's `tx` stat is the payload the pump has already handed to the
 * carrier's TCP send queue.  Acking exactly that advances the near's window
 * and retention without ever acking a frame the pump has not written: the old
 * 64 KiB ack ramp ran to 2.6 MiB while the pump had written 475 KiB, so the
 * pump dropped the rest as "already delivered".  The ack is still a test
 * shortcut -- the fake far has not read the bytes -- but it is bounded by the
 * write cursor, which is what keeps the frame stream intact. */
static uint64_t near_tx_ack(int tunnel)
{
    /* " tx=", not "tx": stat_key() matches a substring, and the same stats
     * block later carries "dup_tx=0", which a bare "tx" would match and
     * return as zero. */
    return (uint64_t)stat_key(tunnel, " tx");
}

/* Keep the carrier's read side moving with the bounded ack above. */
static void pad_for(int fd, int idx, uint32_t sid, uint8_t sidhi,
                    int tunnel, long ms)
{
    long end = now_ms() + ms;

    while (now_ms() < end)
    {
        raw_send_pad(fd, idx, sid, sidhi, near_tx_ack(tunnel));
        msleep(30);
    }
}

/* Wait for the pump's -EAGAIN proof.  The RST must be queued while the
 * carrier's send buffer is full; a fixed ramp raced the fill (measured on the
 * same guest: hiwat 0 -> 0 in one run, 0 -> 2732 in the next).  Returns the
 * last value read, so the caller can report it; the common hiwat gate below
 * still runs. */
static unsigned long long pad_until_hiwat(int fd, int idx, uint32_t sid,
                                          uint8_t sidhi, long ms, int tunnel,
                                          unsigned long long hiwat0)
{
    long end = now_ms() + ms;
    unsigned long long hiwat = hiwat0;

    while (now_ms() < end)
    {
        raw_send_pad(fd, idx, sid, sidhi, near_tx_ack(tunnel));
        hiwat = stat_key(tunnel, "hiwat");
        if (hiwat > hiwat0)
            break;
        msleep(30);
    }
    return hiwat;
}

/* Same, but stop as soon as `donefd` reports the application's shutdown. */
static int pad_until_done(int fd, int idx, uint32_t sid, uint8_t sidhi,
                          int tunnel, long ms, int donefd)
{
    long end = now_ms() + ms;
    int done = 0;

    while (now_ms() < end)
    {
        raw_send_pad(fd, idx, sid, sidhi, near_tx_ack(tunnel));
        if (!done && donefd >= 0)
        {
            struct pollfd p = { .fd = donefd, .events = POLLIN };
            char c;

            if (poll(&p, 1, 0) > 0 && (p.revents & POLLIN) &&
                read(donefd, &c, 1) == 1)
                done = 1;
        }
        msleep(30);
    }
    return done;
}

/* Accept one near carrier and answer its HELLO.  The near dials a replacement
 * when it loses a carrier, so mode_fineagain() calls this again for the second
 * connection instead of reading a dead socket.  Returns the connected fd, or
 * -1 with *err naming the failure. */
static int carrier_join(int ls, struct rb *r, struct fr *f, unsigned char *body,
                        size_t cap, int *idx, int *gen, unsigned char *nonce,
                        const char **err)
{
    struct fr h;
    int fd, i, have_hello = 0;

    *err = "the near side never dialled the fake far";
    fd = accept_to(ls, 8000);
    if (fd < 0)
        return -1;
    {
        int rb_sz = 4096;

        /* A tiny receive buffer closes the near's TCP window early, so the
         * 2 MiB carrier send queue fills long before the 4 MiB application
         * write ends. */
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb_sz, sizeof(rb_sz));
    }
    memset(r, 0, sizeof(*r));
    r->fd = fd;
    for (i = 0; i < 8; i++)
    {
        if (raw_read_frame(r, f, body, cap, 3000))
            break;
        if (f->type == TBT_F_HELLO)
        {
            memcpy(nonce, body + 2, 16);
            *gen = (body[22] >> 2) & 0xf;
            *idx = f->path;
            have_hello = 1;
            break;
        }
    }
    if (!have_hello)
    {
        *err = "no HELLO from the near carrier";
        close(fd);
        return -1;
    }
    h = (struct fr){ .type = TBT_F_HELLO, .path = (uint8_t)*idx,
                     .conv = TBT_CONV };
    /* The near tunnel is profile=raw, whose HELLO profile bits are 3. */
    hello_body(body, TBT_ROLE_FAR, nonce, (unsigned char)(3 | *gen << 2));
    raw_send_frame(fd, &h, body, 23);
    return fd;
}

/* The near's carrier send queue is full when the application half-closes (or
 * resets).  A pinned control frame that meets -EAGAIN must be requeued; the
 * baseline frees it.  FIN has a retry from the FBK tick, RST has none, so the
 * pair separates "control frame survives backpressure" from "FIN happens to
 * be retried". */
static int mode_fineagain(int port, int pubport, int tunnel, int rst)
{
    const char *name = rst ? "near.rst_eagain" : "near.fin_eagain";
    int ls = listen_on(port);
    int fd, sig[2], done[2], have_open = 0, i, gen = 0, idx = 0, seen = 0;
    int redialed = 0;
    unsigned char nonce[16], body[8192];
    struct fr f;
    static struct rb r;
    uint32_t sid = 0;
    uint8_t sidhi = 0;
    unsigned long long hiwat0, hiwat1, hu0, hu1;
    int parsed = 0;
    uint8_t last_type = 0;
    int rb_set = -1, rb_now = 0;
    int brk = 0, rfail = 0;
    long end, next_ack;
    pid_t pid;
    int rc = -1;

    if (ls < 0)
    {
        result(name, 0, "cannot bind the fake far port %d", port);
        return failures;
    }
    klog_start();
    hiwat0 = stat_key(tunnel, "hiwat");
    hu0 = stat_key(tunnel, "holes_unrecoverable");
    {
        const char *err;

        fd = carrier_join(ls, &r, &f, body, sizeof(body), &idx, &gen, nonce,
                          &err);
        if (fd < 0)
        {
            result(name, 0, "%s", err);
            close(ls);
            return failures;
        }
    }

    /* The OPEN exists only once the application connects, so start the app
     * first and then wait for the near to announce the stream. */
    if (pipe(sig) || pipe(done))
    {
        result(name, 0, "pipe() failed");
        close(fd);
        close(ls);
        return failures;
    }
    pid = fork();
    if (pid == 0)
    {
        close(sig[1]);
        close(done[0]);
        close(ls);
        app_backpressure_child(pubport, rst, sig[0], done[1]);
        _exit(2);
    }
    close(sig[0]);
    close(done[1]);

    for (i = 0; i < 40 && !have_open; i++)
    {
        if (raw_read_frame(&r, &f, body, sizeof(body), 1000))
            break;
        if (f.type == TBT_F_OPEN)
        {
            sid = f.sid;
            sidhi = f.sidhi;
            have_open = 1;
        }
    }
    if (!have_open)
    {
        kill(pid, SIGKILL);
        wait_child(pid);
        close(sig[1]);
        close(done[0]);
        result(name, 0, "the near side sent no OPEN for the application");
        close(fd);
        close(ls);
        return failures;
    }

    /* The fake far is not reading its application side yet: tell the near the
     * state the real far reports when its delivery queue is full, so the
     * stall/retransmit timers do not count this window as loss.  The acks
     * below stay truthful (0 until DATA is parsed). */
    raw_send_sack(fd, idx, sid, sidhi, 2);

    if (!rst)
    {
        int signalled = pad_until_done(fd, idx, sid, sidhi, tunnel, 1200, done[0]);

        if (!signalled)
        {
            char c;

            /* The app wrote its bytes and shut the write side down. */
            if (pipe_read(done[0], &c, 1, 3000) != 1)
            {
                kill(pid, SIGKILL);
                wait_child(pid);
                result(name, 0, "the application never reported its shutdown");
                close(fd);
                close(ls);
                return failures;
            }
            pad_for(fd, idx, sid, sidhi, tunnel, 600);
        }
    }
    else
    {
        /* Fill the carrier before the reset, then let the app close.  The
         * reset must be queued while the carrier's send buffer is full, so
         * wait for the pump's -EAGAIN proof instead of racing a fixed ramp;
         * keep acking while waiting so the capture can finish.  The common
         * gate below still requires hiwat to have grown. */
        pad_until_hiwat(fd, idx, sid, sidhi, 3000, tunnel, hiwat0);
        /* Widen the far's receive buffer now, not after the close: the
         * carrier's TCP has been in zero-window for seconds, and the 256 KiB
         * request (512 KiB after the kernel's doubling) lets the multi-MiB
         * backlog drain before the reset record has to wait behind the
         * slow-start/persist recovery.  The backpressure proof is already
         * taken; this does not undo it. */
        {
            int rb = 256 * 1024;
            socklen_t rbl = sizeof(rb_now);

            rb_set = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
            getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb_now, &rbl);
        }
        if (write(sig[1], "G", 1) != 1)
        {
            kill(pid, SIGKILL);
            wait_child(pid);
        }
        msleep(300);
    }
    close(sig[1]);
    close(done[0]);
    /* The verdict needs proof that the pinned frame met real backpressure:
     * wait for the pump's -EAGAIN counter to move before draining. */
    {
        long endw = now_ms() + 3000;

        while (now_ms() < endw && stat_key(tunnel, "hiwat") <= hiwat0)
            msleep(20);
    }
    hiwat1 = stat_key(tunnel, "hiwat");

    /* The 4 KiB window did its job: the carrier filled and hiwat grew.  Widen
     * it now so the multi-MiB backlog and the control record drain well inside
     * the 60 s budget instead of at the tiny-window rate.  The backpressure
     * verdict was already taken above; this only shortens the observation
     * window. */
    {
        int rb = 256 * 1024;
        socklen_t rbl = sizeof(rb_now);

        rb_set = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
        getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb_now, &rbl);
    }

    /* Drain the carrier.  This is the half the earlier construction missed:
     * a fake far that never reads keeps the send queue full forever, so the
     * capture can never finish and the FIN is never emitted.  Read as fast as
     * the socket delivers, ack the near's write cursor (its tx stat), and give
     * the pump time to write the queued records and the control frame.  A
     * pinned FIN/RST freed on -EAGAIN never reappears.  Acking ahead of the
     * read (the old UINT64_MAX) made the pump drop frames it had not written
     * yet, so the carrier never reached its memory line during the fill.
     *
     * The 4 KiB receive buffer (8 KiB after the kernel's doubling) delivers
     * at roughly 35 KiB/s: one window per delayed-ACK / persist cycle
     * (measured on this kernel: 8 KiB every ~208 ms).  A 4 MiB backlog would
     * need minutes at that rate, so the buffer is widened above before this
     * drain; the 60 s budget covers the widened-window drain of the multi-MiB
     * backlog with margin.  The verdict still requires the FIN/RST record
     * itself, so a drain that merely runs out the clock cannot pass. */
    end = now_ms() + 60000;
    next_ack = 0;
redial:
    while (now_ms() < end && !seen)
    {
        struct pollfd p = { .fd = fd, .events = POLLIN | POLLRDHUP };
        int pr;

        if (!rst && now_ms() >= next_ack)
        {
            raw_send_pad(fd, idx, sid, sidhi, near_tx_ack(tunnel));
            next_ack = now_ms() + 50;
        }
        pr = poll(&p, 1, 20);
        if (pr <= 0)
            continue;
        if (p.revents & (POLLERR | POLLNVAL))
        {
            brk = p.revents;
            break;
        }
        if (p.revents & POLLIN)
        {
            if (raw_read_frame(&r, &f, body, sizeof(body), 50))
            {
                /* Readable but no complete frame: a peer FIN (POLLRDHUP)
                 * is a broken carrier, not a slow sender. */
                if (p.revents & POLLRDHUP)
                {
                    brk = p.revents;
                    break;
                }
                rfail++;
                continue;
            }
        }
        else if (p.revents & (POLLHUP | POLLRDHUP))
        {
            brk = p.revents;
            break;
        }
        else
        {
            continue;
        }
        parsed++;
        last_type = f.type;
        if (f.sid == sid && f.type == (rst ? TBT_F_RST : TBT_F_FIN))
            seen = 1;
    }
    /* The near re-dials a carrier it lost and re-pins the RST to the
     * replacement.  Accept that one connection so the frame can still be
     * read; the stream's sid does not change.  The 60 s budget is shared, so
     * the redial cannot stretch the test past the runner's timeout. */
    if (!seen && rst && brk && !redialed)
    {
        const char *err;
        int nfd = carrier_join(ls, &r, &f, body, sizeof(body), &idx, &gen,
                               nonce, &err);

        if (nfd >= 0)
        {
            int rb = 256 * 1024;

            /* The fill proof was taken on the first carrier; a tiny window on
             * the replacement would park the RST behind the retransmitted
             * DATA at the persist-cycle rate. */
            setsockopt(nfd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
            close(fd);
            fd = nfd;
            redialed = 1;
            goto redial;
        }
    }
    if (seen && !rst)
    {
        /* The far's FIN for this stream.  seq 0: no DATA ever went toward the
         * application.  It turns the app's read into EOF once its bytes are
         * all delivered.  Send it more than once: the near's FIN handling is
         * idempotent (rx_fin_seen is set once), and a duplicate costs 32
         * bytes.  With the 4 KiB window in place (before the widening above)
         * a single record after the long PAD stream was measured not to reach
         * the near's parser at all. */
        int i;

        for (i = 0; i < 3; i++)
        {
            raw_send_fin(fd, idx, sid, sidhi, 0);
            msleep(100);
        }
        rc = wait_child_timeout(pid, 6000);
    }
    else if (rst)
    {
        rc = wait_child_timeout(pid, 1000);
    }
    else
    {
        kill(pid, SIGKILL);
        wait_child(pid);
    }
    hu1 = stat_key(tunnel, "holes_unrecoverable");
    result(name,
           rst ? (seen && hiwat1 > hiwat0 && klog_check(name) == 0)
               : (seen && rc == 0 && hiwat1 > hiwat0 && klog_check(name) == 0),
           "%s under carrier backpressure: %s=%d eof=%d child_rc=%d "
           "pump_hiwat %llu -> %llu (must grow) holes_unrecoverable %llu -> %llu; "
           "tx_ack=%llu txq=%llu rx_records=%llu hole_checks=%llu time_fail=%llu "
           "parsed=%d last_type=%u wire=%llu car_wmem=%llu rb_set=%d rb_now=%d "
           "brk=%d rfail=%d redialed=%d",
           rst ? "RST" : "FIN", rst ? "rst_seen" : "fin_seen", seen,
           !rst && rc == 0, rc, hiwat0, hiwat1, hu0, hu1,
           stat_key(tunnel, "tx_ack"), stat_key(tunnel, "txq"),
           stat_key(tunnel, "rx_records"), stat_key(tunnel, "hole_checks"),
           stat_key(tunnel, "time_fail"), parsed, last_type,
           stat_key(tunnel, "wire"), stat_key(tunnel, "car_wmem"),
           rb_set, rb_now, brk, rfail, redialed);
    close(fd);
    close(ls);
    return failures;
}

/* ------------------------------------------------------------------ */
/* Attack 2: public port abuse                                         */

enum churn_rc
{
    CHURN_USABLE = 0,
    CHURN_REFUSED,
    CHURN_TIMEOUT,
    CHURN_CONNECT,
    CHURN_SEND
};

/* One churn cycle: connect, send one byte, wait for the echo.  A read
 * timeout is kept apart from a refusal because a busy host can delay the
 * echo of a session the module still holds; the caller retries that cycle
 * once.  A failed connect or send is never retried: it is a real refusal. */
static int churn_cycle(int port, int ms)
{
    int fd = connect_to("127.0.0.1", port, NULL);
    char c;
    ssize_t r;

    if (fd < 0)
        return CHURN_CONNECT;
    if (send_all(fd, "x", 1))
    {
        close(fd);
        return CHURN_SEND;
    }
    r = read_to(fd, &c, 1, ms);
    close(fd);
    if (r == 1)
        return CHURN_USABLE;
    if (r == -2)
        return CHURN_TIMEOUT;
    return CHURN_REFUSED;
}

static int mode_churn(int port, int n)
{
    int live = 0, refused = 0, other = 0, retried = 0, i;

    for (i = 0; i < n; i++)
    {
        int r = churn_cycle(port, i == 0 ? 3000 : 2000);

        if (r == CHURN_TIMEOUT)
        {
            /* The retry is still the same one of the n cycles: the 10k total
             * and the "every connection usable" contract do not change.
             * Only scheduler jitter is absorbed; a refused or unreachable
             * connection fails as before. */
            retried++;
            r = churn_cycle(port, 2000);
        }
        switch (r)
        {
        case CHURN_USABLE:
            live++;
            break;
        case CHURN_REFUSED:
            refused++;
            break;
        default:
            other++;
            break;
        }
    }
    result("pub.churn", live == n && refused == 0 && other == 0,
           "%d connect/send/close cycles: usable=%d refused=%d other=%d "
           "retried=%d (a closed stream must free the session for the next "
           "connection)", n, live, refused, other, retried);
    return 0;
}

static int mode_two(int port)
{
    int a = connect_to("127.0.0.1", port, NULL);
    int b, ok_a = 0, ok_b = 0, ok_a2 = 0;
    char buf[8];

    if (a < 0)
    {
        result("pub.two", 0, "cannot connect");
        return 1;
    }
    send_all(a, "AAAA", 4);
    ok_a = read_to(a, buf, 4, 3000) == 4 && !memcmp(buf, "AAAA", 4);

    b = connect_to("127.0.0.1", port, NULL);
    if (b >= 0)
    {
        send_all(b, "BBBB", 4);
        ok_b = read_to(b, buf, 4, 3000) == 4 && !memcmp(buf, "BBBB", 4);
        close(b);
    }
    send_all(a, "CCCC", 4);
    ok_a2 = read_to(a, buf, 4, 3000) == 4 && !memcmp(buf, "CCCC", 4);
    result("pub.two", ok_a && ok_b && ok_a2,
           "A usable=%d, B usable=%d, A after B close=%d", ok_a, ok_b, ok_a2);
    close(a);
    return 0;
}

/* P3b3: 100 concurrent public streams.  Every connection is established
 * before any payload, so the module must hold all of them at once.  Each
 * slot's pattern carries its slot number, so a crossed byte shows up at its
 * exact offset.  Closing one stream must not disturb another, and after all
 * of them close the tunnel must still accept a fresh connection. */
#define CONC_MAX 128 /* == TBT_STREAMS_MAX */

enum conc_stage
{
    CONC_OK = 0,
    CONC_CONNECT,
    CONC_SEND,
    CONC_READ,
    CONC_SHORT,
    CONC_CORRUPT,
    CONC_ERR,
};

struct conc_slot
{
    int fd;
    unsigned char *want, *got;
    size_t got_n;
    int stage;
    int err;
    ssize_t diff;
};

static ssize_t first_diff(const unsigned char *a, const unsigned char *b, size_t n)
{
    size_t k;

    for (k = 0; k < n; k++)
        if (a[k] != b[k])
            return (ssize_t)k;
    return -1;
}

static int mode_concurrent(int port, int n, int bytes, int timeout_s, int tunnel)
{
    struct conc_slot slot[CONC_MAX];
    struct pollfd pfd[CONC_MAX];
    unsigned char tag[64];
    long t0 = now_ms(), deadline;
    long active = -1, total = -1;
    int i, k, connected = 0, sent = 0, echoed = 0;
    int short_n = 0, corrupt = 0, timeout_n = 0;
    int isolation = 0, recover = 0, first_bad = -1;
    ssize_t first_off = -1;

    if (n < 1)
        n = 1;
    if (n > CONC_MAX)
        n = CONC_MAX;
    if (bytes < 4)
        bytes = 4; /* p32() names the slot */
    if (bytes > 65536)
        bytes = 65536;
    if (timeout_s < 1)
        timeout_s = 1;

    memset(slot, 0, sizeof(slot));
    for (i = 0; i < n; i++)
    {
        slot[i].fd = -1;
        slot[i].stage = CONC_CONNECT;
        slot[i].diff = -1;
        slot[i].want = malloc((size_t)bytes);
        slot[i].got = malloc((size_t)bytes);
        if (!slot[i].want || !slot[i].got)
        {
            for (k = 0; k < n; k++)
            {
                free(slot[k].want);
                free(slot[k].got);
            }
            result("pub.concurrent", 0, "n=%d malloc %d bytes failed", n, bytes);
            return 1;
        }
        for (k = 0; k < bytes; k++)
            slot[i].want[k] = (unsigned char)(i * 31 + k * 7);
        p32(slot[i].want, (uint32_t)i); /* the first 4 bytes name the slot */
        pfd[i].fd = -1;
        pfd[i].events = POLLIN;
        pfd[i].revents = 0;
    }

    /* 1. Establish every stream before any payload. */
    for (i = 0; i < n; i++)
    {
        slot[i].fd = connect_to("127.0.0.1", port, NULL);
        if (slot[i].fd < 0)
        {
            slot[i].err = errno;
            continue;
        }
        connected++;
    }

    /* 2. Send every payload before reading any echo. */
    for (i = 0; i < n; i++)
    {
        if (slot[i].fd < 0)
            continue;
        if (send_all(slot[i].fd, slot[i].want, (size_t)bytes))
        {
            slot[i].stage = CONC_SEND;
            slot[i].err = errno;
            continue;
        }
        sent++;
        pfd[i].fd = slot[i].fd;
    }

    /* 3. Read every echo under one global deadline. */
    deadline = now_ms() + (long)timeout_s * 1000;
    for (;;)
    {
        int live = 0, r;

        for (i = 0; i < n; i++)
            if (pfd[i].fd >= 0 && slot[i].got_n < (size_t)bytes)
                live++;
        if (!live || now_ms() >= deadline)
            break;
        r = poll(pfd, (nfds_t)n, 200);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        for (i = 0; i < n; i++)
        {
            ssize_t got;

            if (pfd[i].fd < 0 || !(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            got = recv(pfd[i].fd, slot[i].got + slot[i].got_n,
                       (size_t)bytes - slot[i].got_n, MSG_DONTWAIT);
            if (got > 0)
            {
                slot[i].got_n += (size_t)got;
                if (slot[i].got_n == (size_t)bytes)
                {
                    pfd[i].fd = -1;
                    if (!memcmp(slot[i].got, slot[i].want, (size_t)bytes))
                    {
                        slot[i].stage = CONC_OK;
                        echoed++;
                    }
                    else
                    {
                        slot[i].stage = CONC_CORRUPT;
                        slot[i].diff = first_diff(slot[i].got, slot[i].want,
                                                  (size_t)bytes);
                        corrupt++;
                    }
                }
            }
            else if (got == 0)
            {
                pfd[i].fd = -1;
                slot[i].stage = CONC_SHORT;
                short_n++;
            }
            else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                /* POLLHUP with no readable byte is a closed stream. */
                if (pfd[i].revents & POLLHUP)
                {
                    pfd[i].fd = -1;
                    slot[i].stage = CONC_SHORT;
                    short_n++;
                }
            }
            else
            {
                pfd[i].fd = -1;
                slot[i].stage = CONC_ERR;
                slot[i].err = errno;
                short_n++;
            }
        }
    }
    for (i = 0; i < n; i++)
        if (pfd[i].fd >= 0 && slot[i].got_n < (size_t)bytes)
        {
            slot[i].stage = CONC_READ; /* deadline hit */
            pfd[i].fd = -1;
            timeout_n++;
        }

    /* 4. Isolation: closing one stream must not disturb another. */
    {
        int victim = -1, witness = -1;

        for (i = 0; i < n; i++)
            if (slot[i].stage == CONC_OK)
            {
                if (victim < 0)
                    victim = i;
                else
                {
                    witness = i;
                    break;
                }
            }
        if (victim >= 0 && witness >= 0)
        {
            unsigned char got[64];

            close(slot[victim].fd);
            slot[victim].fd = -1;
            for (k = 0; k < (int)sizeof(tag); k++)
                tag[k] = (unsigned char)(0xa5 ^ (witness + k));
            isolation = send_all(slot[witness].fd, tag, sizeof(tag)) == 0 &&
                        recv_exact(slot[witness].fd, got, sizeof(got), 5000) == 0 &&
                        !memcmp(got, tag, sizeof(tag));
        }
    }

    /* 5. Close the rest and give the session time to release them.
     * tbt_procfs.c exports no streams_active today: stat_key_opt() returns
     * -1 and the 8 s wait still lets the 2 s tombstones expire before the
     * recovery probe.  When the field appears, this becomes a hard check. */
    for (i = 0; i < n; i++)
        if (slot[i].fd >= 0)
        {
            close(slot[i].fd);
            slot[i].fd = -1;
        }
    for (i = 0; i < 80; i++)
    {
        active = stat_key_opt(tunnel, "streams_active", -1);
        if (active == 0)
            break;
        if (active < 0)
            total = stat_streams_total();
        msleep(100);
    }
    if (total < 0)
        total = stat_streams_total();

    /* 6. The tunnel must still accept a fresh public connection. */
    {
        int fd = connect_to("127.0.0.1", port, NULL);
        unsigned char got[64];

        for (k = 0; k < (int)sizeof(tag); k++)
            tag[k] = (unsigned char)(0x5a ^ k);
        if (fd >= 0)
        {
            recover = send_all(fd, tag, sizeof(tag)) == 0 &&
                      recv_exact(fd, got, sizeof(got), 5000) == 0 &&
                      !memcmp(got, tag, sizeof(tag));
            close(fd);
        }
    }

    /* The headline failure is the first crossed byte when there is one;
     * otherwise the first slot that did not complete. */
    for (i = 0; i < n; i++)
        if (slot[i].stage == CONC_CORRUPT)
        {
            first_bad = i;
            first_off = slot[i].diff;
            break;
        }
    if (first_bad < 0)
        for (i = 0; i < n; i++)
            if (slot[i].stage != CONC_OK)
            {
                first_bad = i;
                break;
            }
    for (i = 0, k = 0; i < n && k < 8; i++)
        if (slot[i].stage != CONC_OK)
        {
            printf("CONC_FAIL slot=%d stage=%d errno=%d got=%zu diff=%zd\n",
                   i, slot[i].stage, slot[i].err, slot[i].got_n, slot[i].diff);
            k++;
        }
    fflush(stdout);

    if (first_bad < 0)
        result("pub.concurrent",
               connected == n && sent == n && echoed == n && short_n == 0 &&
                   corrupt == 0 && timeout_n == 0 && isolation && recover &&
                   (active < 0 || active == 0),
               "n=%d connected=%d echoed=%d short=%d corrupt=%d timeout=%d "
               "isolation=%d recover=%d first_fail=none streams_active=%ld "
               "streams_total=%ld; %ld ms",
               n, connected, echoed, short_n, corrupt, timeout_n, isolation,
               recover, active, total, now_ms() - t0);
    else
        result("pub.concurrent",
               connected == n && sent == n && echoed == n && short_n == 0 &&
                   corrupt == 0 && timeout_n == 0 && isolation && recover &&
                   (active < 0 || active == 0),
               "n=%d connected=%d echoed=%d short=%d corrupt=%d timeout=%d "
               "isolation=%d recover=%d first_fail=slot:%d stage:%d diff:%zd "
               "streams_active=%ld streams_total=%ld; %ld ms",
               n, connected, echoed, short_n, corrupt, timeout_n, isolation,
               recover, first_bad, slot[first_bad].stage, first_off, active,
               total, now_ms() - t0);

    for (i = 0; i < n; i++)
    {
        free(slot[i].want);
        free(slot[i].got);
    }
    return 0;
}

static int mode_halfclose(int port)
{
    int fd = connect_to("127.0.0.1", port, NULL);
    char buf[64];
    size_t got = 0;
    int eof = 0;

    if (fd < 0)
    {
        result("pub.halfclose", 0, "cannot connect");
        return 1;
    }
    send_all(fd, "half-close", 10);
    shutdown(fd, SHUT_WR);
    for (;;)
    {
        ssize_t n = read_to(fd, buf + got, sizeof(buf) - got, 3000);

        if (n == 0)
        {
            eof = 1;
            break;
        }
        if (n < 0)
            break;
        got += (size_t)n;
        if (got >= sizeof(buf))
            break;
    }
    result("pub.halfclose", eof && got == 10 && !memcmp(buf, "half-close", 10),
           "app half-close: echo %zu bytes, EOF=%d", got, eof);
    close(fd);
    return 0;
}

static int mode_rst(int port, int mib)
{
    int fd = connect_to("127.0.0.1", port, NULL);
    long total = (long)mib * 1024 * 1024, sent = 0;
    char buf[65536];

    if (fd < 0)
    {
        result("pub.rst", 0, "cannot connect");
        return 1;
    }
    memset(buf, 'R', sizeof(buf));
    while (sent < total)
    {
        ssize_t n = send(fd, buf, sizeof(buf), MSG_NOSIGNAL);

        if (n <= 0)
            break;
        sent += n;
    }
    close_rst(fd);
    msleep(500);
    result("pub.rst", klog_check("pub.rst") == 0,
           "RST after %ld bytes: no warning, module still loaded", sent);
    return 0;
}

/* The old `src=` allow list is gone, and its test with it.  The public port
 * has no application-source filter by design: the tunnel's identity is the
 * session nonce (and, on the far side, the peer anchor), not the address the
 * application happens to use.  What must hold is that both keys survive the
 * canonical config, or a save/restore silently drops the pin. */
static int mode_srcfilter(int port, int tunnel)
{
    int fd = connect_to("127.0.0.1", port, "127.0.0.2");
    char buf[8];
    int usable = 0, nonce_rendered, peer_rendered;

    if (fd >= 0)
    {
        send_all(fd, "src", 3);
        usable = read_to(fd, buf, 3, 3000) == 3 && !memcmp(buf, "src", 3);
        close(fd);
    }
    nonce_rendered = cfg_line_has(tunnel, "nonce=00112233445566778899aabbccddeeff");
    peer_rendered = cfg_line_has(tunnel, "peer=127.0.0.1:1");
    result("pub.srcfilter", usable && nonce_rendered && peer_rendered,
           "app from 127.0.0.2 usable=%d (no src= allow list; the tunnel "
           "identity is the nonce), far config renders nonce=%d peer=%d",
           usable, nonce_rendered, peer_rendered);
    return usable && nonce_rendered && peer_rendered ? 0 : 1;
}

static unsigned long mem_available_kb(void)
{
    FILE *p = fopen("/proc/meminfo", "r");
    char line[256];
    unsigned long v = 0;

    if (!p)
        return 0;
    while (fgets(line, sizeof(line), p))
        if (sscanf(line, "MemAvailable: %lu kB", &v) == 1)
            break;
    fclose(p);
    return v;
}

/* A bounded backpressure refusal is a legal writer outcome; an OOM kill is
 * not.  dmesg is the only place the latter appears, so count its lines
 * before and after the transfer and require no new one. */
static int oom_count(void)
{
    FILE *p = popen("dmesg 2>/dev/null", "r");
    char line[512];
    int n = 0;

    if (!p)
        return 0;
    while (fgets(line, sizeof(line), p))
        if (strstr(line, "Out of memory") || strstr(line, "oom-kill") ||
            strstr(line, "Killed process"))
            n++;
    pclose(p);
    return n;
}

static int config_flush(void)
{
    int fd = open("/proc/net/tbtun/config", O_WRONLY);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = write(fd, "flush\n", 6);
    close(fd);
    return n == 6 ? 0 : -1;
}

/* The writer publishes its progress through a shared page: the parent must
 * decide from state, not from a child that may be parked inside write(). */
struct writer_state {
    long sent;
    int err;
};

static int mode_writer(int port, int mib, int do_read)
{
    int fd = connect_to("127.0.0.1", port, NULL);
    long total = (long)mib * 1024 * 1024, sent = 0;
    unsigned long before = mem_available_kb(), during_min = before, after_write;
    unsigned long after_flush = 0;
    struct timeval zero = { 0, 0 };
    struct writer_state *st;
    char *buf = malloc(1024 * 1024);
    int err = 0, oom0, oom1, flushed = 0, legal, mem_ok, rst;
    int blocked = 0, reaped = 0, st_status = 0;
    const char *child_state = "unknown";
    long flush_start, flush_ms = -1, deadline;
    pid_t pid;

    if (fd < 0 || !buf)
    {
        result("pub.writer", 0, "cannot connect or allocate");
        free(buf);
        return 1;
    }
    /* The public socket is ordinary TCP.  connect_to()'s 5 s send timeout is
     * a convenience for the short echo paths; here it would turn the module's
     * backpressure into EAGAIN, which is the opposite of what a blocked
     * application looks like.  Zero disables the timeout for this socket. */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &zero, sizeof(zero));
    st = mmap(NULL, sizeof(*st), PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (st == MAP_FAILED)
    {
        result("pub.writer", 0, "mmap of the writer state failed");
        close(fd);
        free(buf);
        return 1;
    }
    memset(st, 0, sizeof(*st));
    oom0 = oom_count();
    memset(buf, 'W', 1024 * 1024);

    pid = fork();
    if (pid == 0)
    {
        long got = 0;

        while (__atomic_load_n(&st->sent, __ATOMIC_RELAXED) < total)
        {
            ssize_t n = send(fd, buf, 1024 * 1024, MSG_NOSIGNAL);

            if (n <= 0)
            {
                __atomic_store_n(&st->err, errno, __ATOMIC_RELAXED);
                break;
            }
            __atomic_store_n(&st->sent,
                             __atomic_load_n(&st->sent, __ATOMIC_RELAXED) + n,
                             __ATOMIC_RELAXED);
            if (do_read)
            {
                while (got < __atomic_load_n(&st->sent, __ATOMIC_RELAXED))
                {
                    ssize_t r = read_to(fd, buf, 1024 * 1024, 2000);

                    if (r <= 0)
                        break;
                    got += r;
                }
            }
        }
        _exit(0);
    }
    if (pid < 0)
    {
        result("pub.writer", 0, "fork of the writer failed: errno=%d", errno);
        munmap(st, sizeof(*st));
        close(fd);
        free(buf);
        return 1;
    }

    /* Observe for at most 10 s.  A child that has not exited is still inside
     * write(): that is the contract, not a timeout. */
    deadline = now_ms() + 10000;
    for (;;)
    {
        unsigned long m = mem_available_kb();
        pid_t r;

        if (m && (!during_min || m < during_min))
            during_min = m;
        r = waitpid(pid, &st_status, WNOHANG);
        if (r == pid)
        {
            reaped = 1;
            break;
        }
        if (r < 0 && errno != EINTR)
            break;
        if (now_ms() >= deadline)
        {
            blocked = 1;
            break;
        }
        msleep(20);
    }
    sent = __atomic_load_n(&st->sent, __ATOMIC_ACQUIRE);
    err = __atomic_load_n(&st->err, __ATOMIC_ACQUIRE);
    if (!reaped)
    {
        kill(pid, SIGKILL);
        waitpid(pid, &st_status, 0);
    }
    child_state = blocked ? "running" : reaped ? "exited" : "unknown";
    after_write = mem_available_kb();
    if (after_write && (!during_min || after_write < during_min))
        during_min = after_write;
    close(fd);

    /* `flush` is synchronous: tbt_tunnel_shutdown() has run when write()
     * returns.  Reclaim may still trail by an RCU grace period, so poll for
     * at most 3 s.  The socket is closed first, so no live queue can keep
     * the memory: after a flush the pre-write MemAvailable may trail by at
     * most 50 MB of resident/noise, and a hundred-MB drop is a leak. */
    flush_start = now_ms();
    if (config_flush() == 0)
    {
        flushed = 1;
        for (;;)
        {
            after_flush = mem_available_kb();
            if (before && after_flush && after_flush + 51200 >= before)
                break;
            if (now_ms() - flush_start >= 3000)
                break;
            msleep(20);
        }
        flush_ms = now_ms() - flush_start;
    }
    oom1 = oom_count();

    /* PASS: the write finished, or the child was still blocked in write()
     * when the observation ended.  An exited child with a partial write is
     * a failure: EAGAIN, a reset, or any other errno means the module turned
     * backpressure into an error instead of a stall. */
    legal = err == 0 && sent >= 0 && sent <= total && (sent == total || blocked);
    rst = err == ECONNRESET || err == EPIPE;
    mem_ok = flushed && before > 0 && after_flush > 0 &&
             after_flush + 51200 >= before;
    result("pub.writer", legal && mem_ok && !rst && oom1 <= oom0,
           "attempted %d MiB: sent=%ld/%ld errno=%d eagain=%d blocked=%d "
           "child_state=%s mem_before=%lu mem_during_min=%lu "
           "mem_after_flush=%lu flush_secs=%.2f oom=%d->%d",
           mib, sent, total, err, err == EAGAIN, blocked, child_state, before,
           during_min, after_flush, flush_ms < 0 ? -1.0 : flush_ms / 1000.0,
           oom0, oom1);
    munmap(st, sizeof(*st));
    free(buf);
    return 0;
}

static int mode_echo(int port, int mib, int delay_ms)
{
    int fd = connect_to("127.0.0.1", port, NULL);
    long total = (long)mib * 1024 * 1024, done = 0;
    char *buf = malloc(65536);
    char *in = malloc(65536);

    if (fd < 0 || !buf || !in)
    {
        printf("ECHO_FAIL connect\n");
        free(buf);
        free(in);
        return 1;
    }
    memset(buf, 'E', 65536);
    while (done < total)
    {
        size_t want = (size_t)((total - done) < 65536 ? (total - done) : 65536);
        size_t got = 0;
        ssize_t n;

        if (send_all(fd, buf, want))
        {
            printf("ECHO_ABORT op=send errno=%d bytes=%ld\n", errno, done);
            goto out;
        }
        while (got < want)
        {
            n = read_to(fd, in + got, want - got, 5000);
            if (n == -2)
            {
                printf("ECHO_TIMEOUT op=read bytes=%ld\n", done);
                goto out;
            }
            if (n == 0)
            {
                printf("ECHO_EOF bytes=%ld\n", done);
                goto out;
            }
            if (n < 0)
            {
                printf("ECHO_ABORT op=read errno=%d bytes=%ld\n", errno, done);
                goto out;
            }
            got += (size_t)n;
        }
        if (memcmp(in, buf, want))
        {
            printf("ECHO_CORRUPT bytes=%ld\n", done);
            goto out;
        }
        done += (long)want;
        if (delay_ms)
            msleep(delay_ms);
    }
    printf("ECHO_OK bytes=%ld\n", done);
out:
    close(fd);
    free(buf);
    free(in);
    return 0;
}

/* 100 repetitions of an unauthenticated garbage connection, for the slab
 * growth measurement. */
static int mode_garbage(int port, int n)
{
    int i, closed = 0;

    for (i = 0; i < n; i++)
    {
        int fd = connect_to("127.0.0.1", port, NULL);
        char g[9000];

        if (fd < 0)
            continue;
        memset(g, 'G', sizeof(g));
        send_all(fd, g, sizeof(g));
        if (expect_close(fd, 1000))
            closed++;
        close(fd);
    }
    printf("GARBAGE_DONE n=%d closed=%d\n", n, closed);
    return 0;
}

/* Write one evidence line through /dev/kmsg.  A printk line is atomic, so the
 * STALL records survive the kernel's own carrier logs on the same serial
 * console; a userspace printf can be split by an interleaved printk. */
static void kmsg(const char *fmt, ...)
{
    char b[768];
    int fd, n;
    va_list ap;

    fd = open("/dev/kmsg", O_WRONLY);
    if (fd < 0)
        return;
    n = snprintf(b, sizeof(b), "<6>STALL ");
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof(b) - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > 0)
        write(fd, b, (size_t)n);
    close(fd);
}

/* Sequential carrier joins: each connection sends a valid HELLO, reads the
 * far's HELLO, closes, and the next one reuses the slot.  After each step the
 * session counters are recorded, so "accepted but never parsed" has a
 * per-connection trail: hello=0 means the far never answered, adopt= means
 * the accept was not counted, and the work/read counters say whether the
 * receive work ran at all. */
static int mode_stall(int port, int n, int tunnel)
{
    unsigned char nonce[16], body[128];
    struct fr f;
    static struct rb r;
    int i, all_hello = 1;
    unsigned long long adopt = 0, dup = 0, nr = 0;

    for (i = 0; i < 16; i++)
        nonce[i] = (unsigned char)(0x5a + i);

    for (i = 0; i < n; i++)
    {
        int idx = i % 4;
        int fd = far_conn(port);
        int hello = 0;

        if (fd < 0)
        {
            kmsg("i=%d idx=%d connect=0\n", i, idx);
            all_hello = 0;
            continue;
        }
        memset(&r, 0, sizeof(r));
        r.fd = fd;
        if (send_hello(fd, nonce, idx, 1, TBT_ROLE_NEAR) == 0)
            hello = read_hello(&r, &f, body, sizeof(body), NULL, NULL) == 0;
        if (!hello)
            all_hello = 0;

        adopt = stat_key(tunnel, "rx_adopt_events");
        dup = stat_key(tunnel, "join_dup");
        nr = stat_key(tunnel, "join_nonce_reject");
        kmsg("i=%d idx=%d hello=%d adopt=%llu join_dup=%llu nonce_reject=%llu "
             "work_runs=%llu ready=%llu reads=%llu zero=%llu paused=%llu "
             "early=%llu records=%llu\n",
             i, idx, hello, adopt, dup, nr,
             stat_key(tunnel, "rx_work_runs"), stat_key(tunnel, "rx_ready_calls"),
             stat_key(tunnel, "rx_reads"), stat_key(tunnel, "rx_read_zero"),
             stat_key(tunnel, "rx_paused_skips"), stat_key(tunnel, "rx_work_early"),
             stat_key(tunnel, "rx_records"));
        close(fd);
        msleep(250);
    }
    kmsg("END n=%d hello_all=%d adopt=%llu join_dup=%llu nonce_reject=%llu\n",
         n, all_hello, stat_key(tunnel, "rx_adopt_events"),
         stat_key(tunnel, "join_dup"), stat_key(tunnel, "join_nonce_reject"));
    result("stall.hello_chain", all_hello && stat_key(tunnel, "rx_adopt_events") >= (unsigned long long)n,
           "%d sequential joins: every HELLO answered=%d, rx_adopt_events=%llu, "
           "join_dup=%llu, join_nonce_reject=%llu", n, all_hello,
           stat_key(tunnel, "rx_adopt_events"), stat_key(tunnel, "join_dup"),
           stat_key(tunnel, "join_nonce_reject"));
    return 0;
}

/* A deterministic echo target: accept, echo until EOF, shut the write side,
 * close.  Busybox `nc -e cat` is fine for a bulk echo, but its close
 * semantics after cat exits are not specified; half-close tests need a target
 * whose FIN is guaranteed.  The parent reaps children so the guest does not
 * accumulate zombies across a long suite. */
static int mode_target(int port)
{
    int ls = listen_on(port);

    if (ls < 0)
    {
        printf("TARGET_FAIL bind %d errno=%d\n", port, errno);
        return 1;
    }
    printf("TARGET_READY port=%d\n", port);
    fflush(stdout);
    /* 100 short-lived echo children must not wait for the next accept to be
     * reaped; the parent's waitpid() then just returns ECHILD. */
    signal(SIGCHLD, SIG_IGN);
    for (;;)
    {
        int fd = accept(ls, NULL, NULL);
        pid_t pid;

        if (fd < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        pid = fork();
        if (pid == 0)
        {
            char b[65536];
            ssize_t n;

            close(ls);
            while ((n = read(fd, b, sizeof(b))) > 0)
            {
                if (send_all(fd, b, (size_t)n))
                    break;
            }
            shutdown(fd, SHUT_WR);
            close(fd);
            _exit(0);
        }
        close(fd);
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Attack 4: control surface                                           */

static int mode_nonroot(const char *path)
{
    pid_t pid = fork();
    int st = 0;

    if (pid == 0)
    {
        int fd;

        setgid(1000);
        setuid(1000);
        fd = open(path, O_WRONLY);
        if (fd < 0)
        {
            printf("NONROOT_OPEN errno=%d\n", errno);
            _exit(0);
        }
        if (write(fd, "flush\n", 6) < 0)
            printf("NONROOT_WRITE errno=%d\n", errno);
        else
            printf("NONROOT_WRITE ok\n");
        close(fd);
        _exit(1);
    }
    waitpid(pid, &st, 0);
    result("ctl.nonroot", WIFEXITED(st) && WEXITSTATUS(st) == 0,
           "uid 1000 write to %s: open failed (the documented mode is 0600 root)", path);
    return 0;
}

static int mode_cfgwrite(const char *path, const char *what)
{
    char *buf;
    size_t len, chars;
    int newline;
    ssize_t n;
    int fd, err;

    if (!strcmp(what, "513"))
    {
        chars = 513;
        newline = 1;
    }
    else if (!strcmp(what, "512"))
    {
        chars = 512;
        newline = 1;
    }
    else if (!strcmp(what, "512nonl"))
    {
        chars = 512;
        newline = 0;
    }
    else if (!strcmp(what, "1025"))
    {
        chars = 1025;
        newline = 1;
    }
    else if (!strcmp(what, "1024nonl"))
    {
        chars = 1024;
        newline = 0;
    }
    else
    {
        result("ctl.cfgwrite", 0, "unknown mode");
        return 1;
    }
    len = chars + (size_t)newline;
    buf = malloc(len);
    if (!buf)
    {
        result("ctl.cfgwrite", 0, "malloc failed");
        return 1;
    }
    memset(buf, 'a', chars);
    if (newline)
        buf[chars] = '\n';
    fd = open(path, O_WRONLY);
    if (fd < 0)
    {
        result("ctl.cfgwrite", 0, "open %s: errno=%d", path, errno);
        free(buf);
        return 1;
    }
    errno = 0;
    n = write(fd, buf, len);
    err = errno;
    close(fd);
    free(buf);
    if (!strcmp(what, "513"))
        result("ctl.line513", n < 0 || n == (ssize_t)len,
               "513-byte line + newline: write=%zd errno=%d (refused or buffered; "
               "the current line buffer is 1024 bytes)", n, err);
    else if (!strcmp(what, "512"))
        result("ctl.line512", n < 0 || n == (ssize_t)len,
               "512-byte line + newline: write=%zd errno=%d (refused or buffered)", n, err);
    else if (!strcmp(what, "512nonl"))
        result("ctl.line512nonl", n == 512 || (n < 0 && err == E2BIG),
               "512 bytes with no newline: write=%zd errno=%d (buffered or refused, "
               "never applied)", n, err);
    else if (!strcmp(what, "1025"))
        result("ctl.line1025", n < 0,
               "1025-byte line + newline: write=%zd errno=%d (must be refused with "
               "the 1024-byte buffer)", n, err);
    else
        result("ctl.line1024nonl", n < 0 || n == 1024,
               "1024 bytes with no newline: write=%zd errno=%d", n, err);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Config read-back at the exact-size limit (review 19/30)             */

/* Read a whole file. `chunk` is the per-read size, so the caller can force
 * the offset-serving path with a buffer much smaller than the file. Returns
 * the byte count, -1 on an error, or -2 when the file did not fit in cap. */
static ssize_t read_whole(const char *path, char *buf, size_t cap, size_t chunk)
{
    int fd = open(path, O_RDONLY);
    size_t off = 0;

    if (fd < 0)
        return -1;
    while (off < cap)
    {
        ssize_t n = read(fd, buf + off, chunk);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (n == 0)
        {
            close(fd);
            return (ssize_t)off;
        }
        off += (size_t)n;
    }
    close(fd);
    return -2;
}

static int line_count(const char *b, size_t n)
{
    size_t i;
    int lines = 0;

    for (i = 0; i < n; i++)
        if (b[i] == '\n')
            lines++;
    return lines;
}

static int line_of_offset(const char *b, size_t n, size_t off)
{
    size_t i;
    int line = 1;

    if (off > n)
        off = n;
    for (i = 0; i < off; i++)
        if (b[i] == '\n')
            line++;
    return line;
}

static size_t first_difference(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return i;
    return n;
}

/* Compare the module's rendered config with the exact lines that were
 * written, reading it once in 1000-byte chunks (the offset path that review
 * 19 fixed) and once in one large read. A >32 KiB config proves the old
 * TBT_CFG_RENDER_MAX truncation cannot return. */
static int mode_cfgbig(const char *cfg, const char *expected)
{
    static char small[262144], big[262144], exp[262144];
    ssize_t ns, nb, ne;
    size_t diff = 0;
    int ok, tunnels = 0;
    size_t p;

    ns = read_whole(cfg, small, sizeof(small), 1000);
    nb = read_whole(cfg, big, sizeof(big), 65536);
    ne = read_whole(expected, exp, sizeof(exp), 65536);
    if (ns < 0 || nb < 0 || ne < 0)
    {
        result("cfg.big_roundtrip", 0,
               "read failed: cfg/1000=%zd cfg/65536=%zd expected=%zd errno=%d",
               ns, nb, ne, errno);
        return 1;
    }
    p = 0;
    while (p < (size_t)ne)
    {
        size_t q = p;

        while (q < (size_t)ne && exp[q] != '\n')
            q++;
        if (q - p > 7 && !memcmp(exp + p, "tunnel ", 7))
            tunnels++;
        p = q + 1;
    }
    ok = ns == ne && nb == ns && (size_t)ne > 32768 &&
         !memcmp(small, exp, (size_t)ne) && !memcmp(big, small, (size_t)ns);
    if (!ok)
    {
        size_t m = (size_t)(ns < ne ? ns : ne);

        diff = first_difference(small, exp, m);
    }
    if (ok)
        result("cfg.big_roundtrip", 1,
               "config served in 1000-byte reads: %zd bytes, %d lines (%d tunnel "
               "directives); one-shot read %zd bytes; expected %zd bytes; "
               "byte-identical and larger than 32768",
               ns, line_count(exp, (size_t)ne), tunnels, nb, ne);
    else
        result("cfg.big_roundtrip", 0,
               "config served in 1000-byte reads: %zd bytes, %d lines (%d tunnels); "
               "one-shot %zd bytes; expected %zd bytes; first difference at "
               "offset %zu (line %d)",
               ns, line_count(exp, (size_t)ne), tunnels, nb, ne, diff,
               line_of_offset(exp, (size_t)ne, diff));
    return 0;
}

/* ------------------------------------------------------------------ */
/* /proc/net/tbtun/http: a torn request block (review 33)              */

static const char http_reqid_key[] = "\r\nX-Request-Id: ";

/* A block is either absent or the complete POST header block the near side
 * emitted: the request line, CRLF line endings only, one 26-character base32
 * request id, and the closing CRLFCRLF. */
static int http_block_ok(const char *b, size_t n, char *why, size_t whylen)
{
    static const char req[] = "POST /api/v2/upload HTTP/1.1\r\n";
    const char *id;
    size_t i;

    if (n < sizeof(req) - 1 + 4)
    {
        snprintf(why, whylen, "short block (%zu bytes)", n);
        return 0;
    }
    if (memchr(b, '\0', n))
    {
        snprintf(why, whylen, "NUL inside %zu bytes", n);
        return 0;
    }
    if (memcmp(b, req, sizeof(req) - 1))
    {
        snprintf(why, whylen, "prefix is not the POST request line");
        return 0;
    }
    if (memcmp(b + n - 4, "\r\n\r\n", 4))
    {
        snprintf(why, whylen, "no CRLFCRLF at the end (%zu bytes)", n);
        return 0;
    }
    for (i = 0; i < n; i++)
        if (b[i] == '\n' && (i == 0 || b[i - 1] != '\r'))
        {
            snprintf(why, whylen, "bare LF at offset %zu", i);
            return 0;
        }
    id = memmem(b, n, http_reqid_key, sizeof(http_reqid_key) - 1);
    if (!id)
    {
        snprintf(why, whylen, "no X-Request-Id header");
        return 0;
    }
    id += sizeof(http_reqid_key) - 1;
    if ((size_t)(id - b) + 28 > n)
    {
        snprintf(why, whylen, "truncated X-Request-Id");
        return 0;
    }
    for (i = 0; i < 26; i++)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '2' && id[i] <= '7')))
        {
            snprintf(why, whylen, "bad X-Request-Id byte %zu", i);
            return 0;
        }
    if (id[26] != '\r' || id[27] != '\n')
    {
        snprintf(why, whylen, "X-Request-Id is not terminated");
        return 0;
    }
    if (memmem(id + 28, n - (size_t)(id + 28 - b), http_reqid_key,
               sizeof(http_reqid_key) - 1))
    {
        snprintf(why, whylen, "two X-Request-Id headers");
        return 0;
    }
    return 1;
}

/* Keep one application stream busy and read the debug surface 100 times.
 * Every read must be empty or the whole block; a reader that copies the live
 * buffer without pump_lock can catch the writer mid-memcpy. */
static int mode_httptorn(int port, int reads)
{
    int fd = connect_to("127.0.0.1", port, NULL);
    int complete = 0, empty = 0, partial = 0, first_bad = -1, i;
    char why[128] = "none";
    pid_t pid;

    if (fd < 0)
    {
        result("cat.http_torn", 0, "cannot connect to the public port");
        return 1;
    }
    pid = fork();
    if (pid == 0)
    {
        char *b = malloc(4096);

        if (!b)
            _exit(1);
        memset(b, 'H', 4096);
        for (;;)
        {
            if (send_all(fd, b, 4096))
                break;
            msleep(1);
        }
        _exit(0);
    }
    /* Let the near's carrier 0 emit its first POST block before reading. */
    msleep(300);
    for (i = 0; i < reads; i++)
    {
        char buf[65536];
        int cfd = open("/proc/net/tbtun/http", O_RDONLY);
        ssize_t n;

        if (cfd < 0)
        {
            partial++;
            if (first_bad < 0)
            {
                first_bad = i;
                snprintf(why, sizeof(why), "open errno=%d", errno);
            }
            msleep(2);
            continue;
        }
        n = read(cfd, buf, sizeof(buf));
        close(cfd);
        if (n == 0)
            empty++;
        else if (n < 0)
        {
            partial++;
            if (first_bad < 0)
            {
                first_bad = i;
                snprintf(why, sizeof(why), "read errno=%d", errno);
            }
        }
        else
        {
            char tmp[128];

            if (http_block_ok(buf, (size_t)n, tmp, sizeof(tmp)))
                complete++;
            else
            {
                partial++;
                if (first_bad < 0)
                {
                    first_bad = i;
                    snprintf(why, sizeof(why), "%s", tmp);
                }
            }
        }
        msleep(2);
    }
    if (pid > 0)
    {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    close(fd);
    result("cat.http_torn", partial == 0 && complete > 0 && complete + empty == reads,
           "%d reads of /proc/net/tbtun/http during an active stream: complete=%d "
           "empty=%d partial=%d first_bad=%d (%s)",
           reads, complete, empty, partial, first_bad, why);
    return 0;
}

/* ------------------------------------------------------------------ */
/* A real hole kill and its age (review 24)                            */

/* Join a far tunnel, open a stream, then send one byte at offset 1 so offset
 * 0 stays missing. The tunnel is configured with hole_budget_ms=100, so the
 * 10 ms feedback tick logs RXHOLEKILL about 100-110 ms later. The shell reads
 * dmesg and checks age_us; this mode proves the kill happened at all. */
static int mode_hole(int port, int tunnel)
{
    unsigned char nonce[16];
    static struct rb r;
    uint32_t sid = 0x686f6c65u; /* "hole" */
    unsigned long long before, after;
    int fd, gen = 0, i;

    for (i = 0; i < 16; i++)
        nonce[i] = (unsigned char)(0x40 + i);
    before = stat_key(tunnel, "holes_unrecoverable");
    fd = far_join_rb(port, nonce, 0, 1, &gen, NULL, &r);
    if (fd < 0)
    {
        result("hole.created", 0, "carrier join failed");
        return 1;
    }
    {
        struct fr o = { .type = TBT_F_OPEN, .path = 0, .sid = sid,
                        .conv = TBT_CONV };

        if (send_frame(fd, &o, NULL, 0))
        {
            result("hole.created", 0, "OPEN send failed");
            close(fd);
            return 1;
        }
    }
    msleep(300);
    if (send_data(fd, 0, sid, 0, 1, "X", 1))
    {
        result("hole.created", 0, "DATA send failed");
        close(fd);
        return 1;
    }
    after = before;
    for (i = 0; i < 30; i++)
    {
        msleep(100);
        after = stat_key(tunnel, "holes_unrecoverable");
        if (after > before)
            break;
    }
    result("hole.created", after > before,
           "DATA seq=1 len=1 leaves offset 0 missing: holes_unrecoverable "
           "%llu -> %llu after %d00 ms (ooo_bytes=%llu ooo_frames=%llu "
           "rx_max_stall_us=%llu)",
           before, after, i, stat_key(tunnel, "ooo_bytes"),
           stat_key(tunnel, "ooo_frames"), stat_key(tunnel, "rx_max_stall_us"));
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* A 100 MiB write with a reader draining the echo                     */

struct drain_state {
    long sent;
    long got;
    int werr;
    int rerr;
};

/* The blocked-writer case proves backpressure; this one proves the other half
 * of the contract: with the application reading, the same 100 MiB must finish
 * byte-exact, with no EAGAIN, no RST, and no memory left after flush. */
static int mode_writerdrain(int port, int mib)
{
    int fd = connect_to("127.0.0.1", port, NULL);
    long total = (long)mib * 1024 * 1024;
    const char *name = mib >= 1024 ? "pub.writer_drain_1g" : "pub.writer_drain";
    struct timeval zero = { 0, 0 };
    struct drain_state *st;
    char *buf = malloc(1024 * 1024);
    unsigned long before, during_min, after_write, after_flush = 0;
    int oom0, oom1, flushed = 0, legal, mem_ok, rst, eagain;
    long flush_start, flush_ms = -1, deadline, t0, elapsed_ms;
    long sent, got;
    int werr, rerr, writer_done = 0, reader_done = 0;
    int wstatus = 0, rstatus = 0;
    pid_t wp = -1, rp = -1;

    if (fd < 0 || !buf)
    {
        result(name, 0, "cannot connect or allocate");
        free(buf);
        return 1;
    }
    /* The public socket is ordinary TCP. A send/recv timeout would turn the
     * module's backpressure into EAGAIN, which is the failure this test looks
     * for; disable both and let the drain run to completion. */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &zero, sizeof(zero));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));
    st = mmap(NULL, sizeof(*st), PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (st == MAP_FAILED)
    {
        result(name, 0, "mmap of the drain state failed");
        close(fd);
        free(buf);
        return 1;
    }
    memset(st, 0, sizeof(*st));
    memset(buf, 'D', 1024 * 1024);
    before = mem_available_kb();
    during_min = before;
    oom0 = oom_count();
    t0 = now_ms();

    rp = fork();
    if (rp == 0)
    {
        char *rb = malloc(65536);
        long got = 0;

        if (!rb)
            _exit(1);
        for (;;)
        {
            ssize_t n = recv(fd, rb, 65536, 0);

            if (n > 0)
            {
                got += n;
                __atomic_store_n(&st->got, got, __ATOMIC_RELEASE);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            __atomic_store_n(&st->rerr, n < 0 ? errno : 0, __ATOMIC_RELEASE);
            break;
        }
        free(rb);
        _exit(0);
    }
    wp = fork();
    if (wp == 0)
    {
        long sent = 0;

        while (sent < total)
        {
            ssize_t n = send(fd, buf, 1024 * 1024, MSG_NOSIGNAL);

            if (n <= 0)
            {
                __atomic_store_n(&st->werr, errno, __ATOMIC_RELEASE);
                break;
            }
            sent += n;
            __atomic_store_n(&st->sent, sent, __ATOMIC_RELEASE);
        }
        _exit(0);
    }
    if (rp < 0 || wp < 0)
    {
        if (rp > 0)
        {
            kill(rp, SIGKILL);
            waitpid(rp, &rstatus, 0);
        }
        if (wp > 0)
        {
            kill(wp, SIGKILL);
            waitpid(wp, &wstatus, 0);
        }
        result(name, 0, "fork failed: errno=%d", errno);
        munmap(st, sizeof(*st));
        close(fd);
        free(buf);
        return 1;
    }

    /* The writer's deadline is generous: 100 MiB through one raw carrier on
     * loopback, with the reader keeping the pipe moving. A timeout means the
     * drain path is broken, not that the test is slow. */
    deadline = now_ms() + 180000;
    while (now_ms() < deadline)
    {
        unsigned long m = mem_available_kb();
        pid_t r;

        if (m && (!during_min || m < during_min))
            during_min = m;
        r = waitpid(wp, &wstatus, WNOHANG);
        if (r == wp)
        {
            writer_done = 1;
            break;
        }
        if (r < 0 && errno != EINTR)
            break;
        msleep(10);
    }
    if (!writer_done)
    {
        kill(wp, SIGKILL);
        waitpid(wp, &wstatus, 0);
    }
    /* The application's write side is done; the target must see EOF so the
     * reader can drain the last echo instead of waiting for a close. */
    shutdown(fd, SHUT_WR);
    deadline = now_ms() + 60000;
    while (now_ms() < deadline)
    {
        pid_t r = waitpid(rp, &rstatus, WNOHANG);

        if (r == rp)
        {
            reader_done = 1;
            break;
        }
        if (r < 0 && errno != EINTR)
            break;
        msleep(10);
    }
    if (!reader_done)
    {
        kill(rp, SIGKILL);
        waitpid(rp, &rstatus, 0);
    }
    elapsed_ms = now_ms() - t0;
    sent = __atomic_load_n(&st->sent, __ATOMIC_ACQUIRE);
    got = __atomic_load_n(&st->got, __ATOMIC_ACQUIRE);
    werr = __atomic_load_n(&st->werr, __ATOMIC_ACQUIRE);
    rerr = __atomic_load_n(&st->rerr, __ATOMIC_ACQUIRE);
    after_write = mem_available_kb();
    if (after_write && (!during_min || after_write < during_min))
        during_min = after_write;
    close(fd);

    flush_start = now_ms();
    if (config_flush() == 0)
    {
        flushed = 1;
        for (;;)
        {
            after_flush = mem_available_kb();
            if (before && after_flush && after_flush + 51200 >= before)
                break;
            if (now_ms() - flush_start >= 3000)
                break;
            msleep(20);
        }
        flush_ms = now_ms() - flush_start;
    }
    oom1 = oom_count();

    legal = writer_done && werr == 0 && sent == total && reader_done &&
            got == total && rerr == 0;
    rst = werr == ECONNRESET || werr == EPIPE || rerr == ECONNRESET ||
          rerr == EPIPE;
    eagain = werr == EAGAIN || rerr == EAGAIN;
    mem_ok = flushed && before > 0 && after_flush > 0 &&
             after_flush + 51200 >= before;
    result(name, legal && !rst && !eagain && mem_ok && oom1 <= oom0,
           "attempted %d MiB with a draining reader: sent=%ld/%ld got=%ld/%ld "
           "errno=%d/%d eagain=%d blocked=%d secs=%.2f mbps=%.1f "
           "mem_before=%lu mem_during_min=%lu mem_after_flush=%lu "
           "flush_secs=%.2f oom=%d->%d",
           mib, sent, total, got, total, werr, rerr, eagain, !writer_done,
           elapsed_ms / 1000.0,
           elapsed_ms > 0 ? (double)mib / (elapsed_ms / 1000.0) : 0.0,
           before, during_min, after_flush,
           flush_ms < 0 ? -1.0 : flush_ms / 1000.0, oom0, oom1);
    munmap(st, sizeof(*st));
    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* An over-long unterminated line must not be applied at release        */

static int tunnel_row_present(int id)
{
    FILE *p = fopen("/proc/net/tbtun/tunnels", "r");
    char line[512], want[16];
    int found = 0;

    if (!p)
        return 0;
    snprintf(want, sizeof(want), "%d ", id);
    while (fgets(line, sizeof(line), p))
        if (!strncmp(line, want, strlen(want)))
            found = 1;
    fclose(p);
    return found;
}

/* review 31: a valid line buffered without a newline, then enough bytes to
 * overflow the 1024-byte line buffer. The write must report -E2BIG and the
 * release-time drain must discard the tail instead of applying its prefix. */
static int mode_overflow(const char *path)
{
    static const char line[] =
        "tunnel 77 role=far profile=raw listen=127.0.0.1:22777 target=127.0.0.1:19070 n=1 m=0";
    char junk[1100];
    ssize_t n1, n2;
    int fd, err, applied;

    memset(junk, 'x', sizeof(junk));
    fd = open(path, O_WRONLY);
    if (fd < 0)
    {
        result("ctl.release_overflow", 0, "open %s: errno=%d", path, errno);
        return 1;
    }
    errno = 0;
    n1 = write(fd, line, sizeof(line) - 1);
    errno = 0;
    n2 = write(fd, junk, sizeof(junk));
    err = errno;
    close(fd);
    applied = tunnel_row_present(77);
    result("ctl.release_overflow",
           n1 == (ssize_t)(sizeof(line) - 1) && n2 < 0 && err == E2BIG && !applied,
           "valid %zu-byte tunnel line buffered without a newline, then %zu "
           "bytes: write2=%zd errno=%d; tunnel 77 applied=%d (the release "
           "drain must discard an overflowed tail)",
           sizeof(line) - 1, sizeof(junk), n2, err, applied);
    return 0;
}

/* Write an unterminated line and exit without closing the descriptor.  The
 * kernel's exit_files() releases it from task_work, after
 * exit_task_namespaces(); the module's release path then applies the line
 * with current->nsproxy == NULL.  This is the documented
 * "printf 'del 1' > config" close-flush contract, with a tunnel line. */
static int mode_partial(const char *path, const char *line)
{
    int fd = open(path, O_WRONLY);
    ssize_t n;

    if (fd < 0)
    {
        printf("PARTIAL_OPEN errno=%d\n", errno);
        return 1;
    }
    n = write(fd, line, strlen(line));
    printf("PARTIAL_WROTE n=%zd\n", n);
    fflush(stdout);
    _exit(0);
}

/* ------------------------------------------------------------------ */
/* main                                                                */

static void usage(void)
{
    fprintf(stderr,
            "usage: adv_probe far <carrier_port> <tunnel_id>\n"
            "       adv_probe fakefar <carrier_port> <pub_port> <tunnel_id>\n"
            "       adv_probe churn <pub_port> <n>\n"
            "       adv_probe two <pub_port>\n"
            "       adv_probe concurrent <pub_port> <n> <bytes> <timeout_s> <tunnel_id>\n"
            "       adv_probe halfclose <pub_port>\n"
            "       adv_probe rst <pub_port> <mib>\n"
            "       adv_probe srcfilter <pub_port> <tunnel_id>\n"
            "       adv_probe writer <pub_port> <mib> <read>\n"
            "       adv_probe echo <pub_port> <mib> <delay_ms>\n"
            "       adv_probe garbage <carrier_port> <n>\n"
            "       adv_probe stall <carrier_port> <n> <tunnel_id>\n"
            "       adv_probe noncepinned <carrier_port> <tunnel_id>\n"
            "       adv_probe srcanchor <carrier_port> <tunnel_id>\n"
            "       adv_probe overlap <carrier_port> <tunnel_id>\n"
            "       adv_probe secondcarrier <carrier_port> <tunnel_id>\n"
            "       adv_probe fineagain <fake_port> <pub_port> <tunnel_id> fin|rst\n"
            "       adv_probe target <port>\n"
            "       adv_probe partial <config_path> <line-without-newline>\n"
            "       adv_probe nonroot <path>\n"
            "       adv_probe cfgwrite <path> 513|512|512nonl\n"
            "       adv_probe cfgbig <config_path> <expected_lines_file>\n"
            "       adv_probe httptorn <pub_port> <reads>\n"
            "       adv_probe hole <carrier_port> <tunnel_id>\n"
            "       adv_probe writerdrain <pub_port> <mib>\n"
            "       adv_probe overflow <config_path>\n");
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2)
    {
        usage();
        return 2;
    }
    if (!strcmp(argv[1], "far") && argc == 4)
        return far_suite(atoi(argv[2]), atoi(argv[3])) ? 1 : 0;
    if (!strcmp(argv[1], "fakefar") && argc == 5)
        return fakefar_suite(atoi(argv[2]), atoi(argv[3]), atoi(argv[4])) ? 1 : 0;
    if (!strcmp(argv[1], "churn") && argc == 4)
        return mode_churn(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(argv[1], "two") && argc == 3)
        return mode_two(atoi(argv[2]));
    if (!strcmp(argv[1], "concurrent") && argc == 7)
        return mode_concurrent(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                               atoi(argv[5]), atoi(argv[6]));
    if (!strcmp(argv[1], "halfclose") && argc == 3)
        return mode_halfclose(atoi(argv[2]));
    if (!strcmp(argv[1], "rst") && argc == 4)
        return mode_rst(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(argv[1], "srcfilter") && argc == 4)
        return mode_srcfilter(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(argv[1], "writer") && argc == 5)
        return mode_writer(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(argv[1], "echo") && argc == 5)
        return mode_echo(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(argv[1], "garbage") && argc == 4)
        return mode_garbage(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(argv[1], "stall") && argc == 5)
        return mode_stall(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(argv[1], "noncepinned") && argc == 4)
        return mode_noncepinned(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(argv[1], "srcanchor") && argc == 4)
        return mode_srcanchor(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(argv[1], "overlap") && argc == 4)
        return mode_overlap(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(argv[1], "secondcarrier") && argc == 4)
        return mode_secondcarrier(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(argv[1], "fineagain") && argc == 6 &&
        (!strcmp(argv[5], "fin") || !strcmp(argv[5], "rst")))
        return mode_fineagain(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                              !strcmp(argv[5], "rst"));
    if (!strcmp(argv[1], "target") && argc == 3)
        return mode_target(atoi(argv[2]));
    if (!strcmp(argv[1], "partial") && argc == 4)
        return mode_partial(argv[2], argv[3]);
    if (!strcmp(argv[1], "nonroot") && argc == 3)
        return mode_nonroot(argv[2]);
    if (!strcmp(argv[1], "cfgwrite") && argc == 4)
        return mode_cfgwrite(argv[2], argv[3]);
    if (!strcmp(argv[1], "cfgbig") && argc == 4)
        return mode_cfgbig(argv[2], argv[3]);
    if (!strcmp(argv[1], "httptorn") && argc == 4)
        return mode_httptorn(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(argv[1], "hole") && argc == 4)
        return mode_hole(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(argv[1], "writerdrain") && argc == 4)
        return mode_writerdrain(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(argv[1], "overflow") && argc == 3)
        return mode_overflow(argv[2]);
    usage();
    return 2;
}
