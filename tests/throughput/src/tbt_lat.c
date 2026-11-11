/* tbt_lat.c - request/response latency microbenchmark for the bench guest.
 *
 * Sends BYTES bytes, reads exactly BYTES back, and records one CLOCK_MONOTONIC
 * interval per iteration. The peer is an echo (tbt_echo), so each interval is
 * one application-visible round trip.
 *
 * Usage: tbt_lat HOST PORT ITERS BYTES [SPORT] [TIMEOUT_S] [ITERS2 [MARKER [GO]]]
 *
 * SPORT binds the client's source port (0 = ephemeral). The direct path uses
 * it to land in the same shaped qdisc class a carrier would use, so "direct"
 * and "tunnel" see the same per-hop delay and rate limit.
 *
 * With ITERS2 > 0 the run has two phases on one connection: ITERS iterations,
 * then MARKER is created and the process waits for GO to appear, then ITERS2
 * more. tests/throughput/qemu/init uses that handshake to arm the module's loss
 * injector between the phases without closing the stream: a second public
 * connection to the same tunnel is reset by the module (observed), so a
 * separate connection cannot measure the lossy phase.
 *
 * Prints one machine-readable line per phase:
 *   lat phase=N complete=1 iters=N bytes=B min_us=.. p50_us=.. p90_us=..
 *       p95_us=.. p99_us=.. max_us=.. mean_us=.. err=.. failed_at=..
 * On an I/O error the phase prints complete=0 for the iterations that finished
 * and the process exits 1, so a stalled path still yields a distribution.
 *
 * Exit codes: 0 all requested phases done, 1 connect or I/O failure, 2 usage.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define WARMUP 50
#define GO_TIMEOUT_MS 30000

struct phase
{
    long done;
    int complete;
    int err;
    long failed_at;
};

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return x < y ? -1 : x > y;
}

static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len)
    {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static int read_all(int fd, char *buf, size_t len)
{
    size_t off = 0;

    while (off < len)
    {
        ssize_t n = recv(fd, buf + off, len - off, 0);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static void run_phase(int fd, long iters, long bytes, const char *tx, char *rx,
                      uint64_t *samples, struct phase *p)
{
    long i;

    p->done = 0;
    p->complete = 1;
    p->err = 0;
    p->failed_at = -1;
    for (i = 0; i < iters; i++)
    {
        uint64_t t0, t1;

        t0 = now_ns();
        if (write_all(fd, tx, (size_t)bytes) || read_all(fd, rx, (size_t)bytes))
        {
            p->err = errno;
            p->failed_at = i;
            p->complete = 0;
            break;
        }
        t1 = now_ns();
        if (memcmp(tx, rx, (size_t)bytes) != 0)
        {
            fprintf(stderr, "tbt_lat: echoed payload differs at iteration %ld\n", i);
            p->failed_at = i;
            p->complete = 0;
            break;
        }
        samples[i] = t1 - t0;
        p->done = i + 1;
    }
}

static void report(int phase, const struct phase *p, long bytes, uint64_t *s)
{
    long done = p->done;
    uint64_t sum = 0;
    long over5 = 0, over20 = 0, h[8] = {0};
    long first_tail = -1, tn = 0;
    double tmin = 0, tp50 = 0, tp90 = 0, tp99 = 0, tmax = 0;
    long nout = 0;
    char out[64 * 12 + 1];
    size_t ooff = 0;
    double mean;

    if (done <= 0)
    {
        printf("lat phase=%d complete=%d iters=0 bytes=%ld err=%d failed_at=%ld "
               "over5ms=0 over20ms=0 hist=empty outliers=none\n",
               phase, p->complete, bytes, p->err, p->failed_at);
        return;
    }
    qsort(s, (size_t)done, sizeof(*s), cmp_u64);
    for (long i = 0; i < done; i++)
    {
        long us = (long)(s[i] / 1000);
        int b;

        sum += s[i];
        if (us > 5000)
            over5++;
        if (us > 20000)
            over20++;
        if (us < 1000)
            b = 0;
        else if (us < 2000)
            b = 1;
        else if (us < 5000)
            b = 2;
        else if (us < 10000)
            b = 3;
        else if (us < 20000)
            b = 4;
        else if (us < 50000)
            b = 5;
        else if (us < 100000)
            b = 6;
        else
            b = 7;
        h[b]++;
        /* The tail is the measurement: keep every sample over 5 ms, up to a
         * bounded list, so a re-run can show the distribution, not one max. */
        if (us > 5000 && nout < 64)
        {
            int n = snprintf(out + ooff, sizeof(out) - ooff, "%s%ld",
                             nout ? ";" : "", us);

            if (n > 0 && (size_t)n < sizeof(out) - ooff)
            {
                ooff += (size_t)n;
                nout++;
            }
        }
    }
    if (!nout)
        snprintf(out, sizeof(out), "none");
    out[sizeof(out) - 1] = 0;
    /* The tail is the hole cost: every sample over 5 ms is one iteration
     * that hit a hole (the request/response stream carries one frame each
     * way). Its percentiles are the per-hole distribution the max alone
     * cannot show. */
    for (long i = 0; i < done; i++)
        if (s[i] > 5000ull * 1000)
        {
            first_tail = i;
            break;
        }
    if (first_tail >= 0)
    {
        tn = done - first_tail;
        tmin = (double)s[first_tail] / 1000.0;
        tp50 = (double)s[first_tail + (tn - 1) * 50 / 100] / 1000.0;
        tp90 = (double)s[first_tail + (tn - 1) * 90 / 100] / 1000.0;
        tp99 = (double)s[first_tail + (tn - 1) * 99 / 100] / 1000.0;
        tmax = (double)s[done - 1] / 1000.0;
    }
    mean = (double)sum / (double)done;
    printf("lat phase=%d complete=%d iters=%ld bytes=%ld min_us=%.1f p50_us=%.1f "
           "p90_us=%.1f p95_us=%.1f p99_us=%.1f p999_us=%.1f max_us=%.1f "
           "mean_us=%.1f err=%d failed_at=%ld "
           "over5ms=%ld over20ms=%ld "
           "hist=0-1ms:%ld;1-2ms:%ld;2-5ms:%ld;5-10ms:%ld;10-20ms:%ld;"
           "20-50ms:%ld;50-100ms:%ld;100ms+:%ld outliers=%s "
           "tail_n=%ld tail_min_us=%.1f tail_p50_us=%.1f tail_p90_us=%.1f "
           "tail_p99_us=%.1f tail_max_us=%.1f\n",
           phase, p->complete, done, bytes, (double)s[0] / 1000.0,
           (double)s[(done - 1) * 50 / 100] / 1000.0,
           (double)s[(done - 1) * 90 / 100] / 1000.0,
           (double)s[(done - 1) * 95 / 100] / 1000.0,
           (double)s[(done - 1) * 99 / 100] / 1000.0,
           (double)s[(done - 1) * 999 / 1000] / 1000.0,
           (double)s[done - 1] / 1000.0, mean / 1000.0, p->err, p->failed_at,
           over5, over20, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], out,
           tn, tmin, tp50, tp90, tp99, tmax);
}

