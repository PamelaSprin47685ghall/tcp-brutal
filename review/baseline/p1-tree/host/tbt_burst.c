/* tbt_burst.c - in-guest traffic checker for tests/qemu.
 *
 * Contract: send exactly MIB bytes, receive exactly MIB bytes, byte-compared in
 * order; stops sending when either side is complete.
 *
 * The primary case is the strict single-threaded blocking pattern: send all
 * MIB bytes with blocking send(), then read all MIB bytes with blocking
 * recv(). A tunnel that only survives an application which interleaves read
 * and write would deadlock real applications, so this is the case that decides
 * the exit status. A second interleaved case (EPOLLIN and EPOLLOUT handled in
 * one poll loop) runs afterwards on the same connection for coverage; its
 * failure is reported but does not fail the run.
 *
 * Runs inside the VM as an ordinary process. It does no configuration and
 * touches no kernel interface beyond reading /proc/net/tbtun/stats when it
 * fails: tests/qemu/init writes the /proc/net/tbtun files with `echo`, starts
 * an echo target, and then calls this program with the tunnel's public
 * address. That split keeps the /proc contract exercised exactly as an
 * operator would, and leaves this file with one job: prove the bytes that
 * cross the tunnel come back unchanged.
 *
 * The stream is a xorshift64* keyed by block index, so the sender and the
 * checker generate the expected block independently and neither holds the whole
 * payload. Every returned byte is compared in order; the first mismatch is
 * reported with its offset.
 *
 * Usage: tbt_burst HOST PORT [MIB] [SECONDS]
 *
 * Exit codes:
 *   0  MIB crossed in both directions in the strict case, byte for byte
 *   1  connect failed, or the stream stalled past the timeout
 *   2  the listener accepted and closed without sending a byte back (the
 *      tunnel accepted the connection but carries no data)
 *   3  the echo was short or differed from what was sent
 *   4  bad usage or allocation failure
 *
 * Built by tests/qemu/mkinitramfs.sh with the host compiler. glibc's static
 * crt files are not installed here, so the binary is dynamic and the
 * initramfs carries its loader and libraries; a musl cross build would make
 * this unnecessary but is not required.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <unistd.h>

#define BLOCK 8192

struct result
{
    long sent;     /* bytes accepted by send() in this phase */
    long recvd;    /* bytes read back in this phase */
    long mismatch; /* absolute stream offset of the first bad byte, or -1 */
    int err;       /* errno of the failure, 0 for EOF/short */
};

static uint64_t xs(uint64_t *s)
{
    uint64_t x = *s;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

/* Bytes of block `block`, generated identically on both ends of the check. */
static void fill(unsigned char *buf, size_t len, uint64_t block)
{
    uint64_t s = 0x9E3779B97F4A7C15ull ^ (block * 0xD1B54A32D192ED03ull);
    size_t i;

    for (i = 0; i < len; i += 8)
    {
        uint64_t v = xs(&s);
        size_t n = len - i < 8 ? len - i : 8;

        memcpy(buf + i, &v, n);
    }
}

/* Fill `len` bytes of the endless stream starting at absolute offset `off`. */
static void stream_fill(unsigned char *dst, size_t len, uint64_t off)
{
    unsigned char block[BLOCK];

    while (len)
    {
        size_t in = (size_t)(off % BLOCK);
        size_t n = BLOCK - in;

        if (n > len)
            n = len;
        fill(block, BLOCK, off / BLOCK);
        memcpy(dst, block + in, n);
        dst += n;
        off += n;
        len -= n;
    }
}

/* Absolute offset of the first received byte that differs from the stream, or
 * -1 when all `len` bytes starting at stream offset `off` match. */
static long mismatch_at(const unsigned char *got, size_t off, size_t len)
{
    unsigned char expect[BLOCK];
    size_t done = 0;

    while (done < len)
    {
        size_t in = (off + done) % BLOCK;
        size_t take = BLOCK - in;

        if (take > len - done)
            take = len - done;
        fill(expect, BLOCK, (uint64_t)((off + done) / BLOCK));
        if (memcmp(got + done, expect + in, take))
        {
            size_t k;

            for (k = 0; k < take; k++)
                if (got[done + k] != expect[in + k])
                    return (long)(off + done + k);
        }
        done += take;
    }
    return -1;
}

/* The failure banner carries everything a human needs without another run:
 * the counters, the first bad offset, errno, and the tunnel counters. */
static void dump_stats(void)
{
    int fd = open("/proc/net/tbtun/stats", O_RDONLY);
    char buf[4096];
    ssize_t n;

    if (fd < 0)
    {
        printf("burst: /proc/net/tbtun/stats: %s\n", strerror(errno));
        return;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, stdout);
    close(fd);
}

static void report(const char *phase, const struct result *r)
{
    printf("burst: %s failed: sent=%ld recvd=%ld first_mismatch=%ld errno=%d (%s)\n",
           phase, r->sent, r->recvd, r->mismatch, r->err,
           r->err ? strerror(r->err) : "none");
    dump_stats();
}

static int set_timeouts(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)))
        return -1;
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* Primary case: blocking send() of the whole volume, then blocking recv().
 * No poll, no interleaving; this is what an unmodified single-threaded
 * application does. */
