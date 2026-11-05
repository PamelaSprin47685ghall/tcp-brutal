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

    /* 15. OPEN with a different stream id while the anchor owns the session. */
    fd = far_join(port, nonce, 1, 1, NULL, NULL);
    if (fd >= 0)
    {
        struct fr o = { .type = TBT_F_OPEN, .path = 1, .sid = sid ^ 0x55, .conv = TBT_CONV };

        send_frame(fd, &o, NULL, 0);
        result("far.open_other_sid", expect_close(fd, 3000),
               "OPEN for a second sid must be PROTO, not a second stream");
        close(fd);
    }
    else
        result("far.open_other_sid", 0, "carrier join failed");
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
        int ok = 1, got_ping = 0, got_pong = 0, extra = 0;

        d.seq = 0;
        send_frame(anchor, &d, "ping", 4);
        d.seq = 4;
        send_frame(anchor, &d, "pong", 4);
        d.seq = 0;
        send_frame(anchor, &d, "XXXX", 4); /* duplicate seq: must be dropped */
        for (i = 0; i < 20; i++)
        {
            if (rb_frame(&ar, &f, body, sizeof(body), 1000))
                break;
            if (f.type == TBT_F_DATA && f.sid == sid)
            {
                if (f.seq == 0 && f.blen == 4 && !memcmp(body, "ping", 4))
                    got_ping = 1;
                else if (f.seq == 4 && f.blen == 4 && !memcmp(body, "pong", 4))
                    got_pong = 1;
                else
                    extra++;
            }
        }
        if (!got_ping || !got_pong || extra)
            ok = 0;
        result("far.data_path", ok,
               "anchor echo: ping=%d pong=%d unexpected=%d (duplicate seq must not deliver); "
               "far stats rx_records=%llu capture_runs=%llu actor_calls=%llu submit_bytes=%llu "
               "tx_ack=%llu",
               got_ping, got_pong, extra, stat_key(tunnel, "rx_records"),
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
/* Attack 2: public port abuse                                         */

static int mode_churn(int port, int n)
{
    int live = 0, refused = 0, other = 0, i;

    for (i = 0; i < n; i++)
    {
        int fd = connect_to("127.0.0.1", port, NULL);
        ssize_t r;
        char c;

        if (fd < 0)
        {
            other++;
            continue;
        }
        send_all(fd, "x", 1);
        r = read_to(fd, &c, 1, i == 0 ? 3000 : 800);
        if (r == 1)
            live++;
        else if (r == 0 || r == -1)
            refused++;
        else
            other++;
        close(fd);
    }
    result("pub.churn", live == n && other == 0,
           "%d connect/send/close cycles: usable=%d refused=%d other=%d "
           "(a closed stream must free the session for the next connection)",
           n, live, refused, other);
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

static int mode_srcfilter(int port)
{
    int fd = connect_to("127.0.0.1", port, "127.0.0.2");
    char buf[8];
    int ok;

    if (fd < 0)
    {
        result("pub.srcfilter", 0, "cannot connect from 127.0.0.2");
        return 1;
    }
    send_all(fd, "src", 3);
    ok = read_to(fd, buf, 3, 3000) == 3 && !memcmp(buf, "src", 3);
    result("pub.srcfilter", 0,
           "connection from 127.0.0.2 was accepted=%d: no src= allow list exists", ok);
    close(fd);
    return 0;
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

static int mode_writer(int port, int mib, int do_read)
{
    int fd = connect_to("127.0.0.1", port, NULL);
    long total = (long)mib * 1024 * 1024, sent = 0, got = 0;
    unsigned long before = mem_available_kb(), after;
    char *buf = malloc(1024 * 1024);
    int err = 0;

    if (fd < 0 || !buf)
    {
        result("pub.writer", 0, "cannot connect or allocate");
        free(buf);
        return 1;
    }
    memset(buf, 'W', 1024 * 1024);
    while (sent < total)
    {
        ssize_t n = send(fd, buf, 1024 * 1024, MSG_NOSIGNAL);

        if (n <= 0)
        {
            err = errno;
            break;
        }
        sent += n;
        if (do_read)
        {
            while (got < sent)
            {
                ssize_t r = read_to(fd, buf, 1024 * 1024, 2000);

                if (r <= 0)
                    break;
                got += r;
            }
        }
    }
    after = mem_available_kb();
    result("pub.writer", sent < total && after > 200000,
           "attempted %d MiB, accepted %ld bytes then blocked (errno=%d), "
           "MemAvailable %lu -> %lu kB", mib, sent, err, before, after);
    close(fd);
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
            "       adv_probe srcfilter <pub_port>\n"
            "       adv_probe writer <pub_port> <mib> <read>\n"
            "       adv_probe echo <pub_port> <mib> <delay_ms>\n"
            "       adv_probe garbage <carrier_port> <n>\n"
            "       adv_probe stall <carrier_port> <n> <tunnel_id>\n"
            "       adv_probe target <port>\n"
            "       adv_probe partial <config_path> <line-without-newline>\n"
            "       adv_probe nonroot <path>\n"
            "       adv_probe cfgwrite <path> 513|512|512nonl\n");
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
    if (!strcmp(argv[1], "srcfilter") && argc == 3)
        return mode_srcfilter(atoi(argv[2]));
    if (!strcmp(argv[1], "writer") && argc == 5)
        return mode_writer(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(argv[1], "echo") && argc == 5)
        return mode_echo(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(argv[1], "garbage") && argc == 4)
        return mode_garbage(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(argv[1], "stall") && argc == 5)
        return mode_stall(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(argv[1], "target") && argc == 3)
        return mode_target(atoi(argv[2]));
    if (!strcmp(argv[1], "partial") && argc == 4)
        return mode_partial(argv[2], argv[3]);
    if (!strcmp(argv[1], "nonroot") && argc == 3)
        return mode_nonroot(argv[2]);
    if (!strcmp(argv[1], "cfgwrite") && argc == 4)
        return mode_cfgwrite(argv[2], argv[3]);
    usage();
    return 2;
}