int main(int argc, char **argv)
{
    struct sockaddr_in sin;
    struct timeval tv;
    uint64_t *samples;
    char *tx, *rx;
    long iters, iters2 = 0, bytes;
    int sport = 0, timeout_s = 10;
    const char *marker = NULL, *go = NULL;
    int fd, one = 1;
    struct phase p1, p2;
    int rc = 0;

    if (argc < 5 || argc > 10)
    {
        fprintf(stderr, "usage: tbt_lat HOST PORT ITERS BYTES [SPORT] [TIMEOUT_S] "
                        "[ITERS2 [MARKER [GO]]]\n");
        return 2;
    }
    iters = atol(argv[3]);
    bytes = atol(argv[4]);
    if (argc > 5)
        sport = atoi(argv[5]);
    if (argc > 6)
        timeout_s = atoi(argv[6]);
    if (argc > 7)
        iters2 = atol(argv[7]);
    if (argc > 8)
        marker = argv[8];
    if (argc > 9)
        go = argv[9];
    if (iters <= 0 || bytes <= 0 || bytes > 65536 || iters > 1000000 ||
        iters2 < 0 || iters2 > 1000000)
    {
        fprintf(stderr, "tbt_lat: bad iters/bytes\n");
        return 2;
    }

    samples = calloc((size_t)(iters > iters2 ? iters : iters2), sizeof(*samples));
    tx = malloc((size_t)bytes);
    rx = malloc((size_t)bytes);
    if (!samples || !tx || !rx)
    {
        fprintf(stderr, "tbt_lat: out of memory\n");
        return 2;
    }
    /* A deterministic, non-compressible payload; the echo returns it. */
    for (long k = 0; k < bytes; k++)
        tx[k] = (char)(0x5a ^ (k * 31));

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &sin.sin_addr) != 1)
    {
        fprintf(stderr, "tbt_lat: %s is not an IPv4 literal\n", argv[1]);
        return 2;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("tbt_lat: socket");
        return 2;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    tv.tv_sec = timeout_s;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (sport)
    {
        struct sockaddr_in local;

        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_port = htons((uint16_t)sport);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0)
        {
            perror("tbt_lat: bind");
            return 1;
        }
    }
    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
    {
        perror("tbt_lat: connect");
        return 1;
    }

    /* Warm up the path: cwnd, ARP, and the far side's target connection. */
    run_phase(fd, WARMUP, bytes, tx, rx, samples, &p1);
    run_phase(fd, iters, bytes, tx, rx, samples, &p1);
    report(1, &p1, bytes, samples);
    if (!p1.complete)
        rc = 1;

    if (iters2 > 0 && p1.complete)
    {
        if (marker)
        {
            int m = open(marker, O_CREAT | O_WRONLY, 0644);

            if (m >= 0)
                close(m);
        }
        if (go)
        {
            int waited = 0;

            while (access(go, F_OK) != 0 && waited < GO_TIMEOUT_MS)
            {
                usleep(1000);
                waited++;
            }
            if (waited >= GO_TIMEOUT_MS)
            {
                fprintf(stderr, "tbt_lat: timed out waiting for %s\n", go);
                rc = 1;
            }
        }
        if (rc == 0)
        {
            run_phase(fd, iters2, bytes, tx, rx, samples, &p2);
            report(2, &p2, bytes, samples);
            if (!p2.complete)
                rc = 1;
        }
    }
    close(fd);
    free(samples);
    free(tx);
    free(rx);
    return rc;
}