static int strict_phase(int fd, long total, struct result *r)
{
    unsigned char *buf = malloc(BLOCK);
    long off;

    if (!buf)
    {
        r->err = ENOMEM;
        return -1;
    }
    for (off = 0; off < total;)
    {
        size_t want = (size_t)(total - off < BLOCK ? total - off : BLOCK);
        ssize_t n;

        stream_fill(buf, want, (uint64_t)off);
        n = send(fd, buf, want, MSG_NOSIGNAL);
        if (n <= 0)
        {
            r->err = n ? errno : EPIPE;
            free(buf);
            return -1;
        }
        off += n;
        r->sent = off;
    }
    for (off = 0; off < total;)
    {
        size_t want = (size_t)(total - off < BLOCK ? total - off : BLOCK);
        ssize_t n;

        n = recv(fd, buf, want, 0);
        if (n < 0)
        {
            r->err = errno;
            free(buf);
            return -1;
        }
        if (n == 0)
        {
            free(buf);
            return -1;
        }
        if (r->mismatch < 0)
        {
            long m = mismatch_at(buf, (size_t)off, (size_t)n);

            if (m >= 0)
                r->mismatch = m;
        }
        off += n;
        r->recvd = off;
    }
    free(buf);
    return r->mismatch < 0 ? 0 : -1;
}

/* Coverage case: both directions in one poll loop, stopping the send when
 * either side is complete. Starts at stream offset `base` so it continues the
 * same byte stream as the strict phase on the same connection. */
static int interleaved_phase(int fd, long total, uint64_t base, int seconds, struct result *r)
{
    unsigned char *buf = malloc(BLOCK);
    struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLOUT };
    long sent = 0, recvd = 0;

    if (!buf)
    {
        r->err = ENOMEM;
        return -1;
    }
    while (sent < total || recvd < total)
    {
        ssize_t n;

        pfd.revents = 0;
        if (poll(&pfd, 1, seconds * 1000) <= 0)
        {
            r->err = errno ? errno : ETIMEDOUT;
            break;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            r->err = EPIPE;
            break;
        }
        if (pfd.revents & POLLIN)
        {
            size_t want = (size_t)(total - recvd < BLOCK ? total - recvd : BLOCK);

            n = recv(fd, buf, want, 0);
            if (n < 0)
            {
                r->err = errno;
                break;
            }
            if (n == 0)
                break;
            if (r->mismatch < 0)
            {
                long m = mismatch_at(buf, (size_t)(base + (uint64_t)recvd), (size_t)n);

                if (m >= 0)
                    r->mismatch = m;
            }
            recvd += n;
            r->recvd = recvd;
        }
        if (sent < total && recvd < total && (pfd.revents & POLLOUT))
        {
            size_t want = (size_t)(total - sent < BLOCK ? total - sent : BLOCK);

            stream_fill(buf, want, base + (uint64_t)sent);
            n = send(fd, buf, want, MSG_NOSIGNAL);
            if (n <= 0)
            {
                r->err = n ? errno : EPIPE;
                break;
            }
            sent += n;
            r->sent = sent;
        }
    }
    free(buf);
    return (r->mismatch < 0 && r->err == 0 && recvd == total && sent == total) ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *host;
    int port, mib, seconds;
    long total;
    struct sockaddr_in sa;
    struct result r;
    int fd, rc;

    if (argc < 3)
    {
        fprintf(stderr, "usage: tbt_burst HOST PORT [MIB] [SECONDS]\n");
        return 4;
    }
    host = argv[1];
    port = atoi(argv[2]);
    mib = argc > 3 ? atoi(argv[3]) : 4;
    seconds = argc > 4 ? atoi(argv[4]) : 20;
    if (port <= 0 || port > 65535 || mib <= 0 || seconds <= 0)
    {
        fprintf(stderr, "tbt_burst: bad arguments\n");
        return 4;
    }
    total = (long)mib * 1024 * 1024;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("tbt_burst: socket");
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1)
    {
        fprintf(stderr, "tbt_burst: %s is not an IPv4 literal\n", host);
        return 4;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)))
    {
        fprintf(stderr, "tbt_burst: connect %s:%d: %s\n", host, port, strerror(errno));
        return 1;
    }
    if (set_timeouts(fd, seconds))
    {
        perror("tbt_burst: setsockopt timeout");
        return 1;
    }
    printf("burst: connected to %s:%d, %d MiB each way, strict send-then-read first\n",
           host, port, mib);

    memset(&r, 0, sizeof(r));
    r.mismatch = -1;
    rc = strict_phase(fd, total, &r);
    if (rc)
    {
        report("strict send-then-read", &r);
        close(fd);
        if (r.mismatch >= 0)
            return 3;
        if (r.recvd == 0 && r.sent > 0)
            return 2;
        return 1;
    }
    printf("burst: strict phase passed: %ld sent, %ld back, byte for byte\n",
           r.sent, r.recvd);

    /* Coverage only: the run already passed the case that matters. */
    memset(&r, 0, sizeof(r));
    r.mismatch = -1;
    if (interleaved_phase(fd, total, (uint64_t)total, seconds, &r))
        report("interleaved coverage", &r);
    else
        printf("burst: interleaved coverage phase passed\n");

    close(fd);
    printf("burst: %d MiB crossed each way\n", mib);
    return 0;
}
