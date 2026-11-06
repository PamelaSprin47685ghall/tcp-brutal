/* tbt_ping.c - one small message through a tbtun public port, and back.
 *
 * Usage: tbt_ping HOST PORT [BYTES] [SECONDS] [HOLD_MS]
 *
 * Connects, sends BYTES (default 37) in ONE write, then waits for exactly
 * BYTES back on the same connection. The socket stays open for the whole
 * exchange, so no FIN can flush the tunnel's send buffer: this is the case a
 * block-oriented data plane deadlocks on. The FEC v6 packetizer emits a share
 * only when its 8192-byte packet is full, so a small write used to sit in the
 * ring until the application closed the connection, and an application that
 * waits for an answer before writing more (an HTTP request, iperf3's control
 * cookie) waited forever. A 1 MiB burst never shows it because the packet
 * fills immediately.
 *
 * The payload is a fixed pattern derived from the byte offset, so the reply is
 * checked byte for byte and a short or shifted answer is visible.
 *
 * Exit codes:
 *   0  BYTES crossed in both directions, byte for byte
 *
 * HOLD_MS keeps the connection open after the reply, so a second connection can
 * run while this one is live. That is the shape iperf3 has (a control
 * connection that stays open while the data connection runs), and it is the one
 * a single-stream data plane cannot carry.
 *   1  connect failed, or the reply did not arrive inside the timeout
 *   3  the reply was short or differed
 *   4  bad usage or allocation failure
 */
#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#define MAX_MSG 262144

static uint8_t pattern(uint32_t off)
{
    return (uint8_t)((off * 37u + 11u) ^ (off >> 8));
}

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    struct sockaddr_in addr = { .sin_family = AF_INET };
    uint8_t out[MAX_MSG], in[MAX_MSG];
    long deadline;
    size_t want, sent = 0, got = 0;
    int fd, seconds, hold_ms = 0, i;

    if (argc < 3 || argc > 6)
    {
        fprintf(stderr, "usage: tbt_ping HOST PORT [BYTES] [SECONDS] [HOLD_MS]\n");
        return 4;
    }
    want = argc > 3 ? (size_t)strtoul(argv[3], NULL, 10) : 37;
    seconds = argc > 4 ? atoi(argv[4]) : 5;
    hold_ms = argc > 5 ? atoi(argv[5]) : 0;
    if (!want || want > MAX_MSG || seconds <= 0 || hold_ms < 0)
    {
        fprintf(stderr, "tbt_ping: bytes must be 1..%d and seconds > 0\n", MAX_MSG);
        return 4;
    }
    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1)
    {
        fprintf(stderr, "tbt_ping: bad address %s\n", argv[1]);
        return 4;
    }
    addr.sin_port = htons((uint16_t)atoi(argv[2]));

    for (i = 0; i < (int)want; i++)
        out[i] = pattern((uint32_t)i);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("tbt_ping: socket");
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)))
    {
        fprintf(stderr, "tbt_ping: connect: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    deadline = now_ms() + (long)seconds * 1000;

    while (sent < want)
    {
        ssize_t n = send(fd, out + sent, want - sent, 0);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "tbt_ping: send: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        sent += (size_t)n;
    }

    while (got < want)
    {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        long left = deadline - now_ms();
        ssize_t n;

        if (left <= 0)
            break;
        if (poll(&p, 1, (int)left) < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (!(p.revents & (POLLIN | POLLHUP)))
            continue;
        n = recv(fd, in + got, want - got, 0);
        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            break;
        }
        if (!n)
            break;
        got += (size_t)n;
    }
    if (hold_ms)
        usleep((useconds_t)hold_ms * 1000);
    close(fd);

    if (got != want)
    {
        fprintf(stderr, "tbt_ping: %zu/%zu bytes back after %d s\n", got, want,
                seconds);
        return 1;
    }
    for (i = 0; i < (int)want; i++)
    {
        if (in[i] != out[i])
        {
            fprintf(stderr, "tbt_ping: byte %d is 0x%02x, expected 0x%02x\n", i,
                    in[i], out[i]);
            return 3;
        }
    }
    printf("tbt_ping: %zu bytes crossed in both directions, byte for byte\n", want);
    return 0;
}
