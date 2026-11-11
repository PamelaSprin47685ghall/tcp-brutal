/* tbt_conn.c - second-public-connection probe for the bench guest.
 *
 * Opens a public connection A to the tunnel, proves it is active with one
 * 1 KB echo, then opens B while A is still open. After closing A it opens C.
 * Each connection is classified as ok, refused (connect errno), reset (the
 * peer closed or reset after a successful connect and write), timeout, or
 * error. The app-visible errno and the elapsed time are reported.
 *
 * This is the regression test for the observed defect "a second public
 * connection is accepted and then reset": the script exits 0 only when both
 * the concurrent and the after-close connection complete an echo.
 *
 * Usage: tbt_conn HOST PORT [CONNECT_TIMEOUT_MS]
 * Prints one line:
 *   conn while_active=ok|refused|reset|timeout|error errno_active=N
 *        ms_active=F after_close=... errno_after=N ms_after=F
 * Exit: 0 both ok, 1 otherwise, 2 usage.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define IO_BYTES 1024
#define IO_TIMEOUT_S 3

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int send_all(int fd, const char *buf, size_t len)
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

static int recv_all(int fd, char *buf, size_t len)
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
        {
            errno = 0; /* clean EOF, not an errno failure */
            return -2;
        }
        off += (size_t)n;
    }
    return 0;
}

/* One echo round trip. Returns 0 ok, 1 reset/EOF, 2 errno, 3 timeout. */
static int echo_once(int fd, int *errp)
{
    char tx[IO_BYTES], rx[IO_BYTES];
    int r;

    for (int i = 0; i < IO_BYTES; i++)
        tx[i] = (char)(0x3c ^ (i * 17));
    if (send_all(fd, tx, sizeof(tx)) != 0)
    {
        *errp = errno;
        return (errno == EPIPE || errno == ECONNRESET) ? 1 : 2;
    }
    r = recv_all(fd, rx, sizeof(rx));
    if (r == -2)
    {
        *errp = 0;
        return 1;
    }
    if (r != 0)
    {
        *errp = errno;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 3;
        return errno == ECONNRESET ? 1 : 2;
    }
    if (memcmp(tx, rx, sizeof(tx)) != 0)
    {
        *errp = 0;
        return 2;
    }
    *errp = 0;
    return 0;
}

static const char *name_of(int code)
{
    switch (code)
    {
    case 0:
        return "ok";
    case 1:
        return "reset";
    case 2:
        return "error";
    case 3:
        return "timeout";
    default:
        return "refused";
    }
}

/* Connect with a bounded wait. Returns fd or -1 with *errp and *code set. */
static int connect_to(const char *host, int port, int timeout_ms, int *errp,
                      int *code)
{
    struct sockaddr_in sin;
    struct pollfd pfd;
    struct timeval tv;
    int fd, one = 1, r, err = 0;
    socklen_t elen = sizeof(err);

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sin.sin_addr) != 1)
    {
        *errp = EINVAL;
        *code = 2;
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        *errp = errno;
        *code = 2;
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    r = connect(fd, (struct sockaddr *)&sin, sizeof(sin));
    if (r < 0 && errno != EINPROGRESS)
    {
        *errp = errno;
        *code = errno == ECONNREFUSED ? -1 : 2;
        close(fd);
        return -1;
    }
    if (r < 0)
    {
        pfd.fd = fd;
        pfd.events = POLLOUT;
        r = poll(&pfd, 1, timeout_ms);
        if (r <= 0)
        {
            *errp = r == 0 ? ETIMEDOUT : errno;
            *code = 3;
            close(fd);
            return -1;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0)
            err = errno;
        if (err)
        {
            *errp = err;
            *code = err == ECONNREFUSED ? -1 : 2;
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    tv.tv_sec = IO_TIMEOUT_S;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    *errp = 0;
    *code = 0;
    return fd;
}

/* Connect + one echo. Returns 0 ok, else a classification code. */
static int probe(const char *host, int port, int timeout_ms, int *errp,
                 double *ms)
{
    uint64_t t0 = now_ns();
    int code, fd = connect_to(host, port, timeout_ms, errp, &code);

    if (fd < 0)
    {
        *ms = (double)(now_ns() - t0) / 1e6;
        return code;
    }
    code = echo_once(fd, errp);
    *ms = (double)(now_ns() - t0) / 1e6;
    close(fd);
    return code;
}

int main(int argc, char **argv)
{
    int timeout_ms = 3000;
    int ea = 0, eb = 0, ec = 0;
    double mb = 0, mc = 0;
    int a, b, c, setup;

    if (argc < 3 || argc > 4)
    {
        fprintf(stderr, "usage: tbt_conn HOST PORT [CONNECT_TIMEOUT_MS]\n");
        return 2;
    }
    if (argc == 4)
        timeout_ms = atoi(argv[3]);

    /* A stays open across B's probe. */
    a = connect_to(argv[1], atoi(argv[2]), timeout_ms, &ea, &b);
    if (a < 0)
    {
        printf("conn while_active=%s errno_active=%d ms_active=0.0 "
               "after_close=%s errno_after=%d ms_after=0.0\n",
               name_of(b), ea, "not_run", 0);
        return 1;
    }
    setup = echo_once(a, &ea);
    if (setup != 0)
    {
        printf("conn while_active=setup_%s errno_active=%d ms_active=0.0 "
               "after_close=not_run errno_after=0 ms_after=0.0\n",
               name_of(setup), ea);
        close(a);
        return 1;
    }
    b = probe(argv[1], atoi(argv[2]), timeout_ms, &eb, &mb);
    close(a);
    usleep(200000);
    c = probe(argv[1], atoi(argv[2]), timeout_ms, &ec, &mc);

    printf("conn while_active=%s errno_active=%d ms_active=%.1f "
           "after_close=%s errno_after=%d ms_after=%.1f\n",
           name_of(b), eb, mb, name_of(c), ec, mc);
    return (b == 0 && c == 0) ? 0 : 1;
}
