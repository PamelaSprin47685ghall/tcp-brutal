/* probe_user.c - the application half of tests/probe/tbt_probe.ko.
 *
 * Two phases, because the architectural question has two halves and only one
 * of them is about proto_ops.
 *
 * PHASE A - the descriptor a real application has.
 *   The process connects to the module's listener and keeps its own socket fd.
 *   That fd is the client half of a loopback connection; the module holds the
 *   accepted half. Nothing here reaches the module's replaced ops unless the
 *   two halves are the same socket object, which they are not.
 *   Measured: poll returns 0 both times, read times out, and the module's
 *   counters stay at zero. That is the finding, not a test bug: replacing ops
 *   on the accepted socket cannot serve the application's read()/poll().
 *
 * PHASE B - the control that proves the ops replacement itself works.
 *   GETFD makes the module wrap its accepted socket in a file and install it in
 *   this process. Now read()/poll()/epoll() do reach the replaced ops, and the
 *   requirements can be checked exactly: first poll 0, second poll POLLIN after
 *   the delayed wake, read 4096, MSG_PEEK, read to EOF == 0, epoll sees EPOLLIN.
 *
 * The verdict:
 *   PROBE_PASS  phase B met every assertion, and phase A behaved as the
 *               "different socket" model predicts (counters still zero).
 *   PROBE_FAIL  an assertion failed; the line names it.
 *
 * Usage: probe_user [/dev/tbtprobe]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include "tbt_probe_uapi.h"

#define PROBE_PORT 19200
#define PAYLOAD_UNIT "PROBE-OK\n"
#define PAYLOAD_LEN 65536
#define RELEASE_MS 1500

static int failures;
static const char *dev = "/dev/tbtprobe";

static void ok(const char *what, const char *detail)
{
    printf("  ok   %s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
}

static void bad(const char *what, const char *detail)
{
    printf("  FAIL %s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
    failures++;
}

static int stats(int ctl, struct tbt_probe_stats *st)
{
    memset(st, 0, sizeof(*st));
    return ioctl(ctl, TBT_PROBE_STATS, st);
}

static int check_payload(const char *buf, size_t len, size_t off)
{
    const size_t unit = sizeof(PAYLOAD_UNIT) - 1;
    size_t i;

    for (i = 0; i < len; i++)
    {
        if (buf[i] != PAYLOAD_UNIT[(off + i) % unit])
            return 0;
    }
    return 1;
}

static int dial(void)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PROBE_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)))
    {
        perror("connect 127.0.0.1:19200");
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------------- */
/* Phase A: the application's own fd                                          */

static void phase_a(int ctl)
{
    struct tbt_probe_stats st;
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    struct pollfd p;
    char buf[4096];
    int fd, rc, n;

    printf("-- phase A: the application's own connected socket\n");
    fd = dial();
    if (fd < 0)
    {
        bad("phase A connect", "see errno");
        return;
    }
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0)
    {
        char d[64];
        snprintf(d, sizeof(d), "the app's local port is %u", ntohs(peer.sin_port));
        ok("the application holds one end of the connection", d);
    }

    /* Same order as phase B: watch, then let time pass. */
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    rc = poll(&p, 1, 1000);
    if (rc == 0)
        ok("poll(app fd, 1000) returned 0 before the release", NULL);
    else
        bad("poll(app fd, 1000) returned 0", "we expected no data yet");

    p.revents = 0;
    rc = poll(&p, 1, 5000);
    if (rc == 1 && (p.revents & POLLIN))
        ok("poll(app fd, 5000) returned POLLIN - the app fd DOES reach the module ops", NULL);
    else
    {
        char d[160];
        snprintf(d, sizeof(d),
                 "rc=%d revents=0x%x after the release: the app fd is a different "
                 "socket, so the module's queue is invisible to it", rc, p.revents);
        printf("  NOTE %s\n", d);
    }

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    n = read(fd, buf, sizeof(buf));
    if (n > 0)
        ok("read(app fd) returned data", "unexpected but welcome");
    else
    {
        char d[96];
        snprintf(d, sizeof(d), "n=%d errno=%s (no data on the app's own socket)",
                 n, n < 0 ? strerror(errno) : "n/a");
        printf("  NOTE %s\n", d);
    }

    if (stats(ctl, &st) == 0)
    {
        printf("STATS-A recvmsg_calls=%llu poll_calls=%llu wakes=%llu\n",
               (unsigned long long)st.recvmsg_calls, (unsigned long long)st.poll_calls,
               (unsigned long long)st.wakes);
        if (st.recvmsg_calls == 0 && st.poll_calls == 0)
            ok("phase A never entered the module's ops", "proves the two sockets are distinct");
        else
            bad("phase A must not enter the module's ops", "counters moved");
    }

    /* Let the app see the socket close; the module shuts it down later. */
    close(fd);
}

/* ------------------------------------------------------------------------- */
/* Phase B: the descriptor the module hands out                               */

static void phase_b(int ctl)
{
    struct tbt_probe_stats st;
    struct epoll_event ev;
    char buf[PAYLOAD_LEN];
    struct pollfd p;
    size_t total;
    int fd, n, rc;

    printf("-- phase B: the accepted socket the module hands out\n");
    if (dial() < 0)
    {
        bad("phase B connect", "see errno");
        return;
    }
    usleep(300 * 1000); /* let the accept work item run */

    fd = ioctl(ctl, TBT_PROBE_GETFD);
    if (fd < 0)
    {
        char d[64];
        snprintf(d, sizeof(d), "GETFD: %s", strerror(errno));
        bad("GETFD installs the accepted socket as an fd", d);
        return;
    }
    {
        char d[64];
        snprintf(d, sizeof(d), "fd %d, payload rearmed for +%d ms", fd, RELEASE_MS);
        ok("GETFD installed the accepted socket", d);
    }

    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    rc = poll(&p, 1, 1000);
    if (rc == 0)
        ok("first poll() returned 0: the module had not released the payload", NULL);
    else
    {
        char d[96];
        snprintf(d, sizeof(d), "rc=%d revents=0x%x", rc, p.revents);
        bad("first poll() must return 0", d);
    }

    p.revents = 0;
    rc = poll(&p, 1, 5000);
    if (rc == 1 && (p.revents & POLLIN))
        ok("second poll() woke with POLLIN from the module's delayed wake", NULL);
    else
    {
        char d[96];
        snprintf(d, sizeof(d), "rc=%d revents=0x%x", rc, p.revents);
        bad("second poll() must return POLLIN", d);
    }

    n = read(fd, buf, 4096);
    if (n == 4096 && check_payload(buf, n, 0))
        ok("read() returned 4096 bytes from the module queue", "PROBE-OK");
    else
    {
        char d[96];
        snprintf(d, sizeof(d), "n=%d errno=%s", n, n < 0 ? strerror(errno) : "content");
        bad("read() 4096 bytes", d);
    }

    memset(buf, 0, sizeof(buf));
    n = recv(fd, buf, sizeof(buf), MSG_PEEK | MSG_DONTWAIT);
    if (n > 0 && check_payload(buf, n, 4096))
    {
        char d[64];
        snprintf(d, sizeof(d), "%d bytes, stream position unchanged", n);
        ok("MSG_PEEK returns the next bytes without consuming", d);
    }
    else
    {
        char d[96];
        snprintf(d, sizeof(d), "n=%d errno=%s", n, n < 0 ? strerror(errno) : "content");
        bad("MSG_PEEK", d);
    }

    {
        int fl = fcntl(fd, F_GETFL);

        if (fl >= 0)
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    total = 4096;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
    {
        if (!check_payload(buf, n, total))
        {
            bad("read() stream content", "mismatch after the first 4 KiB");
            break;
        }
        total += (size_t)n;
        if (total > PAYLOAD_LEN + 8192)
            break;
    }
    if (n == 0 && total == PAYLOAD_LEN)
    {
        char d[64];
        snprintf(d, sizeof(d), "%zu bytes, PROBE-OK repeated", total);
        ok("read() to EOF returned exactly the payload", d);
    }
    else
    {
        char d[96];
        snprintf(d, sizeof(d), "total=%zu last n=%d errno=%s", total, n,
                 n < 0 ? strerror(errno) : "n/a");
        bad("read() to EOF", d);
    }

    n = read(fd, buf, sizeof(buf));
    if (n == 0)
        ok("read() after EOF returns 0", NULL);
    else
    {
        char d[64];
        snprintf(d, sizeof(d), "n=%d errno=%s", n, strerror(errno));
        bad("read() after EOF returns 0", d);
    }

    if (stats(ctl, &st) == 0)
    {
        printf("STATS-B recvmsg_calls=%llu poll_calls=%llu wakes=%llu bytes_served=%llu "
               "ready=%u eof=%u handed_out=%u\n",
               (unsigned long long)st.recvmsg_calls, (unsigned long long)st.poll_calls,
               (unsigned long long)st.wakes, (unsigned long long)st.bytes_served,
               st.ready, st.eof, st.handed_out);
        if (st.recvmsg_calls > 0)
            ok("read() reached the module's .recvmsg", NULL);
        else
            bad("read() reached the module's .recvmsg", "counter is 0");
        if (st.poll_calls > 0)
            ok("poll() reached the module's .poll", NULL);
        else
            bad("poll() reached the module's .poll", "counter is 0");
    }
    close(fd);

    /* epoll, registered before the release so the wake is what reports it */
    printf("-- phase B2: epoll\n");
    if (dial() < 0)
    {
        bad("epoll variant connect", "see errno");
        return;
    }
    usleep(300 * 1000);
    fd = ioctl(ctl, TBT_PROBE_GETFD);
    if (fd < 0)
    {
        char d[64];
        snprintf(d, sizeof(d), "GETFD: %s", strerror(errno));
        bad("GETFD for the epoll variant", d);
        return;
    }
    {
        int ep = epoll_create1(0);
        int got = 0, i;

        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev))
            bad("epoll_ctl(ADD)", strerror(errno));
        for (i = 0; i < 2 && !got; i++)
        {
            int w = epoll_wait(ep, &ev, 1, 5000);

            got = w == 1 && (ev.events & EPOLLIN);
        }
        if (got)
            ok("epoll_wait() saw EPOLLIN from the module's wake", NULL);
        else
            bad("epoll_wait() saw EPOLLIN", "no event within 10 s");
        close(ep);
    }
    close(fd);
}

int main(int argc, char **argv)
{
    int ctl;

    if (argc > 1)
        dev = argv[1];

    ctl = open(dev, O_RDWR);
    if (ctl < 0)
    {
        perror("open /dev/tbtprobe");
        return 1;
    }

    phase_a(ctl);
    phase_b(ctl);

    ioctl(ctl, TBT_PROBE_SHUTDOWN);
    close(ctl);

    printf("%s\n", failures ? "PROBE_FAIL" : "PROBE_PASS");
    return failures ? 1 : 0;
}
